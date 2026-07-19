/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

#ifndef APC_CACHE_H
#define APC_CACHE_H

#include "apc.h"
#include "apc_sma.h"
#include "apc_lock.h"
#include "apc_globals.h"
#include "TSRM.h"

typedef struct apc_cache_slam_key_t apc_cache_slam_key_t;
struct apc_cache_slam_key_t {
	zend_ulong hash;         /* hash of the key */
	size_t len;              /* length of the key */
	time_t mtime;            /* creation time of this key */
	pid_t owner_pid;         /* the pid that created this key */
#ifdef ZTS
	void ***owner_thread;    /* TSRMLS cache of thread that created this key */
#endif
};

/*
 * The value of an entry lives in its own allocation, so that updating an
 * existing key replaces a pointer instead of relinking list nodes (GH #346).
 * All pointers inside the payload (zval Z_PTR, ei) are offsets relative to
 * the START OF THIS STRUCT; the block is relocatable as raw bytes.
 * The (val, ei) pair is only ever swapped as a whole — see AddEi.tla.
 */
typedef struct apc_cache_entry_data_t apc_cache_entry_data_t;
struct apc_cache_entry_data_t {
	uintptr_t owner;         /* offset (from cache header) of the owning entry, 0 = detached */
	uintptr_t gc_next;       /* next data block on the data gc list (only while detached) */
	zend_long ref_count;     /* readers currently unpersisting from this block */
	zend_long mem_size;      /* size of this allocation */
	zend_long ttl;           /* the ttl of this value (expiry travels with the value: it is
	                          * published atomically with it and can never be torn — GH #345) */
	time_t ctime;            /* time this value was stored (the expiry base) */
	time_t mtime;            /* last modification (ctime, or the last in-place inc/cas) */
	time_t dtime;            /* time the block was detached from its entry */
	uintptr_t ei;            /* offset to the expiration identifier string, 0 if none */
	zval val;                /* the zval copied at store time */
};

typedef struct apc_cache_entry_t apc_cache_entry_t;
struct apc_cache_entry_t {
	uintptr_t next;          /* offset to next entry (MUST BE THE 1st FIELD OF THE STRUCT!) */
	uintptr_t prev;          /* offset to previous entry / head-pointer of the linked list */
	zend_long ref_count;     /* the reference count of this entry */
	zend_long nhits;         /* number of hits to this entry */
	time_t dtime;            /* time entry was removed from cache */
	time_t atime;            /* time entry was last accessed */
	zend_long mem_size;      /* memory used by this block (the value is accounted separately) */
	zend_uchar pooled;       /* 0: directly allocated; 1: live pool slot; 2: free pool slot */
	uintptr_t data;          /* offset (from cache header) of the entry's apc_cache_entry_data_t;
	                          * swapped ATOMICALLY under the read lock by value updates (GH #345) */
	zend_string key;         /* entry key (MUST BE THE LAST FIELD OF THE STRUCT!) */
};

/* Any values that must be shared among processes should go in here. */
typedef struct _apc_cache_header_t {
	apc_lock_t lock;                /* header lock */
	zend_long nhits;                /* hit count */
	zend_long nmisses;              /* miss count */
	zend_long ninserts;             /* insert count */
	zend_long ncleanups;            /* default expunge count */
	zend_long ndefragmentations;    /* defragmentation count */
	zend_long nexpunges;            /* real expunge count */
	zend_long nskipped;             /* stores skipped because the value was identical (apcu_update) */
	zend_long nentries;             /* entry count */
	zend_long mem_size;             /* used */
	time_t stime;                   /* start time */
	apc_cache_slam_key_t lastkey;   /* last key inserted (not necessarily without error) */
	uintptr_t gc;                   /* offset in shm to the first entry of gc list */
	uintptr_t gc_data;              /* offset in shm to the first detached data block of the data gc list */
	zend_long nparked;              /* number of blocks on gc_data (drain trigger for read-locked swaps) */
	uintptr_t entry_pool;           /* offset in shm to the first free slot of the entry pool */
	uintptr_t pool_batches;         /* offset in shm to the first entry-pool batch block */
} apc_cache_header_t;

typedef struct _apc_cache_t {
	apc_cache_header_t* header;   /* cache header (stored in SHM) */
	uintptr_t* slots;             /* array of cache slots (stored in SHM) */
	apc_sma_t* sma;               /* shared memory allocator */
	apc_serializer_t* serializer; /* serializer */
	size_t nslots;                /* number of slots in cache */
	zend_long gc_ttl;             /* maximum time on GC list for a entry */
	zend_long ttl;                /* if slot is needed and entry's access time is older than this ttl, remove it */
	double expunge_threshold;     /* expunge if less than this percentage of shm is free after cleanup */
	zend_bool defend;             /* defense parameter for runtime */
} apc_cache_t;

typedef zend_bool (*apc_cache_updater_t)(apc_cache_t*, apc_cache_entry_data_t*, void* data);

typedef zend_bool (*apc_cache_atomic_updater_t)(apc_cache_t*, zend_long*, void* data);

/*
 * apc_cache_create creates the shared memory cache.
 *
 * This function should be called once per process per cache
 *
 * serializer for APCu is set by globals on MINIT and ensured with apc_cache_serializer
 * during execution. Using apc_cache_serializer avoids race conditions between MINIT/RINIT of
 * APCU and the third party serializer. API users can choose to leave this null to use default
 * PHP serializers, or search the list of serializers for the preferred serializer
 *
 * size_hint is a "hint" at the total number entries that will be expected.
 * It determines the physical size of the hash table. Passing 0 for
 * this argument will use a reasonable default value
 *
 * gc_ttl is the maximum time a cache entry may speed on the garbage
 * collection list. This is basically a work around for the inherent
 * unreliability of our reference counting mechanism (see apc_cache_release).
 *
 * ttl is the maximum time a cache entry can idle in a slot in case the slot
 * is needed.  This helps in cleaning up the cache and ensuring that entries
 * hit frequently stay cached and ones not hit very often eventually disappear.
 *
 * expunge_threshold specifies the percentage of memory that must be free after
 * removing expired entries during default expunge. If not enough memory could be freed,
 * a full cache wipe will be performed, so that the default expunge operation is called
 * less often when memory pressure is high.
 *
 * defend enables/disables slam defense for this particular cache
 */
PHP_APCU_API apc_cache_t* apc_cache_create(
        apc_sma_t* sma, apc_serializer_t* serializer, zend_long size_hint,
        zend_long gc_ttl, zend_long ttl, double expunge_threshold, zend_bool defend);

/*
* apc_cache_preload preloads the data at path into the specified cache
*/
PHP_APCU_API zend_bool apc_cache_preload(apc_cache_t* cache, const char* path);

/*
 * apc_cache_detach detaches from the shared memory cache and cleans up
 * local allocations. Under apache, this function can be safely called by
 * the child processes when they exit.
 */
PHP_APCU_API void apc_cache_detach(apc_cache_t* cache);

/*
 * apc_cache_clear empties a cache. This can safely be called at any time.
 */
PHP_APCU_API void apc_cache_clear(apc_cache_t* cache);

/*
 * apc_cache_store creates key, entry and context in which to make an insertion of val into the specified cache
 */
PHP_APCU_API zend_bool apc_cache_store(
        apc_cache_t* cache, zend_string *key, const zval *val,
        const zend_long ttl, const zend_bool exclusive);

/*
 * apc_cache_store_if_changed stores val under key like apc_cache_store, except that
 * when the cache already holds a live entry with an identical value, the store is
 * skipped entirely: no SMA allocation and no entry replacement; only the entry's
 * ttl/ctime/mtime/atime are refreshed. Identity detection is best-effort: false
 * negatives merely perform an ordinary store. The skip decision is only made while
 * holding the cache write lock. Returns true when the value was stored or skipped.
 */
PHP_APCU_API zend_bool apc_cache_store_if_changed(
        apc_cache_t* cache, zend_string *key, const zval *val, const zend_long ttl);

/*
 * apc_cache_add_ei stores val under key tagged with the expiration identifier ei,
 * unless the cache already holds a live entry for key whose identifier is identical —
 * then nothing is written or refreshed and 0 is returned (add semantics, like
 * apc_cache_store with exclusive on an existing key). Entries stored without an
 * identifier never match and are replaced. The match decision is only made while
 * holding the cache write lock; no serialization or allocation happens on a match.
 * Returns true only when the value was stored.
 */
PHP_APCU_API zend_bool apc_cache_add_ei(
        apc_cache_t* cache, zend_string *key, zend_string *ei, const zval *val,
        const zend_long ttl);

/*
 * apc_cache_update updates an entry in place. The updater function must not bailout.
 * The update is performed under write-lock and doesn't have to be atomic.
 */
PHP_APCU_API zend_bool apc_cache_update(
		apc_cache_t *cache, zend_string *key, apc_cache_updater_t updater, void *data,
		zend_bool insert_if_not_found, zend_long ttl);

/*
 * apc_cache_atomic_update_long updates an integer entry in place. The updater function must
 * perform the update atomically, as the update is performed under read-lock.
 */
PHP_APCU_API zend_bool apc_cache_atomic_update_long(
		apc_cache_t *cache, zend_string *key, apc_cache_atomic_updater_t updater, void *data,
		zend_bool insert_if_not_found, zend_long ttl);

/*
 * apc_cache_fetch fetches an entry from the cache directly into dst
 */
PHP_APCU_API zend_bool apc_cache_fetch(apc_cache_t* cache, zend_string *key, time_t t, zval *dst);

/*
 * apc_cache_exists searches for a cache entry by its hashed identifier,
 * and returns whether the entry exists.
 */
PHP_APCU_API zend_bool apc_cache_exists(apc_cache_t* cache, zend_string *key, time_t t);

/*
 * apc_cache_delete and apc_cache_delete finds an entry in the cache and deletes it.
 */
PHP_APCU_API zend_bool apc_cache_delete(apc_cache_t* cache, zend_string *key);

/*
 * apc_cache_data_fetch_zval copies a cached value to be usable at runtime.
 * The data block must be pinned (apc_cache_entry_data_incref) or otherwise
 * protected (a held lock) while this runs.
 */
PHP_APCU_API zend_bool apc_cache_data_fetch_zval(
		apc_cache_t *cache, apc_cache_entry_data_t *data, zval *dst);

/*
 * apc_cache_entry_data_incref pins an entry's current data block so it stays
 * allocated (and unmoved) after the lock is released; must be called under at
 * least the read lock. Release with apc_cache_data_release.
 */
PHP_APCU_API apc_cache_entry_data_t *apc_cache_entry_data_incref(
		apc_cache_t *cache, apc_cache_entry_t *entry);
PHP_APCU_API void apc_cache_data_release(apc_cache_t *cache, apc_cache_entry_data_t *data);

/*
 * apc_cache_entry_release decrements the reference count associated with a cache
 * entry. Calling apc_cache_rlocked_find_incref automatically increments the reference count,
 * and this function must be called post-execution to return the count to its
 * original value. Failing to do so will prevent the entry from being
 * garbage-collected.
 *
 * entry is the cache entry whose ref count you want to decrement.
 */
PHP_APCU_API void apc_cache_entry_release(apc_cache_t *cache, apc_cache_entry_t *entry);

/*
 * apc_cache_entry_incref increments the reference count of a cache entry to pin it,
 * preventing it from being freed or relocated by defragmentation while a reference is
 * held. It must be called while holding the cache read lock; release the reference with
 * apc_cache_entry_release once the entry is no longer needed.
 */
PHP_APCU_API void apc_cache_entry_incref(apc_cache_t *cache, apc_cache_entry_t *entry);

/*
 * fetches information about the cache provided for userland status functions
 */
PHP_APCU_API zend_bool apc_cache_info(zval *info, apc_cache_t *cache, zend_bool limited);

/*
 * fetches information about the key provided
 */
PHP_APCU_API void apc_cache_stat(apc_cache_t *cache, zend_string *key, zval *stat);

/*
* apc_cache_defense: guard against slamming a key
* will return true if the following conditions are met:
*  - the key provided has a matching hash and length to the last key inserted into cache
*  - the last key has a different owner
* in ZTS mode, TSRM determines owner
* in non-ZTS mode, PID determines owner
* Note: this function sets the owner of key during execution
*/
PHP_APCU_API zend_bool apc_cache_defense(apc_cache_t *cache, zend_string *key, time_t t);

/*
* apc_cache_serializer sets the serializer for a cache, and by proxy contexts created for the cache.
* Note: this avoids race conditions between third party serializers and APCu
*/
PHP_APCU_API void apc_cache_serializer(apc_cache_t* cache, const char* name);

/*
* The remaining functions allow a third party to reimplement expunge
*
* Look at the source of apc_cache_default_expunge for what is expected of this function
*
* The default behaviour of expunge is explained below, should no combination of those options
* be suitable, you will need to reimplement apc_cache_default_expunge and pass it to your
* call to apc_sma_api_impl, this will replace the default functionality.
* The functions below you can use during your own implementation of expunge to gain more
* control over how the expunge process works ...
*
* Note: beware of locking (copy it exactly), setting states is also important
*/

/*
* apc_cache_default_expunge() is executed by the sma layer when there is not enough
* free shared memory to satisfy an allocation request. It attempts to free memory
* (e.g., by removing entries) so that the allocation request can be satisfied.
*
* The TTL of an entry takes precedence over the TTL of a cache
*/
PHP_APCU_API zend_bool apc_cache_default_expunge(apc_cache_t* cache, size_t size);

/*
* apc_cache_entry: generate and create or fetch an entry
*
* @see https://github.com/krakjoe/apcu/issues/142
*/
PHP_APCU_API void apc_cache_entry(apc_cache_t *cache, zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long ttl, zend_long now, zval *return_value);

/* apcu_entry() holds a write lock on the cache while executing user code.
 * That code may call other apcu_* functions, which also try to acquire a
 * read or write lock, which would deadlock. As such, don't try to acquire a
 * lock if the current thread is inside apcu_entry().
 *
 * Whether the current thread is inside apcu_entry() is tracked by APCG(entry_level).
 * This breaks the self-contained apc_cache_t abstraction, but is currently
 * necessary because the entry_level needs to be tracked per-thread, while
 * apc_cache_t is a per-process structure.
 */

static inline zend_bool apc_cache_wlock(apc_cache_t *cache) {
	if (!APCG(entry_level)) {
		return WLOCK(&cache->header->lock);
	}
	return 1;
}

static inline zend_bool apc_cache_try_wlock(apc_cache_t *cache) {
	if (!APCG(entry_level)) {
		return TRY_WLOCK(&cache->header->lock);
	}
	return 1;
}

static inline void apc_cache_wunlock(apc_cache_t *cache) {
	if (!APCG(entry_level)) {
		WUNLOCK(&cache->header->lock);
	}
}

static inline zend_bool apc_cache_rlock(apc_cache_t *cache) {
	if (!APCG(entry_level)) {
		return RLOCK(&cache->header->lock);
	}
	return 1;
}

static inline void apc_cache_runlock(apc_cache_t *cache) {
	if (!APCG(entry_level)) {
		RUNLOCK(&cache->header->lock);
	}
}

/* APC_ENTRY_SIZE takes into account the trailing key-string + terminating 0-byte */
#define APC_ENTRY_SIZE(key_len) (ZEND_MM_ALIGNED_SIZE(offsetof(apc_cache_entry_t, key.val) + key_len + 1))

/* Entry blocks for keys up to this length come from a pool of fixed-size
 * slots, batch-allocated so that inserting a fresh key costs no SMA
 * allocation of its own (GH #346: "entry_t is allocated via a pool").
 * Longer keys fall back to a direct allocation. */
#define APC_POOL_KEY_MAX 152
#define APC_POOL_SLOT_SIZE APC_ENTRY_SIZE(APC_POOL_KEY_MAX)
#define APC_POOL_BATCH 64

/* Slot states in apc_cache_entry_t.pooled */
#define APC_ENTRY_DIRECT    0
#define APC_ENTRY_POOL_LIVE 1
#define APC_ENTRY_POOL_FREE 2

/* Pool slots come in batch blocks, registered in a list so that they can be
 * reclaimed by a real expunge. Each batch starts with this header. */
typedef struct apc_pool_batch_t {
	uintptr_t next;          /* offset (from cache header) of the next batch */
} apc_pool_batch_t;
#define APC_POOL_BATCH_HDR ZEND_MM_ALIGNED_SIZE(sizeof(apc_pool_batch_t))
#define APC_POOL_BATCH_BYTES (APC_POOL_BATCH_HDR + (size_t) APC_POOL_BATCH * APC_POOL_SLOT_SIZE)
#define APC_POOL_BATCH_SLOT(batch, i) \
	((apc_cache_entry_t *)((char *)(batch) + APC_POOL_BATCH_HDR + (size_t)(i) * APC_POOL_SLOT_SIZE))

/* SMA block types used by the cache (3 tag bits available) */
#define APC_SMA_BLOCK_ENTRY 0  /* a directly allocated [entry|key] block */
#define APC_SMA_BLOCK_DATA  1  /* an apc_cache_entry_data_t value block */
#define APC_SMA_BLOCK_POOL  2  /* a batch of entry-pool slots; immovable */

/* ENTRYAT and ENTRYOF are used to convert between offsets and pointers to cache entries.
 * Both expect the presence of cache->header that points to the cache header in the
 * shared memory segment. DATAAT/DATAOF are the same for entry data blocks;
 * ENTRY_DATA resolves an entry's current data block. */
#define ENTRYAT(offset) ((apc_cache_entry_t *)((uintptr_t)cache->header + (uintptr_t)offset))
#define ENTRYOF(entry) (((uintptr_t)entry) - (uintptr_t)cache->header)
#define DATAAT(offset) ((apc_cache_entry_data_t *)((uintptr_t)cache->header + (uintptr_t)offset))
#define DATAOF(data) (((uintptr_t)data) - (uintptr_t)cache->header)
#define ENTRY_DATA(entry) DATAAT((entry)->data)

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: noexpandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: noexpandtab sw=4 ts=4 sts=4
 */
