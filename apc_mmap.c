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
  | Authors: Rasmus Lerdorf <rasmus@php.net>                             |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

#include "apc.h"
#include "apc_mmap.h"
#include "apc_lock.h"

#ifdef APC_MMAP

#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Some operating systems (like FreeBSD) have a MAP_NOSYNC flag that
 * tells whatever update daemons might be running to not flush dirty
 * vm pages to disk unless absolutely necessary.  My guess is that
 * most systems that don't have this probably default to only synching
 * to disk when absolutely necessary.
 */
#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0
#endif

/* support for systems where MAP_ANONYMOUS is defined but not MAP_ANON, ie: HP-UX bug #14615 */
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
# define MAP_ANON MAP_ANONYMOUS
#endif

static int apc_mmap_hugepage_flags(size_t size, zend_long hugepage_size)
{
	if (!hugepage_size) return 0; // not use hugepages

#if defined(MAP_HUGETLB) && defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT)
	if (size % hugepage_size) {
		zend_error_noreturn(E_CORE_ERROR, "apc.shm_size must be a multiple of apc.mmap_hugepage_size");
	}

	zend_long page_size = hugepage_size;
	int log2_page_size = -1;

	// calculate log2 of hugepage size
	while (page_size) {
		page_size >>= 1;
		log2_page_size++;
	}

	if (!log2_page_size || (log2_page_size & MAP_HUGE_MASK) != log2_page_size) {
		// maybe hugepage size is too large or small
		zend_error_noreturn(E_CORE_ERROR, "Invalid hugepage size: " ZEND_LONG_FMT, hugepage_size);
	}

	return MAP_HUGETLB | ((unsigned int)log2_page_size << MAP_HUGE_SHIFT);
#else
	zend_error_noreturn(E_CORE_ERROR, "This system does not support hugepages");
#endif
}

void *apc_mmap(char *file_mask, size_t size, zend_long hugepage_size)
{
	void *shmaddr;
	int fd = -1;
	int flags = MAP_SHARED | MAP_NOSYNC;

	/* If no filename was provided, do an anonymous mmap */
	if (!file_mask || (file_mask && !strlen(file_mask))) {
#if !defined(MAP_ANON)
		zend_error_noreturn(E_CORE_ERROR, "Anonymous mmap does not appear to be available on this system (MAP_ANON/MAP_ANONYMOUS).  Please see the apc.mmap_file_mask INI option.");
#else
		fd = -1;
		flags = MAP_SHARED | MAP_ANON;
#endif
	} else if (!strcmp(file_mask,"/dev/zero")) {
		fd = open("/dev/zero", O_RDWR, S_IRUSR | S_IWUSR);
		if (fd == -1) {
			zend_error_noreturn(E_CORE_ERROR, "apc_mmap: open on /dev/zero failed");
		}
	} else {
		/* Otherwise we do a normal filesystem mmap */
		fd = mkstemp(file_mask);
		if (fd == -1) {
			zend_error_noreturn(E_CORE_ERROR, "apc_mmap: mkstemp on %s failed", file_mask);
		}
		if (ftruncate(fd, size) < 0) {
			close(fd);
			unlink(file_mask);
			zend_error_noreturn(E_CORE_ERROR, "apc_mmap: ftruncate failed");
		}
		unlink(file_mask);
	}

	flags |= apc_mmap_hugepage_flags(size, hugepage_size);
	shmaddr = (void *)mmap(NULL, size, PROT_READ | PROT_WRITE, flags, fd, 0);

	if ((long)shmaddr == -1) {
		if (hugepage_size) {
			zend_error_noreturn(E_CORE_ERROR, "apc_mmap: Failed to mmap %zu bytes with hugepage size " ZEND_LONG_FMT ". apc.shm_size may be too large, apc.mmap_hugepage_size may be invalid, or the system lacks sufficient reserved hugepages.", size, hugepage_size);
		} else {
			zend_error_noreturn(E_CORE_ERROR, "apc_mmap: Failed to mmap %zu bytes. apc.shm_size may be too large.", size);
		}
	}

#ifdef MADV_HUGEPAGE
	/* enable transparent huge pages to reduce TLB misses (Linux only) */
	if (!hugepage_size) {
		madvise(shmaddr, size, MADV_HUGEPAGE);
	}
#endif

	if (fd != -1) close(fd);

	return shmaddr;
}

void apc_unmap(void *shmaddr, size_t size)
{
	if (munmap(shmaddr, size) < 0) {
		apc_warning("apc_unmap: munmap failed");
	}
}

/* fd backing the shared-file segment; kept open while the exclusive flock
 * taken during create/attach is held, closed (releasing the lock) once the
 * segment is fully initialized or validated. */
static int apc_mmap_shared_lock_fd = -1;

/* O_NOFOLLOW refuses to open a symlink at the final path component (the
 * classic "plant a symlink at a predictable path so root writes elsewhere"
 * attack); O_CLOEXEC keeps the segment/lock fd out of exec'd children. Not
 * every platform defines them, so degrade gracefully. */
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
# define O_CLOEXEC 0
#endif
#define APC_SHARED_OPEN_FLAGS (O_RDWR | O_NOFOLLOW | O_CLOEXEC)

/*
 * Predicate: is a stat'd shared-segment file plausibly trustworthy rather than
 * something an attacker planted at a predictable path? Requires a regular file,
 * owned by us or by root, with no extra hard links and not group/world
 * writable. This does not make a file a hostile user can still write safe —
 * that is impossible by construction, and the path must live in a directory
 * only trusted users can write — but it closes the pre-creation, symlink, and
 * crafted-content classes for the recommended 0600 same-owner deployment.
 */
/* Returns NULL if the file is acceptable, else a human-readable reason. */
static const char *apc_mmap_shared_fd_reason(const struct stat *st)
{
	if (!S_ISREG(st->st_mode)) {
		return "not a regular file";
	}
	if (st->st_uid != geteuid() && st->st_uid != 0) {
		return "owned by another non-root user (possible planted file)";
	}
	if (st->st_nlink != 1) {
		return "has extra hard links";
	}
	if (st->st_mode & (S_IWGRP | S_IWOTH)) {
		return "group/world writable (any writer could corrupt every attached process)";
	}
	return NULL;
}

static zend_bool apc_mmap_shared_fd_ok(const struct stat *st)
{
	return apc_mmap_shared_fd_reason(st) == NULL;
}

/*
 * Reserve backing storage so a later store cannot SIGBUS on out-of-space.
 * Best effort: filesystems that do not support fallocate keep the sparse
 * ftruncate behaviour; only a genuine ENOSPC is reported (returned errno).
 */
static int apc_mmap_reserve(int fd, size_t size)
{
	int rc = posix_fallocate(fd, 0, (off_t)size);
	if (rc == EOPNOTSUPP || rc == ENOSYS || rc == EINVAL) {
		return 0;
	}
	return rc;
}

void *apc_mmap_shared(char *file_path, size_t *size, zend_bool *existed, char *errbuf, size_t errlen)
{
	void *shmaddr;
	const char *reason;
	struct stat st, st_path;
	int fd = -1, rc, attempts;

	errbuf[0] = '\0';

	/* Defensive: APCu only ever sets up one shared segment, so a lingering
	 * lock fd here would be a bug; close it rather than leak the fd + flock. */
	if (apc_mmap_shared_lock_fd != -1) {
		close(apc_mmap_shared_lock_fd);
		apc_mmap_shared_lock_fd = -1;
	}

	/* A segment rotation can atomically rename() a successor over file_path
	 * between our open() and flock(); detect that by comparing the inode we
	 * locked with the inode currently at the path, and retry. */
	for (attempts = 0; attempts < 5; attempts++) {
		fd = open(file_path, APC_SHARED_OPEN_FLAGS | O_CREAT, S_IRUSR | S_IWUSR);
		if (fd == -1) {
			snprintf(errbuf, errlen, "open on %s failed: %s (a symlink at this path is refused)", file_path, strerror(errno));
			return NULL;
		}

		/* Serialize segment creation/attach across processes. The lock is
		 * held beyond this function so that a process attaching concurrently
		 * cannot observe a partially initialized segment. */
		do {
			rc = flock(fd, LOCK_EX);
		} while (rc == -1 && errno == EINTR);
		if (rc == -1) {
			snprintf(errbuf, errlen, "flock on %s failed: %s", file_path, strerror(errno));
			close(fd);
			return NULL;
		}

		if (fstat(fd, &st) != 0) {
			snprintf(errbuf, errlen, "fstat on %s failed: %s", file_path, strerror(errno));
			close(fd);
			return NULL;
		}

		if (stat(file_path, &st_path) == 0 && st_path.st_ino == st.st_ino) {
			break;
		}

		/* The file we locked is no longer the one at the path (rotated or
		 * removed); start over with the current file. */
		close(fd);
		fd = -1;
	}
	if (fd == -1) {
		snprintf(errbuf, errlen, "%s keeps changing while attaching (concurrent rotations?)", file_path);
		return NULL;
	}

	reason = apc_mmap_shared_fd_reason(&st);
	if (reason) {
		snprintf(errbuf, errlen, "%s is %s; refusing to use it as a shared segment", file_path, reason);
		close(fd);
		return NULL;
	}

	if (st.st_size == 0) {
		/* Fresh file: this process becomes the creator at the desired size.
		 * On failure, unlink what we created (the flock + inode check above
		 * prove the path still names our file): a leftover sized-but-sparse
		 * file would poison later attachers, which cannot even read its
		 * header on a full filesystem without SIGBUSing on page allocation.
		 * A process blocked in flock() on the unlinked inode re-runs the
		 * inode-vs-path check, retries, and becomes a fresh creator. */
		*existed = 0;
		if (ftruncate(fd, *size) < 0) {
			snprintf(errbuf, errlen, "ftruncate on %s failed: %s", file_path, strerror(errno));
			unlink(file_path);
			close(fd);
			return NULL;
		}
		rc = apc_mmap_reserve(fd, *size);
		if (rc != 0) {
			snprintf(errbuf, errlen, "cannot reserve %zu bytes for %s: %s", *size, file_path, strerror(rc));
			unlink(file_path);
			close(fd);
			return NULL;
		}
	} else {
		/* Existing segment: map it at its own size. A rotation may have
		 * resized it, so apc.shm_size only applies when creating; the caller
		 * validates the segment and adopts its size. */
		*existed = 1;
		*size = (size_t)st.st_size;
	}

	shmaddr = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOSYNC, fd, 0);
	if (shmaddr == MAP_FAILED) {
		snprintf(errbuf, errlen, "failed to mmap %zu bytes of %s (apc.shm_size may be too large)", *size, file_path);
		if (!*existed) {
			unlink(file_path);
		}
		close(fd);
		return NULL;
	}

	apc_mmap_shared_lock_fd = fd;
	return shmaddr;
}

int apc_mmap_shared_reserve_current(size_t size)
{
	if (apc_mmap_shared_lock_fd == -1) {
		return 0;
	}
	return apc_mmap_reserve(apc_mmap_shared_lock_fd, size);
}

/*
 * Reads from the shared segment's locked fd without touching the mapping.
 * pread() returns zeros for holes in a sparse file without allocating
 * backing pages, so unlike a read through the mapping it cannot SIGBUS on a
 * crashed-init segment sitting on a full filesystem. Returns 0 or an errno;
 * a file shorter than the requested range is reported as EIO.
 */
int apc_mmap_shared_pread(void *buf, size_t len, size_t offset)
{
	ssize_t n;

	if (apc_mmap_shared_lock_fd == -1) {
		return EBADF;
	}
	n = pread(apc_mmap_shared_lock_fd, buf, len, (off_t)offset);
	if (n < 0) {
		return errno;
	}
	if ((size_t)n != len) {
		return EIO;
	}
	return 0;
}

void apc_mmap_shared_release_lock(void)
{
	if (apc_mmap_shared_lock_fd != -1) {
		/* An flock is bound to the open file description, and the segment's
		 * MAP_SHARED mapping keeps that description referenced for the life of
		 * the process — so close() alone does NOT drop the lock (the lock would
		 * persist until munmap, i.e. forever, serializing every other process's
		 * MINIT attach and self-deadlocking rotation). Release it explicitly;
		 * the mapping stays valid. */
		flock(apc_mmap_shared_lock_fd, LOCK_UN);
		close(apc_mmap_shared_lock_fd);
		apc_mmap_shared_lock_fd = -1;
	}
}

/*
 * Takes the rotation/creation flock on the segment file currently at
 * file_path (without creating it), retrying if a concurrent rotation
 * rename()s a successor over the path between open and lock. Returns the
 * locked fd, or -1 on failure. The caller releases the lock via close().
 */
int apc_mmap_shared_lock_path(char *file_path)
{
	struct stat st, st_path;
	int fd, rc, attempts;

	for (attempts = 0; attempts < 5; attempts++) {
		fd = open(file_path, APC_SHARED_OPEN_FLAGS);
		if (fd == -1) {
			return -1;
		}
		do {
			rc = flock(fd, LOCK_EX);
		} while (rc == -1 && errno == EINTR);
		if (rc == -1) {
			close(fd);
			return -1;
		}
		if (fstat(fd, &st) == 0 && apc_mmap_shared_fd_ok(&st)
				&& stat(file_path, &st_path) == 0
				&& st.st_ino == st_path.st_ino) {
			return fd;
		}
		close(fd);
	}
	return -1;
}

/*
 * Maps an existing shared segment file at whatever size it currently has,
 * without creating or locking anything. Used to re-attach to a successor
 * segment after rotation (the file at the path is always fully initialized,
 * because rotation builds the successor under a private name and rename()s
 * it into place atomically). Returns NULL on failure.
 */
void *apc_mmap_file_open_existing(char *file_path, size_t *size_out)
{
	void *shmaddr;
	struct stat st;
	int fd;

	fd = open(file_path, APC_SHARED_OPEN_FLAGS);
	if (fd == -1) {
		return NULL;
	}
	if (fstat(fd, &st) != 0 || st.st_size == 0 || !apc_mmap_shared_fd_ok(&st)) {
		close(fd);
		return NULL;
	}

	shmaddr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOSYNC, fd, 0);
	close(fd);
	if (shmaddr == MAP_FAILED) {
		return NULL;
	}

	*size_out = st.st_size;
	return shmaddr;
}

/*
 * Creates and maps a fresh file of the given size at file_path (exclusive:
 * fails if the file exists). Used by rotation to build a successor segment
 * under a private name before rename()ing it over the shared file path.
 * Returns NULL on failure.
 */
void *apc_mmap_file_create(char *file_path, size_t size)
{
	void *shmaddr;
	int fd;

	fd = open(file_path, APC_SHARED_OPEN_FLAGS | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		return NULL;
	}
	if (ftruncate(fd, size) < 0 || apc_mmap_reserve(fd, size) != 0) {
		close(fd);
		unlink(file_path);
		return NULL;
	}

	shmaddr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOSYNC, fd, 0);
	close(fd);
	if (shmaddr == MAP_FAILED) {
		unlink(file_path);
		return NULL;
	}

	return shmaddr;
}

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: noexpandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: noexpandtab sw=4 ts=4 sts=4
 */
