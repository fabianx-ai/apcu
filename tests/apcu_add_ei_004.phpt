--TEST--
apcu_add_ei: composition with apcu_update — skips preserve the identifier, replaces wipe it
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
var_dump(apcu_add_ei('k', 'ei:1', 'value'));

// An identical apcu_update is skipped and must leave the identifier in place.
var_dump(apcu_update('k', 'value'));
var_dump(apcu_key_info('k')['expiration_identifier']);
var_dump(apcu_add_ei('k', 'ei:1', 'other'));

// A changing apcu_update replaces the entry; the identifier is gone.
var_dump(apcu_update('k', 'changed'));
var_dump(apcu_key_info('k')['expiration_identifier'] ?? null);
var_dump(apcu_add_ei('k', 'ei:1', 'restored'));
var_dump(apcu_fetch('k'));
?>
--EXPECT--
bool(true)
bool(true)
string(4) "ei:1"
bool(false)
bool(true)
NULL
bool(true)
string(8) "restored"
