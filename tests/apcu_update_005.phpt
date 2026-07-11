--TEST--
apcu_update: structurally persisted arrays always store (best-effort skip), values stay correct
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
apc.serializer=default
--FILE--
<?php
$arr = ['a' => 1, 'nested' => ['x', 'y']];

// Without a serializer arrays are persisted structurally; their identical
// stores are not skipped in the current implementation (best-effort by
// design), but the semantics must match apcu_store exactly.
var_dump(apcu_update('arr', $arr));
var_dump(apcu_update('arr', $arr));
var_dump(apcu_fetch('arr') === $arr);

$arr['b'] = 2;
var_dump(apcu_update('arr', $arr));
var_dump(apcu_fetch('arr') === $arr);

// Scalars still skip under this configuration.
apcu_update('int', 7);
var_dump(apcu_update('int', 7));

$info = apcu_cache_info(true);
var_dump((int) $info['num_skipped']);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
int(1)
