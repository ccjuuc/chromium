// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function load(nativeCall) {
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'darwin', __xenonArch: 'arm64', __xenonEndianness: 'LE',
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
  assert.equal(typeof app.setLoginItemSettings, 'undefined');
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
