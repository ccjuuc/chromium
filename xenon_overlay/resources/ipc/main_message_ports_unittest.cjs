'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter, once} = require('node:events');
const {MessageChannel} = require('node:worker_threads');
const fs = require('node:fs');
const path = require('node:path');
const create = new Function(fs.readFileSync(path.join(__dirname, 'main/message_ports.js'), 'utf8') +
    '\nreturn createMessagePortMainModule;')();

// Exercise the facade against real Node message pipes; the C++ tests cover
// the production Mojo pipe and Blink wire implementation independently.
function setup(t) {
  const native = new Map();
  const brands = new WeakMap();
  const all = new Set();
  const closeCalls = [];
  let next = 1;
  const host = {};
  function adopt(port) {
    const id = next++;
    native.set(id, port);
    all.add(port);
    port.on('close', () => {
      all.delete(port);
      if (native.delete(id)) host.__xenonDispatchMessagePort(id, 'close');
    });
    port.unref();
    return id;
  }
  Object.assign(host, {
    __xenonMessagePortCreate() {
      const {port1, port2} = new MessageChannel();
      return [adopt(port1), adopt(port2)];
    },
    __xenonMessagePortBind(id, wrapper) { brands.set(wrapper, id); },
    __xenonMessagePortStart(id) {
      const port = native.get(id);
      if (!port || port.listenerCount('message')) return;
      port.on('message', ({data, ports}) => host.__xenonDispatchMessagePort(id, 'message', data,
          host.__xenonAdoptMessagePorts(ports.map(adopt))));
      port.unref();
    },
    __xenonMessagePortPost(id, data, transfer) {
      if (!native.has(id)) return false;
      const ids = transfer.map(port => brands.get(port));
      if (new Set(ids).size !== ids.length || ids.includes(id) || ids.some(id => !native.has(id)))
        throw new DOMException('Invalid transfer', 'DataCloneError');
      const ports = ids.map(id => native.get(id));
      native.get(id).postMessage({data, ports}, ports);
      ids.forEach(id => native.delete(id));
      return true;
    },
    __xenonMessagePortClose(id) { closeCalls.push(id); native.get(id)?.close(); },
    __xenonPostPortMessage() { throw new Error('Target renderer document is unavailable'); },
  });
  t.after(() => { for (const port of all) port.close(); });
  return {...create({EventEmitter, host, dispatchIpc() {}}), host, closeCalls};
}
function message(port) {
  return once(port, 'message', {signal: AbortSignal.timeout(2000)}).then(([event]) => event);
}

test('MessagePortMain waits for explicit start and exposes Electron message events', async t => {
  const {MessageChannelMain, MessagePortMain} = setup(t);
  assert.throws(() => new MessagePortMain(), /Illegal constructor/);
  const {port1, port2} = new MessageChannelMain();
  let messages = 0;
  port2.on('message', () => ++messages);
  const value = {map: new Map([['x', 42n]]), bytes: new Uint8Array([1, 2])};
  value.self = value;
  port1.postMessage(value);
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(messages, 0);
  const received = message(port2);
  port2.start();
  const event = await received;
  assert.equal(event.data.self, event.data);
  assert.equal(event.data.map.get('x'), 42n);
  assert.deepEqual(event.ports, []);
});

test('Transferred port is adopted once and its old wrapper cannot send', async t => {
  const {MessageChannelMain} = setup(t);
  const {port1, port2} = new MessageChannelMain();
  const channel = new MessageChannelMain();
  port2.start();
  const received = message(port2);
  port1.postMessage('port', [channel.port1]);
  const moved = (await received).ports[0];
  assert.notEqual(moved, channel.port1);
  channel.port2.start();
  const reply = message(channel.port2);
  channel.port1.postMessage('stale');
  moved.postMessage('moved');
  assert.equal((await reply).data, 'moved');
});

test('Clone failure preserves transfer ownership and close is asynchronous and once', async t => {
  const {MessageChannelMain} = setup(t);
  const {port1, port2} = new MessageChannelMain();
  const channel = new MessageChannelMain();
  assert.throws(() => port1.postMessage(() => {}, [channel.port1]), {name: 'DataCloneError'});
  channel.port2.start();
  const received = message(channel.port2);
  channel.port1.postMessage('owned');
  assert.equal((await received).data, 'owned');
  let closed = 0;
  port1.on('close', () => ++closed);
  const first = once(port1, 'close');
  const second = once(port2, 'close');
  port1.close(); port1.close();
  assert.equal(closed, 0);
  await Promise.all([first, second]);
  assert.equal(closed, 1);
});

test('Host detachment releases a started wrapper without closing its transferred endpoint', async t => {
  const {MessageChannelMain, host, closeCalls} = setup(t);
  const {port1, port2} = new MessageChannelMain();
  port1.start();
  let messages = 0, closes = 0;
  port1.on('message', () => ++messages);
  port1.on('close', () => ++closes);
  host.__xenonDetachMessagePort(port1);
  port1.close();
  port1.start();
  port1.postMessage('detached');
  port2.postMessage('queued for the new owner');
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(closeCalls.length, 0);
  assert.equal(messages, 0);
  assert.equal(closes, 0);
});
