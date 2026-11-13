// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const {types} = require('node:util');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

const bootstrapPath = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const source = readBootstrap(bootstrapPath);
const privilegedGlobals = ['require', 'process', 'Buffer', 'global', '__filename',
  '__dirname', 'xenonIpcRenderer', '__xenonElectronRequire', '__xenonElectronIpc',
  '__xenonPaths', 'electron', '__xenonPageHandler__', '__xenonPageCallbackRouter__',
  '__xenonPageHandlerBound__'];

function createPage({isGuest = false, contextIsolated = false,
                     preferences = {contextIsolation: false, nodeIntegration: false},
                     sandboxed = false, preload = ''} = {}) {
  const sent = [];
  const appPath = 'C:\\isolated-fixture';
  const transport = {
    isPromise(value) {
      assert.equal(this, transport);
      return types.isPromise(value);
    },
    isNativeError(value) {
      assert.equal(this, transport);
      return types.isNativeError(value);
    },
    getRuntimeConfig: () => ({appPath, exeDir: appPath, cwd: appPath,
      execPath: appPath + '\\host.exe', platform: 'win32', arch: 'x64',
      contextIsolated, isGuest, isMainFrame: true, sandboxed, webPreferences: preferences,
      sessionPreloads: preload ? [{filePath: appPath + '\\preload.cjs', source: preload}] : []}),
    setDispatchHandler() {},
    send(...args) { sent.push(args); },
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat') {
        if (request.path === appPath) return {isFile: false, isDirectory: true};
        if (request.path === appPath + '\\data.json') return {isFile: true, isDirectory: false};
        throw Object.assign(new Error('Missing fixture file'), {code: 'ENOENT'});
      }
      if (request.operation === 'read_file') return Buffer.from('{"answer":42}').toString('base64');
      throw new Error('Unexpected filesystem operation: ' + request.operation);
    },
  };
  const context = vm.createContext({xenonIpcRenderer: transport,
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    location: {protocol: 'file:', hostname: '', pathname: '/C:/isolated-fixture/index.html'},
    document: {readyState: 'loading', addEventListener() {}},
    console: {log() {}, warn() {}, error() {}},
  });
  context.window = context;
  vm.runInContext(source, context, {filename: bootstrapPath});
  return {context, sent};
}

for (const isGuest of [false, true]) {
  const kind = isGuest ? 'guest' : 'BrowserWindow';
  test(`${kind}: disabled Node integration removes all automatic privileged aliases`, () => {
    const {context} = createPage({isGuest});
    for (const key of privilegedGlobals) assert.equal(context[key], undefined, key);
  });

  test(`${kind}: preload closures retain require, Buffer, process and IPC after page cleanup`, () => {
    const {context, sent} = createPage({isGuest, preload: `
      const ipc = require('electron').ipcRenderer;
      globalThis.preloadProbe = () => {
        const answer = require('./data.json').answer;
        ipc.send('fixture:probe', answer);
        return {answer, type: process.type, byte: Buffer.from([19])[0]};
      };
    `});
    for (const key of privilegedGlobals) assert.equal(context[key], undefined, key);
    assert.deepEqual({...context.preloadProbe()}, {answer: 42, type: 'renderer', byte: 19});
    assert.equal(sent.length, 1);
    assert.equal(sent[0][0], 'fixture:probe');
    assert.equal(sent[0][1], 42);
  });

  test(`${kind}: preload util keeps local brands and deep equality after cleanup`, () => {
    const {context} = createPage({isGuest, preload: `
      const util = require('node:util');
      globalThis.preloadUtil = (externalPromise, externalError) => {
        const fakePromise = Object.create(Promise.prototype);
        const fakeError = Object.create(Error.prototype);
        const thenable = {get then() { throw new Error('must not inspect then'); }};
        const first = {items: new Set([{value: 1}, {value: 2}])};
        const second = {items: new Set([{value: 2}, {value: 1}])};
        first.self = first;
        second.self = second;
        return {
          sameModule: util === require('util'),
          externalPromise: util.types.isPromise(externalPromise),
          externalError: util.types.isNativeError(externalError),
          fakePromise: util.types.isPromise(fakePromise),
          fakeError: util.types.isNativeError(fakeError),
          thenable: util.types.isPromise(thenable),
          missingPromise: util.types.isPromise(),
          missingError: util.types.isNativeError(),
          cyclic: util.isDeepStrictEqual(first, second),
          fakePromisesEqual: util.isDeepStrictEqual(fakePromise, Object.create(Promise.prototype)),
          distinctPromisesEqual: util.isDeepStrictEqual(Promise.resolve(1), Promise.resolve(1)),
        };
      };
    `});
    for (const key of privilegedGlobals) assert.equal(context[key], undefined, key);
    assert.deepEqual({...context.preloadUtil(Promise.resolve(1), new Error('outside'))}, {
      sameModule: true, externalPromise: true, externalError: true,
      fakePromise: false, fakeError: false, thenable: false,
      missingPromise: false, missingError: false, cyclic: true,
      fakePromisesEqual: true, distinctPromisesEqual: false,
    });
    for (const key of privilegedGlobals) assert.equal(context[key], undefined, key);
  });

  test(`${kind}: explicit Node integration retains require and Electron IPC`, () => {
    const {context} = createPage({isGuest,
      preferences: {contextIsolation: false, nodeIntegration: true}});
    assert.equal(typeof context.require, 'function');
    assert.equal(context.require('electron'), context.electron);
    assert.equal(context.process.type, 'renderer');
    assert.equal(typeof context.xenonIpcRenderer.send, 'function');
  });

  test(`${kind}: delayed preload OS calls use private process state after cleanup`, async () => {
    const {context} = createPage({isGuest, preload: `
      globalThis.preloadOs = (home, temp) => {
        process.env.USERPROFILE = home;
        process.env.TEMP = temp;
        const os = require('os');
        return {home: os.homedir(), temp: os.tmpdir()};
      };
    `});
    await Promise.resolve();
    for (const key of privilegedGlobals) assert.equal(context[key], undefined, key);
    for (const suffix of ['first', 'changed']) {
      const home = 'C:/fixture/home-' + suffix;
      const temp = 'C:/fixture/temp-' + suffix;
      assert.deepEqual({...context.preloadOs(home, temp)}, {home, temp});
    }
    assert.equal(context.process, undefined, 'OS calls must not republish Node globals');
  });
}

test('isolated preload world retains its private Node environment', () => {
  const {context} = createPage({contextIsolated: true,
    preferences: {contextIsolation: true, nodeIntegration: false}});
  assert.equal(typeof context.require, 'function');
  assert.equal(context.process.contextIsolated, true);
});

test('preload can expose an application API named electron without retaining automatic IPC', () => {
  const {context} = createPage({preload: 'globalThis.electron = {readVersion: () => "fixture"};'});
  assert.equal(context.electron.readVersion(), 'fixture');
  for (const key of privilegedGlobals.filter(key => key !== 'electron'))
    assert.equal(context[key], undefined, key);
});

test('origin-authorized WebUI without BrowserWindow preferences retains its IPC contract', () => {
  const {context} = createPage({preferences: {}});
  assert.equal(typeof context.require, 'function');
  assert.equal(context.require('electron'), context.electron);
});

for (const [name, preferences, contextIsolated, sandboxed] of [
  ['sandboxed preload', {sandbox: true, contextIsolation: true, nodeIntegration: false}, true, true],
  ['isolated preload', {sandbox: false, contextIsolation: true, nodeIntegration: false}, true, false],
  ['preload without Node integration', {sandbox: false, contextIsolation: false, nodeIntegration: false}, false, false],
  ['Node-enabled page', {sandbox: false, contextIsolation: false, nodeIntegration: true}, false, false],
]) {
  test(`${name}: additionalArguments reaches preload unchanged and stays local to its window`, () => {
    const args = ['--fixture-config=ipc://window/42', 'space in one argument', '',
      '--language=中文', '--duplicate', '--duplicate'];
    const config = {...preferences, additionalArguments: [...args]};
    const preload = `
      globalThis.readArguments = () => [...process.argv];
      globalThis.appendArgument = value => process.argv.push(value);
    `;
    const {context} = createPage({preferences: config, contextIsolated, sandboxed, preload});
    assert.equal(typeof context.readArguments, 'function', 'preload must execute');
    assert.deepEqual(Array.from(context.readArguments()), ['C:\\isolated-fixture\\host.exe', ...args]);
    context.appendArgument('preload-local');
    assert.deepEqual(config.additionalArguments, args, 'preload must not change Browser preferences');
    config.additionalArguments.push('changed-after-startup');
    assert.deepEqual(Array.from(context.readArguments()), ['C:\\isolated-fixture\\host.exe', ...args, 'preload-local']);

    const second = createPage({preferences: {...preferences, additionalArguments: ['--window=second']},
      contextIsolated, sandboxed, preload});
    assert.deepEqual(Array.from(second.context.readArguments()), ['C:\\isolated-fixture\\host.exe', '--window=second']);
    if (sandboxed || !contextIsolated && !preferences.nodeIntegration) {
      for (const key of privilegedGlobals) assert.equal(context[key], undefined, key);
    }
  });
}

test('renderer additionalArguments accepts only supplied string array entries', () => {
  for (const additionalArguments of [undefined, null, '--not-an-array', [null, 42, {}, '--kept']]) {
    const {context} = createPage({preferences: {sandbox: false, contextIsolation: false,
      nodeIntegration: true, additionalArguments}});
    assert.deepEqual(Array.from(context.process.argv), ['C:\\isolated-fixture\\host.exe',
      ...(Array.isArray(additionalArguments) ? ['--kept'] : [])]);
  }
});
