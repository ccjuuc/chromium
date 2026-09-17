// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const filename = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const source = readBootstrap(filename);

function fixture() {
  let dispatch;
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler(callback) { dispatch = callback; },
      send(channel, value) {
        // Only a native bind acknowledgement permits the local pipe shortcut.
        if (channel === '__xenon:net:listen') {
          dispatch('__xenon:net:listening', [{serverId: value.serverId}]);
        } else if (channel === '__xenon:net:unlisten') {
          dispatch('__xenon:net:server-closed', [{serverId: value.serverId}]);
        } else {
          assert.fail('A locally paired pipe must not send socket IPC');
        }
      },
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, info() {}, warn() {}, error() {}},
  });
  vm.runInContext(source, context, {filename});
  return {context, net: context.require('net')};
}

const settle = () => new Promise(resolve => setImmediate(resolve));
const pipePath = String.raw`\\.\pipe\local-fixture`;

test('local net destruction before queued connection delivery cancels all connect callbacks', async () => {
  const {net} = fixture();
  const events = [];
  const server = net.createServer(() => events.push('server-connection')).listen(pipePath);
  const socket = net.connect(pipePath, () => events.push('connect-callback'));
  socket.on('connect', () => events.push('client-connect'));
  socket.on('close', () => events.push('client-close'));
  socket.destroy();
  socket.destroy();
  await settle();
  assert.deepEqual(events, ['client-close']);
  assert.equal(socket.connecting, false);
  assert.equal(socket.write('after close'), false);
  server.close();
});

test('local net server connection callback can destroy before client connect delivery', async () => {
  const {net} = fixture();
  const events = [];
  const server = net.createServer(incoming => {
    events.push('server-connection');
    incoming.destroy();
  }).listen(pipePath);
  const socket = net.connect(pipePath, () => events.push('connect-callback'));
  socket.on('connect', () => events.push('client-connect'));
  socket.on('close', () => events.push('client-close'));
  await settle();
  assert.deepEqual(events, ['server-connection', 'client-close']);
  assert.equal(socket.write('after close'), false);
  server.close();
});

test('local net connect listener destruction cancels the deferred connect callback', async () => {
  const {net} = fixture();
  const events = [];
  const server = net.createServer(() => events.push('server-connection')).listen(pipePath);
  const socket = net.connect(pipePath, () => events.push('connect-callback'));
  socket.on('connect', () => { events.push('client-connect'); socket.destroy(); });
  socket.on('close', () => events.push('client-close'));
  await settle();
  assert.deepEqual(events, ['server-connection', 'client-connect', 'client-close']);
  server.close();
});

test('local net connect callback can destroy both endpoints exactly once', async () => {
  const {net} = fixture();
  let incomingCloses = 0;
  let clientCloses = 0;
  let callbacks = 0;
  const server = net.createServer(incoming => {
    incoming.on('close', () => ++incomingCloses);
  }).listen(pipePath);
  const socket = net.connect(pipePath, function() {
    assert.equal(this, socket);
    ++callbacks;
    this.destroy();
  });
  socket.on('close', () => ++clientCloses);
  await settle();
  socket.destroy();
  assert.equal(callbacks, 1);
  assert.equal(clientCloses, 1);
  assert.equal(incomingCloses, 1);
  server.close();
});

test('local net successful connection preserves asynchronous ordering and bidirectional bytes', async () => {
  const {net, context} = fixture();
  const events = [];
  let reply;
  const server = net.createServer(incoming => {
    events.push('server-connection');
    incoming.on('data', bytes => incoming.write(bytes));
  }).listen(pipePath);
  const socket = net.connect(pipePath, function() {
    assert.equal(this, socket);
    events.push('connect-callback');
    assert.equal(socket.write(context.Buffer.from([0, 255, 128, 65])), true);
  });
  socket.on('connect', () => events.push('client-connect'));
  socket.on('data', bytes => { reply = Array.from(bytes); });
  assert.deepEqual(events, []);
  await settle();
  assert.deepEqual(events, ['server-connection', 'client-connect', 'connect-callback']);
  assert.deepEqual(reply, [0, 255, 128, 65]);
  socket.destroy();
  server.close();
});
