// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const realFs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const nativeStream = require('node:stream');
const nativePromises = require('node:stream/promises');
const {getEventListeners} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

const tick = () => new Promise(resolve => setImmediate(resolve));
const lifecycleEvents = ['data', 'drain', 'end', 'finish', 'close', 'error'];
const listeners = stream => Object.fromEntries(lifecycleEvents.map(name => [name, stream.listenerCount(name)]));

function runtime(t, kind, hooks = {}) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-pipeline-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const calls = [];
  const perform = async (request, bytes) => {
    calls.push({...request, bytes: bytes?.byteLength});
    if (request.operation === 'read_file') {
      const contents = await realFs.promises.readFile(request.path);
      return Uint8Array.from(contents.subarray(request.start || 0,
          request.end === undefined ? undefined : request.end + 1));
    }
    if (request.operation === 'write_file') return realFs.promises.writeFile(request.path, bytes);
    if (request.operation === 'append_file') {
      await hooks.beforeWrite?.(request, bytes);
      return realFs.promises.appendFile(request.path, bytes);
    }
    throw new Error('Unexpected operation: ' + request.operation);
  };
  const context = vm.createContext({
    __xenonPaths: {platform: process.platform, arch: process.arch, endianness: os.endianness()},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, AbortController,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: directory, execPath: process.execPath,
        exeDir: path.dirname(process.execPath)}),
      setDispatchHandler() {},
      invoke: async (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        const value = await perform(request, request.data);
        return value instanceof Uint8Array ? value.buffer : value;
      },
    },
    __xenonFsCallAsync: perform,
    __xenonPlatform: process.platform, __xenonArch: process.arch,
    __xenonEndianness: os.endianness(), __xenonOsRelease: os.release(),
    __xenonAppPath: directory, __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`)), context);
  const stream = kind === 'main' ? context.__xenonStream : context.require('stream');
  return {directory, calls, context, stream,
    fs: kind === 'main' ? context.__xenonFs : context.require('fs')};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: callback pipeline copies real files with backpressure and retains late-error guards`, {timeout: 5000}, async t => {
    const {directory, fs, stream} = runtime(t, kind, {
      beforeWrite: () => new Promise(resolve => setTimeout(resolve, 1)),
    });
    const input = path.join(directory, 'source.bin');
    const output = path.join(directory, 'destination.bin');
    const bytes = Buffer.from(Array.from({length: 32768}, (_, index) => index % 256));
    realFs.writeFileSync(input, bytes);
    const source = fs.createReadStream(input, {highWaterMark: 1024});
    const destination = fs.createWriteStream(output, {highWaterMark: 512});
    let pauses = 0, drains = 0, callbacks = 0;
    source.on('pause', () => ++pauses);
    destination.on('drain', () => ++drains);
    const sourceBefore = listeners(source), destinationBefore = listeners(destination);
    await new Promise((resolve, reject) => {
      const returned = stream.pipeline(source, destination, failure => {
        ++callbacks;
        failure ? reject(failure) : resolve();
      });
      assert.equal(returned, destination);
    });
    assert.equal(callbacks, 1);
    assert.deepEqual(realFs.readFileSync(output), bytes);
    assert.ok(pauses > 2 && drains > 2, 'write(false) pauses reads until actual drains');
    assert.equal(source.closed, true);
    assert.equal(destination.closed, true);
    assert.equal(destination.writableFinished, true);
    // Node intentionally retains pipeline's terminal guards. Active pumping
    // listeners are gone, and late transport errors remain safely observed.
    for (const [value, before] of [[source, sourceBefore], [destination, destinationBefore]]) {
      assert.equal(value.listenerCount('data'), before.data);
      assert.equal(value.listenerCount('drain'), before.drain);
      assert.doesNotThrow(() => value.emit('error', new Error('late transport error')));
    }
  });

  test(`${kind}: Promise pipeline supports an implemented transform between real file streams`, {timeout: 5000}, async t => {
    const {directory, fs, stream, context} = runtime(t, kind);
    const input = path.join(directory, 'input.txt'), output = path.join(directory, 'output.txt');
    realFs.writeFileSync(input, 'first line\nsecond line\n');
    const transform = new nativeStream.Transform({
      transform(chunk, _encoding, callback) { callback(null, chunk.toString().toUpperCase()); },
    });
    await stream.promises.pipeline([fs.createReadStream(input, {highWaterMark: 3}),
      transform, fs.createWriteStream(output)]);
    assert.equal(realFs.readFileSync(output, 'utf8'), 'FIRST LINE\nSECOND LINE\n');
    assert.equal(transform.closed, true);
    assert.doesNotThrow(() => transform.emit('error', new Error('late transform error')));
    if (kind === 'renderer') {
      assert.equal(context.require('stream/promises'), stream.promises);
      assert.equal(context.require('node:stream/promises'), stream.promises);
    }
  });

  test(`${kind}: finished waits for both duplex sides and returns exact listener cleanup`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const duplex = new nativeStream.PassThrough({autoDestroy: false});
    const ownError = () => {};
    duplex.on('error', ownError);
    const before = listeners(duplex);
    let completed = false, cleanup;
    const completion = new Promise((resolve, reject) => {
      cleanup = stream.finished(duplex, failure => {
        completed = true;
        failure ? reject(failure) : resolve();
      });
    });
    duplex.end('must be consumed');
    await tick();
    assert.equal(duplex.writableFinished, true);
    assert.equal(completed, false);
    duplex.resume();
    await completion;
    assert.equal(duplex.destroyed, false);
    assert.ok(duplex.listenerCount('error') > before.error, 'callback finished retains its guard until cleanup');
    cleanup();
    cleanup();
    assert.deepEqual(listeners(duplex), before);
    duplex.destroy();
  });

  test(`${kind}: finished handles already-completed streams asynchronously and cleanup cancels delivery`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const source = nativeStream.Readable.from([]);
    source.resume();
    await nativePromises.finished(source, {cleanup: true});
    let synchronous = true;
    await new Promise((resolve, reject) => {
      const cleanup = stream.finished(source, failure => {
        try { assert.equal(synchronous, false); cleanup(); failure ? reject(failure) : resolve(); }
        catch (error) { reject(error); }
      });
      synchronous = false;
    });
    let called = false;
    const cleanup = stream.finished(source, () => { called = true; });
    cleanup();
    await tick();
    assert.equal(called, false);
    assert.equal(source.listenerCount('error'), 0);
  });

  test(`${kind}: finished can observe one duplex side without waiting for the other`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const writableOnly = new nativeStream.PassThrough();
    const writeDone = stream.promises.finished(writableOnly, {readable: false, cleanup: true});
    writableOnly.end('unconsumed bytes');
    await writeDone;
    assert.equal(writableOnly.readableEnded, false);
    assert.equal(writableOnly.closed, false);
    writableOnly.destroy();
    const readableOnly = new nativeStream.Duplex({
      read() { this.push(null); }, write(_chunk, _encoding, callback) { callback(); },
    });
    const readDone = stream.promises.finished(readableOnly, {writable: false, cleanup: true});
    readableOnly.resume();
    await readDone;
    assert.equal(readableOnly.writableFinished, false);
    assert.equal(readableOnly.closed, false);
    readableOnly.destroy();
  });

  test(`${kind}: finished preserves a real file read failure even when attached after close`, {timeout: 2000}, async t => {
    const {directory, fs, stream} = runtime(t, kind);
    const source = fs.createReadStream(path.join(directory, 'missing'));
    let original;
    source.on('error', failure => { original = failure; });
    await new Promise(resolve => source.once('close', resolve));
    assert.equal(original.code, 'ENOENT');
    await assert.rejects(stream.promises.finished(source, {cleanup: true}), failure => failure === original);
  });

  test(`${kind}: Promise finished reports premature close and cleans up requested listeners`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const writable = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(); }});
    const before = listeners(writable);
    const done = stream.promises.finished(writable, {cleanup: true});
    writable.destroy();
    await assert.rejects(done, {code: 'ERR_STREAM_PREMATURE_CLOSE'});
    assert.deepEqual(listeners(writable), before);
  });

  test(`${kind}: aborting finished cancels observation without destroying the stream`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const source = new nativeStream.Readable({read() {}});
    const controller = new AbortController();
    const reason = new Error('stop observing');
    const before = listeners(source);
    const done = stream.promises.finished(source, {signal: controller.signal, cleanup: true});
    controller.abort(reason);
    await assert.rejects(done, error => error.code === 'ABORT_ERR' && error.cause === reason);
    assert.equal(source.destroyed, false);
    assert.deepEqual(listeners(source), before);
    assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
    source.destroy();
  });

  test(`${kind}: a destination error destroys all stages and preserves the first failure`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const failure = Object.assign(new Error('disk failed'), {code: 'EIO'});
    const source = nativeStream.Readable.from(['one', 'two', 'three']);
    const transform = new nativeStream.PassThrough();
    const destination = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(failure); }});
    await assert.rejects(stream.promises.pipeline(source, transform, destination), error => error === failure);
    await tick(); // Node destroy schedules close separately from pipeline rejection.
    for (const stage of [source, transform, destination]) {
      assert.equal(stage.destroyed, true);
      assert.equal(stage.closed, true);
      assert.doesNotThrow(() => stage.emit('error', new Error('late destroy error')));
      assert.equal(stage.listenerCount('data'), 0);
      assert.equal(stage.listenerCount('drain'), 0);
    }
  });

  test(`${kind}: source errors and early destination close cannot report pipeline success`, {timeout: 3000}, async t => {
    const {stream} = runtime(t, kind);
    const sourceFailure = new Error('read failed');
    const source = new nativeStream.Readable({read() { this.destroy(sourceFailure); }});
    const destination = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(); }});
    await assert.rejects(stream.promises.pipeline(source, destination), error => error === sourceFailure);
    const nextSource = nativeStream.Readable.from(['a', 'b', 'c']);
    const early = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(); this.destroy(); }});
    await assert.rejects(stream.promises.pipeline(nextSource, early), error =>
      ['ERR_STREAM_PREMATURE_CLOSE', 'ERR_STREAM_DESTROYED'].includes(error.code));
    assert.equal(nextSource.destroyed, true);
  });

  test(`${kind}: abort waits for an already-submitted real write before settling`, {timeout: 3000}, async t => {
    let release, writing;
    const gate = new Promise(resolve => { release = resolve; });
    const entered = new Promise(resolve => { writing = resolve; });
    const {directory, fs, stream} = runtime(t, kind, {beforeWrite: async () => { writing(); await gate; }});
    const input = path.join(directory, 'input.bin'), output = path.join(directory, 'output.bin');
    realFs.writeFileSync(input, Buffer.alloc(8192, 42));
    const source = fs.createReadStream(input, {highWaterMark: 1024});
    const destination = fs.createWriteStream(output, {highWaterMark: 1});
    const controller = new AbortController();
    const reason = new Error('cancel copy');
    let settled = false;
    const done = stream.promises.pipeline(source, destination, {signal: controller.signal});
    const checked = assert.rejects(done, error => error.code === 'ABORT_ERR' && error.cause === reason);
    done.then(() => { settled = true; }, () => { settled = true; });
    await entered;
    controller.abort(reason);
    await tick();
    assert.equal(settled, false, 'native write cannot be claimed cancelled while still in flight');
    release();
    await checked;
    assert.equal(source.closed, true);
    assert.equal(destination.closed, true);
    assert.equal(realFs.statSync(output).size, 1024);
    assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
    assert.doesNotThrow(() => destination.emit('error', new Error('late write error')));
  });

  test(`${kind}: end:false leaves queued writes and the destination usable until explicit end`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    const chunks = [];
    const destination = new nativeStream.Writable({write(chunk, _encoding, callback) {
      setImmediate(() => { chunks.push(Buffer.from(chunk)); callback(); });
    }});
    await stream.promises.pipeline(nativeStream.Readable.from(['first']), destination, {end: false});
    assert.equal(destination.writableEnded, false);
    assert.equal(destination.destroyed, false);
    // As with Node, pipeline end:false waits for read completion and
    // backpressure, not for every accepted below-HWM write callback.
    assert.equal(Buffer.concat(chunks).toString(), '');
    assert.ok(destination.writableLength > 0);
    destination.end('second');
    await nativePromises.finished(destination, {cleanup: true});
    assert.equal(Buffer.concat(chunks).toString(), 'firstsecond');
  });

  test(`${kind}: end:false still rejects a destination closed before the source ends`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    for (const alreadyClosed of [false, true]) {
      const source = new nativeStream.Readable({read() {}});
      const destination = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(); }});
      if (alreadyClosed) {
        destination.destroy();
        await tick();
      }
      const done = stream.promises.pipeline(source, destination, {end: false});
      const rejected = assert.rejects(done, {code: 'ERR_STREAM_PREMATURE_CLOSE'});
      destination.destroy();
      await tick();
      source.push(null);
      await rejected;
      assert.equal(source.destroyed, true);
      assert.doesNotThrow(() => source.emit('error', new Error('late source error')));
      assert.doesNotThrow(() => destination.emit('error', new Error('late sink error')));
    }
  });

  test(`${kind}: emitClose:false completes real file transfers without an invented close event`, {timeout: 2000}, async t => {
    const {directory, fs, stream} = runtime(t, kind);
    const input = path.join(directory, 'silent-source.txt'), output = path.join(directory, 'silent-output.txt');
    realFs.writeFileSync(input, 'no close event');
    const source = fs.createReadStream(input, {emitClose: false});
    const destination = fs.createWriteStream(output, {emitClose: false});
    let closes = 0;
    source.on('close', () => ++closes);
    destination.on('close', () => ++closes);
    await stream.promises.pipeline(source, destination);
    assert.equal(realFs.readFileSync(output, 'utf8'), 'no close event');
    assert.equal(closes, 0);
    assert.equal(source.readableEnded, true);
    assert.equal(destination.writableFinished, true);
    assert.doesNotThrow(() => source.emit('error', new Error('late silent-source error')));
    assert.doesNotThrow(() => destination.emit('error', new Error('late silent-sink error')));
  });

  test(`${kind}: pre-aborted pipelines do not begin consuming and remove abort listeners`, {timeout: 2000}, async t => {
    const {stream} = runtime(t, kind);
    let reads = 0;
    const source = new nativeStream.Readable({read() { ++reads; this.push('data'); }});
    const destination = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(); }});
    const controller = new AbortController();
    controller.abort('already stopped');
    await assert.rejects(stream.promises.pipeline(source, destination, {signal: controller.signal}), {code: 'ABORT_ERR'});
    assert.equal(reads, 0);
    assert.equal(source.destroyed, true);
    assert.equal(destination.destroyed, true);
    assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${kind}: pipeline accepts actual iterable, async generator and Web sources`, {timeout: 2000}, async t => {
    const {stream, context} = runtime(t, kind);
    const web = kind === 'main' ? context.__xenonStreamWeb : context.require('stream/web');
    for (const source of [['one', 'two'], (async function*() { yield 'one'; yield 'two'; })(),
      async function*() { yield 'one'; yield 'two'; }, web.ReadableStream.from(['one', 'two'])]) {
      const chunks = [];
      const destination = new stream.Writable({write(chunk, _encoding, callback) {
        chunks.push(chunk.toString()); callback();
      }});
      await stream.promises.pipeline(source, destination);
      assert.equal(chunks.join(''), 'onetwo');
      assert.equal(destination.writableFinished, true);
    }
  });

  test(`${kind}: missing read implementations and invalid pipeline arguments report Node errors`, async t => {
    const {stream} = runtime(t, kind);
    await assert.rejects(stream.promises.pipeline(new stream.Readable(),
      new stream.Writable({write(_chunk, _encoding, done) { done(); }})),
      {code: 'ERR_METHOD_NOT_IMPLEMENTED'});
    const empty = new stream.Writable();
    const completed = stream.promises.finished(empty, {cleanup: true});
    empty.end(); await completed;
    assert.equal(empty.writableFinished, true);
    const destination = new nativeStream.Writable({write(_chunk, _encoding, callback) { callback(); }});
    assert.throws(() => stream.pipeline(destination, undefined), {code: 'ERR_INVALID_ARG_TYPE'});
    await assert.rejects(stream.promises.pipeline(nativeStream.Readable.from([]), destination,
      {signal: {aborted: false}}), {name: 'TypeError'});
    destination.destroy();
  });
}
