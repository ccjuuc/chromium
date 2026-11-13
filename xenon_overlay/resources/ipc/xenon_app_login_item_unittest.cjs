// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function load(nativeCall, platform = 'darwin') {
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: platform, __xenonArch: 'arm64', __xenonEndianness: 'LE',
    __xenonOsRelease: 'fixture', __xenonAppPath: '/untrusted/embedded/Thunder.app',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: '/untrusted/embedded/Thunder.app/Contents/MacOS/Thunder',
    __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonBrowserWindowCall: nativeCall,
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      'xenon_ipc_main_bootstrap.js')), context);
  return context.__xenonElectron.app;
}

test('login item query returns fresh native state without sending application paths', () => {
  const calls = [];
  let enabled = true;
  const app = load((windowId, command, request) => {
    calls.push({windowId, command, request: JSON.parse(JSON.stringify(request))});
    return {openAtLogin: enabled, openAsHidden: true, wasOpenedAtLogin: false,
      wasOpenedAsHidden: false, restoreState: false};
  });
  assert.equal(app.getLoginItemSettings().openAtLogin, true);
  enabled = false;
  assert.equal(app.getLoginItemSettings().openAtLogin, false);
  assert.deepEqual(calls, [0, 1].map(() => ({windowId: 0, command: 'electron-api',
    request: {operation: 'app.getLoginItemSettings'}})));
  assert.equal(typeof app.setLoginItemSettings, 'function');
});

test('Windows login items use the Browser-owned context without emulated executable paths', () => {
  const calls = [];
  const app = load((windowId, command, request) => {
    calls.push({windowId, command, request: JSON.parse(JSON.stringify(request))});
    return request.method === 'get' ? {openAtLogin: false, executableWillLaunchAtLogin: true,
      launchItems: [{name: 'actual', args: ['--opened-at-login=1']}]} : null;
  }, 'win32');
  assert.equal(app.setLoginItemSettings({openAtLogin: false, args: []}), undefined);
  assert.equal(app.getLoginItemSettings().executableWillLaunchAtLogin, true);
  assert.deepEqual(calls, [
    {windowId: 0, command: 'app-login-item-settings', request: {method: 'set', options: {openAtLogin: false, args: []}}},
    {windowId: 0, command: 'app-login-item-settings', request: {method: 'get', options: {}}},
  ]);
});

test('login item setters reject invalid options and preserve native error codes', () => {
  let calls = 0;
  const app = load(() => { ++calls; throw new Error('ERR_NOT_SUPPORTED: option is unsupported'); }, 'win32');
  for (const value of [undefined, null, [], true, 'value']) {
    assert.throws(() => app.setLoginItemSettings(value), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  assert.equal(calls, 0);
  assert.throws(() => app.setLoginItemSettings({path: 'C:/other.exe'}), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(calls, 1);
  assert.throws(() => load(undefined, 'win32').setLoginItemSettings({openAtLogin: false}), {code: 'ERR_NOT_SUPPORTED'});
});

test('login item query forwards options for native validation and propagates failures', () => {
  const app = load((_windowId, _command, request) => {
    assert.deepEqual(JSON.parse(JSON.stringify(request.options)), {path: '/other.app'});
    throw new Error('ERR_NOT_SUPPORTED: caller-selected paths are unsupported');
  });
  assert.throws(() => app.getLoginItemSettings({path: '/other.app'}),
                {code: 'ERR_NOT_SUPPORTED'});
  const unavailable = load(() => { throw new Error('ERR_SYSTEM_ERROR: query failed'); });
  assert.throws(() => unavailable.getLoginItemSettings(), {code: 'ERR_SYSTEM_ERROR'});
  assert.throws(() => load().getLoginItemSettings(), {code: 'ERR_NOT_SUPPORTED'});
});
