// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');

function createRuntimes() {
  const calls = [];
  const configuration = {autoDownload: true, autoInstallOnAppQuit: true};
  function nativeApi(request) {
    calls.push({...request});
    if (request.operation === 'autoUpdater.configure') {
      for (const key of Object.keys(configuration)) {
        if (key in request && typeof request[key] !== 'boolean') {
          throw new Error(`ERR_INVALID_ARG_TYPE: ${key} must be a boolean`);
        }
      }
      for (const key of Object.keys(configuration)) {
        if (key in request) configuration[key] = request[key];
      }
    }
    if (['autoUpdater.configure', 'autoUpdater.getConfiguration'].includes(request.operation)) {
      return {...configuration};
    }
    return {status: 'checking'};
  }
  const common = {
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
  };
  let rendererDispatch;
  const rendererContext = vm.createContext({
    ...common,
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe',
        cwd: 'C:\\fixture', contextIsolated: false, sandboxed: true, isPackaged: true}),
      setDispatchHandler(handler) { rendererDispatch = handler; },
      sendSync(channel, request) {
        if (channel === '__xenon:electron-api') return nativeApi(request);
        if (channel === '__xenon:fs') return false;
        throw new Error(`Unexpected channel ${channel}`);
      },
      invoke: async (channel, request) => {
        assert.equal(channel, '__xenon:electron-api');
        return nativeApi(request);
      },
    },
  });
  vm.runInContext(readBootstrapPart('xenon_ipc_renderer_bootstrap.js'), rendererContext);
  const deliveries = [];
  const mainContext = vm.createContext({
    ...common,
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '10.0', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonWorkingDirectory: 'C:\\fixture', __xenonResourcesPath: 'C:\\fixture',
    __xenonIsPackaged: false, __xenonChromeVersion: '153', __xenonV8Version: '',
    __xenonGetPath: () => '',
    __xenonBrowserWindowCall(id, command, request) {
      assert.equal(id, 0);
      assert.equal(command, 'electron-api');
      return nativeApi(request);
    },
    __xenonSendToRenderer(endpoint, channel, args) {
      deliveries.push({endpoint, channel, args});
      rendererDispatch(channel, args);
    },
  });
  vm.runInContext(readBootstrapPart('xenon_ipc_main_bootstrap.js'), mainContext);
  const sender = {endpointId: 'fixture-document', windowId: 0,
    processId: 7, frameId: 1, isMainFrame: true};
  mainContext.__xenonDispatchRendererEvent(sender, true);
  return {
    calls, configuration, deliveries,
    main: mainContext.__xenonElectron.autoUpdater,
    renderer: rendererContext.require('electron').autoUpdater,
    detach: () => mainContext.__xenonDispatchRendererEvent(sender, false),
    emit: (event, ...args) => mainContext.__xenonDispatchBrowserWindowEvent(
        0, 'auto-updater', {event, args}),
  };
}

test('main and renderer updater properties read and synchronously update native configuration', () => {
  const {main, renderer, configuration} = createRuntimes();
  assert.equal(main.autoDownload, true);
  renderer.autoDownload = false;
  assert.equal(configuration.autoDownload, false);
  assert.equal(main.autoDownload, false);
  main.autoInstallOnAppQuit = false;
  assert.equal(renderer.autoInstallOnAppQuit, false);
  assert.throws(() => { renderer.autoDownload = 'false'; }, {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(configuration.autoDownload, false);
});

test('native updater events reach main and renderer once without synthetic checking events', async () => {
  const runtime = createRuntimes();
  const mainEvents = [];
  const rendererEvents = [];
  const names = ['checking-for-update', 'update-available', 'download-progress',
    'update-downloaded', 'update-not-available'];
  for (const name of names) {
    runtime.main.on(name, (...args) => mainEvents.push({name, args}));
    runtime.renderer.on(name, (...args) => rendererEvents.push({name, args}));
  }
  await runtime.main.checkForUpdates();
  await runtime.renderer.checkForUpdates();
  assert.equal(mainEvents.length, 0);
  assert.equal(rendererEvents.length, 0);
  for (const name of names) runtime.emit(name, {version: '2.0.0.0'});
  assert.deepEqual(mainEvents, rendererEvents);
  assert.deepEqual(mainEvents.map(event => event.name), names);
  assert.equal(runtime.deliveries.length, names.length);
  runtime.detach();
  runtime.emit('update-available', {version: '3.0.0.0'});
  assert.equal(mainEvents.length, names.length + 1);
  assert.equal(rendererEvents.length, names.length);
});

test('native errors become Error objects and do not break containers without updater listeners', () => {
  const runtime = createRuntimes();
  assert.doesNotThrow(() => runtime.emit('error', 'no updater configured'));
  let mainError;
  let rendererError;
  runtime.main.once('error', error => { mainError = error; });
  runtime.renderer.once('error', error => { rendererError = error; });
  runtime.emit('error', 'hash mismatch');
  assert.equal(mainError.message, 'hash mismatch');
  assert.equal(rendererError.message, 'hash mismatch');
  assert.equal(mainError.name, 'Error');
  assert.equal(rendererError.name, 'Error');
});
