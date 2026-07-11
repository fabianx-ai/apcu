--TEST--
apcu_add_ei: array-of-pairs form with a shared identifier (invalidation wave)
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
// First wave: everything stores.
var_dump(apcu_add_ei(['a' => 1, 'b' => 'two', 'c' => [3]], 'wave:1'));

// Same wave replayed: everything is skipped, add-style "failed" keys returned.
var_dump(apcu_add_ei(['a' => 9, 'b' => 9, 'c' => 9], 'wave:1'));
var_dump(apcu_fetch('a'), apcu_fetch('b'), apcu_fetch('c'));

// Next wave: everything replaces.
var_dump(apcu_add_ei(['a' => 10, 'b' => 20], 'wave:2'));
var_dump(apcu_fetch('a'), apcu_fetch('b'));
// c keeps wave:1
var_dump(apcu_key_info('c')['expiration_identifier']);

$info = apcu_cache_info(true);
var_dump((int) $info['num_inserts'], (int) $info['num_skipped']);
?>
--EXPECT--
array(0) {
}
array(3) {
  ["a"]=>
  int(-1)
  ["b"]=>
  int(-1)
  ["c"]=>
  int(-1)
}
int(1)
string(3) "two"
array(1) {
  [0]=>
  int(3)
}
array(0) {
}
int(10)
int(20)
string(6) "wave:1"
int(5)
int(3)
