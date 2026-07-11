--TEST--
APC: shared segment - rotation and refresh are refused inside apcu_entry()
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

$seg = shared_segment_path('006');
shared_segment_cleanup($seg);

/* apcu_entry() holds the cache lock across its callback; rotating or
 * refreshing in there would migrate an empty snapshot and unlock the wrong
 * segment's lock. Both must refuse, and the cache must stay fully
 * functional afterwards (same process and a later attacher). */
list($out) = shared_segment_run($seg, '
	apcu_store("existing", "data");

	$value = apcu_entry("computed", function ($key) {
		$rotate = @apcu_rotate_segment();
		echo "rotate inside entry: ", var_export($rotate, true), "\n";
		$refresh = @apcu_segment_refresh();
		echo "refresh inside entry: ", var_export($refresh, true), "\n";
		return "computed-value";
	});
	echo "entry result: ", var_export($value, true), "\n";

	/* lock protocol must be intact: these all need the cache lock */
	echo "existing: ", var_export(apcu_fetch("existing"), true), "\n";
	apcu_store("after-entry", "still-working");
	echo "store after entry: ", var_export(apcu_fetch("after-entry"), true), "\n";
	echo "entries: ", apcu_cache_info(true)["num_entries"], "\n";

	/* outside the callback rotation works again */
	echo "rotate outside entry: ", apcu_rotate_segment(), "\n";
');
echo $out, "\n";

/* a fresh process can still attach and see the rotated data */
list($out) = shared_segment_run($seg, '
	echo "attacher: ", var_export(apcu_fetch("computed"), true), "\n";
');
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
rotate inside entry: false
refresh inside entry: false
entry result: 'computed-value'
existing: 'data'
store after entry: 'still-working'
entries: 3
rotate outside entry: 3
attacher: 'computed-value'
===DONE===
