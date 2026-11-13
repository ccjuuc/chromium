// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
'use strict';

const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function fixture(bindings = {}) {
  const context = vm.createContext({EventEmitter, ...bindings});
  vm.runInContext(readBootstrapPart('main/app_command_line.js') + '\n' +
      readBootstrapPart('main/app.js') + '\nglobalThis.api = app;', context);
  return context.api;
}

test('system language query preserves OS order independently of application locale', () => {
  const languages = ['zh-Hans-CN', 'fr-CA', 'en-US'];
  const app = fixture({__xenonBrowserWindowCall(window, command, args) {
    assert.equal(window, 0);
    assert.deepEqual(Object.keys(args), []);
    if (command === 'app-get-locale') return 'de';
    assert.equal(command, 'app-get-preferred-system-languages');
    return languages;
  }});
  assert.equal(app.getLocale(), 'de');
  const first = app.getPreferredSystemLanguages();
  assert.deepEqual(Array.from(first), languages);
  first[0] = 'application-mutated-value';
  assert.deepEqual(Array.from(app.getPreferredSystemLanguages()), languages);
  assert.notStrictEqual(app.getPreferredSystemLanguages(), languages);
});

test('C-only environments may return no system language preferences', () => {
  const app = fixture({__xenonBrowserWindowCall: () => []});
  assert.deepEqual(Array.from(app.getPreferredSystemLanguages()), []);
});

test('system language query does not silently accept malformed host responses', () => {
  for (const value of [undefined, null, 'en-US', {}, [42], [''], ['en\0US'],
    ['en-US', null], Array(1)]) {
    const app = fixture({__xenonBrowserWindowCall: () => value});
    assert.throws(() => app.getPreferredSystemLanguages(), {code: 'ERR_IPC_INVALID_RESPONSE'});
  }
});

test('host errors propagate and missing query support is explicit', () => {
  const error = Object.assign(new Error('system query failed'), {code: 'ERR_SYSTEM_ERROR'});
  const app = fixture({__xenonBrowserWindowCall() { throw error; }});
  assert.throws(() => app.getPreferredSystemLanguages(), candidate => candidate === error);
  assert.throws(() => fixture().getPreferredSystemLanguages(), {code: 'ERR_NOT_SUPPORTED'});
});
