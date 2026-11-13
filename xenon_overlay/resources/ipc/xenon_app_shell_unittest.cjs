// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
'use strict';
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
function fixture(host) {
  const context = vm.createContext({EventEmitter, __xenonBrowserWindowCall: host});
  vm.runInContext(readBootstrapPart('main/app_command_line.js') + '\n' +
    readBootstrapPart('main/app.js') + '\nglobalThis.api = app;', context);
  return context.api;
}
test('JumpList preserves native failure statuses and null clearing', () => {
  const calls = [];
  const app = fixture((id, command, args) => {
    calls.push([id, command, args.categories]);
    return args.categories === null ? 'ok' : 'customCategoryAccessDenied';
  });
  const items = [{name:'Recent',items:[{type:'file',path:'C:/document.txt'}]}];
  assert.equal(app.setJumpList(items), 'customCategoryAccessDenied');
  assert.equal(app.setJumpList(null), 'ok');
  assert.deepEqual(calls, [[0,'app-set-jump-list',items],[0,'app-set-jump-list',null]]);
  for (const value of [undefined, {}, 1, 'list'])
    assert.throws(() => app.setJumpList(value), {code:'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls.length, 2);
});
test('AppUserModelID validates strings before updating the native window identity', () => {
  const calls = [];
  const app = fixture((...args) => calls.push(args));
  assert.equal(app.setAppUserModelId('com.example.editor'), undefined);
  assert.equal(calls[0][1], 'app-set-user-model-id');
  assert.equal(calls[0][2].id, 'com.example.editor');
  for (const value of ['', 'x'.repeat(129), 'id\0'])
    assert.throws(() => app.setAppUserModelId(value), {code:'ERR_INVALID_ARG_VALUE'});
  assert.throws(() => app.setAppUserModelId(42), {code:'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls.length, 1);
});
