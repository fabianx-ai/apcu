--TEST--
APC: shared segment - migrated entries are reclaimable (not pinned by a phantom reference)
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

$seg = shared_segment_path('007');
shared_segment_cleanup($seg);

/* With apc.gc_ttl=0 an entry that still has references when removed is
 * parked on the GC list forever. Migrated entries must therefore carry no
 * phantom reference: after deleting them, their memory must come back. */
list($out) = shared_segment_run($seg, '
	$payload = str_repeat("x", 100 * 1024);
	for ($i = 0; $i < 10; $i++) {
		apcu_store("bulk$i", $payload);
	}

	echo "migrated: ", apcu_rotate_segment(), "\n";

	$avail_before = apcu_sma_info(true)["avail_mem"];
	for ($i = 0; $i < 10; $i++) {
		apcu_delete("bulk$i");
	}
	$avail_after = apcu_sma_info(true)["avail_mem"];

	echo "entries after delete: ", apcu_cache_info(true)["num_entries"], "\n";
	$reclaimed = $avail_after - $avail_before;
	echo "reclaimed at least 900K: ", $reclaimed > 900 * 1024 ? "yes" : "NO ($reclaimed bytes)", "\n";

	/* the reclaimed space must actually be reusable */
	$ok = true;
	for ($i = 0; $i < 10; $i++) {
		$ok = $ok && apcu_store("refill$i", $payload);
	}
	echo "refill: ", $ok ? "ok" : "FAILED", "\n";
', '4M', ['apc.gc_ttl=0']); /* gc_ttl=0: a pinned entry would leak forever;
                              small segment so it would visibly break refill */
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
migrated: 10
entries after delete: 0
reclaimed at least 900K: yes
refill: ok
===DONE===
