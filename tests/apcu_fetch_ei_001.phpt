--TEST--
apcu_fetch_ei: values fetched together with their expiration identifiers
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
apcu_add_ei('a', 'wave:1', ['x' => 1]);
apcu_store('plain', 'v');

var_dump(apcu_fetch_ei('a'));
var_dump(apcu_fetch_ei('missing'));

$r = apcu_fetch_ei(['a', 'plain', 'missing']);
var_dump(array_keys($r));
var_dump($r['a']['ei'], $r['plain']['ei'], $r['plain']['value']);

// a plain store wipes the identifier; the pair reflects that atomically
apcu_store('a', 'replaced');
$r = apcu_fetch_ei('a');
var_dump($r['value'], $r['ei']);
?>
--EXPECT--
array(2) {
  ["value"]=>
  array(1) {
    ["x"]=>
    int(1)
  }
  ["ei"]=>
  string(6) "wave:1"
}
bool(false)
array(2) {
  [0]=>
  string(1) "a"
  [1]=>
  string(5) "plain"
}
string(6) "wave:1"
NULL
string(1) "v"
string(8) "replaced"
NULL
