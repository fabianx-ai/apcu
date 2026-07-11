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
  | Authors: Gopal V <gopalv@php.net>                                    |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

#ifndef APC_MMAP_H
#define APC_MMAP_H

#include <limits.h>

#include "apc.h"

/* Wrapper functions for shared memory mapped files */

#ifdef APC_MMAP
void *apc_mmap(char *file_mask, size_t size, zend_long hugepage_size);
void apc_unmap(void *shmaddr, size_t size);

/*
 * Maps a shared segment backed by a regular file at a fixed path, so that
 * independent PHP processes (e.g. CLI and FPM) can map the same segment.
 * The file is created and sized on first use; subsequent processes attach.
 * On return the process holds an exclusive flock on the file; the caller
 * must call apc_mmap_shared_release_lock() once segment initialization (or
 * attach validation) is complete. *size is the desired size when creating;
 * on return it holds the actual mapped size (an existing segment is mapped
 * at its own size, which a rotation may have changed). *existed is set to 1
 * when a non-empty file was already present (attach candidate), 0 when this
 * process created the file and must initialize the segment.
 */
void *apc_mmap_shared(char *file_path, size_t *size, zend_bool *existed, char *errbuf, size_t errlen);
/* Reserve backing storage for the currently-locked shared segment (used on the
 * crashed-init recovery path, where the file exists but may be sparse). Returns
 * 0 on success or when unsupported, else an errno-like value. */
int apc_mmap_shared_reserve_current(size_t size);
void apc_mmap_shared_release_lock(void);

/* Rotation support: map the current segment file as-is / build a successor. */
void *apc_mmap_file_open_existing(char *file_path, size_t *size_out);
void *apc_mmap_file_create(char *file_path, size_t size);
int apc_mmap_shared_lock_path(char *file_path);
#endif

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: noexpandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: noexpandtab sw=4 ts=4 sts=4
 */
