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
    __xenonCloseBrowserWindow(id) {
      calls.push({id, command: 'close'});
    },
    __xenonExitApp(code) {
      calls.push({command: 'exit-app', code});
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
