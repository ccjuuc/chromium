// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {Readable} = require('node:stream');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function fixture(read = async () => ({data: Buffer.alloc(0), eof: true})) {
  const calls = [], controls = [], ports = [];
  class Port extends EventEmitter {
    start() { this.started = true; }
    close() { this.closed = true; }
    postMessage(data, transfer) { this.lastPost = {data, transfer}; }
  }
  class MessageChannelMain {
    constructor() {
      this.port1 = new Port(); this.port2 = new Port();
      ports.push(this.port1, this.port2);
    }
  }
  const host = {process: {cwd: () => 'C:\\app', env: {PATH: 'fixture'}},
    __xenonUtilityCreate(options, port) {
      port.detached = true; calls.push({options, port}); return calls.length;
    },
    __xenonUtilityControl(...args) { controls.push(args); return true; },
    __xenonUtilityRead: read};
  const context = vm.createContext({EventEmitter, Readable, Buffer, MessageChannelMain,
    path: path.win32, host, queueMicrotask});
  vm.runInContext(readBootstrapPart('main/utility_process.js') +
    '\nglobalThis.module = createUtilityProcessModule({EventEmitter, Readable, Buffer, MessageChannelMain, path, host});', context);
  return {module: context.module, host, calls, controls, ports,
    event: (...args) => host.__xenonDispatchUtilityProcessEvent(...args)};
}

test('utilityProcess snapshots launch options, process identity and transferable port messages', () => {
  const f = fixture();
  const env = JSON.parse('{"__proto__":"safe","COUNT":3}');
  const argv = ['first'];
  const child = f.module.fork('./child.cjs', argv,
    {env, cwd: 'C:\\work', execArgv: ['--expose-gc']});
  argv[0] = 'later'; env.COUNT = 9;
  assert.equal(child.pid, undefined);
  assert.equal(child.stdout, null);
  assert.equal(child.stderr, null);
  assert.equal(f.calls[0].options.modulePath, 'C:\\app\\child.cjs');
  assert.equal(f.calls[0].options.cwd, 'C:\\work');
  assert.equal(f.calls[0].options.env.__proto__, 'safe');
  assert.equal(f.calls[0].options.env.COUNT, '3');
  assert.deepEqual([...f.calls[0].options.args], ['first']);
  assert.equal(f.ports[1].detached, true);
  let spawned = 0, exited;
  child.on('spawn', () => { ++spawned; assert.equal(child.pid, 456); });
  child.on('exit', code => exited = code);
  f.event(1, 'spawn', 456, '');
  const message = {bytes: Buffer.from([0, 255])}, transfer = [f.ports[1]];
  child.postMessage(message, transfer);
  assert.equal(f.ports[0].lastPost.data, message);
  assert.equal(f.ports[0].lastPost.transfer, transfer);
  const received = [];
  child.on('message', data => received.push(data));
  f.ports[0].emit('message', {data: message});
  assert.equal(received[0], message);
  assert.equal(child.kill(), true);
  f.event(1, 'exit', 7, 'exited');
  assert.equal(spawned, 1); assert.equal(exited, 7); assert.equal(child.pid, undefined);
  assert.equal(child.kill(), false);
  assert.deepEqual(f.controls, [[1, 'kill'], [1, 'release']]);
});

test('utility output honors bounded demand and remains readable after process exit', async () => {
  const bytes = Buffer.alloc(1280 * 1024);
  for (let i = 0; i < bytes.length; ++i) bytes[i] = i * 37 & 255;
  let offset = 0, active = 0, maximumActive = 0, reads = 0;
  const f = fixture(async (id, fd, maximum) => {
    assert.equal(id, 1); assert.equal(fd, 1); assert.ok(maximum <= 65536);
    ++reads; maximumActive = Math.max(maximumActive, ++active);
    await new Promise(setImmediate);
    const data = bytes.subarray(offset, offset + maximum); offset += data.length;
    --active;
    return {data, eof: offset === bytes.length};
  });
  const child = f.module.fork('worker.js', [], {stdio: ['ignore', 'pipe', 'ignore']});
  assert.equal(reads, 0);
  f.event(1, 'spawn', 10, '');
  child.stdout.read(0);
  await new Promise(setImmediate); await new Promise(setImmediate);
  assert.ok(reads <= 2, 'paused output cannot drain the whole OS pipe');
  f.event(1, 'exit', 0, 'exited');
  assert.equal(f.controls.some(call => call[1] === 'release'), false);
  const chunks = [];
  for await (const chunk of child.stdout) chunks.push(chunk);
  await new Promise(setImmediate);
  assert.deepEqual(Buffer.concat(chunks), bytes);
  assert.equal(maximumActive, 1);
  assert.deepEqual(f.controls, [[1, 'close-output', 1], [1, 'release']]);
});

test('destroying an output closes the actual pipe and launch failure releases unconsumed pipes', async () => {
  const f = fixture();
  const child = f.module.fork('worker.js', {stdio: 'pipe'});
  const errors = [];
  child.on('error', failure => errors.push(failure.code));
  child.stdout.destroy();
  assert.deepEqual(f.controls, [[1, 'close-output', 1]]);
  f.event(1, 'error', 0, 'ENOENT: entry missing');
  f.event(1, 'exit', -1, 'launch-failed');
  await new Promise(setImmediate);
  assert.deepEqual(errors, ['ENOENT']);
  assert.equal(child.stderr.destroyed, true);
  assert.deepEqual(f.controls, [[1, 'close-output', 1], [1, 'close-output', 2], [1, 'release']]);
});

test('invalid utility options reject before allocating a port or native process', () => {
  const f = fixture();
  for (const args of [[null], [''], ['bad\0path'], ['a', [4]], ['a', [], {stdio: 'invalid'}],
    ['a', [], {stdio: ['pipe', 'pipe', 'pipe']}], ['a', [], {execArgv: [2]}],
    ['a', [], {env: null}], ['a', [], {cwd: 3}], ['a', [], {serviceName: 3}],
    ['a', [], {detached: true}], ['a', [], {allowLoadingUnsignedLibraries: 1}]])
    assert.throws(() => f.module.fork(...args));
  assert.equal(f.ports.length, 0); assert.equal(f.calls.length, 0);
});

test('exit drains untouched outputs while preserving a deliberately paused consumer', async () => {
  const positions = new Map(), length = 4;
  const f = fixture(async (id, fd) => {
    const count = (positions.get(fd) || 0) + 1; positions.set(fd, count);
    return {data: Buffer.from([fd, count]), eof: count === length};
  });
  const child = f.module.fork('worker.js', [], {stdio: 'pipe'});
  child.stderr.pause();
  f.event(1, 'spawn', 33, '');
  f.event(1, 'exit', 0, 'exited');
  await new Promise(setImmediate); await new Promise(setImmediate);
  assert.equal(positions.get(1), length);
  assert.equal(positions.has(2), false);
  assert.equal(f.controls.some(call => call[1] === 'release'), false);
  const parts = [];
  for await (const bytes of child.stderr) parts.push(bytes);
  assert.deepEqual(Buffer.concat(parts), Buffer.from([2, 1, 2, 2, 2, 3, 2, 4]));
  assert.equal(f.controls.filter(call => call[1] === 'release').length, 1);
});
