// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
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
    __xenonCloseBrowserWindow(id) {
      calls.push({id, command: 'close'});
    },
    __xenonExitApp(code) {
      calls.push({command: 'exit-app', code});
    },
  });
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readBootstrap(filename), context, {filename});
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

test('WebContents queries the OS process through its native owner', () => {
  const {BrowserWindow, calls} = createRuntime((id, command) =>
      command === 'get-os-process-id' ? 4000 + id : null);
  const window = new BrowserWindow();
  assert.equal(window.webContents.getOSProcessId(), 4000 + window.id);
  assert.equal(calls.at(-1).id, window.id);
  assert.equal(calls.at(-1).command, 'get-os-process-id');
});

test('failed BrowserWindow initialization releases native and JS resources', () => {
  for (const failedCommand of ['set-web-preferences', 'set-minimum-size']) {
    const failure = new Error('native initialization failed');
    const {BrowserWindow, calls, context} = createRuntime((id, command) => {
      if (command === failedCommand) throw failure;
      return null;
    });
    let allClosed = 0;
    context.__xenonElectron.app.on('window-all-closed', () => ++allClosed);
    assert.throws(() => new BrowserWindow({minWidth: 300, minHeight: 200}),
      error => error === failure);
    const created = calls.find(call => call.command === 'create');
    assert.equal(calls.filter(call => call.command === 'close' && call.id === created.id).length, 1);
    assert.equal(BrowserWindow.getAllWindows().length, 0);
    assert.equal(context.__xenonElectron.webContents.getAllWebContents().length, 0);
    assert.equal(allClosed, 0);
  }
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
    () => electron.app.requestSingleInstanceLock(),
    () => electron.app.releaseSingleInstanceLock(),
    () => electron.app.setAsDefaultProtocolClient('fixture'),
    () => electron.app.removeAsDefaultProtocolClient('fixture'),
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

test('declared renderer mappings retain non-home pages, query and hash with Windows path casing', async () => {
  const {BrowserWindow, calls, context} = createRuntime();
  context.__xenonRendererBaseUrl = 'chrome://fixture-host/';
  context.__xenonRendererUrlMappings = [{
    sourcePathPrefix: 'C:\\Fixture\\main-renderer',
    targetBaseUrl: 'chrome://fixture-host/',
  }];
  const window = new BrowserWindow();
  for (const [input, expected] of [
    ['file:///C:/Fixture/main-renderer/clipper.html?windowType=clipper#preview',
      'chrome://fixture-host/clipper.html?windowType=clipper#preview'],
    ['file:///c:/FIXTURE/MAIN-RENDERER/tools/ClipEditor.html?value=a%20b#selection',
      'chrome://fixture-host/tools/ClipEditor.html?value=a%20b#selection'],
  ]) {
    await window.loadURL(input);
    assert.equal(calls.at(-1).url, expected);
    assert.equal(window.webContents.getURL(), expected);
  }
});

test('unmapped file pages remain unchanged even when a renderer base URL exists', async () => {
  const {BrowserWindow, calls, context} = createRuntime();
  context.__xenonRendererBaseUrl = 'chrome://fixture-host/';
  context.__xenonRendererUrlMappings = [{
    sourcePathPrefix: 'C:/Fixture/main-renderer',
    targetBaseUrl: 'chrome://fixture-host/',
  }];
  const window = new BrowserWindow();
  for (const input of [
    'file:///C:/Other/clipper.html?windowType=clipper#preview',
    'file:///C:/Fixture/main-renderer-backup/clipper.html?value=a%20b#selection',
  ]) {
    await window.loadURL(input);
    assert.equal(calls.at(-1).url, input);
    assert.equal(window.webContents.getURL(), input);
  }
});

test('localhost renderer pages remain unchanged instead of loading the configured home page', async () => {
  const {BrowserWindow, calls, context} = createRuntime();
  context.__xenonRendererBaseUrl = 'chrome://fixture-host/index.html';
  const window = new BrowserWindow();
  for (const input of [
    'http://localhost:9527/clipper?windowType=clipper#preview',
    'https://localhost:9528/tools/editor.html?value=a%20b#selection',
  ]) {
    await window.loadURL(input);
    assert.equal(calls.at(-1).url, input);
    assert.equal(window.webContents.getURL(), input);
  }
});

test('renderer file mapping respects POSIX path case and preserves unmatched URLs', async () => {
  const {BrowserWindow, calls, context} = createRuntime();
  context.__xenonPlatform = 'linux';
  context.__xenonRendererBaseUrl = 'chrome://fixture-host/';
  context.__xenonRendererUrlMappings = [{
    sourcePathPrefix: '/opt/Fixture/main-renderer',
    targetBaseUrl: 'chrome://fixture-host/',
  }];
  const window = new BrowserWindow();
  await window.loadURL('file:///opt/Fixture/main-renderer/clipper.html?mode=edit#preview');
  assert.equal(calls.at(-1).url, 'chrome://fixture-host/clipper.html?mode=edit#preview');
  const unmatched = 'file:///opt/fixture/main-renderer/clipper.html?mode=edit#preview';
  await window.loadURL(unmatched);
  assert.equal(calls.at(-1).url, unmatched);
  assert.equal(window.webContents.getURL(), unmatched);
});

test('BrowserWindow applies web preferences to its native owner before navigation', async () => {
  const {BrowserWindow, calls} = createRuntime();
  const window = new BrowserWindow({webPreferences: {webSecurity: false}});
  await window.loadURL('https://fixture.invalid/');
  const relevant = calls.filter(call =>
    ['create', 'set-web-preferences', 'load-url'].includes(call.command));
  assert.deepEqual(relevant.map(call => call.command),
      ['create', 'set-web-preferences', 'load-url']);
  assert.equal(relevant.every(call => call.id === window.id), true);
  assert.deepEqual(relevant[1].details,
      {contextIsolation: true, nodeIntegration: false, webSecurity: false});

  const defaultWindow = new BrowserWindow();
  const defaults = calls.find(call =>
    call.id === defaultWindow.id && call.command === 'set-web-preferences');
  assert.equal(defaults.details.webSecurity, undefined);
});

test('BrowserWindow establishes constructor minimum dimensions before navigation', async () => {
  const {BrowserWindow, calls} = createRuntime();
  const window = new BrowserWindow({width: 320, height: 200,
    minWidth: 480, minHeight: 300, show: false});
  await window.loadURL('https://fixture.invalid/');
  const relevant = calls.filter(call =>
    ['create', 'set-minimum-size', 'load-url'].includes(call.command));
  assert.deepEqual(relevant.map(call => call.command),
      ['create', 'set-minimum-size', 'load-url']);
  assert.deepEqual(relevant[1].details, {width: 480, height: 300});
  // Installing a constraint does not itself resize an existing window.
  assert.deepEqual(Array.from(window.getSize()), [320, 200]);
  assert.deepEqual(Array.from(window.getMinimumSize()), [480, 300]);

  const oneAxis = new BrowserWindow({minHeight: 240});
  assert.deepEqual(Array.from(oneAxis.getMinimumSize()), [0, 240]);
  const invalid = new BrowserWindow({minWidth: '480', minHeight: 20.5});
  assert.deepEqual(Array.from(invalid.getMinimumSize()), [0, 0]);
});

test('BrowserWindow minimum size queries and programmatic resize use native results', () => {
  let minimum = {width: 0, height: 0};
  let bounds = {x: 10, y: 20, width: 640, height: 480};
  const {BrowserWindow} = createRuntime((id, command, details) => {
    if (command === 'get-bounds') return {...bounds};
    if (command === 'set-minimum-size') {
      if (details.width || details.height) minimum = {...details};
      return {...minimum};
    }
    if (command === 'get-minimum-size') return {...minimum};
    if (command === 'set-bounds') {
      bounds = {...bounds, ...details};
      bounds.width = Math.max(bounds.width, minimum.width);
      bounds.height = Math.max(bounds.height, minimum.height);
      return {...bounds};
    }
    return null;
  });
  const window = new BrowserWindow({minWidth: 480, minHeight: 320});
  window.setSize(100, 100);
  assert.deepEqual(Array.from(window.getSize()), [480, 320]);
  window.setMinimumSize(700, 500);
  assert.deepEqual(Array.from(window.getSize()), [480, 320]);
  window.setBounds({width: 200, height: 150});
  assert.deepEqual(Array.from(window.getSize()), [700, 500]);
  window.setMinimumSize(0, 0);
  assert.deepEqual(Array.from(window.getMinimumSize()), [700, 500]);
  minimum = {width: 200, height: 100};
  assert.deepEqual(Array.from(window.getMinimumSize()), [200, 100]);
  window.setMinimumSize(-1, 80);
  assert.deepEqual(Array.from(window.getMinimumSize()), [0, 80]);
});

test('BrowserWindow rejects invalid minimum sizes before IPC and retains state on native failure', () => {
  let fail = false;
  const {BrowserWindow, calls} = createRuntime((id, command) => {
    if (fail && command === 'set-minimum-size') throw new Error('native failure');
    return null;
  });
  const window = new BrowserWindow({minWidth: 300, minHeight: 200});
  const before = calls.length;
  for (const value of [undefined, null, '500', 1.5, NaN, Infinity,
                       2147483648, -2147483649]) {
    assert.throws(() => window.setMinimumSize(value, 200), /32-bit integers/);
    assert.throws(() => window.setMinimumSize(300, value), /32-bit integers/);
  }
  assert.equal(calls.length, before);
  fail = true;
  assert.throws(() => window.setMinimumSize(500, 400), /native failure/);
  assert.deepEqual(Array.from(window.getMinimumSize()), [300, 200]);
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

test('native close requests honor cancellation and app activation restores the same window', async () => {
  const runtime = createRuntime((id, command) => {
    if (command === 'show' || command === 'hide') {
      runtime.context.__xenonDispatchBrowserWindowEvent(id, command);
    }
  });
  const {BrowserWindow, context, calls} = runtime;
  const {app} = context.__xenonElectron;
  const window = new BrowserWindow();
  let closeCount = 0;
  let closedCount = 0;
  let allClosedCount = 0;
  window.on('close', event => {
    ++closeCount;
    event.preventDefault();
    window.hide();
  });
  window.on('closed', () => ++closedCount);
  app.on('window-all-closed', () => ++allClosedCount);
  app.whenReady().then(() => app.on('activate', (event, hasVisibleWindows) => {
    assert.equal(event.sender, app);
    assert.equal(hasVisibleWindows, false);
    window.show();
  }));
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);

  for (let i = 0; i < 2; ++i) {
    context.__xenonDispatchBrowserWindowEvent(window.id, 'close-requested');
    assert.equal(window.isVisible(), false);
    assert.equal(window.isDestroyed(), false);
    context.__xenonDispatchAppEvent('activate', [false]);
    assert.equal(window.isVisible(), true);
  }
  assert.equal(closeCount, 2);
  assert.equal(closedCount, 0);
  assert.equal(allClosedCount, 0);
  assert.equal(calls.filter(call => call.command === 'create').length, 1);
  assert.equal(calls.filter(call => call.command === 'close').length, 0);
});

test('allowed native close emits closed then window-all-closed and activation recreates once', async () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const events = [];
  let window = new BrowserWindow();
  const first = window;
  window.on('close', () => events.push('close'));
  window.on('closed', () => { events.push('closed'); window = null; });
  app.on('window-all-closed', () => events.push('window-all-closed'));
  app.on('activate', () => {
    if (!window) window = new BrowserWindow();
  });
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  context.__xenonDispatchBrowserWindowEvent(first.id, 'close-requested');
  context.__xenonDispatchBrowserWindowEvent(first.id, 'closed');
  context.__xenonDispatchBrowserWindowEvent(first.id, 'close-requested');
  assert.deepEqual(events, ['close', 'closed', 'window-all-closed']);
  assert.equal(first.webContents.isDestroyed(), true);
  assert.equal(BrowserWindow.getAllWindows().length, 0);
  context.__xenonDispatchAppEvent('activate', [false]);
  context.__xenonDispatchAppEvent('activate', [true]);
  assert.notEqual(window, first);
  assert.equal(BrowserWindow.getAllWindows().length, 1);
  assert.equal(calls.filter(call => call.command === 'create').length, 2);
  assert.equal(calls.filter(call => call.command === 'close').length, 1);
});

test('hidden auxiliary windows remain alive for getAllWindows and final-close notification', async () => {
  const {BrowserWindow, context} = createRuntime();
  const {app} = context.__xenonElectron;
  const main = new BrowserWindow();
  const auxiliary = new BrowserWindow({show: false});
  let allClosed = 0;
  let created = 0;
  app.on('window-all-closed', () => ++allClosed);
  // This is the installed TH application's activate condition. Hidden
  // auxiliary windows count as live windows, even with no visible main window.
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      new BrowserWindow();
      ++created;
    }
  });
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  main.destroy();
  assert.deepEqual(Array.from(BrowserWindow.getAllWindows()), [auxiliary]);
  assert.equal(allClosed, 0);
  context.__xenonDispatchAppEvent('activate', [false]);
  assert.equal(created, 0);
  auxiliary.destroy();
  assert.equal(allClosed, 1);
  context.__xenonDispatchAppEvent('activate', [false]);
  assert.equal(created, 1);
});

test('paired and reentrant close callbacks finish once before window-all-closed', () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const parent = new BrowserWindow();
  const child = new BrowserWindow({parent});
  const events = [];
  parent.on('close', () => { events.push('parent-close'); parent.close(); });
  parent.on('closed', () => { events.push('parent-closed'); child.destroy(); });
  child.on('closed', () => {
    events.push('child-closed');
    parent.destroy();
    child.close();
  });
  app.on('window-all-closed', () => events.push('window-all-closed'));
  const closeNative = context.__xenonCloseBrowserWindow;
  context.__xenonCloseBrowserWindow = id => {
    closeNative(id);
    // Exercise native completion re-entering while destroy is still running.
    context.__xenonDispatchBrowserWindowEvent(id, 'closed');
  };
  parent.close();
  for (const window of [parent, child]) {
    window.destroy();
    window.close();
    context.__xenonDispatchBrowserWindowEvent(window.id, 'closed');
  }
  assert.deepEqual(events,
      ['parent-close', 'parent-closed', 'child-closed', 'window-all-closed']);
  assert.deepEqual(calls.filter(call => call.command === 'close').map(call => call.id),
      [parent.id, child.id]);
});

test('a replacement created in closed suppresses premature window-all-closed', () => {
  const {BrowserWindow, context} = createRuntime();
  const first = new BrowserWindow();
  const events = [];
  let replacement;
  first.on('closed', () => {
    events.push('first-closed');
    replacement = new BrowserWindow();
    replacement.on('closed', () => events.push('replacement-closed'));
  });
  context.__xenonElectron.app.on('window-all-closed', () => events.push('window-all-closed'));
  first.destroy();
  assert.deepEqual(events, ['first-closed']);
  replacement.close();
  assert.deepEqual(events, ['first-closed', 'replacement-closed', 'window-all-closed']);
});

test('native closed completion never sends another close and duplicate requests stay inert', () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const window = new BrowserWindow();
  const events = [];
  window.on('close', () => events.push('close'));
  window.on('closed', () => events.push('closed'));
  context.__xenonElectron.app.on('window-all-closed', () => events.push('window-all-closed'));
  for (const name of ['closed', 'closed', 'close-requested']) {
    context.__xenonDispatchBrowserWindowEvent(window.id, name);
  }
  assert.deepEqual(events, ['closed', 'window-all-closed']);
  assert.equal(calls.some(call => call.command === 'close'), false);
});

test('bounds queries and setters do not consume queued move and resize events', () => {
  let actual = {x: 10, y: 20, width: 300, height: 200};
  const {BrowserWindow, context} = createRuntime((_id, command, details) => {
    if (command === 'set-bounds') actual = {...actual, ...details};
    if (command === 'get-bounds' || command === 'set-bounds') return {...actual};
    return null;
  });
  const window = new BrowserWindow();
  const events = [];
  window.on('move', () => events.push('move'));
  window.on('resize', () => events.push('resize'));
  window.setBounds({x: 40, width: 500});
  window.getBounds();
  context.__xenonDispatchBrowserWindowEvent(window.id, 'bounds-changed', {...actual});
  assert.deepEqual(events, ['move', 'resize']);
  context.__xenonDispatchBrowserWindowEvent(window.id, 'bounds-changed', {...actual});
  assert.deepEqual(events, ['move', 'resize']);
  actual = {...actual, y: 80};
  window.getBounds();
  context.__xenonDispatchBrowserWindowEvent(window.id, 'bounds-changed', {...actual});
  assert.deepEqual(events, ['move', 'resize', 'move']);
});

function createTransactionalWindowRuntime() {
  const states = new Map();
  const state = id => {
    if (!states.has(id)) states.set(id, {
      bounds: {x: 0, y: 0, width: 800, height: 600}, revision: 0,
    });
    return states.get(id);
  };
  const change = (id, patch) => {
    const current = state(id);
    const before = current.bounds;
    current.bounds = {...before, ...patch};
    const moved = before.x !== current.bounds.x || before.y !== current.bounds.y;
    const resized = before.width !== current.bounds.width ||
        before.height !== current.bounds.height;
    return {windowId: id, revision: ++current.revision,
      bounds: {...current.bounds}, moved, resized};
  };
  let additionalChange;
  const runtime = createRuntime((id, command, details) => {
    const current = state(id);
    const changes = [];
    let value = null;
    if (command === 'set-bounds') changes.push(change(id, details));
    if (command === 'center') changes.push(change(id, {x: 40, y: 30}));
    if (command === 'set-minimum-size') {
      changes.push(change(id, {
        width: Math.max(current.bounds.width, details.width),
        height: Math.max(current.bounds.height, details.height),
      }));
      value = {width: details.width, height: details.height};
    }
    if (command === 'get-bounds' || command === 'set-bounds')
      value = {...current.bounds};
    if (command === 'screen') value = {id: 1, bounds: {...current.bounds}};
    if (additionalChange) changes.push(...additionalChange(id, command));
    return {__xenonWindowCall: true, value,
      boundsRevision: current.revision, boundsChanges: changes};
  });
  return {...runtime, state, change,
    setAdditionalChange(callback) { additionalChange = callback; },
    dispatch(change) {
      runtime.context.__xenonDispatchBrowserWindowEvent(
          change.windowId, 'bounds-changed', {
            ...change.bounds, revision: change.revision,
            moved: change.moved, resized: change.resized,
          });
    },
  };
}

test('programmatic bounds events complete before a later show listener is installed', () => {
  const {BrowserWindow} = createTransactionalWindowRuntime();
  const window = new BrowserWindow({show: false});
  const events = [];
  window.on('move', () => events.push('move'));
  window.on('resize', () => events.push('resize'));
  window.setMinimumSize(1180, 500);
  assert.deepEqual(events, ['resize']);
  window.setBounds({width: 1200, height: 700});
  window.setSize(1200, 700);
  assert.deepEqual(events, ['resize', 'resize']);
  const saved = window.getBounds();
  window.setPosition(-20000, -20000);
  assert.deepEqual(events, ['resize', 'resize', 'move']);
  let resizedDuringShow = false;
  window.on('resize', () => { resizedDuringShow = true; });
  window.show();
  assert.equal(resizedDuringShow, false);
  window.setBounds(saved);
  assert.deepEqual({...window.getBounds()}, {...saved});
  assert.deepEqual(Object.keys(window.getBounds()).sort(), ['height', 'width', 'x', 'y']);
});

test('earlier native bounds events remain observable without reverting newer geometry', () => {
  const {BrowserWindow, change, dispatch} = createTransactionalWindowRuntime();
  const window = new BrowserWindow();
  const events = [];
  window.on('move', () => events.push('move'));
  window.on('resize', () => events.push('resize'));
  const drag = change(window.id, {x: 90});
  window.getBounds();
  window.setSize(900, 700);
  const latest = window.getBounds();
  assert.deepEqual(events, ['resize']);
  dispatch(drag);
  assert.deepEqual(events, ['resize', 'move']);
  assert.deepEqual({...window._bounds}, {...latest});
  dispatch(drag);
  assert.deepEqual(events, ['resize', 'move']);
  const next = change(window.id, {height: 720});
  window.getBounds();
  dispatch(next);
  assert.deepEqual(events, ['resize', 'move', 'resize']);
});

test('synchronous resize listeners can resize again or destroy their window', () => {
  const {BrowserWindow} = createTransactionalWindowRuntime();
  const window = new BrowserWindow();
  window.once('resize', () => window.setSize(1000, 800));
  window.setSize(900, 700);
  assert.equal(window._bounds.width, 1000);
  assert.equal(window._bounds.height, 800);
  const events = [];
  window.on('move', () => { events.push('move'); window.destroy(); });
  window.on('resize', () => events.push('resize'));
  window.setBounds({x: 100, width: 1100});
  assert.deepEqual(events, ['move']);
  assert.equal(window.isDestroyed(), true);
});

test('command state is committed before geometry listeners and survives reentrant setters', () => {
  const {BrowserWindow, change, setAdditionalChange} = createTransactionalWindowRuntime();
  const first = new BrowserWindow();
  const second = new BrowserWindow();
  const child = new BrowserWindow();
  setAdditionalChange((id, command) => command === 'set-parent-window' ?
    [change(id, {x: 100})] : []);
  child.once('move', () => {
    assert.equal(child.getParentWindow(), first);
    child.setParentWindow(second);
  });
  child.setParentWindow(first);
  assert.equal(child.getParentWindow(), second);
  child.once('resize', () => {
    assert.equal(child._minSize.width, 900);
    child.setMinimumSize(1000, 800);
  });
  child.setMinimumSize(900, 700);
  assert.deepEqual({...child._minSize}, {width: 1000, height: 800});
});

test('paired bounds replies commit all windows before invoking reentrant listeners', () => {
  const {BrowserWindow, change, setAdditionalChange} = createTransactionalWindowRuntime();
  const parent = new BrowserWindow();
  const child = new BrowserWindow();
  setAdditionalChange((id, command) => id === parent.id && command === 'set-bounds' ?
    [change(child.id, {x: 80})] : []);
  parent.once('move', () => {
    assert.equal(child._bounds.x, 80);
    child.setPosition(120, 130);
  });
  parent.setPosition(80, 90);
  assert.equal(child._bounds.x, 120);
  assert.equal(child._bounds.y, 130);
});

test('partial setters retain native geometry and size and position getters query it', () => {
  const {BrowserWindow, change, calls} = createTransactionalWindowRuntime();
  const window = new BrowserWindow();
  change(window.id, {x: 250, y: 160});
  window.setSize(900, 700);
  assert.deepEqual({...calls.at(-1).details}, {width: 900, height: 700});
  assert.deepEqual([...window.getPosition()], [250, 160]);
  change(window.id, {width: 1000, height: 800});
  window.setPosition(80, 70);
  assert.deepEqual({...calls.at(-1).details}, {x: 80, y: 70});
  assert.deepEqual([...window.getSize()], [1000, 800]);
});

test('real show-time geometry changes still emit resize and screen replies are unwrapped', () => {
  const {BrowserWindow, context, change, setAdditionalChange} =
      createTransactionalWindowRuntime();
  const window = new BrowserWindow({show: false});
  setAdditionalChange((id, command) => command === 'show' ?
    [change(id, {width: 950})] : []);
  let resized = 0;
  window.on('resize', () => ++resized);
  window.show();
  assert.equal(resized, 1);
  assert.equal(window._bounds.width, 950);
  const display = context.__xenonElectron.screen.getPrimaryDisplay();
  assert.equal(display.id, 1);
  assert.equal(display.__xenonWindowCall, undefined);
});

test('destroyed windows ignore queued native events while live windows still receive them', () => {
  const {BrowserWindow, context, calls} = createRuntime();
  const window = new BrowserWindow();
  const live = new BrowserWindow();
  const events = [];
  for (const name of ['hide', 'blur', 'resize', 'focus', 'page-title-updated']) {
    window.on(name, () => {
      events.push(name);
      window.focus();
    });
  }
  live.on('blur', () => events.push('live-blur'));
  window.destroy();
  const nativeCalls = calls.length;
  for (const name of ['hide', 'blur', 'focus', 'close-requested', 'closed'])
    context.__xenonDispatchBrowserWindowEvent(window.id, name);
  context.__xenonDispatchBrowserWindowEvent(window.id, 'bounds-changed',
      {x: 0, y: 0, width: 320, height: 240});
  context.__xenonDispatchBrowserWindowEvent(window.id, 'web-contents-page-title-updated',
      {title: 'late title', explicitSet: true});
  assert.deepEqual(events, []);
  assert.equal(calls.length, nativeCalls);
  context.__xenonDispatchBrowserWindowEvent(live.id, 'blur');
  assert.deepEqual(events, ['live-blur']);
});

test('application activation waits for ready listeners and whenReady then preserves late events', async () => {
  const {context} = createRuntime();
  const {app} = context.__xenonElectron;
  const events = [];
  const earlyArgs = [false];
  context.__xenonDispatchAppEvent('activate', earlyArgs);
  earlyArgs[0] = true;
  app.on('ready', () => {
    events.push('ready');
    context.__xenonDispatchAppEvent('activate', [true]);
  });
  app.whenReady().then(() => {
    events.push('when-ready');
    app.on('activate', (event, hasVisibleWindows) => {
      assert.equal(event.sender, app);
      assert.equal(typeof event.preventDefault, 'function');
      events.push(['activate', hasVisibleWindows]);
    });
  });
  assert.equal(app.isReady(), false);
  context.__xenonMarkAppReady();
  assert.deepEqual(events, ['ready']);
  await new Promise(queueMicrotask);
  assert.deepEqual(events, ['ready', 'when-ready', ['activate', false], ['activate', true]]);
  context.__xenonMarkAppReady();
  context.__xenonDispatchAppEvent('activate', [false]);
  context.__xenonDispatchAppEvent('activate', [true]);
  assert.deepEqual(events, ['ready', 'when-ready', ['activate', false], ['activate', true],
    ['activate', false], ['activate', true]]);
});

test('shutdown before ready discards queued and subsequent application events', async () => {
  const {context, calls, BrowserWindow} = createRuntime();
  const {app} = context.__xenonElectron;
  app.on('activate', () => new BrowserWindow());
  context.__xenonDispatchAppEvent('activate', [false]);
  context.__xenonShutdownApp();
  context.__xenonDispatchAppEvent('activate', [false]);
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  context.__xenonDispatchAppEvent('activate', [true]);
  assert.equal(calls.some(call => call.command === 'create'), false);
});

test('process exit in ready prevents the pending activation microtask from reopening windows', async () => {
  const {context, calls, BrowserWindow} = createRuntime();
  const {app} = context.__xenonElectron;
  const events = [];
  app.on('ready', () => context.process.exit());
  app.on('before-quit', () => events.push('before-quit'));
  app.on('will-quit', () => events.push('will-quit'));
  app.on('quit', () => events.push('quit'));
  app.whenReady().then(() => {
    app.on('activate', () => new BrowserWindow());
    context.__xenonDispatchAppEvent('activate', [false]);
  });
  context.__xenonDispatchAppEvent('activate', [false]);
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  context.__xenonDispatchAppEvent('activate', [true]);
  assert.deepEqual(events, ['quit']);
  assert.equal(calls.some(call => call.command === 'create'), false);
  assert.deepEqual(calls.filter(call => call.command === 'exit-app'),
      [{command: 'exit-app', code: 0}]);
});

test('shutdown during activation clears remaining events and rejects reentrant activation', async () => {
  const {context} = createRuntime();
  const {app} = context.__xenonElectron;
  const events = [];
  app.on('activate', () => {
    events.push('activate');
    context.__xenonShutdownApp();
  });
  app.on('before-quit', () => {
    events.push('before-quit');
    context.__xenonDispatchAppEvent('activate', [false]);
  });
  app.on('quit', () => events.push('quit'));
  context.__xenonDispatchAppEvent('activate', [false]);
  context.__xenonDispatchAppEvent('activate', [true]);
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  context.__xenonDispatchAppEvent('activate', [false]);
  assert.deepEqual(events, ['activate', 'before-quit', 'quit']);
});

test('app.quit closes the main and hidden player helper before exiting the container once', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const main = new BrowserWindow();
  const helper = new BrowserWindow({show: false});
  const events = [];
  app.on('before-quit', () => events.push('before-quit'));
  for (const [name, window] of [['main', main], ['helper', helper]]) {
    window.on('close', () => events.push(name + '-close'));
    window.on('closed', () => events.push(name + '-closed'));
  }
  app.on('window-all-closed', () => events.push('window-all-closed'));
  app.on('will-quit', () => events.push('will-quit'));
  app.on('quit', (event, code) => events.push(['quit', code]));
  context.process.on('exit', code => events.push(['process-exit', code]));
  app.quit();
  app.quit();
  app.exit(1);
  context.__xenonShutdownApp();
  assert.deepEqual(events, ['before-quit', 'main-close', 'main-closed',
    'helper-close', 'helper-closed', 'will-quit', ['quit', 0], ['process-exit', 0]]);
  assert.equal(BrowserWindow.getAllWindows().length, 0);
  assert.deepEqual(calls.filter(call => call.command === 'exit-app'),
      [{command: 'exit-app', code: 0}]);
});

test('before-quit cancellation preserves windows and later activation and quit remain usable', async () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const window = new BrowserWindow();
  const events = [];
  app.once('before-quit', event => {
    events.push('cancel-quit');
    event.preventDefault();
    context.__xenonDispatchAppEvent('activate', [false]);
  });
  app.on('activate', () => events.push('activate'));
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  app.quit();
  assert.equal(window.isDestroyed(), false);
  assert.deepEqual(events, ['cancel-quit']);
  assert.equal(calls.some(call => call.command === 'exit-app'), false);
  context.__xenonDispatchAppEvent('activate', [true]);
  assert.deepEqual(events, ['cancel-quit', 'activate']);
  app.quit();
  assert.equal(window.isDestroyed(), true);
  assert.equal(calls.filter(call => call.command === 'exit-app').length, 1);
});

test('a helper close veto cancels quit without retrying that close or closing later windows', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const main = new BrowserWindow();
  const helper = new BrowserWindow({show: false});
  const later = new BrowserWindow({show: false});
  const events = [];
  const veto = event => { events.push('helper-close'); event.preventDefault(); };
  helper.on('close', veto);
  app.on('before-quit', () => events.push('before-quit'));
  app.on('will-quit', () => events.push('will-quit'));
  app.on('window-all-closed', () => events.push('window-all-closed'));
  app.quit();
  assert.equal(main.isDestroyed(), true);
  assert.equal(helper.isDestroyed(), false);
  assert.equal(later.isDestroyed(), false);
  assert.deepEqual(events, ['before-quit', 'helper-close']);
  assert.equal(calls.some(call => call.command === 'exit-app'), false);
  helper.removeListener('close', veto);
  app.quit();
  assert.deepEqual(events, ['before-quit', 'helper-close', 'before-quit', 'will-quit']);
  assert.equal(BrowserWindow.getAllWindows().length, 0);
  assert.equal(calls.filter(call => call.command === 'exit-app').length, 1);
});

test('will-quit cancellation permits a subsequent activation to create a new window', async () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  let window = new BrowserWindow();
  const first = window;
  app.on('activate', () => { window = new BrowserWindow(); });
  app.once('will-quit', event => {
    event.preventDefault();
    context.__xenonDispatchAppEvent('activate', [false]);
    assert.throws(() => new BrowserWindow(), {code: 'ERR_APP_QUITTING'});
  });
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  app.quit();
  assert.equal(first.isDestroyed(), true);
  assert.equal(BrowserWindow.getAllWindows().length, 0);
  assert.equal(calls.some(call => call.command === 'exit-app'), false);
  context.__xenonDispatchAppEvent('activate', [false]);
  assert.notEqual(window, first);
  app.quit();
  assert.equal(window.isDestroyed(), true);
  assert.equal(calls.filter(call => call.command === 'exit-app').length, 1);
});

test('app.exit skips cancellable events and force destroys all windows with its exit code', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const events = [];
  const windows = [new BrowserWindow(), new BrowserWindow({show: false})];
  for (const name of ['before-quit', 'will-quit', 'window-all-closed']) {
    app.on(name, event => { events.push(name); event?.preventDefault(); });
  }
  for (const window of windows) {
    window.on('close', event => { events.push('close'); event.preventDefault(); });
    window.on('closed', () => events.push('closed'));
  }
  app.on('quit', (event, code) => { events.push(['quit', code]); app.exit(9); });
  app.exit(7);
  assert.deepEqual(events, ['closed', 'closed', ['quit', 7]]);
  assert.equal(windows.every(window => window.isDestroyed()), true);
  assert.deepEqual(calls.filter(call => call.command === 'exit-app'),
      [{command: 'exit-app', code: 7}]);
});

test('quit callbacks cannot reenter quit or activate or rebuild windows during shutdown', async () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const window = new BrowserWindow();
  const events = [];
  const attemptReentry = name => {
    events.push(name);
    app.quit();
    context.__xenonDispatchAppEvent('activate', [false]);
    assert.throws(() => new BrowserWindow(), {code: 'ERR_APP_QUITTING'});
  };
  app.on('before-quit', () => attemptReentry('before-quit'));
  window.on('close', () => attemptReentry('close'));
  window.on('closed', () => attemptReentry('closed'));
  app.on('will-quit', () => attemptReentry('will-quit'));
  app.on('quit', () => attemptReentry('quit'));
  app.on('activate', () => events.push('activate'));
  context.__xenonMarkAppReady();
  await new Promise(queueMicrotask);
  app.quit();
  assert.deepEqual(events, ['before-quit', 'close', 'closed', 'will-quit', 'quit']);
  assert.equal(calls.filter(call => call.command === 'create').length, 1);
  assert.equal(calls.filter(call => call.command === 'exit-app').length, 1);
});

test('app.quit inside a window close listener waits for its original close and honors its veto', () => {
  for (const veto of [false, true]) {
    const {context, BrowserWindow, calls} = createRuntime();
    const {app} = context.__xenonElectron;
    const window = new BrowserWindow();
    const helper = new BrowserWindow({show: false});
    let closeCount = 0;
    window.on('close', event => {
      ++closeCount;
      app.quit();
      if (veto) event.preventDefault();
    });
    window.close();
    assert.equal(closeCount, 1);
    assert.equal(window.isDestroyed(), !veto);
    assert.equal(helper.isDestroyed(), !veto);
    assert.equal(calls.filter(call => call.command === 'exit-app').length, veto ? 0 : 1);
  }
});

test('a throwing close listener cancels its reentrant quit without repeating the failing handler', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const window = new BrowserWindow();
  const helper = new BrowserWindow({show: false});
  let closeCount = 0;
  const handler = () => {
    ++closeCount;
    app.quit();
    throw new Error('fixture close failure');
  };
  window.on('close', handler);
  assert.throws(() => window.close(), /fixture close failure/);
  assert.equal(closeCount, 1);
  assert.equal(window.isDestroyed(), false);
  assert.equal(helper.isDestroyed(), false);
  assert.equal(calls.some(call => call.command === 'exit-app'), false);
  window.removeListener('close', handler);
  app.quit();
  assert.equal(BrowserWindow.getAllWindows().length, 0);
  assert.equal(calls.filter(call => call.command === 'exit-app').length, 1);
});

test('app.exit can override an in-progress cancellable quit without repeating its events', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const window = new BrowserWindow();
  const events = [];
  app.on('before-quit', event => {
    events.push('before-quit');
    app.exit(4);
    event.preventDefault();
  });
  app.on('will-quit', () => events.push('will-quit'));
  app.on('quit', (event, code) => events.push(['quit', code]));
  app.quit();
  assert.deepEqual(events, ['before-quit', ['quit', 4]]);
  assert.equal(window.isDestroyed(), true);
  assert.deepEqual(calls.filter(call => call.command === 'exit-app'),
      [{command: 'exit-app', code: 4}]);
});

test('native shutdown during quit cleans all windows once without requesting native exit', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const windows = [new BrowserWindow(), new BrowserWindow({show: false})];
  const events = [];
  app.on('before-quit', event => {
    events.push('before-quit');
    context.__xenonShutdownApp();
    event.preventDefault();
  });
  app.on('will-quit', event => {
    events.push('will-quit');
    context.__xenonShutdownApp();
    event.preventDefault();
  });
  app.on('quit', () => {
    events.push('quit');
    context.__xenonShutdownApp();
  });
  app.quit();
  context.__xenonShutdownApp();
  assert.deepEqual(events, ['before-quit', 'will-quit', 'quit']);
  assert.equal(windows.every(window => window.isDestroyed()), true);
  assert.equal(calls.some(call => call.command === 'exit-app'), false);
});

test('force exit continues cleanup and requests termination even when a closed listener throws', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const first = new BrowserWindow();
  const helper = new BrowserWindow({show: false});
  first.on('closed', () => { throw new Error('fixture cleanup failure'); });
  assert.throws(() => context.__xenonElectron.app.exit(5), /fixture cleanup failure/);
  assert.equal(first.isDestroyed(), true);
  assert.equal(helper.isDestroyed(), true);
  assert.deepEqual(calls.filter(call => call.command === 'exit-app'),
      [{command: 'exit-app', code: 5}]);
});

test('missing native exit support fails before lifecycle events or window cleanup', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const window = new BrowserWindow();
  const events = [];
  for (const name of ['before-quit', 'will-quit', 'quit']) app.on(name, () => events.push(name));
  delete context.__xenonExitApp;
  for (const operation of [() => app.quit(), () => app.exit(0), () => context.process.exit(0)]) {
    assert.throws(operation, {code: 'ERR_NOT_SUPPORTED'});
  }
  assert.deepEqual(events, []);
  assert.equal(window.isDestroyed(), false);
  assert.equal(calls.some(call => call.command === 'close'), false);
  // Host-driven cleanup remains possible without an exit hook.
  context.__xenonShutdownApp();
  context.__xenonShutdownApp();
  assert.deepEqual(events, ['before-quit', 'will-quit', 'quit']);
  assert.equal(window.isDestroyed(), true);
});

test('a bound exit hook with no native owner fails before cleanup and accepts later owner binding', () => {
  const {context, BrowserWindow, calls} = createRuntime();
  const {app} = context.__xenonElectron;
  const helper = new BrowserWindow({show: false});
  const events = [];
  let canExit = false;
  context.__xenonCanExitApp = () => canExit;
  for (const name of ['before-quit', 'will-quit', 'quit']) app.on(name, () => events.push(name));
  for (const operation of [() => app.quit(), () => app.exit(0), () => context.process.exit(0)]) {
    assert.throws(operation, {code: 'ERR_NOT_SUPPORTED'});
  }
  assert.deepEqual(events, []);
  assert.equal(helper.isDestroyed(), false);
  assert.equal(calls.some(call => ['close', 'exit-app'].includes(call.command)), false);
  canExit = true;
  app.quit();
  assert.deepEqual(events, ['before-quit', 'will-quit', 'quit']);
  assert.equal(helper.isDestroyed(), true);
  assert.deepEqual(calls.filter(call => call.command === 'exit-app'),
      [{command: 'exit-app', code: 0}]);
});
