--TEST--
APC: shared segment - fallback to private segment and iterator guard
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

$seg = shared_segment_path('011');

/* M7: an incompatible/garbage file that does not match apc.shm_size normally
 * aborts startup. With apc.mmap_shared_file_fallback=1 the process instead
 * warns and runs on a private (unshared) segment — still functional. */
shared_segment_cleanup($seg);
file_put_contents($seg, random_bytes(2 * 1024 * 1024)); /* 2M != 8M shm_size */
chmod($seg, 0600); /* planted files must not trip the group/world-writable refusal (umask-proof) */

list($out, $status) = shared_segment_run($seg, '
	var_dump(apcu_store("k", "v") && apcu_fetch("k") === "v");
', '8M', ['apc.mmap_shared_file_fallback=1']);
/* Drop the expected MINIT fallback warning; keep the var_dump result. */
$lines = array_filter(explode("\n", $out), function ($l) {
	return $l !== '' && strpos($l, 'Warning') === FALSE && strpos($l, 'Startup') === FALSE;
});
echo "fallback works: ", trim(implode("\n", $lines)), " (exit $status)\n";
shared_segment_cleanup($seg);

/* M8: apcu_rotate_segment() / apcu_segment_refresh() must refuse while an
 * APCUIterator is alive (it holds raw pointers into the segment). */
list($out) = shared_segment_run($seg, '
	error_reporting(0);
	for ($i = 0; $i < 3; $i++) apcu_store("k$i", $i);
	$it = new APCUIterator();
	echo "rotate while iterating: ", var_export(apcu_rotate_segment(), true), "\n";
	echo "refresh while iterating: ", var_export(apcu_segment_refresh(), true), "\n";
	unset($it); /* iterator gone */
	echo "rotate after iterating: ", apcu_rotate_segment(), "\n";
');
echo $out, "\n";
shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
fallback works: bool(true) (exit 0)
rotate while iterating: false
refresh while iterating: false
rotate after iterating: 3
===DONE===
