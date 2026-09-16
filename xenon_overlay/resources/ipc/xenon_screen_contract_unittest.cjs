// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createRuntime(host) {
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10.0',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture',
    __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  if (host) context.__xenonBrowserWindowCall = host;
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readFileSync(filename, 'utf8'), context, {filename});
  return context.__xenonElectron.screen;
}

test('screen queries use live host coordinates and display data without requiring a window', () => {
  const calls = [];
  let cursor = {x: -1250, y: 340};
  const displays = [
    {id: 10, scaleFactor: 1.5, bounds: {x: 0, y: 0, width: 1706, height: 960},
      workArea: {x: 0, y: 0, width: 1706, height: 920}},
    {id: 11, scaleFactor: 1, bounds: {x: -1920, y: 0, width: 1920, height: 1080},
      workArea: {x: -1920, y: 0, width: 1920, height: 1040}},
  ];
  const screen = createRuntime((id, command, details) => {
    calls.push({id, command, details: JSON.parse(JSON.stringify(details))});
    switch (details.method) {
      case 'getCursorScreenPoint': return {...cursor};
      case 'getPrimaryDisplay': return displays[0];
      case 'getAllDisplays': return displays;
      case 'getDisplayMatching': return displays[1];
      case 'getDisplayNearestPoint': return displays[1];
      default: throw new Error('Unexpected screen request');
    }
  });
  assert.deepEqual(screen.getCursorScreenPoint(), cursor);
  cursor = {x: -1170, y: 400};
  assert.deepEqual(screen.getCursorScreenPoint(), cursor);
  assert.equal(screen.getPrimaryDisplay(), displays[0]);
  assert.equal(screen.getAllDisplays(), displays);
  const rect = {x: -1400, y: 100, width: 1080, height: 607};
  const point = {x: -1300, y: 120};
  assert.equal(screen.getDisplayMatching(rect), displays[1]);
  assert.equal(screen.getDisplayNearestPoint(point), displays[1]);
  assert.deepEqual(calls, [
    {id: 0, command: 'screen', details: {method: 'getCursorScreenPoint'}},
    {id: 0, command: 'screen', details: {method: 'getCursorScreenPoint'}},
    {id: 0, command: 'screen', details: {method: 'getPrimaryDisplay'}},
    {id: 0, command: 'screen', details: {method: 'getAllDisplays'}},
    {id: 0, command: 'screen', details: {method: 'getDisplayMatching', rect}},
    {id: 0, command: 'screen', details: {method: 'getDisplayNearestPoint', point}},
  ]);
});

test('screen propagates host failures instead of returning fabricated coordinates', () => {
  const failure = new TypeError('screen host rejected coordinates');
  const screen = createRuntime(() => { throw failure; });
  assert.throws(() => screen.getDisplayMatching({x: NaN}), error => error === failure);
  const unsupported = createRuntime();
  assert.throws(() => unsupported.getCursorScreenPoint(), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => unsupported.getAllDisplays(), {code: 'ERR_NOT_SUPPORTED'});
});
