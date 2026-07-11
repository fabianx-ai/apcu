--TEST--
APC: shared segment - shrink rotation with overflowing data must not hang
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

$seg = shared_segment_path('005');
shared_segment_cleanup($seg);

/* Fill an 8M segment with ~4M of entries, then rotate into a 1M successor:
 * migration must stop when the successor fills (best effort), not spin. */
list($out) = shared_segment_run($seg, '
	$payload = str_repeat("x", 100 * 1024);
	for ($i = 0; $i < 40; $i++) {
		apcu_store("bulk$i", $payload);
	}
	$stored = apcu_cache_info(true)["num_entries"];

	$migrated = apcu_rotate_segment(1024 * 1024);

	echo "returned: ", is_int($migrated) ? "int" : var_export($migrated, true), "\n";
	echo "partial migration: ", ($migrated >= 0 && $migrated < $stored) ? "yes" : "NO ($migrated of $stored)", "\n";
	echo "entry count matches: ", apcu_cache_info(true)["num_entries"] === $migrated ? "yes" : "NO", "\n";

	/* migration picks entries in slot order, so count and verify whatever
	 * survived (before any new store, which could trigger an expunge) */
	$found = 0;
	$intact = true;
	for ($i = 0; $i < 40; $i++) {
		$v = apcu_fetch("bulk$i", $ok);
		if ($ok) {
			$found++;
			if ($v !== $payload) $intact = false;
		}
	}
	echo "survivors fetchable: ", $found === $migrated ? "yes" : "NO ($found vs $migrated)", "\n";
	echo "survivors intact: ", $intact ? "yes" : "NO", "\n";

	/* the shrunken cache must still accept writes */
	apcu_store("after", "works");
	echo "store after shrink: ", var_export(apcu_fetch("after"), true), "\n";
');
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
returned: int
partial migration: yes
entry count matches: yes
survivors fetchable: yes
survivors intact: yes
store after shrink: 'works'
===DONE===
