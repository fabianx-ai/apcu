--TEST--
APC: shared segment - attach validation, crash recovery, size adoption
--SKIPIF--
<?php
require_once(dirname(__FILE__) . '/shared_segment.inc');
shared_segment_skipif();
if (PHP_ZTS) die('skip rotation is not supported on ZTS builds');
?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
require_once(dirname(__FILE__) . '/shared_segment.inc');

$seg = shared_segment_path('002');

/* A sized file with no valid segment and a size that doesn't match
 * apc.shm_size must be refused, not clobbered. */
shared_segment_cleanup($seg);
file_put_contents($seg, random_bytes(2 * 1024 * 1024));
chmod($seg, 0600); /* planted files must not trip the group/world-writable refusal (umask-proof) */
list($out, $status) = shared_segment_run($seg, 'apcu_store("x", 1);');
echo "garbage wrong size: ", (strpos($out, 'contains no valid segment') !== false && $status !== 0) ? 'refused' : "UNEXPECTED: $out", "\n";

/* A zero-filled file of exactly apc.shm_size looks like an initialization
 * that crashed before completing; it is safely re-initialized. */
shared_segment_cleanup($seg);
file_put_contents($seg, str_repeat("\0", 8 * 1024 * 1024));
chmod($seg, 0600); /* planted files must not trip the group/world-writable refusal (umask-proof) */
list($out) = shared_segment_run($seg, 'var_dump(apcu_store("x", 1) && apcu_fetch("x") === 1);');
echo "crashed-init recovery: ", trim($out), "\n";

/* After a rotation grew the segment, attachers whose apc.shm_size still has
 * the old value must adopt the file's size instead of erroring. */
shared_segment_cleanup($seg);
shared_segment_run($seg, 'apcu_store("y", "kept"); apcu_rotate_segment(16 * 1024 * 1024);');
list($out) = shared_segment_run($seg, '
	echo apcu_sma_info(true)["seg_size"] > 8 * 1024 * 1024 ? "adopted grown size" : "STUCK AT INI SIZE", "\n";
	echo "migrated value: ", var_export(apcu_fetch("y"), true), "\n";
'); /* note: still runs with apc.shm_size=8M */
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
garbage wrong size: refused
crashed-init recovery: bool(true)
adopted grown size
migrated value: 'kept'
===DONE===
