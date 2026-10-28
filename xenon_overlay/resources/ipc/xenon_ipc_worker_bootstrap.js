// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(() => {
  'use strict';
  const transport = globalThis.__xenonWorker;
  delete globalThis.__xenonWorker;
  const listeners = new Map();
  const validate = (channel, listener) => {
    if (typeof channel !== 'string') throw new TypeError('IPC channel must be a string');
    if (typeof listener !== 'function') throw new TypeError('Listener must be a function');
  };
  const ipcRenderer = {
    send(channel, ...args) { transport.send(channel, ...args); },
    invoke(channel, ...args) { return transport.invoke(channel, ...args); },
    on(channel, listener) {
      validate(channel, listener);
      const list = listeners.get(channel) || [];
      list.push(listener);
      listeners.set(channel, list);
      return this;
    },
    once(channel, listener) {
      validate(channel, listener);
      function once(...args) {
        ipcRenderer.removeListener(channel, once);
        return listener(...args);
      }
      once.listener = listener;
      return this.on(channel, once);
    },
    removeListener(channel, listener) {
      validate(channel, listener);
      const list = listeners.get(channel) || [];
      const index = list.findLastIndex(item => item === listener || item.listener === listener);
      if (index >= 0) list.splice(index, 1);
      if (!list.length) listeners.delete(channel);
      return this;
    },
    removeAllListeners(channel) {
      if (channel === undefined) listeners.clear();
      else listeners.delete(channel);
      return this;
    },
    listenerCount(channel) { return listeners.get(channel)?.length || 0; },
  };
  ipcRenderer.addListener = ipcRenderer.on;
  ipcRenderer.off = ipcRenderer.removeListener;
  transport.setDispatchHandler((channel, args) => {
    const event = {sender: ipcRenderer};
    for (const listener of [...(listeners.get(channel) || [])]) {
      listener(event, ...args);
    }
  });
  const contextBridge = transport.contextBridge;
  const electron = Object.freeze({ipcRenderer, contextBridge});
  Object.defineProperty(globalThis, 'process', {
    value: Object.freeze({type: 'service-worker', contextIsolated: true, sandboxed: true}),
  });
  Object.defineProperty(globalThis, 'require', {
    value(id) {
      if (id === 'electron' || id === 'electron/renderer') return electron;
      throw new Error(`Service worker preload module is unavailable: ${String(id)}`);
    },
  });
})();
