// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// One behavior table for both production bootstraps. The shims provide stream
// constructor/inheritance infrastructure; they do not implement a stream queue.
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createRuntime(kind, invoke) {
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture',
        exeDir: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {},
      sendSync() { throw new Error('Unexpected native call'); },
      invoke,
    },
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10.0',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture',
    __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
  vm.runInContext(readBootstrap(filename), context, {filename});
  const modules = kind === 'main' ? {
    stream: context.__xenonStream, events: context.__xenonEvents,
    fs: context.__xenonFs,
  } : {
    stream: context.require('stream'), events: context.require('events'),
    fs: context.require('fs'),
  };
  return {context, ...modules};
}

const unsupportedCalls = stream => [
  () => new stream().pipe({}),
  () => new stream.Readable().read(),
  () => new stream.Readable().push('must not disappear'),
  () => new stream.Readable().pipe({}),
  () => new stream.Writable().write('must not disappear'),
  () => new stream.Writable().end('must not disappear'),
  () => new stream.Duplex().write('must not disappear'),
  () => new stream.Transform().write('must not disappear'),
  () => new stream.PassThrough().end('must not disappear'),
];

for (const kind of ['main', 'renderer']) {
  test(`${kind}: default stream operations never report dropped data as success`, () => {
    const {stream} = createRuntime(kind);
    for (const operation of unsupportedCalls(stream)) {
      assert.throws(operation, {code: 'ERR_NOT_SUPPORTED'});
    }
  });

  test(`${kind}: stream callback operations fail once and asynchronously`, async () => {
    const {stream} = createRuntime(kind);
    const writable = new stream.Writable();
    let finishes = 0;
    writable.on('finish', () => ++finishes);
    const operations = [
      callback => writable.write('bytes', callback),
      callback => writable.end('bytes', callback),
      callback => new stream.Transform()._transform('bytes', 'utf8', callback),
      callback => stream.pipeline(new stream.Readable(), writable, callback),
      callback => stream.finished(writable, callback),
    ];
    for (const operation of operations) {
      let returned = false;
      let count = 0;
      await new Promise((resolve, reject) => {
        operation(error => {
          ++count;
          try {
            assert.equal(returned, true);
            assert.equal(error.code, 'ERR_NOT_SUPPORTED');
            resolve();
          } catch (failure) {
            reject(failure);
          }
        });
        returned = true;
      });
      assert.equal(count, 1);
    }
    assert.equal(finishes, 0);
  });

  test(`${kind}: completion callback validation and cleanup are real`, async () => {
    const {stream} = createRuntime(kind);
    const writable = new stream.Writable();
    for (const callback of [null, 1, 'callback', {}]) {
      assert.throws(() => writable.write('bytes', 'utf8', callback),
          {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
    }
    for (const callback of [undefined, null, 1, 'callback', {}]) {
      assert.throws(() => stream.pipeline(new stream.Readable(), writable, callback),
          {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
      assert.throws(() => stream.finished(writable, {}, callback),
          {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
    }
    let called = false;
    const cleanup = stream.finished(writable, () => { called = true; });
    cleanup();
    await Promise.resolve();
    assert.equal(called, false);
  });

  test(`${kind}: stream constructors preserve implemented userland behavior`, () => {
    const {stream, events} = createRuntime(kind);
    assert.equal(stream.Stream, stream);
    assert.notEqual(stream.Duplex, stream.Readable);
    const read = () => {};
    const write = () => {};
    const transform = () => {};
    assert.equal(new stream.Readable({read})._read, read);
    assert.equal(new stream.Writable({write})._write, write);
    assert.equal(new stream.Transform({transform})._transform, transform);
    for (const Constructor of [stream, stream.Readable, stream.Writable,
      stream.Duplex, stream.Transform, stream.PassThrough]) {
      assert.equal(Constructor() instanceof Constructor, true);
    }
    function UserlandSink() { stream.call(this); }
    Object.setPrototypeOf(UserlandSink.prototype, stream.prototype);
    UserlandSink.prototype.write = function(value) {
      this.emit('data', value);
      return true;
    };
    const sink = new UserlandSink();
    assert.equal(sink instanceof events, true);
    const received = [];
    sink.on('data', value => received.push(value));
    assert.equal(sink.write('kept'), true);
    assert.deepEqual(received, ['kept']);
  });

  test(`${kind}: EventEmitter inheritance and listener contracts stay aligned`, () => {
    const {events: Events} = createRuntime(kind);
    assert.equal(Events.EventEmitter, Events);
    assert.equal(Events.prototype.on, Events.prototype.addListener);
    assert.equal(Events.prototype.off, Events.prototype.removeListener);
    const emitter = new Events();
    const order = [];
    const onData = () => order.push('data');
    emitter.on('data', onData).on('data', onData);
    emitter.once('data', () => order.push('once'));
    emitter.removeListener('data', onData);
    assert.equal(emitter.listenerCount('data', onData), 1);
    emitter.emit('data');
    emitter.emit('data');
    assert.deepEqual(order, ['data', 'once', 'data']);
    assert.throws(() => emitter.emit('error', new Error('unhandled')));
  });

  test(`${kind}: copied EventEmitter methods initialize independent listener state`, () => {
    const {events: Events} = createRuntime(kind);
    // sqlite3 inherits by copying enumerable methods, without invoking Events.
    function ForeignHandle() {}
    for (const key in Events.prototype) {
      ForeignHandle.prototype[key] = Events.prototype[key];
    }
    const first = new ForeignHandle();
    const second = new ForeignHandle();
    assert.equal(first.emit('open'), false);
    assert.equal(second.listenerCount('open'), 0);
    const received = [];
    function onOpen(value) { received.push([this === first, value]); }
    first.on('open', onOpen).prependOnceListener('open', onOpen);
    assert.equal(second.emit('open', 'wrong'), false);
    assert.equal(first.emit('open', 'ready'), true);
    assert.deepEqual(received, [[true, 'ready'], [true, 'ready']]);
    assert.equal(first.listenerCount('open'), 1);
    first.removeAllListeners();
    assert.equal(first.eventNames().length, 0);
    assert.throws(() => second.emit('error', new Error('native failure')),
        /native failure/);

    // A handle inheriting an initialized emitter must not share its listeners.
    const parent = new Events();
    parent.on('close', () => { throw Error('shared listener state'); });
    const child = Object.create(parent);
    assert.equal(child.emit('close'), false);
    child.on('child', onOpen);
    assert.equal(parent.listenerCount('child'), 0);
    assert.equal(parent.listenerCount('close'), 1);
  });

  test(`${kind}: EventEmitter base aliases do not reenter an overridden method`, () => {
    const {events: Events} = createRuntime(kind);
    const emitter = new Events();
    let registrations = 0, removals = 0;
    emitter.on = function(...args) {
      ++registrations;
      return Events.prototype.addListener.apply(this, args);
    };
    emitter.removeListener = function(...args) {
      ++removals;
      return Events.prototype.off.apply(this, args);
    };
    const callback = () => {};
    emitter.on('data', callback);
    emitter.removeListener('data', callback);
    assert.equal(registrations, 1);
    assert.equal(removals, 1);
    assert.equal(emitter.listenerCount('data'), 0);
  });

  test(`${kind}: unsupported fs Promise permissions and descriptors reject consistently`, async () => {
    const {fs} = createRuntime(kind);
    for (const method of ['open', 'chmod', 'chown']) {
      await assert.rejects(fs.promises[method]('unused', 0o644),
          {code: 'ERR_NOT_SUPPORTED'});
    }
  });
}

test('renderer HTTP uses its working native boundary independently of stream base classes', async () => {
  const requests = [];
  const {context} = createRuntime('renderer', async (channel, request) => {
    assert.equal(channel, '__xenon:net-request');
    requests.push(request);
    return {statusCode: 201, statusMessage: 'Created', headers: {'x-fixture': 'ok'},
      finalUrl: request.url, bodyBase64: Buffer.from([0, 255, 128, 65]).toString('base64')};
  });
  const bytes = vm.runInContext('Buffer.from([9, 0, 255, 128, 65, 9]).subarray(1, 5)', context);
  const response = await new Promise((resolve, reject) => {
    const req = context.require('http').request({hostname: 'fixture.invalid',
      method: 'POST', path: '/bytes'}, incoming => {
      const chunks = [];
      incoming.on('data', chunk => chunks.push(Buffer.from(chunk)));
      incoming.on('end', () => resolve({status: incoming.statusCode,
        data: Buffer.concat(chunks)}));
    });
    req.on('error', reject);
    req.end(bytes);
  });
  assert.equal(response.status, 201);
  assert.deepEqual(Array.from(response.data), [0, 255, 128, 65]);
  assert.equal(requests.length, 1);
  assert.deepEqual(Array.from(Buffer.from(requests[0].bodyBase64, 'base64')),
      [0, 255, 128, 65]);
});

test('main HTTP and HTTPS propagate native failures; TLS servers stay unsupported', async () => {
  const {context} = createRuntime('main');
  context.__xenonHttpRequest = () => ({id: 1, promise: Promise.reject(
      Object.assign(new Error('native connection failed'), {code: 'ECONNREFUSED'}))});
  context.__xenonHttpAbort = () => {};
  let callbacks = 0;
  for (const [http, protocol] of [[context.__xenonHttp, 'http:'], [context.__xenonHttps, 'https:']]) {
    assert.equal(typeof http.Agent, 'function');
    if (protocol === 'https:') assert.throws(() => http.createServer(), {code: 'ERR_NOT_SUPPORTED'});
    else assert.equal(http.createServer().listening, false);
    for (const method of ['request', 'get']) {
      await new Promise(resolve => {
        const request = http[method](protocol + '//fixture.invalid', () => ++callbacks);
        request.on('error', error => { assert.equal(error.code, 'ECONNREFUSED'); resolve(); });
        if (method === 'request') request.end();
      });
    }
  }
  await Promise.resolve();
  assert.equal(callbacks, 0);
});

test('renderer HTTP snapshots binary writes and honors encoded string chunks', async () => {
  let request;
  const {context} = createRuntime('renderer', async (channel, value) => {
    assert.equal(channel, '__xenon:net-request');
    request = value;
    return {statusCode: 204, bodyBase64: ''};
  });
  const bytes = vm.runInContext('Buffer.from([9, 0, 255, 128, 9])', context);
  const wide = vm.runInContext('new Uint16Array([0x1234, 0xff80])', context);
  const data = vm.runInContext('new DataView(new Uint8Array([9, 0, 255, 9]).buffer, 1, 2)', context);
  const rawWide = Buffer.from(new Uint8Array(wide.buffer));
  const done = new Promise((resolve, reject) => {
    const req = context.require('http').request({hostname: 'fixture.invalid',
      method: 'POST', path: '/chunks'}, incoming => incoming.on('end', resolve));
    req.on('error', reject);
    req.write(bytes.subarray(1, 4));
    bytes.fill(7);
    req.write(wide);
    wide.fill(7);
    req.write(data);
    new Uint8Array(data.buffer).fill(7);
    req.write('41', 'hex');
    req.write('中', 'utf8');
    req.write('ÿ', 'latin1');
    req.write('中', 'utf16le');
    assert.throws(() => req.write('ignored', 'invalid-encoding'),
        {code: 'ERR_UNKNOWN_ENCODING'});
    req.end('Qg==', 'base64');
  });
  await done;
  assert.deepEqual(Buffer.from(request.bodyBase64, 'base64'),
      Buffer.concat([Buffer.from([0, 255, 128]), rawWide, Buffer.from([0, 255, 65]), Buffer.from('中'),
        Buffer.from('ÿ', 'latin1'), Buffer.from('中', 'utf16le'), Buffer.from('B')]));
});
