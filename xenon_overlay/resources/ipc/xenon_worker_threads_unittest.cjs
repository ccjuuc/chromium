// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const url = require('node:url');
const test = require('node:test');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const source = fs.readFileSync(path.join(__dirname, 'main/worker_threads.js'), 'utf8');
const plain = value => JSON.parse(JSON.stringify(value));
const turn = () => new Promise(resolve => setImmediate(resolve));

function fixture(threadId = 0, workerData = null) {
  const creates = [], posts = [], controls = [];
  let nextId = 20;
  const parentProcess = {env: {TEST_WORKER_ENV: 'parent'}};
  const effectiveLimits = {stackSizeMb: 4, maxOldGenerationSizeMb: 128, maxYoungGenerationSizeMb: 16};
  const context = vm.createContext({URL, queueMicrotask,
    __xenonWorkerThreadId: threadId, __xenonWorkerThreadName: threadId ? 'native-worker' : '',
    __xenonWorkerData: workerData, __xenonWorkerResourceLimits: threadId ? effectiveLimits : {},
    __xenonWorkerCreate: (filename, options, data, transfers) => {
      if (threadId) throw Object.assign(new Error('Nested Workers are not supported'), {code: 'ERR_NOT_SUPPORTED'});
      const cloned = structuredClone(data, {transfer: transfers});
      creates.push({filename, options: plain(options), data: cloned, originalData: data});
      return {id: ++nextId, threadName: options.name || 'native-default-name', resourceLimits: effectiveLimits};
    },
    __xenonWorkerPost: (id, data, transfers) => posts.push({id, originalData: data, data: structuredClone(data, {transfer: transfers})}),
    __xenonWorkerControl: (id, command) => { controls.push({id, command}); return true; },
  });
  vm.runInContext(source, context);
  const api = context.createWorkerThreads({EventEmitter, getProcess: () => parentProcess, pathModule: path, urlModule: url});
  return {context, api, creates, posts, controls, parentProcess, effectiveLimits,
    dispatch: (id, type, payload) => context.__xenonDispatchWorkerEvent(id, type, payload)};
}

test('Worker entry and environment are snapshotted while workerData goes to native structured clone', () => {
  const f = fixture();
  const data = {map: new Map([['key', 42n]]), date: new Date('2026-01-01')};
  const worker = new f.api.Worker('./fixture-worker.cjs', {workerData: data, argv: ['one', 2],
    name: 'named-worker', resourceLimits: {maxOldGenerationSizeMb: 64}, trackUnmanagedFds: false});
  f.parentProcess.env.TEST_WORKER_ENV = 'changed';
  const request = f.creates[0];
  assert.equal(request.filename, path.resolve('./fixture-worker.cjs'));
  assert.equal(request.options.env.TEST_WORKER_ENV, 'parent');
  assert.deepEqual(request.options.argv, ['one', '2']);
  assert.equal(request.options.trackUnmanagedFds, false);
  assert.equal(request.originalData, data);
  assert.equal(request.data.map.get('key'), 42n);
  assert.equal(request.data.date.getTime(), data.date.getTime());
  assert.equal(worker.threadId, 21);
  assert.equal(worker.threadName, 'named-worker');
  assert.deepEqual(plain(worker.resourceLimits), f.effectiveLimits);
  assert.equal(f.api.isMainThread, true); assert.equal(f.api.parentPort, null);
});

test('file URLs resolve through the normal platform file URL helper', () => {
  const f = fixture();
  const filename = path.join(process.cwd(), 'worker with spaces.mjs');
  new f.api.Worker(url.pathToFileURL(filename));
  assert.equal(f.creates[0].filename, filename);
  assert.throws(() => new f.api.Worker('package-name'), {code: 'ERR_WORKER_PATH'});
  assert.throws(() => new f.api.Worker(new URL('https://fixture.test/worker.js')), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => new f.api.Worker('./bad\0name.cjs'), {code: 'ERR_INVALID_ARG_VALUE'});
});

test('markAsUncloneable forwards object identity to native and leaves primitives alone', () => {
  const f = fixture(), marked = [];
  for (const value of [undefined,null,0,'value',Symbol('primitive')])
    assert.equal(f.api.markAsUncloneable(value), undefined);
  assert.throws(() => f.api.markAsUncloneable({}), {code:'ERR_NOT_SUPPORTED'});
  f.context.__xenonMarkAsUncloneable = value => {
    marked.push(value);
    require('node:worker_threads').markAsUncloneable(value);
  };
  const value = Object.freeze({nested:'retained'});
  assert.equal(f.api.markAsUncloneable(value), undefined);
  assert.equal(marked[0], value);
  assert.throws(() => new f.api.Worker('source', {eval:true,workerData:{value}}),
    {name:'DataCloneError',code:25});
  const bytes = new Uint8Array([1,2]);
  f.api.markAsUncloneable(bytes);
  const worker = new f.api.Worker('source', {eval:true,workerData:bytes});
  assert.ok(worker.threadId > 0);
  assert.deepEqual(Array.from(f.creates[0].data), [1,2]);
});

test('ArrayBuffer transfer and SharedArrayBuffer use the native transport without JSON conversion', () => {
  const f = fixture();
  const transfer = new ArrayBuffer(8);
  new Uint8Array(transfer)[0] = 17;
  const worker = new f.api.Worker('source', {eval: true, workerData: {transfer}, transferList: [transfer]});
  assert.equal(transfer.byteLength, 0);
  assert.equal(new Uint8Array(f.creates[0].data.transfer)[0], 17);
  const shared = new SharedArrayBuffer(4);
  const message = {shared, number: 42n, map: new Map([['value', 1]])};
  worker.postMessage(message);
  assert.equal(f.posts[0].originalData, message);
  new Int32Array(f.posts[0].data.shared)[0] = 123;
  assert.equal(new Int32Array(shared)[0], 123);
  let received;
  worker.on('message', value => { received = value; });
  f.dispatch(worker.threadId, 'message', f.posts[0].data);
  assert.equal(received.number, 42n);
  assert.equal(received.map.get('value'), 1);
});

test('failed structured clone never detaches a listed buffer', () => {
  const f = fixture();
  const buffer = new ArrayBuffer(8);
  assert.throws(() => new f.api.Worker('source', {eval: true, workerData: {buffer, invalid: () => {}}, transferList: [buffer]}));
  assert.equal(buffer.byteLength, 8);
  assert.equal(f.creates.length, 0);
});

test('terminate resolves only on the native exit event and ignores late delivery', async () => {
  const f = fixture(), events = [];
  const worker = new f.api.Worker('source', {eval: true});
  const id = worker.threadId;
  worker.on('online', () => events.push('online'));
  worker.on('message', () => events.push('message'));
  worker.on('exit', code => events.push('exit:' + code));
  f.dispatch(id, 'online'); f.dispatch(id, 'online');
  const pending = worker.terminate();
  assert.equal(worker.terminate(), pending);
  let resolved = false; pending.then(() => { resolved = true; });
  await turn(); assert.equal(resolved, false);
  f.dispatch(id, 'exit', 1);
  assert.equal(await pending, 1);
  assert.equal(worker.threadId, -1);
  assert.equal(worker.threadName, null);
  assert.deepEqual(plain(worker.resourceLimits), {});
  f.dispatch(id, 'message', 'late'); f.dispatch(id, 'exit', 2);
  const buffer = new ArrayBuffer(4); worker.postMessage(buffer, [buffer]);
  assert.equal(buffer.byteLength, 4);
  assert.deepEqual(events, ['online', 'exit:1']);
  assert.equal(await worker.terminate(), undefined);
});

test('native error retains actual details and exit listeners cannot strand terminate promises', async () => {
  const f = fixture(); let observed;
  const worker = new f.api.Worker('source', {eval: true});
  worker.on('error', error => { observed = error; });
  f.dispatch(worker.threadId, 'error', {name: 'TypeError', message: 'actual worker failure', stack: 'actual stack', code: 'ERR_WORKER_TEST'});
  assert.equal(observed.name, 'TypeError'); assert.equal(observed.stack, 'actual stack');
  assert.equal(observed.code, 'ERR_WORKER_TEST');
  const pending = worker.terminate();
  worker.on('exit', () => { throw new Error('exit listener failure'); });
  assert.throws(() => f.dispatch(worker.threadId, 'exit', 7), /exit listener failure/);
  assert.equal(await pending, 7);
});

test('ref/unref and shutdown reach the real worker controls', () => {
  const f = fixture();
  const worker = new f.api.Worker('source', {eval: true});
  assert.equal(worker.unref(), worker); assert.equal(worker.ref(), worker);
  f.context.__xenonShutdownWorkers();
  assert.deepEqual(f.controls.map(entry => entry.command), ['unref', 'ref', 'terminate']);
});

test('parentPort queues early messages, refs listeners and waits for native close', async () => {
  const data = new Map([['worker', true]]);
  const f = fixture(7, data), messages = [];
  assert.equal(f.api.isMainThread, false); assert.equal(f.api.threadId, 7);
  assert.equal(f.api.workerData, data);
  assert.equal(f.api.threadName, 'native-worker');
  const port = f.api.parentPort;
  assert.ok(port instanceof f.api.MessagePort);
  f.dispatch(0, 'message', {early: true}); await turn();
  const listener = message => messages.push(message);
  port.on('message', listener); await turn();
  assert.deepEqual(messages, [{early: true}]); assert.equal(port.hasRef(), true);
  port.removeListener('message', listener); assert.equal(port.hasRef(), false);
  let closed = 0; port.on('close', () => closed++);
  const callbacks = [];
  port.close(() => callbacks.push('first')); assert.equal(closed, 0);
  port.close(() => callbacks.push('second'));
  f.dispatch(0, 'close'); f.dispatch(0, 'close');
  assert.equal(closed, 1);
  port.close(() => callbacks.push('late'));
  assert.deepEqual(callbacks, ['first', 'second']);
  const buffer = new ArrayBuffer(4); port.postMessage(buffer, [buffer]);
  assert.equal(buffer.byteLength, 4);
  assert.deepEqual(f.controls.map(entry => entry.command), ['ref', 'unref', 'close']);
});

test('unsupported worker options and unavailable port features fail explicitly', () => {
  const f = fixture();
  for (const options of [{eval: true, env: f.api.SHARE_ENV}, {eval: true, stdout: true},
    {eval: true, execArgv: ['--inspect']}, {eval: true, resourceLimits: {codeRangeSizeMb: 32}},
    {eval: true, unknownWorkerFlag: true}])
    assert.throws(() => new f.api.Worker('source', options), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => new f.api.MessageChannel(), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => new f.api.MessagePort(), {code: 'ERR_ILLEGAL_CONSTRUCTOR'});
  const nested = fixture(1);
  assert.throws(() => new nested.api.Worker('source', {eval: true}), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => { nested.api.parentPort.onmessage = () => {}; }, {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => { nested.api.parentPort.onmessageerror = () => {}; }, {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(f.creates.length, 0);
});

test('removing the last parentPort listener retains later messages until listening resumes', async () => {
  const f = fixture(7), messages = [], port = f.api.parentPort;
  port.once('message', value => messages.push(value));
  f.dispatch(0, 'message', 'first');
  f.dispatch(0, 'message', 'second');
  await turn();
  assert.deepEqual(messages, ['first']);
  assert.equal(port.hasRef(), false);
  port.on('message', value => messages.push(value));
  await turn();
  assert.deepEqual(messages, ['first', 'second']);
});

test('invalid native metadata terminates the allocated Worker instead of leaking it', () => {
  const f = fixture();
  f.context.__xenonWorkerCreate = () => ({id: 99, resourceLimits: {}});
  assert.throws(() => new f.api.Worker('source', {eval: true}), {code: 'ERR_INVALID_STATE'});
  assert.deepEqual(f.controls, [{id: 99, command: 'terminate'}]);
});

function bootstrapFixture() {
  const exits = [], controls = [];
  const context = vm.createContext({TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
    __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
    __xenonEnv: {WORKER_ONLY: 'yes'}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonWorkerThreadId: 9, __xenonWorkerThreadName: 'worker-nine', __xenonWorkerData: {test: 9},
    __xenonWorkerResourceLimits: {stackSizeMb: 4}, __xenonWorkerExit: code => exits.push(code),
    __xenonInstallAsyncContextHooks: () => {},
    __xenonWorkerControl: (id, command) => { controls.push({id, command}); return true; },
    __xenonExitApp: () => { throw new Error('Worker must not exit the Electron app'); },
  });
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readBootstrap(filename), context, {filename});
  return {context, exits, controls};
}

test('complete bootstrap separates worker process metadata and exit from Electron app lifecycle', () => {
  const {context, exits} = bootstrapFixture();
  assert.equal(context.__xenonElectron, undefined);
  assert.equal(context.process.type, undefined);
  assert.equal(context.process.versions.electron, undefined);
  assert.equal(context.process.electronBinding, undefined);
  assert.equal(context.process.env.WORKER_ONLY, 'yes');
  assert.equal(context.__xenonWorkerThreads.threadId, 9);
  assert.equal(context.MessagePort, context.__xenonWorkerThreads.MessagePort);
  assert.ok(context.__xenonWorkerThreads.parentPort instanceof context.MessagePort);
  assert.throws(() => new context.MessagePort(), {code: 'ERR_ILLEGAL_CONSTRUCTOR'});
  assert.deepEqual(plain(context.__xenonWorkerThreads.workerData), {test: 9});
  assert.equal(context.process.emit('uncaughtException', new Error('worker failure')), false);
  context.__xenonMarkAppReady(); context.__xenonShutdownApp();
  context.process.exit(3);
  assert.deepEqual(exits, [3]);
});

test('worker beforeExit can create more work and natural shutdown emits exit once', async () => {
  const {context, exits} = bootstrapFixture(), events = [];
  context.process.exitCode = '4';
  context.process.once('beforeExit', code => {
    events.push('before:' + code);
    context.setTimeout(() => events.push('timer'), 0);
    context.process.exitCode = 5;
  });
  context.process.on('exit', code => events.push('exit:' + code));
  assert.equal(context.__xenonWorkerBeforeExit(), 5);
  assert.deepEqual(events, ['before:4']);
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(context.__xenonWorkerBeforeExit(), 5);
  assert.equal(context.__xenonWorkerShutdown(5), 5);
  context.__xenonWorkerShutdown(9); context.__xenonWorkerBeforeExit();
  assert.deepEqual(events, ['before:4', 'timer', 'exit:5']);
  assert.deepEqual(exits, []);
});

test('explicit worker exit preserves listener exceptions and emits exit only once', () => {
  const {context, exits} = bootstrapFixture(), events = [];
  for (const value of [true, {}, ''])
    assert.throws(() => { context.process.exitCode = value; }, {code: 'ERR_INVALID_ARG_TYPE'});
  for (const value of [1.5, NaN, Infinity])
    assert.throws(() => { context.process.exitCode = value; }, {code: 'ERR_OUT_OF_RANGE'});
  context.process.exitCode = '7';
  context.process.on('beforeExit', () => events.push('unexpected'));
  context.process.on('exit', code => { events.push(code); context.process.exitCode = 8; throw new Error('exit listener'); });
  assert.throws(() => context.process.exit(), /exit listener/);
  assert.deepEqual(events, [7]);
  assert.deepEqual(exits, []);
  assert.equal(context.process._exiting, true);
  context.__xenonWorkerShutdown(9);
  assert.deepEqual(events, [7]);
  context.process.exit();
  assert.deepEqual(exits, [8]);
});

test('native worker events start fresh async contexts while listener continuations retain their store', async () => {
  const {context} = bootstrapFixture(), stores = [];
  const storage = new context.__xenonAsyncHooks.AsyncLocalStorage();
  context.__xenonWorkerCreate = () => ({id: 17, threadName: 'async-context', resourceLimits: {}});
  const worker = new context.__xenonWorkerThreads.Worker('source', {eval: true});
  worker.on('message', value => {
    stores.push(storage.getStore());
    storage.enterWith(value);
    context.queueMicrotask(() => stores.push(storage.getStore()));
  });
  context.__xenonDispatchWorkerEvent(17, 'message', 'first');
  await turn();
  context.__xenonDispatchWorkerEvent(17, 'message', 'second');
  await turn();
  assert.deepEqual(stores, [undefined, 'first', undefined, 'second']);
});
