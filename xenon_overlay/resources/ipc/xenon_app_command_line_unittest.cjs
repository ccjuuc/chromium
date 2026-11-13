// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function commandLine(host) {
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'main', 'app_command_line.js'), 'utf8'), context);
  return context.createAppCommandLine(host);
}
const plain = value => JSON.parse(JSON.stringify(value));

test('command line queries return native values, including missing and valueless switches', () => {
  const requests = [];
  const command = commandLine(request => {
    requests.push(plain(request));
    return request.method === 'hasSwitch' ? request.name === 'present' : request.name === 'unicode' ? 'a b=中' : '';
  });
  assert.equal(command.hasSwitch('present'), true);
  assert.equal(command.hasSwitch('absent'), false);
  assert.equal(command.getSwitchValue('unicode'), 'a b=中');
  assert.equal(command.getSwitchValue('valueless'), '');
  assert.equal(command.getSwitchValue('absent'), '');
  assert.equal(requests.length, 5);
});

test('mutation requests preserve arguments exactly and retain native failure', () => {
  const requests = [];
  const command = commandLine(request => {
    requests.push(plain(request));
    if (request.name === 'no-sandbox') throw Error('ERR_NOT_SUPPORTED: shared host setting');
    return 'ignored native mutation return';
  });
  assert.equal(command.appendSwitch('Mixed-CASE', 'a b=中'), undefined);
  assert.equal(command.appendSwitch('valueless'), undefined);
  assert.equal(command.removeSwitch('Mixed-CASE'), undefined);
  assert.equal(command.appendArgument('--still-an-argument=x y'), undefined);
  assert.deepEqual(requests, [
    {method:'appendSwitch', name:'Mixed-CASE', value:'a b=中'},
    {method:'appendSwitch', name:'valueless', value:''},
    {method:'removeSwitch', name:'Mixed-CASE'},
    {method:'appendArgument', value:'--still-an-argument=x y'},
  ]);
  assert.throws(() => command.appendSwitch('no-sandbox'), {code:'ERR_NOT_SUPPORTED'});
});

test('invalid inputs fail before IPC without conversion side effects', () => {
  let calls = 0;
  const command = commandLine(() => { ++calls; });
  const poison = {toString() { throw Error('must not convert'); }};
  for (const action of [() => command.hasSwitch(poison), () => command.getSwitchValue(null),
    () => command.appendSwitch('valid', 1), () => command.appendArgument(poison),
    () => command.removeSwitch(undefined)])
    assert.throws(action, {code:'ERR_INVALID_ARG_TYPE'});
  for (const action of [() => command.appendSwitch('name\0'),
    () => command.appendSwitch('name', 'value\0'), () => command.appendArgument('arg\0')])
    assert.throws(action, {code:'ERR_INVALID_ARG_VALUE'});
  assert.equal(calls, 0);
});

test('a missing host result never masquerades as absent switch or empty value', () => {
  for (const result of [undefined, null, 0, {}, []]) {
    const command = commandLine(() => result);
    assert.throws(() => command.hasSwitch('name'), {code:'ERR_IPC_INVALID_RESPONSE'});
    assert.throws(() => command.getSwitchValue('name'), {code:'ERR_IPC_INVALID_RESPONSE'});
  }
});
