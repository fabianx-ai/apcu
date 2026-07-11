--TEST--
APC: shared segment - a stale rotator adopts the published segment (no split brain)
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

$seg = shared_segment_path('009');
shared_segment_cleanup($seg);

/* Process A creates the segment and stores a key. */
shared_segment_run($seg, 'apcu_store("orig", "in-original");');

/* Process X rotates with the retire step suppressed (simulating a crash
 * between rename() and retire): the successor S1 is published at the path and
 * gets its own key, but the original segment is never tombstoned. */
list($out) = shared_segment_run($seg, '
	apcu_rotate_segment();               /* -> S1 published, retire skipped */
	apcu_store("only_in_successor", "in-S1");
	echo "published successor, orig migrated: ", var_export(apcu_fetch("orig"), true), "\n";
', '8M', ['apc.enable_cli=1'], TRUE /* skip_retire */);
echo $out, "\n";

/* A fresh process now rotates normally. Because the original was never
 * tombstoned, without the segment-identity check it would rotate stale data
 * and discard S1 (losing "only_in_successor"). With the check it must adopt
 * S1 first, so both keys survive into the new successor. */
list($out) = shared_segment_run($seg, '
	$migrated = apcu_rotate_segment();
	echo "second rotate migrated: ", $migrated, "\n";
	echo "orig: ", var_export(apcu_fetch("orig"), true), "\n";
	echo "only_in_successor: ", var_export(apcu_fetch("only_in_successor"), true), "\n";
');
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
published successor, orig migrated: 'in-original'
second rotate migrated: 2
orig: 'in-original'
only_in_successor: 'in-S1'
===DONE===
