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

/* Process A creates segment O, stores a key, then stays alive holding O's
 * mapping (so O is never re-created and A keeps mapping the pre-crash
 * segment). It rotates only after signalled. */
$code = '
	apcu_store("orig", "in-original");
	echo "ready\n"; fflush(STDOUT);
	fgets(STDIN);              /* wait until a successor has been published */
	$migrated = apcu_rotate_segment();
	echo "A rotate migrated: ", $migrated, "\n";
	echo "A sees orig: ", var_export(apcu_fetch("orig"), true), "\n";
	echo "A sees only_in_successor: ", var_export(apcu_fetch("only_in_successor"), true), "\n";
';
$proc = proc_open(shared_segment_cmd($seg, $code), [
	0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w'],
], $pipes);
echo fgets($pipes[1]); /* "ready" — A holds O */

/* Process B attaches to O and rotates with the retire step suppressed
 * (simulated crash between rename and retire): successor S1 is published at
 * the path (carrying O's migrated "orig"), O is NOT tombstoned, B stores a
 * key that only exists in S1, then exits. */
list($out) = shared_segment_run($seg, '
	apcu_rotate_segment();
	apcu_store("only_in_successor", "in-S1");
', '8M', [], TRUE /* skip_retire */);
echo $out;

/* Now A (still mapping O, which was never tombstoned) rotates. Its retired
 * check is false, so only the segment-id identity check can save it: it must
 * notice the path holds S1 (different id), adopt S1, and rotate THAT — so both
 * keys survive. Without the check A would rotate stale O and discard S1. */
fwrite($pipes[0], "go\n");
fclose($pipes[0]);
echo stream_get_contents($pipes[1]);
fclose($pipes[1]); fclose($pipes[2]); proc_close($proc);

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
ready
A rotate migrated: 2
A sees orig: 'in-original'
A sees only_in_successor: 'in-S1'
===DONE===
