// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function fixture(host = () => false, metadata = {}) {
  const calls = [];
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '10.0', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonWorkingDirectory: 'C:\\fixture\\work', __xenonResourcesPath: 'C:\\fixture',
    __xenonIsPackaged: false, __xenonChromeVersion: '142', __xenonV8Version: '',
    __xenonGetPath: () => '', __xenonExitApp() {},
    __xenonBrowserWindowCall(id, command, details) {
      assert.equal(id, 0);
      assert.equal(command, 'app-single-instance');
      calls.push(details);
      return host(details);
    },
    ...metadata,
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')), context);
  return {context, calls, app: context.__xenonElectron.app,
    notify: details => context.__xenonDispatchBrowserWindowEvent(
        0, '__xenon-second-instance', details)};
}

test('single-instance methods query native ownership and send only additionalData', () => {
  let locked = false;
  const {context, calls, app} = fixture(({method}) => {
    if (method === 'request') locked = true;
    if (method === 'release') locked = false;
    return locked;
  });
  assert.equal(app.hasSingleInstanceLock(), false);
  const additionalData = {argv: ['original exe', '文档 名称.md'], nested: {flag: true}};
  assert.equal(app.requestSingleInstanceLock(additionalData), true);
  assert.equal(app.hasSingleInstanceLock(), true);
  const request = calls.find(call => call.method === 'request');
  assert.equal(Object.hasOwn(request, 'argv'), false);
  assert.equal(Object.hasOwn(request, 'cwd'), false);
  assert.deepEqual(JSON.parse(JSON.stringify(request.additionalData)), additionalData);
  additionalData.nested.flag = false;
  assert.equal(request.additionalData.nested.flag, true);
  assert.equal(app.releaseSingleInstanceLock(), undefined);
  assert.equal(app.hasSingleInstanceLock(), false);
  assert.equal(app.requestSingleInstanceLock(), true);
  assert.deepEqual(JSON.parse(JSON.stringify(calls.at(-1).additionalData)), {});
});

test('secondary result and native errors are preserved without a cached JS lock', () => {
  const {app} = fixture(() => false);
  assert.equal(app.requestSingleInstanceLock({}), false);
  assert.equal(app.hasSingleInstanceLock(), false);
  const failing = fixture(() => {
    throw new Error('ERR_SINGLE_INSTANCE_UNAVAILABLE: primary did not acknowledge');
  });
  assert.throws(() => failing.app.requestSingleInstanceLock(),
    {code: 'ERR_SINGLE_INSTANCE_UNAVAILABLE'});
  for (const invalid of [null, undefined, 1, 'true', {}]) {
    const bad = fixture(() => invalid);
    assert.throws(() => bad.app.requestSingleInstanceLock(), {code: 'ERR_IPC_INVALID_RESPONSE'});
    assert.throws(() => bad.app.hasSingleInstanceLock(), {code: 'ERR_IPC_INVALID_RESPONSE'});
  }
});

test('mutating process.argv does not replace native launch arguments', () => {
  const supplied = ['C:\\host.exe', '--new-window', '文档 名称.md', '', '--', '--literal'];
  const {context, app, calls} = fixture(() => false, {__xenonArgv: supplied});
  assert.deepEqual(Array.from(context.process.argv), supplied);
  context.process.argv.unshift(null);
  context.process.argv.push('runtime-change');
  assert.equal(supplied.length, 6);
  app.requestSingleInstanceLock({argv: context.process.argv});
  assert.equal(Object.hasOwn(calls[0], 'argv'), false);
  assert.deepEqual(Array.from(calls[0].additionalData.argv), [null, ...supplied, 'runtime-change']);
  context.process.argv = null;
  context.process.cwd = () => { throw new Error('JS cwd must not be consulted'); };
  assert.equal(app.requestSingleInstanceLock(), false);
  assert.deepEqual(Object.keys(calls[1]).sort(), ['additionalData', 'method']);
});

test('only bounded JSON objects cross native IPC; invalid data fails before calling host', () => {
  const {app, calls} = fixture();
  for (const value of [null, [], 'text', 1, true]) {
    assert.throws(() => app.requestSingleInstanceLock(value), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  const cyclic = {}; cyclic.self = cyclic;
  assert.throws(() => app.requestSingleInstanceLock(cyclic), {code: 'ERR_INVALID_ARG_VALUE'});
  for (const invalid of [undefined, () => {}, Symbol('value'), 1n, new Date(), new Map()]) {
    assert.throws(() => app.requestSingleInstanceLock({invalid}), {code: 'ERR_NOT_SUPPORTED'});
  }
  for (const invalid of [NaN, Infinity, -Infinity]) {
    assert.throws(() => app.requestSingleInstanceLock({invalid}), {code: 'ERR_INVALID_ARG_VALUE'});
  }
  const getter = {get value() { throw new Error('getter must not execute'); }};
  assert.throws(() => app.requestSingleInstanceLock(getter), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => app.requestSingleInstanceLock({[Symbol('key')]: 1}), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => app.requestSingleInstanceLock({large: '😀'.repeat(300000)}), {code: 'ERR_OUT_OF_RANGE'});
  let deep = {}; for (let i = 0; i < 64; ++i) deep = {deep};
  assert.throws(() => app.requestSingleInstanceLock(deep), {code: 'ERR_OUT_OF_RANGE'});
  assert.equal(calls.length, 0);
});

test('JSON snapshots retain special own keys and shared references without prototype mutation', () => {
  const {app, calls} = fixture();
  const shared = {value: 'safe'};
  const payload = JSON.parse('{"__proto__":{"value":"own data"},"constructor":"ordinary"}');
  payload.first = shared;
  payload.second = shared;
  payload.list = [null, false, 42, 'value'];
  app.requestSingleInstanceLock(payload);
  const actual = calls[0].additionalData;
  assert.equal(Object.hasOwn(actual, '__proto__'), true);
  assert.equal(actual.__proto__.value, 'own data');
  assert.equal(actual.constructor, 'ordinary');
  assert.notEqual(actual.first, actual.second);
  assert.deepEqual(JSON.parse(JSON.stringify(actual)), payload);
});

test('second-instance waits for ready and whenReady handlers before delivery', async () => {
  const {context, app, notify} = fixture();
  const events = [];
  app.on('ready', () => events.push('ready'));
  app.whenReady().then(() => {
    events.push('whenReady');
    app.on('second-instance', (event, argv, cwd, additionalData) => {
      assert.equal(event.sender, app);
      events.push({argv, cwd, additionalData});
    });
  });
  const details = {argv: ['host', 'relative.md'], workingDirectory: 'C:\\docs',
    additionalData: {argv: ['exact', 'relative.md']}};
  notify(details);
  assert.equal(events.length, 0);
  context.__xenonMarkAppReady();
  await Promise.resolve();
  await Promise.resolve();
  assert.deepEqual(events.slice(0, 2), ['ready', 'whenReady']);
  assert.deepEqual(events[2], {argv: details.argv, cwd: details.workingDirectory,
    additionalData: details.additionalData});
  notify({...details, argv: ['third']});
  assert.deepEqual(events.at(-1).argv, ['third']);
});

test('shutdown drops queued and future second-instance events', async () => {
  const {context, app, notify} = fixture();
  let delivered = 0;
  app.on('second-instance', () => delivered++);
  const details = {argv: ['host'], workingDirectory: 'C:\\docs', additionalData: {}};
  notify(details);
  context.__xenonShutdownApp();
  context.__xenonMarkAppReady();
  notify(details);
  await Promise.resolve();
  assert.equal(delivered, 0);
});
