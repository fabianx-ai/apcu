--TEST--
apcu_add_ei: an identifier match refreshes nothing — pure add semantics (TTL untouched)
--SKIPIF--
<?php
require_once(dirname(__FILE__) . '/skipif.inc');
if (!function_exists('apcu_inc_request_time')) die('skip APC debug build required');
?>
--INI--
apc.enabled=1
apc.enable_cli=1
apc.use_request_time=1
--FILE--
<?php
// Entry with a 5 second TTL.
var_dump(apcu_add_ei('key', 'ei:1', 'value', 5));

// 3 seconds later a matching add_ei is a no-op: TTL must NOT be refreshed.
apcu_inc_request_time(3);
var_dump(apcu_add_ei('key', 'ei:1', 'value', 5));

// 3 more seconds: 6s since insert — hard expired despite the recent no-op.
apcu_inc_request_time(3);
var_dump(apcu_fetch('key'));

// The expired key stores fresh, even with the same identifier.
var_dump(apcu_add_ei('key', 'ei:1', 'value', 5));
var_dump(apcu_fetch('key'));

// A different identifier replaces and re-arms the TTL.
apcu_inc_request_time(3);
var_dump(apcu_add_ei('key', 'ei:2', 'value2', 5));
apcu_inc_request_time(4);
var_dump(apcu_fetch('key'));
?>
--EXPECT--
bool(true)
bool(false)
bool(false)
bool(true)
string(5) "value"
bool(true)
string(6) "value2"
