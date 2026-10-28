'use strict';
const {app, BrowserWindow, ipcMain, session, protocol} = require('electron');
const path = require('node:path');
const ORIGIN = 'http://127.0.0.1:9339';
const PAGE = ORIGIN + '/index.html';
const WORKER = ORIGIN + '/worker.js';
const PARTITION = 'xenon-game-host-smoke';
const BINARY = [0, 1, 127, 128, 254, 255, 83, 77, 79, 75, 69];
protocol.registerSchemesAsPrivileged([{scheme: 'xenonsmoke', privileges: {
  standard: true, secure: true, supportFetchAPI: true, corsEnabled: true,
}}]);
const acceptedReports = new Set([
  'frame-isolation', 'protocol-binary', 'hidden-with-throttling-disabled',
  'hidden-with-throttling-enabled', 'reshown-visible', 'worker-preload',
  'worker-isolation', 'worker-identity', 'worker-timers', 'page-complete',
]);
let window;
let smokeSession;
let complete = false;
const results = [];
const workers = new WeakSet();
function mark(check, ok, detail = {}) {
  const result = {suite: 'xenon-game-host-smoke', check, ok: ok === true, ...detail};
  results.push(result);
  console.log('XENON_GAME_SMOKE ' + JSON.stringify(result));
  return result;
}
function requireFrame(event) {
  if (!window || event.type !== 'frame' || event.sender !== window.webContents ||
      event.senderFrame !== window.webContents.mainFrame ||
      event.senderFrame?.url !== PAGE || event.sender.session !== smokeSession) {
    throw new Error('SMOKE_FRAME_IDENTITY_REJECTED');
  }
}
ipcMain.handle('smoke:frame-ready', event => {
  const checks = {
    isMainFrame: event.senderFrame === event.sender.mainFrame,
    hasWindow: BrowserWindow.getAllWindows().some(item => item.webContents === event.sender),
    sameWindow: event.sender === window?.webContents,
    sameSession: event.sender.session === smokeSession,
    committedUrlMatches: event.senderFrame?.url === PAGE,
  };
  mark('frame-identity', Object.values(checks).every(Boolean), checks);
  requireFrame(event);
  return {ok: true, nativeThrottlingDisabled: window.webContents.getBackgroundThrottling() === false};
});
ipcMain.handle('smoke:enable-throttling', event => {
  requireFrame(event);
  window.webContents.setBackgroundThrottling(true);
  const ok = window.webContents.getBackgroundThrottling() === true;
  mark('native-throttling-enabled', ok);
  mark('native-window-stays-hidden', !window.isVisible());
  return ok;
});
ipcMain.handle('smoke:show-window', event => {
  requireFrame(event);
  window.show();
  return true;
});
ipcMain.on('smoke:begin-shutdown-navigation', event => {
  requireFrame(event);
  if (!complete) throw new Error('SMOKE_SUITE_NOT_COMPLETE');
  // Start only after CDP has read the completed page and native results:
  // DevTools may switch targets while a cross-site navigation is pending.
  window.loadURL('http://localhost:9339/delayed-navigation');
});
ipcMain.on('smoke:page-report', (event, check, ok) => {
  requireFrame(event);
  if (!acceptedReports.has(check) || typeof ok !== 'boolean') return;
  mark(check, ok);
  if (check === 'page-complete') {
    // Exercise shutdown while hidden rendering is still enabled. The runner
    // rejects new renderer dumps after it closes the browser.
    window.webContents.setBackgroundThrottling(false);
    window.hide();
    mark('shutdown-background-rendering',
        !window.isVisible() && !window.webContents.getBackgroundThrottling());
    complete = true;
    mark('suite-complete', ok && results.every(result => result.ok));
  }
});
ipcMain.handle('smoke:snapshot', event => {
  requireFrame(event);
  return {complete, results};
});

app.whenReady().then(() => {
  smokeSession = session.fromPartition(PARTITION);
  const preload = path.join(__dirname, 'preload.cjs');
  for (const type of ['frame', 'service-worker']) {
    smokeSession.registerPreloadScript({id: 'smoke-' + type, type, filePath: preload});
  }
  const registered = smokeSession.getPreloadScripts();
  mark('session-preloads-registered', ['frame', 'service-worker'].every(type =>
      registered.some(item => item.id === 'smoke-' + type && item.type === type)));

  smokeSession.serviceWorkers.on('running-status-changed', event => {
    if (event.runningStatus !== 'starting' && event.runningStatus !== 'running') return;
    const worker = smokeSession.serviceWorkers.getWorkerFromVersionID(event.versionId);
    if (!worker || worker.scope !== ORIGIN + '/' || workers.has(worker)) return;
    workers.add(worker);
    worker.ipc.handle('smoke:worker-probe', incoming => {
      const ok = incoming.type === 'service-worker' &&
          incoming.session === smokeSession && incoming.serviceWorker === worker &&
          incoming.serviceWorker.scope === ORIGIN + '/' &&
          incoming.serviceWorker.scriptURL === WORKER &&
          incoming.sender === undefined && incoming.senderFrame === undefined;
      mark('native-worker-identity', ok);
      return {ok};
    });
  });

  smokeSession.protocol.handle('xenonsmoke', request => {
    if (request.url !== 'xenonsmoke://fixture/binary') {
      return new Response(null, {status: 404});
    }
    return new Response(new Uint8Array(BINARY), {status: 200, headers: {
      'content-type': 'application/octet-stream',
      'access-control-allow-origin': ORIGIN,
      'cache-control': 'no-store',
    }});
  });
  window = new BrowserWindow({
    title: 'Xenon Host Smoke', width: 720, height: 420, show: false,
    webPreferences: {
      session: smokeSession,
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      backgroundThrottling: false,
    },
  });
  window.webContents.on('preload-error', () => mark('preload-load', false));
  mark('native-throttling-disabled', window.webContents.getBackgroundThrottling() === false);
  window.loadURL(PAGE);
  setTimeout(() => { if (!complete) mark('suite-timeout', false); }, 30000);
}).catch(() => mark('main-setup', false));

// The parent task owns browser shutdown. Retain the app and window for CDP.
