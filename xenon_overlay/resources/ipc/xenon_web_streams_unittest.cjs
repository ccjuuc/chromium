// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const native = require('node:stream/web');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const source = fs.readFileSync(path.join(__dirname,
    'third_party/web_streams_polyfill/dist/ponyfill.js'), 'utf8');
const wrapper = fs.readFileSync(path.join(__dirname, 'common/web_streams.js'), 'utf8');
const create = vm.runInThisContext(wrapper + ';createWebStreamsModule');
const ponyfill = vm.runInThisContext(`(() => { const module = {exports:{}};
    const exports = module.exports; ${source}; return module.exports; })()`);
const streams = create({}, () => ponyfill);
const tick = () => new Promise(resolve => setImmediate(resolve));

test('native constructors are reused as one family and fallback globals share module identity', () => {
  const host = {...native};
  const module = create(host, () => { throw new Error('Native streams must not load fallback'); });
  for (const name of Object.keys(module)) assert.equal(module[name], native[name]);
  const fallback = {};
  const result = create(fallback, () => ponyfill);
  for (const name of Object.keys(result)) assert.equal(result[name], fallback[name]);
  assert.equal(result.CompressionStream, undefined);
  assert.equal(result.TextDecoderStream, undefined);
  const withoutTransfer = vm.runInNewContext(wrapper + ';createWebStreamsModule',
      {ArrayBuffer: {prototype: {}}});
  assert.throws(() => withoutTransfer({}, () => ponyfill), {code:'ERR_NOT_SUPPORTED'});
});

test('byte enqueue transfers buffers and BYOB transfers the supplied view like Node', async () => {
  async function observe(s) {
    let controller;
    const stream = new s.ReadableStream({type:'bytes', start(c) { controller = c; }});
    const reader = stream.getReader({mode:'byob'});
    const target = new Uint8Array(8);
    const pending = reader.read(target);
    const input = new Uint8Array([2, 4, 6]);
    controller.enqueue(input);
    controller.close();
    const first = await pending;
    const done = await reader.read(new Uint8Array(2));
    await reader.closed;
    return {inputDetached:input.buffer.byteLength,targetDetached:target.buffer.byteLength,
      bytes:Array.from(first.value),done:first.done,final:done.done,finalBytes:done.value.byteLength};
  }
  assert.deepEqual(await observe(streams), await observe(native));
});

test('private stream state reports actual read/error transitions rather than reader locks', async () => {
  let getState;
  const module = create({}, () => ponyfill, read => { getState = read; });
  const node = require('node:stream');
  let controller, nativeController;
  const readable = new module.ReadableStream({start(c) { controller = c; }});
  const reference = new native.ReadableStream({start(c) { nativeController = c; }});
  const compare = () => {
    const state = getState(readable);
    assert.equal(state.disturbed, node.isDisturbed(reference));
    assert.equal(state.errored, node.isErrored(reference));
    assert.equal(state.readable, node.isReadable(reference));
  };
  compare();
  const reader = readable.getReader(), nativeReader = reference.getReader();
  compare();
  assert.equal(getState(readable).disturbed, false);
  const failure = new Error('state failure');
  const checks = [reader, nativeReader].flatMap(r => [assert.rejects(r.read(), e => e === failure),
    assert.rejects(r.closed, e => e === failure)]);
  compare();
  assert.equal(getState(readable).disturbed, true);
  controller.error(failure); nativeController.error(failure);
  compare();
  await Promise.all(checks);
  const canceled = new module.ReadableStream();
  await canceled.cancel();
  assert.equal(getState(canceled).disturbed, true);
  assert.equal(getState(canceled).readable, false);
  assert.equal(getState({_disturbed: true, _state: 'errored'}), undefined);
  assert.equal(module.getState, undefined);
});

test('private writable state observes erroring versus errored and native state stays explicit', async () => {
  let getState, controller, nativeController;
  const module = create({}, () => ponyfill, read => { getState = read; });
  const node = require('node:stream');
  const writable = new module.WritableStream({start(c) { controller = c; }});
  const reference = new native.WritableStream({start(c) { nativeController = c; }});
  controller.error('failure'); nativeController.error('failure');
  assert.equal(getState(writable).errored, node.isErrored(reference));
  await tick();
  assert.equal(getState(writable).errored, node.isErrored(reference));
  assert.equal(getState(writable).errored, true);
  let nativeState;
  create({...native}, () => { throw Error('fallback'); }, read => { nativeState = read; });
  assert.throws(() => nativeState(reference), {code:'ERR_NOT_SUPPORTED'});
  assert.equal(nativeState({}), undefined);
  const expected = {kind: 'writable', errored: true};
  create({...native, __xenonGetWebStreamState(value) {
    assert.equal(value, reference); return expected;
  }}, () => {}, read => { nativeState = read; });
  assert.equal(nativeState(reference), expected);
});

test('BYOB request/respond and locking use actual controller state', async () => {
  async function observe(s) {
    let calls = 0;
    const stream = new s.ReadableStream({type:'bytes', pull(controller) {
      ++calls;
      const request = controller.byobRequest;
      request.view.set([7, 9]);
      request.respond(2);
      controller.close();
    }});
    const reader = stream.getReader({mode:'byob'});
    assert.throws(() => stream.getReader(), {name:'TypeError'});
    const input = new Uint16Array(4);
    const result = await reader.read(input);
    const bytes = Array.from(new Uint8Array(result.value.buffer, result.value.byteOffset, result.value.byteLength));
    await reader.closed;
    reader.releaseLock();
    return {calls,bytes,detached:input.byteLength,locked:stream.locked};
  }
  assert.deepEqual(await observe(streams), await observe(native));
});

test('byte tee gives independent branch bytes and forwards both cancellation reasons', async () => {
  async function observe(s) {
    let reason;
    const original = new s.ReadableStream({type:'bytes',start(c){c.enqueue(new Uint8Array([1,2,3]));},
      cancel(value){reason=value;}});
    const [a,b] = original.tee();
    const ra=a.getReader(),rb=b.getReader();
    const [av,bv]=await Promise.all([ra.read(),rb.read()]);
    av.value[0]=99;
    await Promise.all([ra.cancel('first'),rb.cancel('second')]);
    return {right:Array.from(bv.value),reason,locked:original.locked};
  }
  assert.deepEqual(await observe(streams), await observe(native));
});

test('Writable backpressure keeps ready pending until the underlying write completes', async () => {
  let release,started=false,ready=false;
  const stream=new streams.WritableStream({write(){started=true;return new Promise(r=>{release=r;});}},
      new streams.CountQueuingStrategy({highWaterMark:1}));
  const writer=stream.getWriter();
  await writer.ready;
  const pending=writer.write('one');
  writer.ready.then(()=>{ready=true;});
  assert.equal(writer.desiredSize,0);
  await tick();
  assert.equal(started,true); assert.equal(ready,false);
  release(); await pending; await writer.ready;
  assert.equal(ready,true); assert.equal(writer.desiredSize,1);
  await writer.close(); await writer.closed;
});

test('pipeTo applies bounded pull demand and cancellation aborts the actual source and sink', async () => {
  let pulls=0,release,writeStarted;
  const started=new Promise(r=>{writeStarted=r;});
  const abort=new AbortController(),failure=new Error('stop');
  const reasons=[];
  const readable=new streams.ReadableStream({pull(c){++pulls;c.enqueue(pulls);},cancel(reason){reasons.push(['source',reason]);}},
      {highWaterMark:1});
  const writable=new streams.WritableStream({write(){writeStarted();return new Promise(r=>{release=r;});},
    abort(reason){reasons.push(['sink',reason]);}});
  const pipe=readable.pipeTo(writable,{signal:abort.signal});
  const rejected=assert.rejects(pipe,e=>e===failure);
  await started; await tick();
  assert.ok(pulls<=2,`Unbounded pulls while sink blocked: ${pulls}`);
  abort.abort(failure); release(); await rejected;
  assert.equal(reasons.length,2);
  assert.ok(reasons.every(([,reason])=>reason===failure));
  assert.equal(readable.locked,false); assert.equal(writable.locked,false);
});

test('transform flush produces real bytes and transform errors reject both sides', async () => {
  const transform=new streams.TransformStream({transform(v,c){c.enqueue(v*2);},flush(c){c.enqueue(99);}});
  const output=[];
  const consume=transform.readable.pipeTo(new streams.WritableStream({write(v){output.push(v);}}));
  const writer=transform.writable.getWriter();
  await writer.write(3); await writer.close(); await consume;
  assert.deepEqual(output,[6,99]);
  const failure=new Error('transform failed');
  const broken=new streams.TransformStream({transform(){throw failure;}});
  const r=broken.readable.getReader(),w=broken.writable.getWriter();
  const checks=[assert.rejects(r.read(),e=>e===failure),assert.rejects(r.closed,e=>e===failure),
    assert.rejects(w.closed,e=>e===failure),assert.rejects(w.write(1),e=>e===failure)];
  await Promise.all(checks);
});

test('Writable controller signal aborts with the original reason', async () => {
  let signal,received;
  const stream=new streams.WritableStream({start(c){signal=c.signal;},abort(reason){received=reason;}});
  const reason=new Error('abort');
  const writer=stream.getWriter();
  const closed=assert.rejects(writer.closed,e=>e===reason);
  await writer.abort(reason); await closed;
  assert.equal(signal.aborted,true); assert.equal(signal.reason,reason); assert.equal(received,reason);
});

test('ReadableStream.from propagates iterator cancellation and source failures', async () => {
  let returned;
  const iterable={[Symbol.asyncIterator](){return this;},async next(){return{value:4,done:false};},
    async return(reason){returned=reason;return{done:true};}};
  const reader=streams.ReadableStream.from(iterable).getReader();
  assert.deepEqual(await reader.read(),{value:4,done:false});
  await reader.cancel('stop'); assert.equal(returned,'stop');
  const reason=new Error('iterator');
  const broken=streams.ReadableStream.from({*[Symbol.iterator](){throw reason;}}).getReader();
  await Promise.all([assert.rejects(broken.read(),e=>e===reason),assert.rejects(broken.closed,e=>e===reason)]);
});

test('Blob streams preserve immutable backing with BYOB reads, slicing and cancellation', async () => {
  const createBlob=vm.runInThisContext(fs.readFileSync(path.join(__dirname,'common/blob.js'),'utf8')+';createBlobConstructor');
  const Blob=createBlob({platform:'linux',ReadableStream:streams.ReadableStream});
  const input=new Uint8Array(150000);for(let i=0;i<input.length;++i)input[i]=i%251;
  const blob=new Blob([input]),slice=blob.slice(100,100000);
  const reader=slice.stream().getReader({mode:'byob'});
  const bytes=[];
  for(;;){const {value,done}=await reader.read(new Uint8Array(8192));if(done)break;
    bytes.push(...value);value.fill(255);}
  assert.deepEqual(bytes,Array.from(input.subarray(100,100000)));
  assert.deepEqual(Array.from(await blob.bytes()),Array.from(input));
  const stream=blob.stream(),r=stream.getReader();
  const first=await r.read();assert.equal(first.value.length,65536);
  await r.cancel('unused');assert.equal((await r.read()).done,true);
  assert.equal((await blob.bytes()).length,150000);
  const empty=new Blob().stream().getReader({mode:'byob'});
  assert.equal((await empty.read(new Uint8Array(4))).done,true);
});

function bootstrap(kind,worker=false,transform=source=>source) {
  const context=vm.createContext({TextEncoder,TextDecoder,URL,URLSearchParams,queueMicrotask,
    setTimeout,clearTimeout,setInterval,clearInterval,atob,btoa,console,performance,
    ...(kind==='renderer'?{...native,Blob,File}:{}),
    __xenonPaths:{platform:'win32',arch:'x64',endianness:'LE',pid:1},
    location:{protocol:'file:',pathname:'/C:/fixture/index.html'},
    xenonIpcRenderer:{getRuntimeConfig:()=>({appPath:'C:\\fixture',exeDir:'C:\\fixture',execPath:'C:\\fixture\\host.exe'}),setDispatchHandler(){}},
    __xenonPlatform:'win32',__xenonArch:'x64',__xenonEndianness:'LE',__xenonOsRelease:'',
    __xenonAppPath:'C:\\fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',__xenonExecPath:'C:\\fixture\\host.exe',
    __xenonPid:1,__xenonEnv:{},__xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
    __xenonWorkerThreadId:worker?9:0,__xenonWorkerResourceLimits:{},__xenonWorkerData:null,
  });
  vm.runInContext(transform(readBootstrap(path.join(__dirname,`xenon_ipc_${kind}_bootstrap.js`))),context);
  return context;
}
for(const [kind,worker]of [['main',false],['main',true],['renderer',false]]) {
  test(`${kind}${worker?' Worker':''}: actual bootstrap exposes the same constructors and working byte streams`,async()=>{
    const c=bootstrap(kind,worker);
    const module=kind==='main'?c.__xenonStreamWeb:c.require('node:stream/web');
    assert.equal(module.ReadableStream,c.ReadableStream);
    assert.equal(module.WritableStream,c.WritableStream);
    const buffer=kind==='main'?c.__xenonBuffer:c.require('node:buffer');
    assert.equal(buffer.File,c.File);
    assert.equal(buffer.Blob,c.Blob);
    const file=new buffer.File(['file'], 'example.txt', {lastModified:17});
    assert.equal(file.name,'example.txt');
    assert.equal(file.lastModified,17);
    assert.equal(await file.text(),'file');
    assert.equal(file instanceof buffer.Blob,true);
    assert.equal(file.slice() instanceof buffer.File,false);
    if(kind==='renderer'){
      assert.equal(module.ReadableStream,native.ReadableStream);
      assert.equal(buffer.File,File);
      assert.equal(c.require('stream/web'),module);
      assert.equal(c.require('module').isBuiltin('node:stream/web'),true);
    }else{
      const blob=new c.Blob(['data']);
      assert.equal(blob.stream() instanceof module.ReadableStream,true);
      const r=blob.stream().getReader({mode:'byob'});
      assert.deepEqual(Array.from((await r.read(new Uint8Array(8))).value),[100,97,116,97]);
    }
  });
}


test('unused fallback streams stay lazy and assignments survive another constructor first use', () => {
  let loads = 0, getState;
  const host = {};
  const module = create(host, () => { ++loads; return ponyfill; }, read => { getState = read; });
  assert.equal(loads, 0);
  assert.equal(Object.keys(module).length, 13);
  assert.equal(getState({}), undefined);
  assert.equal(loads, 0);
  const replacement = function Replacement() {};
  host.ReadableStream = replacement;
  module.WritableStream = replacement;
  delete host.TransformStream;
  assert.equal(loads, 0);
  assert.equal(module.ReadableStream, ponyfill.ReadableStream);
  assert.equal(loads, 1);
  assert.equal(host.ReadableStream, replacement);
  assert.equal(module.WritableStream, replacement);
  assert.equal(host.TransformStream, undefined);
  assert.equal(module.TransformStream, ponyfill.TransformStream);
  assert.equal(host.WritableStream, ponyfill.WritableStream);
  assert.equal(loads, 1);
});

for (const kind of ['main', 'renderer']) {
  test(`${kind}: bootstrap defers Web construction while preserving Node stream inheritance and identities`, async () => {
    const c = bootstrap(kind, false, source => source
      .replace("  'use strict';", "  'use strict'; globalThis.streamInitialization = {ponyfill: 0, adapters: 0};")
      .replace('const webStreamsModule = createWebStreamsModule(globalThis, () => {',
          'const webStreamsModule = createWebStreamsModule(globalThis, () => { ++streamInitialization.ponyfill;')
      .replace('function createStreamWebAdapters({Readable, Writable, Duplex, Buffer, webStreams}) {',
          'function createStreamWebAdapters({Readable, Writable, Duplex, Buffer, webStreams}) { ++streamInitialization.adapters;'));
    const stream = kind === 'main' ? c.__xenonStream : c.require('stream');
    const web = kind === 'main' ? c.__xenonStreamWeb : c.require('stream/web');
    const fs = kind === 'main' ? c.__xenonFs : c.require('fs');
    const net = kind === 'main' ? c.__xenonNet : c.require('net');
    const buffer = kind === 'main' ? c.__xenonBuffer : c.require('buffer');
    assert.equal(stream.Stream, stream);
    assert.equal(Object.getPrototypeOf(fs.ReadStream.prototype), stream.Readable.prototype);
    assert.equal(Object.getPrototypeOf(fs.WriteStream.prototype), stream.Writable.prototype);
    assert.equal(Object.getPrototypeOf(net.Socket.prototype), stream.Duplex.prototype);
    assert.equal(buffer.Buffer, c.Buffer);
    assert.equal(await new c.Blob(['unused']).text(), 'unused');
    assert.equal(stream.isReadable(new stream.Readable({read() {}})), true);
    assert.equal(c.streamInitialization.ponyfill, 0);
    assert.equal(c.streamInitialization.adapters, kind === 'main' ? 0 : 1);
    if (kind === 'renderer') {
      assert.equal(c.require('node:stream'), stream);
      assert.equal(c.require('node:stream/promises'), stream.promises);
      assert.equal(c.require('node:buffer').Buffer, buffer.Buffer);
    }
    const ReadableStream = web.ReadableStream;
    assert.equal(ReadableStream, c.ReadableStream);
    assert.equal(c.streamInitialization.ponyfill, kind === 'main' ? 1 : 0);
    const toWeb = stream.Readable.toWeb;
    assert.equal(toWeb, stream.Readable.toWeb);
    assert.equal(toWeb.name, 'readableToWeb');
    assert.equal(toWeb.length, 2);
    assert.equal(c.streamInitialization.adapters, 1);
    const converted = toWeb(stream.Readable.from(['data']));
    assert.equal(converted instanceof ReadableStream, true);
    assert.equal((await converted.getReader().read()).value, 'data');
    assert.equal(c.streamInitialization.ponyfill, kind === 'main' ? 1 : 0);
    assert.equal(c.streamInitialization.adapters, 1);
  });
}

test('main lazy Blob and conversion helpers retain intrinsic constructors after export mutation', () => {
  const c = bootstrap('main');
  const web = c.__xenonStreamWeb, stream = c.__xenonStream;
  const OriginalReadable = stream.Readable;
  const readOriginalWeb = Object.getOwnPropertyDescriptor(web, 'ReadableStream').get;
  const replacement = function Replacement() { throw new Error('Replacement must not be used'); };
  web.ReadableStream = replacement;
  c.ReadableStream = replacement;
  stream.Readable = replacement;
  const result = new c.Blob(['bytes']).stream();
  assert.equal(result instanceof readOriginalWeb(), true);
  const converted = OriginalReadable.fromWeb(result);
  assert.equal(converted instanceof OriginalReadable, true);
  converted.destroy();
});


for (const restriction of ['freeze', 'seal']) {
  for (const target of ['module', 'host']) {
    test(`${restriction} lazy Web Streams ${target} before first use preserves its constructors`, () => {
      let loads = 0;
      const host = {};
      const module = create(host, () => { ++loads; return ponyfill; });
      const selected = target === 'module' ? module : host;
      Object[restriction](selected);
      const getter = Object.getOwnPropertyDescriptor(selected, 'ReadableStream').get;
      assert.equal(loads, 0);
      assert.equal(selected.ReadableStream, ponyfill.ReadableStream);
      assert.equal(selected.WritableStream, ponyfill.WritableStream);
      assert.equal(module.ReadableStream, host.ReadableStream);
      assert.equal(module.WritableStream, host.WritableStream);
      assert.equal(Object.getOwnPropertyDescriptor(selected, 'ReadableStream').get, getter);
      assert.equal(Object.getOwnPropertyDescriptor(selected, 'ReadableStream').configurable, false);
      assert.equal(loads, 1);
    });
  }
}

test('freezing a lazy Node stream constructor does not block its Web adapters', () => {
  const c = bootstrap('main');
  const Readable = c.__xenonStream.Readable;
  Object.freeze(Readable);
  assert.equal(Readable.toWeb, Readable.toWeb);
  const result = Readable.toWeb(Readable.from([]));
  assert.equal(result instanceof c.ReadableStream, true);
});


test('native state inspection retains intrinsic brands when applications replace global constructors', () => {
  const readable = new native.ReadableStream();
  const writable = new native.WritableStream();
  const readableState = {kind: 'readable', disturbed: false};
  const writableState = {kind: 'writable', errored: false};
  let getState, queries = 0;
  const host = {...native, __xenonGetWebStreamState(value) {
    ++queries;
    if (value === readable) return readableState;
    if (value === writable) return writableState;
    throw new Error('Unbranded value reached native state inspection');
  }};
  const module = create(host, () => { throw new Error('Native streams must not load fallback'); },
      read => { getState = read; });
  host.ReadableStream = function ReplacementReadable() {};
  host.WritableStream = function ReplacementWritable() {};
  assert.equal(module.ReadableStream, native.ReadableStream);
  assert.equal(module.WritableStream, native.WritableStream);
  assert.equal(getState(readable), readableState);
  assert.equal(getState(writable), writableState);
  assert.equal(getState({}), undefined);
  assert.equal(queries, 2);
});
