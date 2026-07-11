--TEST--
apcu_update: arrays under an explicit serializer are compared in serialized form
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
apc.serializer=php
--FILE--
<?php
$arr = ['a' => 1, 'nested' => ['x', 'y'], 3.5, true, null];

var_dump(apcu_update('arr', $arr));  // insert
var_dump(apcu_update('arr', $arr));  // identical: skip

$arr['nested'][] = 'z';
var_dump(apcu_update('arr', $arr));  // different: store
var_dump(apcu_fetch('arr') === $arr);

$info = apcu_cache_info(true);
var_dump((int) $info['num_inserts'], (int) $info['num_skipped']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
int(2)
int(1)
