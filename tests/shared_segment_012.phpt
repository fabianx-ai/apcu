--TEST--
APC: shared segment - a concurrent independent process attaches without blocking
--SKIPIF--
<?php
require_once(dirname(__FILE__) . '/shared_segment.inc');
shared_segment_skipif();
?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
require_once(dirname(__FILE__) . '/shared_segment.inc');

$seg = shared_segment_path('012');
shared_segment_cleanup($seg);

/* Regression for the flock-release bug: the segment's mmap keeps the locked
 * open file description alive, so releasing the create/attach flock requires
 * an explicit LOCK_UN — close() alone leaves it held for the life of the
 * process. If that regresses, an independent process's MINIT attach blocks on
 * the first process's lock until it exits, serializing the whole point of the
 * feature.
 *
 * Process A attaches and holds the mapping for 3s. Process B then attaches
 * independently and must complete promptly (well under 3s) AND see A's key —
 * proving both that the lock was released and that the segment is shared. */
$code = 'apcu_store("from_a", "shared"); echo "ready\n"; fflush(STDOUT); usleep(3000000);';
$proc = proc_open(shared_segment_cmd($seg, $code), [
	0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w'],
], $pipes);
fgets($pipes[1]); /* "ready" — A is attached and holding */

$t0 = microtime(TRUE);
list($out) = shared_segment_run($seg, 'echo apcu_fetch("from_a"), "\n";');
$elapsed = microtime(TRUE) - $t0;

echo "B saw: ", trim($out), "\n";
echo "B did not block on A: ", ($elapsed < 2.0) ? "yes" : "NO ({$elapsed}s)", "\n";

fclose($pipes[0]); fclose($pipes[1]); fclose($pipes[2]); proc_close($proc);
shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
B saw: shared
B did not block on A: yes
===DONE===
