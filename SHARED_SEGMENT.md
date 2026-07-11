# Sharing one APCu segment across processes (`apc.mmap_shared_file`)

By default each PHP master process gets its own anonymous APCu segment, so a
CLI process (e.g. a cron job or `drush`) and the web tier (FPM/Apache) never
see each other's user cache. `apc.mmap_shared_file` backs the segment with a
fixed-path file that unrelated processes map, so they share one cache.

## INI settings

| Setting | Default | Meaning |
|---|---|---|
| `apc.mmap_shared_file` | *(unset)* | Path of the file that backs the shared segment. When set, unrelated processes that map the same path share one APCu cache. Mutually exclusive with `apc.mmap_hugepage_size`; overrides `apc.mmap_file_mask`. |
| `apc.mmap_shared_file_fallback` | `0` | When `1`, an operational failure setting up the shared segment (permission, incompatible/foreign file, out of space) logs a warning and the process falls back to a private, unshared segment instead of failing to start. When `0` (default) such a failure is fatal, so a misconfiguration is not masked. |
| `apc.shm_size` | `32M` | Segment size **at creation only**. Processes attaching to an existing segment adopt its current size (a rotation may have changed it). |

The first process to open the path creates and initializes the segment; later
processes attach to it. Creation and attach are serialized by an exclusive
`flock` held until initialization completes, so an attaching process can never
observe a half-initialized segment.

## Requirements and safety

- **All sharers must be the same PHP build**: same version, same ZTS/NTS and
  debug flavor, same APCu build, same lock backend, same `apc.shm_size` at
  creation. Any mismatch is detected at attach and refused (or, with fallback,
  degrades to a private segment) — it never corrupts.
- **Not supported on ZTS builds or fcntl-lock builds**, which are refused: the
  former cannot swap the mapping safely across threads, the latter cannot lock
  across unrelated processes.
- **The path must live in a directory only trusted users can write.** The file
  is created `0600`; APCu refuses a symlink (`O_NOFOLLOW`), a file owned by
  another non-root user, a hard-linked file, or a group/world-writable file.
  This closes the pre-creation/symlink/crafted-file classes, but a file a
  hostile user can *write* is unsafe by construction — keep the directory
  restricted. Sharing between different UIDs (e.g. root FPM master and a
  deploy-user CLI) is therefore not supported by default.
- Prefer a path on **tmpfs** and remove it on boot/deploy (see below).

## Runtime segment rotation

`apcu_rotate_segment(?int $new_size = null, bool $migrate = true): int|false`
replaces the live segment without a restart:

1. builds a fresh segment (optionally a different size — a live resize) under a
   private name,
2. best-effort migrates live entries into it (`$migrate = false` skips this,
   giving a coherent full clear),
3. atomically `rename()`s it over the path and tombstones the old segment.

It returns the number of migrated entries, or `false` on failure. Every
attached process re-attaches automatically at its next request start; a
long-running CLI process can force it with `apcu_segment_refresh(): bool`.

Rotation is refused inside an `apcu_entry()` callback and while an
`APCUIterator` is live (both hold references into the current segment).

### Semantics to know

- **Rotation is a snapshot, not write-through.** Writes that land between the
  migration and a process re-attaching are discarded. Treat a rotation like a
  partial cache clear; do not assume a write made just before a rotation
  survives it.
- **Never `rm -f` the segment file while processes are running.** Running
  processes keep their mapping of the now-unlinked file while new processes
  create a fresh one — a split brain. Use `apcu_rotate_segment(null, false)`
  for a live full clear, and `rm` only before any process starts (boot/deploy).
- A crash between the `rename()` and the tombstone leaves old processes on the
  previous segment until the next rotation; they converge then (data is stale
  but never corrupt).

## Operational recipes

- **Full clear, coherent across CLI and web:** `apcu_rotate_segment(null, false)`.
- **Grow the cache without downtime:** `apcu_rotate_segment(256 * 1024 * 1024)`.
- **Recover a cache lock wedged by a killed process:**
  `apcu_rotate_segment(null, false)` (rotation's publish/tombstone is guarded
  by the file lock, not the wedged cache lock).
- **Cold reset on deploy:** remove the segment file before starting PHP.
- **Check state:** `phpinfo()` shows "MMAP Shared Segment" (Created/Attached),
  its state (Current/Retired) and id.
