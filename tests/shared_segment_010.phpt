--TEST--
APC: shared segment - rotating too small for the entry hint fails softly (no worker kill)
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

$seg = shared_segment_path('010');
shared_segment_cleanup($seg);

/* entries_hint of 1,000,000 => a slots array of several MB. Rotating into a
 * 1MB successor cannot fit that index. It must return false and leave the
 * process alive and the cache usable, not fatal with E_CORE_ERROR. */
list($out, $status) = shared_segment_run($seg, '
	error_reporting(0);
	apcu_store("keep", "value");
	$r = apcu_rotate_segment(1024 * 1024);
	echo "rotate result: ", var_export($r, true), "\n";
	echo "still alive, keep = ", var_export(apcu_fetch("keep"), true), "\n";
', '16M', ['apc.entries_hint=1000000']);
echo $out, "\n";
echo "child exit: ", $status, "\n";

/* An absurd size must also be rejected up front, not overflow. */
list($out) = shared_segment_run($seg, '
	error_reporting(0);
	var_dump(apcu_rotate_segment(PHP_INT_MAX));
', '16M', ['apc.entries_hint=0']);
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
rotate result: false
still alive, keep = 'value'
child exit: 0
bool(false)
===DONE===
