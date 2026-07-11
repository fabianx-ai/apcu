<?php
/*
 * Helpers for apc.mmap_shared_file tests. Shared-segment behavior is
 * multi-process by nature and the segment is created at MINIT, so these
 * tests run the parent WITHOUT shared mode and drive child PHP processes
 * that load the extension with the shared-file INI settings.
 */

function shared_segment_skipif() {
	if (!extension_loaded('apcu')) {
		die('skip APCu extension not loaded');
	}
	if (PHP_OS_FAMILY === 'Windows') {
		die('skip requires mmap + flock (POSIX)');
	}
	if (!shared_segment_extension_path()) {
		die('skip cannot locate apcu.so for child processes');
	}
}

function shared_segment_extension_path() {
	/* Prefer the build tree: a system extension_dir may hold an unpatched
	 * apcu.so (e.g. a disabled distro package) that silently ignores the
	 * shared-segment INI settings. */
	$candidates = [
		__DIR__ . '/../modules/apcu.so',
		ini_get('extension_dir') . '/apcu.so',
	];
	foreach ($candidates as $candidate) {
		$real = realpath($candidate);
		if ($real !== false) {
			return $real;
		}
	}
	return false;
}

function shared_segment_path($name) {
	return sys_get_temp_dir() . '/apcu_test_' . $name . '_' . getmypid() . '.seg';
}

function shared_segment_cmd($seg, $code, $shm_size = '8M', $extra_ini = []) {
	$php = getenv('TEST_PHP_EXECUTABLE') ?: PHP_BINARY;
	$args = [
		'-n',
		'-d', 'extension=' . shared_segment_extension_path(),
		'-d', 'apc.enabled=1',
		'-d', 'apc.enable_cli=1',
		'-d', 'apc.mmap_shared_file=' . $seg,
		'-d', 'apc.shm_size=' . $shm_size,
		'-d', 'display_errors=1',
		'-d', 'display_startup_errors=1',
		'-d', 'log_errors=0',
		'-d', 'error_reporting=' . E_ALL,
	];
	foreach ($extra_ini as $ini) {
		$args[] = '-d';
		$args[] = $ini;
	}
	$cmd = escapeshellarg($php);
	foreach ($args as $arg) {
		$cmd .= ' ' . escapeshellarg($arg);
	}
	return $cmd . ' -r ' . escapeshellarg($code);
}

function shared_segment_run($seg, $code, $shm_size = '8M', $extra_ini = []) {
	$output = [];
	$status = 0;
	exec(shared_segment_cmd($seg, $code, $shm_size, $extra_ini) . ' 2>&1', $output, $status);
	return [implode("\n", $output), $status];
}

function shared_segment_cleanup($seg) {
	@unlink($seg);
	@unlink($seg . '.new');
}
