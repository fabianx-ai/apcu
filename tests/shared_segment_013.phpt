--TEST--
APC: shared segment - a creator that cannot reserve space unlinks its file
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

$seg = shared_segment_path('013');
shared_segment_cleanup($seg);

/* Ask for more space than the target filesystem can provide: the creator
 * must fail cleanly at MINIT AND must not leave a sized-but-sparse file
 * behind — later attachers to such a file cannot even read its header on a
 * full filesystem without SIGBUSing on page allocation. */
$free = (float) disk_free_space(dirname($seg));
$size = (string) (int) ($free * 2 + 256 * 1024 * 1024);

list($out, $status) = shared_segment_run($seg, 'echo "should not run\n";', $size);
echo 'creator failed: ';
if (preg_match('/cannot reserve|failed to mmap|ftruncate on/', $out) && $status !== 0) {
	echo "yes\n";
} else {
	echo "no (status=$status output=$out)\n";
}
echo 'file left behind: ', var_export(file_exists($seg), true), "\n";

/* The path must be immediately reusable at a sane size. */
list($out, $status) = shared_segment_run($seg,
	'apcu_store("k", "ok"); echo "retry stored: ", var_export(apcu_fetch("k"), true), "\n";');
echo $out, "\n";
echo 'segment created: ', var_export(file_exists($seg), true), "\n";

shared_segment_cleanup($seg);
?>
--EXPECT--
creator failed: yes
file left behind: false
retry stored: 'ok'
segment created: true
