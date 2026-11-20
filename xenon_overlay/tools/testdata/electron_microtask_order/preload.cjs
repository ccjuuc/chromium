'use strict';

const {contextBridge, ipcRenderer} = require('electron');
const api = {
  contextIsolated: process.contextIsolated === true,
  request(transport, callback) {
    if (!['send', 'postMessage'].includes(transport) ||
        typeof callback !== 'function') {
      throw new TypeError('Expected a supported transport and callback');
    }
    ipcRenderer.once('microtask-order:response:' + transport, (_event, payload) => {
      callback(payload);
    });
    ipcRenderer.send('microtask-order:request', transport);
  },
};

if (process.contextIsolated) {
  contextBridge.exposeInMainWorld('microtaskOrderHost', api);
} else {
  globalThis.microtaskOrderHost = api;
}
