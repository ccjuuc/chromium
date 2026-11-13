// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrap, readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const native = require('node:zlib');

function renderer() {
  const streams = streamFixture();
  const decodeOptions = options => options && options.dictionary !== undefined
    ? {...options, dictionary: Buffer.from(options.dictionary, 'base64')} : options;
  const streamRequest = (request, asynchronous) => {
    const decoded = {...request, options: decodeOptions(request.options)};
    if (request.action === 'process') decoded.data = Buffer.from(request.data, 'base64');
    const result = streams.streamCall(decoded, asynchronous);
    const encode = value => request.action === 'process'
      ? {...value, output: Buffer.from(value.output).toString('base64')} : value;
    return asynchronous ? Promise.resolve(result).then(encode) : encode(result);
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams,
    queueMicrotask, atob, btoa,
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}), setDispatchHandler() {},
      sendSync(channel, request) {
        if (channel === '__xenon:zlib-stream') return streamRequest(request, false);
        assert.equal(channel, '__xenon:zlib');
        return native[request.operation + 'Sync'](
            Buffer.from(request.dataBase64, 'base64'), decodeOptions(request.options)).toString('base64');
      },
      invoke(channel, request) {
        if (channel === '__xenon:zlib-stream') return streamRequest(request, true);
        assert.equal(channel, '__xenon:zlib');
        return new Promise((resolve, reject) => native[request.operation](
            Buffer.from(request.dataBase64, 'base64'), decodeOptions(request.options),
            (error, output) => error ? reject(error) : resolve(output.toString('base64'))));
      },
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js')), context);
  return {context, zlib: context.require('zlib'), streams};
}

function main() {
  const factory = readBootstrapPart('common/zlib.js');
  const context = vm.createContext({Buffer, queueMicrotask,
    call(operation, input, options, asynchronous) {
      if (!asynchronous) return native[operation + 'Sync'](input, options);
      return new Promise((resolve, reject) => native[operation](input, options,
          (error, output) => error ? reject(error) : resolve(output)));
    },
  });
  return {context, zlib: vm.runInContext(factory + '\ncreateZlibModule(call)', context)};
}

for (const [label, create] of [['main', main], ['renderer', renderer]]) {
  test(`${label} zlib sync operations round trip binary subviews and strings`, () => {
    const {context, zlib} = create();
    const input = vm.runInContext('new Uint8Array([99, 0, 255, 128, 65, 99]).subarray(1, 5)', context);
    for (const [compress, decompress] of [['gzip', 'gunzip'], ['deflate', 'inflate'],
        ['deflateRaw', 'inflateRaw']]) {
      const encoded = zlib[compress + 'Sync'](input, {level: 9});
      assert.deepEqual(Buffer.from(zlib[decompress + 'Sync'](encoded)), Buffer.from([0, 255, 128, 65]));
      assert.equal(Buffer.from(native[decompress + 'Sync'](encoded)).toString('hex'), '00ff8041');
    }
    assert.equal(zlib.unzipSync(zlib.gzipSync('中文')).toString(), '中文');
    assert.equal(zlib.unzipSync(zlib.deflateSync('zlib')).toString(), 'zlib');
    assert.equal(zlib.codes[-3], 'Z_DATA_ERROR');
  });

  test(`${label} zlib callbacks are asynchronous and snapshot the input`, async () => {
    const {context, zlib} = create();
    const input = vm.runInContext('new Uint8Array([0, 255, 128, 65])', context);
    let returned = false;
    const pending = new Promise((resolve, reject) => zlib.gzip(input, (error, packed) => {
      if (error) return reject(error);
      assert.equal(returned, true);
      resolve(packed);
    }));
    input.fill(9);
    returned = true;
    assert.equal(zlib.gunzipSync(await pending).toString('hex'), '00ff8041');
  });

  test(`${label} zlib corruption and unsupported operations do not report success`, async () => {
    const {zlib} = create();
    assert.throws(() => zlib.gunzipSync('bad'), {code: 'Z_DATA_ERROR'});
    assert.throws(() => zlib.gzipSync('payload', {maxOutputLength: 2}), {code: 'ERR_BUFFER_TOO_LARGE'});
    if (label === 'main') {
      assert.throws(() => zlib.createGzip(), {code: 'ERR_NOT_SUPPORTED'});
      assert.throws(() => new zlib.Gunzip(), {code: 'ERR_NOT_SUPPORTED'});
    }
    assert.throws(() => zlib.gzipSync('x', {dictionary: 1}), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => zlib.gzipSync('x', {level: 10}), {code: 'ERR_OUT_OF_RANGE'});
    assert.throws(() => zlib.gzip('x'), {code: 'ERR_INVALID_ARG_TYPE'});
    const failure = await new Promise(resolve => zlib.gunzip('bad', error => resolve(error)));
    assert.equal(failure.code, 'Z_DATA_ERROR');
  });
}

test('renderer zlib aliases preserve module identity', () => {
  const {context, zlib} = renderer();
  assert.equal(context.require('node:zlib'), zlib);
});

function streamFixture() {
  // The JS pump is tested against Node's real incremental codec handles. Native
  // C++ tests independently cover Xenon's engine and its resource boundaries.
  const handles = new Map();
  let nextId = 0, calls = 0, closes = 0;
  function streamCall(request, asynchronous) {
    if (request.action === 'create') {
      const name = 'create' + request.operation[0].toUpperCase() + request.operation.slice(1);
      const handle = native[name](request.options);
      handle.on('error', () => {});
      handles.set(++nextId, handle);
      return nextId;
    }
    if (request.action === 'close') {
      const handle = handles.get(request.id);
      if (handle) { handles.delete(request.id); ++closes; handle.close(); }
      return !!handle;
    }
    assert.equal(asynchronous, true);
    assert.ok(request.data.length <= 65536);
    assert.ok(request.outputSize <= 65536);
    ++calls;
    return new Promise((resolve, reject) => setImmediate(() => {
      const handle = handles.get(request.id);
      if (!handle) return reject(Object.assign(new Error('closed'), {code: 'ERR_STREAM_DESTROYED'}));
      const output = Buffer.alloc(request.outputSize);
      let failure;
      handle._handle.onerror = (message, errno, code) => { failure = Object.assign(new Error(message), {code}); };
      handle._handle.writeSync(request.flush, request.data, 0, request.data.length,
        output, 0, output.length);
      if (failure) return reject(failure);
      const [remainingOutput, remainingInput] = handle._writeState;
      resolve({consumed: request.data.length - remainingInput,
        output: output.subarray(0, output.length - remainingOutput),
        done: remainingInput === 0 && remainingOutput > 0});
    }));
  }
  const context = vm.createContext({Buffer, queueMicrotask, FinalizationRegistry,
    dependencies: {Transform: require('node:stream').Transform, streamCall},
    call(operation, input, options, asynchronous) {
      if (!asynchronous) return native[operation + 'Sync'](input, options);
      return new Promise((resolve, reject) => native[operation](input, options,
        (error, output) => error ? reject(error) : resolve(output)));
    }});
  return {zlib: vm.runInContext(readBootstrapPart('common/zlib.js') +
    '\ncreateZlibModule(call, dependencies)', context),
    streamCall, get calls() { return calls; }, get closes() { return closes; }, handles};
}
const nextTurn = () => new Promise(resolve => setImmediate(resolve));
const collect = async stream => {
  const chunks = [];
  for await (const chunk of stream) chunks.push(chunk);
  return Buffer.concat(chunks);
};

test('all compression streams emit real bounded chunks and interoperate with Node', async () => {
  const fixture = streamFixture();
  for (const [compress, decompress] of [['Gzip', 'gunzip'], ['Deflate', 'inflate'],
      ['DeflateRaw', 'inflateRaw'], ['BrotliCompress', 'brotliDecompress'],
      ['ZstdCompress', 'zstdDecompress']]) {
    const input = Buffer.alloc(190000);
    for (let index = 0; index < input.length; ++index) input[index] = index % 251;
    const encoder = new fixture.zlib[compress]({chunkSize: 127});
    assert.ok(encoder instanceof fixture.zlib[compress]);
    const encoded = collect(encoder);
    encoder.write(input.subarray(0, 777));
    encoder.write(input.subarray(777, 90000));
    encoder.end(input.subarray(90000));
    const packed = await encoded;
    assert.deepEqual(native[decompress + 'Sync'](packed), input);
    const name = 'create' + decompress[0].toUpperCase() + decompress.slice(1);
    const decoder = fixture.zlib[name]({chunkSize: 1000});
    const decoded = collect(decoder);
    for (let index = 0; index < packed.length; index += 3) decoder.write(packed.subarray(index, index + 3));
    decoder.end();
    assert.deepEqual(await decoded, input);
    assert.equal(encoder.bytesWritten, input.length);
  }
  assert.equal(fixture.handles.size, 0);
  assert.equal(fixture.closes, 10);
});

test('readable backpressure stops native decompression until data is consumed', async () => {
  const fixture = streamFixture();
  const input = Buffer.alloc(2 * 1024 * 1024, 71);
  const decoder = fixture.zlib.createGunzip({chunkSize: 1024, highWaterMark: 1024});
  let writeDone = false;
  decoder.end(native.gzipSync(input), () => { writeDone = true; });
  await nextTurn(); await nextTurn();
  const pausedCalls = fixture.calls;
  assert.ok(pausedCalls <= 2);
  assert.equal(writeDone, false);
  await nextTurn(); await nextTurn();
  assert.equal(fixture.calls, pausedCalls);
  assert.ok(decoder.readableLength <= 1024);
  assert.deepEqual(await collect(decoder), input);
  assert.equal(writeDone, true);
  assert.equal(fixture.handles.size, 0);
});

test('flush preserves write ordering and emits output before end', async () => {
  for (const name of ['Gzip', 'BrotliCompress', 'ZstdCompress']) {
    const fixture = streamFixture();
    const stream = fixture.zlib['create' + name]({chunkSize: 64});
    const chunks = [];
    stream.on('data', chunk => chunks.push(chunk));
    stream.write('first');
    await new Promise((resolve, reject) => stream.flush(error => error ? reject(error) : resolve()));
    assert.ok(Buffer.concat(chunks).length > 0);
    assert.equal(stream.readableEnded, false);
    const closed = new Promise(resolve => stream.once('close', resolve));
    stream.end('second');
    await closed;
    const decompress = {Gzip: 'gunzipSync', BrotliCompress: 'brotliDecompressSync', ZstdCompress: 'zstdDecompressSync'}[name];
    assert.equal(native[decompress](Buffer.concat(chunks)).toString(), 'firstsecond');
  }
});

test('malformed data and destroy during native work settle once and release handles', async () => {
  const fixture = streamFixture();
  const bad = fixture.zlib.createGunzip();
  const failed = collect(bad);
  bad.end('bad');
  await assert.rejects(failed, {code: 'Z_DATA_ERROR'});
  const stream = fixture.zlib.createGzip();
  let callbacks = 0, closed = 0;
  stream.on('error', () => {});
  stream.on('close', () => ++closed);
  stream.write(Buffer.alloc(200000), error => { ++callbacks; assert.equal(error.code, 'ERR_STREAM_DESTROYED'); });
  stream.destroy();
  await nextTurn(); await nextTurn();
  assert.equal(callbacks, 1);
  assert.equal(closed, 1);
  assert.equal(fixture.handles.size, 0);
});

test('Brotli and Zstd convenience functions and preset dictionaries round trip', async () => {
  const {zlib} = streamFixture();
  for (const [compress, decompress] of [['brotliCompress', 'brotliDecompress'], ['zstdCompress', 'zstdDecompress']]) {
    const packed = zlib[compress + 'Sync']('payload');
    assert.equal(zlib[decompress + 'Sync'](packed).toString(), 'payload');
    const asyncPacked = await new Promise((resolve, reject) => zlib[compress]('asynchronous',
      (error, output) => error ? reject(error) : resolve(output)));
    assert.equal(zlib[decompress + 'Sync'](asyncPacked).toString(), 'asynchronous');
  }
  const options = {dictionary: Buffer.from('shared dictionary')};
  const packed = zlib.deflateSync('shared dictionary payload', options);
  assert.equal(zlib.inflateSync(packed, options).toString(), 'shared dictionary payload');
});

test('renderer compression uses real Transform streams and bounded binary transport', {timeout: 10000}, async () => {
  const fixture = renderer();
  const input = Buffer.alloc(150000, 89);
  for (const [compress, decompress] of [['Gzip', 'gunzipSync'],
      ['BrotliCompress', 'brotliDecompressSync'], ['ZstdCompress', 'zstdDecompressSync']]) {
    const stream = fixture.zlib['create' + compress]({chunkSize: 128});
    const pending = collect(stream);
    stream.write(input.subarray(0, 70000));
    await new Promise((resolve, reject) => stream.flush(error => error ? reject(error) : resolve()));
    stream.end(input.subarray(70000));
    assert.deepEqual(native[decompress](await pending), input);
  }
  const dictionary = Buffer.from('shared');
  const packed = fixture.zlib.deflateSync('shared payload', {dictionary});
  assert.equal(fixture.zlib.inflateSync(packed, {dictionary}).toString(), 'shared payload');
  const decoder = fixture.zlib.createGunzip({chunkSize: 512, highWaterMark: 512});
  const before = fixture.streams.calls;
  decoder.end(fixture.context.require('buffer').Buffer.from(native.gzipSync(input)));
  await nextTurn(); await nextTurn();
  const paused = fixture.streams.calls;
  assert.ok(paused - before <= 2);
  await nextTurn();
  assert.equal(fixture.streams.calls, paused);
  assert.deepEqual(await collect(decoder), input);
  assert.equal(fixture.streams.handles.size, 0);
});
