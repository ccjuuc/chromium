// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const native = require('node:child_process');
const {EventEmitter} = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function load(side) {
  let listener;
  const children = new Map(), calls = [];
  const notify = (id, event) => listener({id, ...event});
  const nativeCall = request => {
    calls.push(request);
    if (request.op === 'validate-signal') {
      const signal = typeof request.signal === 'string'
          ? os.constants.signals[request.signal] : request.signal;
      return Object.values(os.constants.signals).includes(signal)
          ? {ok:true, signal} : {ok:false, code:'EINVAL'};
    }
    if (request.op === 'spawn') {
      if (!fs.existsSync(request.file)) return {ok:false,code:'ENOENT',errno:-2,message:'not found'};
      const child = native.spawn(request.file, Array.from(request.args), {
        env: request.env, cwd: request.cwd, stdio: Array.from(request.stdio),
        windowsHide:true, argv0:request.argv0,
      });
      children.set(request.id, child);
      for (const fd of [1,2]) if (child.stdio[fd]) {
        child.stdio[fd].on('data', chunk => notify(request.id, {event:'data',fd,data:chunk.toString('base64')}));
        child.stdio[fd].on('end', () => notify(request.id, {event:'end',fd}));
        child.stdio[fd].pause();
      }
      child.on('exit', (code, signal) => notify(request.id, {
        event:'exit',exitCode:code,signal:signal?os.constants.signals[signal]:0,signalName:signal,
      }));
      child.on('close', () => { children.delete(request.id); notify(request.id,{event:'close'}); });
      return {ok:true,pid:child.pid};
    }
    const child = children.get(request.id);
    if (!child) return {ok:false,code:'ESRCH'};
    const stream = child.stdio[request.fd];
    if (request.op === 'resume') stream?.resume();
    else if (request.op === 'pause') stream?.pause();
    else if (request.op === 'destroy') stream?.destroy();
    else if (request.op === 'kill') return child.kill(request.signal) ? {ok:true} : {ok:false,code:'ESRCH'};
    else if (request.op === 'ref') child.ref();
    else if (request.op === 'unref') child.unref();
    return {ok:true};
  };
  const context = vm.createContext({Buffer, Uint8Array, queueMicrotask,
    ReadableStream, WritableStream, TransformStream, AbortController,
    setTimeout, clearTimeout, currentAsyncContext:'exec-file-context',
    runWithAsyncContext: (_context, callback, receiver, args) => callback.apply(receiver,args),
    process:{env:process.env,platform:process.platform}, nativeCall,
    subscribe: callback => {listener=callback;}});
  vm.runInContext(readBootstrapPart(`${side}/events.js`) + '\n' +
      readBootstrapPart('third_party/readable_stream/bundle.js') + '\n' +
      readBootstrapPart('common/stream_web_adapters.js') + '\n' +
      readBootstrapPart('common/node_streams.js') + '\n' +
      'const streamModule = createNodeStreams({Buffer, EventEmitter, modules: createReadableStreamModules(), webStreams: {ReadableStream, WritableStream, TransformStream}}).stream;\n' +
      readBootstrapPart('common/promisify.js') + '\n' +
      readBootstrapPart('common/child_process.js') + '\n' +
      'globalThis.cp=createChildProcessModule(nativeCall,subscribe);globalThis.promisify=createNodePromisify();', context);
  return {cp:context.cp,promisify:context.promisify,calls,children};
}
function execute(cp, args, options) {
  return new Promise(resolve => cp.execFile(process.execPath, args, options,
      (error,stdout,stderr) => resolve({error,stdout,stderr})));
}
const plain = value => JSON.parse(JSON.stringify(value));

for (const side of ['main','renderer']) {
  test(`${side}: execFile collects real output without shell interpretation`, async () => {
    const {cp,calls} = load(side);
    const result = await execute(cp, ['-e',
      'process.stdout.write(JSON.stringify({arg:process.argv[1],cwd:process.cwd(),env:process.env.MARKER}));process.stderr.write("中文 stderr");',
      'a b & $literal'], {cwd:process.cwd(),env:{MARKER:'value'},windowsHide:true});
    assert.equal(result.error,null);
    assert.deepEqual(JSON.parse(result.stdout),{arg:'a b & $literal',cwd:process.cwd(),env:'value'});
    assert.equal(result.stderr,'中文 stderr');
    assert.deepEqual(Array.from(calls.find(value=>value.op==='spawn').args).slice(-1),['a b & $literal']);
  });
  test(`${side}: execFile supports omitted args, binary output and split UTF-8`, async () => {
    const {cp} = load(side);
    const binary=await execute(cp,['-e','process.stdout.write(Buffer.from([0,255,128]));'],{encoding:'buffer'});
    assert.equal(binary.error,null);
    assert.deepEqual(binary.stdout,Buffer.from([0,255,128]));
    assert.deepEqual(binary.stderr,Buffer.alloc(0));
    const utf8=await execute(cp,['-e','process.stdout.write(Buffer.from([0xe4,0xb8]));setTimeout(()=>process.stdout.write(Buffer.from([0xad,0xe6,0x96,0x87])),10);'],{});
    assert.equal(utf8.error,null);
    assert.equal(utf8.stdout,'中文');
    const output=await new Promise(resolve=>cp.execFile(process.execPath,{env:{NODE_OPTIONS:'--version'}},(error,stdout,stderr)=>resolve({error,stdout,stderr})));
    // NODE_OPTIONS forbids --version, so this exercises the no-argv overload
    // without leaving an interactive Node child waiting on its stdin.
    assert.equal(typeof output.error?.code,'number');
    assert.match(output.stderr,/not allowed in NODE_OPTIONS/);
  });
  test(`${side}: nonzero exit and custom promisify preserve both outputs and child`, async () => {
    const {cp,promisify}=load(side);
    const fn=promisify(cp.execFile);
    assert.equal(fn,cp.execFile[Symbol.for('nodejs.util.promisify.custom')]);
    assert.equal(fn.name,cp.execFile.name);
    const promise=fn(process.execPath,['-e','process.stdout.write("out");process.stderr.write("err");process.exitCode=7;']);
    assert(promise.child instanceof cp.ChildProcess);
    await assert.rejects(promise,error=>{
      assert.equal(error.code,7);assert.equal(error.signal,null);assert.equal(error.killed,false);
      assert.equal(error.stdout,'out');assert.equal(error.stderr,'err');assert.match(error.cmd,/-e /);return true;
    });
    const success=await fn(process.execPath,['-e','process.stdout.write("ok");process.stderr.write("note");']);
    assert.deepEqual(plain(success),{stdout:'ok',stderr:'note'});
  });
  test(`${side}: maxBuffer counts encoded bytes and matches Node truncation at UTF-8 boundaries`, async () => {
    const {cp}=load(side);
    for(const [fd,options] of [['stdout',{maxBuffer:4}],['stderr',{maxBuffer:4}],['stdout',{maxBuffer:4,encoding:null}],['stdout',{maxBuffer:Infinity}]]){
      const args=['-e',`process.${fd}.write('中文😀more');`];
      const expected=await execute(native,args,options),actual=await execute(cp,args,options);
      assert.equal(actual.error?.code,expected.error?.code);
      assert.deepEqual(actual.stdout,expected.stdout);
      assert.deepEqual(actual.stderr,expected.stderr);
    }
  });
  test(`${side}: timeout kills the real process and reports its close result once`, async () => {
    const {cp}=load(side);
    let callbacks=0;
    const value=await new Promise(resolve=>{
      const child=cp.execFile(process.execPath,['-e','setInterval(()=>{},1000)'],{timeout:35,killSignal:'sigterm'},(error,stdout,stderr)=>{
        callbacks++;resolve({error,stdout,stderr,child});
      });
    });
    assert.equal(value.error.killed,true);assert.equal(value.error.signal,'SIGTERM');
    assert.equal(value.error.code,null);assert.equal(value.child.killed,true);assert.equal(callbacks,1);
  });
  test(`${side}: abort reports cause once for pending and pre-aborted signals`, async () => {
    for(const preAborted of [false,true]){
      const {cp}=load(side),controller=new AbortController(),reason={why:'probe'};
      if(preAborted)controller.abort(reason);
      let callbacks=0;
      const value=await new Promise(resolve=>{
        const child=cp.execFile(process.execPath,['-e','setInterval(()=>{},1000)'],{signal:controller.signal},(error)=>{
          callbacks++;resolve({error,child});
        });
        if(!preAborted)setTimeout(()=>controller.abort(reason),20);
      });
      assert.equal(value.error.name,'AbortError');assert.equal(value.error.code,'ABORT_ERR');
      assert.equal(value.error.cause,reason);assert.equal(value.child.killed,true);
      await new Promise(resolve=>value.child.once('close',resolve));
      assert.equal(callbacks,1);
    }
  });
  test(`${side}: execFile rejects invalid options before spawning and reports ENOENT asynchronously`, async () => {
    const {cp,calls}=load(side);
    for(const options of [{timeout:-1},{timeout:0.2},{maxBuffer:-1},{maxBuffer:NaN},{killSignal:'SIG_NOT_REAL'},{killSignal:0},{signal:{}}])
      assert.throws(()=>cp.execFile(process.execPath,[],options));
    assert.throws(()=>cp.execFile(process.execPath,[],{shell:true}),{code:'ERR_NOT_SUPPORTED'});
    assert.equal(calls.some(value=>value.op==='spawn'),false);
    let synchronous=true;
    const value=await new Promise(resolve=>{
      cp.execFile(process.execPath+'.missing',[],(error,stdout,stderr)=>resolve({error,stdout,stderr,synchronous}));
      synchronous=false;
    });
    assert.equal(value.synchronous,false);assert.equal(value.error.code,'ENOENT');
    assert.equal(value.stdout,'');assert.equal(value.stderr,'');
  });
}

test('shared promisify preserves receiver, descriptors and registered custom symbol',async()=>{
  const {promisify}=load('main');
  function original(value,callback){callback(null,this.prefix+value);}
  Object.defineProperty(original,'metadata',{value:'kept',enumerable:false});
  const wrapped=promisify(original);
  assert.equal(await wrapped.call({prefix:'prefix:'},'value'),'prefix:value');
  assert.equal(promisify(wrapped),wrapped);
  assert.equal(wrapped.name,original.name);assert.equal(wrapped.length,original.length);
  assert.deepEqual(Object.getOwnPropertyDescriptor(wrapped,'metadata'),Object.getOwnPropertyDescriptor(original,'metadata'));
  assert.equal(promisify.custom,require('node:util').promisify.custom);
  assert.throws(()=>promisify(null),{code:'ERR_INVALID_ARG_TYPE'});
  original[promisify.custom]=true;
  assert.throws(()=>promisify(original),{code:'ERR_INVALID_ARG_TYPE'});
  original[promisify.custom]=()=>Promise.resolve('custom');
  assert.equal(await promisify(original)(),'custom');
  assert.equal(promisify(promisify(original)),promisify(original));
});
