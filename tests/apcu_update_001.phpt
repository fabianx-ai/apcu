--TEST--
apcu_update: stores like apcu_store, skips when the stored value is identical
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
function counters() {
	$info = apcu_cache_info(true);
	return "inserts=" . (int) $info['num_inserts'] . " skipped=" . (int) $info['num_skipped'];
}

// Insert when the key is absent.
var_dump(apcu_update('str', 'value'));
var_dump(apcu_fetch('str'));

// Identical string: skipped, not re-inserted.
var_dump(apcu_update('str', 'value'));
echo counters(), "\n";

// Different string: stored.
var_dump(apcu_update('str', 'other'));
var_dump(apcu_fetch('str'));
echo counters(), "\n";

// Scalars.
apcu_update('int', 42);
var_dump(apcu_update('int', 42));    // skip
var_dump(apcu_update('int', 43));    // store
var_dump(apcu_fetch('int'));

apcu_update('float', 1.5);
var_dump(apcu_update('float', 1.5)); // skip
var_dump(apcu_update('float', 2.5)); // store

apcu_update('bool', true);
var_dump(apcu_update('bool', true));  // skip
var_dump(apcu_update('bool', false)); // store (different type)
var_dump(apcu_fetch('bool'));

apcu_update('null', null);
var_dump(apcu_update('null', null)); // skip

// Type change is a store, never a skip.
var_dump(apcu_update('int', 'fourty-three'));
var_dump(apcu_fetch('int'));

// Objects are compared in serialized form.
$obj = new stdClass;
$obj->list = [1, 2, ['deep' => 'value']];
apcu_update('obj', $obj);
var_dump(apcu_update('obj', $obj));  // skip
$obj->list[] = 4;
var_dump(apcu_update('obj', $obj));  // store
var_dump(apcu_fetch('obj')->list[3]);

echo counters(), "\n";
?>
--EXPECT--
bool(true)
string(5) "value"
bool(true)
inserts=1 skipped=1
bool(true)
string(5) "other"
inserts=2 skipped=1
bool(true)
bool(true)
int(43)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
string(12) "fourty-three"
bool(true)
bool(true)
int(4)
inserts=12 skipped=6
