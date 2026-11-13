// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-fd-js-'));
  const owned = new Set();
  const calls = [];
  t.after(() => {
    for (const fd of owned) fs.closeSync(fd);
    fs.rmSync(directory, {recursive: true, force: true});
  });
  function perform(request, bytes) {
    calls.push({...request});
    const fd = request.fd;
    switch (request.operation) {
      case 'fd_open': {
        const fd = fs.openSync(request.path, request.flags, request.mode);
        owned.add(fd); return fd;
      }
      case 'fd_close': fs.closeSync(fd); owned.delete(fd); return;
      case 'fd_read': {
        const bytes = Buffer.alloc(request.length);
        return bytes.subarray(0, fs.readSync(fd, bytes, 0, bytes.length, request.position));
      }
      case 'fd_write': return fs.writeSync(fd, bytes, 0, bytes.length, request.position);
      case 'fd_fstat': {
        const stat = fs.fstatSync(fd, {bigint: request.bigint});
        return Object.fromEntries(Object.entries(stat).map(([name, value]) =>
          [name, typeof value === 'bigint' ? String(value) : value]));
      }
      case 'fd_fsync': return fs.fsyncSync(fd);
      case 'fd_fdatasync': return fs.fdatasyncSync(fd);
      case 'fd_ftruncate': return fs.ftruncateSync(fd, request.length);
      case 'fd_fchmod': return fs.fchmodSync(fd, request.mode);
      default: throw new Error('Unknown request');
    }
  }
  const api = {...fs, promises: {...fs.promises}};
  const context = vm.createContext({Buffer, EventEmitter, api, osPlatform:process.platform, bindings: {
    sync: perform,
    asynchronous: async (request, bytes) => {
      // Snapshot as the native V8-to-value bridge does before posting I/O.
      const snapshot = bytes && Buffer.from(bytes);
      await new Promise(resolve => setImmediate(resolve));
      return perform(request, snapshot);
    },
    path: value => value instanceof URL ? require('node:url').fileURLToPath(value) : value,
  }});
  vm.runInContext(readBootstrapPart('common/fs_stats.js') + '\n' +
      readBootstrapPart('main/fs_descriptors.js') +
      '\nbindings.stats = createFsStats; installNativeFileDescriptors(api, bindings);', context);
  return {api, file: path.join(directory, 'file'), calls};
}

test('descriptor writes and reads preserve position, byte offsets and real persisted data', t => {
  const {api, file} = fixture(t);
  const fd = api.openSync(file, 'w+');
  assert.equal(api.writeSync(fd, '中文abc', 0, 'utf8'), 9);
  assert.equal(api.fstatSync(fd).size, 9);
  const target = Buffer.alloc(8, 0xee);
  assert.equal(api.readSync(fd, target, {offset: 2, length: 3, position: 6}), 3);
  assert.deepEqual(target, Buffer.from([238, 238, 97, 98, 99, 238, 238, 238]));
  assert.equal(api.writeSync(fd, Buffer.from([0, 255]), 0, 2, null), 2);
  api.fsyncSync(fd);
  api.closeSync(fd);
  assert.deepEqual(fs.readFileSync(file), Buffer.concat([Buffer.from([0, 255]), Buffer.from('中文abc').subarray(2)]));
  assert.throws(() => api.readSync(fd, Buffer.alloc(1)), {code: 'EBADF'});
});

test('write callback returns original buffer and snapshots bytes before native work', async t => {
  const {api, file} = fixture(t);
  const fd = api.openSync(file, 'w+');
  const bytes = Buffer.from([1, 2, 3, 4]);
  let synchronous = true;
  const result = new Promise((resolve, reject) => api.write(fd, bytes, 1, 2, 0,
      (error, count, original) => {
        if (error) return reject(error);
        assert.equal(synchronous, false);
        assert.equal(original, bytes);
        resolve(count);
      }));
  bytes.fill(9); synchronous = false;
  assert.equal(await result, 2);
  const target = Buffer.alloc(4, 7);
  await new Promise((resolve, reject) => api.read(fd, target, 1, 2, 0,
      (error, count, original) => {
        if (error) return reject(error);
        assert.equal(count, 2); assert.equal(original, target); resolve();
      }));
  assert.deepEqual(target, Buffer.from([7, 2, 3, 7]));
  api.closeSync(fd);
});

test('FileHandle close waits for pending operations, is idempotent and invalidates fd', async t => {
  const {api, file} = fixture(t);
  const handle = await api.promises.open(file, 'w+');
  const written = handle.write('payload', 0, 'utf8');
  const closed = handle.close();
  assert.equal(handle.close(), closed);
  await assert.rejects(handle.stat(), {code: 'EBADF'});
  assert.equal((await written).bytesWritten, 7);
  await closed;
  assert.equal(handle.fd, -1);
  assert.equal(fs.readFileSync(file, 'utf8'), 'payload');
});

test('fstat sync callback and FileHandle stat return precise BigInt Stats', async t => {
  const {api, file, calls} = fixture(t);
  const handle = await api.promises.open(file, 'w+');
  await handle.write('payload');
  const expected = fs.fstatSync(handle.fd, {bigint:true});
  const fields = Object.keys(expected);
  const callback = await new Promise((resolve, reject) => api.fstat(handle.fd, {bigint:true},
    (error, stat) => error ? reject(error) : resolve(stat)));
  for (const stat of [api.fstatSync(handle.fd, {bigint:true}), callback,
    await handle.stat({bigint:true})]) {
    assert.equal(stat.constructor.name, 'BigIntStats');
    for (const field of fields) assert.equal(stat[field], expected[field], field);
    assert.equal(stat.isFile(), true); assert.equal(stat.isDirectory(), false);
    assert.equal(stat.mtime.getTime(), expected.mtime.getTime());
  }
  assert.equal(calls.at(-1).bigint, true);
  assert.equal(typeof (await handle.stat({bigint:1})).size, 'number');
  await handle.close();
});

test('readFile/writeFile with descriptor keep ownership and read from the current offset', async t => {
  const {api, file} = fixture(t);
  const fd = api.openSync(file, 'w+');
  api.writeFileSync(fd, 'hello');
  assert.equal(api.readFileSync(fd, 'utf8'), '');
  api.closeSync(fd);
  const handle = await api.promises.open(file);
  const prefix = await handle.read({length: 2});
  assert.equal(prefix.bytesRead, 2);
  assert.equal(await api.promises.readFile(handle, 'utf8'), 'llo');
  assert.equal((await handle.stat()).size, 5);
  await handle.close();
});

test('invalid descriptor buffers and ranges are rejected before native I/O', t => {
  const {api, file, calls} = fixture(t);
  const fd = api.openSync(file, 'w+');
  const before = calls.length;
  assert.throws(() => api.readSync(fd, {}, 0, 1), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => api.readSync(fd, Buffer.alloc(2), 2, 1), {code: 'ERR_OUT_OF_RANGE'});
  assert.throws(() => api.writeSync(fd, Buffer.alloc(2), 0, 2, -2), {code: 'ERR_OUT_OF_RANGE'});
  assert.throws(() => api.openSync(file, {}, 0o666), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(calls.length, before);
  api.closeSync(fd);
});
