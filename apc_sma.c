/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
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

#include "apc_sma.h"
#include "apc.h"
#include "apc_globals.h"
#include "apc_mutex.h"
#include "apc_shm.h"
#include "apc_cache.h"

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include "apc_mmap.h"

#ifdef APC_SMA_DEBUG
# ifdef HAVE_VALGRIND_MEMCHECK_H
#  include <valgrind/memcheck.h>
# endif
# define APC_SMA_CANARIES 1
#endif

/* Magic stamped into shared_info once a file-backed shared segment is fully
 * initialized; processes seeing it may attach instead of initializing. */
#define APC_SHARED_SEG_MAGIC 0x55435041 /* "APCU" */

typedef struct apc_shared_seg_info_t {
	uint32_t magic;           /* APC_SHARED_SEG_MAGIC once fully initialized */
	uint32_t php_version_id;  /* PHP_VERSION_ID of the creating process */
	uint32_t layout_check;    /* sizeof checks of shm-resident structs */
	uint32_t retired;         /* set once a successor segment replaced this one */
	uint64_t seg_id;          /* unique id of this segment instance (identity across rotations) */
	size_t segsize;           /* creator's segment size */
	uintptr_t cache_off;      /* offset of the user cache header in the segment */
	size_t cache_span;        /* bytes the cache header + slots array occupy */
	size_t nslots;            /* user cache slot count chosen by the creator */
	char serializer[APC_SHARED_SEG_SERIALIZER_LEN]; /* creator's serializer name */
} apc_shared_seg_info_t;

typedef struct sma_header_t sma_header_t;
struct sma_header_t {
	apc_mutex_t sma_lock;   /* segment lock */
	size_t min_block_size;  /* expected minimum size of allocated blocks */
	size_t avail;           /* bytes available (not necessarily contiguous) */
	apc_shared_seg_info_t shared_info; /* only used with apc.mmap_shared_file */
};

#define SMA_DEFAULT_SEGSIZE (30*1024*1024)

#define SMA_HDR(sma)  ((sma_header_t*)sma->shmaddr)
#define SMA_ADDR(sma) ((char *)sma->shmaddr)
#define SMA_LCK(sma)  ((SMA_HDR(sma))->sma_lock)

#define SMA_CREATE_LOCK  APC_CREATE_MUTEX
#define SMA_DESTROY_LOCK APC_DESTROY_MUTEX
#define SMA_LOCK(sma) APC_MUTEX_LOCK(&SMA_LCK(sma))
#define SMA_UNLOCK(sma) APC_MUTEX_UNLOCK(&SMA_LCK(sma))

typedef struct block_t block_t;
struct block_t {
	size_t size;       /* size of this block */
	size_t prev_size;  /* size of sequentially previous block, 0 if prev is allocated */
	size_t fnext;      /* offset in segment of next free block */
	size_t fprev;      /* offset in segment of prev free block */
#ifdef APC_SMA_CANARIES
	size_t canary;     /* canary to check for memory overwrites */
#endif
};

/* The macros BLOCKAT and OFFSET are used for convenience throughout this
 * module. Both assume the presence of a variable smaheader that points to the
 * beginning of the shared memory segment. */
#define BLOCKAT(offset) ((block_t*)((char *)smaheader + offset))
#define OFFSET(block) ((size_t)(((char*)block) - (char*)smaheader))

static uint32_t apc_str_hash32(const char *s) {
	uint32_t h = 5381;
	while (*s) {
		h = ((h << 5) + h) ^ (unsigned char)*s++;
	}
	return h;
}

/*
 * A fingerprint of every build property that affects the binary layout of the
 * shm-resident structures or the meaning of the in-segment lock. Two processes
 * may share a segment only if these match. ZEND_MODULE_BUILD_ID encodes the
 * Zend API version, ZTS-vs-NTS and debug-vs-release (so a ZTS process cannot
 * silently attach to an NTS segment whose apc_cache_header_t differs); the
 * struct sizes catch APCu-side layout changes; sizeof(apc_lock_t) catches an
 * incompatible lock backend even when the other sizes coincide.
 */
static uint32_t apc_sma_layout_check(void) {
	return apc_str_hash32(ZEND_MODULE_BUILD_ID)
		^ (uint32_t)sizeof(sma_header_t)
		^ ((uint32_t)sizeof(block_t) << 8)
		^ ((uint32_t)sizeof(apc_mutex_t) << 16)
		^ ((uint32_t)sizeof(apc_lock_t) << 4)
		^ ((uint32_t)sizeof(apc_cache_header_t) << 12)
		^ ((uint32_t)sizeof(apc_cache_entry_t) << 20)
		^ ((uint32_t)SIZEOF_SIZE_T << 24);
}

/*
 * Generates an id that is unique per segment instance, so a process can tell
 * whether the file now at the shared path is still the segment it has mapped
 * (a rotation replaces the file). Best-effort randomness: /dev/urandom, with a
 * pid/address/counter fallback that only needs to avoid collision between
 * successive segments at one path on one host, not cryptographic strength.
 */
static uint64_t apc_sma_generate_seg_id(void) {
	static uint64_t counter = 0;
	uint64_t id = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd != -1) {
		ssize_t n = read(fd, &id, sizeof(id));
		close(fd);
		if (n == (ssize_t)sizeof(id) && id != 0) {
			return id;
		}
	}

	id = ((uint64_t)getpid() << 32) ^ (uintptr_t)&counter ^ (++counter);
	return id ? id : 1;
}

/* macros for getting the next or previous sequential block */
#define NEXT_SBLOCK(block) ((block_t*)((char*)block + block->size))
#define PREV_SBLOCK(block) ((block_t*)((char*)block - block->prev_size))

/* Canary macros for setting, checking and resetting memory canaries */
#ifdef APC_SMA_CANARIES
	#define SET_CANARY(v) (v)->canary = 0x42424242
	#define CHECK_CANARY(v) assert((v)->canary == 0x42424242)
	#define RESET_CANARY(v) (v)->canary = -42
#else
	#define SET_CANARY(v)
	#define CHECK_CANARY(v)
	#define RESET_CANARY(v)
#endif

#define MINBLOCKSIZE (ALIGNWORD(1) + ALIGNWORD(sizeof(block_t)))

/* How many extra blocks to check for a better fit */
#define BEST_FIT_LIMIT 3

static inline void link_free_block_at_start(sma_header_t *smaheader, block_t *cur) {
	block_t *dst = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)));

	/* insert cur as first block in the free list */
	cur->fnext = dst->fnext;
	cur->fprev = OFFSET(dst);
	dst->fnext = OFFSET(cur);
	BLOCKAT(cur->fnext)->fprev = dst->fnext;
}

static inline void unlink_free_block(sma_header_t *smaheader, block_t *cur) {
	BLOCKAT(cur->fprev)->fnext = cur->fnext;
	BLOCKAT(cur->fnext)->fprev = cur->fprev;
}

static inline block_t *find_block(sma_header_t *smaheader, size_t realsize) {
	block_t *cur = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)));
	block_t *found = NULL;
	uint32_t i;
	CHECK_CANARY(cur);

	/* First, ensure that at least realsize free bytes are available, even if they are not contiguous. */
	if (smaheader->avail < realsize) {
		return NULL;
	}

	while (cur->fnext) {
		cur = BLOCKAT(cur->fnext);
		CHECK_CANARY(cur);

		/* Found a suitable block */
		if (cur->size >= realsize) {
			found = cur;
			break;
		}
	}

	if (found) {
		/* Try to find a smaller block that also fits */
		for (i = 0; i < BEST_FIT_LIMIT && cur->fnext; i++) {
			cur = BLOCKAT(cur->fnext);
			CHECK_CANARY(cur);

			if (cur->size >= realsize && cur->size < found->size) {
				found = cur;
			}
		}
	}

	return found;
}

/* sma_allocate: tries to allocate at least size bytes of shared memory */
static APC_HOTSPOT size_t sma_allocate(sma_header_t *smaheader, size_t size)
{
	block_t* cur;           /* working block in list */
	size_t realsize;        /* actual size of block needed, including block header */

	realsize = ALIGNWORD(size + ALIGNWORD(sizeof(block_t)));

	cur = find_block(smaheader, realsize);
	if (!cur) {
		/* No suitable block found */
		return SIZE_MAX;
	}

	/* remove cur from the list of free blocks */
	unlink_free_block(smaheader, cur);

	if (cur->size >= realsize && cur->size < (realsize + smaheader->min_block_size)) {
		/* cur is big enough for realsize, but too small to split */
		NEXT_SBLOCK(cur)->prev_size = 0;  /* block is alloc'd */
	} else {
		/* cur is too big; split it into two smaller blocks */
		block_t* nxt;      /* the new block (chopped part of cur) */
		size_t oldsize;    /* size of cur before split */

		oldsize = cur->size;
		cur->size = realsize;
		nxt = NEXT_SBLOCK(cur);
		nxt->prev_size = 0;                       /* block is alloc'd */
		nxt->size = oldsize - realsize;           /* and fix the size */
		NEXT_SBLOCK(nxt)->prev_size = nxt->size;  /* adjust size */
		SET_CANARY(nxt);

		/* put the remaining block (nxt) back into the free list */
		link_free_block_at_start(smaheader, nxt);
	}

	/* mark cur as allocated */
	cur->fnext = 0;

	/* store used space to be able to reclaim unused space during defragmentation */
	cur->fprev = realsize;

	/* update the segment header */
	smaheader->avail -= cur->size;

	SET_CANARY(cur);

	return OFFSET(cur) + ALIGNWORD(sizeof(block_t));
}

/* sma_deallocate: deallocates the block at the given offset */
static APC_HOTSPOT size_t sma_deallocate(sma_header_t *smaheader, size_t offset)
{
	block_t* cur;       /* the new block to insert */
	block_t* prv;       /* the block before cur */
	block_t* nxt;       /* the block after cur */
	size_t size;        /* size of deallocated block */

	assert(offset >= ALIGNWORD(sizeof(block_t)));
	offset -= ALIGNWORD(sizeof(block_t));

	/* find position of new block in free list */
	cur = BLOCKAT(offset);

	/* update the segment header */
	smaheader->avail += cur->size;
	size = cur->size;

	if (cur->prev_size != 0) {
		/* remove prv from the list of free blocks */
		prv = PREV_SBLOCK(cur);
		unlink_free_block(smaheader, prv);

		/* cur and prv share an edge, combine them */
		prv->size += cur->size;

		RESET_CANARY(cur);
		cur = prv;
	}

	nxt = NEXT_SBLOCK(cur);
	if (nxt->fnext != 0) {
		assert(NEXT_SBLOCK(NEXT_SBLOCK(cur))->prev_size == nxt->size);
		/* remove nxt from the list of free blocks */
		unlink_free_block(smaheader, nxt);

		/* cur and nxt shared an edge, combine them */
		cur->size += nxt->size;

		CHECK_CANARY(nxt);
		RESET_CANARY(nxt);
	}

	/* mark in the sequentially next block that the previous block is free */
	NEXT_SBLOCK(cur)->prev_size = cur->size;

	/* insert cur into the free list */
	link_free_block_at_start(smaheader, cur);

	return size;
}

/*
 * Validates that shmaddr holds a fully initialized shared segment of the
 * given size that is layout-compatible with this process.
 */
PHP_APCU_API zend_bool apc_sma_shared_validate(void *shmaddr, size_t size) {
	apc_shared_seg_info_t *info = &((sma_header_t *)shmaddr)->shared_info;

	if (info->magic != APC_SHARED_SEG_MAGIC
			|| info->php_version_id != (uint32_t)PHP_VERSION_ID
			|| info->layout_check != apc_sma_layout_check()
			|| info->segsize != size) {
		return 0;
	}

	/* cache_off, cache_span and nslots are read straight from the segment and
	 * then used for pointer arithmetic and slot iteration in every attaching
	 * process (apc_sma_shared_get_cache_info -> apc_cache_shared_adopt). A
	 * crafted or corrupt segment that passed the magic check must not be able
	 * to point the cache header or slots array outside the mapping. Require
	 * the whole cache header + slots array to fit within the segment, and the
	 * span to be consistent with the slot count. */
	if (info->cache_off < ALIGNWORD(sizeof(sma_header_t)) || info->cache_off >= size) {
		return 0;
	}
	if (info->nslots == 0 || info->nslots > size / sizeof(uintptr_t)) {
		return 0;
	}
	/* The slots array that apc_cache_shared_adopt iterates starts at
	 * cache_off + sizeof(apc_cache_header_t) and runs nslots*sizeof(uintptr_t)
	 * bytes, so cache_span must cover the header AND the slots, and its end
	 * must fall inside the segment. Requiring cache_span to include the header
	 * (not just the slots) is what stops a crafted header from placing the
	 * slots' end past the mapping. */
	if (info->cache_span < sizeof(apc_cache_header_t) + info->nslots * sizeof(uintptr_t)
			|| info->cache_span > size
			|| info->cache_off + info->cache_span > size) {
		return 0;
	}

	return 1;
}

/*
 * Initializes all shm-resident SMA structures (lock, header, block lists)
 * on sma->shmaddr and computes the process-local allocation limit. In shared
 * mode the compatibility info is recorded, but the segment is not marked
 * ready until apc_sma_shared_mark_ready().
 */
PHP_APCU_API void apc_sma_init_segment(apc_sma_t *sma, size_t min_alloc_size) {
	sma_header_t *smaheader = sma->shmaddr;
	SMA_CREATE_LOCK(&smaheader->sma_lock);
	smaheader->min_block_size = min_alloc_size > 0 ? ALIGNWORD(min_alloc_size + ALIGNWORD(sizeof(block_t))) : MINBLOCKSIZE;
	smaheader->avail = sma->size - ALIGNWORD(sizeof(sma_header_t)) - ALIGNWORD(sizeof(block_t)) - ALIGNWORD(sizeof(block_t));
	sma->max_alloc_size = smaheader->avail - ALIGNWORD(sizeof(block_t));

	block_t *first = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)));
	first->size = 0;
	first->fnext = ALIGNWORD(sizeof(sma_header_t)) + ALIGNWORD(sizeof(block_t));
	first->fprev = 0;
	first->prev_size = 0;
	SET_CANARY(first);

	block_t *empty = BLOCKAT(first->fnext);
	empty->size = smaheader->avail;
	empty->fnext = OFFSET(empty) + empty->size;
	empty->fprev = ALIGNWORD(sizeof(sma_header_t));
	empty->prev_size = 0;
	SET_CANARY(empty);

	block_t *last = BLOCKAT(empty->fnext);
	last->size = 0;
	last->fnext = 0;
	last->fprev =  OFFSET(empty);
	last->prev_size = empty->size;
	SET_CANARY(last);

	if (sma->shared_mode) {
		/* Record compatibility data for attaching processes. The magic is
		 * deliberately not set here: apc_sma_shared_mark_ready() stamps it
		 * once all shm-resident structures (including the user cache) exist. */
		apc_shared_seg_info_t *info = &smaheader->shared_info;
		memset(info, 0, sizeof(*info));
		info->php_version_id = (uint32_t)PHP_VERSION_ID;
		info->layout_check = apc_sma_layout_check();
		info->segsize = sma->size;
		info->seg_id = apc_sma_generate_seg_id();
		sma->shared_seg_id = info->seg_id;
	}
}

PHP_APCU_API void apc_sma_init(apc_sma_t* sma, void** data, apc_sma_expunge_f expunge, size_t size, size_t min_alloc_size, char *mask, zend_long hugepage_size, char *shared_file) {
	if (sma->initialized) {
		return;
	}

	sma->initialized = 1;
	sma->expunge = expunge;
	sma->data = data;
	sma->size = ALIGNWORD(size > 0 ? size : SMA_DEFAULT_SEGSIZE);
	sma->shared_mode = 0;
	sma->shared_attached = 0;

#ifdef APC_MMAP
	if (shared_file && *shared_file) {
		zend_bool existed = 0;

		if (hugepage_size) {
			zend_error_noreturn(E_CORE_ERROR, "apc.mmap_hugepage_size cannot be combined with apc.mmap_shared_file");
		}
#ifdef ZTS
		/* Under ZTS one mapping is shared by all threads; a rotation cannot
		 * swap it safely without quiescing them, and apc_cache_header_t
		 * differs from NTS, so cross-flavor attach would corrupt. */
		zend_error_noreturn(E_CORE_ERROR, "apc.mmap_shared_file is not supported on ZTS (thread-safe) builds");
#endif
#ifdef APC_LOCK_FILE
		/* The fcntl lock backend stores a process-local file descriptor in
		 * shared memory, so an attaching process would run with effectively no
		 * cross-process locking. Refuse rather than corrupt silently. */
		zend_error_noreturn(E_CORE_ERROR, "apc.mmap_shared_file requires a process-shared lock backend, but this APCu build uses fcntl file locks which cannot coordinate unrelated processes; rebuild with pthread rwlocks/mutexes or spinlocks");
#endif

		sma->shared_mode = 1;
		{
			/* An existing segment is mapped and adopted at its own size (a
			 * rotation may have resized it); apc.shm_size only applies when
			 * this process creates the segment. */
			char err[256] = "";
			size_t mapped_size = sma->size;
			sma->shmaddr = apc_mmap_shared(shared_file, &mapped_size, &existed, err, sizeof(err));

			if (sma->shmaddr && existed) {
				sma_header_t *smaheader = sma->shmaddr;
				apc_shared_seg_info_t *info = &smaheader->shared_info;

				if (info->magic == APC_SHARED_SEG_MAGIC) {
					/* Fully initialized segment from another process: validate
					 * compatibility, then attach without touching shm state. */
					if (apc_sma_shared_validate(sma->shmaddr, mapped_size)) {
						sma->size = mapped_size;
						/* Recompute max_alloc_size exactly as the creator did
						 * from the initial avail; header->avail changed since. */
						sma->max_alloc_size = sma->size
							- ALIGNWORD(sizeof(sma_header_t))
							- 3 * ALIGNWORD(sizeof(block_t));
						sma->shared_attached = 1;
						sma->shared_seg_id = info->seg_id;
						return;
					}
					snprintf(err, sizeof(err),
						"existing segment in %s is incompatible (created by PHP version id %u, segment size %zu); remove the file or align the PHP version/build across processes",
						shared_file, info->php_version_id, info->segsize);
					apc_mmap_shared_release_lock();
					apc_unmap(sma->shmaddr, mapped_size);
					sma->shmaddr = NULL;
				} else if (mapped_size != sma->size) {
					/* A file this process didn't size, without a valid segment
					 * in it: refuse to clobber what we cannot identify. */
					snprintf(err, sizeof(err),
						"%s has size %zu, contains no valid segment, and does not match apc.shm_size (%zu); remove the file manually",
						shared_file, mapped_size, sma->size);
					apc_mmap_shared_release_lock();
					apc_unmap(sma->shmaddr, mapped_size);
					sma->shmaddr = NULL;
				}
				/* else: sized file without the ready-magic — a previous
				 * initialization crashed midway (the flock we hold proves no
				 * live process is initializing it); fall through and
				 * re-initialize as the creator. */
			}

			if (!sma->shmaddr) {
				/* Operational failure (permission, incompatible/foreign file,
				 * out of space, ...). By default this is fatal; with
				 * apc.mmap_shared_file_fallback the process degrades to a
				 * private, unshared segment with a warning instead of failing
				 * to start. */
				if (!APCG(mmap_shared_file_fallback)) {
					zend_error_noreturn(E_CORE_ERROR, "apc.mmap_shared_file: %s", err);
				}
				apc_warning("apc.mmap_shared_file: %s; falling back to a private (unshared) segment", err);
				sma->shared_mode = 0;
				sma->shared_attached = 0;
				sma->shmaddr = apc_mmap(mask, sma->size, hugepage_size);
			}
		}
	} else {
		sma->shmaddr = apc_mmap(mask, sma->size, hugepage_size);
	}
#else
	sma->shmaddr = apc_shm_attach(sma->size);
#endif

	apc_sma_init_segment(sma, min_alloc_size);
}

PHP_APCU_API zend_bool apc_sma_shared_attached(const apc_sma_t *sma) {
	return sma->shared_attached;
}

PHP_APCU_API void apc_sma_shared_set_cache_info(
		apc_sma_t *sma, void *cache_header, size_t cache_span, size_t nslots, const char *serializer_name) {
	sma_header_t *smaheader = sma->shmaddr;
	apc_shared_seg_info_t *info = &smaheader->shared_info;

	ZEND_ASSERT(sma->shared_mode && !sma->shared_attached);

	info->cache_off = (uintptr_t)((char *)cache_header - (char *)sma->shmaddr);
	info->cache_span = cache_span;
	info->nslots = nslots;
	strncpy(info->serializer, serializer_name ? serializer_name : "php",
		APC_SHARED_SEG_SERIALIZER_LEN - 1);
}

PHP_APCU_API void *apc_sma_shared_get_cache_info(
		apc_sma_t *sma, size_t *nslots, char *serializer_name) {
	sma_header_t *smaheader = sma->shmaddr;
	apc_shared_seg_info_t *info = &smaheader->shared_info;

	ZEND_ASSERT(sma->shared_attached);

	*nslots = info->nslots;
	memcpy(serializer_name, info->serializer, APC_SHARED_SEG_SERIALIZER_LEN);
	serializer_name[APC_SHARED_SEG_SERIALIZER_LEN - 1] = '\0';
	return (char *)sma->shmaddr + info->cache_off;
}

PHP_APCU_API void apc_sma_shared_mark_ready(apc_sma_t *sma) {
	sma_header_t *smaheader = sma->shmaddr;

	if (!sma->shared_mode || sma->shared_attached) {
		return;
	}
	smaheader->shared_info.magic = APC_SHARED_SEG_MAGIC;
}

PHP_APCU_API zend_bool apc_sma_shared_is_retired(const apc_sma_t *sma) {
	if (!sma->shared_mode) {
		return 0;
	}
	return ((sma_header_t *)sma->shmaddr)->shared_info.retired != 0;
}

PHP_APCU_API zend_bool apc_sma_shared_addr_retired(void *shmaddr) {
	return ((sma_header_t *)shmaddr)->shared_info.retired != 0;
}

PHP_APCU_API uint64_t apc_sma_shared_addr_seg_id(void *shmaddr) {
	return ((sma_header_t *)shmaddr)->shared_info.seg_id;
}

PHP_APCU_API uint64_t apc_sma_shared_current_seg_id(const apc_sma_t *sma) {
	return sma->shared_seg_id;
}

PHP_APCU_API void apc_sma_shared_retire(apc_sma_t *sma) {
	/* Guarded by the rotating process's flock on the segment file, not by
	 * the cache lock: retirement must work even when the cache lock is
	 * wedged by a crashed process. Attached processes poll this flag at
	 * request start and re-attach to the successor segment. */
	((sma_header_t *)sma->shmaddr)->shared_info.retired = 1;
}

PHP_APCU_API void apc_sma_shared_swap(apc_sma_t *sma, void *new_addr, size_t new_size) {
	sma->shmaddr = new_addr;
	sma->size = new_size;
	sma->max_alloc_size = new_size
		- ALIGNWORD(sizeof(sma_header_t))
		- 3 * ALIGNWORD(sizeof(block_t));
	sma->shared_attached = 1;
	sma->shared_seg_id = ((sma_header_t *)new_addr)->shared_info.seg_id;
}

PHP_APCU_API void apc_sma_detach(apc_sma_t* sma) {
	/* Important: This function should not clean up anything that's in shared memory,
	 * only detach our process-local use of it. In particular locks cannot be destroyed
	 * here. */

	assert(sma->initialized);
	sma->initialized = 0;

#ifdef APC_MMAP
	apc_unmap(sma->shmaddr, sma->size);
#else
	apc_shm_detach(sma->shmaddr);
#endif
}

PHP_APCU_API void* apc_sma_malloc(apc_sma_t* sma, size_t n, apc_sma_malloc_init_f init_callback) {
	size_t off;
	zend_bool nuked = 0;

restart:
	assert(sma->initialized);

	/* Prevent cache wipes caused by huge allocations that don't fit into shm */
	if (n > sma->max_alloc_size) {
		return NULL;
	}

	if (!SMA_LOCK(sma)) {
		return NULL;
	}

	off = sma_allocate(SMA_HDR(sma), n);

	if (off != SIZE_MAX) {
		void *p = (void *)(SMA_ADDR(sma) + off);

		if (init_callback) {
			/* Perform initializations that must be done before releasing the lock */
			init_callback(p);
		}

		SMA_UNLOCK(sma);
#ifdef VALGRIND_MALLOCLIKE_BLOCK
		VALGRIND_MALLOCLIKE_BLOCK(p, n, 0, 0);
#endif
		return p;
	}

	SMA_UNLOCK(sma);

	/* Expunge cache in hope of freeing up memory, but only once */
	if (!nuked) {
		/* nuke is not set if expunge() was skipped internally to get another try */
		nuked = sma->expunge(*sma->data, n);
		goto restart;
	}

	return NULL;
}

PHP_APCU_API void apc_sma_free(apc_sma_t* sma, void* p) {
	size_t offset;

	if (p == NULL) {
		return;
	}

	assert(sma->initialized);

	offset = (size_t)((char *)p - SMA_ADDR(sma));
	if (p < (void *)SMA_ADDR(sma) || offset >= sma->size) {
		apc_error("apc_sma_free: could not locate address %p", p);
		return;
	}

	if (!SMA_LOCK(sma)) {
		return;
	}

	sma_deallocate(SMA_HDR(sma), offset);
	SMA_UNLOCK(sma);
#ifdef VALGRIND_FREELIKE_BLOCK
	VALGRIND_FREELIKE_BLOCK(p, 0);
#endif
}

PHP_APCU_API apc_sma_info_t *apc_sma_info(apc_sma_t* sma, zend_bool limited) {

	if (!sma->initialized) {
		return NULL;
	}

	apc_sma_info_t *info = emalloc(sizeof(apc_sma_info_t));
	info->seg_size = sma->size - (ALIGNWORD(sizeof(sma_header_t)) + ALIGNWORD(sizeof(block_t)) + ALIGNWORD(sizeof(block_t)));
	info->list = NULL;

	if (limited) {
		return info;
	}

	if (!SMA_LOCK(sma)) {
		efree(info);
		return NULL;
	}

	sma_header_t *smaheader = SMA_HDR(sma);
	block_t *cur = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)));
	apc_sma_link_t **link = &info->list;

	/* Skip 1st (0-sized) block */
	cur = BLOCKAT(cur->fnext);

	/* For each free block */
	while (cur->fnext != 0) {
		CHECK_CANARY(cur);

		*link = emalloc(sizeof(apc_sma_link_t));
		(*link)->size = cur->size;
		(*link)->offset = OFFSET(cur);
		(*link)->next = NULL;
		link = &(*link)->next;

		cur = BLOCKAT(cur->fnext);
	}
	SMA_UNLOCK(sma);

	return info;
}

PHP_APCU_API void apc_sma_free_info(apc_sma_t *sma, apc_sma_info_t *info) {
	apc_sma_link_t *p = info->list;

	while (p) {
		apc_sma_link_t *q = p;
		p = p->next;
		efree(q);
	}

	efree(info);
}

PHP_APCU_API size_t apc_sma_get_avail_mem(apc_sma_t* sma) {
	return SMA_HDR(sma)->avail;
}

PHP_APCU_API zend_bool apc_sma_check_avail(apc_sma_t *sma, size_t size) {
	return SMA_HDR(sma)->avail >= ALIGNWORD(size + ALIGNWORD(sizeof(block_t)));
}

PHP_APCU_API zend_bool apc_sma_check_avail_contiguous(apc_sma_t *sma, size_t size) {
	size_t realsize = ALIGNWORD(size + ALIGNWORD(sizeof(block_t)));
	sma_header_t *smaheader = SMA_HDR(sma);

	/* If total size of available memory is too small, we can skip the contiguous-block check */
	if (smaheader->avail < realsize) {
		return 0;
	}

	if (!SMA_LOCK(sma)) {
		return 0;
	}

	block_t *cur = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)));

	/* Look for a contiguous block of memory */
	while (cur->fnext) {
		cur = BLOCKAT(cur->fnext);

		if (cur->size >= realsize) {
			SMA_UNLOCK(sma);
			return 1;
		}
	}

	SMA_UNLOCK(sma);

	return 0;
}

PHP_APCU_API void apc_sma_defrag(apc_sma_t *sma, void *data, apc_sma_move_f move) {
	sma_header_t *smaheader = SMA_HDR(sma);
	block_t *cur = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)) + ALIGNWORD(sizeof(block_t)));
	block_t *first = BLOCKAT(ALIGNWORD(sizeof(sma_header_t)));
	size_t reclaimed_size = 0;

	if (!SMA_LOCK(sma)) {
		return;
	}

	/* empty the free list */
	first->fnext = sma->size - ALIGNWORD(sizeof(block_t));
	BLOCKAT(first->fnext)->fprev = OFFSET(first);

	/* loop through all blocks */
	while (cur->size != 0) {
		/* continue until cur points to a free block */
		if (!cur->fnext) {
			cur = NEXT_SBLOCK(cur);
			continue;
		}

		/* if cur is free, nxt must be an allocated block, since we never have two consecutive free blocks */
		block_t *nxt = NEXT_SBLOCK(cur);

		/* if nxt is the last block, or if nxt can't be moved, cur can't be combined with other free blocks */
		if (nxt->size == 0 || !move(data, (char *)nxt + ALIGNWORD(sizeof(block_t)), (char *)cur + ALIGNWORD(sizeof(block_t)))) {
			/* insert cur into the free list */
			link_free_block_at_start(smaheader, cur);

			cur->prev_size = 0;
			nxt->prev_size = cur->size;

			cur = NEXT_SBLOCK(nxt);
			continue;
		}

		/* reclaim unused space from the allocated block (nxt->fprev contains the used space) */
		size_t free_size = nxt->size - nxt->fprev;
		reclaimed_size += free_size;
		nxt->size -= free_size;
		free_size += cur->size;

		/* swap cur and nxt by moving nxt (incl. header) and initializing a new block header for cur behind it */
		memmove(cur, nxt, nxt->size);
		cur->prev_size = 0;
		cur = NEXT_SBLOCK(cur);
		cur->size = free_size;
		cur->fnext = 1; /* mark cur as free */

		/* if the next block is also free, combine cur and nxt to one larger free block */
		nxt = NEXT_SBLOCK(cur);
		if (nxt->fnext) {
			cur->size += nxt->size;
		}
	}

	smaheader->avail += reclaimed_size;

	SMA_UNLOCK(sma);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: noexpandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: noexpandtab sw=4 ts=4 sts=4
 */
