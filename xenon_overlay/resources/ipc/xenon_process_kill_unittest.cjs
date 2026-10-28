// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function load(platform, nativeCall) {
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: platform, __xenonArch: 'arm64', __xenonEndianness: 'LE',
    __xenonOsRelease: 'fixture', __xenonAppPath: '/fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: '/fixture/host', __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonChildProcessCall: nativeCall,
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      'xenon_ipc_main_bootstrap.js')), context);
  return context.process;
}

for (const platform of ['darwin', 'win32']) {
  test(`${platform}: process.kill signal 0 forwards the exact PID and returns native success`, () => {
    const calls = [];
    const hosted = load(platform, request => {
      calls.push(JSON.parse(JSON.stringify(request)));
      return {ok: true};
    });
    assert.equal(hosted.kill(4321, 0), true);
    assert.deepEqual(calls, [{id: '@process', op: 'processProbe', pid: 4321, signal: 0}]);
  });

  test(`${platform}: missing or inaccessible PIDs retain native errors`, () => {
    for (const [code, errno] of [['ESRCH', -3], ['EPERM', -1]]) {
      const hosted = load(platform, () => ({ok: false, code, errno, message: 'native failure'}));
      assert.throws(() => hosted.kill(4321, 0), {code, errno, syscall: 'kill'});
    }
  });

  test(`${platform}: unsupported signals never reach the native bridge`, () => {
    const hosted = load(platform, () => assert.fail('must not send a signal'));
    for (const signal of [undefined, 'SIGTERM', 9, '0', null]) {
      assert.throws(() => hosted.kill(4321, signal), {code: 'ERR_NOT_SUPPORTED'});
    }
  });
}

test('process.kill validates PID before invoking the native bridge', () => {
  const hosted = load('darwin', () => assert.fail('invalid PID reached the bridge'));
  for (const pid of [undefined, null, '4321', true, {}]) {
    assert.throws(() => hosted.kill(pid, 0), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  for (const pid of [NaN, Infinity, 1.5, 2147483648, -2147483649]) {
    assert.throws(() => hosted.kill(pid, 0), {code: 'ERR_OUT_OF_RANGE'});
  }
});

test('process.kill does not report unavailable or malformed transport responses as success', () => {
  assert.throws(() => load('darwin').kill(4321, 0), {code: 'ERR_NOT_SUPPORTED'});
  for (const response of [undefined, null, {}, {ok: false}, {ok: 'true'}]) {
    assert.throws(() => load('darwin', () => response).kill(4321, 0),
                  {code: 'ERR_SYSTEM_ERROR', syscall: 'kill'});
  }
});

test('process.kill signal 0 preserves the real host process existence check', () => {
  const hosted = load(process.platform, request => {
    assert.equal(request.signal, 0);
    try {
      return {ok: process.kill(request.pid, 0)};
    } catch (error) {
      return {ok: false, code: error.code, errno: error.errno, message: error.message};
    }
  });
  assert.equal(hosted.kill(process.pid, 0), true);
});
