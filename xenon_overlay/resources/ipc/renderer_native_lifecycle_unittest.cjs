// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run directly with node --expose-gc to include the real GC lifetime checks.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const modulePath = 'C:\\test-app\\fixture.node';
const source = readBootstrap(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js'));
const members = ['run', 'waitLoadFinish'].map(name => ({name, kind: 'function'}));

function createRenderer(overrides = {}, realFinalization = false) {
  const calls = [], registrations = [], timers = [];
  let dispatch, cleanup;
  class TestFinalizationRegistry {
    constructor(callback) { cleanup = callback; }
    register(target, heldValue) {
      registrations.push({target: new WeakRef(target), heldValue});
    }
  }
  const transport = {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe'}),
    setDispatchHandler(callback) { dispatch = callback; },
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat' && request.path === modulePath)
        return {isFile: true, isDirectory: false};
      throw new Error('ENOENT: no such file');
    },
    requireNodeModuleSync() {
      calls.push(['load']);
      return [{name: 'Worker', kind: 'class', prototype: members},
        {name: 'read', kind: 'function'}];
    },
    constructNodeExportSync(...args) {
      calls.push(['construct', ...args]);
      return {instance_id: 7, owner_token: 'owner-a'};
    },
    invokeNodeInstanceSync(...args) { calls.push(['instance', ...args]); return 42; },
    invokeNodeExportSync(...args) { calls.push(['export', ...args]); return 42; },
    inspectNodeInstanceMemberSync(...args) {
      calls.push(['inspect', ...args]); return {kind: 'undefined'};
    },
    invoke(channel, request) {
      calls.push(['async', channel, request]);
      if (channel.endsWith('construct-export'))
        return Promise.resolve({instance_id: 8, owner_token: 'owner-async'});
      return Promise.resolve(43);
    },
    releaseNodeInstance(...args) { calls.push(['release', ...args]); },
    ...overrides,
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    xenonIpcRenderer: transport,
    FinalizationRegistry: realFinalization ? FinalizationRegistry : TestFinalizationRegistry,
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    setTimeout(callback, delay) { timers.push({callback, delay}); return timers.length; },
    clearTimeout() {}, setInterval() {}, clearInterval() {},
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(source, context);
  return {context, transport, calls, registrations, timers,
    load: () => context.require(modulePath),
    finalize: index => cleanup(registrations[index].heldValue),
    dispatch: (...args) => dispatch(...args)};
}

test('native handles carry owner tokens through calls, inspection, arguments and cleanup', () => {
  const renderer = createRenderer();
  const addon = renderer.load();
  const instance = new addon.Worker();
  assert.equal(instance.run(3), 42);
  assert.equal(instance.optional, undefined);
  assert.equal(addon.read(instance), 42);
  assert.equal(renderer.registrations.length, 1);
  const registration = renderer.registrations[0];
  const handle = registration.target.deref();
  assert.notEqual(handle, instance);
  assert.equal(Object.hasOwn(handle, 'proxy'), false);
  assert.equal(Object.hasOwn(handle, 'ctorArgs'), false);
  assert.ok(Object.getOwnPropertySymbols(instance).some(key => instance[key] === handle));
  assert.deepEqual(Object.keys(registration.heldValue).sort(), ['instanceId', 'modulePath', 'ownerToken']);
  assert.ok(Object.values(registration.heldValue).every(value =>
    typeof value === 'string' || typeof value === 'number'));
  const call = renderer.calls.find(call => call[0] === 'instance');
  assert.deepEqual(JSON.parse(JSON.stringify(call[2])), {instanceId: 7, ownerToken: 'owner-a'});
  const inspection = renderer.calls.find(call => call[0] === 'inspect');
  assert.deepEqual(JSON.parse(JSON.stringify(inspection[2])), {instanceId: 7, ownerToken: 'owner-a'});
  const wire = renderer.calls.find(call => call[0] === 'export')[3];
  assert.equal(wire.instance_id, 7);
  assert.equal(wire.owner_token, 'owner-a');
  renderer.finalize(0);
  assert.deepEqual(renderer.calls.at(-1), ['release', modulePath, 7, 'owner-a']);
});

test('an async constructor registers its trusted reply token and forwards it to async calls', async () => {
  const renderer = createRenderer({constructNodeExportSync: undefined,
    invokeNodeInstanceSync: undefined});
  const instance = new (renderer.load().Worker)(() => {});
  assert.equal(renderer.registrations.length, 0);
  assert.equal(await instance.run(() => {}), 43);
  assert.equal(renderer.registrations.length, 1);
  const request = renderer.calls.find(call =>
    call[0] === 'async' && call[1].endsWith('invoke-instance'))[2];
  assert.equal(request.instanceId, 8);
  assert.equal(request.ownerToken, 'owner-async');
  renderer.finalize(0);
  assert.deepEqual(renderer.calls.at(-1), ['release', modulePath, 8, 'owner-async']);
});

test('returned native functions use token-scoped instance calls and cleanup', () => {
  const renderer = createRenderer({invokeNodeExportSync() {
    return {__xenon_node_wire_type__: 'native_function', module_path: modulePath,
      instance_id: 11, owner_token: 'owner-function', name: 'nativeFn'};
  }});
  const fn = renderer.load().read();
  assert.equal(typeof fn, 'function');
  assert.equal(fn(3), 42);
  assert.equal(renderer.registrations.length, 1);
  const call = renderer.calls.find(call => call[0] === 'instance');
  assert.equal(call[2].instanceId, 11);
  assert.equal(call[2].ownerToken, 'owner-function');
  renderer.finalize(0);
  assert.deepEqual(renderer.calls.at(-1), ['release', modulePath, 11, 'owner-function']);
});

test('numeric legacy handles remain usable without unsafe GC release', () => {
  const renderer = createRenderer({constructNodeExportSync() { return 7; }});
  const instance = new (renderer.load().Worker)();
  assert.equal(instance.run(), 42);
  assert.equal(renderer.calls.find(call => call[0] === 'instance')[2], 7);
  assert.equal(renderer.registrations.length, 0);
});

test('waitLoadFinish has no business-specific timeout or fabricated callback', async () => {
  const renderer = createRenderer();
  const instance = new (renderer.load().Worker)();
  let callbacks = 0;
  await instance.waitLoadFinish(() => { ++callbacks; });
  assert.equal(callbacks, 0);
  assert.deepEqual(renderer.timers, []);
  const callback = renderer.calls.find(call => call[0] === 'instance')[4];
  renderer.dispatch('__xenon:node-addon:callback', [callback.callback_id, []]);
  assert.equal(callbacks, 1);
});

test('sync native errors preserve the original error and never retry based on its text', () => {
  const error = new Error('Unknown instance id; Utility service restarted');
  let invokes = 0;
  const renderer = createRenderer({
    invokeNodeInstanceSync() { ++invokes; throw error; },
    invokeNodeExportSync() { ++invokes; throw error; },
  });
  const addon = renderer.load();
  const instance = new addon.Worker();
  assert.throws(() => instance.run(), value => value === error);
  assert.throws(() => addon.read(), value => value === error);
  assert.equal(invokes, 2);
  assert.equal(renderer.calls.filter(call => call[0] === 'construct').length, 1);
  assert.equal(renderer.calls.filter(call => call[0] === 'load').length, 1);
});

test('async errors propagate once and explicit release never reconstructs an instance', async () => {
  const error = Object.assign(new Error('Unknown instance id'), {code: 'ERR_NATIVE_INSTANCE_INVALIDATED'});
  let released = false, invokes = 0;
  const renderer = createRenderer({
    releaseNodeInstance() { released = true; },
    invokeNodeInstanceSync(...args) {
      ++invokes;
      if (released) throw error;
      return args[3]?.__xenon_node_wire_type__ === 'callback' ? Promise.reject(error) : 42;
    },
    invoke() { ++invokes; return Promise.reject(error); },
  });
  const instance = new (renderer.load().Worker)();
  await assert.rejects(instance.run(() => {}), value => value === error);
  renderer.finalize(0);
  assert.throws(() => instance.run(), value => value === error);
  assert.equal(invokes, 2);
  assert.equal(renderer.calls.filter(call => call[0] === 'construct').length, 1);
});

test('a stale finalizer retains its old token when a new owner reuses the numeric id', () => {
  let token = 'old-owner';
  const freed = [];
  const renderer = createRenderer({
    constructNodeExportSync() { return {instance_id: 7, owner_token: token}; },
    releaseNodeInstance(_path, id, suppliedToken) {
      if (suppliedToken === token) freed.push(id);
    },
  });
  const Worker = renderer.load().Worker;
  const first = new Worker();
  token = 'new-owner';
  const second = new Worker();
  renderer.finalize(0);
  assert.deepEqual(freed, []);
  renderer.finalize(1);
  assert.deepEqual(freed, [7]);
  assert.equal(first.__instanceId, second.__instanceId);
});

async function collectGarbage(turns = 8) {
  for (let index = 0; index < turns; ++index) {
    global.gc();
    await new Promise(resolve => setImmediate(resolve));
  }
}

test('native callbacks reuse their original receiver and refresh native fields', async () => {
  const renderer = createRenderer();
  const instance = new (renderer.load().Worker)();
  instance.localState = 'preserved';
  let received;
  await instance.run(function(error) {
    'use strict';
    received = {receiver: this, error, id: this.lastID, local: this.localState};
  });
  const callback = renderer.calls.find(call => call[0] === 'instance')[4];
  const receiver = {__xenon_node_wire_type__: 'native_instance', module_path: modulePath,
    instance_id: 7, owner_token: 'owner-a', fields: {lastID: 42, changes: 1}};
  renderer.dispatch('__xenon:node-addon:callback', [callback.callback_id,
    [null], receiver]);
  assert.equal(received.receiver, instance);
  assert.equal(received.error, null);
  assert.equal(received.id, 42);
  assert.equal(received.local, 'preserved');
  assert.equal(renderer.registrations.length, 1);
});

test('native callback receivers retain primitive, global and legacy undefined semantics', async () => {
  const renderer = createRenderer();
  const values = [];
  await renderer.load().read(function() { 'use strict'; values.push(this); });
  const callbackId = renderer.calls.find(call => call[0] === 'export')[3].callback_id;
  for (const receiver of [null, 7, 'text', {__xenon_node_wire_type__: 'global'}, undefined]) {
    renderer.dispatch('__xenon:node-addon:callback', [callbackId, [], receiver]);
  }
  assert.equal(values[0], null);
  assert.equal(values[1], 7);
  assert.equal(values[2], 'text');
  assert.equal(values[3], vm.runInContext('globalThis', renderer.context));
  assert.equal(values[4], undefined);
});

test('a constructor callback arriving before its reply waits for the original wrapper', async () => {
  let respond, request;
  const renderer = createRenderer({constructNodeExportSync: undefined, invoke(_channel, value) {
    request = value;
    return new Promise(resolve => { respond = resolve; });
  }});
  let receiver;
  const instance = new (renderer.load().Worker)(function() { receiver = this; });
  await new Promise(resolve => setImmediate(resolve));
  renderer.dispatch('__xenon:node-addon:callback', [request.arguments[0].callback_id, [], {
    __xenon_node_wire_type__: 'native_instance', module_path: modulePath,
    instance_id: 9, owner_token: 'ctor-owner',
  }]);
  assert.equal(receiver, undefined);
  respond({instance_id: 9, owner_token: 'ctor-owner'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(receiver, instance);
  assert.equal(renderer.registrations.length, 1);
});

test('repeated native return values share one wrapper and one GC lease',
    {skip: typeof global.gc !== 'function'}, async () => {
  const renderer = createRenderer({invokeNodeExportSync() {
    return {__xenon_node_wire_type__: 'native_instance', module_path: modulePath,
      instance_id: 7, owner_token: 'owner-a', prototype: members};
  }}, true);
  const addon = renderer.load();
  let first = new addon.Worker();
  let second = addon.read();
  assert.equal(first, second);
  first = null;
  await collectGarbage();
  assert.equal(renderer.calls.some(call => call[0] === 'release'), false);
  assert.equal(second.run(), 42);
  second = null;
  await collectGarbage(24);
  assert.equal(renderer.calls.filter(call => call[0] === 'release').length, 1);
});

test('native constructors forward added prototype methods without overriding native originals', () => {
  let properties, forwardedCallback;
  const renderer = createRenderer({constructNodeExportWithPrototypeSync(_path, _name, values, callback) {
    properties = values;
    forwardedCallback = callback;
    return {instance_id: 7, owner_token: 'owner-a'};
  }});
  const Worker = renderer.load().Worker;
  const nativeRun = Worker.prototype.run;
  let wrapperCalls = 0;
  Worker.prototype.run = function(...args) {
    ++wrapperCalls;
    return nativeRun.apply(this, args);
  };
  Worker.prototype.emit = function(event) { this.lastEvent = event; };
  Object.defineProperty(Worker.prototype, 'getter', {get() {
    throw new Error('Prototype snapshot must not invoke accessors');
  }});
  const instance = new Worker(() => {});
  assert.equal(forwardedCallback.__xenon_node_wire_type__, 'callback');
  assert.deepEqual(Object.keys(properties), ['emit']);
  assert.equal(instance.run(), 42);
  assert.equal(wrapperCalls, 1);
  renderer.dispatch('__xenon:node-addon:callback', [properties.emit.callback_id, ['open'], {
    __xenon_node_wire_type__: 'native_instance', module_path: modulePath,
    instance_id: 7, owner_token: 'owner-a',
  }]);
  assert.equal(instance.lastEvent, 'open');
});

test('legacy async constructors also forward added prototype callbacks', async () => {
  let request;
  const renderer = createRenderer({constructNodeExportSync: undefined,
    invoke(_channel, value) {
      request = value;
      return Promise.resolve({instance_id: 7, owner_token: 'owner-a'});
    }});
  const Worker = renderer.load().Worker;
  Worker.prototype.emit = function(event) { this.lastEvent = event; };
  const instance = new Worker();
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(Object.keys(request.prototypeProperties), ['emit']);
  renderer.dispatch('__xenon:node-addon:callback',
    [request.prototypeProperties.emit.callback_id, ['open'], {
      __xenon_node_wire_type__: 'native_instance', module_path: modulePath,
      instance_id: 7, owner_token: 'owner-a',
    }]);
  assert.equal(instance.lastEvent, 'open');
});

test('real GC waits for an extracted method to become unreachable',
    {skip: typeof global.gc !== 'function'}, async () => {
  const renderer = createRenderer({}, true);
  let method = new (renderer.load().Worker)().run;
  await collectGarbage();
  assert.equal(renderer.calls.some(call => call[0] === 'release'), false);
  assert.equal(method(), 42);
  method = null;
  await collectGarbage(24);
  assert.equal(renderer.calls.filter(call => call[0] === 'release').length, 1);
});

test('real GC keeps a receiver alive until its pending native call completes',
    {skip: typeof global.gc !== 'function'}, async () => {
  let complete;
  const result = new Promise(resolve => { complete = resolve; });
  const renderer = createRenderer({invokeNodeInstanceSync() { return result; }}, true);
  let instance = new (renderer.load().Worker)();
  const pending = instance.run();
  instance = null;
  await collectGarbage();
  assert.equal(renderer.calls.some(call => call[0] === 'release'), false);
  complete(42);
  assert.equal(await pending, 42);
  await collectGarbage(24);
  assert.equal(renderer.calls.filter(call => call[0] === 'release').length, 1);
});

for (const mode of ['callback export', 'callback constructor', 'callback instance',
  'promise export', 'promise instance', 'rejected export']) {
  test(`real GC retains nested native arguments through ${mode}`,
      {skip: typeof global.gc !== 'function'}, async () => {
    let complete, fail, nextId = 7;
    const response = new Promise((resolve, reject) => { complete = resolve; fail = reject; });
    const releases = [];
    const renderer = createRenderer({
      constructNodeExportSync() { return {instance_id: nextId++, owner_token: 'args-owner'}; },
      invokeNodeExportSync() { return response; },
      invokeNodeInstanceSync() { return response; },
      invoke() { return response; },
      releaseNodeInstance(_path, id) { releases.push(id); },
    }, true);
    const addon = renderer.load();
    let argument = new addon.Worker();
    const receiver = new addon.Worker();
    let operation, constructed;
    if (mode === 'callback constructor') {
      renderer.transport.constructNodeExportSync = undefined;
      constructed = new addon.Worker([0, {nested: argument}], () => {});
    } else if (mode === 'callback instance') {
      operation = receiver.run([0, {nested: argument}], () => {});
    } else if (mode === 'promise instance') {
      operation = receiver.run([0, {nested: argument}]);
    } else if (mode === 'callback export' || mode === 'rejected export') {
      operation = addon.read([0, {nested: argument}], () => {});
    } else {
      operation = addon.read([0, {nested: argument}]);
    }
    argument = null;
    await collectGarbage();
    assert.equal(releases.includes(7), false);
    if (mode === 'rejected export') {
      const rejection = new Error('native operation failed');
      fail(rejection);
      await assert.rejects(operation, error => error === rejection);
    } else {
      complete(mode === 'callback constructor' ?
          {instance_id: 9, owner_token: 'args-owner'} : 42);
      if (operation) assert.equal(await operation, 42);
    }
    await collectGarbage(24);
    assert.equal(releases.filter(id => id === 7).length, 1);
    // Keep unrelated live wrappers reachable through the end of the check.
    assert.equal(receiver.__instanceId, 8);
    if (constructed) assert.equal(constructed.__instanceId, 9);
  });
}
