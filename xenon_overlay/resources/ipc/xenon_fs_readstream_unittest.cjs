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
  const returnedSizes = [];
  const read = request => {
    reads.push({...request});
    assert.equal(request.operation, 'read_file');
    assert.equal(request.returnBytes, true);
    if (request.path !== file) throw Object.assign(new Error('ENOENT: missing file'), {code: 'ENOENT'});
    const start = request.start === undefined ? 0 : request.start;
    const end = request.end === undefined ? bytes.length : Math.min(bytes.length, request.end + 1);
    // Model native range reads with independent result storage. Native disk and
    // ASAR behavior is covered separately by the filesystem bridge tests.
    const result = Uint8Array.from(bytes.subarray(start, end));
    returnedSizes.push(result.byteLength);
    return result;
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
      sendSync: (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        return read(request).buffer;
      },
      invoke: async (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        const result = read(request).buffer;
        if (options.readGate) await options.readGate;
        return result;
      },
    },
    __xenonFsCall: read,
    __xenonFsCallAsync: async request => {
      const result = read(request);
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
  return {file, bytes, reads, returnedSizes,
    fs: kind === 'main' ? context.__xenonFs : context.require('fs'),
    readline: kind === 'main' ? context.__xenonReadline : context.require('readline')};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: read stream delivers exact byte range and one end/close`, async () => {
    const {fs, file, bytes, reads, returnedSizes} = runtime(kind);
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
    assert.deepEqual(reads, [{operation: 'read_file', path: file,
      returnBytes: true, start: 1, end: 12}]);
    assert.deepEqual(returnedSizes, [12]);
  });

  test(`${kind}: read stream ranges preserve inclusive end, EOF and safe-integer offsets`, async () => {
    for (const {contents, options, expected} of [
      {contents: '0123456789', options: {}, expected: '0123456789'},
      {contents: '0123456789', options: {end: 0}, expected: '0'},
      {contents: '0123456789', options: {start: 4, end: 4}, expected: '4'},
      {contents: '0123456789', options: {start: 7}, expected: '789'},
      {contents: '0123456789', options: {start: 7, end: 100}, expected: '789'},
      {contents: '0123456789', options: {start: 7, end: Infinity}, expected: '789'},
      {contents: '0123456789', options: {start: 7, end: Number.MAX_SAFE_INTEGER}, expected: '789'},
      {contents: '0123456789', options: {start: 10}, expected: ''},
      {contents: '0123456789', options: {start: 11, end: 11}, expected: ''},
      {contents: '0123456789', options: {start: Number.MAX_SAFE_INTEGER}, expected: ''},
      {contents: '0123456789', options: {start: Number.MAX_SAFE_INTEGER,
        end: Number.MAX_SAFE_INTEGER}, expected: ''},
      {contents: '', options: {start: 0, end: 0}, expected: ''},
    ]) {
      const {fs, file, reads, returnedSizes} = runtime(kind, contents);
      const stream = fs.createReadStream(file, options), chunks = [], events = [];
      for (const event of ['ready', 'end', 'close']) stream.on(event, () => events.push(event));
      await new Promise((resolve, reject) => stream.on('data', bytes => chunks.push(Buffer.from(bytes)))
          .on('error', reject).on('close', resolve));
      assert.equal(Buffer.concat(chunks).toString(), expected);
      assert.deepEqual(events, ['ready', 'end', 'close']);
      assert.equal(stream.bytesRead, Buffer.byteLength(expected));
      assert.equal(reads.length, 1);
      assert.equal(reads[0].start, options.start === undefined ? 0 : options.start);
      if (options.end !== undefined && options.end !== Infinity) assert.equal(reads[0].end, options.end);
      else assert.equal(Object.hasOwn(reads[0], 'end'), false);
      assert.deepEqual(returnedSizes, [Buffer.byteLength(expected)]);
    }
  });

  test(`${kind}: a 64 KiB range of a 16 MiB file receives and retains only the requested range`, async () => {
    const fixture = Buffer.alloc(16 * 1024 * 1024);
    for (let i = 0; i < fixture.length; ++i) fixture[i] = (i * 73 + 19) & 255;
    const start = 8 * 1024 * 1024 + 17, size = 64 * 1024;
    const {fs, file, reads, returnedSizes} = runtime(kind, fixture);
    const stream = fs.createReadStream(file, {start, end: start + size - 1, highWaterMark: 16384});
    const chunks = [], backingBuffers = new Set();
    await new Promise((resolve, reject) => stream.on('data', chunk => {
      assert.equal(chunk.buffer.byteLength, size);
      backingBuffers.add(chunk.buffer);
      chunks.push(Buffer.from(chunk));
    }).on('error', reject).on('close', resolve));
    assert.deepEqual(Buffer.concat(chunks), fixture.subarray(start, start + size));
    assert.equal(chunks.length, 4);
    assert.equal(backingBuffers.size, 1);
    assert.equal(reads.length, 1);
    assert.deepEqual(returnedSizes, [size]);
    assert.equal(stream.bytesRead, size);
  });

  test(`${kind}: ordinary readFile APIs keep the full-file request contract`, async () => {
    const {fs, file, bytes, reads} = runtime(kind, 'whole file');
    assert.deepEqual(Buffer.from(fs.readFileSync(file)), bytes);
    assert.deepEqual(Buffer.from(await fs.promises.readFile(file)), bytes);
    const result = await new Promise((resolve, reject) => fs.readFile(file,
        (error, value) => error ? reject(error) : resolve(value)));
    assert.deepEqual(Buffer.from(result), bytes);
    assert.equal(reads.length, 3);
    for (const request of reads) {
      assert.equal(Object.hasOwn(request, 'start'), false);
      assert.equal(Object.hasOwn(request, 'end'), false);
    }
  });

  test(`${kind}: range UTF-8 boundaries decode the selected bytes without reading adjacent characters`, async () => {
    const bytes = Buffer.from('前中文😀后');
    const {fs, file} = runtime(kind, bytes, {nativeDecoder: kind !== 'main'});
    for (const [start, end] of [[1, 4], [3, 8], [8, 10]]) {
      const chunks = [];
      const stream = fs.createReadStream(file, {start, end, encoding: 'utf8', highWaterMark: 1});
      await new Promise((resolve, reject) => stream.on('data', chunk => chunks.push(chunk))
          .on('error', reject).on('close', resolve));
      assert.equal(chunks.join(''), bytes.subarray(start, end + 1).toString('utf8'));
      assert.equal(stream.bytesRead, end - start + 1);
    }
  });

  test(`${kind}: invalid ranges fail before native submission`, async () => {
    const {fs, file, reads} = runtime(kind);
    for (const options of [
      {start: -1}, {start: 0.5}, {start: Infinity}, {start: Number.MAX_SAFE_INTEGER + 1},
      {end: -1}, {end: 0.5}, {end: NaN}, {end: Number.MAX_SAFE_INTEGER + 1},
      {start: 2, end: 1}, {start: '1'}, {end: '2'},
    ]) {
      assert.throws(() => fs.createReadStream(file, options), {code: 'ERR_OUT_OF_RANGE'});
    }
    await Promise.resolve();
    assert.deepEqual(reads, []);
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

test('renderer: range streams keep synthetic filesystem data and errors inside the virtual mount', async () => {
  const {fs, reads} = runtime('renderer');
  const filename = 'chrome://fixture/data.bin';
  fs.writeFileSync(filename, new Uint8Array([0, 255, 128, 65, 9]));
  const chunks = [];
  const stream = fs.createReadStream(filename, {start: 1, end: 2, highWaterMark: 1});
  await new Promise((resolve, reject) => stream.on('data', chunk => {
    assert.equal(chunk.buffer.byteLength, 2);
    chunks.push(Buffer.from(chunk));
  }).on('error', reject).on('close', resolve));
  assert.deepEqual(Buffer.concat(chunks), Buffer.from([255, 128]));
  assert.equal(stream.bytesRead, 2);
  assert.deepEqual(Buffer.from(fs.readFileSync(filename)), Buffer.from([0, 255, 128, 65, 9]));
  assert.deepEqual(Buffer.from(await fs.promises.readFile(filename)), Buffer.from([0, 255, 128, 65, 9]));
  fs.mkdirSync('chrome://fixture/directory');
  for (const [path, code] of [
    ['chrome://fixture/missing', 'ENOENT'], ['chrome://fixture/directory', 'EISDIR'],
  ]) {
    const events = [];
    const failed = fs.createReadStream(path, {start: 1, end: 2});
    for (const name of ['ready', 'data', 'end']) failed.on(name, () => events.push(name));
    await new Promise(resolve => failed.on('error', error => events.push(error.code))
        .on('close', () => { events.push('close'); resolve(); }));
    assert.deepEqual(events, [code, 'close']);
  }
  assert.deepEqual(reads, []);
});
