// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const nodeNet = require('node:net');
const path = require('node:path');
const test = require('node:test');
const {promiseHooks} = require('node:v8');
const vm = require('node:vm');

const filename = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const source = readBootstrap(filename);
const pipePath = String.raw`\\.\pipe\net-contract-fixture`;
const settle = () => new Promise(resolve => setImmediate(resolve));

function fixture(platform = 'win32', sendError) {
  let dispatch, realmPromise, stopHooks, now = 0, nextTimer = 0;
  const timers = new Map();
  const sent = [];
  const context = vm.createContext({
    __xenonPaths: {platform, arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    setTimeout(callback, delay) {
      const id = ++nextTimer;
      timers.set(id, {callback, when: now + delay});
      return id;
    },
    clearTimeout(id) { timers.delete(id); },
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler(callback) { dispatch = callback; },
      installAsyncContextHooks(init, before, after) {
        const owned = new WeakSet();
        stopHooks = promiseHooks.createHook({
          init(promise) {
            if (!(promise instanceof realmPromise)) return;
            owned.add(promise);
            init(promise);
          },
          before(promise) { if (owned.has(promise)) before(promise); },
          after(promise) { if (owned.has(promise)) after(promise); },
        });
      },
      send(channel, value) {
        if (sendError) throw sendError;
        sent.push({channel, value});
      },
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, info() {}, warn() {}, error() {}},
  });
  realmPromise = vm.runInContext('Promise', context);
  vm.runInContext(source, context, {filename});
  return {
    net: context.require('net'), hooks: context.require('async_hooks'), sent,
    deliver(channel, value) { dispatch(`__xenon:net:${channel}`, [value]); },
    advance(delay) {
      now += delay;
      for (const [id, timer] of [...timers]) {
        if (timer.when <= now && timers.delete(id)) timer.callback();
      }
    },
    timerCount: () => timers.size,
    dispose: () => stopHooks?.(),
  };
}

test('renderer IP classifiers match Node for full, compressed, mapped, zoned and invalid addresses', () => {
  const {net} = fixture();
  const values = [undefined, null, 4, {}, false, '', 'localhost', 'example.com',
    '0.0.0.0', '255.255.255.255', '256.1.2.3', '127.01.0.1', '1.2.3',
    '1.2.3.4.5', ' 1.2.3.4', '1.2.3.4\n', '1.2.3.4/24', '::', '::1',
    '::ffff:192.0.2.1', '::ffff:192.00.2.1', '1:2:3:4:5:6:192.0.2.1',
    'fe80::1234%eth0', '::%12', '::%en_0', '::%', '::%a:b-c.d',
    '1:2:3:4:5:6:7:8', '1:2:3:4:5:6:7:8::', ':1:2:3:4:5:6:7',
    '1:2:3:4:5:6:7:', '1::2::3', ':::', '[::1]', 'gggg::', '12345::'];
  for (let i = 0; i < 300; ++i) {
    values.push(`${i}.2.3.4`, `1.2.3.${i}`, `1.02.${i}.4`, `${i.toString(16)}::1`);
  }
  for (let groups = 0; groups < 11; ++groups) {
    for (let cut = 0; cut <= groups; ++cut) {
      const parts = Array(groups).fill('abcd');
      values.push(parts.join(':'), parts.slice(0, cut).join(':') + '::' + parts.slice(cut).join(':'));
    }
  }
  for (const value of values) {
    for (const method of ['isIP', 'isIPv4', 'isIPv6']) {
      assert.equal(net[method](value), nodeNet[method](value), `${method}(${JSON.stringify(value)})`);
    }
  }
});

test('TCP and Unix endpoints report asynchronous unsupported errors without synthetic binds or connects', async () => {
  for (const platform of ['win32', 'linux']) {
    for (const endpoint of [0, '8000', {port: null}, {port: undefined},
      {port: 8000, host: 'localhost'}, {port: 80, path: pipePath}, '/tmp/net-fixture.sock',
      {path: '/tmp/net-fixture.sock'}, ...(platform === 'linux' ? [pipePath] : [])]) {
      const {net, sent} = fixture(platform);
      const events = [];
      const server = net.createServer(() => assert.fail('unsupported connection'));
      assert.equal(server.address(), null);
      assert.equal(server.listening, false);
      server.on('error', error => events.push(['server', error.code, error.syscall]));
      server.on('listening', () => assert.fail('unsupported bind emitted listening'));
      assert.equal(server.listen(endpoint, () => assert.fail('unsupported listen callback')), server);
      const connectEndpoint = endpoint && typeof endpoint === 'object' && 'port' in endpoint &&
          (endpoint.port == null || endpoint.path) ? 80 : endpoint;
      const socket = net.connect(connectEndpoint, () => assert.fail('unsupported connect callback'));
      socket.on('error', error => events.push(['socket', error.code, error.syscall]));
      socket.on('end', () => assert.fail('failed connect emitted end'));
      socket.on('close', hadError => events.push(['close', hadError]));
      assert.deepEqual(events, []);
      assert.equal(server.address(), null);
      assert.equal(socket.write('queued'), true);
      await settle();
      assert.deepEqual(events, [['server', 'ERR_NOT_SUPPORTED', 'listen'],
        ['socket', 'ERR_NOT_SUPPORTED', 'connect'], ['close', true]]);
      assert.equal(server.address(), null);
      assert.equal(server.listening, false);
      assert.equal(socket.connecting, false);
      assert.equal(socket.destroyed, true);
      assert.equal(socket.write('after failure'), false);
      assert.deepEqual(sent, []);
    }
  }
});

test('invalid endpoints throw before allocating native resources', () => {
  const {net, sent} = fixture();
  for (const endpoint of [-1, 65536, 1.5, {port: 'abc'}, {port: NaN}]) {
    assert.throws(() => net.connect(endpoint), {code: 'ERR_SOCKET_BAD_PORT'});
    assert.throws(() => net.createServer().listen(endpoint), {code: 'ERR_SOCKET_BAD_PORT'});
  }
  for (const endpoint of [{path: 1}, {host: false, port: 80}]) {
    assert.throws(() => net.connect(endpoint), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => net.createServer().listen(endpoint), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  assert.throws(() => net.connect(), {code: 'ERR_MISSING_ARGS'});
  assert.throws(() => net.connect(null), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => net.createServer().listen({}), {code: 'ERR_INVALID_ARG_VALUE'});
  assert.throws(() => net.connect({path: 'nul\0byte'}), {code: 'ERR_INVALID_ARG_VALUE'});
  assert.deepEqual(sent, []);
});

test('listen without an endpoint or with only a callback reports unsupported ephemeral TCP', async () => {
  const {net, sent} = fixture();
  let errors = 0;
  for (const args of [[], [null], [() => assert.fail('unsupported listen callback')]]) {
    const server = net.createServer();
    server.on('error', error => {
      assert.equal(error.code, 'ERR_NOT_SUPPORTED');
      assert.equal(error.port, 0);
      ++errors;
    });
    server.listen(...args);
  }
  assert.equal(errors, 0);
  await settle();
  assert.equal(errors, 3);
  assert.deepEqual(sent, []);
});

test('native binding owns listening/address state and failed duplicate binds preserve the first server', () => {
  const {net, sent, deliver} = fixture();
  let listens = 0, connections = 0;
  const server = net.createServer({}, () => ++connections).listen({path: pipePath}, () => ++listens);
  const serverId = sent[0].value.serverId;
  assert.equal(server.address(), null);
  assert.equal(server.listening, false);
  assert.throws(() => server.listen(pipePath), {code: 'ERR_SERVER_ALREADY_LISTEN'});
  deliver('listening', {serverId});
  deliver('listening', {serverId});
  assert.equal(listens, 1);
  assert.equal(server.address(), pipePath);
  assert.equal(server.listening, true);
  deliver('connection', {serverId, socketId: 'native-accepted'});
  assert.equal(connections, 1);

  const duplicate = net.createServer().listen(pipePath, () => assert.fail('failed bind callback'));
  let failure;
  duplicate.on('error', error => { failure = error; });
  const duplicateId = sent.at(-1).value.serverId;
  deliver('error', {serverId: duplicateId, code: 'EADDRINUSE'});
  assert.equal(failure.code, 'EADDRINUSE');
  assert.equal(duplicate.address(), null);
  assert.equal(duplicate.listening, false);
  assert.equal(server.address(), pipePath);
  deliver('listening', {serverId: duplicateId});
  assert.equal(duplicate.address(), null);

  let closed = 0;
  server.close(() => ++closed);
  assert.equal(server.address(), null);
  assert.equal(server.listening, false);
  deliver('listening', {serverId});
  assert.equal(listens, 1);
  deliver('server-closed', {serverId});
  deliver('server-closed', {serverId});
  assert.equal(closed, 1);
});

test('closing during a pending native bind suppresses late listening and connection events', () => {
  const {net, sent, deliver} = fixture();
  const server = net.createServer(() => assert.fail('closed server accepted'))
    .listen(pipePath, () => assert.fail('closed server listening'));
  const serverId = sent[0].value.serverId;
  server.close();
  deliver('listening', {serverId});
  deliver('connection', {serverId, socketId: 'late-accepted'});
  assert.equal(server.address(), null);
  assert.equal(server.listening, false);
  assert.equal(sent.at(-1).channel, '__xenon:net:close');
  assert.equal(sent.at(-1).value.toId, 'late-accepted');
});

test('a pipe connect before native bind acknowledgement cannot use the local pairing shortcut', () => {
  const {net, sent, deliver} = fixture();
  const server = net.createServer(() => assert.fail('unbound local server accepted')).listen(pipePath);
  const serverId = sent[0].value.serverId;
  let connected = false;
  const socket = net.connect(pipePath, () => { connected = true; });
  const request = sent.at(-1);
  assert.equal(request.channel, '__xenon:net:connect');
  assert.equal(request.value.path, pipePath);
  assert.equal(connected, false);
  deliver('listening', {serverId});
  deliver('connected', {toId: request.value.fromId, peerId: 'native-peer'});
  assert.equal(connected, true);
  socket.destroy();
  server.close();
});

test('native transport submission failures become asynchronous errors and release pending resources', async () => {
  const {net} = fixture('win32', new Error('transport unavailable'));
  let serverError, socketError;
  const server = net.createServer().listen(pipePath, () => assert.fail('failed bind callback'));
  server.on('error', error => { serverError = error; });
  const socket = net.connect(pipePath, () => assert.fail('failed connect callback'));
  socket.on('error', error => { socketError = error; });
  assert.equal(serverError, undefined);
  assert.equal(socketError, undefined);
  await settle();
  assert.equal(serverError.code, 'ERR_NOT_SUPPORTED');
  assert.equal(socketError.code, 'ERR_NOT_SUPPORTED');
  assert.equal(server.address(), null);
  assert.equal(server.listening, false);
  assert.equal(socket.destroyed, true);
  assert.equal(socket.connecting, false);
});

test('socket inactivity timeout resets on connect/read/write and can be disabled without closing the pipe', () => {
  const {net, sent, deliver, advance, timerCount} = fixture();
  const socket = net.connect(pipePath);
  const socketId = sent[0].value.fromId;
  let timeouts = 0, callbacks = 0;
  socket.on('timeout', () => ++timeouts);
  assert.equal(socket.setTimeout(100, () => ++callbacks), socket);
  assert.equal(socket.setNoDelay(true), socket);
  assert.equal(socket.setKeepAlive(true, 1000), socket);
  advance(90);
  deliver('connected', {toId: socketId, peerId: 'native-peer'});
  advance(90);
  deliver('data', {toId: socketId, wire: {t: 's', d: 'read'}});
  advance(90);
  socket.write('write');
  advance(99);
  assert.equal(timeouts, 0);
  advance(1);
  assert.equal(timeouts, 1);
  assert.equal(callbacks, 1);
  assert.equal(socket.destroyed, false);
  advance(200);
  assert.equal(timeouts, 1);
  socket.write('rearm');
  advance(100);
  assert.equal(timeouts, 2);
  assert.equal(callbacks, 1);
  socket.setTimeout(0);
  socket.write('disabled');
  advance(200);
  assert.equal(timeouts, 2);
  assert.equal(timerCount(), 0);
  socket.setTimeout(100);
  socket.destroy();
  advance(200);
  assert.equal(timeouts, 2);
  assert.equal(timerCount(), 0);
});

test('timeout arguments match Node error codes and zero removes a registered callback', () => {
  const {net, advance} = fixture();
  const socket = new net.Socket();
  for (const value of [undefined, '100', null, true]) {
    assert.throws(() => socket.setTimeout(value), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  for (const value of [-1, Infinity, NaN]) {
    assert.throws(() => socket.setTimeout(value), {code: 'ERR_OUT_OF_RANGE'});
  }
  assert.throws(() => socket.setTimeout(1, null), {code: 'ERR_INVALID_ARG_TYPE'});
  let calls = 0;
  const callback = () => ++calls;
  socket.setTimeout(10, callback).setTimeout(0, callback).setTimeout(10);
  advance(10);
  assert.equal(calls, 0);
  socket.destroy();
});

test('timeout listeners run in the socket connection async context', t => {
  const {net, hooks, advance, dispose} = fixture();
  t.after(dispose);
  const storage = new hooks.AsyncLocalStorage();
  const socket = storage.run('client-context', () => net.connect(pipePath));
  let callbackContext;
  socket.setTimeout(10, () => { callbackContext = storage.getStore(); });
  storage.run('timer-caller', () => advance(10));
  assert.equal(callbackContext, 'client-context');
  assert.equal(storage.getStore(), undefined);
  socket.destroy();
});
