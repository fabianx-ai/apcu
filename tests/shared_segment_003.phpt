--TEST--
APC: shared segment - apcu_rotate_segment(): migration, resize, clear, errors
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

/* Without apc.mmap_shared_file rotation must refuse (this process). */
var_dump(@apcu_rotate_segment());
var_dump(apcu_segment_refresh());

$seg = shared_segment_path('003');
shared_segment_cleanup($seg);

list($out) = shared_segment_run($seg, '
	for ($i = 0; $i < 5; $i++) {
		apcu_store("key$i", ["n" => $i, "payload" => str_repeat("x", 100)]);
	}

	/* same-size rotation migrates everything */
	echo "rotate: ", var_export(apcu_rotate_segment(), true), "\n";
	$ok = true;
	for ($i = 0; $i < 5; $i++) {
		$v = apcu_fetch("key$i");
		$ok = $ok && $v === ["n" => $i, "payload" => str_repeat("x", 100)];
	}
	echo "integrity after rotate: ", $ok ? "ok" : "FAILED", "\n";

	/* grow */
	$before = apcu_sma_info(true)["seg_size"];
	echo "grow: ", var_export(apcu_rotate_segment(16 * 1024 * 1024), true), "\n";
	echo "grew: ", apcu_sma_info(true)["seg_size"] > $before ? "yes" : "NO", "\n";
	echo "entries after grow: ", apcu_cache_info(true)["num_entries"], "\n";

	/* rotate without migration = coherent full clear */
	echo "clear-rotate: ", var_export(apcu_rotate_segment(null, false), true), "\n";
	echo "entries after clear: ", apcu_cache_info(true)["num_entries"], "\n";

	/* too small must refuse */
	echo "tiny: ", var_export(@apcu_rotate_segment(512 * 1024), true), "\n";
');
echo $out, "\n";

/* A later process attaches to the final (grown, cleared) segment. */
list($out) = shared_segment_run($seg, '
	echo "attacher entries: ", apcu_cache_info(true)["num_entries"], "\n";
	echo "attacher sees grown segment: ", apcu_sma_info(true)["seg_size"] > 8 * 1024 * 1024 ? "yes" : "NO", "\n";
');
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
bool(false)
bool(false)
rotate: 5
integrity after rotate: ok
grow: 5
grew: yes
entries after grow: 5
clear-rotate: 0
entries after clear: 0
tiny: false
attacher entries: 0
attacher sees grown segment: yes
===DONE===
