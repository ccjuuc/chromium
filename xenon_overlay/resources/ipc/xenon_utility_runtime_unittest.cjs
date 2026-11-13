// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const stream = require('node:stream');
const {finished} = require('node:stream/promises');
const {EventEmitter, once} = require('node:events');
const {MessageChannel} = require('node:worker_threads');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const install = vm.runInThisContext(fs.readFileSync(path.join(__dirname,
    'main/utility_runtime.js'), 'utf8') + ';installUtilityRuntime');
const installParent = vm.runInThisContext(fs.readFileSync(path.join(__dirname,
    'main/utility_runtime.js'), 'utf8') + ';installUtilityParentPort');

function transport(t, input = Buffer.alloc(0)) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-utility-stdio-'));
  const paths = ['stdin', 'stdout', 'stderr'].map(name => path.join(directory, name));
  fs.writeFileSync(paths[0], input);
  const files = paths.map((name, fd) => fs.openSync(name, fd ? 'w+' : 'r'));
  t.after(() => { for (const file of files) fs.closeSync(file); fs.rmSync(directory, {recursive:true}); });
  const calls = [];
  const call = (operation, fd, argument) => {
    calls.push([operation, fd, typeof argument === 'number' ? argument : argument?.byteLength]);
    if (operation === 'isTTY') return false;
    if (operation === 'write') {
      assert.ok(fd === 1 || fd === 2);
      // Force partial OS writes to prove the wrapper neither drops bytes nor
      // retries a chunk already written. These are real filesystem handles.
      return fs.writeSync(files[fd], argument, 0, Math.min(argument.byteLength, 4093));
    }
    assert.equal(operation, 'read'); assert.equal(fd, 0);
    const bytes = Buffer.alloc(argument);
    const count = fs.readSync(files[0], bytes, 0, bytes.length, null);
    return count ? bytes.subarray(0, count) : null;
  };
  return {call, calls, contents: fd => fs.readFileSync(paths[fd])};
}

test('utility stdio writes exact binary bytes and reads real bounded OS input', async t => {
  const input = Buffer.alloc(150000); for (let i=0;i<input.length;++i) input[i]=i%251;
  const io = transport(t, input), process = new EventEmitter();
  install({process, Buffer, stream, call:io.call, exitHost() {}});
  assert.equal(process.type, 'utility');
  assert.equal(process.stdout.fd, 1); assert.equal(process.stderr.fd, 2); assert.equal(process.stdin.fd, 0);
  let callback = false;
  const flushed = new Promise((resolve, reject) => process.stdout.write(input, error => {
    callback = true; error ? reject(error) : resolve();
  }));
  await flushed;
  assert.equal(callback, true);
  process.stderr.end('错误🙂');
  await finished(process.stderr);
  assert.deepEqual(io.contents(1), input);
  assert.equal(io.contents(2).toString(), '错误🙂');
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  assert.deepEqual(Buffer.concat(chunks), input);
  assert.ok(io.calls.filter(c => c[0] !== 'isTTY').every(c => c[2] <= 65536));
});

test('utility stdout errors reject callbacks and never report a successful zero write', async () => {
  for (const fail of ['throw', 'zero']) {
    const process = new EventEmitter(), failure = Object.assign(new Error('pipe closed'), {code:'EPIPE'});
    install({process,Buffer,stream,exitHost(){},call(operation) {
      if (operation === 'isTTY') return false;
      if (fail === 'throw') throw failure;
      return 0;
    }});
    const errorEvent = once(process.stdout, 'error');
    const rejected = new Promise(resolve => process.stdout.write('x', error => resolve(error)));
    const error = await rejected;
    assert.equal(error.code, fail === 'throw' ? 'EPIPE' : 'EIO');
    assert.equal((await errorEvent)[0], error);
  }
});

test('utility exit notifies once, validates codes and waits for the host retirement path', () => {
  const process = new EventEmitter(), events = [], codes = [], args = ['--example'];
  const runtime = install({process,Buffer,stream,execArgv:args,
    call() { return false; }, exitHost(code) { codes.push(code); }});
  args.push('--changed'); assert.deepEqual(process.execArgv, ['--example']);
  process.on('beforeExit', code => events.push(['beforeExit', code]));
  process.on('exit', code => events.push(['exit', code, process._exiting]));
  process.exitCode = '17';
  process.exit(); process.exit(2); runtime.shutdown(3);
  assert.deepEqual(events, [['exit',17,true]]);
  assert.deepEqual(codes, [17]);
  assert.equal(process.exitCode, 17);
  assert.throws(() => { process.exitCode = {}; }, {code:'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => { process.exitCode = 1.5; }, {code:'ERR_OUT_OF_RANGE'});
});

test('utility shutdown from host only emits exit and does not recursively request native exit', () => {
  const process = new EventEmitter(), codes = [];
  const runtime = install({process,Buffer,stream,call(){return false;},exitHost(){throw Error('recursive');}});
  process.on('exit', code => codes.push(code));
  process.exitCode = 9;
  runtime.shutdown(); runtime.shutdown();
  assert.deepEqual(codes, [9]);
});

test('parentPort retains real queued messages across late subscriptions and pauses', {timeout:2000}, async t => {
  const channel = new MessageChannel(), raw = new EventEmitter(), process = {};
  t.after(() => { channel.port1.close(); channel.port2.close(); });
  const forward = data => raw.emit('message', {data, ports: []});
  let active = false;
  const transitions = [];
  raw.postMessage = (...args) => channel.port1.postMessage(...args);
  const parent = installParent({process, EventEmitter, port:raw, setPaused(port, paused) {
    assert.equal(port, raw);
    if (active === !paused) return;
    transitions.push(paused);
    active = !paused;
    if (active) channel.port1.on('message', forward);
    else channel.port1.removeListener('message', forward);
  }});
  assert.equal(process.parentPort, parent);
  assert.equal(Object.getOwnPropertyDescriptor(process, 'parentPort').writable, false);
  assert.equal(parent.close, undefined);
  assert.deepEqual(transitions, []);
  channel.port2.postMessage('before listener');
  await new Promise(setImmediate);
  assert.equal((await once(parent, 'message'))[0].data, 'before listener');
  assert.deepEqual(transitions, [false,true]);
  channel.port2.postMessage('between listeners');
  await new Promise(setImmediate);
  assert.equal((await once(parent, 'message'))[0].data, 'between listeners');
  assert.deepEqual(transitions, [false,true,false,true]);
  const events = [];
  const listener = event => events.push(event);
  parent.on('message', listener);
  parent.pause();
  channel.port2.postMessage('manual pause');
  await new Promise(setImmediate);
  assert.deepEqual(events, []);
  const received = once(parent, 'message');
  parent.start();
  assert.equal((await received)[0].data, 'manual pause');
  parent.removeListener('message', listener);
  assert.equal(active, false);
});

test('parentPort forwards structured clone and transferred native ports', {timeout:2000}, async t => {
  const channel = new MessageChannel(), transfer = new MessageChannel();
  t.after(() => {
    channel.port1.close(); channel.port2.close();
    transfer.port1.close(); transfer.port2.close();
  });
  const raw = new EventEmitter();
  raw.postMessage = (...args) => channel.port1.postMessage(...args);
  const parent = installParent({process:{},EventEmitter,port:raw,setPaused(){}});
  const received = once(channel.port2, 'message');
  const bytes = new Uint8Array([0,255,128]);
  parent.postMessage({bytes, port:transfer.port1}, [transfer.port1]);
  bytes.fill(1);
  const [message] = await received;
  t.after(() => message.port.close());
  assert.deepEqual(Array.from(message.bytes), [0,255,128]);
  const portMessage = once(message.port, 'message');
  transfer.port2.postMessage('transferred endpoint');
  assert.equal((await portMessage)[0], 'transferred endpoint');
});

test('actual main bootstrap utility role exposes net without main APIs and routes console to OS streams', async t => {
  const io = transport(t), exits = [], requests = [];
  let online = false;
  const context = vm.createContext({TextEncoder,TextDecoder,URL,URLSearchParams,queueMicrotask,
    setTimeout,clearTimeout,setInterval,clearInterval,atob,btoa,console,performance,
    __xenonUtilityProcess:true,__xenonUtilityProcessCall:io.call,__xenonUtilityExit:code=>exits.push(code),
    __xenonExecArgv:[],__xenonArgv:['host','child.js','value'],
    __xenonPlatform:process.platform,__xenonArch:process.arch,__xenonEndianness:os.endianness(),__xenonOsRelease:os.release(),
    __xenonAppPath:'C:\\fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',__xenonExecPath:process.execPath,
    __xenonPid:process.pid,__xenonEnv:{UTILITY_TEST:'isolated'},__xenonChromeVersion:'153',
    __xenonV8Version:'',__xenonGetPath:()=>'',__xenonWorkerThreadId:0,
    __xenonWorkerResourceLimits:{},__xenonWorkerData:null,
    __xenonBrowserWindowCall: () => { throw Error('Utility must not use a main window/session channel'); },
    __xenonElectronNetStart: request => {
      requests.push(request);
      const id = requests.length;
      queueMicrotask(() => {
        context.__xenonDispatchElectronNetEvent({id, type:'response', statusCode:200,
          statusMessage:'OK', httpVersion:'1.1', url:request.url, headers:{}, rawHeaders:[]});
        context.__xenonDispatchElectronNetEvent({id, type:'data', bodyBase64:Buffer.from('utility network').toString('base64')});
        context.__xenonDispatchElectronNetEvent({id, type:'end'});
      });
      return id;
    },
    __xenonElectronNetControl: () => true,
    __xenonElectronNetOnline: () => online,
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')),context);
  assert.equal(context.process.type, 'utility');
  assert.deepEqual(Object.keys(context.__xenonElectron), ['net']);
  const {net} = context.__xenonElectron;
  assert.equal(net.online, false);
  online = true;
  assert.equal(net.isOnline(), true);
  assert.throws(() => net.request({url:'https://fixture.test', session:{partition:''}}), {code:'ERR_INVALID_ARG_TYPE'});
  assert.equal(await (await net.fetch('https://fixture.test')).text(), 'utility network');
  assert.equal(requests.length, 1);
  assert.equal(requests[0].partition, '');
  const response = await new Promise((resolve, reject) => {
    const request = net.request({url:'https://fixture.test', partition:'persist:other'}, resolve);
    request.once('error', reject);
    request.end();
  });
  response.resume();
  assert.equal(requests.length, 2);
  assert.equal(requests[1].partition, 'persist:other');
  assert.deepEqual(Array.from(context.process.argv), ['host','child.js','value']);
  assert.equal(context.process.env.UTILITY_TEST, 'isolated');
  context.console.log('from utility'); context.console.error('stderr utility');
  assert.equal(io.contents(1).toString(), 'from utility\n');
  assert.equal(io.contents(2).toString(), 'stderr utility\n');
  const exitEvents = [];
  context.process.on('exit', code => exitEvents.push(code));
  context.process.exit(7);
  context.__xenonShutdownApp();
  assert.deepEqual(exits, [7]); assert.deepEqual(exitEvents, [7]);
});
