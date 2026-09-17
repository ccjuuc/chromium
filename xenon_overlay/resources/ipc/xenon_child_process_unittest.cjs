// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function load(side, result = {ok: true, pid: 4321}) {
  const calls = [];
  const ipc = new EventEmitter();
  const call = request => {
    calls.push(request);
    return request.op === 'spawn' ? result : {ok: true};
  };
  const context = vm.createContext({Buffer, Uint8Array, queueMicrotask,
    streamModule: {Readable: class extends EventEmitter {}, Writable: class extends EventEmitter {}},
    currentAsyncContext: 'child-context',
    runWithAsyncContext: (_context, callback, receiver, args) => callback.apply(receiver, args),
    process: {env: {FIXTURE_INHERITED: 'yes'}, platform: 'win32'},
    ipcMain: ipc, ipcRenderer: ipc,
    __xenonChildProcessCall: call,
    transport: {sendSync(channel, request) {
      assert.equal(channel, '__xenon:child-process:call');
      return call(request);
    }}});
  vm.runInContext(readBootstrapPart(`${side}/events.js`) + '\n' +
      readBootstrapPart('common/child_process.js') + '\n' +
      readBootstrapPart(`${side}/child_process.js`) + '\n' +
      'globalThis.cp = childProcessModule;', context);
  const notify = event => ipc.emit('__xenon:child-process:event', {}, {id: '1', ...event});
  return {cp: context.cp, calls, notify};
}

for (const side of ['main', 'renderer']) {
  test(`${side}: spawn returns native identity and preserves arguments without a shell`, async () => {
    const {cp, calls, notify} = load(side);
    const child = cp.spawn('fixture.exe', ['a b', '& literal'], {
      cwd: 'C:\\fixture', env: {FIXTURE: 'value', OMIT: undefined}, windowsHide: true,
    });
    assert.equal(child.pid, 4321);
    assert(child instanceof cp.ChildProcess);
    assert.deepEqual(Array.from(child.spawnargs), ['fixture.exe', 'a b', '& literal']);
    assert.deepEqual(JSON.parse(JSON.stringify(calls[0])), {
      id: '1', op: 'spawn', file: 'fixture.exe', args: ['a b', '& literal'],
      cwd: 'C:\\fixture', env: {FIXTURE: 'value'}, windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    const order = [];
    child.on('spawn', () => order.push('spawn'));
    child.stdout.on('data', bytes => order.push(bytes.toString()));
    child.on('exit', (code, signal) => { assert.equal(code, 7); assert.equal(signal, null); order.push('exit'); });
    child.on('close', () => order.push('close'));
    assert.deepEqual(order, []);
    await Promise.resolve();
    notify({event: 'data', fd: 1, data: Buffer.from('fixture-output').toString('base64')});
    notify({event: 'exit', exitCode: 7, signal: 0});
    notify({event: 'end', fd: 1});
    notify({event: 'end', fd: 2});
    notify({event: 'close'});
    await Promise.resolve();
    assert.deepEqual(order, ['spawn', 'fixture-output', 'exit', 'close']);
    assert.equal(child.kill(), false);
  });

  test(`${side}: failed spawn emits error then close without pid or spawn event`, async () => {
    const {cp} = load(side, {ok: false, code: 'ENOENT', errno: -2, message: 'missing'});
    const child = cp.spawn('missing.exe');
    assert.equal(child.pid, undefined);
    const order = [];
    child.on('spawn', () => order.push('spawn'));
    child.on('error', error => {
      assert.equal(error.code, 'ENOENT');
      assert.equal(error.path, 'missing.exe');
      order.push('error');
    });
    const closed = new Promise(resolve => child.on('close', (code, signal) => {
      order.push('close'); assert.equal(code, -2); assert.equal(signal, null); resolve();
    }));
    assert.deepEqual(order, []);
    await closed;
    assert.deepEqual(order, ['error', 'close']);
  });

  test(`${side}: stdin callbacks follow native write and graceful shutdown completion`, async () => {
    const {cp, calls, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const order = [];
    child.stdin.write(Buffer.from([0, 1, 255]), error => { assert.equal(error, null); order.push('write'); });
    child.stdin.end('last', () => order.push('finish'));
    assert.deepEqual(order, []);
    assert.equal(calls.find(call => call.op === 'write').data, 'AAH/');
    notify({event: 'write', token: 1});
    notify({event: 'write', token: 2});
    notify({event: 'finish'});
    assert.deepEqual(order, ['write', 'finish']);
    assert.equal(child.stdin.writableFinished, true);
    assert.equal(calls.filter(call => call.op === 'end').length, 1);
    await Promise.resolve();
  });

  test(`${side}: stdout keeps split UTF-8 intact and pauses native reads`, async () => {
    const {cp, calls, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const output = [];
    child.stdout.setEncoding('utf8').on('data', value => output.push(value));
    const bytes = Buffer.from('中文');
    notify({event: 'data', fd: 1, data: bytes.subarray(0, 2).toString('base64')});
    assert.deepEqual(output, []);
    child.stdout.pause();
    assert.equal(calls.at(-1).op, 'pause');
    notify({event: 'data', fd: 1, data: bytes.subarray(2).toString('base64')});
    assert.deepEqual(output, []);
    child.stdout.resume();
    await Promise.resolve();
    notify({event: 'end', fd: 1});
    assert.deepEqual(output, ['中文']);
  });

  test(`${side}: encoded read sizes and readableLength count UTF-16 code units`, async () => {
    const {cp, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    child.stdout.setEncoding('utf8');
    const bytes = Buffer.from('中文😀');
    notify({event: 'data', fd: 1, data: bytes.subarray(0, 2).toString('base64')});
    assert.equal(child.stdout.readableLength, 0);
    assert.equal(child.stdout.read(1), null);
    notify({event: 'data', fd: 1, data: bytes.subarray(2, 7).toString('base64')});
    assert.equal(child.stdout.readableLength, 2);
    assert.equal(child.stdout.read(1), '中');
    assert.equal(child.stdout.readableLength, 1);
    assert.equal(child.stdout.read(1), '文');
    assert.equal(child.stdout.read(1), null);
    notify({event: 'data', fd: 1, data: bytes.subarray(7).toString('base64')});
    assert.equal(child.stdout.readableLength, 2);
    assert.equal(child.stdout.read(2), '😀');
    assert.equal(child.stdout.readableLength, 0);
    notify({event: 'end', fd: 1});
    assert.equal(child.stdout.readableEnded, true);
    notify({event: 'exit', exitCode: 0, signal: 0});
    notify({event: 'end', fd: 2});
    notify({event: 'close'});
    await Promise.resolve();
  });

  test(`${side}: setEncoding converts already buffered data and keeps split UTF-8`, async () => {
    const {cp, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const bytes = Buffer.from('中文');
    notify({event: 'data', fd: 1, data: bytes.subarray(0, 4).toString('base64')});
    assert.equal(child.stdout.readableLength, 4);
    child.stdout.setEncoding('UTF-8');
    assert.equal(child.stdout.readableLength, 1);
    assert.equal(child.stdout.read(1), '中');
    child.stdout.setEncoding('utf8');
    notify({event: 'data', fd: 1, data: bytes.subarray(4).toString('base64')});
    assert.equal(child.stdout.readableLength, 1);
    assert.equal(child.stdout.read(1), '文');
    notify({event: 'end', fd: 1});
    notify({event: 'exit', exitCode: 0, signal: 0});
    notify({event: 'end', fd: 2});
    notify({event: 'close'});
    await Promise.resolve();
  });

  test(`${side}: encoded readable mode receives a short tail and an incomplete final character`, async () => {
    const {cp, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const output = [];
    let closed = false;
    child.stdout.setEncoding('utf8').on('readable', () => {
      let chunk;
      while ((chunk = child.stdout.read(4)) !== null) output.push(chunk);
    });
    child.on('close', () => { closed = true; });
    notify({event: 'data', fd: 1,
      data: Buffer.concat([Buffer.from('中文'), Buffer.from([0xe4, 0xb8])]).toString('base64')});
    assert.deepEqual(output, []);
    assert.equal(child.stdout.readableLength, 2);
    notify({event: 'exit', exitCode: 0, signal: 0});
    await Promise.resolve();
    assert.equal(child.stdout.readableLength, 2);
    notify({event: 'end', fd: 1});
    assert.deepEqual(output, ['中文\ufffd']);
    notify({event: 'end', fd: 2});
    notify({event: 'close'});
    await Promise.resolve();
    assert.equal(closed, true);
  });

  test(`${side}: explicit env and stdio modes are respected and unsupported options fail`, () => {
    const {cp, calls} = load(side);
    const child = cp.spawn('fixture.exe', [], {stdio: ['ignore', 'inherit', 'pipe']});
    assert.equal(child.stdin, null);
    assert.equal(child.stdout, null);
    assert(child.stderr);
    assert.deepEqual(JSON.parse(JSON.stringify(calls[0].env)), {FIXTURE_INHERITED: 'yes'});
    for (const options of [{shell: true}, {detached: true}, {stdio: ['ipc']}, {timeout: 100}])
      assert.throws(() => cp.spawn('fixture.exe', [], options), {code: 'ERR_NOT_SUPPORTED'});
    assert.throws(() => cp.spawn('fixture\0.exe'), {code: 'ERR_INVALID_ARG_VALUE'});
    assert.throws(() => cp.spawn('fixture.exe', [], {cwd: 42}), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => cp.spawn('fixture.exe', [], {env: {BAD: 'nul\0'}}), {code: 'ERR_INVALID_ARG_VALUE'});
    assert.equal(calls.filter(call => call.op === 'spawn').length, 1);
  });

  test(`${side}: destroying stdin waits for cancelled native writes before close`, async () => {
    const {cp, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const order = [];
    child.stdin.write('pending', error => {
      assert.equal(error.code, 'ECANCELED');
      order.push('write.callback');
    });
    child.stdin.on('close', () => order.push('stdin.close'));
    child.stdin.destroy();
    await Promise.resolve();
    assert.deepEqual(order, []);
    notify({event: 'write', token: 1, code: 'ECANCELED'});
    assert.deepEqual(order, ['write.callback', 'stdin.close']);
  });

  test(`${side}: child close waits for buffered output consumed after exit`, async () => {
    const {cp, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const order = [];
    child.stdout.on('data', bytes => { order.push('data:' + bytes); child.stdout.pause(); });
    child.stdout.on('end', () => order.push('stdout.end'));
    child.stdout.on('close', () => order.push('stdout.close'));
    child.on('close', () => order.push('child.close'));
    for (const text of ['a', 'b', 'c'])
      notify({event: 'data', fd: 1, data: Buffer.from(text).toString('base64')});
    notify({event: 'exit', exitCode: 0, signal: 0});
    notify({event: 'end', fd: 1});
    notify({event: 'end', fd: 2});
    notify({event: 'close'});
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(order, ['data:a', 'data:b']);
    assert.equal(child.stdout.readableLength, 1);
    child.stdout.resume();
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(order, ['data:a', 'data:b', 'data:c', 'stdout.end', 'stdout.close', 'child.close']);
  });

  test(`${side}: readable mode receives a short final chunk before or after process exit`, async () => {
    for (const exitBeforeEof of [false, true]) {
      const {cp, notify} = load(side);
      const child = cp.spawn('fixture.exe');
      const output = [];
      let readableEvents = 0;
      let closed = 0;
      child.stdout.on('readable', () => {
        ++readableEvents;
        let chunk;
        while ((chunk = child.stdout.read(4)) !== null) output.push(chunk.toString());
      });
      child.on('close', (code, signal) => {
        assert.equal(code, 0);
        assert.equal(signal, null);
        ++closed;
      });
      await Promise.resolve();
      notify({event: 'data', fd: 1, data: Buffer.from('abc').toString('base64')});
      assert.deepEqual(output, []);
      assert.equal(child.stdout.readableLength, 3);
      if (exitBeforeEof) {
        notify({event: 'exit', exitCode: 0, signal: 0});
        await Promise.resolve();
        assert.deepEqual(output, []);
        assert.equal(child.stdout.readableLength, 3);
      }
      notify({event: 'end', fd: 1});
      assert.deepEqual(output, ['abc']);
      assert.equal(readableEvents, 2);
      if (!exitBeforeEof) notify({event: 'exit', exitCode: 0, signal: 0});
      notify({event: 'end', fd: 2});
      notify({event: 'close'});
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(child.stdout.readableEnded, true);
      assert.equal(child.stdout.closed, true);
      assert.equal(closed, 1);
    }
  });

  test(`${side}: data mode drains pipe output arriving after process exit`, async () => {
    const {cp, notify} = load(side);
    const child = cp.spawn('fixture.exe');
    const order = [];
    child.stdout.on('data', chunk => order.push('data:' + chunk));
    child.stdout.on('end', () => order.push('stdout.end'));
    child.on('exit', () => order.push('exit'));
    child.on('close', () => order.push('close'));
    await Promise.resolve();
    notify({event: 'exit', exitCode: 0, signal: 0});
    notify({event: 'data', fd: 1, data: Buffer.from('tail').toString('base64')});
    notify({event: 'end', fd: 1});
    notify({event: 'end', fd: 2});
    notify({event: 'close'});
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(order, ['exit', 'data:tail', 'stdout.end', 'close']);
  });

  test(`${side}: empty argv0 remains explicit and signal kill uses native result`, () => {
    const {cp, calls} = load(side);
    const child = cp.spawn('fixture.exe', [], {argv0: '', stdio: 'ignore'});
    assert.equal(child.spawnargs[0], '');
    assert.equal(calls[0].argv0, '');
    assert.equal(child.kill('SIGTERM'), true);
    assert.equal(child.killed, true);
    assert.equal(calls.at(-1).signal, 'SIGTERM');
    assert.throws(() => child.kill('not-a-signal'), {code: 'ERR_UNKNOWN_SIGNAL'});
  });

  test(`${side}: exit uses native platform signal names and null env inherits hosted values`, () => {
    const {cp, calls, notify} = load(side);
    const child = cp.spawn('fixture.exe', [], {env: null, stdio: 'ignore'});
    assert.equal(calls[0].env.FIXTURE_INHERITED, 'yes');
    const statuses = [];
    child.on('exit', (code, signal) => statuses.push([code, signal]));
    child.on('close', (code, signal) => statuses.push([code, signal]));
    notify({event: 'exit', exitCode: 0, signal: 11, signalName: 'SIGSEGV'});
    notify({event: 'close'});
    assert.deepEqual(statuses, [[null, 'SIGSEGV'], [null, 'SIGSEGV']]);
  });
}
