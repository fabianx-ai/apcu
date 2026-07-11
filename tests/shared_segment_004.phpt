--TEST--
APC: shared segment - a long-running process converges via apcu_segment_refresh()
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

$seg = shared_segment_path('004');
shared_segment_cleanup($seg);

/* Long-running process A: attaches first, then waits on stdin while another
 * process rotates the segment underneath it (pipe handshake, no sleeps). */
$code = '
	apcu_store("pre", "stored-before-rotation");
	echo "ready\n";
	fflush(STDOUT);
	fgets(STDIN); /* parent signals: rotation happened */

	$stale = apcu_fetch("post", $hit);
	echo "stale read hit: ", var_export($hit, true), "\n";
	echo "refresh: ", var_export(apcu_segment_refresh(), true), "\n";
	echo "fresh read: ", var_export(apcu_fetch("post"), true), "\n";
	echo "migrated key: ", var_export(apcu_fetch("pre"), true), "\n";
';
$proc = proc_open(shared_segment_cmd($seg, $code), [
	0 => ['pipe', 'r'],
	1 => ['pipe', 'w'],
	2 => ['pipe', 'w'],
], $pipes);

echo trim(fgets($pipes[1])), "\n"; /* "ready" — A is attached */

/* Process B: rotates (migrating A's key) and stores a successor-only key. */
list($out) = shared_segment_run($seg, '
	echo "rotator migrated: ", var_export(apcu_rotate_segment(), true), "\n";
	apcu_store("post", "only-in-successor");
');
echo $out, "\n";

fwrite($pipes[0], "go\n");
fclose($pipes[0]);
echo stream_get_contents($pipes[1]);
fclose($pipes[1]);
fclose($pipes[2]);
proc_close($proc);

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
ready
rotator migrated: 1
stale read hit: false
refresh: true
fresh read: 'only-in-successor'
migrated key: 'stored-before-rotation'
===DONE===
