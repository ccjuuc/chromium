// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const factory = readBootstrapPart('main/net.js');
const asyncScope = readBootstrapPart('main/async_context.js');
const pipe = '\\\\.\\pipe\\xenon-main-routing-test';
const settle = () => new Promise(resolve => setImmediate(resolve));

function fixture(onSend = () => {}, platform = 'win32') {
  const sent = [];
  const context = vm.createContext({EventEmitter, Buffer, Uint8Array, queueMicrotask,
    __xenonPlatform: platform, console: {info() {}},
    __xenonSendToRenderer() { assert.fail('net operations must never broadcast to renderers'); },
    __xenonNetSend(channel, payload) {
      sent.push({channel, payload});
      onSend(channel, payload, (event, value) => queueMicrotask(() =>
        context.deliver(event, value)));
    },
  });
  vm.runInContext('let dispatchXenonNet;\n' + asyncScope + factory +
      '\nglobalThis.net = netModule; globalThis.deliver = (channel, payload) => dispatchXenonNet(channel, payload, {endpointId:"@main"});', context);
  return {context, net: context.net, sent};
}

test('main Darwin Unix socket routes retain their exact filesystem path', () => {
  const {net, sent, context} = fixture(undefined, 'darwin');
  const endpoint = '/tmp/MixedCase.sock';
  let listening = 0, connected = 0;
  const server = net.createServer().listen(endpoint, () => ++listening);
  const bind = sent[0];
  assert.equal(bind.channel, '__xenon:net:listen');
  assert.equal(bind.payload.path, endpoint);
  assert.equal(bind.payload.port, undefined);
  assert.equal(server.address(), null);
  const socket = net.connect(endpoint, () => ++connected);
  const connect = sent.at(-1);
  assert.equal(connect.channel, '__xenon:net:connect');
  assert.equal(connect.payload.path, endpoint);
  assert.equal(connect.payload.port, undefined);
  context.deliver('__xenon:net:listening', {serverId: bind.payload.serverId, address: endpoint});
  assert.equal(listening, 1);
  assert.equal(server.address(), endpoint);
  context.deliver('__xenon:net:connected', {toId: connect.payload.fromId, peerId: 'native-unix'});
  assert.equal(connected, 1);
  socket.destroy();
  server.close();
});

test('main Darwin pauseOnConnect retains accepted bytes until resume', async () => {
  const {net, sent, context} = fixture(undefined, 'darwin');
  let incoming;
  const received = [];
  const server = net.createServer({pauseOnConnect: true}, socket => {
    incoming = socket;
    assert.equal(socket.isPaused(), true);
    socket.on('data', bytes => received.push(Array.from(bytes)));
  }).listen('/tmp/Paused.sock');
  const serverId = sent[0].payload.serverId;
  context.deliver('__xenon:net:listening', {serverId});
  context.deliver('__xenon:net:connection', {serverId, socketId: 'native-paused'});
  assert.equal(sent.at(-1).channel, '__xenon:net:pause');
  context.deliver('__xenon:net:data', {toId: 'native-paused', wire: {t: 'b64', d: 'AP+A'}});
  assert.deepEqual(received, []);
  incoming.resume();
  await settle();
  assert.deepEqual(received, [[0, 255, 128]]);
  assert.equal(sent.at(-1).channel, '__xenon:net:resume');
  incoming.destroy();
  server.close();
});

test('main connect uses one native route with unrelated renderer listeners present', async () => {
  const pages = [{path: 'other-a', calls: 0}, {path: pipe, calls: 0},
    {path: 'other-b', calls: 0}];
  const {net, sent} = fixture((channel, value, deliver) => {
    if (channel !== '__xenon:net:connect') return;
    const owner = pages.find(page => page.path === value.path);
    assert.ok(owner);
    ++owner.calls;
    deliver('__xenon:net:connected', {toId: value.fromId, peerId: 'native-1'});
  });
  const events = [];
  const client = net.connect(pipe, function() {
    assert.equal(this, client);
    events.push('callback');
  });
  client.on('connect', () => events.push('connect'));
  client.on('error', error => assert.fail(error.message));
  assert.equal(client.write('queued registration'), true);
  await settle();
  assert.deepEqual(events, ['connect', 'callback']);
  assert.deepEqual(pages.map(page => page.calls), [0, 1, 0]);
  assert.equal(sent[1].channel, '__xenon:net:data');
  assert.equal(sent[1].payload.toId, 'native-1');
  assert.equal(sent[1].payload.wire.d, 'queued registration');
  client.destroy();
});

test('main missing listener reports connection failure without a successful callback', async () => {
  const {net} = fixture((channel, value, deliver) => {
    if (channel === '__xenon:net:connect') {
      deliver('__xenon:net:error', {toId: value.fromId, code: 'ECONNREFUSED'});
    }
  });
  const events = [];
  const client = net.connect(pipe, () => assert.fail('must not connect'));
  client.on('error', error => events.push(error.code));
  client.on('close', () => events.push('close'));
  await settle();
  assert.deepEqual(events, ['ECONNREFUSED', 'close']);
  assert.equal(client.connecting, false);
  assert.equal(client.write('after error'), false);
});

test('main destroyed pending connection releases a late native peer', () => {
  const {net, sent, context} = fixture();
  const client = net.connect(pipe, () => assert.fail('must not connect'));
  const id = sent[0].payload.fromId;
  client.destroy();
  context.deliver('__xenon:net:connected', {toId: id, peerId: 'native-late'});
  assert.equal(sent.at(-1).channel, '__xenon:net:close');
  assert.equal(sent.at(-1).payload.toId, 'native-late');
});

test('main named-pipe listen waits for native readiness and close cancels queued events', () => {
  const {net, sent, context} = fixture();
  const events = [];
  const server = net.createServer(() => assert.fail('closed server accepted a connection'));
  server.on('listening', () => events.push('listening'));
  server.on('close', () => events.push('close'));
  server.listen(pipe, () => events.push('callback'));
  const id = sent[0].payload.serverId;
  assert.match(id, /^server-m-/);
  assert.deepEqual(events, []);
  server.close();
  context.deliver('__xenon:net:listening', {serverId: id});
  context.deliver('__xenon:net:connection', {serverId: id, socketId: 'native-late'});
  context.deliver('__xenon:net:server-closed', {serverId: id});
  assert.deepEqual(events, ['close']);
  assert.equal(sent.at(-1).channel, '__xenon:net:close');
  assert.equal(sent.at(-1).payload.toId, 'native-late');
});

test('main named-pipe accepted sockets preserve binary bytes and native errors close them', () => {
  const {net, context, sent} = fixture();
  let incoming;
  const server = net.createServer(socket => {
    incoming = socket;
    socket.on('data', bytes => socket.write(bytes));
  }).listen(pipe);
  const id = sent[0].payload.serverId;
  context.deliver('__xenon:net:listening', {serverId: id});
  context.deliver('__xenon:net:connection', {serverId: id, socketId: 'native-peer'});
  context.deliver('__xenon:net:data', {toId: 'native-peer', wire: {t: 'b64', d: 'AP+A'}});
  assert.equal(sent.at(-1).payload.wire.t, 'b64');
  assert.equal(sent.at(-1).payload.wire.d, 'AP+A');
  const events = [];
  incoming.on('error', error => events.push(error.code));
  incoming.on('close', () => events.push('close'));
  context.deliver('__xenon:net:error', {toId: 'native-peer', code: 'EPIPE'});
  assert.deepEqual(events, ['EPIPE', 'close']);
  assert.equal(incoming.write('after close'), false);
  server.close();
});
