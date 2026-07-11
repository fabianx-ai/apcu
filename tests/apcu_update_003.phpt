--TEST--
apcu_update: array-of-pairs form and interaction with in-place updates (apcu_inc)
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
// Array-of-pairs behaves like apcu_store: returns keys that failed.
var_dump(apcu_update(['a' => 1, 'b' => 'two']));
var_dump(apcu_fetch('a'), apcu_fetch('b'));

// Second identical batch: all skipped, none failed.
var_dump(apcu_update(['a' => 1, 'b' => 'two']));
$info = apcu_cache_info(true);
var_dump((int) $info['num_skipped']);

// apcu_update compares against in-place mutations done by apcu_inc.
apcu_inc('a');                 // 1 -> 2 in place
var_dump(apcu_update('a', 2)); // identical to current value: skip
var_dump(apcu_update('a', 3)); // different: store
var_dump(apcu_fetch('a'));
$info = apcu_cache_info(true);
var_dump((int) $info['num_inserts'], (int) $info['num_skipped']);
?>
--EXPECT--
array(0) {
}
int(1)
string(3) "two"
array(0) {
}
int(2)
bool(true)
bool(true)
int(3)
int(3)
int(3)
