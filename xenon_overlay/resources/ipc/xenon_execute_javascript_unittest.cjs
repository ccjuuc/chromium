// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

const requestChannel = '__xenon:execute-javascript';
const resultChannel = '__xenon:execute-javascript-result';
const cancelChannel = '__xenon:cancel-execute-javascript';
const mainPath = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
const rendererPath = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const mainSource = readBootstrap(mainPath);
const rendererSource = readBootstrap(rendererPath);
const common = {
  TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
  setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
  console: {log() {}, warn() {}, error() {}},
};

function createMain(runtimeReply) {
  const deliveries = [];
  const nativeCalls = [];
  const renderers = new Map();
  const documents = new Map();
  const timers = new Map();
  let nextWindowId = 1;
  let nextTimerId = 1;
  const context = vm.createContext({
    ...common,
    setTimeout(callback, delay, ...args) {
      const id = nextTimerId++;
      timers.set(id, {callback: () => callback(...args), delay});
      return id;
    },
    clearTimeout: id => timers.delete(id),
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '10.0', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonWorkingDirectory: 'C:\\fixture', __xenonResourcesPath: 'C:\\fixture',
    __xenonIsPackaged: false, __xenonChromeVersion: '142', __xenonV8Version: '',
    __xenonGetPath: () => '',
    __xenonCreateBrowserWindow: () => ({id: nextWindowId++, hwnd: '0'}),
    __xenonBrowserWindowCall(id, command, details) {
      if (id === 0 && command === 'electron-session' && details.method === 'create-window') {
        return context.__xenonCreateBrowserWindow(details.options);
      }
      nativeCalls.push({id, command, details});
      assert.notEqual(command, 'execute-javascript',
          'script execution must not use the synchronous Browser command');
      return null;
    },
    __xenonCloseBrowserWindow() {},
    __xenonBrowserWindowCallAsync(id, command) {
      assert.equal(command, 'wait-for-main-frame-runtime');
      const endpoint = [...documents.values()].filter(sender =>
          sender.windowId === id && sender.isMainFrame === true).at(-1)?.endpointId || '';
      return runtimeReply ? runtimeReply(id, endpoint) : Promise.resolve(endpoint);
    },
    __xenonSendToRenderer(endpointId, channel, args) {
      deliveries.push({endpointId, channel, args});
      renderers.get(endpointId)?.dispatch(channel, args);
    },
  });
  vm.runInContext(mainSource, context, {filename: mainPath});
  const electron = context.__xenonElectron;
  const attach = sender => {
    documents.set(sender.endpointId, sender);
    context.__xenonDispatchRendererEvent(sender, true);
  };
  const detach = sender => {
    documents.delete(sender.endpointId);
    context.__xenonDispatchRendererEvent(sender, false);
  };
  const reply = (sender, id, success, value) =>
    context.__xenonDispatchSend(sender, resultChannel, [id, success, value]);
  function createWindow(endpointId = 'main-document') {
    const window = new electron.BrowserWindow();
    const sender = {endpointId, windowId: window.id, processId: 17,
      frameId: 23, isMainFrame: true, url: 'file:///fixture/index.html'};
    attach(sender);
    return {window, contents: window.webContents, sender};
  }
  async function latestRequest() {
    // Drain cross-realm native-Promise adoption before inspecting deliveries.
    await new Promise(setImmediate);
    const request = deliveries.filter(item => item.channel === requestChannel).at(-1);
    assert.ok(request, 'an asynchronous script request was sent');
    return {...request, id: request.args[0], code: request.args[1]};
  }
  return {context, electron, deliveries, nativeCalls, renderers, timers,
    attach, detach, reply, createWindow, latestRequest};
}

function createRenderer(main, sender, {isGuest = false, execute} = {}) {
  const nativeCalls = [];
  const cancelled = [];
  const sent = [];
  let dispatch;
  let evalCalls = 0;
  // Native execution targets a separate page world. The bootstrap world has
  // its own marker and an eval trap, so executing in that world fails the test.
  const page = vm.createContext({worldMarker: 'page-main-world'});
  const transport = {
    getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
      cwd: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe',
      platform: 'win32', arch: 'x64', isMainFrame: true, isGuest,
      contextIsolated: true,
      webPreferences: {contextIsolation: true, nodeIntegration: false}}),
    setDispatchHandler(handler) { dispatch = handler; },
    executeJavaScript(id, code) {
      assert.equal(this, transport);
      assert.equal(typeof id, 'number');
      nativeCalls.push({id, code});
      return execute ? execute(code, page) : vm.runInContext(code, page);
    },
    cancelExecuteJavaScript(id) {
      assert.equal(this, transport);
      cancelled.push(id);
    },
    send(channel, ...args) {
      sent.push({channel, args});
      main.context.__xenonDispatchSend(sender, channel, args);
    },
    sendSync(channel) {
      throw new Error('Unexpected synchronous renderer IPC: ' + channel);
    },
  };
  const context = vm.createContext({
    ...common,
    xenonIpcRenderer: transport,
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    location: {protocol: 'file:', hostname: '', pathname: '/C:/fixture/index.html'},
    document: {readyState: 'loading', addEventListener() {}},
    worldMarker: 'bootstrap-isolated-world',
    eval() {
      ++evalCalls;
      throw new Error('Bootstrap eval must not execute page code');
    },
  });
  context.window = context;
  vm.runInContext(rendererSource, context, {filename: rendererPath});
  assert.equal(typeof dispatch, 'function');
  const renderer = {context, page, nativeCalls, cancelled, sent, dispatch,
    get evalCalls() { return evalCalls; }};
  main.renderers.set(sender.endpointId, renderer);
  return renderer;
}

function assertCancelled(main, sender, id) {
  const cancellation = main.deliveries.find(item =>
    item.endpointId === sender.endpointId && item.channel === cancelChannel &&
    item.args[0] === id);
  assert.ok(cancellation, 'the original native execution is cancelled');
}

function observe(promise) {
  const state = {status: 'pending'};
  promise.then(value => { state.status = 'fulfilled'; state.value = value; },
      error => { state.status = 'rejected'; state.error = error; });
  return state;
}

async function assertPending(state) {
  // Flush promise continuations without waiting for any wall-clock timers.
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(state.status, 'pending');
}

for (const kind of ['BrowserWindow', 'guest']) {
  test(`${kind}: native page-world execution preserves values, promises and undefined`, async () => {
    const main = createMain();
    const host = main.createWindow();
    let {contents, sender} = host;
    if (kind === 'guest') {
      const id = await main.context.__xenonDispatchInvoke(host.sender,
          '__xenon:register-guest', [-1, {contextIsolation: true}]);
      contents = main.electron.webContents.fromId(id);
      sender = {...host.sender, endpointId: 'guest-document', windowId: -1};
      main.attach(sender);
    }
    const renderer = createRenderer(main, sender, {isGuest: kind === 'guest'});
    assert.equal(await contents.executeJavaScript('worldMarker'), 'page-main-world');
    assert.equal(await contents.executeJavaScript('Promise.resolve(42)', false), 42);
    assert.equal(await contents.executeJavaScript('undefined'), undefined);
    const value = await contents.executeJavaScript('Promise.resolve({answer: 42, empty: undefined})');
    assert.deepEqual({...value}, {answer: 42, empty: undefined});
    assert.equal(renderer.evalCalls, 0);
    assert.equal(renderer.nativeCalls.length, 4);
    assert.equal(renderer.sent.length, 4);
    assert.equal(renderer.sent.at(-1).args[2], value);
    assert.equal(main.timers.size, 0, 'completed requests release their timeout');
  });
}

test('native synchronous throws and rejected promises preserve Error and rejection values', async () => {
  const main = createMain();
  const {contents, sender} = main.createWindow();
  const failure = Object.assign(new Error('native script failed'), {code: 'ERR_FIXTURE'});
  const renderer = createRenderer(main, sender, {execute(code) {
    if (code === 'throw') throw failure;
    if (code === 'reject') return Promise.reject(failure);
    return Promise.reject(undefined);
  }});
  for (const code of ['throw', 'reject']) {
    await assert.rejects(contents.executeJavaScript(code), error => error === failure);
    assert.equal(renderer.sent.at(-1).channel, resultChannel);
    assert.equal(renderer.sent.at(-1).args[1], false);
    assert.equal(renderer.sent.at(-1).args[2], failure);
  }
  await contents.executeJavaScript('reject undefined').then(
      () => assert.fail('a rejected native promise must remain rejected'),
      error => assert.equal(error, undefined));
  assert.equal(renderer.evalCalls, 0);
  assert.equal(main.timers.size, 0);
});

test('execution waits for the native promise and concurrent results retain request identity', async () => {
  const main = createMain();
  const {contents, sender} = main.createWindow();
  const resolve = new Map();
  const renderer = createRenderer(main, sender, {execute: code =>
    new Promise(done => resolve.set(code, done))});
  const first = contents.executeJavaScript('first');
  const second = contents.executeJavaScript('second');
  const firstState = observe(first);
  await assertPending(firstState);
  assert.equal(renderer.sent.length, 0);
  resolve.get('second')(2);
  assert.equal(await second, 2);
  await assertPending(firstState);
  resolve.get('first')(1);
  assert.equal(await first, 1);
  assert.equal(main.timers.size, 0);
});

test('another window and a subframe cannot settle a main-frame request', async () => {
  const main = createMain();
  const first = main.createWindow('first-document');
  const other = main.createWindow('other-document');
  const child = {...first.sender, endpointId: 'child-document', frameId: 24,
    isMainFrame: false};
  main.attach(child);
  const result = first.contents.executeJavaScript('42');
  const state = observe(result);
  const request = await main.latestRequest();
  assert.equal(request.endpointId, first.sender.endpointId);
  main.reply(other.sender, request.id, true, 'wrong window');
  await assertPending(state);
  main.reply(child, request.id, false, new Error('wrong frame'));
  await assertPending(state);
  main.reply(first.sender, request.id, true, 42);
  assert.equal(await result, 42);
  assert.equal(main.timers.size, 0);
});

test('a replacement document rejects old work and old replies cannot settle new work', async () => {
  const main = createMain();
  const {contents, sender} = main.createWindow();
  const oldResult = contents.executeJavaScript('old work');
  const oldRejected = assert.rejects(oldResult, /navigat|document|detach|replace/i);
  const oldRequest = await main.latestRequest();
  const next = {...sender, endpointId: 'next-document', processId: 18,
    frameId: 25, url: 'file:///fixture/next.html'};
  main.attach(next);
  await oldRejected;
  assertCancelled(main, sender, oldRequest.id);
  const result = contents.executeJavaScript('new work');
  const state = observe(result);
  const request = await main.latestRequest();
  assert.equal(request.endpointId, next.endpointId);
  main.reply(sender, oldRequest.id, true, 'old result');
  main.reply(sender, request.id, true, 'forged new result');
  await assertPending(state);
  main.detach(sender);
  await assertPending(state);
  main.reply(next, request.id, true, 'current result');
  assert.equal(await result, 'current result');
  assert.equal(main.timers.size, 0);
});

test('same-document updates and subframe detach leave main-frame execution pending', async () => {
  const main = createMain();
  const {contents, sender} = main.createWindow();
  const child = {...sender, endpointId: 'child-document', frameId: 24, isMainFrame: false};
  main.attach(child);
  const frame = contents.mainFrame;
  const result = contents.executeJavaScript('42');
  const state = observe(result);
  const request = await main.latestRequest();
  main.attach({...sender, url: sender.url + '#same-document'});
  main.detach(child);
  assert.equal(contents.mainFrame, frame);
  await assertPending(state);
  main.reply(sender, request.id, true, 42);
  assert.equal(await result, 42);
});

test('detaching the executing document rejects outstanding work', async () => {
  const main = createMain();
  const {contents, sender} = main.createWindow();
  const result = contents.executeJavaScript('pending');
  const rejected = assert.rejects(result, /detach|document|unavailable/i);
  const request = await main.latestRequest();
  main.detach(sender);
  await rejected;
  assertCancelled(main, sender, request.id);
  assert.equal(contents.isDestroyed(), false);
  assert.equal(main.timers.size, 0);
  await assert.rejects(contents.executeJavaScript('after detach'), /unavailable|frame|document/i);
});

test('destroying WebContents rejects outstanding work and subsequent execution', async () => {
  const main = createMain();
  const {window, contents, sender} = main.createWindow();
  const result = contents.executeJavaScript('pending');
  const rejected = assert.rejects(result, /destroy/i);
  const request = await main.latestRequest();
  main.context.__xenonDispatchBrowserWindowEvent(window.id, 'closed');
  await rejected;
  assertCancelled(main, sender, request.id);
  assert.equal(contents.isDestroyed(), true);
  assert.equal(main.timers.size, 0);
  await assert.rejects(contents.executeJavaScript('after destruction'), /destroy|unavailable/i);
});

test('execution waits for the current document registration acknowledgement', async () => {
  let acknowledge;
  const main = createMain(() => new Promise(resolve => { acknowledge = resolve; }));
  const window = new main.electron.BrowserWindow();
  const result = window.webContents.executeJavaScript('42');
  const state = observe(result);
  await assertPending(state);
  assert.equal(main.deliveries.length, 0);
  const sender = {endpointId: 'registered-document', windowId: window.id,
    processId: 17, frameId: 23, isMainFrame: true, url: 'https://fixture.test/'};
  main.attach(sender);
  acknowledge(sender.endpointId);
  const request = await main.latestRequest();
  main.reply(sender, request.id, true, 42);
  assert.equal(await result, 42);
});

test('a registration acknowledgement cannot execute code in a replacement document', async () => {
  let acknowledge;
  const main = createMain(() => new Promise(resolve => { acknowledge = resolve; }));
  const {contents, sender} = main.createWindow();
  const result = contents.executeJavaScript('must not run');
  const rejected = assert.rejects(result, /document|unavailable/i);
  main.attach({...sender, endpointId: 'replacement'});
  acknowledge(sender.endpointId);
  await rejected;
  assert.equal(main.deliveries.length, 0);
});

test('argument validation rejects invalid input and unsupported user gestures without IPC', async () => {
  const main = createMain();
  const {contents} = main.createWindow();
  for (const code of [undefined, null, 42, {}, new String('42')]) {
    await assert.rejects(contents.executeJavaScript(code), {name: 'TypeError'});
  }
  for (const userGesture of [null, 1, 'false', {}]) {
    await assert.rejects(contents.executeJavaScript('42', userGesture), {name: 'TypeError'});
  }
  await assert.rejects(contents.executeJavaScript('42', true), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(main.deliveries.length, 0);
  assert.equal(main.timers.size, 0);
});

test('only explicitly trusted main-frame metadata selects the execution endpoint', async () => {
  const main = createMain();
  const window = new main.electron.BrowserWindow();
  const sender = {endpointId: 'unknown-frame', windowId: window.id, processId: 17, frameId: 23};
  main.attach(sender);
  main.attach({...sender, endpointId: 'child-frame', isMainFrame: false});
  await assert.rejects(window.webContents.executeJavaScript('42'), /unavailable|frame|document/i);
  assert.equal(main.deliveries.length, 0);
  assert.equal(main.timers.size, 0);
});

test('an unanswered request rejects at 30 seconds and ignores a late result', async () => {
  const main = createMain();
  const {contents, sender} = main.createWindow();
  const result = contents.executeJavaScript('never completes');
  const rejected = assert.rejects(result, /timed? ?out|timeout/i);
  const request = await main.latestRequest();
  const matchingTimers = [...main.timers].filter(([, timer]) => timer.delay === 30000);
  assert.equal(matchingTimers.length, 1);
  const [timerId, timer] = matchingTimers[0];
  main.timers.delete(timerId);
  timer.callback();
  await rejected;
  assertCancelled(main, sender, request.id);
  assert.doesNotThrow(() => main.reply(sender, request.id, true, 'too late'));
  assert.equal(main.timers.size, 0);
});

test('renderer cancellation reaches the native execution by request id', () => {
  const main = createMain();
  const {sender} = main.createWindow();
  const renderer = createRenderer(main, sender);
  renderer.dispatch(cancelChannel, [73]);
  assert.deepEqual(renderer.cancelled, [73]);
  assert.equal(renderer.evalCalls, 0);
});
