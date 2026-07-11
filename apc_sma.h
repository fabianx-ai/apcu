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
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

#ifndef APC_SMA_H
#define APC_SMA_H

/*
 * SMA API
 * APC SMA API provides support for shared memory allocators to external libraries ( and to APC )
 * Skip to the bottom macros for error free usage of the SMA API
*/

#include "apc.h"

typedef struct apc_sma_link_t apc_sma_link_t;
struct apc_sma_link_t {
	zend_long size;         /* size of this free block */
	zend_long offset;       /* offset in segment of this block */
	apc_sma_link_t* next;   /* link to next free block */
};

typedef struct apc_sma_info_t apc_sma_info_t;
struct apc_sma_info_t {
	size_t seg_size;       /* segment size */
	apc_sma_link_t* list;  /* list of free blocks */
};

typedef zend_bool (*apc_sma_expunge_f)(void *pointer, size_t size);

/* maximum stored length (incl. NUL) of the serializer name recorded in a
 * file-backed shared segment for cross-process validation */
#define APC_SHARED_SEG_SERIALIZER_LEN 16

typedef struct _apc_sma_t {
	zend_bool initialized;         /* flag to indicate this sma has been initialized */

	/* callback */
	apc_sma_expunge_f expunge;     /* expunge */
	void** data;                   /* expunge data */

	/* info */
	size_t size;                   /* segment size */
	size_t max_alloc_size;         /* max size of memory available for allocation */
	void  *shmaddr;                /* address of shm segment */

	/* file-backed shared segment (apc.mmap_shared_file) state */
	zend_bool shared_mode;         /* segment is backed by a named shared file */
	zend_bool shared_attached;     /* attached to a segment initialized by another process */
	uint64_t shared_seg_id;        /* id of the segment instance this process currently maps */
} apc_sma_t;

/*
* apc_sma_init will initialize a shared memory allocator with the given size of shared memory
*
* should be called once per allocator per process
*
* When shared_file is non-NULL and non-empty, the segment is backed by that
* file so that unrelated processes (CLI and web SAPIs) can share it. The
* first process initializes the segment, later processes attach to it; the
* caller must invoke apc_sma_shared_mark_ready() + apc_mmap_shared_release_lock()
* once all shm-resident structures are set up.
*/
PHP_APCU_API void apc_sma_init(
		apc_sma_t* sma, void** data, apc_sma_expunge_f expunge,
		size_t size, size_t min_alloc_size, char *mask, zend_long hugepage_size,
		char *shared_file);

/*
 * Returns 1 when this process attached to a shared segment initialized by
 * another process (in which case shm-resident structures must not be
 * re-initialized).
 */
PHP_APCU_API zend_bool apc_sma_shared_attached(const apc_sma_t *sma);

/*
 * Records the location of the user cache header inside a shared segment,
 * along with the slot count and serializer name, so attaching processes can
 * adopt them. Only valid in shared mode, called by the creating process.
 */
PHP_APCU_API void apc_sma_shared_set_cache_info(
		apc_sma_t *sma, void *cache_header, size_t cache_span, size_t nslots, const char *serializer_name);

/*
 * Returns the user cache header of a shared segment this process attached
 * to, filling in the creator's slot count and serializer name.
 * serializer_name must have room for APC_SHARED_SEG_SERIALIZER_LEN bytes.
 */
PHP_APCU_API void *apc_sma_shared_get_cache_info(
		apc_sma_t *sma, size_t *nslots, char *serializer_name);

/*
 * Stamps the ready-magic into a shared segment. Must be the last step of
 * segment initialization, before apc_mmap_shared_release_lock(). No-op when
 * this process attached to an existing segment.
 */
PHP_APCU_API void apc_sma_shared_mark_ready(apc_sma_t *sma);

/*
 * Returns 1 when the currently mapped shared segment has been retired by a
 * rotation (a successor segment exists at the shared file path).
 */
PHP_APCU_API zend_bool apc_sma_shared_is_retired(const apc_sma_t *sma);

/* Same check for a raw mapping that is not (yet) tracked by an apc_sma_t. */
PHP_APCU_API zend_bool apc_sma_shared_addr_retired(void *shmaddr);

/* Unique id of the segment instance at a raw mapping / currently mapped by sma.
 * Used to detect that the file now at the shared path is a different segment
 * instance than this process mapped (a rotation replaced it). */
PHP_APCU_API uint64_t apc_sma_shared_addr_seg_id(void *shmaddr);
PHP_APCU_API uint64_t apc_sma_shared_current_seg_id(const apc_sma_t *sma);

/*
 * Marks the currently mapped segment as retired. Called by the rotating
 * process after the successor was atomically renamed over the shared file
 * path, under the rotation flock.
 */
PHP_APCU_API void apc_sma_shared_retire(apc_sma_t *sma);

/*
 * Validates that shmaddr holds a ready, layout-compatible shared segment of
 * the given size. Unlike attach-time validation this never raises.
 */
PHP_APCU_API zend_bool apc_sma_shared_validate(void *shmaddr, size_t size);

/*
 * Points sma at a different, already validated shared segment mapping
 * (successor after rotation) and recomputes process-local limits. The caller
 * unmaps the previous mapping afterwards.
 */
PHP_APCU_API void apc_sma_shared_swap(apc_sma_t *sma, void *new_addr, size_t new_size);

/*
 * Initializes all shm-resident SMA structures on sma->shmaddr (segment lock,
 * header, block lists). Used at MINIT for fresh segments and by rotation to
 * build a successor segment.
 */
PHP_APCU_API void apc_sma_init_segment(apc_sma_t *sma, size_t min_alloc_size);

/*
 * apc_sma_detach will detach from shared memory and cleanup local allocations.
 */
PHP_APCU_API void apc_sma_detach(apc_sma_t* sma);

/*
* apc_smap_api_malloc will allocate a block from the sma of the given size.
* The init_callack() can be used to perform initializations that must be completed
* before the lock of the sma layer is released.
*/
typedef void (*apc_sma_malloc_init_f)(void *pointer);
PHP_APCU_API void* apc_sma_malloc(apc_sma_t* sma, size_t size, apc_sma_malloc_init_f init_callback);

/*
* apc_sma_api_free will free p (which should be a pointer to a block allocated from sma)
*/
PHP_APCU_API void apc_sma_free(apc_sma_t* sma, void* p);

/*
* apc_sma_api_info returns information about the allocator
*/
PHP_APCU_API apc_sma_info_t* apc_sma_info(apc_sma_t* sma, zend_bool limited);

/*
* apc_sma_api_info_free_info is for freeing apc_sma_info_t* returned by apc_sma_api_info
*/
PHP_APCU_API void apc_sma_free_info(apc_sma_t* sma, apc_sma_info_t* info);

/*
* apc_sma_api_get_avail_mem will return the amount of memory available left to sma
*/
PHP_APCU_API size_t apc_sma_get_avail_mem(apc_sma_t* sma);

/*
* apc_sma_check_avail returns true if at least size bytes are available across all free blocks
*/
PHP_APCU_API zend_bool apc_sma_check_avail(apc_sma_t *sma, size_t size);

/*
* apc_sma_check_avail_contiguous returns true if at least size contiguous bytes can be allocated from the sma
*/
PHP_APCU_API zend_bool apc_sma_check_avail_contiguous(apc_sma_t *sma, size_t size);

/*
* apc_sma_defrag defragments the shared memory by shifting all allocated blocks to the left,
* allowing all free blocks to be coalesced to one larger free block on the right side.
*
* The move() callback is called for each allocated block before it is moved. Therefore, move() can be used
* to prepare for the move or to prevent the block from being moved by returning 0. The argument "data" is
* passed as the first argument from apc_sma_defrag() to move(), while the old and the new address of the
* allocation is passed as the 2nd and 3rd argument. The callback must not write to the new memory area
* because the area is not yet allocated during the callback.
*/
typedef zend_bool (*apc_sma_move_f)(void *data, void *pointer_old, void *pointer_new);
PHP_APCU_API void apc_sma_defrag(apc_sma_t *sma, void *data, apc_sma_move_f move);

/* ALIGNWORD: pad up x, aligned to the system's word boundary */
#define ALIGNWORD(x) ZEND_MM_ALIGNED_SIZE(x)

#endif
