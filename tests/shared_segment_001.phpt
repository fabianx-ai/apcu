--TEST--
APC: shared segment - unrelated processes share one cache (apc.mmap_shared_file)
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

$seg = shared_segment_path('001');
shared_segment_cleanup($seg);

/* Process 1: creates the segment, stores all value shapes, exits. */
list($out) = shared_segment_run($seg, '
	$ok = apcu_store("k_int", 42)
		&& apcu_store("k_float", 1.5)
		&& apcu_store("k_bool", true)
		&& apcu_store("k_string", str_repeat("ab", 50))
		&& apcu_store("k_array", ["deep" => ["x" => [1, 2, 3]], "y" => "z"])
		&& apcu_store("k_empty", [])
		&& apcu_store("k_obj", (object) ["p" => "v"]);
	echo $ok ? "writer: stored" : "writer: FAILED", "\n";
');
echo $out, "\n";

/* Process 2: completely unrelated (writer already exited): attaches, reads
 * everything, destroys a fetched array (cross-process pDestructor), and
 * writes back. */
list($out) = shared_segment_run($seg, '
	$expect = [
		"k_int" => 42,
		"k_float" => 1.5,
		"k_bool" => true,
		"k_string" => str_repeat("ab", 50),
		"k_array" => ["deep" => ["x" => [1, 2, 3]], "y" => "z"],
		"k_empty" => [],
	];
	$fail = 0;
	foreach ($expect as $k => $want) {
		$got = apcu_fetch($k, $success);
		if (!$success || $got !== $want) $fail++;
	}
	$obj = apcu_fetch("k_obj");
	if (!($obj instanceof stdClass) || $obj->p !== "v") $fail++;
	echo $fail === 0 ? "reader: all values ok" : "reader: $fail FAILURES", "\n";

	$arr = apcu_fetch("k_array");
	$arr["deep"]["x"][] = 999;
	unset($arr, $obj);
	gc_collect_cycles();
	echo "reader: fetched copies destroyed", "\n";

	$again = apcu_fetch("k_array");
	echo $again === $expect["k_array"] ? "reader: cache unchanged" : "reader: CORRUPTED", "\n";

	echo "reader: inc=", apcu_inc("k_int"), "\n";
');
echo $out, "\n";

/* Process 3: sees process 2's write-back. */
list($out) = shared_segment_run($seg, 'echo "third: k_int=", apcu_fetch("k_int"), "\n";');
echo $out, "\n";

shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
writer: stored
reader: all values ok
reader: fetched copies destroyed
reader: cache unchanged
reader: inc=43
third: k_int=43
===DONE===
