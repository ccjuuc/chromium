// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const {once} = require('node:events');
const nodeStreams = require('node:stream');
const {finished} = require('node:stream/promises');
const nativeWeb = require('node:stream/web');
const ponyfill = require('./third_party/web_streams_polyfill/dist/ponyfill.js');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const create = vm.runInThisContext(fs.readFileSync(path.join(__dirname,
    'common/stream_web_adapters.js'), 'utf8') + ';createStreamWebAdapters');
const tick = () => new Promise(resolve => setImmediate(resolve));
const adaptersFor = webStreams => create({...nodeStreams, Buffer, webStreams});
const adapters = adaptersFor(ponyfill);
const collect = async source => { const out = []; for await (const chunk of source) out.push(chunk); return out; };

test('fromWeb reads only bounded demand, preserves bytes and holds the conversion lock', async () => {
  let pulls = 0;
  const source = new ponyfill.ReadableStream({pull(c) {
    ++pulls;
    if (pulls <= 20) c.enqueue(new Uint8Array([pulls]));
    else c.close();
  }}, {highWaterMark: 0});
  const converted = adapters.Readable.fromWeb(source, {highWaterMark: 2});
  await tick();
  assert.equal(pulls, 0);
  converted.read(0);
  await tick();
  assert.equal(pulls, 2);
  assert.equal(converted.readableLength, 2);
  const chunks = await collect(converted);
  assert.deepEqual([...Buffer.concat(chunks)], Array.from({length: 20}, (_, i) => i + 1));
  assert.equal(source.locked, true);
});

test('Readable.fromWeb objectMode, encoding and source errors follow Node', async () => {
  for (const [web, convert] of [[ponyfill, adapters.Readable.fromWeb], [nativeWeb, nodeStreams.Readable.fromWeb]]) {
    const objects = [{n:1}, {n:2}];
    assert.deepEqual(await collect(convert(web.ReadableStream.from(objects), {objectMode: true})), objects);
    const text = new web.ReadableStream({start(c) {
      c.enqueue(new Uint8Array([0xe4])); c.enqueue(new Uint8Array([0xb8, 0xad])); c.close();
    }});
    assert.equal((await collect(convert(text, {encoding: 'utf8'}))).join(''), '中');
    const failure = new Error('reader failure');
    const broken = new web.ReadableStream({pull() { throw failure; }});
    await assert.rejects(collect(convert(broken)), e => e === failure);
  }
});

test('Readable.fromWeb destroy cancels the real source exactly once with its reason', async () => {
  let reason, cancellations = 0;
  const source = new ponyfill.ReadableStream({cancel(value) { reason = value; ++cancellations; }},
      {highWaterMark: 0});
  const converted = adapters.Readable.fromWeb(source);
  const failure = new Error('destroy source');
  const completion = assert.rejects(finished(converted), e => e === failure);
  converted.destroy(failure);
  await completion;
  assert.equal(reason, failure);
  assert.equal(cancellations, 1);
});

test('fromWeb validates before locking and rolls back duplex lock acquisition', () => {
  const source = new ponyfill.ReadableStream();
  assert.throws(() => adapters.Readable.fromWeb(source, {highWaterMark: -1}), {code: 'ERR_INVALID_ARG_VALUE'});
  assert.equal(source.locked, false);
  assert.throws(() => adapters.Readable.fromWeb({getReader(){}}), {code: 'ERR_INVALID_ARG_TYPE'});
  const sink = new ponyfill.WritableStream();
  const owner = sink.getWriter();
  assert.throws(() => adapters.Duplex.fromWeb({readable: source, writable: sink}), {name: 'TypeError'});
  assert.equal(source.locked, false);
  owner.releaseLock();
});

test('Writable.fromWeb waits for real write completion and propagates Node drain', async () => {
  let release, writes = 0, callback = false, closed = false;
  const sink = new ponyfill.WritableStream({write(chunk) {
    ++writes;
    assert.equal(Buffer.from(chunk).toString(), 'first');
    return new Promise(resolve => { release = resolve; });
  }, close() { closed = true; }});
  const converted = adapters.Writable.fromWeb(sink, {highWaterMark: 1});
  assert.equal(converted.write('first', () => { callback = true; }), false);
  const drained = once(converted, 'drain');
  await tick();
  assert.equal(writes, 1);
  assert.equal(callback, false);
  release();
  await drained;
  assert.equal(callback, true);
  converted.end();
  await finished(converted);
  assert.equal(closed, true);
  assert.equal(sink.locked, true);
});

test('Writable.fromWeb options preserve objects and undecoded strings', async () => {
  const values = [];
  const a = adapters.Writable.fromWeb(new ponyfill.WritableStream({write(v) { values.push(v); }}),
      {objectMode: true});
  const object = {value: 3};
  a.end(object); await finished(a);
  const b = adapters.Writable.fromWeb(new ponyfill.WritableStream({write(v) { values.push(v); }}),
      {decodeStrings: false});
  b.end('文字'); await finished(b);
  assert.deepEqual(values, [object, '文字']);
});

test('Writable.fromWeb aborts the underlying sink and source errors reach Node', async () => {
  let reason;
  const failure = new Error('sink stop');
  const sink = new ponyfill.WritableStream({abort(value) { reason = value; }});
  const converted = adapters.Writable.fromWeb(sink);
  const done = assert.rejects(finished(converted), e => e === failure);
  converted.destroy(failure); await done;
  assert.equal(reason, failure);
  let controller;
  const broken = adapters.Writable.fromWeb(new ponyfill.WritableStream({start(c) { controller = c; }}));
  const rejected = assert.rejects(finished(broken), e => e === failure);
  controller.error(failure);
  await rejected;
});

test('Readable.toWeb pauses a Node producer at the byte budget and copies Buffer ownership', async () => {
  let produced = 0;
  const original = Buffer.alloc(8, 7);
  const source = new nodeStreams.Readable({highWaterMark: 8, read() {
    ++produced;
    if (produced <= 5) this.push(original);
    else this.push(null);
  }});
  const web = adapters.Readable.toWeb(source);
  await tick();
  assert.ok(produced <= 2, `read-ahead exceeded two queues: ${produced}`);
  const reader = web.getReader();
  const first = await reader.read();
  assert.equal(Buffer.isBuffer(first.value), false);
  first.value.fill(0);
  assert.deepEqual([...original], Array(8).fill(7));
  let count = 1;
  while (!(await reader.read()).done) ++count;
  assert.equal(count, 5);
});

test('Readable.toWeb byte mode supports BYOB without detaching Node buffers and ends empty reads', async () => {
  const original = Buffer.from([4, 8, 15, 16, 23, 42]);
  const source = nodeStreams.Readable.from([original], {objectMode: false});
  const reader = adapters.Readable.toWeb(source, {type: 'bytes'}).getReader({mode: 'byob'});
  const out = [];
  for (;;) {
    const result = await reader.read(new Uint8Array(4));
    if (result.done) break;
    out.push(...result.value);
  }
  assert.deepEqual(out, [...original]);
  assert.equal(original.byteLength, 6);
  const empty = adapters.Readable.toWeb(nodeStreams.Readable.from([], {objectMode: false}),
      {type: 'bytes'}).getReader({mode: 'byob'});
  assert.equal((await empty.read(new Uint8Array(4))).done, true);
});

test('Readable.toWeb cancellation destroys producer and errors or premature close reject readers', async () => {
  let reason;
  const source = new nodeStreams.Readable({read() {}, destroy(value, done) { reason = value; done(); }});
  const failure = new Error('cancel Node producer');
  await adapters.Readable.toWeb(source).cancel(failure);
  assert.equal(source.destroyed, true);
  assert.equal(reason, failure);
  for (const supplied of [failure, undefined]) {
    const node = new nodeStreams.Readable({read() {}});
    const reader = adapters.Readable.toWeb(node).getReader();
    const check = e => supplied ? e === failure : e.code === 'ERR_STREAM_PREMATURE_CLOSE';
    const rejected = [assert.rejects(reader.read(), check), assert.rejects(reader.closed, check)];
    node.destroy(supplied);
    await Promise.all(rejected);
  }
});

test('Writable.toWeb waits for Node drain, serializes writes and waits for final flush', async () => {
  let release, finalize, count = 0;
  const node = new nodeStreams.Writable({highWaterMark: 1,
    write(chunk, encoding, done) { ++count; release = done; },
    final(done) { finalize = done; }});
  const writer = adapters.Writable.toWeb(node).getWriter();
  let firstDone = false;
  const first = writer.write(new Uint8Array([1])).then(() => { firstDone = true; });
  await tick();
  assert.equal(count, 1); assert.equal(firstDone, false);
  release(); await first;
  const second = writer.write(new Uint8Array([2]));
  await tick(); assert.equal(count, 2); release(); await second;
  let finished = false;
  const closing = writer.close().then(() => { finished = true; });
  await tick(); assert.equal(finished, false);
  finalize(); await closing; await writer.closed;
  assert.equal(node.writableFinished, true);
});

test('Writable.toWeb propagates errors during drain and close, and abort destroys the sink', async () => {
  const failure = new Error('Node sink failure');
  for (const duringFinal of [false, true]) {
    const sink = new nodeStreams.Writable({highWaterMark: 1, write(c, e, done) { if (duringFinal) done(); },
      final(done) {}});
    const writer = adapters.Writable.toWeb(sink).getWriter();
    const pending = duringFinal ? writer.close() : writer.write('a');
    const rejected = [assert.rejects(pending, e => e === failure), assert.rejects(writer.closed, e => e === failure)];
    await tick(); sink.destroy(failure); await Promise.all(rejected);
  }
  let reason;
  const sink = new nodeStreams.Writable({write(c, e, d) { d(); }, destroy(e, d) { reason = e; d(); }});
  await adapters.Writable.toWeb(sink).abort(failure);
  assert.equal(sink.destroyed, true); assert.equal(reason, failure);
});

test('Duplex conversions transfer both sides, preserve half-close and cancel both underlying halves', async () => {
  const transform = new ponyfill.TransformStream({transform(value, c) { c.enqueue(value * 2); }});
  const duplex = adapters.Duplex.fromWeb(transform, {objectMode: true});
  const output = collect(duplex);
  duplex.write(2); duplex.end(5);
  assert.deepEqual(await output, [4, 10]);
  await finished(duplex);
  const node = new nodeStreams.Transform({objectMode: true, transform(v, e, d) { d(null, v + 1); }});
  const pair = adapters.Duplex.toWeb(node);
  const read = collect(pair.readable);
  const writer = pair.writable.getWriter();
  await writer.write(8); await writer.close();
  assert.deepEqual(await read, [9]);
  const reasons = [];
  const broken = adapters.Duplex.fromWeb({
    readable: new ponyfill.ReadableStream({cancel(e) { reasons.push(e); }}),
    writable: new ponyfill.WritableStream({abort(e) { reasons.push(e); }}),
  });
  const failure = new Error('pair stop');
  const done = assert.rejects(finished(broken), e => e === failure);
  broken.destroy(failure); await done;
  assert.deepEqual(reasons, [failure, failure]);
});

test('adapters work with native Web brands without installing or replacing constructors', async () => {
  const bridge = adaptersFor(nativeWeb);
  const source = nativeWeb.ReadableStream.from([Buffer.from('native')]);
  const node = bridge.Readable.fromWeb(source);
  assert.equal(Buffer.concat(await collect(node)).toString(), 'native');
  const pair = bridge.Duplex.toWeb(new nodeStreams.PassThrough());
  assert.ok(pair.readable instanceof nativeWeb.ReadableStream);
  assert.ok(pair.writable instanceof nativeWeb.WritableStream);
  const output = collect(pair.readable);
  const writer = pair.writable.getWriter();
  await writer.write(Buffer.from('roundtrip')); await writer.close();
  assert.equal(Buffer.concat(await output).toString(), 'roundtrip');
});

function bootstrap(kind, worker) {
  const context = vm.createContext({TextEncoder,TextDecoder,URL,URLSearchParams,queueMicrotask,
    setTimeout,clearTimeout,setInterval,clearInterval,atob,btoa,console,performance,
    ...(kind === 'renderer' ? {...nativeWeb,Blob,File} : {}),
    __xenonPaths:{platform:'win32',arch:'x64',endianness:'LE',pid:1},
    location:{protocol:'file:',pathname:'/C:/fixture/index.html'},
    xenonIpcRenderer:{getRuntimeConfig:()=>({appPath:'C:\\fixture',exeDir:'C:\\fixture',execPath:'C:\\fixture\\host.exe'}),setDispatchHandler(){}},
    __xenonPlatform:'win32',__xenonArch:'x64',__xenonEndianness:'LE',__xenonOsRelease:'',
    __xenonAppPath:'C:\\fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',__xenonExecPath:'C:\\fixture\\host.exe',
    __xenonPid:1,__xenonEnv:{},__xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
    __xenonWorkerThreadId:worker?9:0,__xenonWorkerResourceLimits:{},__xenonWorkerData:null,
  });
  vm.runInContext(readBootstrap(path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`)), context);
  return context;
}

for (const [kind, worker] of [['main',false], ['main',true], ['renderer',false]]) {
  test(`${kind}${worker ? ' Worker' : ''}: actual bundled Node queues bridge both Web directions`, async () => {
    const context = bootstrap(kind, worker);
    const node = kind === 'main' ? context.__xenonStream : context.require('node:stream');
    const {Buffer: RuntimeBuffer} = kind === 'main' ? context.__xenonBuffer : context.require('buffer');
    const web = kind === 'main' ? context.__xenonStreamWeb : context.require('stream/web');
    let pulls = 0;
    const source = new web.ReadableStream({pull(c) {
      ++pulls;
      if (pulls <= 4) c.enqueue(RuntimeBuffer.from([pulls]));
      else c.close();
    }}, {highWaterMark: 0});
    const converted = node.Readable.fromWeb(source, {highWaterMark: 1});
    assert.equal(converted instanceof node.Readable, true);
    converted.read(0); await tick();
    assert.equal(pulls, 1);
    const chunks = await collect(converted);
    assert.deepEqual([...RuntimeBuffer.concat(chunks)], [1,2,3,4]);
    const transform = new node.Transform({objectMode: true, transform(value, encoding, done) {
      done(null, value * 3);
    }});
    const pair = node.Duplex.toWeb(transform);
    const result = collect(pair.readable);
    const writer = pair.writable.getWriter();
    await writer.write(4); await writer.write(7); await writer.close();
    assert.deepEqual(await result, [12,21]);
    const reasons = [];
    const sink = node.Writable.fromWeb(new web.WritableStream({abort(reason) { reasons.push(reason); }}));
    const failure = new Error('integrated cancellation');
    const destroyed = new Promise(resolve => sink.once('close', resolve));
    sink.on('error', () => {}); sink.destroy(failure); await destroyed;
    assert.deepEqual(reasons, [failure]);
    if (kind === 'main') {
      let controller;
      const tracked = new web.ReadableStream({start(c) { controller = c; }});
      assert.equal(node.isDisturbed(tracked), false);
      const reader = tracked.getReader();
      assert.equal(node.isDisturbed(tracked), false);
      const pending = assert.rejects(reader.read(), e => e === failure);
      const closed = assert.rejects(reader.closed, e => e === failure);
      assert.equal(node.isDisturbed(tracked), true);
      controller.error(failure);
      assert.equal(node.isErrored(tracked), true);
      assert.equal(node.isReadable(tracked), false);
      await Promise.all([pending, closed]);
    }
  });
}
