// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Self-contained main module for the chrome://xenon-player-by-elec/ IPC
// diagnostics page. Keep this limited to generic Electron-compatible APIs so
// the page tests the container rather than an embedding application's code.
const {app, ipcMain} = require('electron');

let counter = 0;

function increment(amount) {
  counter += Number(amount) || 0;
  return {counter};
}

ipcMain.handle('test:is-ready', () => app.isReady());

ipcMain.handle('test:async-add', (_event, left, right) => left + right);

ipcMain.handle(
    'test:delayed-add',
    (_event, left, right) =>
        new Promise(resolve => setTimeout(() => resolve(left + right), 25)));

ipcMain.handle('test:runtime-info', () => {
  const {AsyncLocalStorage} = require('node:async_hooks');
  const asyncLocalStorage = new AsyncLocalStorage();
  return {
    versions: process.versions,
    platform: process.platform,
    arch: process.arch,
    performance: {
      now: performance.now(),
      timeOrigin: performance.timeOrigin,
      uptime: process.uptime(),
      hrtime: process.hrtime(),
      perfHooksIsGlobal:
          require('node:perf_hooks').performance === performance,
    },
    asyncHooks: {
      runAndGetStore: asyncLocalStorage.run(
          'xenon-context', () => asyncLocalStorage.getStore()),
    },
  };
});

ipcMain.handle('test:app-paths', () => ({
  app: app.getAppPath(),
  userData: app.getPath('userData'),
  temp: app.getPath('temp'),
  exe: app.getPath('exe'),
}));

ipcMain.on('test:increment', (event, amount) => {
  const result = increment(amount);
  event.returnValue = result;
  event.reply('test:reply-result', result);
});

// Electron keeps event listeners and invoke handlers in separate registries,
// so a diagnostic channel can intentionally exercise both transports.
ipcMain.handle('test:increment', (_event, amount) => increment(amount));

ipcMain.on('test:get-sync', event => {
  event.returnValue = counter;
});
