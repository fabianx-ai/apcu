--TEST--
apcu_add_ei: stores unless the expiration identifier already matches
--SKIPIF--
<?php require_once(dirname(__FILE__) . '/skipif.inc'); ?>
--INI--
apc.enabled=1
apc.enable_cli=1
--FILE--
<?php
$value = ['payload' => str_repeat('x', 64)];

// Insert when the key is absent.
var_dump(apcu_add_ei('k', 'wave:100', $value));

// Matching identifier: nothing stored, add-style false.
var_dump(apcu_add_ei('k', 'wave:100', $value));

// Matching identifier wins even when the value differs — the identifier is
// the version, the stored value stays untouched.
var_dump(apcu_add_ei('k', 'wave:100', ['payload' => 'changed']));
var_dump(apcu_fetch('k') === $value);

// Different identifier: replaced, new identifier recorded.
var_dump(apcu_add_ei('k', 'wave:101', ['payload' => 'changed']));
var_dump(apcu_fetch('k')['payload']);
var_dump(apcu_key_info('k')['expiration_identifier']);

// Integer identifiers are normalized to their string form.
var_dump(apcu_add_ei('i', 123, 'v1'));
var_dump(apcu_add_ei('i', '123', 'v2'));
var_dump(apcu_fetch('i'));

// Entries stored without an identifier never match.
apcu_store('plain', 'v');
var_dump(apcu_key_info('plain')['expiration_identifier'] ?? null);
var_dump(apcu_add_ei('plain', 'any', 'w'));
var_dump(apcu_fetch('plain'));

// A plain store wipes the identifier again.
apcu_store('k', 'v');
var_dump(apcu_key_info('k')['expiration_identifier'] ?? null);

// Invalid identifier type warns and fails.
var_dump(@apcu_add_ei('k', [1], 'v'));

$info = apcu_cache_info(true);
var_dump((int) $info['num_inserts'], (int) $info['num_skipped']);
?>
--EXPECT--
bool(true)
bool(false)
bool(false)
bool(true)
bool(true)
string(7) "changed"
string(8) "wave:101"
bool(true)
bool(false)
string(2) "v1"
NULL
bool(true)
string(1) "w"
NULL
bool(false)
int(6)
int(3)
