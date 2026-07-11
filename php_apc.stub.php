<?php

/**
 * @generate-function-entries PHP_APCU_API
 * @generate-legacy-arginfo
 */

/**
 * Rotates the file-backed shared segment (apc.mmap_shared_file): builds a
 * fresh segment (optionally with a new size), migrates live entries into it,
 * atomically replaces the shared file and retires the old segment so all
 * attached processes re-attach at their next request start.
 * Returns the number of migrated entries, or false on failure.
 */
function apcu_rotate_segment(?int $new_size = null, bool $migrate = true): int|false {}

/**
 * Re-attaches to the current shared segment if this process's mapping was
 * retired by a rotation. Mainly useful for long-running CLI processes; web
 * requests refresh automatically at request start.
 * Returns true when the process is attached to the current segment.
 */
function apcu_segment_refresh(): bool {}

function apcu_clear_cache(): bool {}

function apcu_cache_info(bool $limited = false): array|false {}

function apcu_key_info(string $key): ?array {}

function apcu_sma_info(bool $limited = false): array|false {}

function apcu_enabled(): bool {}

/** @param array|string $key */
function apcu_store($key, mixed $value = UNKNOWN, int $ttl = 0): array|bool {}

/** @param array|string $key */
function apcu_add($key, mixed $value = UNKNOWN, int $ttl = 0): array|bool {}

/** @param bool $success */
function apcu_inc(string $key, int $step = 1, &$success = null, int $ttl = 0): int|false {}

/** @param bool $success */
function apcu_dec(string $key, int $step = 1, &$success = null, int $ttl = 0): int|false {}

function apcu_cas(string $key, int $old, int $new): bool {}

/**
 * @param array|string $key
 * @param bool $success
 */
function apcu_fetch($key, &$success = null): mixed {}

/** @param array|string $key */
function apcu_exists($key): array|bool {}

/** @param APCUIterator|array|string $key */
function apcu_delete($key): array|bool {}

function apcu_entry(string $key, callable $callback, int $ttl = 0): mixed {}

#ifdef APC_DEBUG
function apcu_inc_request_time(int $by = 1): void {}
#endif
