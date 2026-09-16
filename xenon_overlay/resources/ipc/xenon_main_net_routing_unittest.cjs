// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const source = readFileSync(path.join(__dirname, 'xenon_ipc_main_bootstrap.js'), 'utf8');
const factory = source.slice(source.indexOf('  const netModule = (() => {'),
    source.indexOf('  function createMainNetwork('));
const asyncScope = source.slice(source.indexOf('  let currentAsyncContext;'),
    source.indexOf('  class AsyncLocalStorage'));
const pipe = '\\\\.\\pipe\\xenon-main-routing-test';
const settle = () => new Promise(resolve => setImmediate(resolve));

function fixture(onSend = () => {}) {
  const sent = [];
  const context = vm.createContext({EventEmitter, Buffer, Uint8Array, queueMicrotask,
    __xenonPlatform: 'win32', console: {info() {}},
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
