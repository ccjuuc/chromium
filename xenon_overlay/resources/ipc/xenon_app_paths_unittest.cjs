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

test('path arguments reject coercion and NUL before reaching native code', () => {
  let calls = 0;
  const app = fixture({__xenonGetPath() { calls++; },
    __xenonSetPath() { calls++; }, __xenonSetAppLogsPath() { calls++; }});
  for (const value of [undefined, null, 1, {}, [], {toString() { throw new Error('coercion'); }}]) {
    assert.throws(() => app.getPath(value), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => app.setPath(value, '/path'), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => app.setPath('logs', value), {code: 'ERR_INVALID_ARG_TYPE'});
    if (value !== undefined)
      assert.throws(() => app.setAppLogsPath(value), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  for (const call of [() => app.getPath('home\0'),
    () => app.setPath('logs\0', '/path'), () => app.setPath('logs', '/path\0suffix'),
    () => app.setAppLogsPath('/path\0suffix')]) {
    assert.throws(call, {code: 'ERR_INVALID_ARG_VALUE'});
  }
  assert.equal(calls, 0);
});

test('log path default is distinguished from a supplied path and setters return void', () => {
  const calls = [];
  const app = fixture({
    __xenonSetPath(...args) { calls.push(args); return 'not exposed'; },
    __xenonSetAppLogsPath(...args) { calls.push(args); return 'not exposed'; },
  });
  assert.equal(app.setPath('downloads', '/file system/未创建'), undefined);
  assert.equal(app.setAppLogsPath(), undefined);
  assert.equal(app.setAppLogsPath(undefined), undefined);
  assert.equal(app.setAppLogsPath('/logs'), undefined);
  assert.deepEqual(calls, [['downloads', '/file system/未创建'], [], [], ['/logs']]);
});

test('native failures propagate without changing a local shadow path', () => {
  const error = Object.assign(new Error('host owns this directory'), {code: 'ERR_NOT_SUPPORTED'});
  const app = fixture({__xenonGetPath: () => '/real-host-profile',
    __xenonSetPath() { throw error; }, __xenonSetAppLogsPath() { throw error; }});
  assert.throws(() => app.setPath('userData', '/different'), candidate => candidate === error);
  assert.equal(app.getPath('userData'), '/real-host-profile');
  assert.throws(() => app.setAppLogsPath('/different'), candidate => candidate === error);
});

test('missing setters report unsupported rather than silently accepting a path', () => {
  const app = fixture();
  assert.throws(() => app.setPath('logs', '/logs'), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => app.setAppLogsPath(), {code: 'ERR_NOT_SUPPORTED'});
});
