// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createPreload() {
  const calls = [];
  let dispatch;
  const bridge = Object.freeze({exposeInMainWorld() {}, executeInMainWorld() {}});
  const context = vm.createContext({__xenonWorker: {
    send: (...args) => calls.push(['send', ...args]),
    invoke: (...args) => { calls.push(['invoke', ...args]); return Promise.resolve(42); },
    setDispatchHandler(handler) { dispatch = handler; },
    contextBridge: bridge,
  }});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'xenon_ipc_worker_bootstrap.js'), 'utf8'), context);
  return {context, calls, dispatch, electron: context.require('electron'), bridge};
}

test('worker preload has restricted modules and hides the native transport', () => {
  const {context, electron, bridge} = createPreload();
  assert.equal(context.process.type, 'service-worker');
  assert.equal(context.process.contextIsolated, true);
  assert.equal(context.process.sandboxed, true);
  assert.equal(context.process.binding, undefined);
  assert.equal(context.require('electron/renderer'), electron);
  assert.equal(electron.contextBridge, bridge);
  assert.equal(context.__xenonWorker, undefined);
  assert.equal(electron.ipcRenderer.sendSync, undefined);
  for (const module of ['node:fs', 'fs', 'child_process', './file', 'module']) {
    assert.throws(() => context.require(module), /unavailable/);
  }
});

test('worker preload IPC dispatch preserves listener removal and once semantics', async () => {
  const {electron, dispatch, calls} = createPreload();
  const ipc = electron.ipcRenderer;
  const received = [];
  const onMessage = (event, value) => {
    assert.equal(event.sender, ipc);
    received.push(value);
  };
  ipc.on('data', onMessage);
  ipc.once('data', onMessage);
  dispatch('data', [1]);
  dispatch('data', [2]);
  ipc.off('data', onMessage);
  dispatch('data', [3]);
  assert.deepEqual(received, [1, 1, 2]);
  assert.equal(ipc.listenerCount('data'), 0);
  assert.equal(await ipc.invoke('calculate', 20, 22), 42);
  ipc.send('event', {ready: true});
  assert.deepEqual(calls, [
    ['invoke', 'calculate', 20, 22], ['send', 'event', {ready: true}],
  ]);
});
