// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createRuntime(onWindowCall = () => null) {
  const calls = [];
  let nextWindowId = 1;
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
    __xenonCreateBrowserWindow(options) {
      const id = nextWindowId++;
      calls.push({id, command: 'create', options: {...options}});
      return {id, hwnd: '0'};
    },
    __xenonBrowserWindowCall(id, command, details) {
      calls.push({id, command, details: {...details}});
      return onWindowCall(id, command, details);
    },
    __xenonLoadBrowserWindowURL(id, url) {
      calls.push({id, command: 'load-url', url});
    },
  });
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readFileSync(filename, 'utf8'), context, {filename});
  return {context, calls, BrowserWindow: context.__xenonElectron.BrowserWindow};
}

test('BrowserWindow preserves missing properties and resolves as an ordinary object', async () => {
  const {BrowserWindow} = createRuntime();
  const window = new BrowserWindow();
  assert.equal(window.then, undefined);
  assert.equal(window.setBackgroundMaterial, undefined);
  assert.equal(window.isUnknownCapability, undefined);
  assert.equal(window.hasUnknownCapability, undefined);
  assert.equal(await Promise.resolve(window), window);
  assert.equal(BrowserWindow.fromWebContents(window.webContents), window);
  assert.equal(BrowserWindow.fromId(window.id), window);
});

test('BrowserWindow parenting validates cycles and only commits successful native changes', () => {
  const {BrowserWindow, calls, context} = createRuntime((id, command, details) => {
    if (command === 'set-parent-window' && details.parentId === 3) {
      throw new Error('Parent unavailable');
    }
    if (command === 'set-progress-bar') {
      throw new Error('ERR_NOT_SUPPORTED: BrowserWindow.setProgressBar');
    }
    return null;
  });
  const first = new BrowserWindow();
  const child = new BrowserWindow();
  const unavailable = new BrowserWindow();
  child.setParentWindow(first);
  assert.equal(child.getParentWindow(), first);
  assert.deepEqual(Array.from(first.getChildWindows()), [child]);
  assert.throws(() => first.setParentWindow(child), /parent cycle/);
  assert.throws(() => child.setParentWindow(unavailable), /Parent unavailable/);
  assert.equal(child.getParentWindow(), first);
  child.setParentWindow(null);
  assert.equal(child.getParentWindow(), null);
  child.moveTop();
  assert.throws(() => child.setProgressBar(0.5, {mode: 'paused'}),
      {code: 'ERR_NOT_SUPPORTED'});
  assert.ok(calls.some(call => call.id === child.id && call.command === 'move-top'));
  assert.deepEqual(calls.at(-1).details, {progress: 0.5, mode: 'paused'});
  delete context.__xenonBrowserWindowCall;
  assert.throws(() => child.setParentWindow(first), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(child.getParentWindow(), null);
  delete context.__xenonCreateBrowserWindow;
  assert.throws(() => new BrowserWindow(), {code: 'ERR_NOT_SUPPORTED'});
});

test('BrowserWindow focus and always-on-top queries use actual host state', () => {
  const {BrowserWindow} = createRuntime((id, command) => {
    if (command === 'is-focused') return id === 1;
    if (command === 'is-always-on-top') return id === 2;
    return null;
  });
  const first = new BrowserWindow();
  const second = new BrowserWindow();
  assert.equal(BrowserWindow.getFocusedWindow(), first);
  assert.equal(first.isAlwaysOnTop(), false);
  assert.equal(second.isAlwaysOnTop(), true);
});

test('unimplemented Electron APIs report errors instead of fabricated success or cancellation', async () => {
  const {context} = createRuntime();
  const electron = context.__xenonElectron;
  const session = electron.session.defaultSession;
  for (const operation of [
    () => session.cookies.get({}), () => session.cookies.set({}),
    () => session.cookies.remove('https://fixture.invalid', 'key'),
    () => session.cookies.flushStore(), () => session.setProxy({}),
    () => session.resolveProxy('https://fixture.invalid'), () => session.clearCache(),
    () => session.clearStorageData(), () => session.getBlobData('missing'),
    () => electron.dialog.showOpenDialog({}), () => electron.dialog.showSaveDialog({}),
    () => electron.dialog.showMessageBox({}),
  ]) await assert.rejects(operation(), {code: 'ERR_NOT_SUPPORTED'});
  for (const operation of [
    () => session.webRequest.onBeforeRequest({}, () => {}),
    () => session.setPermissionRequestHandler(() => {}),
    () => session.protocol.registerFileProtocol('fixture', () => {}),
    () => electron.globalShortcut.register('Ctrl+F10', () => {}),
    () => electron.dialog.showOpenDialogSync({}),
  ]) assert.throws(operation, {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(electron.globalShortcut.isRegistered('Ctrl+F10'), false);
});

test('main clipboard and shell use host results and preserve errors', async () => {
  const {context, calls} = createRuntime((id, command, details) => {
    assert.equal(id, 0);
    assert.equal(command, 'electron-api');
    if (details.operation === 'clipboard.readText') return 'fixture text';
    if (details.operation === 'shell.openPath') return 'Path not found';
    if (details.operation === 'shell.openExternal') throw new Error('ERR_FAILED: fixture failure');
    return null;
  });
  const {clipboard, shell} = context.__xenonElectron;
  assert.equal(clipboard.readText(), 'fixture text');
  assert.equal(clipboard.writeText('fixture value'), undefined);
  assert.equal(await shell.openPath('C:/missing-fixture'), 'Path not found');
  await assert.rejects(shell.openExternal('https://fixture.invalid'), {code: 'ERR_FAILED'});
  assert.equal(calls.length, 4);
  delete context.__xenonBrowserWindowCall;
  assert.throws(() => clipboard.readText(), {code: 'ERR_NOT_SUPPORTED'});
  await assert.rejects(shell.openPath('C:/fixture'), {code: 'ERR_NOT_SUPPORTED'});
});

test('BrowserWindow applies the requested background before loading its page', async () => {
  const {BrowserWindow, calls} = createRuntime();
  const window = new BrowserWindow({backgroundColor: '#202020', show: false});
  await window.loadURL('https://fixture.invalid/menu');
  const relevant = calls.filter(call =>
    ['create', 'set-background-color', 'load-url'].includes(call.command));
  assert.deepEqual(relevant.map(call => call.command),
      ['create', 'set-background-color', 'load-url']);
  assert.equal(relevant.every(call => call.id === window.id), true);
  assert.deepEqual(relevant[1].details, {color: '#202020'});
  assert.equal(window.getBackgroundColor(), '#202020');
});

test('BrowserWindow preserves default and transparent creation without an explicit color', () => {
  const {BrowserWindow, calls} = createRuntime();
  new BrowserWindow();
  new BrowserWindow({transparent: true});
  assert.equal(calls.some(call => call.command === 'set-background-color'), false);
  const created = calls.filter(call => call.command === 'create');
  assert.deepEqual(created.map(call => call.options.transparent), [false, true]);
});

test('BrowserWindow runtime background changes use the host and preserve cached color on failure', () => {
  const failure = new TypeError('Invalid BrowserWindow background color');
  const {BrowserWindow, calls} = createRuntime((id, command, details) => {
    if (command === 'set-background-color' && details.color === 'invalid-color') {
      throw failure;
    }
    return null;
  });
  const window = new BrowserWindow({backgroundColor: '#202020'});
  assert.equal(window.setBackgroundColor('#303030'), undefined);
  assert.equal(window.getBackgroundColor(), '#303030');
  assert.throws(() => window.setBackgroundColor('invalid-color'),
      error => error === failure);
  assert.equal(window.getBackgroundColor(), '#303030');
  assert.deepEqual(calls.filter(call => call.command === 'set-background-color'), [
    {id: window.id, command: 'set-background-color', details: {color: '#202020'}},
    {id: window.id, command: 'set-background-color', details: {color: '#303030'}},
    {id: window.id, command: 'set-background-color', details: {color: 'invalid-color'}},
  ]);
});

test('BrowserWindow propagates constructor and unavailable-host background failures', () => {
  const failure = new TypeError('Window host rejected background color');
  const {BrowserWindow, calls} = createRuntime((id, command) => {
    if (command === 'set-background-color') throw failure;
    return null;
  });
  assert.throws(() => {
    const window = new BrowserWindow({backgroundColor: '#202020'});
    window.loadURL('https://fixture.invalid/menu');
  }, error => error === failure);
  assert.equal(calls.some(call => call.command === 'load-url'), false);

  const runtime = createRuntime();
  const window = new runtime.BrowserWindow({backgroundColor: '#202020'});
  delete runtime.context.__xenonBrowserWindowCall;
  assert.throws(() => window.setBackgroundColor('#303030'), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(window.getBackgroundColor(), '#202020');
});

test('BrowserWindow native mouse hooks expose signed Buffer reads and full-width LPARAM', () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const window = new BrowserWindow({show: false});
  const {Buffer} = context.__xenonBuffer;
  const WM_MOUSEMOVE = 0x0200;
  const MK_LBUTTON = 0x0001;
  const received = [];
  window.hookWindowMessage(WM_MOUSEMOVE, (wParam, lParam) => {
    assert.equal(Buffer.isBuffer(wParam), true);
    assert.equal(Buffer.isBuffer(lParam), true);
    assert.equal(wParam.length, 8);
    assert.equal(lParam.length, 8);
    // PLE's makeWindowFullyDraggable uses this exact read before moving.
    const flags = wParam.readInt16LE(0);
    received.push({
      flags,
      leftButton: Boolean(flags & MK_LBUTTON),
      unsignedLParam: lParam.readBigUInt64LE(0),
      signedLParam: lParam.readBigInt64LE(0),
    });
  });
  assert.equal(window.isWindowMessageHooked(WM_MOUSEMOVE), true);

  const lParams = [0x1234567890abcdefn, 0xfedcba9876543210n];
  for (const [index, bits] of lParams.entries()) {
    context.__xenonDispatchBrowserWindowEvent(window.id, 'window-message', {
      message: WM_MOUSEMOVE,
      wParam: String(index === 0 ? MK_LBUTTON : 0x8001),
      lParam: String(BigInt.asIntN(64, bits)),
    });
  }
  assert.deepEqual(received, [
    {flags: 1, leftButton: true, unsignedLParam: lParams[0], signedLParam: lParams[0]},
    {flags: -32767, leftButton: true, unsignedLParam: lParams[1],
      signedLParam: BigInt.asIntN(64, lParams[1])},
  ]);

  window.unhookWindowMessage(WM_MOUSEMOVE);
  assert.equal(window.isWindowMessageHooked(WM_MOUSEMOVE), false);
  context.__xenonDispatchBrowserWindowEvent(window.id, 'window-message', {
    message: WM_MOUSEMOVE, wParam: '1', lParam: '0',
  });
  assert.equal(received.length, 2);
  assert.deepEqual(calls.filter(call => call.command.endsWith('hook-window-message')), [
    {id: window.id, command: 'hook-window-message', details: {message: WM_MOUSEMOVE}},
    {id: window.id, command: 'unhook-window-message', details: {message: WM_MOUSEMOVE}},
  ]);
});

test('BrowserWindow constructor preserves default, explicit, and empty native titles', () => {
  const {BrowserWindow, calls} = createRuntime();
  const windows = [
    new BrowserWindow(),
    new BrowserWindow({title: undefined}),
    new BrowserWindow({title: '迅雷登录'}),
    new BrowserWindow({title: ''}),
  ];
  const titles = ['Electron', 'Electron', '迅雷登录', ''];
  assert.deepEqual(calls.filter(call => call.command === 'create')
      .map(call => call.options.title), titles);
  assert.deepEqual(windows.map(window => window.getTitle()), titles);
  assert.deepEqual(windows.map(window => window.webContents.getTitle()),
      ['', '', '', '']);
});

test('page title updates notify BrowserWindow, apply the native title, then notify webContents', () => {
  const sequence = [];
  const {BrowserWindow, context, calls} = createRuntime((id, command, details) => {
    if (command === 'set-title') sequence.push(['host', id, details.title]);
    return null;
  });
  const window = new BrowserWindow({title: 'initial native title'});
  const contents = window.webContents;
  let previousTitle = window.getTitle();
  let windowEvent;
  window.on('page-title-updated', (event, title, explicitSet) => {
    windowEvent = event;
    assert.equal(event.sender, window);
    assert.equal(event.defaultPrevented, false);
    assert.equal(typeof event.preventDefault, 'function');
    assert.equal(contents.getTitle(), title);
    assert.equal(window.getTitle(), previousTitle);
    sequence.push(['window', title, explicitSet]);
  });
  contents.on('page-title-updated', (event, title, explicitSet) => {
    assert.equal(event.sender, contents);
    assert.notEqual(event, windowEvent);
    assert.equal(contents.getTitle(), title);
    assert.equal(window.getTitle(), title);
    sequence.push(['contents', title, explicitSet]);
  });

  // TH starts without a constructor title, later changes document.title,
  // and clears it when a reusable popup is released.
  for (const [title, explicitSet] of [['迅雷', true], ['download.html', false], ['', true]]) {
    context.__xenonDispatchBrowserWindowEvent(window.id,
        'web-contents-page-title-updated', {title, explicitSet});
    assert.equal(window.getTitle(), title);
    assert.equal(contents.getTitle(), title);
    previousTitle = title;
  }
  assert.deepEqual(sequence, [
    ['window', '迅雷', true], ['host', window.id, '迅雷'], ['contents', '迅雷', true],
    ['window', 'download.html', false], ['host', window.id, 'download.html'],
    ['contents', 'download.html', false],
    ['window', '', true], ['host', window.id, ''], ['contents', '', true],
  ]);
  assert.deepEqual(calls.filter(call => call.command === 'set-title')
      .map(call => call.details.title), ['迅雷', 'download.html', '']);
});

test('BrowserWindow can prevent the native title change while webContents keeps the page title', () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const window = new BrowserWindow({title: 'fixed native title'});
  const received = [];
  let preventedEvent;
  window.on('page-title-updated', (event, title, explicitSet) => {
    assert.equal(window.webContents.getTitle(), '迅雷');
    event.preventDefault();
    assert.equal(event.defaultPrevented, true);
    preventedEvent = event;
    received.push(['window', title, explicitSet]);
  });
  window.webContents.on('page-title-updated', (event, title, explicitSet) => {
    assert.notEqual(event, preventedEvent);
    assert.equal(window.getTitle(), 'fixed native title');
    received.push(['contents', title, explicitSet]);
  });
  context.__xenonDispatchBrowserWindowEvent(window.id,
      'web-contents-page-title-updated', {title: '迅雷', explicitSet: true});
  assert.equal(window.getTitle(), 'fixed native title');
  assert.equal(window.webContents.getTitle(), '迅雷');
  assert.equal(calls.some(call => call.command === 'set-title'), false);
  assert.deepEqual(received, [['window', '迅雷', true], ['contents', '迅雷', true]]);
});

test('destroying BrowserWindow during its title event stops native updates and webContents events', () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const window = new BrowserWindow({title: 'initial native title'});
  let contentsEvents = 0;
  window.on('page-title-updated', () => window.destroy());
  window.webContents.on('page-title-updated', () => contentsEvents++);
  assert.doesNotThrow(() => context.__xenonDispatchBrowserWindowEvent(window.id,
      'web-contents-page-title-updated', {title: 'closing popup', explicitSet: true}));
  assert.equal(window.isDestroyed(), true);
  assert.equal(window.webContents.isDestroyed(), true);
  assert.equal(contentsEvents, 0);
  assert.equal(calls.some(call => call.command === 'set-title'), false);
});

test('explicit native title changes preserve the page title and retain cached title on host failure', () => {
  const failure = new Error('Window host rejected title');
  const {BrowserWindow, context, calls} = createRuntime((id, command, details) => {
    if (command === 'set-title' && details.title === 'rejected') throw failure;
    return null;
  });
  const window = new BrowserWindow({title: 'initial native title'});
  context.__xenonDispatchBrowserWindowEvent(window.id,
      'web-contents-page-title-updated', {title: 'page title', explicitSet: true});
  const titleEvents = [];
  window.on('page-title-updated', () => titleEvents.push('window'));
  window.webContents.on('page-title-updated', () => titleEvents.push('contents'));
  assert.equal(window.setTitle('迅雷登录'), undefined);
  assert.equal(window.getTitle(), '迅雷登录');
  assert.equal(window.webContents.getTitle(), 'page title');
  assert.equal(window.setTitle(''), undefined);
  assert.equal(window.getTitle(), '');
  assert.equal(window.webContents.getTitle(), 'page title');
  assert.throws(() => window.setTitle('rejected'), error => error === failure);
  assert.equal(window.getTitle(), '');
  assert.equal(window.webContents.getTitle(), 'page title');
  assert.deepEqual(titleEvents, []);
  assert.deepEqual(calls.filter(call => call.command === 'set-title'), [
    {id: window.id, command: 'set-title', details: {title: 'page title'}},
    {id: window.id, command: 'set-title', details: {title: '迅雷登录'}},
    {id: window.id, command: 'set-title', details: {title: ''}},
    {id: window.id, command: 'set-title', details: {title: 'rejected'}},
  ]);
});
