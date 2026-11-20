'use strict';

const {app, BrowserWindow, ipcMain} = require('electron');
const path = require('node:path');
const windows = [];
const transports = new Set(['send', 'postMessage']);

ipcMain.on('microtask-order:request', (event, transport) => {
  if (!transports.has(transport) ||
      !windows.some(window => window.webContents === event.sender)) {
    return;
  }
  const channel = 'microtask-order:response:' + transport;
  const payload = {transport};
  try {
    if (transport === 'postMessage') {
      // This must use the transferable-message delivery path, even with no ports.
      event.sender.postMessage(channel, payload, []);
    } else {
      event.sender.send(channel, payload);
    }
  } catch (error) {
    event.sender.send(channel, {transport, error: String(error)});
  }
});

app.whenReady().then(async () => {
  for (const contextIsolation of [true, false]) {
    const window = new BrowserWindow({
      title: 'Xenon Microtask Order: ' + (contextIsolation ? 'isolated' : 'shared'),
      width: 640,
      height: 360,
      show: false,
      webPreferences: {
        preload: path.join(__dirname, 'preload.cjs'),
        contextIsolation,
        nodeIntegration: false,
        sandbox: false,
        backgroundThrottling: false,
      },
    });
    windows.push(window);
    await window.loadFile(path.join(__dirname, 'index.html'), {
      query: {contextIsolation: contextIsolation ? '1' : '0'},
    });
  }
}).catch(error => {
  console.error('MICROTASK_ORDER_SETUP_FAILED', error);
});

// Keep both documents alive so the runner can inspect their actual page results.
