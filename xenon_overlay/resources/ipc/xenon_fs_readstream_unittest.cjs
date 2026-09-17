// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function runtime(kind, contents = '第一行\r\nsecond\n最后一行', options = {}) {
  const file = 'C:\\fixture\\config.txt';
  const bytes = Buffer.from(contents);
  const reads = [];
  const read = requested => {
    reads.push(requested);
    if (requested !== file) throw Object.assign(new Error('ENOENT: missing file'), {code: 'ENOENT'});
    return bytes;
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {},
      invoke: async (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        assert.equal(request.operation, 'read_file');
        assert.equal(request.returnBytes, true);
        const result = Uint8Array.from(read(request.path)).buffer;
        if (options.readGate) await options.readGate;
        return result;
      },
    },
    __xenonFsCallAsync: async request => {
      assert.equal(request.operation, 'read_file');
      const result = read(request.path);
      if (options.readGate) await options.readGate;
      return result;
    },
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10.0',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture',
    __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  if (options.nativeDecoder === false) delete context.TextDecoder;
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${kind}_bootstrap.js`)), context);
  return {file, bytes, reads, fs: kind === 'main' ? context.__xenonFs : context.require('fs'),
    readline: kind === 'main' ? context.__xenonReadline : context.require('readline')};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: read stream delivers exact byte range and one end/close`, async () => {
    const {fs, file, bytes} = runtime(kind);
    const stream = fs.createReadStream(file, {start: 1, end: 12, highWaterMark: 2});
    const chunks = [];
    let ends = 0, opens = 0;
    stream.on('open', () => ++opens);
    stream.on('end', () => ++ends);
    await new Promise((resolve, reject) => stream.on('data', data => chunks.push(Buffer.from(data)))
        .on('error', reject).on('close', resolve));
    assert.deepEqual(Buffer.concat(chunks), bytes.subarray(1, 13));
    assert.equal(stream.bytesRead, 12);
    assert.equal(ends, 1);
    assert.equal(opens, 0); // The bridge does not invent a file descriptor.
    assert.equal(stream.destroyed, true);
    assert.equal(stream.closed, true);
  });

  test(`${kind}: UTF-8 chunks preserve characters and pause/resume delivery`, async () => {
    const {fs, file, bytes} = runtime(kind);
    const stream = fs.createReadStream(file, {encoding: 'utf8', highWaterMark: 1});
    const chunks = [];
    const done = new Promise((resolve, reject) => stream.on('error', reject).on('close', resolve));
    let resumed = false;
    stream.on('data', chunk => {
      chunks.push(chunk);
      if (chunks.length === 1) {
        stream.pause();
        setImmediate(() => {
          assert.equal(chunks.length, 1);
          resumed = true;
          stream.resume();
        });
      }
    });
    await done;
    assert.equal(resumed, true);
    assert.equal(chunks.join(''), bytes.toString('utf8'));
  });

  test(`${kind}: destroy in data suppresses remaining chunks and end`, async () => {
    const {fs, file} = runtime(kind);
    const stream = fs.createReadStream(file, {highWaterMark: 1});
    let chunks = 0, ends = 0;
    stream.on('end', () => ++ends);
    await new Promise((resolve, reject) => stream.on('data', () => { ++chunks; stream.destroy(); })
        .on('error', reject).on('close', resolve));
    assert.equal(chunks, 1);
    assert.equal(ends, 0);
  });

  test(`${kind}: missing file emits its real error before close`, async () => {
    const {fs} = runtime(kind);
    const events = [];
    const stream = fs.createReadStream('C:\\fixture\\missing');
    await new Promise(resolve => stream.on('error', error => events.push(error.code))
        .on('close', () => { events.push('close'); resolve(); }));
    assert.deepEqual(events, ['ENOENT', 'close']);
  });

  test(`${kind}: application readline reads CRLF and trailing UTF-8 lines`, async () => {
    const {fs, readline, file} = runtime(kind);
    const input = fs.createReadStream(file, {highWaterMark: 2});
    const lines = [];
    const reader = readline.createInterface({input, crlfDelay: Infinity});
    await new Promise((resolve, reject) => reader.on('line', line => lines.push(line))
        .on('error', reject).on('close', resolve));
    assert.deepEqual(lines, ['第一行', 'second', '最后一行']);
  });

  test(`${kind}: encoded chunks preserve BOMs and malformed UTF-8 like Node`, async () => {
    const bytes = Buffer.concat([Buffer.from('\ufeff中文\ufeff😀'),
        Buffer.from([0xc0, 0xaf, 0xe0, 0x80, 0x80, 0xed, 0xa0, 0x80,
                     0xf4, 0x90, 0x80, 0x80, 0xe2, 0x28, 0xa1, 0xf0, 0x9f])]);
    // Bare main V8 uses the bootstrap fallback, not a native TextDecoder.
    const {fs, file} = runtime(kind, bytes, {nativeDecoder: kind !== 'main'});
    for (const highWaterMark of [1, 2, 3, 7]) {
      const chunks = [];
      const stream = fs.createReadStream(file, {encoding: 'utf8', highWaterMark});
      await new Promise((resolve, reject) => stream.on('data', chunk => chunks.push(chunk))
          .on('error', reject).on('close', resolve));
      assert.equal(chunks.join(''), bytes.toString('utf8'));
      assert.ok(chunks.every(chunk => chunk.length > 0));
      assert.equal(stream.bytesRead, bytes.length);
    }
  });

  test(`${kind}: attaching data listeners respects an explicitly paused stream`, async () => {
    const {fs, file} = runtime(kind, 'abc');
    const stream = fs.createReadStream(file, {highWaterMark: 1});
    let pauses = 0, resumes = 0;
    const chunks = [];
    stream.on('pause', () => ++pauses).on('resume', () => ++resumes);
    assert.equal(stream.isPaused(), false);
    stream.pause();
    stream.pause();
    stream.on('data', value => chunks.push(value));
    stream.on('data', () => {});
    await new Promise(setImmediate);
    assert.equal(stream.isPaused(), true);
    assert.deepEqual(chunks, []);
    assert.equal(pauses, 1);
    assert.equal(resumes, 0);
    const done = new Promise(resolve => stream.on('close', resolve));
    stream.resume();
    stream.resume();
    await done;
    assert.equal(resumes, 1);
    assert.equal(Buffer.concat(chunks.map(Buffer.from)).toString(), 'abc');
  });

  test(`${kind}: early abort prevents read submission and emits one error then close`, async () => {
    const {fs, file, reads} = runtime(kind);
    const controller = new AbortController();
    controller.abort();
    const stream = fs.createReadStream(file, {signal: controller.signal});
    const events = [];
    stream.on('data', () => events.push('data'));
    stream.on('ready', () => events.push('ready'));
    stream.on('end', () => events.push('end'));
    await new Promise(resolve => stream.on('error', error => events.push(error.code))
        .on('close', () => { events.push('close'); resolve(); }));
    assert.deepEqual(events, ['ABORT_ERR', 'close']);
    assert.deepEqual(reads, []);
  });

  test(`${kind}: destroying a pending read drops the late result and all data events`, async () => {
    let release;
    const readGate = new Promise(resolve => release = resolve);
    const {fs, file, reads} = runtime(kind, 'late', {readGate});
    const stream = fs.createReadStream(file);
    const events = [];
    for (const name of ['data', 'ready', 'end', 'error']) stream.on(name, () => events.push(name));
    await new Promise(setImmediate);
    assert.equal(reads.length, 1);
    const closed = new Promise(resolve => stream.on('close', () => { events.push('close'); resolve(); }));
    stream.destroy();
    await closed;
    release();
    await new Promise(setImmediate);
    assert.deepEqual(events, ['close']);
    assert.equal(stream.bytesRead, 0);
    assert.equal(stream.pending, false);
  });

  test(`${kind}: destroying in the decoder tail suppresses end and duplicate close`, async () => {
    const {fs, file} = runtime(kind, Buffer.from([0x41, 0xf0, 0x9f]));
    const stream = fs.createReadStream(file, {encoding: 'utf8', highWaterMark: 1});
    const events = [];
    stream.on('end', () => events.push('end'));
    await new Promise((resolve, reject) => stream.on('data', value => {
      events.push(value);
      if (value === '\ufffd') stream.destroy();
    }).on('error', reject).on('close', () => { events.push('close'); resolve(); }));
    assert.deepEqual(events, ['A', '\ufffd', 'close']);
  });

  test(`${kind}: unsupported descriptor options fail without submitting I/O`, async () => {
    const {fs, file, reads} = runtime(kind);
    for (const options of [{fd: 3}, {fs: {}}, {flags: 'w'}, {autoClose: false}]) {
      assert.throws(() => fs.createReadStream(file, options), {code: 'ERR_NOT_SUPPORTED'});
    }
    await Promise.resolve();
    assert.deepEqual(reads, []);
  });
}
