--TEST--
apcu_update: a skipped store refreshes the entry's TTL like a real store
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
var_dump(apcu_update('key', 'value', 5));

// 3 seconds later an identical update is skipped but must refresh the TTL.
apcu_inc_request_time(3);
var_dump(apcu_update('key', 'value', 5));
$info = apcu_cache_info(true);
var_dump((int) $info['num_skipped']);

// 4 more seconds: 7s since insert, 4s since refresh — still alive.
apcu_inc_request_time(4);
var_dump(apcu_fetch('key'));

// 2 more seconds: 6s since refresh — hard expired.
apcu_inc_request_time(2);
var_dump(apcu_fetch('key'));

// An update on the expired key inserts anew (no skip).
var_dump(apcu_update('key', 'value', 5));
var_dump(apcu_fetch('key'));
$info = apcu_cache_info(true);
var_dump((int) $info['num_skipped']);
?>
--EXPECT--
bool(true)
bool(true)
int(1)
string(5) "value"
bool(false)
bool(true)
string(5) "value"
int(1)
