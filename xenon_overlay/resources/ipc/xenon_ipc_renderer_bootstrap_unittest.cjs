// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run with: node --test xenon_overlay/resources/ipc/xenon_ipc_renderer_bootstrap_unittest.cjs
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const bootstrapPath = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const bootstrapSource = readBootstrap(bootstrapPath);
const addonPath = 'C:\\test-app\\fixture.node';

// Exercise the complete production bootstrap through require(), replacing
// only its native transport. No application modules or network are involved.
function createRenderer(exportsList = [], overrides = {}, withWindow = false, initializeContext) {
  const calls = [];
  let dispatchHandler;
  const transport = {
    getRuntimeConfig: () => ({
      appPath: 'C:\\test-app',
      exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe',
      cwd: 'C:\\test-app',
      contextIsolated: false,
      sandboxed: true,
      isPackaged: true,
    }),
    setDispatchHandler(handler) { dispatchHandler = handler; },
    sendSync(channel, options) {
      assert.equal(channel, '__xenon:fs');
      if (options.operation === 'realpath') return options.path;
      if (options.operation === 'stat') {
        if (options.path === addonPath) return {isFile: true, isDirectory: false};
        throw new Error('ENOENT: no such file or directory');
      }
      assert.equal(options.operation, 'exists');
      return options.path === addonPath;
    },
    requireNodeModuleSync(modulePath) {
      calls.push(['require', modulePath]);
      return exportsList;
    },
    invokeNodeExportSync(modulePath, exportPath, ...args) {
      calls.push(['invoke', modulePath, exportPath, ...args]);
      return 42;
    },
    constructNodeExportSync(modulePath, exportPath, ...args) {
      calls.push(['construct', modulePath, exportPath, ...args]);
      return 7;
    },
    ...overrides,
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    xenonIpcRenderer: transport,
    TextEncoder,
    TextDecoder,
    URL,
    URLSearchParams,
    queueMicrotask,
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    atob,
    btoa,
    location: {
      protocol: 'chrome:',
      hostname: 'xenon-player-electron',
      search: '',
    },
    console: {log() {}, warn() {}, error() {}},
  });
  if (withWindow) {
    context.window = context;
    context.document = {readyState: 'loading', addEventListener() {}};
  }
  if (initializeContext) initializeContext(context);
  vm.runInContext(bootstrapSource, context, {filename: bootstrapPath});
  return {
    calls,
    context,
    dispatch: (channel, args) => dispatchHandler(channel, args),
    load: () => context.require(addonPath),
  };
}

test('renderer process and app identity use native document metadata', () => {
  const config = {
    appPath: '/fixture/app', execPath: '/fixture/host', cwd: '/fixture/work',
    platform: 'linux', arch: 'arm64', pid: 4817, contextId: 'fixture-document-2',
    versions: {chrome: '142.0.fixture', v8: '14.2.fixture',
      node: '0.0.0-compat', electron: '0.0.0-compat'},
    contextIsolated: false, sandboxed: true, isMainFrame: false, isPackaged: false,
  };
  const {context} = createRenderer([], {getRuntimeConfig: () => config});
  const process = context.require('process');
  for (const key of ['platform', 'arch', 'pid', 'contextId',
                     'contextIsolated', 'sandboxed', 'isMainFrame']) {
    assert.equal(process[key], config[key], key);
  }
  assert.deepEqual({...process.versions}, config.versions);
  assert.equal(process.version, 'v0.0.0-compat');
  assert.equal(process.cwd(), config.cwd);
  assert.equal(context.require('electron').app.isPackaged, false);
  assert.equal(context.__filename, '/fixture/app/index.js');
  assert.equal(context.__dirname, '/fixture/app');
});

test('missing runtime metadata never invents a Node version, PID or working directory', () => {
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\fixture'}),
  });
  assert.deepEqual({...context.process.versions}, {});
  for (const key of ['version', 'pid', 'contextId', 'contextIsolated', 'sandboxed'])
    assert.equal(context.process[key], undefined, key);
  assert.throws(() => context.process.cwd(), {code: 'ERR_NOT_SUPPORTED'});
});

test('bootstrap preserves global descriptors and application RPC method identity', () => {
  const {context} = createRenderer([], {}, false, context => {
    vm.runInContext('globalThis.originalDefineProperty = Object.defineProperty', context);
  });
  assert.equal(vm.runInContext('Object.defineProperty === originalDefineProperty', context), true);
  assert.equal(vm.runInContext("Object.hasOwn(Object.prototype, 'callRemoteClientFunction')", context), false);
  vm.runInContext(`
    globalThis.rpcCalls = [];
    globalThis.originalRpc = function(...args) {
      rpcCalls.push({receiver: this, args});
      return args;
    };
    globalThis.assignedRpc = {callRemoteClientFunction: originalRpc};
    globalThis.descriptorRpc = {};
    globalThis.rpcGetter = () => originalRpc;
    Object.defineProperty(descriptorRpc, 'callRemoteClientFunction', {
      get: rpcGetter, enumerable: true, configurable: false,
    });
    for (const receiver of [assignedRpc, descriptorRpc]) {
      for (const name of ['openElectronSelectFileDialog', 'showOpenDialog', 'unrelated']) {
        const args = ['fixture-main', name, {title: 'fixture'}];
        const result = receiver.callRemoteClientFunction(...args);
        if (result[2] !== args[2]) throw new Error('RPC return value changed');
      }
    }
  `, context);
  assert.equal(context.assignedRpc.callRemoteClientFunction, context.originalRpc);
  assert.equal(context.descriptorRpc.callRemoteClientFunction, context.originalRpc);
  assert.equal(Object.getOwnPropertyDescriptor(context.descriptorRpc,
      'callRemoteClientFunction').get, context.rpcGetter);
  assert.equal(context.rpcCalls.length, 6);
  for (let index = 0; index < 6; ++index)
    assert.equal(context.rpcCalls[index].receiver,
        index < 3 ? context.assignedRpc : context.descriptorRpc);
});

function createPreloadRenderer(preload, files, extraPreferences = {}) {
  const sent = [];
  const renderer = createRenderer([], {
    sendSync(channel, request) {
      if (channel === '__xenon:renderer-web-preferences') {
        return {preload, contextIsolation: false, ...extraPreferences};
      }
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat') {
        if (files.has(request.path)) return {isFile: true, isDirectory: false};
        if ([...files.keys()].some(name => name.startsWith(request.path + '\\'))) {
          return {isFile: false, isDirectory: true};
        }
        throw new Error('ENOENT: no such file or directory');
      }
      assert.equal(request.operation, 'read_file');
      assert.ok(files.has(request.path));
      return Buffer.from(files.get(request.path)).toString('base64');
    },
    send(...args) { sent.push(args); },
  }, true);
  return {...renderer, sent};
}

test('document preload runs before application code and once per document', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  const files = new Map([[preload, `
    globalThis.preloadRuns = (globalThis.preloadRuns || 0) + 1;
    globalThis.fixtureAPI = {send: () => require('electron').ipcRenderer.send('fixture:click')};
    globalThis.preloadLocation = [__filename, __dirname, process.contextIsolated];
  `]]);
  for (let document = 0; document < 2; ++document) {
    const {context, sent} = createPreloadRenderer(preload, files);
    assert.equal(context.preloadRuns, 1);
    assert.deepEqual(Array.from(context.preloadLocation),
        [preload, 'C:\\test-app\\preload', false]);
    context.fixtureAPI.send();
    assert.deepEqual(sent, [['fixture:click']]);
    vm.runInContext(bootstrapSource, context);
    assert.equal(context.preloadRuns, 1);
  }
});

test('preload CommonJS dependencies remain relative after evaluation and in ASAR paths', () => {
  for (const root of ['C:\\test-app', 'C:\\test-app\\resources.asar']) {
    const preload = root + '\\preload\\entry.cjs';
    const files = new Map([
      [preload, `globalThis.readFixture = require('./nested/module.js');`],
      [root + '\\preload\\nested\\module.js', `module.exports = () => require('../data.json').value;`],
      [root + '\\preload\\data.json', '{"value":42}'],
    ]);
    const {context, sent} = createPreloadRenderer(preload, files);
    assert.deepEqual(sent, []);
    assert.equal(context.readFixture(), 42);
  }
});

test('missing and throwing preloads report a real preload-error without aborting bootstrap', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  for (const files of [new Map(), new Map([[preload, `throw new Error('fixture failure');`]])]) {
    const {context, sent} = createPreloadRenderer(preload, files);
    assert.equal(typeof context.require('electron').ipcRenderer.send, 'function');
    assert.equal(sent.length, 1);
    assert.equal(sent[0][0], '__xenon:preload-error');
    assert.equal(sent[0][1], preload);
    assert.match(sent[0][2], /Cannot find module|fixture failure/);
  }
});

test('isolated preload is never silently executed in the main world', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  const {context, sent} = createPreloadRenderer(
      preload, new Map([[preload, 'globalThis.leaked = true;']]),
      {contextIsolation: true});
  assert.equal(context.leaked, undefined);
  assert.match(sent[0][2], /Isolated-world preloads/);
});

test('a preload exporting undefined still evaluates successfully and is cached', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  const {context, sent} = createPreloadRenderer(preload, new Map([[preload,
    'globalThis.preloadRuns = (globalThis.preloadRuns || 0) + 1; module.exports = undefined;',
  ]]));
  assert.deepEqual(sent, []);
  assert.equal(context.require(preload), undefined);
  assert.equal(context.preloadRuns, 1);
});

test('renderer filename follows the HTML document, including ASAR directories', () => {
  const documentPath = 'C:\\app\\renderer.asar\\modal-renderer\\index.html';
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\app', documentPath}),
  });
  assert.equal(context.__filename, documentPath);
  assert.equal(context.__dirname, 'C:\\app\\renderer.asar\\modal-renderer');
  assert.equal(context.require('path').join(context.__dirname, '../preload/entry.js'),
      'C:\\app\\renderer.asar\\preload\\entry.js');
});

test('guest preload retains Node closures without exposing transport to remote page', () => {
  const preload = 'C:\\test-app\\preload.js';
  const source = `const ipc = require('electron').ipcRenderer;
    globalThis.fixtureNative = () => {
      ipc.sendToHost('fixture', require('./value.json').value);
      return process.type;
    };`;
  const sent = [];
  const files = new Map([[preload, source], ['C:\\test-app\\value.json', '{"value":42}']]);
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', isGuest: true, isMainFrame: true}),
    send(...args) { sent.push(args); },
    sendSync(channel, request) {
      if (channel === '__xenon:renderer-web-preferences')
        return {preload, contextIsolation: false, nodeIntegration: false};
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat') {
        if (files.has(request.path)) return {isFile: true, isDirectory: false};
        throw new Error('ENOENT: no such file or directory');
      }
      return Buffer.from(files.get(request.path)).toString('base64');
    },
  }, true);
  for (const key of ['require', 'process', 'electron', 'xenonIpcRenderer', '__xenonElectronIpc'])
    assert.equal(context[key], undefined);
  assert.equal(context.fixtureNative(), 'renderer');
  assert.equal(sent.length, 1);
  assert.deepEqual(sent[0].slice(0, 2), ['__xenon:send-to-host', 'fixture']);
  assert.deepEqual(Array.from(sent[0][2]), [42]);
});

test('guest without preload cannot reach IPC through automatic Electron aliases', () => {
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', isGuest: true}),
    sendSync: () => ({nodeIntegration: false}),
  }, true);
  for (const key of ['require', 'process', 'Buffer', 'electron',
                    'xenonIpcRenderer', '__xenonElectronRequire', '__xenonElectronIpc']) {
    assert.equal(context[key], undefined, key);
  }
});

test('guest with explicit Node integration retains its Electron convenience alias', () => {
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', isGuest: true}),
    sendSync: () => ({nodeIntegration: true}),
  }, true);
  assert.equal(context.electron, context.require('electron'));
  assert.equal(typeof context.electron.ipcRenderer.send, 'function');
});

test('guest keeps only APIs explicitly exposed by a trusted preload', () => {
  const preload = 'C:\\test-app\\preload.js';
  const files = new Map([[preload,
    `const {ipcRenderer} = require('electron');
     globalThis.electron = {sendFixture: () => ipcRenderer.send('fixture')};`]]);
  const sent = [];
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', isGuest: true}),
    send(...args) { sent.push(args); },
    sendSync(channel, request) {
      if (channel === '__xenon:renderer-web-preferences')
        return {preload, contextIsolation: false, nodeIntegration: false};
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat') return {isFile: true, isDirectory: false};
      return Buffer.from(files.get(request.path)).toString('base64');
    },
  }, true);
  assert.equal(context.electron.ipcRenderer, undefined);
  context.electron.sendFixture();
  assert.deepEqual(sent, [['fixture']]);
  assert.equal(context.xenonIpcRenderer, undefined);
});

test('renderer process.isMainFrame reflects current document metadata', () => {
  for (const isMainFrame of [true, false]) {
    const {context} = createRenderer([], {
      getRuntimeConfig: () => ({appPath: 'C:\\test-app', isMainFrame}),
    });
    assert.equal(context.process.isMainFrame, isMainFrame);
  }
});

test('renderer process builtin and node alias have the global process identity', () => {
  const {context} = createRenderer();
  assert.equal(context.require('process'), context.process);
  assert.equal(context.require('node:process'), context.process);
  assert.equal(context.require.resolve('process'), 'process');
  assert.equal(context.require.resolve('node:process'), 'node:process');
});

test('renderer clipboard and shell use the host API and preserve results and failures', async () => {
  const calls = [];
  const failure = Object.assign(new Error('host operation failed'), {code: 'EIO'});
  let fail = false;
  const reply = request => {
    calls.push(JSON.parse(JSON.stringify(request)));
    if (fail) throw failure;
    switch (request.operation) {
      case 'clipboard.readText': return 'native clipboard text';
      case 'clipboard.readHTML': return '<p>native</p>';
      case 'shell.openPath': return 'File not found';
    }
  };
  const {context} = createRenderer([], {
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:electron-api');
      return reply(request);
    },
    async invoke(channel, request) {
      assert.equal(channel, '__xenon:electron-api');
      return reply(request);
    },
  });
  const {clipboard, shell} = context.require('electron');
  assert.equal(clipboard.readText(), 'native clipboard text');
  clipboard.writeText('fixture');
  assert.equal(clipboard.readHTML(), '<p>native</p>');
  clipboard.writeHTML('<b>fixture</b>');
  clipboard.clear();
  assert.equal(await shell.openExternal('https://fixture.invalid', {activate: false}), undefined);
  assert.equal(await shell.openPath('C:\\missing.file'), 'File not found');
  assert.equal(shell.showItemInFolder('C:\\fixture.mp4'), undefined);
  assert.deepEqual(calls, [
    {operation: 'clipboard.readText', type: 'clipboard'},
    {operation: 'clipboard.writeText', text: 'fixture', type: 'clipboard'},
    {operation: 'clipboard.readHTML', type: 'clipboard'},
    {operation: 'clipboard.writeHTML', markup: '<b>fixture</b>', type: 'clipboard'},
    {operation: 'clipboard.clear', type: 'clipboard'},
    {operation: 'shell.openExternal', url: 'https://fixture.invalid', options: {activate: false}},
    {operation: 'shell.openPath', path: 'C:\\missing.file'},
    {operation: 'shell.showItemInFolder', path: 'C:\\fixture.mp4'},
  ]);
  fail = true;
  assert.throws(() => clipboard.readText(), error => error === failure);
  assert.throws(() => clipboard.writeText('fixture'), error => error === failure);
  assert.throws(() => shell.showItemInFolder('C:\\fixture.mp4'), error => error === failure);
  await assert.rejects(shell.openExternal('https://fixture.invalid'), error => error === failure);
  await assert.rejects(shell.openPath('C:\\fixture.mp4'), error => error === failure);
});

test('renderer host APIs reject missing transport and expose native error codes', async () => {
  const {context} = createRenderer([], {sendSync: undefined, invoke: undefined});
  const {clipboard, shell} = context.require('electron');
  assert.throws(() => clipboard.readText(), {code: 'ERR_NOT_SUPPORTED'});
  await assert.rejects(shell.openExternal('https://fixture.invalid'), {code: 'ERR_NOT_SUPPORTED'});
  const unsupported = () => { throw new Error('ERR_NOT_SUPPORTED: fixture'); };
  const native = createRenderer([], {sendSync: unsupported, invoke: unsupported})
      .context.require('electron');
  assert.throws(() => native.clipboard.readText(), {code: 'ERR_NOT_SUPPORTED'});
  await assert.rejects(native.shell.openPath('C:\\fixture'), {code: 'ERR_NOT_SUPPORTED'});
});

test('renderer unavailable dialogs and window APIs fail explicitly', async () => {
  const {context} = createRenderer();
  const electron = context.require('electron');
  for (const dialog of [electron.dialog, electron.remote.dialog]) {
    assert.throws(() => dialog.showOpenDialogSync({}), {code: 'ERR_NOT_SUPPORTED'});
    await assert.rejects(dialog.showSaveDialog({}), {code: 'ERR_NOT_SUPPORTED'});
    await assert.rejects(dialog.showMessageBox({}), {code: 'ERR_NOT_SUPPORTED'});
    await assert.rejects(dialog.showOpenDialog({}), {code: 'ERR_NOT_SUPPORTED'});
  }
  assert.throws(() => electron.remote.getCurrentWindow(), {code: 'ERR_NOT_SUPPORTED'});
  for (const method of ['setZoomFactor', 'getZoomFactor', 'setZoomLevel', 'getZoomLevel'])
    assert.throws(() => electron.webFrame[method](1), {code: 'ERR_NOT_SUPPORTED'});
});

test('renderer open dialog preserves real cancellation, selection and native failures', async () => {
  const failure = new Error('picker disconnected');
  let result = {filePaths: []};
  const {context} = createRenderer([], {}, false, context => {
    context.__xenonPageHandler__ = {
      async openNativeFileDialog() {
        if (result instanceof Error) throw result;
        return result;
      },
    };
    context.__xenonPageCallbackRouter__ = {__xenonBootstrapListenersAttached: true};
  });
  const dialog = context.require('electron').dialog;
  assert.equal((await dialog.showOpenDialog({})).canceled, true);
  result = {filePaths: ['C:\\fixture.mp4']};
  const selection = await dialog.showOpenDialog({});
  assert.equal(selection.canceled, false);
  assert.deepEqual(selection.filePaths, ['C:\\fixture.mp4']);
  result = failure;
  await assert.rejects(dialog.showOpenDialog({}), error => error === failure);
  result = {};
  await assert.rejects(dialog.showOpenDialog({}), /invalid result/);
});

test('application and ASAR paths use native fs while chrome resources stay virtual', () => {
  const files = new Map([
    ['C:\\test-app\\plugins\\config.json', '{"plugin":"cloud"}'],
    ['C:\\test-app\\renderer.asar\\assets\\config.json', '{"archive":true}'],
  ]);
  const nativeCalls = [];
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({
      appName: 'sample-app',
      appVersion: '2.3.4',
      appPath: 'C:\\test-app',
      exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe',
    }),
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      nativeCalls.push([request.operation, request.path]);
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'read_file') {
        if (!files.has(request.path)) throw new Error('ENOENT: missing fixture');
        return Buffer.from(files.get(request.path)).toString('base64');
      }
      throw new Error(`unexpected fs operation: ${request.operation}`);
    },
  });
  const fs = context.require('fs');
  for (const [filename, contents] of files) {
    assert.equal(fs.existsSync(filename), true);
    assert.equal(fs.readFileSync(filename, 'utf8'), contents);
  }
  // Synthetic package descriptor for virtual chrome mounts stays in memory
  // without calling the broker and reflects container identity.
  const virtualRoot = 'chrome:\\xenon-player-electron';
  const virtualPackage = virtualRoot + '\\package.json';
  assert.equal(fs.existsSync(virtualRoot), true);
  assert.equal(fs.statSync(virtualRoot).isDirectory(), true);
  assert.deepEqual(Array.from(fs.readdirSync(virtualRoot)), ['package.json']);
  assert.equal(
      fs.readFileSync(virtualPackage, 'utf8'),
      '{"name":"sample-app","version":"2.3.4"}');
  assert.throws(
      () => fs.readFileSync(virtualRoot + '\\static\\js\\package.json'),
      error => error.code === 'ENOENT');
  assert.throws(
      () => fs.readFileSync('chrome:\\thunder-2025\\package.json'),
      error => error.code === 'ENOENT');
  assert.throws(
      () => fs.writeFileSync(virtualPackage, '{}'),
      error => error.code === 'EROFS');
  assert.deepEqual(nativeCalls, [
    ['exists', 'C:\\test-app\\plugins\\config.json'],
    ['read_file', 'C:\\test-app\\plugins\\config.json'],
    ['exists', 'C:\\test-app\\renderer.asar\\assets\\config.json'],
    ['read_file', 'C:\\test-app\\renderer.asar\\assets\\config.json'],
  ]);
});

test('universal ipcRenderer.send and app.getName without business hooks', () => {
  const sent = [];
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({
      appName: 'demo-app',
      appVersion: '5.6.7',
      execPath: 'C:\\app\\demo.exe',
    }),
    send(...args) { sent.push(args); },
  });
  const electron = context.require('electron');
  assert.equal(electron.app.getName(), 'demo-app');
  assert.equal(electron.app.getVersion(), '5.6.7');

  // Verify business channels like AplayerWndBind are transparently forwarded
  // and do not mutate global state or check legacy hooks.
  electron.ipcRenderer.send('AplayerWndBind', 12345);
  assert.deepEqual(sent, [['AplayerWndBind', 12345]]);
  assert.equal(context.__xenonLastAplayerWnd__, undefined);
  assert.equal(context.__xenonPlayerHostApi__, undefined);
});

test('ipcRenderer forwards async, sync, invoke and postMessage values unchanged', async () => {
  const calls = [];
  const rejection = new Error('transport disconnected');
  const {context} = createRenderer([], {
    send(...args) { calls.push(['send', ...args]); },
    sendSync(...args) {
      calls.push(['sendSync', ...args]);
      return {ok: true};
    },
    invoke(...args) {
      calls.push(['invoke', ...args]);
      return args[0] === 'reject' ? Promise.reject(rejection) : Promise.resolve(42);
    },
    postMessage(...args) { calls.push(['postMessage', ...args]); },
  });
  const ipcRenderer = context.require('electron').ipcRenderer;
  const payload = vm.runInContext('({missing: undefined, big: 7n})', context);
  ipcRenderer.send('send', payload);
  assert.equal(ipcRenderer.sendSync('sync', payload).ok, true);
  assert.equal(await ipcRenderer.invoke('invoke', payload), 42);
  await assert.rejects(ipcRenderer.invoke('reject'), error => error === rejection);
  ipcRenderer.postMessage('post', payload, []);
  assert.deepEqual(calls.map(call => call[0]),
      ['send', 'sendSync', 'invoke', 'invoke', 'postMessage']);
  assert.equal(calls[0][2], payload);
  assert.equal(calls[4][2], payload);
  assert.throws(
      () => ipcRenderer.postMessage('post', payload, [{}]),
      /MessagePort transfer is not supported/);
  assert.throws(() => ipcRenderer.postMessage('post', payload, {}),
      /"transfer" argument must be an array/);
});

test('ipcRenderer events expose Electron event shape and EventEmitter semantics', () => {
  const {context, dispatch} = createRenderer();
  const ipcRenderer = context.require('electron').ipcRenderer;
  const order = [];
  function repeated() { order.push('repeated'); }
  function once() { order.push('once'); }
  ipcRenderer.on('fixture', repeated);
  ipcRenderer.on('fixture', repeated);
  ipcRenderer.once('fixture', once);
  ipcRenderer.prependListener('fixture', () => order.push('first'));
  assert.equal(ipcRenderer.listenerCount('fixture', repeated), 2);
  assert.equal(ipcRenderer.listeners('fixture')[3], once);
  assert.notEqual(ipcRenderer.rawListeners('fixture')[3], once);
  ipcRenderer.removeListener('fixture', repeated);
  assert.equal(ipcRenderer.listenerCount('fixture', repeated), 1);

  let receivedEvent;
  let receivedValue;
  ipcRenderer.on('fixture', (event, value) => {
    receivedEvent = event;
    receivedValue = value;
  });
  dispatch('fixture', [17]);
  assert.deepEqual(order, ['first', 'repeated', 'once']);
  assert.equal(receivedEvent.sender, ipcRenderer);
  assert.deepEqual(Array.from(receivedEvent.ports), []);
  assert.equal(receivedValue, 17);
  assert.equal(ipcRenderer.listenerCount('fixture'), 3);
  assert.throws(() => ipcRenderer.on('invalid', null), {name: 'TypeError'});
  assert.throws(() => ipcRenderer.emit('error', new Error('unhandled')),
      /unhandled/);
  ipcRenderer.setMaxListeners(0);
  assert.equal(ipcRenderer.getMaxListeners(), 0);
  assert.deepEqual(Array.from(ipcRenderer.eventNames()), ['fixture']);
});

test('executable identity is document-scoped and is not renamed to the app name', () => {
  for (const [appName, execPath] of [
    ['DisplayName', 'C:\\apps\\one\\actual.exe'],
    ['Display Name', 'D:\\apps\\two\\launcher.exe'],
  ]) {
    const {context} = createRenderer([], {
      getRuntimeConfig: () => ({appName, execPath, appPath: 'C:\\test-app'}),
    });
    assert.equal(context.process.execPath, execPath);
    assert.equal(context.process.argv[0], execPath);
    assert.equal(context.require('electron').app.getPath('exe'), execPath);
  }
});

test('networkInterfaces returns fresh native IPv4 and IPv6 snapshots unchanged', () => {
  let reads = 0;
  const {context} = createRenderer([], {
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:os');
      assert.equal(request.method, 'networkInterfaces');
      ++reads;
      return {'fixture.adapter': [
        {address: '192.0.2.1', netmask: '255.255.255.0', family: 'IPv4',
          mac: '02:00:00:00:00:01', internal: false, cidr: '192.0.2.1/24'},
        {address: '::1', netmask: 'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff',
          family: 'IPv6', mac: '00:00:00:00:00:00', internal: true,
          cidr: '::1/128', scopeid: 0},
      ]};
    },
  });
  const os = context.require('node:os');
  const first = os.networkInterfaces();
  assert.equal(first['fixture.adapter'][0].family, 'IPv4');
  assert.equal(first['fixture.adapter'][1].scopeid, 0);
  assert.equal(first['fixture.adapter'][1].internal, true);
  first['fixture.adapter'].length = 0;
  assert.equal(os.networkInterfaces()['fixture.adapter'].length, 2);
  assert.equal(reads, 2);
});

test('networkInterfaces preserves an empty result and propagates native failures', () => {
  const failure = new Error('EIO: enumeration failed');
  let fail = false;
  const {context} = createRenderer([], {
    sendSync() {
      if (fail) throw failure;
      return {};
    },
  });
  assert.deepEqual(context.require('os').networkInterfaces(), {});
  fail = true;
  assert.throws(() => context.require('os').networkInterfaces(), e => e === failure);
});

test('missing exports remain undefined without mutating the export object', () => {
  const {load, calls} = createRenderer();
  const addon = load();
  const keys = Reflect.ownKeys(addon);
  for (const name of ['optionalFeature', 'OptionalClass', '', Symbol('missing')]) {
    assert.equal(addon[name], undefined);
    assert.equal(name in addon, false);
    assert.equal(Object.hasOwn(addon, name), false);
  }
  assert.deepEqual(Reflect.ownKeys(addon), keys);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('optional native calls use the caller fallback without invoking native code', () => {
  const {load, calls} = createRenderer();
  const addon = load();
  let fallbackCalls = 0;
  const fallback = () => ++fallbackCalls;
  assert.equal(addon.optionalFeature?.() ?? fallback(), 1);
  assert.equal(addon.optionalFeature?.() ?? fallback(), 2);
  assert.equal(fallbackCalls, 2);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('direct calls to missing exports fail locally without native dispatch', () => {
  const {load, calls} = createRenderer();
  const addon = load();
  assert.throws(() => addon.missing(), TypeError);
  assert.throws(() => new addon.Missing(), TypeError);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('declared functions, classes, constants and nested exports still work', () => {
  const {load, calls} = createRenderer([
    {name: 'readValue', kind: 'function'},
    {name: 'Handle', kind: 'class', prototype: []},
    {name: 'version', kind: 'value', value: {stringValue: '1.0'}},
    {name: 'enabled', kind: 'value', value: {boolValue: false}},
    {name: 'nested', kind: 'object', children: [
      {name: 'readValue', kind: 'function'},
    ]},
  ]);
  const addon = load();
  assert.equal(addon.readValue(3), 42);
  assert.equal(addon.nested.readValue(4), 42);
  const handle = new addon.Handle(5);
  assert.equal(handle.__instanceId, 7);
  assert.equal(handle instanceof addon.Handle, true);
  assert.equal(addon.version, '1.0');
  assert.equal(addon.enabled, false);
  assert.equal(addon.nested.missing, undefined);
  assert.deepEqual(calls, [
    ['require', addonPath],
    ['invoke', addonPath, 'readValue', 3],
    ['invoke', addonPath, 'nested.readValue', 4],
    ['construct', addonPath, 'Handle', 5],
  ]);
});

test('declared native function errors still propagate instead of using a fallback', () => {
  const error = new Error('native operation failed');
  const {load} = createRenderer([{name: 'readValue', kind: 'function'}], {
    invokeNodeExportSync() { throw error; },
  });
  const addon = load();
  assert.throws(() => addon.readValue?.() ?? 'fallback', value => value === error);
});

test('synchronous native module loads preserve identity without fabricated exports', async () => {
  const {load, calls} = createRenderer();
  const addon = load();
  assert.equal(load(), addon);
  assert.equal(addon.__xenonReady, undefined);
  assert.equal(await Promise.resolve(addon), addon);
  assert.equal(addon.default, undefined);
  assert.equal(addon.__esModule, undefined);
  assert.equal(addon.then, undefined);
  addon.localValue = 9;
  assert.equal(load().localValue, 9);
  assert.equal(typeof addon.toString, 'function');
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('native loading consumes the root descriptor in one reply and inspects nested values once', () => {
  let loads = 0;
  const inspections = [];
  const {context, load} = createRenderer([], {
    requireNodeModuleSync() {
      ++loads;
      return {kind: 'object', children: [
        {name: 'version', kind: 'number', value: 2},
        {name: 'nested', kind: 'object'},
      ]};
    },
    inspectNodeExportSync(modulePath, exportPath) {
      inspections.push(exportPath);
      assert.equal(exportPath, 'nested');
      return {kind: 'object', children: [{name: 'value', kind: 'number', value: 42}]};
    },
  });
  const addon = load();
  assert.equal(load(), addon);
  assert.equal(addon.version, 2);
  assert.deepEqual(inspections, []);
  assert.equal(addon.nested.value, 42);
  assert.equal(addon.nested.value, 42);
  assert.deepEqual(inspections, ['nested']);
  assert.equal(loads, 1);
  delete context.require.cache[addonPath];
  assert.notEqual(load(), addon);
  assert.equal(loads, 2);
});

test('invalid native load replies throw without caching an empty success', () => {
  let attempts = 0;
  const {load} = createRenderer([], {requireNodeModuleSync() {
    if (++attempts === 1) return null;
    return {kind: 'object', children: []};
  }});
  assert.throws(load, {code: 'ERR_INVALID_NATIVE_EXPORTS'});
  assert.equal(typeof load(), 'object');
  assert.equal(attempts, 2);
});

test('exact native descriptors preserve deep objects, arrays, statics and declared names', () => {
  const reads = [];
  const number = (name, value) => ({name, kind: 'number', value, enumerable: true, writable: true});
  const descriptions = {
    '': {kind: 'object', children: [
      {name: 'nested', kind: 'object'}, {name: 'items', kind: 'array'},
      {name: 'run', kind: 'function'}, {name: 'then', kind: 'function'},
      number('default', 7), {name: '__esModule', kind: 'boolean', value: false},
      {name: 'absentValue', kind: 'undefined'},
    ]},
    nested: {kind: 'object', children: [{name: 'deep', kind: 'object'}]},
    'nested.deep': {kind: 'object', children: [number('answer', 42)]},
    items: {kind: 'array', children: [number('0', 9), number('length', 1)]},
    run: {kind: 'function', children: [number('version', 2), number('length', 1)]},
    then: {kind: 'function', children: []},
  };
  const {load, calls} = createRenderer([], {inspectNodeExportSync(modulePath, exportPath) {
    assert.equal(modulePath, addonPath);
    reads.push(exportPath);
    return descriptions[exportPath];
  }});
  const addon = load();
  assert.deepEqual(reads, ['']);
  assert.equal(addon.missing, undefined);
  assert.equal(Object.hasOwn(addon, 'missing'), false);
  assert.equal(Object.hasOwn(addon, 'absentValue'), true);
  assert.equal(addon.absentValue, undefined);
  assert.equal(addon.default, 7);
  assert.equal(addon.__esModule, false);
  assert.equal(addon.nested.deep.answer, 42);
  assert.equal(addon.nested, addon.nested);
  assert.equal(addon.nested.deep, addon.nested.deep);
  assert.equal(Array.isArray(addon.items), true);
  assert.deepEqual(Array.from(addon.items), [9]);
  assert.equal(addon.run.version, 2);
  assert.equal(addon.run.length, 1);
  assert.equal(addon.run(4), 42);
  assert.equal(typeof addon.then, 'function');
  assert.deepEqual(reads, ['', 'nested', 'nested.deep', 'items', 'run', 'then']);
  assert.deepEqual(calls, [['require', addonPath], ['invoke', addonPath, 'run', 4]]);
});

test('exact native roots retain primitive and callable types including root construction', () => {
  for (const [description, expected] of [
    [{kind: 'number', value: 7}, 7], [{kind: 'string', value: 'native'}, 'native'],
    [{kind: 'bigint', value: '9007199254740993'}, 9007199254740993n],
    [{kind: 'undefined'}, undefined], [{kind: 'null'}, null],
  ]) {
    const {load} = createRenderer([], {inspectNodeExportSync() { return description; }});
    assert.equal(load(), expected);
  }
  for (const kind of ['function', 'class']) {
    const {load, calls} = createRenderer([], {
      inspectNodeExportSync() { return {kind, children: [], prototype: []}; },
    });
    const addon = load();
    assert.equal(typeof addon, 'function');
    assert.equal(addon.call(null, 3), 42);
    const instance = new addon(4);
    assert.equal(instance.__instanceId, 7);
    assert.equal(instance instanceof addon, true);
    assert.deepEqual(calls, [
      ['require', addonPath], ['invoke', addonPath, '', 3], ['construct', addonPath, '', 4],
    ]);
  }
});

test('exact native accessors run only on reads and propagate failures', () => {
  let reads = 0;
  const error = new Error('getter failed');
  const {load} = createRenderer([], {inspectNodeExportSync(modulePath, exportPath) {
    if (!exportPath) return {kind: 'object', children: [{name: 'value', kind: 'property'}]};
    if (++reads === 3) throw error;
    return {kind: 'number', value: reads};
  }});
  const addon = load();
  assert.equal(reads, 0);
  assert.deepEqual(Object.keys(addon), ['value']);
  assert.equal(reads, 0);
  assert.equal(addon.value, 1);
  assert.equal(addon.value, 2);
  assert.throws(() => addon.value, value => value === error);
  assert.throws(() => { addon.value = 9; }, {code: 'ERR_NOT_SUPPORTED'});
});

test('native dotted property names never dispatch to a different export', () => {
  const {load} = createRenderer([], {inspectNodeExportSync() {
    return {kind: 'object', children: [{name: 'wrong.path', kind: 'function'}]};
  }});
  assert.throws(() => load()['wrong.path'], {code: 'ERR_NOT_SUPPORTED'});
});

test('native accessors never re-evaluate a getter to invoke its returned value', () => {
  for (const kind of ['object', 'array', 'function', 'class']) {
    let reads = 0;
    const {load, calls} = createRenderer([], {inspectNodeExportSync(modulePath, exportPath) {
      if (!exportPath) return {kind: 'object', children: [{name: 'pick', kind: 'property'}]};
      ++reads;
      return {kind, children: []};
    }});
    assert.throws(() => load().pick, {code: 'ERR_NOT_SUPPORTED'});
    assert.equal(reads, 1);
    assert.deepEqual(calls, [['require', addonPath]]);
  }
});

test('native require cache invalidation reloads the exports from transport', () => {
  const {context, load, calls} = createRenderer();
  const first = load();
  first.localValue = 17;
  delete context.require.cache[addonPath];
  const second = load();
  assert.notEqual(second, first);
  assert.equal(second.localValue, undefined);
  assert.equal(calls.length, 2);
});

test('function-kind native constructors snapshot the actual new.target prototype', () => {
  for (const exact of [false, true]) {
    let additions;
    const descriptor = {name: 'Factory', kind: 'function', children: [], prototype: []};
    const {load} = createRenderer(exact ? descriptor : [descriptor], {
      constructNodeExportWithPrototypeSync(module, name, properties) {
        assert.equal(module, addonPath);
        assert.equal(name, exact ? '' : 'Factory');
        additions = properties;
        return 17;
      },
    });
    const Factory = exact ? load() : load().Factory;
    Factory.prototype.emit = function() {};
    class Derived extends Factory {
      consumerMethod() {}
    }
    const instance = new Derived();
    assert.ok(instance instanceof Derived);
    assert.ok(instance instanceof Factory);
    assert.equal(additions.emit.__xenon_node_wire_type__, 'callback');
    assert.equal(additions.consumerMethod.__xenon_node_wire_type__, 'callback');
    assert.equal(Object.hasOwn(additions, 'constructor'), false);
  }
});

test('native modules preserve declared then, default and __esModule exports without invoking them', () => {
  const {load, calls} = createRenderer([
    {name: 'then', kind: 'function'},
    {name: 'default', kind: 'value', value: {intValue: 7}},
    {name: '__esModule', kind: 'value', value: {boolValue: false}},
  ]);
  const addon = load();
  assert.equal(typeof addon.then, 'function');
  assert.equal(addon.default, 7);
  assert.equal(addon.__esModule, false);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('failed loads throw synchronously and do not poison the module cache', () => {
  const error = new Error('addon load failed');
  let attempts = 0;
  const {load} = createRenderer([], {
    requireNodeModuleSync() {
      if (++attempts === 1) throw error;
      return [{name: 'version', kind: 'value', value: {intValue: 2}}];
    },
  });
  assert.throws(load, value => value === error);
  assert.equal(load().version, 2);
  assert.equal(attempts, 2);
});

test('nextTick forwards all arguments asynchronously and returns undefined', async () => {
  const {context} = createRenderer();
  const value = {reading: false};
  let received;
  const result = context.process.nextTick((...args) => {
    received = args;
  }, value, undefined, 'tail');
  assert.equal(received, undefined);
  await Promise.resolve();
  assert.deepEqual(received, [value, undefined, 'tail']);
  assert.equal(result, undefined);
});

test('nextTick preserves zero arguments and callback order', async () => {
  const {context} = createRenderer();
  const events = [];
  context.process.nextTick((...args) => {
    events.push(['first', args.length]);
    context.process.nextTick(() => events.push(['nested']));
  });
  context.process.nextTick(() => events.push(['second']));
  await Promise.resolve();
  await Promise.resolve();
  assert.deepEqual(events, [['first', 0], ['second'], ['nested']]);
});

test('nextTick rejects non-function callbacks synchronously', () => {
  const {context} = createRenderer();
  for (const callback of [undefined, null, 1, 'callback', {}]) {
    assert.throws(() => context.process.nextTick(callback), {
      name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE',
    });
  }
});

// The native backend has separate real-file SQLite tests. This transport
// fixture checks API scheduling and wire values without duplicating a SQL engine.
function createSqliteRenderer(respond = () => undefined) {
  const requests = [];
  let statementId = 0;
  const {context} = createRenderer([], {
    async invoke(channel, request) {
      assert.equal(channel, '__xenon:sqlite');
      requests.push(request);
      const result = respond(request);
      if (result !== undefined) return result;
      switch (request.operation) {
        case 'open': return 1;
        case 'prepare': return ++statementId;
        case 'run': return {lastID: 17, changes: 2};
        case 'all': return [];
        default: return null;
      }
    },
  });
  return {context, requests, sqlite: context.require('xenon:sqlite3')};
}
function drainDatabase(db) {
  return new Promise(resolve => db.wait(resolve));
}

test('Statement preparation and binding complete asynchronously with statement context', async () => {
  const {sqlite} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  const completions = [];
  const stmt = new sqlite.Statement(db, 'SELECT ?', function(err) {
    completions.push(['prepare', this, err]);
  });
  assert.equal(stmt.bind([9], function(err) {
    completions.push(['bind', this, err]);
  }), stmt);
  assert.equal(completions.length, 0);
  await drainDatabase(db);
  assert.deepEqual(completions, [
    ['prepare', stmt, null], ['bind', stmt, null],
  ]);
});

test('Database.prepare binds parameters and calls back exactly once', async () => {
  const {sqlite, requests} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  const completions = [];
  const stmt = db.prepare('SELECT ?', [9], function(err) {
    completions.push([this, err]);
  });
  assert.equal(completions.length, 0);
  await drainDatabase(db);
  assert.deepEqual(completions, [[stmt, null]]);
  assert.deepEqual(requests.map(r => r.operation), ['open', 'prepare', 'bind']);
  assert.equal(requests[1].sql, 'SELECT ?');
  assert.deepEqual(Array.from(requests[2].params), [9]);
});

for (const method of ['run', 'get', 'all']) {
  test(`Statement.${method} preserves ordering and leaves retained bindings native`, async () => {
    const {sqlite, requests} = createSqliteRenderer();
    const db = new sqlite.Database(':memory:');
    const stmt = new sqlite.Statement(db, 'SELECT ?');
    stmt.bind([9]);
    let callbackThis;
    stmt[method](function(err) {
      assert.equal(err, null);
      callbackThis = this;
    });
    await drainDatabase(db);
    assert.equal(callbackThis, stmt);
    if (method === 'run') {
      assert.equal(stmt.lastID, 17);
      assert.equal(stmt.changes, 2);
    }
    stmt[method](10);
    stmt[method]();
    stmt.reset();
    stmt[method]();
    stmt[method]([]);
    stmt.finalize();
    await drainDatabase(db);
    assert.deepEqual(requests.map(r => r.operation), [
      'open', 'prepare', 'bind', method, method, method, 'reset',
      method, method, 'finalize',
    ]);
    const executions = requests.filter(r => r.operation === method);
    assert.deepEqual(executions.map(r => r.params && Array.from(r.params)),
        [undefined, [10], undefined, undefined, []]);
  });
}

test('Statement.run propagates SQLite execution errors with statement context', async () => {
  const {sqlite} = createSqliteRenderer(request => {
    if (request.operation === 'run') throw new Error('SQLITE_CONSTRAINT: constraint failed');
  });
  const db = new sqlite.Database(':memory:');
  const stmt = new sqlite.Statement(db, 'SELECT ?');
  let completion;
  stmt.run(function(err) { completion = [this, err]; });
  await drainDatabase(db);
  assert.equal(completion[0], stmt);
  assert.equal(completion[1].code, 'SQLITE_CONSTRAINT');
  assert.equal(completion[1].errno, 19);
  assert.equal(stmt.lastID, undefined);
  assert.equal(stmt.changes, undefined);
});

test('Statement reset and finalize callbacks are asynchronous and error-first', async () => {
  const {sqlite} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  const stmt = new sqlite.Statement(db, 'SELECT ?');
  const completions = [];
  assert.equal(stmt.reset(function(err) {
    completions.push(['reset', this, err]);
  }), stmt);
  assert.equal(stmt.finalize(function(err) {
    completions.push(['finalize', this, err]);
  }), stmt);
  assert.equal(completions.length, 0);
  await drainDatabase(db);
  assert.deepEqual(completions, [
    ['reset', stmt, null], ['finalize', stmt, null],
  ]);
});

test('SQLite BLOB IPC preserves bytes including empty blobs and sliced buffers', async () => {
  const encoded = Buffer.from([0, 255, 1, 0]).toString('base64');
  const {sqlite, context, requests} = createSqliteRenderer(request => {
    if (request.operation === 'get') return {
      payload: {__xenon_sqlite_blob__: encoded},
      empty: {__xenon_sqlite_blob__: ''},
      optional: null,
    };
  });
  const db = new sqlite.Database(':memory:');
  const bytes = context.Buffer.from([3, 0, 255, 1, 0, 4]);
  const stmt = db.prepare('SELECT ?, ?', bytes.subarray(1, 5), context.Buffer.alloc(0));
  let row;
  stmt.get(function(err, result) {
    assert.equal(err, null);
    row = result;
  });
  await drainDatabase(db);
  const params = requests.find(r => r.operation === 'bind').params;
  assert.equal(params[0].__xenon_sqlite_blob__, encoded);
  assert.equal(params[1].__xenon_sqlite_blob__, '');
  assert.equal(context.Buffer.isBuffer(row.payload), true);
  assert.deepEqual(Array.from(row.payload), [0, 255, 1, 0]);
  assert.equal(row.empty.length, 0);
  assert.equal(row.optional, null);
});

test('Database wrappers finalize statements and each emits rows before completion', async () => {
  const {sqlite, requests} = createSqliteRenderer(request => {
    if (request.operation === 'all') return [{value: 2}, {value: 3}];
  });
  const db = new sqlite.Database(':memory:');
  const events = [];
  assert.equal(db.each('SELECT value FROM records', function(err, row) {
    assert.equal(err, null);
    events.push(row.value);
  }, function(err, count) {
    assert.equal(err, null);
    events.push(['complete', count]);
  }), db);
  await drainDatabase(db);
  assert.deepEqual(events, [2, 3, ['complete', 2]]);
  assert.deepEqual(requests.map(r => r.operation), ['open', 'prepare', 'all', 'finalize']);
});

test('Database open errors are reported and never become successful operations', async () => {
  const {sqlite} = createSqliteRenderer(request => {
    if (request.operation === 'open') throw new Error('SQLITE_CANTOPEN: cannot open database');
  });
  const errors = [];
  const db = new sqlite.Database('C:\\missing\\records.db', err => errors.push(err.code));
  db.exec('CREATE TABLE records(value)', err => errors.push(err.code));
  await drainDatabase(db);
  assert.equal(db.open, false);
  assert.deepEqual(errors, ['SQLITE_CANTOPEN', 'SQLITE_CANTOPEN']);
});

test('Database close waits for statements and updates open only after success', async () => {
  let closeCalls = 0;
  const {sqlite} = createSqliteRenderer(request => {
    if (request.operation === 'close' && ++closeCalls === 1) {
      throw new Error('SQLITE_BUSY: unfinalized statements');
    }
  });
  const db = new sqlite.Database(':memory:');
  const states = [];
  db.close(err => states.push([err.code, db.open]));
  db.close(function(err) {
    assert.equal(this, db);
    states.push([err, db.open]);
  });
  await drainDatabase(db);
  assert.deepEqual(states, [['SQLITE_BUSY', true], [null, false]]);
});

test('Database.prepare reports invalid SQL once and drops queued operations', async () => {
  const {sqlite, requests} = createSqliteRenderer(request => {
    if (request.operation === 'prepare') throw new Error('SQLITE_ERROR: invalid SQL');
  });
  const db = new sqlite.Database(':memory:');
  const completions = [];
  db.prepare('invalid SQL', [1], err => completions.push(err.code)).finalize();
  await drainDatabase(db);
  assert.deepEqual(completions, ['SQLITE_ERROR']);
  assert.deepEqual(requests.map(r => r.operation), ['open', 'prepare']);
});

test('SQLite named parameters preserve names and BLOB values', async () => {
  const {sqlite, context, requests} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  db.run('INSERT INTO records VALUES ($id, $data)', {
    $id: 1, $data: context.Buffer.from([0, 255]),
  });
  await drainDatabase(db);
  const params = requests.find(r => r.operation === 'run').params;
  assert.deepEqual(Object.keys(params), ['$id', '$data']);
  assert.equal(params.$id, 1);
  assert.equal(params.$data.__xenon_sqlite_blob__, 'AP8=');
});

test('SQLite distinguishes optional undefined parameters from SQL null', async () => {
  const {sqlite, requests} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  db.prepare('SELECT 1', undefined, () => {}).finalize();
  db.prepare('SELECT ?', null, () => {}).finalize();
  await drainDatabase(db);
  const bindings = requests.filter(r => r.operation === 'bind');
  assert.equal(bindings[0].params[0].__xenon_sqlite_undefined__, true);
  assert.equal(bindings[1].params[0], null);
});

test('CommonJS bindings formatter restoration keeps default Error.stack a string', () => {
  const source = `module.exports = function() {
    const previous = Error.prepareStackTrace;
    Error.prepareStackTrace = (_error, sites) => sites;
    const customIsArray = Array.isArray(new Error().stack);
    const saved = Error.prepareStackTrace;
    Error.prepareStackTrace = () => 'temporary';
    Error.prepareStackTrace = saved;
    const sameFormatter = Error.prepareStackTrace === saved;
    Error.prepareStackTrace = previous;
    return {customIsArray, sameFormatter, stackType: typeof new Error().stack};
  };`;
  const files = new Map();
  const {context} = createRenderer([], {
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'write_file') {
        files.set(request.path,
            Buffer.from(request.dataBase64, 'base64').toString('utf8'));
        return true;
      }
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat') {
        if (files.has(request.path)) return {isFile: true, isDirectory: false};
        throw new Error('ENOENT: no such file or directory');
      }
      if (request.operation === 'read_file') {
        assert.ok(files.has(request.path));
        return Buffer.from(files.get(request.path)).toString('base64');
      }
      throw new Error(`unexpected fs operation: ${request.operation}`);
    },
  });
  context.require('fs').writeFileSync('C:\\test-app\\stack-fixture.cjs', source, 'utf8');
  const result = context.require('C:\\test-app\\stack-fixture.cjs')();
  assert.equal(result.customIsArray, true);
  assert.equal(result.sameFormatter, true);
  assert.equal(result.stackType, 'string');
});

for (const fallback of [false, true]) {
  test(`Buffer base64 preserves Node bytes, views and padding (fallback=${fallback})`, () => {
    const {context} = createRenderer([], {}, false, context => {
      if (fallback) vm.runInContext(
          'Uint8Array.prototype.toBase64 = undefined; Uint8Array.fromBase64 = undefined;', context);
    });
    for (const size of [0, 1, 2, 3, 255, 8191, 8192, 8193, 65536, 1048576]) {
      const bytes = Buffer.alloc(size + 4);
      for (let i = 0; i < bytes.length; ++i) bytes[i] = (i * 73 + 19) & 255;
      const view = bytes.subarray(2, size + 2);
      const shim = context.Buffer.from(view);
      for (const encoding of ['base64', 'base64url']) {
        const encoded = shim.toString(encoding);
        assert.equal(encoded, view.toString(encoding));
        assert.deepEqual(Buffer.from(context.Buffer.from(encoded, encoding)), view);
        assert.equal(shim.toString(encoding, 1, size - 1), view.subarray(1, size - 1).toString(encoding));
      }
    }
    for (const value of ['Zg', 'Zm8', ' \tZm9v\r\n', '-_8', '+/8=', 'Z!m@9#v',
                         'A', 'AAAAA', 'Zg===', 'Zg=trailing', '!!!', '']) {
      assert.deepEqual(Buffer.from(context.Buffer.from(value, 'base64')),
                       Buffer.from(value, 'base64'), value);
    }
  });
}

test('builtins use exact names and stable identities without substituting unsupported modules', () => {
  const {context} = createRenderer();
  for (const name of ['buffer', 'fs', 'events', 'path', 'http', 'http2', 'https', 'dns', 'timers', 'url', 'tls', 'readline', 'zlib', 'child_process']) {
    assert.equal(context.require(name), context.require(name));
    assert.equal(context.require(name), context.require('node:' + name));
    assert.equal(context.require.resolve('node:' + name), 'node:' + name);
  }
  assert.equal(context.require('path/win32'), context.require('path').win32);
  assert.equal(context.require('path/posix'), context.require('path').posix);
  for (const request of ['node:electron', 'node:FS', 'node:not-a-builtin']) {
    assert.throws(() => context.require(request), {code: 'ERR_UNKNOWN_BUILTIN_MODULE'});
  }
  assert.notEqual(context.require('http2'), context.require('http'));
  assert.throws(() => context.require('http2').connect('https://localhost'),
                {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => context.require('child_process').spawn('unavailable.exe'),
                {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => context.require('FS'), {code: 'MODULE_NOT_FOUND'});
  assert.notEqual(context.require('tls'), context.require('net'));
  assert.throws(() => context.require('tls').connect({host: 'localhost', port: 443}),
                {code: 'ERR_NOT_SUPPORTED'});
});

test('CommonJS resolves parent packages, scoped packages and package main without executing resolve', () => {
  const root = 'C:\\test-app';
  const files = new Map([
    [root + '\\feature\\index.js', `module.exports = () => [require('value'), require('@scope/pkg'), require.resolve('value')];`],
    [root + '\\feature\\node_modules\\value\\index.json', '17'],
    [root + '\\node_modules\\value\\index.json', '23'],
    [root + '\\node_modules\\@scope\\pkg\\package.json', '{"main":"lib/entry.cjs"}'],
    [root + '\\node_modules\\@scope\\pkg\\lib\\entry.cjs', 'globalThis.packageRuns = (globalThis.packageRuns || 0) + 1; module.exports = 42;'],
  ]);
  const {context} = createPreloadRenderer('', files);
  const entry = root + '\\node_modules\\@scope\\pkg\\lib\\entry.cjs';
  assert.equal(context.require.resolve('@scope/pkg'), entry);
  assert.equal(context.packageRuns, undefined);
  const load = context.require(root + '\\feature');
  assert.deepEqual(Array.from(load()), [17, 42, root + '\\feature\\node_modules\\value\\index.json']);
  assert.equal(context.require('value'), 23);
  assert.equal(context.packageRuns, 1);
});

test('CommonJS honors exact filenames, cycles, current exports and cache invalidation', () => {
  const root = 'C:\\test-app';
  const files = new Map([
    [root + '\\a.js', `exports.before = true; require('./b'); module.exports = {after: true};`],
    [root + '\\b.js', `module.exports = require('./a').before;`],
    [root + '\\extensionless', `module.exports = 7;`],
    [root + '\\extensionless.js', `module.exports = 8;`],
    [root + '\\explicit.cjs', `globalThis.runs = (globalThis.runs || 0) + 1; module.exports = undefined;`],
  ]);
  const {context} = createPreloadRenderer('', files);
  assert.equal(context.require(root + '\\extensionless'), 7);
  assert.equal(context.require(root + '\\a').after, true);
  assert.equal(context.require(root + '\\b'), true);
  assert.throws(() => context.require(root + '\\explicit'), {code: 'MODULE_NOT_FOUND'});
  const filename = root + '\\explicit.cjs';
  assert.equal(context.require(filename), undefined);
  assert.equal(context.require(filename), undefined);
  assert.equal(context.runs, 1);
  delete context.require.cache[filename];
  context.require(filename);
  assert.equal(context.runs, 2);
});

test('require loads real sqlite3 packages and addons and never substitutes missing native paths', () => {
  const root = 'C:\\test-app';
  const native = root + '\\node_sqlite3.node';
  const files = new Map([
    [root + '\\node_modules\\sqlite3\\index.js', 'module.exports = {fromPackage: true};'],
    [root + '\\nested\\sqlite3.js', 'module.exports = 91;'],
    [native, 'native fixture'],
    [root + '\\build\\Release\\missing.node', 'wrong same-named fixture'],
  ]);
  const {context, calls} = createPreloadRenderer('', files);
  assert.equal(context.require('sqlite3').fromPackage, true);
  assert.equal(context.require(root + '\\nested\\sqlite3'), 91);
  assert.equal(context.require(native).Database, undefined);
  assert.deepEqual(calls, [['require', native]]);
  assert.equal(typeof context.require('xenon:sqlite3').Database, 'function');
  assert.throws(() => context.require(root + '\\missing.node'), {code: 'MODULE_NOT_FOUND'});
  assert.equal(calls.length, 1);
});

test('module exceptions preserve identity and unsupported package maps never bypass exports', () => {
  const root = 'C:\\test-app';
  const files = new Map([
    [root + '\\throw.js', 'throw globalThis.sentinel;'],
    [root + '\\node_modules\\mapped\\package.json', '{"exports":"./entry.js","main":"entry.js"}'],
    [root + '\\node_modules\\mapped\\entry.js', 'module.exports = 42;'],
  ]);
  const {context} = createPreloadRenderer('', files);
  context.sentinel = {originalError: true};
  assert.throws(() => context.require(root + '\\throw.js'), error => error === context.sentinel);
  assert.equal(context.require.cache[root + '\\throw.js'], undefined);
  assert.throws(() => context.require('mapped'), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => context.require('mapped/entry.js'), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => context.require('#alias'), {code: 'ERR_NOT_SUPPORTED'});
});
