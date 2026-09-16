// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {promiseHooks} = require('node:v8');
const vm = require('node:vm');

function fixture(kind, t) {
  let dispatch, realmPromise, stop;
  const sent = [];
  const install = (init, before, after) => {
    const owned = new WeakSet();
    stop = promiseHooks.createHook({
      init(promise) {
        if (!(promise instanceof realmPromise)) return;
        owned.add(promise);
        init(promise);
      },
      before(promise) { if (owned.has(promise)) before(promise); },
      after(promise) { if (owned.has(promise)) after(promise); },
    });
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, setImmediate,
    clearImmediate, atob, btoa, console: {log() {}, info() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10.0',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture',
    __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonInstallAsyncContextHooks: install,
    __xenonNetSend: (channel, value) => sent.push({channel, value}),
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe'}),
      installAsyncContextHooks: install,
      setDispatchHandler(callback) { dispatch = callback; },
      send: (channel, value) => sent.push({channel, value}),
    },
  });
  realmPromise = vm.runInContext('Promise', context);
  const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
  vm.runInContext(readFileSync(filename, 'utf8'), context, {filename});
  context.hooks = kind === 'main' ? context.__xenonAsyncHooks : context.require('async_hooks');
  context.net = kind === 'main' ? context.__xenonNet : context.require('net');
  t.after(() => stop?.());
  return {
    sent,
    evaluate: source => vm.runInContext(source, context),
    deliver(channel, value) {
      if (kind === 'main') context.__xenonDispatchSend({endpointId: '@main'}, channel, [value]);
      else dispatch(channel, [value]);
    },
  };
}

for (const kind of ['main', 'renderer']) {
  test(`${kind} net client events retain connect context across native IPC`, t => {
    const runtime = fixture(kind, t);
    runtime.evaluate(`
      globalThis.values = [];
      globalThis.storage = new hooks.AsyncLocalStorage();
      globalThis.socket = new net.Socket();
      for (const event of ['connect', 'data', 'end', 'close']) {
        socket.on(event, () => values.push([event, storage.getStore()]));
      }
      socket.on('manual', () => values.push(['manual', storage.getStore()]));
      storage.run('client', () => socket.connect('remote-fixture', function() {
        values.push(['callback', storage.getStore(), this === socket]);
      }));`);
    const id = runtime.sent[0].value.fromId;
    runtime.deliver('__xenon:net:connected', {toId: id, peerId: 'native-peer'});
    runtime.deliver('__xenon:net:data', {toId: id, wire: {t: 's', d: 'fixture'}});
    runtime.evaluate(`storage.run('caller', () => socket.emit('manual'));`);
    runtime.deliver('__xenon:net:close', {toId: id});
    assert.deepEqual(JSON.parse(runtime.evaluate('JSON.stringify(values)')), [
      ['connect', 'client'], ['callback', 'client', true], ['data', 'client'],
      ['manual', 'caller'], ['end', 'client'], ['close', 'client'],
    ]);
    assert.equal(runtime.evaluate('storage.getStore()'), undefined);
  });

  test(`${kind} native net server and accepted sockets retain listen context`, t => {
    const runtime = fixture(kind, t);
    runtime.evaluate(`
      globalThis.values = [];
      globalThis.storage = new hooks.AsyncLocalStorage();
      globalThis.server = net.createServer(socket => {
        values.push(['connection', storage.getStore()]);
        socket.on('data', () => values.push(['data', storage.getStore()]));
        socket.on('error', () => values.push(['error', storage.getStore()]));
      });
      storage.run('server', () => server.listen('\\\\\\\\.\\\\pipe\\\\fixture', function() {
        values.push(['listening', storage.getStore(), this === server]);
      }));`);
    const serverId = runtime.sent[0].value.serverId;
    assert.ok(serverId);
    runtime.deliver('__xenon:net:listening', {serverId});
    runtime.deliver('__xenon:net:connection', {serverId, socketId: 'accepted'});
    runtime.deliver('__xenon:net:data', {toId: 'accepted', wire: {t: 's', d: 'fixture'}});
    runtime.deliver('__xenon:net:error', {toId: 'accepted', code: 'EPIPE'});
    assert.deepEqual(JSON.parse(runtime.evaluate('JSON.stringify(values)')), [
      ['listening', 'server', true], ['connection', 'server'],
      ['data', 'server'], ['error', 'server'],
    ]);
    assert.equal(runtime.evaluate('storage.getStore()'), undefined);
  });

  test(`${kind} local paired sockets isolate client and server contexts`, async t => {
    const runtime = fixture(kind, t);
    await runtime.evaluate(`(async () => {
      globalThis.values = [];
      globalThis.storage = new hooks.AsyncLocalStorage();
      const server = storage.run('server', () => net.createServer(socket => {
        values.push(['connection', storage.getStore()]);
        socket.on('data', bytes => {
          values.push(['server-data', storage.getStore()]);
          socket.write(bytes);
        });
      }).listen('local-fixture'));
      await new Promise(resolve => {
        const socket = storage.run('client', () => net.connect('local-fixture', () => {
          values.push(['connect', storage.getStore()]);
          socket.write('fixture');
        }));
        socket.on('data', () => {
          values.push(['client-data', storage.getStore()]);
          socket.destroy(); server.close(); resolve();
        });
      });
    })()`);
    assert.deepEqual(JSON.parse(runtime.evaluate('JSON.stringify(values)')), [
      ['connection', 'server'], ['connect', 'client'],
      ['server-data', 'server'], ['client-data', 'client'],
    ]);
  });
}
