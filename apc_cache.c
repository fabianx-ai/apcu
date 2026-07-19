/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http:www.php.net/license/3_01.txt                                    |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

#include "apc_cache.h"
#include "apc_sma.h"
#include "apc_globals.h"
#include "apc_strings.h"
#include "apc_time.h"
#include "php_scandir.h"
#include "SAPI.h"
#include "TSRM.h"
#include "php_main.h"
#include "ext/standard/md5.h"
#include "ext/standard/php_var.h"
#include "zend_smart_str.h"

#if PHP_VERSION_ID < 70300
# define GC_SET_REFCOUNT(ref, rc) (GC_REFCOUNT(ref) = (rc))
# define GC_ADDREF(ref) GC_REFCOUNT(ref)++
#endif

/* If recursive mutexes are used, there is no distinction between read and write locks.
 * As such, if we acquire a read-lock, it's really a write-lock and we are free to perform
 * increments without atomics. */
#ifdef APC_LOCK_RECURSIVE
# define ATOMIC_INC_RLOCKED(a) (a)++
#else
# define ATOMIC_INC_RLOCKED(a) ATOMIC_INC(a)
#endif

/* Defined in apc_persist.c */
apc_cache_entry_data_t *apc_persist_data_ex(
		apc_sma_t *sma, apc_serializer_t *serializer, const zval *val,
		unsigned char *serialized_str, size_t serialized_str_len, zend_string *ei);
zend_bool apc_persist_serialize_value(
		apc_serializer_t *serializer, const zval *zv,
		unsigned char **buf, size_t *buf_len);
zend_bool apc_data_value_identical(
		const apc_cache_entry_data_t *data, const zval *val,
		const unsigned char *serialized_str, size_t serialized_str_len);
zend_bool apc_data_payloads_identical(
		const apc_cache_entry_data_t *a, const apc_cache_entry_data_t *b);
zend_bool apc_data_ei_matches(const apc_cache_entry_data_t *data, const zend_string *ei);
zend_string *apc_data_fetch_ei(const apc_cache_entry_data_t *data);
zend_bool apc_unpersist(zval *dst, const apc_cache_entry_data_t *data, apc_serializer_t *serializer);
apc_cache_entry_t *apc_persist_alloc_entry(apc_sma_t *sma, zend_string *key);
void apc_persist_init_entry(
		apc_cache_entry_t *entry, zend_string *key, zend_long mem_size, zend_uchar pooled);

/* make_prime */
static int const primes[] = {
  257, /*   256 */
  521, /*   512 */
 1031, /*  1024 */
 2053, /*  2048 */
 3079, /*  3072 */
 4099, /*  4096 */
 5147, /*  5120 */
 6151, /*  6144 */
 7177, /*  7168 */
 8209, /*  8192 */
 9221, /*  9216 */
10243, /* 10240 */
11273, /* 11264 */
12289, /* 12288 */
13313, /* 13312 */
14341, /* 14336 */
15361, /* 15360 */
16411, /* 16384 */
17417, /* 17408 */
18433, /* 18432 */
19457, /* 19456 */
20483, /* 20480 */
30727, /* 30720 */
40961, /* 40960 */
61441, /* 61440 */
81929, /* 81920 */
122887,/* 122880 */
163841,/* 163840 */
245771,/* 245760 */
327689,/* 327680 */
491527,/* 491520 */
655373,/* 655360 */
983063,/* 983040 */
1310627,/* 1310720 */
1474489,/* 1474560 */
1965983,/* 1966080 */
2621347,/* 2621440 */
3276719,/* 3276800 */
3932063,/* 3932160 */
4587431,/* 4587520 */
5242801,/* 5242880 */
6553511,/* 6553600 */
7864243,/* 7864320 */
8847271,/* 8847360 */
9830321,/* 9830400 */
10485667,/* 10485760 */
0      /* sentinel */
};

static int make_prime(int n)
{
	int *k = (int*)primes;
	while(*k) {
		if((*k) > n) return *k;
		k++;
	}
	return *(k-1);
}

/* Entry-pool primitives. The pool is a list of fixed-size entry slots in shm,
 * linked through entry->next and only ever touched under the write lock, so a
 * fresh insert of a normal-length key costs no SMA allocation at all
 * (GH #346: "entry_t is allocated via a pool"). Batch blocks are never freed;
 * the pool is bounded by the peak number of live entries. */
static apc_cache_entry_t *apc_cache_wlocked_pool_pop(apc_cache_t *cache, zend_string *key) {
	apc_cache_entry_t *slot;

	if (!cache->header->entry_pool) {
		return NULL;
	}

	slot = ENTRYAT(cache->header->entry_pool);
	cache->header->entry_pool = slot->next;

	apc_persist_init_entry(slot, key, APC_POOL_SLOT_SIZE, APC_ENTRY_POOL_LIVE);
	/* creation reference, dropped once the entry is linked */
	slot->ref_count = 1;
	return slot;
}

static void apc_cache_wlocked_pool_push(apc_cache_t *cache, apc_cache_entry_t *slot) {
	slot->pooled = APC_ENTRY_POOL_FREE;
	slot->next = cache->header->entry_pool;
	cache->header->entry_pool = ENTRYOF(slot);
}

/* Register a batch block and carve it into pool slots. Write lock required. */
static void apc_cache_wlocked_pool_fill(apc_cache_t *cache, apc_pool_batch_t *batch) {
	int i;

	batch->next = cache->header->pool_batches;
	cache->header->pool_batches = ((uintptr_t) batch) - (uintptr_t) cache->header;

	for (i = 0; i < APC_POOL_BATCH; i++) {
		apc_cache_wlocked_pool_push(cache, APC_POOL_BATCH_SLOT(batch, i));
	}
}

static inline void free_entry(apc_cache_t *cache, apc_cache_entry_t *entry) {
	if (entry->pooled == APC_ENTRY_POOL_LIVE) {
		/* pool slots are recycled, not freed; all callers hold the write lock */
		apc_cache_wlocked_pool_push(cache, entry);
	} else {
		apc_sma_free(cache->sma, entry);
	}
}

static inline void free_data(apc_cache_t *cache, apc_cache_entry_data_t *data) {
	apc_sma_free(cache->sma, data);
}

/* Detach a data block from its entry (under write lock). The block is freed
 * immediately when no reader holds a reference — the write lock excludes new
 * references. Otherwise it is parked on the data gc list until they drain. */
static void apc_cache_wlocked_detach_data(apc_cache_t *cache, apc_cache_entry_data_t *data)
{
	if (cache->header->mem_size) {
		cache->header->mem_size -= data->mem_size;
	}

	data->owner = 0;
	if (data->ref_count <= 0) {
		free_data(cache, data);
	} else {
		data->dtime = time(0);
		data->gc_next = cache->header->gc_data;
		cache->header->gc_data = DATAOF(data);
		cache->header->nparked++;
	}
}

/* Park a just-swapped-out data block on the data gc list. Runs under the
 * READ lock: concurrent parkers push with a CAS loop (pushes happen only
 * under the read lock, pops only under the write lock — no ABA). The block
 * is NEVER freed here: a reader holding the same read lock may have loaded
 * the pointer and not yet pinned it. Frees happen only in gc under the
 * write lock, which excludes every rlock holder (DataSwap.tla:
 * NoPinnedFree; the inline-free variant is refuted). */
static void apc_cache_park_data(apc_cache_t *cache, apc_cache_entry_data_t *data, time_t t)
{
	uintptr_t self = DATAOF(data);

	data->owner = 0;
	data->dtime = t;

	do {
		data->gc_next = cache->header->gc_data;
	} while (!ATOMIC_CAS(cache->header->gc_data, data->gc_next, self));

	ATOMIC_INC_RLOCKED(cache->header->nparked);
}

/* These calculations can and should be done outside of a lock */
static inline void apc_cache_hash_slot(
		apc_cache_t* cache, zend_string *key, size_t* slot) {
	*slot = ZSTR_HASH(key) % cache->nslots;
}

static inline zend_bool apc_entry_key_equals(const apc_cache_entry_t *entry, zend_string *key) {
	return ZSTR_H(&entry->key) == ZSTR_H(key)
		&& ZSTR_LEN(&entry->key) == ZSTR_LEN(key)
		&& memcmp(ZSTR_VAL(&entry->key), ZSTR_VAL(key), ZSTR_LEN(key)) == 0;
}

/* An entry is hard expired if its value's store time is older than the value's
 * TTL. Expiry lives in the data block: it is published atomically with the
 * value, so no torn (ttl, ctime) pair can be observed (GH #345). The data
 * pointer is loaded exactly once — a second load could straddle a swap. */
static zend_bool apc_cache_entry_hard_expired(apc_cache_t *cache, apc_cache_entry_t *entry, time_t t) {
	const apc_cache_entry_data_t *d = ENTRY_DATA(entry);
	return d->ttl && (time_t) (d->ctime + d->ttl) < t;
}

/* An entry is soft expired if no per-entry TTL is set, a global cache TTL is set,
 * and the access time of the entry is older than the global TTL. Soft expired entries
 * are accessible by lookup operation, but may be removed from the cache at any time. */
static zend_bool apc_cache_entry_soft_expired(
		apc_cache_t *cache, apc_cache_entry_t *entry, time_t t) {
	return !ENTRY_DATA(entry)->ttl && cache->ttl
		&& (time_t) (entry->atime + cache->ttl) < t;
}

static zend_bool apc_cache_entry_expired(
		apc_cache_t *cache, apc_cache_entry_t *entry, time_t t) {
	return apc_cache_entry_hard_expired(cache, entry, t)
		|| apc_cache_entry_soft_expired(cache, entry, t);
}

/* apc_cache_wlocked_move_block() is called during defragmentation, before a
 * block (an entry, an entry's data block, or a pool batch) is moved. */
static zend_bool apc_cache_wlocked_move_block(apc_cache_t *cache, void *old, void *new, size_t block_type) {
	if (block_type == APC_SMA_BLOCK_POOL) {
		/* A batch of entry-pool slots. Relocatable: live slots have their
		 * neighbor links, gc links and data back references fixed up exactly
		 * like directly allocated entries, and every pool free-list or batch
		 * registry pointer into the moved range is rebased. The memory itself
		 * is moved by the defragmenter after this returns, so all reads go
		 * through the old addresses. */
		apc_pool_batch_t *batch = old;
		ptrdiff_t delta = (char *) new - (char *) old;
		uintptr_t old_base = ((uintptr_t) old) - (uintptr_t) cache->header;
		uintptr_t old_end = old_base + APC_POOL_BATCH_BYTES;
		uintptr_t *link;
		int i;

		/* refuse when any live slot is referenced (a reader may hold it) */
		for (i = 0; i < APC_POOL_BATCH; i++) {
			apc_cache_entry_t *slot = APC_POOL_BATCH_SLOT(batch, i);
			if (slot->pooled == APC_ENTRY_POOL_LIVE && slot->ref_count > 0) {
				return 0;
			}
		}

		/* rebase the links of live slots (slot lists and the gc list share
		 * the same structure; a neighbor inside this batch is written through
		 * its old address and moved afterwards, which yields the same bytes) */
		for (i = 0; i < APC_POOL_BATCH; i++) {
			apc_cache_entry_t *slot = APC_POOL_BATCH_SLOT(batch, i);
			uintptr_t newoff;

			if (slot->pooled != APC_ENTRY_POOL_LIVE) {
				continue;
			}

			newoff = ENTRYOF(slot) + delta;
			ENTRYAT(slot->prev)->next = newoff;
			if (slot->next) {
				ENTRYAT(slot->next)->prev = newoff;
			}
			if (slot->data) {
				ENTRY_DATA(slot)->owner = newoff;
			}
		}

		/* rebase pool free-list pointers into the moved range; chase the
		 * chain through the old addresses */
		link = &cache->header->entry_pool;
		while (*link) {
			uintptr_t cur = *link;
			if (cur >= old_base && cur < old_end) {
				*link = cur + delta;
			}
			link = &ENTRYAT(cur)->next;
		}

		/* rebase batch registry pointers the same way */
		link = &cache->header->pool_batches;
		while (*link) {
			uintptr_t cur = *link;
			if (cur >= old_base && cur < old_end) {
				*link = cur + delta;
			}
			link = &((apc_pool_batch_t *) ((uintptr_t) cache->header + cur))->next;
		}

		return 1;
	}

	if (block_type == APC_SMA_BLOCK_DATA) {
		apc_cache_entry_data_t *data = old;

		/* Readers hold raw pointers into referenced blocks, and detached
		 * blocks are linked from the gc list by address; both are unmovable. */
		if (data->ref_count > 0 || !data->owner) {
			return 0;
		}

		ENTRYAT(data->owner)->data = DATAOF(new);
		return 1;
	}

	apc_cache_entry_t *entry = old;

	/* Check if the entry can be moved. */
	if (entry->ref_count > 0) {
		return 0;
	}

	/* Change all references to this entry to the new position.
	 * Since “next” is the 1st field of apc_cache_entry_t, the head pointer of the list
	 * can be changed like a previous entry via ENTRYAT(entry->prev)->next. */
	ENTRYAT(entry->prev)->next = ENTRYOF(new);
	if (entry->next) {
		ENTRYAT(entry->next)->prev = ENTRYOF(new);
	}

	/* The entry's data block points back at it. */
	if (entry->data) {
		ENTRY_DATA(entry)->owner = ENTRYOF(new);
	}

	return 1;
}

/* Inserts an entry into a linked list. The argument entry_offset must point either
 * to entry->next of an existing entry or to the head pointer of a linked list. */
static void apc_cache_wlocked_link_entry(apc_cache_t *cache, uintptr_t *entry_offset, apc_cache_entry_t *entry) {
	entry->next = *entry_offset;
	entry->prev = ENTRYOF(entry_offset);
	*entry_offset = ENTRYOF(entry);
	if (entry->next) {
		ENTRYAT(entry->next)->prev = *entry_offset;
	}
}

/* Removes an entry from a linked list. */
static void apc_cache_wlocked_unlink_entry(apc_cache_t *cache, apc_cache_entry_t *entry) {
	/* Since “next” is the 1st field of apc_cache_entry_t, the head pointer of the list
	 * can be changed like a previous entry via ENTRYAT(entry->prev)->next. */
	ENTRYAT(entry->prev)->next = entry->next;
	if (entry->next) {
		ENTRYAT(entry->next)->prev = entry->prev;
	}
}

static void apc_cache_wlocked_remove_entry(apc_cache_t *cache, apc_cache_entry_t *entry)
{
    /* unlink entry from list */
	apc_cache_wlocked_unlink_entry(cache, entry);

	/* detach the entry's value */
	if (entry->data) {
		apc_cache_wlocked_detach_data(cache, ENTRY_DATA(entry));
		entry->data = 0;
	}

	/* adjust header info */
	if (cache->header->mem_size)
		cache->header->mem_size -= entry->mem_size;

	if (cache->header->nentries)
		cache->header->nentries--;

	/* free entry if there are no references */
	if (entry->ref_count <= 0) {
		free_entry(cache, entry);
	} else {
		/* add to gc if there are still refs */
		entry->dtime = time(0);
		apc_cache_wlocked_link_entry(cache, &cache->header->gc, entry);
	}
}

static void apc_cache_wlocked_gc(apc_cache_t* cache)
{
	/* This function scans the list of removed cache entries and deletes any
	 * entry whose reference count is zero  or that has been on the gc
	 * list for more than cache->gc_ttl seconds
	 *   (we issue a warning in the latter case).
	 */
	if (!cache->header->gc && !cache->header->gc_data) {
		return;
	}

	time_t now = time(0);

	/* data blocks parked while readers were still copying from them */
	uintptr_t *data_offset = &cache->header->gc_data;
	while (*data_offset) {
		apc_cache_entry_data_t *data = DATAAT(*data_offset);
		time_t gc_sec = cache->gc_ttl ? (now - data->dtime) : 0;

		if (data->ref_count > 0 && gc_sec <= (time_t)cache->gc_ttl) {
			data_offset = &data->gc_next;
			continue;
		}

		if (data->ref_count > 0) {
			apc_debug(
				"GC data block was on gc-list for %lld seconds",
				(long long) gc_sec
			);
		}

		/* set next and free current block */
		*data_offset = data->gc_next;
		cache->header->nparked--;
		free_data(cache, data);
	}

	uintptr_t *entry_offset = &cache->header->gc;
	while (*entry_offset) {
		apc_cache_entry_t *entry = ENTRYAT(*entry_offset);
		time_t gc_sec = cache->gc_ttl ? (now - entry->dtime) : 0;

		if (entry->ref_count > 0 && gc_sec <= (time_t)cache->gc_ttl) {
			entry_offset = &entry->next;
			continue;
		}

		/* good ol' whining */
		if (entry->ref_count > 0) {
			apc_debug(
				"GC cache entry '%s' was on gc-list for %lld seconds",
				ZSTR_VAL(&entry->key), (long long) gc_sec
			);
		}

		/* set next and free current entry */
		apc_cache_wlocked_unlink_entry(cache, entry);
		free_entry(cache, entry);
	}
}

/* php serializer */
PHP_APCU_API int APC_SERIALIZER_NAME(php) (APC_SERIALIZER_ARGS)
{
	smart_str strbuf = {0};
	php_serialize_data_t var_hash;

	/* Lock in case apcu is accessed inside Serializer::serialize() */
	BG(serialize_lock)++;
	PHP_VAR_SERIALIZE_INIT(var_hash);
	php_var_serialize(&strbuf, (zval*) value, &var_hash);
	PHP_VAR_SERIALIZE_DESTROY(var_hash);
	BG(serialize_lock)--;

	if (EG(exception)) {
		smart_str_free(&strbuf);
		strbuf.s = NULL;
	}

	if (strbuf.s != NULL) {
		*buf = (unsigned char *)estrndup(ZSTR_VAL(strbuf.s), ZSTR_LEN(strbuf.s));
		if (*buf == NULL)
			return 0;

		*buf_len = ZSTR_LEN(strbuf.s);
		smart_str_free(&strbuf);
		return 1;
	}
	return 0;
}

/* php unserializer */
PHP_APCU_API int APC_UNSERIALIZER_NAME(php) (APC_UNSERIALIZER_ARGS)
{
	const unsigned char *tmp = buf;
	php_unserialize_data_t var_hash;
	int result;

	/* Lock in case apcu is accessed inside Serializer::unserialize() */
	BG(serialize_lock)++;
	PHP_VAR_UNSERIALIZE_INIT(var_hash);
	result = php_var_unserialize(value, &tmp, buf + buf_len, &var_hash);
	PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
	BG(serialize_lock)--;

	if (!result) {
		php_error_docref(NULL, E_NOTICE, "Error at offset %td of %zu bytes", tmp - buf, buf_len);
		ZVAL_NULL(value);
		return 0;
	}
	return 1;
}

PHP_APCU_API apc_cache_t* apc_cache_create(apc_sma_t* sma, apc_serializer_t* serializer, zend_long size_hint, zend_long gc_ttl, zend_long ttl, double expunge_threshold, zend_bool defend) {
	apc_cache_t* cache;
	zend_long cache_size;
	size_t nslots;

	/* calculate number of slots. Default: 512 slots per MB of shared memory */
	nslots = make_prime(size_hint > 0 ? (size_t)size_hint : sma->size / 2048);

	/* allocate pointer by normal means */
	cache = pemalloc(sizeof(apc_cache_t), 1);

	/* calculate cache size for shm allocation */
	cache_size = sizeof(apc_cache_header_t) + nslots * sizeof(uintptr_t);

	/* allocate shm */
	cache->header = apc_sma_malloc(sma, cache_size, NULL);

	if (!cache->header) {
		zend_error_noreturn(E_CORE_ERROR, "Unable to allocate " ZEND_LONG_FMT " bytes of shared memory for cache structures. Either apc.shm_size is too small or apc.entries_hint too large", cache_size);
		return NULL;
	}

	/* zero cache header and hash slots */
	memset(cache->header, 0, cache_size);

	/* set header values */
	cache->header->nhits = 0;
	cache->header->nmisses = 0;
	cache->header->nentries = 0;
	cache->header->ncleanups = 0;
	cache->header->ndefragmentations = 0;
	cache->header->nexpunges = 0;
	cache->header->gc = 0;
	cache->header->gc_data = 0;
	cache->header->entry_pool = 0;
	cache->header->pool_batches = 0;
	cache->header->stime = time(NULL);

	/* set cache options */
	cache->slots = (uintptr_t *)((uintptr_t)cache->header + sizeof(apc_cache_header_t));
	cache->sma = sma;
	cache->serializer = serializer;
	cache->nslots = nslots;
	cache->gc_ttl = gc_ttl;
	cache->ttl = ttl;
	cache->expunge_threshold = expunge_threshold;
	cache->defend = defend;

	/* header lock */
	CREATE_LOCK(&cache->header->lock);

	return cache;
}

/* Walk the slot for key, removing expired entries along the way (runtime
 * cleanup, as before). Returns the live matching entry, or NULL with *tail
 * set to the link a new entry should be inserted at. A hard-expired matching
 * entry is removed and reported as absent. */
static apc_cache_entry_t *apc_cache_wlocked_find_slot(
		apc_cache_t *cache, zend_string *key, time_t t, uintptr_t **tail) {
	size_t s;

	/* calculate hash and slot */
	apc_cache_hash_slot(cache, key, &s);

	uintptr_t *entry_offset = &cache->slots[s];
	while (*entry_offset) {
		apc_cache_entry_t *entry = ENTRYAT(*entry_offset);

		/* check for a match by hash and string */
		if (apc_entry_key_equals(entry, key)) {
			if (apc_cache_entry_hard_expired(cache, entry, t)) {
				apc_cache_wlocked_remove_entry(cache, entry);
				continue;
			}

			*tail = NULL;
			return entry;
		}

		/*
		 * This is a bit nasty. The idea here is to do runtime cleanup of the linked list of
		 * entries, so we don't always have to skip past a bunch of stale entries.
		 */
		if (apc_cache_entry_expired(cache, entry, t)) {
			apc_cache_wlocked_remove_entry(cache, entry);
			continue;
		}

		/* set next entry */
		entry_offset = &entry->next;
	}

	*tail = entry_offset;
	return NULL;
}

/* Attach a data block to an entry and refresh the entry's metadata exactly as
 * storing always did. Replaces (detaches) the previous value in place: no list
 * surgery, the entry and its key are untouched (GH #346). */
static void apc_cache_wlocked_attach_data(
		apc_cache_t *cache, apc_cache_entry_t *entry, apc_cache_entry_data_t *data,
		const zend_long ttl, const time_t t) {
	if (entry->data) {
		apc_cache_wlocked_detach_data(cache, ENTRY_DATA(entry));
	}

	entry->data = DATAOF(data);
	data->owner = ENTRYOF(entry);
	cache->header->mem_size += data->mem_size;
	cache->header->ninserts++;

	data->ttl = ttl;
	data->ctime = t;
	data->mtime = t;

	entry->nhits = 0;
	entry->atime = t;
	entry->dtime = 0;

	/* Drop the creation reference: the entry owns the block now, and the
	 * reference count only tracks readers. */
	data->ref_count--;
}

/* Link a freshly allocated entry (with its creation reference still held) at
 * tail and attach data to it. */
static void apc_cache_wlocked_insert_entry(
		apc_cache_t *cache, uintptr_t *tail, apc_cache_entry_t *entry,
		apc_cache_entry_data_t *data, const zend_long ttl, const time_t t) {
	apc_cache_wlocked_link_entry(cache, tail, entry);

	cache->header->mem_size += entry->mem_size;
	cache->header->nentries++;

	apc_cache_wlocked_attach_data(cache, entry, data, ttl, t);

	/* drop the creation reference of the entry block */
	entry->ref_count--;
}


/* TODO This function may lead to a deadlock on expunge */
static inline zend_bool apc_cache_wlocked_store_internal(
		apc_cache_t *cache, zend_string *key, const zval *val,
		const zend_long ttl, const zend_bool exclusive) {
	time_t t = apc_time();
	uintptr_t *tail;
	apc_cache_entry_t *entry;
	apc_cache_entry_data_t *data;

	if (apc_cache_defense(cache, key, t)) {
		return 0;
	}

	/* process deleted lists */
	apc_cache_wlocked_gc(cache);

	/* create the value in the shared memory */
	data = apc_persist_data_ex(cache->sma, cache->serializer, val, NULL, 0, NULL);
	if (!data) {
		return 0;
	}

	entry = apc_cache_wlocked_find_slot(cache, key, t, &tail);
	if (entry) {
		if (exclusive) {
			free_data(cache, data);
			return 0;
		}

		apc_cache_wlocked_attach_data(cache, entry, data, ttl, t);
		return 1;
	}

	if (ZSTR_LEN(key) <= APC_POOL_KEY_MAX) {
		entry = apc_cache_wlocked_pool_pop(cache, key);
		if (!entry) {
			/* Refill under the held lock: this runs inside apcu_entry, where
			 * a nested expunge uses the entry-level no-op locks (the same
			 * pre-existing caveat as the value allocation above). */
			apc_pool_batch_t *batch = apc_sma_try_malloc_ex(
				cache->sma, APC_POOL_BATCH_BYTES, APC_SMA_BLOCK_POOL, NULL);
			if (batch) {
				apc_cache_wlocked_pool_fill(cache, batch);
				entry = apc_cache_wlocked_pool_pop(cache, key);
			}
		}
	} else {
		entry = NULL;
	}

	if (!entry) {
		entry = apc_persist_alloc_entry(cache->sma, key);
		if (!entry) {
			free_data(cache, data);
			return 0;
		}
	}

	/* The allocations above may have expunged the cache; re-walk the slot to
	 * get a valid tail (the key cannot have appeared: we hold the lock). */
	if (apc_cache_wlocked_find_slot(cache, key, t, &tail)) {
		ZEND_UNREACHABLE();
	}

	apc_cache_wlocked_insert_entry(cache, tail, entry, data, ttl, t);
	return 1;
}

/* Find entry, without updating stat counters or access time */
static inline apc_cache_entry_t *apc_cache_rlocked_find_nostat(
		apc_cache_t *cache, zend_string *key, time_t t) {
	size_t s;

	/* calculate hash and slot */
	apc_cache_hash_slot(cache, key, &s);

	uintptr_t entry_offset = cache->slots[s];
	while (entry_offset) {
		apc_cache_entry_t *entry = ENTRYAT(entry_offset);

		/* check for a matching key by has and identifier */
		if (apc_entry_key_equals(entry, key)) {
			/* Check to make sure this entry isn't expired by a hard TTL */
			if (apc_cache_entry_hard_expired(cache, entry, t)) {
				break;
			}

			return entry;
		}

		entry_offset = entry->next;
	}

	return NULL;
}

/* Find entry, updating stat counters and access time */
static inline apc_cache_entry_t *apc_cache_rlocked_find(
		apc_cache_t *cache, zend_string *key, time_t t) {
	size_t s;

	/* calculate hash and slot */
	apc_cache_hash_slot(cache, key, &s);

	uintptr_t entry_offset = cache->slots[s];
	while (entry_offset) {
		apc_cache_entry_t *entry = ENTRYAT(entry_offset);

		/* check for a matching key by has and identifier */
		if (apc_entry_key_equals(entry, key)) {
			/* Check to make sure this entry isn't expired by a hard TTL */
			if (apc_cache_entry_hard_expired(cache, entry, t)) {
				break;
			}

			ATOMIC_INC_RLOCKED(cache->header->nhits);
			ATOMIC_INC_RLOCKED(entry->nhits);
			entry->atime = t;

			return entry;
		}

		entry_offset = entry->next;
	}

	ATOMIC_INC_RLOCKED(cache->header->nmisses);
	return NULL;
}

static inline apc_cache_entry_t *apc_cache_rlocked_find_incref(
		apc_cache_t *cache, zend_string *key, time_t t) {
	apc_cache_entry_t *entry = apc_cache_rlocked_find(cache, key, t);
	if (!entry) {
		return NULL;
	}

	ATOMIC_INC_RLOCKED(entry->ref_count);
	return entry;
}

/* The shared store engine: persists the value outside the lock, then attaches
 * it under the write lock — in place for an existing key, linking a fresh
 * entry otherwise. Mode-specific behavior:
 *   - exclusive (apcu_add): fails on a live entry.
 *   - skipped != NULL, ei == NULL (apcu_update): an identical live value is
 *     not replaced; its metadata is refreshed and *skipped is set.
 *   - skipped != NULL, ei != NULL (apcu_add_ei): a live entry with matching
 *     identifier is left completely untouched and *skipped is set.
 * Takes ownership of serialized_str. Returns true when stored or skipped. */
static zend_bool apc_cache_store_ex(
		apc_cache_t* cache, zend_string *key, const zval *val,
		const zend_long ttl, const zend_bool exclusive,
		zend_string *ei, unsigned char *serialized_str, size_t serialized_str_len,
		zend_bool *skipped) {
	time_t t = apc_time();
	zend_bool ret = 0;
	zend_bool done = 0;
	apc_cache_entry_t *new_entry = NULL;
	apc_cache_entry_data_t *data;
	char *pending_batch = NULL;

	/* run cache defense */
	if (apc_cache_defense(cache, key, t)) {
		if (serialized_str) {
			efree(serialized_str);
		}
		return 0;
	}

	/* create the value in the shared memory (takes ownership of serialized_str) */
	data = apc_persist_data_ex(
		cache->sma, cache->serializer, val, serialized_str, serialized_str_len, ei);
	if (!data) {
		return 0;
	}

	/* Existing keys take the fast path: the value pointer is swapped under
	 * the READ lock — writers to existing keys behave like readers and no
	 * longer queue on (or exclude anyone from) the write lock (GH #345).
	 * New keys, and the update flavor's identical-refresh (which mutates the
	 * current block's metadata), fall through to the write-lock path.
	 * With a global cache ttl configured, stores keep the write-locked slot
	 * walk that soft-expiry reaping relies on (see apc_019.phpt), so the
	 * fast path is limited to the apc.ttl=0 default. */
	if (cache->ttl == 0 && apc_cache_rlock(cache)) {
		apc_cache_entry_t *entry = apc_cache_rlocked_find_nostat(cache, key, t);
		zend_bool handled = 0;

		if (entry) {
			apc_cache_entry_data_t *cur = ENTRY_DATA(entry);

			if (exclusive) {
				/* apcu_add: a live entry exists */
				handled = 1;
			} else if (skipped && ei && apc_data_ei_matches(cur, ei)) {
				/* apcu_add_ei: identifier already present — strict no-op */
				ATOMIC_INC_RLOCKED(cache->header->nskipped);
				*skipped = 1;
				ret = 1;
				handled = 1;
			} else if (skipped && !ei && apc_data_payloads_identical(cur, data)) {
				/* identical value: the metadata refresh needs the write lock */
			} else {
				apc_cache_entry_data_t *old;

				/* publish: metadata is written into the (unreachable)
				 * candidate first, the exchange releases it atomically */
				data->ttl = ttl;
				data->ctime = t;
				data->mtime = t;
				data->owner = ENTRYOF(entry);

				old = DATAAT(ATOMIC_XCHG_PTR(entry->data, DATAOF(data)));

				apc_cache_park_data(cache, old, t);

				ATOMIC_ADD(cache->header->mem_size, data->mem_size - old->mem_size);
				ATOMIC_INC_RLOCKED(cache->header->ninserts);
				entry->nhits = 0;
				entry->atime = t;

				/* drop the creation reference; readers may already pin it */
				ATOMIC_DEC(data->ref_count);

				data = NULL; /* consumed */
				ret = 1;
				handled = 1;
			}
		}
		apc_cache_runlock(cache);

		if (handled) {
			if (data) {
				/* unconsumed candidate (add-fail or ei-skip): never published */
				free_data(cache, data);
			}

			/* A swap-only workload never takes the write lock, so nobody
			 * would ever drain the parked blocks: past a threshold, try to
			 * take it — without waiting — and reap. Skipping under contention
			 * is fine: some later swapper (or any write-locked operation)
			 * will get it, and memory stays recycled instead of streamed.
			 * Limit case: under a permanent all-reader storm every try may
			 * fail and nparked can grow past the threshold; the backstop is
			 * SMA pressure itself — a failing allocation forces an expunge,
			 * which holds the write lock and reaps everything parked. */
			if (ret && cache->header->nparked >= 64 && apc_cache_try_wlock(cache)) {
				apc_cache_wlocked_gc(cache);
				apc_cache_wunlock(cache);
			}
			return ret;
		}
	}

	while (!done) {
		done = 1;

		if (!apc_cache_wlock(cache)) {
			break;
		}

		php_apc_try {
			uintptr_t *tail;
			apc_cache_entry_t *entry;

			/* hand a batch allocated on the previous iteration to the pool */
			if (pending_batch) {
				apc_cache_wlocked_pool_fill(cache, (apc_pool_batch_t *) pending_batch);
				pending_batch = NULL;
			}

			/* process deleted lists */
			apc_cache_wlocked_gc(cache);

			entry = apc_cache_wlocked_find_slot(cache, key, t, &tail);
			if (entry) {
				if (exclusive) {
					/* a live entry exists: apcu_add fails */
				} else if (skipped && ei && apc_data_ei_matches(ENTRY_DATA(entry), ei)) {
					/* apcu_add_ei: identifier already present — strict no-op */
					cache->header->nskipped++;
					*skipped = 1;
					ret = 1;
				} else if (skipped && !ei
						&& apc_data_payloads_identical(ENTRY_DATA(entry), data)) {
					/* apcu_update: identical value — refresh metadata only
					 * (in place: the write lock excludes all readers) */
					apc_cache_entry_data_t *d = ENTRY_DATA(entry);
					d->ttl = ttl;
					d->ctime = t;
					d->mtime = t;
					entry->atime = t;
					cache->header->nskipped++;
					*skipped = 1;
					ret = 1;
				} else {
					apc_cache_wlocked_attach_data(cache, entry, data, ttl, t);
					ret = 1;
				}
			} else {
				/* Normal-length keys take their entry block from the pool —
				 * no SMA traffic while the lock is held (GH #346). */
				if (!new_entry && ZSTR_LEN(key) <= APC_POOL_KEY_MAX) {
					new_entry = apc_cache_wlocked_pool_pop(cache, key);
				}

				if (new_entry) {
					apc_cache_wlocked_insert_entry(cache, tail, new_entry, data, ttl, t);
					new_entry = NULL;
					ret = 1;
				} else {
					/* Pool empty or oversized key: allocate outside the lock
					 * (the allocation may trigger an expunge, which takes the
					 * write lock) and retry. */
					done = 0;
				}
			}
		} php_apc_finally {
			apc_cache_wunlock(cache);
		} php_apc_end_try();

		if (!done) {
			if (ZSTR_LEN(key) <= APC_POOL_KEY_MAX) {
				/* Opportunistic: growing the pool must never expunge the
				 * cache; when memory is tight, the entry is allocated
				 * directly and pays its own (justified) expunge. */
				pending_batch = apc_sma_try_malloc_ex(
					cache->sma, APC_POOL_BATCH_BYTES, APC_SMA_BLOCK_POOL, NULL);
			}
			if (!pending_batch) {
				/* oversized key, or batch refused under memory pressure:
				 * a single directly allocated entry block */
				new_entry = apc_persist_alloc_entry(cache->sma, key);
				if (!new_entry) {
					break;
				}
			}
		}
	}

	if (!ret || (skipped && *skipped)) {
		/* failed or unused candidate value; it was never attached, so free it */
		free_data(cache, data);
	}
	if (pending_batch) {
		/* the write lock failed before the batch reached the pool */
		apc_sma_free(cache->sma, pending_batch);
	}
	if (new_entry) {
		/* lost the existence race; the directly allocated entry block is unused
		 * (pool slots never leave the locked section, so this is never pooled) */
		free_entry(cache, new_entry);
	}

	return ret;
}

PHP_APCU_API zend_bool apc_cache_store(
		apc_cache_t* cache, zend_string *key, const zval *val,
		const zend_long ttl, const zend_bool exclusive) {
	if (!cache) {
		return 0;
	}

	return apc_cache_store_ex(cache, key, val, ttl, exclusive, NULL, NULL, 0, NULL);
}

/* Find the live entry for key and, when its value is identical to the candidate,
 * refresh its metadata as if it had been stored. Must be called under write lock. */
static zend_bool apc_cache_wlocked_refresh_identical(
		apc_cache_t *cache, zend_string *key, const zval *val,
		const unsigned char *serialized_str, size_t serialized_str_len,
		const zend_long ttl, time_t t) {
	apc_cache_entry_t *entry = apc_cache_rlocked_find_nostat(cache, key, t);

	if (!entry || !apc_data_value_identical(
			ENTRY_DATA(entry), val, serialized_str, serialized_str_len)) {
		return 0;
	}

	{
		apc_cache_entry_data_t *d = ENTRY_DATA(entry);
		d->ttl = ttl;
		d->ctime = t;
		d->mtime = t;
	}
	entry->atime = t;
	cache->header->nskipped++;
	return 1;
}

PHP_APCU_API zend_bool apc_cache_store_if_changed(
		apc_cache_t *cache, zend_string *key, const zval *val, const zend_long ttl) {
	time_t t = apc_time();
	unsigned char *serialized_str = NULL;
	size_t serialized_str_len = 0;
	zend_bool have_compare_form = 0;
	zend_bool skipped = 0;

	if (!cache) {
		return 0;
	}

	/* Build a request-local comparison form where one exists without touching
	 * shm: scalars and strings compare directly; objects (and arrays under an
	 * explicit serializer) are serialized here — the store engine below reuses
	 * the buffer, so the store path serializes exactly once. Structurally
	 * persisted arrays have no cheap comparison form and take the plain store
	 * path (their identical stores are not skipped). */
	if (Z_TYPE_P(val) <= IS_STRING) {
		have_compare_form = 1;
	} else if (Z_TYPE_P(val) == IS_OBJECT
			|| (cache->serializer && Z_TYPE_P(val) == IS_ARRAY)) {
		if (!apc_persist_serialize_value(
				cache->serializer, val, &serialized_str, &serialized_str_len)) {
			/* the persist path inside the store engine would fail the same way */
			return 0;
		}
		have_compare_form = 1;
	}

	if (have_compare_form) {
		if (!apc_cache_wlock(cache)) {
			if (serialized_str) {
				efree(serialized_str);
			}
			return 0;
		}

		skipped = apc_cache_wlocked_refresh_identical(
			cache, key, val, serialized_str, serialized_str_len, ttl, t);
		apc_cache_wunlock(cache);

		if (skipped) {
			if (serialized_str) {
				efree(serialized_str);
			}
			return 1;
		}
	}

	/* The stored value may have become identical to ours since the compare
	 * above; the store engine re-checks under the lock before replacing. */
	return apc_cache_store_ex(
		cache, key, val, ttl, 0, NULL, serialized_str, serialized_str_len, &skipped);
}

PHP_APCU_API zend_bool apc_cache_add_ei(
		apc_cache_t *cache, zend_string *key, zend_string *ei, const zval *val,
		const zend_long ttl) {
	time_t t = apc_time();
	zend_bool skipped = 0;
	apc_cache_entry_t *entry;

	if (!cache) {
		return 0;
	}

	/* Fast path: when the live entry's identifier already matches, nothing is
	 * written and nothing is serialized. The read lock suffices: replacement,
	 * GC and defragmentation all require the write lock, so the observed
	 * (value, identifier) pair is the entry's current one for the duration of
	 * the lock — the same discipline apc_cache_atomic_update_long relies on.
	 * A matching wave of concurrent callers therefore proceeds in parallel,
	 * like readers. Model: AddEi.tla (PairCoherent/NoStaleFresh). */
	if (!apc_cache_rlock(cache)) {
		return 0;
	}

	entry = apc_cache_rlocked_find_nostat(cache, key, t);
	if (entry && apc_data_ei_matches(ENTRY_DATA(entry), ei)) {
		ATOMIC_INC_RLOCKED(cache->header->nskipped);
		apc_cache_runlock(cache);
		return 0;
	}
	apc_cache_runlock(cache);

	/* A concurrent writer may store the same identifier before we take the
	 * write lock; the store engine re-checks under the lock before replacing. */
	if (!apc_cache_store_ex(cache, key, val, ttl, 0, ei, NULL, 0, &skipped)) {
		return 0;
	}

	/* add semantics: a skip means the identifier was already present */
	return !skipped;
}

#ifndef ZTS
static zval data_unserialize(const char *filename)
{
	zval retval;
	zend_long len = 0;
	zend_stat_t sb;
	char *contents, *tmp;
	FILE *fp;
	php_unserialize_data_t var_hash = {0,};

	if(VCWD_STAT(filename, &sb) == -1) {
		return EG(uninitialized_zval);
	}

	fp = fopen(filename, "rb");

	len = sizeof(char)*sb.st_size;

	tmp = contents = malloc(len);

	if(!contents) {
		fclose(fp);
		return EG(uninitialized_zval);
	}

	if(fread(contents, 1, len, fp) < 1) {
		fclose(fp);
		free(contents);
		return EG(uninitialized_zval);
	}

	ZVAL_UNDEF(&retval);

	PHP_VAR_UNSERIALIZE_INIT(var_hash);

	/* I wish I could use json */
	if(!php_var_unserialize(&retval, (const unsigned char**)&tmp, (const unsigned char*)(contents+len), &var_hash)) {
		fclose(fp);
		free(contents);
		return EG(uninitialized_zval);
	}

	PHP_VAR_UNSERIALIZE_DESTROY(var_hash);

	free(contents);
	fclose(fp);

	return retval;
}

static int apc_load_data(apc_cache_t* cache, const char *data_file)
{
	char *p;
	char key[MAXPATHLEN] = {0,};
	size_t key_len;
	zval data;

	p = strrchr(data_file, DEFAULT_SLASH);

	if(p && p[1]) {
		strlcpy(key, p+1, sizeof(key));
		p = strrchr(key, '.');

		if(p) {
			p[0] = '\0';
			key_len = strlen(key);

			data = data_unserialize(data_file);
			if(Z_TYPE(data) != IS_UNDEF) {
				zend_string *name = zend_string_init(key, key_len, 0);
				apc_cache_store(
					cache, name, &data, 0, 1);
				zend_string_release(name);
				zval_ptr_dtor_nogc(&data);
			}
			return 1;
		}
	}

	return 0;
}
#endif

/* apc_cache_preload shall load the prepared data files in path into the specified cache */
PHP_APCU_API zend_bool apc_cache_preload(apc_cache_t* cache, const char *path)
{
#ifndef ZTS
	zend_bool result = 0;
	char file[MAXPATHLEN]={0,};
	int ndir, i;
	char *p = NULL;
	struct dirent **namelist = NULL;

	if ((ndir = php_scandir(path, &namelist, 0, php_alphasort)) > 0) {
		for (i = 0; i < ndir; i++) {
			/* check for extension */
			if (!(p = strrchr(namelist[i]->d_name, '.'))
					|| (p && strcmp(p, ".data"))) {
				free(namelist[i]);
				continue;
			}

			snprintf(file, MAXPATHLEN, "%s%c%s",
					path, DEFAULT_SLASH, namelist[i]->d_name);

			if(apc_load_data(cache, file)) {
				result = 1;
			}
			free(namelist[i]);
		}
		free(namelist);
	}
	return result;
#else
	apc_error("Cannot load data from apc.preload_path=%s in thread-safe mode", path);
	return 0;
#endif
}

PHP_APCU_API void apc_cache_entry_incref(apc_cache_t *cache, apc_cache_entry_t *entry)
{
	ATOMIC_INC_RLOCKED(entry->ref_count);
}

/* Pin an entry's current data block for reading after the lock is released.
 * Must be called under (at least) the read lock, which excludes swaps, so the
 * observed block stays this entry's value while the lock is held and stays
 * allocated as long as the pin is. */
PHP_APCU_API apc_cache_entry_data_t *apc_cache_entry_data_incref(
		apc_cache_t *cache, apc_cache_entry_t *entry)
{
	apc_cache_entry_data_t *data = ENTRY_DATA(entry);
	ATOMIC_INC_RLOCKED(data->ref_count);
	return data;
}

PHP_APCU_API void apc_cache_data_release(apc_cache_t *cache, apc_cache_entry_data_t *data)
{
	ATOMIC_DEC(data->ref_count);
}

PHP_APCU_API void apc_cache_entry_release(apc_cache_t *cache, apc_cache_entry_t *entry)
{
	ATOMIC_DEC(entry->ref_count);
}

PHP_APCU_API void apc_cache_detach(apc_cache_t *cache)
{
	/* Important: This function should not clean up anything that's in shared memory,
	 * only detach our process-local use of it. In particular locks cannot be destroyed
	 * here. */

	if (!cache) {
		return;
	}

	free(cache);
}

static void apc_cache_wlocked_real_expunge(apc_cache_t* cache) {
	size_t i;

	/* increment counter */
	cache->header->nexpunges++;

	/* expunge */
	for (i = 0; i < cache->nslots; i++) {
		uintptr_t *entry_offset = &cache->slots[i];
		while (*entry_offset) {
			apc_cache_wlocked_remove_entry(cache, ENTRYAT(*entry_offset));
		}
	}

	/* Reclaim entry-pool batches. Batches still containing live slots (only
	 * possible for reference-parked entries on the gc list) are kept and
	 * their free slots re-listed; everything else is returned to the SMA. */
	{
		uintptr_t *batch_link = &cache->header->pool_batches;

		cache->header->entry_pool = 0;

		while (*batch_link) {
			apc_pool_batch_t *batch =
				(apc_pool_batch_t *) ((uintptr_t) cache->header + *batch_link);
			zend_bool live = 0;
			int i;

			for (i = 0; i < APC_POOL_BATCH; i++) {
				if (APC_POOL_BATCH_SLOT(batch, i)->pooled == APC_ENTRY_POOL_LIVE) {
					live = 1;
					break;
				}
			}

			if (live) {
				for (i = 0; i < APC_POOL_BATCH; i++) {
					apc_cache_entry_t *slot = APC_POOL_BATCH_SLOT(batch, i);
					if (slot->pooled == APC_ENTRY_POOL_FREE) {
						apc_cache_wlocked_pool_push(cache, slot);
					}
				}
				batch_link = &batch->next;
			} else {
				*batch_link = batch->next;
				apc_sma_free(cache->sma, batch);
			}
		}
	}

	/* set new time so counters make sense */
	cache->header->stime = apc_time();

	/* reset counters */
	cache->header->ninserts = 0;
	cache->header->nskipped = 0;
	cache->header->nentries = 0;
	cache->header->nhits = 0;
	cache->header->nmisses = 0;

	/* resets lastkey */
	memset(&cache->header->lastkey, 0, sizeof(apc_cache_slam_key_t));
}

PHP_APCU_API void apc_cache_clear(apc_cache_t* cache)
{
	if (!cache) {
		return;
	}

	if (!apc_cache_wlock(cache)) {
		return;
	}

	/* expunge cache */
	apc_cache_wlocked_real_expunge(cache);

	/* garbage collection */
	apc_cache_wlocked_gc(cache);

	/* set info */
	cache->header->ncleanups = 0;
	cache->header->ndefragmentations = 0;
	cache->header->nexpunges = 0;

	apc_cache_wunlock(cache);
}

PHP_APCU_API zend_bool apc_cache_default_expunge(apc_cache_t* cache, size_t size)
{
	time_t t;
	size_t i;

	if (!cache) {
		return 1;
	}

	/* get the number of cleanups before acquiring the lock */
	zend_long ncleanups = cache->header->ncleanups;

	/* apc_time() depends on globals, don't read it if there's no cache. This may happen if SHM
	 * is too small and the initial cache creation during MINIT triggers an expunge. */
	t = apc_time();

	/* get the lock for header */
	if (!apc_cache_wlock(cache)) {
		return 1;
	}

	/* skip processing if another default expunge operation was performed while waiting for the write lock */
	if (ncleanups < cache->header->ncleanups) {
		apc_cache_wunlock(cache);
		return 0;
	}

	/* expunge_threshold specifies the percentage of memory that must be free after removing
	 * expired entries. If not enough memory could be freed, a full cache wipe will be performed,
	 * so that the default expunge operation is called less often when memory pressure is high. */
	size += cache->sma->size / 100 * cache->expunge_threshold;

	/* if size >= total shm size (e.g., if expunge_threshold >= 100), skip removing
	 * expired entries and defragmentation as this always results in a real expunge */
	if (size >= cache->sma->size) {
		apc_cache_wlocked_real_expunge(cache);
		apc_cache_wlocked_gc(cache);
		goto end_lbl;
	}

	/* remove expired entries */
	for (i = 0; i < cache->nslots; i++) {
		uintptr_t *entry_offset = &cache->slots[i];
		while (*entry_offset) {
			apc_cache_entry_t *entry = ENTRYAT(*entry_offset);

			if (apc_cache_entry_expired(cache, entry, t)) {
				apc_cache_wlocked_remove_entry(cache, entry);
				continue;
			}

			/* grab next entry */
			entry_offset = &entry->next;
		}
	}

	/* gc */
	apc_cache_wlocked_gc(cache);

	/* if all free blocks together do not provide enough memory, we immediately perform a real expunge */
	if (!apc_sma_check_avail(cache->sma, size)) {
		apc_cache_wlocked_real_expunge(cache);
		goto end_lbl;
	}

	/* increment defragmentation statistics */
	cache->header->ndefragmentations++;

	/* run defragmentation to coalesce free blocks */
	apc_sma_defrag(cache->sma, cache, (apc_sma_move_f)apc_cache_wlocked_move_block);

	/* if size bytes can't be allocated as a contiguous block after defragmentation, we do a real expunge */
	if (!apc_sma_check_avail_contiguous(cache->sma, size)) {
		apc_cache_wlocked_real_expunge(cache);
		goto end_lbl;
	}

	/* wipe lastkey */
	memset(&cache->header->lastkey, 0, sizeof(apc_cache_slam_key_t));

end_lbl:
	/* Increment cache cleanup statistics (removal of expired entries).
	 * This should be done late to detect stacking of default expunge operations. */
	cache->header->ncleanups++;

	apc_cache_wunlock(cache);
	return 1;
}

PHP_APCU_API zend_bool apc_cache_fetch(apc_cache_t* cache, zend_string *key, time_t t, zval *dst)
{
	apc_cache_entry_t *entry;
	apc_cache_entry_data_t *data;
	zend_bool retval = 0;

	if (!cache) {
		return 0;
	}

	if (!apc_cache_rlock(cache)) {
		return 0;
	}

	entry = apc_cache_rlocked_find(cache, key, t);
	if (!entry) {
		apc_cache_runlock(cache);
		return 0;
	}

	/* Pin the value, not the entry: the entry's data pointer may be swapped
	 * in place by a writer once the lock is released (GH #346). */
	data = apc_cache_entry_data_incref(cache, entry);
	apc_cache_runlock(cache);

	php_apc_try {
		retval = apc_cache_data_fetch_zval(cache, data, dst);
	} php_apc_finally {
		apc_cache_data_release(cache, data);
	} php_apc_end_try();

	return retval;
}

PHP_APCU_API zend_bool apc_cache_exists(apc_cache_t* cache, zend_string *key, time_t t)
{
	apc_cache_entry_t *entry;

	if (!cache) {
		return 0;
	}

	if (!apc_cache_rlock(cache)) {
		return 0;
	}

	entry = apc_cache_rlocked_find(cache, key, t);
	apc_cache_runlock(cache);

	return entry != NULL;
}

PHP_APCU_API zend_bool apc_cache_update(
		apc_cache_t *cache, zend_string *key, apc_cache_updater_t updater, void *data,
		zend_bool insert_if_not_found, zend_long ttl)
{
	apc_cache_entry_t *entry;
	zend_bool retval = 0;
	time_t t = apc_time();

	if (!cache) {
		return 0;
	}

retry_update:
	if (!apc_cache_wlock(cache)) {
		return 0;
	}

	entry = apc_cache_rlocked_find_nostat(cache, key, t);
	if (entry) {
		apc_cache_entry_data_t *d = ENTRY_DATA(entry);

		/* Only allow changes to simple values */
		if (Z_TYPE(d->val) < IS_STRING) {
			retval = updater(cache, d, data);
			d->mtime = t;
		}

		apc_cache_wunlock(cache);
		return retval;
	}

	apc_cache_wunlock(cache);
	if (insert_if_not_found) {
		/* Failed to find matching entry. Add key with value 0 and run the updater again. */
		zval val;
		ZVAL_LONG(&val, 0);

		/* We do not check the return value of the exclusive-store (add), as the entry might have
		 * been added between the cache unlock and the store call. In this case we just want to
		 * update the entry created by a different process. */
		apc_cache_store(cache, key, &val, ttl, 1);

		/* Only attempt to perform insertion once. */
		insert_if_not_found = 0;
		goto retry_update;
	}

	return 0;
}

PHP_APCU_API zend_bool apc_cache_atomic_update_long(
		apc_cache_t *cache, zend_string *key, apc_cache_atomic_updater_t updater, void *data,
		zend_bool insert_if_not_found, zend_long ttl)
{
	apc_cache_entry_t *entry;
	zend_bool retval = 0;
	time_t t = apc_time();

	if (!cache) {
		return 0;
	}

retry_update:
	if (!apc_cache_rlock(cache)) {
		return 0;
	}

	entry = apc_cache_rlocked_find_nostat(cache, key, t);
	if (entry) {
		/* Load the data pointer ONCE: a concurrent store may swap the entry's
		 * value under this same read lock (GH #345). The loaded block cannot
		 * be freed or moved while the read lock is held; an update landing in
		 * a just-parked block is the ordinary lost-to-a-concurrent-store race,
		 * identical to the serialized orders. */
		apc_cache_entry_data_t *d = ENTRY_DATA(entry);

		/* Only supports integers */
		if (Z_TYPE(d->val) == IS_LONG) {
			retval = updater(cache, &Z_LVAL(d->val), data);
			d->mtime = t;
		}

		apc_cache_runlock(cache);
		return retval;
	}

	apc_cache_runlock(cache);
	if (insert_if_not_found) {
		/* Failed to find matching entry. Add key with value 0 and run the updater again. */
		zval val;
		ZVAL_LONG(&val, 0);

		/* We do not check the return value of the exclusive-store (add), as the entry might have
		 * been added between the cache unlock and the store call. In this case we just want to
		 * update the entry created by a different process. */
		apc_cache_store(cache, key, &val, ttl, 1);

		/* Only attempt to perform insertion once. */
		insert_if_not_found = 0;
		goto retry_update;
	}

	return 0;
}

PHP_APCU_API zend_bool apc_cache_delete(apc_cache_t *cache, zend_string *key)
{
	size_t s;

	if (!cache) {
		return 0;
	}

	/* calculate hash and slot */
	apc_cache_hash_slot(cache, key, &s);

	if (!apc_cache_wlock(cache)) {
		return 0;
	}

	/* find head */
	uintptr_t *entry_offset = &cache->slots[s];
	while (*entry_offset) {
		apc_cache_entry_t *entry = ENTRYAT(*entry_offset);

		/* check for a match by hash and identifier */
		if (apc_entry_key_equals(entry, key)) {
			/* executing removal */
			apc_cache_wlocked_remove_entry(cache, entry);

			apc_cache_wunlock(cache);
			return 1;
		}

		entry_offset = &entry->next;
	}

	apc_cache_wunlock(cache);
	return 0;
}

PHP_APCU_API zend_bool apc_cache_data_fetch_zval(
		apc_cache_t *cache, apc_cache_entry_data_t *data, zval *dst)
{
	return apc_unpersist(dst, data, cache->serializer);
}

static inline void array_add_long(zval *array, zend_string *key, zend_long lval) {
	zval zv;
	ZVAL_LONG(&zv, lval);
	zend_hash_add_new(Z_ARRVAL_P(array), key, &zv);
}

static inline void array_add_double(zval *array, zend_string *key, double dval) {
	zval zv;
	ZVAL_DOUBLE(&zv, dval);
	zend_hash_add_new(Z_ARRVAL_P(array), key, &zv);
}

static zval apc_cache_link_info(apc_cache_t *cache, apc_cache_entry_t *p)
{
	zval link, zv;
	array_init(&link);

	ZVAL_STR(&zv, zend_string_dup(&p->key, 0));
	zend_hash_add_new(Z_ARRVAL(link), apc_str_info, &zv);

	{
		const apc_cache_entry_data_t *d = p->data ? ENTRY_DATA(p) : NULL;
		array_add_long(&link, apc_str_ttl, d ? d->ttl : 0);
		array_add_double(&link, apc_str_num_hits, (double) p->nhits);
		array_add_long(&link, apc_str_mtime, d ? d->mtime : 0);
		array_add_long(&link, apc_str_creation_time, d ? d->ctime : 0);
	}
	array_add_long(&link, apc_str_deletion_time, p->dtime);
	array_add_long(&link, apc_str_access_time, p->atime);
	array_add_long(&link, apc_str_ref_count, p->ref_count);
	array_add_long(&link, apc_str_mem_size,
		p->mem_size + (p->data ? ENTRY_DATA(p)->mem_size : 0));

	return link;
}

PHP_APCU_API zend_bool apc_cache_info(zval *info, apc_cache_t *cache, zend_bool limited)
{
	zval list;
	zval gc;
	zval slots;
	uintptr_t entry_offset;
	zend_ulong j;

	ZVAL_NULL(info);
	if (!cache) {
		return 0;
	}

	if (!apc_cache_rlock(cache)) {
		return 0;
	}

	php_apc_try {
		array_init(info);
		add_assoc_long(info, "num_slots", cache->nslots);
		array_add_long(info, apc_str_ttl, cache->ttl);
		array_add_double(info, apc_str_num_hits, (double) cache->header->nhits);
		add_assoc_double(info, "num_misses", (double) cache->header->nmisses);
		add_assoc_double(info, "num_inserts", (double) cache->header->ninserts);
		add_assoc_double(info, "num_skipped", (double) cache->header->nskipped);
		add_assoc_long(info,   "num_entries", cache->header->nentries);
		add_assoc_long(info, "cleanups", cache->header->ncleanups);
		add_assoc_long(info, "defragmentations", cache->header->ndefragmentations);
		add_assoc_long(info, "expunges", cache->header->nexpunges);
		add_assoc_long(info, "start_time", cache->header->stime);
		array_add_double(info, apc_str_mem_size, (double) cache->header->mem_size);

#ifdef APC_MMAP
		add_assoc_stringl(info, "memory_type", "mmap", sizeof("mmap")-1);
#else
		add_assoc_stringl(info, "memory_type", "IPC shared", sizeof("IPC shared")-1);
#endif

		if (!limited) {
			size_t i;

			/* For each hashtable slot */
			array_init(&list);
			array_init(&slots);

			for (i = 0; i < cache->nslots; i++) {
				j = 0;
				entry_offset = cache->slots[i];
				while (entry_offset) {
					apc_cache_entry_t *entry = ENTRYAT(entry_offset);
					zval link = apc_cache_link_info(cache, entry);

					add_next_index_zval(&list, &link);
					j++;
					entry_offset = entry->next;
				}
				if (j != 0) {
					add_index_long(&slots, (zend_ulong)i, j);
				}
			}

			/* For each slot pending deletion */
			array_init(&gc);

			entry_offset = cache->header->gc;
			while (entry_offset) {
				apc_cache_entry_t *entry = ENTRYAT(entry_offset);
				zval link = apc_cache_link_info(cache, entry);

				add_next_index_zval(&gc, &link);
				entry_offset = entry->next;
			}

			add_assoc_zval(info, "cache_list", &list);
			add_assoc_zval(info, "deleted_list", &gc);
			add_assoc_zval(info, "slot_distribution", &slots);
		}
	} php_apc_finally {
		apc_cache_runlock(cache);
	} php_apc_end_try();

	return 1;
}

/* fetches information about the key provided */
PHP_APCU_API void apc_cache_stat(apc_cache_t *cache, zend_string *key, zval *stat) {
	size_t s;

	ZVAL_NULL(stat);
	if (!cache) {
		return;
	}

	/* calculate hash and slot */
	apc_cache_hash_slot(cache, key, &s);

	if (!apc_cache_rlock(cache)) {
		return;
	}

	php_apc_try {
		/* find head */
		uintptr_t entry_offset = cache->slots[s];
		while (entry_offset) {
			apc_cache_entry_t *entry = ENTRYAT(entry_offset);

			/* check for a matching key by has and identifier */
			if (apc_entry_key_equals(entry, key)) {
				array_init(stat);
				{
					const apc_cache_entry_data_t *d = ENTRY_DATA(entry);
					array_add_long(stat, apc_str_hits, entry->nhits);
					array_add_long(stat, apc_str_access_time, entry->atime);
					array_add_long(stat, apc_str_mtime, d->mtime);
					array_add_long(stat, apc_str_creation_time, d->ctime);
					array_add_long(stat, apc_str_deletion_time, entry->dtime);
					array_add_long(stat, apc_str_ttl, d->ttl);
				}
				array_add_long(stat, apc_str_refs, entry->ref_count);
				if (entry->data && ENTRY_DATA(entry)->ei) {
					zend_string *ei = apc_data_fetch_ei(ENTRY_DATA(entry));
					add_assoc_str(stat, "expiration_identifier", ei);
				}
				break;
			}

			/* next */
			entry_offset = entry->next;
		}
	} php_apc_finally {
		apc_cache_runlock(cache);
	} php_apc_end_try();
}

PHP_APCU_API zend_bool apc_cache_defense(apc_cache_t *cache, zend_string *key, time_t t)
{
	/* only continue if slam defense is enabled */
	if (cache->defend) {

		/* for copy of locking key struct */
		apc_cache_slam_key_t *last = &cache->header->lastkey;
		pid_t owner_pid = getpid();
#ifdef ZTS
		void ***owner_thread = TSRMLS_CACHE;
#endif

		/* check the hash and length match */
		/* check the time (last second considered slam) and context */
		if (last->hash == ZSTR_HASH(key) &&
			last->len == ZSTR_LEN(key) &&
			last->mtime == t &&
			(last->owner_pid != owner_pid
#ifdef ZTS
			 || last->owner_thread != owner_thread
#endif
			)
		) {
			/* potential cache slam */
			return 1;
		}

		/* sets enough information for an educated guess, but is not exact */
		last->hash = ZSTR_HASH(key);
		last->len = ZSTR_LEN(key);
		last->mtime = t;
		last->owner_pid = owner_pid;
#ifdef ZTS
		last->owner_thread = owner_thread;
#endif
	}

	return 0;
}

PHP_APCU_API void apc_cache_serializer(apc_cache_t* cache, const char* name) {
	if (cache && !cache->serializer) {
		cache->serializer = apc_find_serializer(name);
	}
}

PHP_APCU_API void apc_cache_entry(apc_cache_t *cache, zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long ttl, zend_long now, zval *return_value) {
	apc_cache_entry_t *entry = NULL;

	if (!cache) {
		return;
	}

	if (!apc_cache_wlock(cache)) {
		return;
	}

	APCG(entry_level)++;
	php_apc_try {
		entry = apc_cache_rlocked_find_incref(cache, key, now);
		if (!entry) {
			int result;
			zval params[1];
			ZVAL_STR_COPY(&params[0], key);

			fci->retval = return_value;
			fci->param_count = 1;
			fci->params = params;

			result = zend_call_function(fci, fcc);

			zval_ptr_dtor(&params[0]);

			if (result == SUCCESS && !EG(exception)) {
				apc_cache_wlocked_store_internal(cache, key, return_value, ttl, 1);
			}
		} else {
			apc_cache_data_fetch_zval(cache, ENTRY_DATA(entry), return_value);
			apc_cache_entry_release(cache, entry);
		}
	} php_apc_finally {
		APCG(entry_level)--;
		apc_cache_wunlock(cache);
	} php_apc_end_try();
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: noexpandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: noexpandtab sw=4 ts=4 sts=4
 */
