--TEST--
APC: shared segment - hardening against planted/hostile segment files
--SKIPIF--
<?php
require_once(dirname(__FILE__) . '/shared_segment.inc');
shared_segment_skipif();
if (getenv('SKIP_SLOW_TESTS')) die('skip slow test');
?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
require_once(dirname(__FILE__) . '/shared_segment.inc');

$seg = shared_segment_path('008');
shared_segment_cleanup($seg);

/* 1. A symlink at the segment path must be refused (O_NOFOLLOW), not
 * followed to its target. */
$target = $seg . '.target';
@unlink($target);
symlink($target, $seg);
list($out, $status) = shared_segment_run($seg, 'apcu_store("x", 1);');
echo "symlink: ", ($status !== 0 && strpos($out, 'open on') !== false) ? 'refused' : "UNEXPECTED: $out", "\n";
echo "symlink target not created: ", file_exists($target) ? 'NO (followed!)' : 'yes', "\n";
@unlink($seg);
@unlink($target);

/* 2. A world-writable regular file at the path must be refused: any local
 * user could otherwise corrupt every attached process. */
file_put_contents($seg, str_repeat("\0", 8 * 1024 * 1024));
chmod($seg, 0666);
list($out, $status) = shared_segment_run($seg, 'apcu_store("x", 1);');
echo "world-writable: ", ($status !== 0 && strpos($out, 'group/world writable') !== false) ? 'refused' : "UNEXPECTED: $out", "\n";
shared_segment_cleanup($seg);

/* 3. A hard-linked file (nlink > 1) must be refused. */
file_put_contents($seg, str_repeat("\0", 8 * 1024 * 1024));
chmod($seg, 0600);
link($seg, $seg . '.hardlink');
list($out, $status) = shared_segment_run($seg, 'apcu_store("x", 1);');
echo "hardlinked: ", ($status !== 0 && strpos($out, 'hard link') !== false) ? 'refused' : "UNEXPECTED: $out", "\n";
@unlink($seg . '.hardlink');
shared_segment_cleanup($seg);

/* 4. Sanity: a normal 0600 file this process would create still works. */
list($out) = shared_segment_run($seg, 'var_dump(apcu_store("x", 1) && apcu_fetch("x") === 1);');
echo "normal file: ", trim($out), "\n";
shared_segment_cleanup($seg);
?>
===DONE===
--EXPECT--
symlink: refused
symlink target not created: yes
world-writable: refused
hardlinked: refused
normal file: bool(true)
===DONE===
