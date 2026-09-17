// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const realFs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {createRequire} = require('node:module');
const test = require('node:test');
const vm = require('node:vm');

function runtime(t, kind, options = {}) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-writer-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const calls = [];
  let active = 0, maxActive = 0;
  const perform = async (request, bytes) => {
    const index = calls.length;
    calls.push({operation: request.operation, bytes: Buffer.from(bytes)});
    ++active;
    maxActive = Math.max(active, maxActive);
    try {
      if (options.beforeOperation) await options.beforeOperation(index, request);
      if (request.operation === 'write_file') await realFs.promises.writeFile(request.path, bytes);
      else if (request.operation === 'append_file') await realFs.promises.appendFile(request.path, bytes);
      else throw new Error('Unexpected operation: ' + request.operation);
    } finally { --active; }
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {},
      invoke: (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        return perform(request, Buffer.from(request.data));
      },
    },
    __xenonFsCallAsync: (request, bytes) => perform(request, Buffer.from(bytes)),
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10.0',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${kind}_bootstrap.js`)), context);
  return {directory, calls, maxActive: () => maxActive,
    fs: kind === 'main' ? context.__xenonFs : context.require('fs')};
}

function closed(stream) {
  return new Promise(resolve => stream.closed ? resolve() : stream.once('close', resolve));
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: writes snapshot binary data and finish only after ordered disk writes`, async t => {
    const r = runtime(t, kind);
    const filename = path.join(r.directory, 'log.bin');
    realFs.writeFileSync(filename, 'old bytes must be truncated');
    const stream = r.fs.createWriteStream(filename);
    const done = closed(stream);
    const events = [];
    stream.on('error', error => { throw error; });
    for (const event of ['ready', 'open', 'prefinish', 'finish', 'close']) stream.on(event, () => events.push(event));
    const bytes = new Uint8Array([9, 255, 0, 128, 9]);
    stream.write(bytes.subarray(1, 4), error => { assert.equal(error, undefined); events.push('write'); });
    bytes.fill(42);
    stream.write('4142', 'hex');
    stream.end('中文', error => { assert.equal(error, undefined); events.push('end'); });
    await done;
    assert.deepEqual(realFs.readFileSync(filename), Buffer.concat([Buffer.from([255, 0, 128, 65, 66]), Buffer.from('中文')]));
    assert.equal(stream.bytesWritten, 11);
    assert.equal(stream.fd, null);
    assert.equal(stream.writableFinished, true);
    assert.equal(stream.writableLength, 0);
    assert.equal(r.maxActive(), 1);
    assert.deepEqual(events.filter(e => ['ready', 'open', 'finish', 'close'].includes(e)), ['ready', 'finish', 'close']);
    assert.ok(events.indexOf('write') < events.indexOf('finish'));
    assert.deepEqual(events, ['ready', 'write', 'prefinish', 'end', 'finish', 'close']);
  });

  test(`${kind}: append creates an empty missing file and preserves existing content`, async t => {
    const r = runtime(t, kind);
    const filename = path.join(r.directory, 'append.log');
    const first = r.fs.createWriteStream(filename, {flags: 'a'});
    const firstDone = closed(first); first.end(); await firstDone;
    assert.equal(realFs.statSync(filename).size, 0);
    realFs.writeFileSync(filename, 'prefix');
    const stream = r.fs.createWriteStream(filename, {flags: 'a'});
    const done = closed(stream); stream.end('-suffix'); await done;
    assert.equal(realFs.readFileSync(filename, 'utf8'), 'prefix-suffix');
    assert.equal(stream.bytesWritten, 7);
  });

  test(`${kind}: backpressure drains after bytes reach disk, with asynchronous callbacks`, async t => {
    let release, started;
    const gate = new Promise(resolve => { release = resolve; });
    const writeStarted = new Promise(resolve => { started = resolve; });
    const r = runtime(t, kind, {beforeOperation: async index => { if (index === 1) { started(); await gate; } }});
    const stream = r.fs.createWriteStream(path.join(r.directory, 'drain.log'), {highWaterMark: 2});
    const done = closed(stream);
    let returned = false, callbackCount = 0;
    const drained = new Promise(resolve => stream.once('drain', resolve));
    assert.equal(stream.write('ab', error => { assert.equal(error, undefined); assert.ok(returned); ++callbackCount; }), false);
    returned = true;
    await writeStarted;
    assert.equal(stream.writableLength, 2);
    assert.equal(stream.writableNeedDrain, true);
    release(); await drained;
    assert.equal(stream.bytesWritten, 2);
    assert.equal(callbackCount, 1);
    stream.end(); await done;
  });

  test(`${kind}: opening errors fail queued callbacks once and never emit finish`, async t => {
    const r = runtime(t, kind);
    const stream = r.fs.createWriteStream(path.join(r.directory, 'missing', 'log'));
    const done = closed(stream), callbackCodes = [], errors = [];
    let finishes = 0;
    stream.on('error', error => errors.push(error.code));
    stream.on('finish', () => ++finishes);
    stream.write('a', error => callbackCodes.push(error.code));
    stream.write('b', error => callbackCodes.push(error.code));
    stream.end(error => callbackCodes.push(error.code));
    await done;
    assert.deepEqual(callbackCodes, ['ENOENT', 'ENOENT', 'ENOENT']);
    assert.deepEqual(errors, ['ENOENT']);
    assert.equal(finishes, 0);
    assert.equal(r.calls.length, 1);
  });

  test(`${kind}: write failure preserves prior data and stops queued writes`, async t => {
    const r = runtime(t, kind, {beforeOperation: index => {
      if (index === 2) throw Object.assign(new Error('EACCES: blocked write'), {code: 'EACCES'});
    }});
    const filename = path.join(r.directory, 'failure.log');
    const stream = r.fs.createWriteStream(filename), done = closed(stream);
    const codes = [], errors = [];
    stream.on('error', error => errors.push(error.code));
    for (const chunk of ['good', 'bad', 'not written']) stream.write(chunk, error => codes.push(error?.code));
    stream.end(error => codes.push(error?.code));
    await done;
    assert.equal(realFs.readFileSync(filename, 'utf8'), 'good');
    assert.deepEqual(codes, [undefined, 'EACCES', 'EACCES', 'EACCES']);
    assert.deepEqual(errors, ['EACCES']);
    assert.equal(stream.bytesWritten, 4);
    assert.equal(r.calls.length, 3);
  });

  test(`${kind}: destroy waits for active I/O and drops unsent bytes with callback errors`, async t => {
    let release, started;
    const gate = new Promise(resolve => { release = resolve; });
    const writeStarted = new Promise(resolve => { started = resolve; });
    const r = runtime(t, kind, {beforeOperation: async index => { if (index === 1) { started(); await gate; } }});
    const filename = path.join(r.directory, 'destroy.log');
    const stream = r.fs.createWriteStream(filename), done = closed(stream), codes = [];
    stream.write('active', error => codes.push(error?.code));
    stream.write('queued', error => codes.push(error?.code));
    await writeStarted;
    stream.destroy(); stream.destroy();
    assert.equal(stream.closed, false);
    release(); await done;
    assert.equal(realFs.readFileSync(filename, 'utf8'), 'active');
    assert.deepEqual(codes, ['ERR_STREAM_DESTROYED', 'ERR_STREAM_DESTROYED']);
    assert.equal(r.calls.length, 2);
    assert.equal(stream.writableFinished, false);
  });

  test(`${kind}: immediate destroy skips opening; unsupported descriptors and offsets reject`, async t => {
    const r = runtime(t, kind), filename = path.join(r.directory, 'no-file');
    for (const options of [{fd: 12}, {start: 0}, {flags: 'wx'}, {autoClose: false}, {fs: {}}]) {
      assert.throws(() => r.fs.createWriteStream(filename, options), {code: 'ERR_NOT_SUPPORTED'});
    }
    const stream = r.fs.createWriteStream(filename), done = closed(stream);
    assert.throws(() => stream.write('a', 'utf8', 3), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => stream.write('a', 'bad-encoding'), {code: 'ERR_UNKNOWN_ENCODING'});
    stream.destroy(); await done;
    assert.equal(r.calls.length, 0);
    assert.equal(realFs.existsSync(filename), false);
  });

  for (const failIndex of [0, 1]) {
    test(`${kind}: close completes when destroyed in-flight operation ${failIndex} fails`, async t => {
      let release, started;
      const gate = new Promise(resolve => { release = resolve; });
      const operationStarted = new Promise(resolve => { started = resolve; });
      const r = runtime(t, kind, {beforeOperation: async index => {
        if (index === failIndex) {
          started(); await gate;
          throw Object.assign(new Error('EACCES: blocked write'), {code: 'EACCES'});
        }
      }});
      const stream = r.fs.createWriteStream(path.join(r.directory, 'failure'));
      const done = closed(stream), codes = [], errors = [];
      stream.on('error', error => errors.push(error.code));
      stream.write('data', error => codes.push(error.code));
      await operationStarted;
      stream.destroy(); release(); await done;
      assert.deepEqual(errors, ['EACCES']);
      assert.deepEqual(codes, ['EACCES']);
      assert.equal(stream.closed, true);
      assert.equal(stream.writableFinished, false);
    });
  }

  test(`${kind}: actual file-stream-rotator writes and rotates through Xenon WriteStream`,
      {skip: !process.env.XENON_TEST_FILE_STREAM_ROTATOR}, async t => {
    const r = runtime(t, kind), streams = [];
    const filename = path.resolve(process.env.XENON_TEST_FILE_STREAM_ROTATOR);
    const nativeRequire = createRequire(filename);
    const module = {exports: {}};
    const fs = Object.create(realFs);
    fs.createWriteStream = (...args) => {
      const stream = r.fs.createWriteStream(...args);
      streams.push(stream);
      return stream;
    };
    vm.runInNewContext('(function(require,module,exports,__filename,__dirname){\n' +
        realFs.readFileSync(filename, 'utf8') + '\n})',
    {Buffer, process, console: {log() {}, error() {}}, setTimeout, clearTimeout})(
        name => name === 'fs' ? fs : nativeRequire(name), module, module.exports, filename, path.dirname(filename));
    const stream = module.exports.getStream({filename: path.join(r.directory, '%DATE%'),
      frequency: 'custom', date_format: 'YYYY-MM-DD', size: '1k', verbose: false,
      file_options: {flags: 'a'}, end_stream: true, extension: '.log'});
    stream.on('error', error => { throw error; });
    stream.write('first\n'); stream.write('x'.repeat(1100) + '\n'); stream.write('last\n');
    const completions = streams.map(closed);
    stream.end(); await Promise.all(completions);
    assert.equal(streams.length, 2);
    assert.equal(realFs.readFileSync(streams[0].path, 'utf8'), 'first\n' + 'x'.repeat(1100) + '\n');
    assert.equal(realFs.readFileSync(streams[1].path, 'utf8'), 'last\n');
    assert.ok(streams.every(stream => stream.writableFinished && stream.closed));
  });
}
