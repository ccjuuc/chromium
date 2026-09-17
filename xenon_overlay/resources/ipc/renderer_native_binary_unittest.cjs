// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const modulePath = 'C:\\test-app\\binary.node';
const source = readBootstrap(process.env.XENON_TEST_BOOTSTRAP ||
    path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js'));
const binaryTag = (kind, value) => ({__xenon_node_wire_type__: 'binary', kind, value});
const bytes = value => Array.from(new Uint8Array(
    ArrayBuffer.isView(value) ? value.buffer : value,
    ArrayBuffer.isView(value) ? value.byteOffset : 0,
    value.byteLength));

function renderer(overrides = {}, exposeMojo = false) {
  let dispatch;
  const calls = [], errors = [];
  const transport = {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe'}),
    setDispatchHandler(callback) { dispatch = callback; },
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat' && request.path === modulePath)
        return {isFile: true, isDirectory: false};
      throw new Error('ENOENT');
    },
    requireNodeModuleSync: () => [
      {name: 'echo', kind: 'function'},
      {name: 'Worker', kind: 'class', prototype: [{name: 'run', kind: 'function'}]},
    ],
    constructNodeExportSync: () => ({instance_id: 1, owner_token: 'binary-owner'}),
    invokeNodeExportSync(_module, _method, ...args) { calls.push(args); return args[0]; },
    invokeNodeInstanceSync(_module, _selector, _method, ...args) { calls.push(args); return args[0]; },
    invoke(_channel, request) { calls.push(request.arguments); return Promise.resolve(request.arguments[0]); },
    ...overrides,
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    xenonIpcRenderer: transport, TextEncoder, TextDecoder, URL, URLSearchParams,
    queueMicrotask, atob, btoa, setTimeout, clearTimeout,
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error(...args) { errors.push(args); }},
  });
  // Legacy Mojo conversion has no current public caller. Expose its real
  // closures only inside this VM fixture; the production bootstrap is intact.
  const runnable = exposeMojo ? source.replace(/\}\)\(\);\s*$/, `
    globalThis.__binaryMojoTest = {valueToMojo, valueFromMojo,
      toMojoInvokeArgs, toMojoInvokeArgsAsync, adoptNativeReturn,
      attachMojoListeners, pendingMojoInvokes};
  })();`) : source;
  vm.runInContext(runnable, context);
  return {context, calls, errors, transport, addon: context.require(modulePath),
    dispatch: (...args) => dispatch(...args)};
}

function countArgumentMaps(r) {
  vm.runInContext(`
    globalThis.argumentMapCount = 0;
    globalThis.Map = class CountedMap extends Map {
      constructor(...args) { super(...args); ++globalThis.argumentMapCount; }
    };
  `, r.context);
}

for (const asynchronous of [false, true]) {
  test(`native ${asynchronous ? 'async' : 'sync'} leaf arguments need no object traversal maps`, async () => {
    const r = renderer(asynchronous ? {
      invokeNodeExportSync: undefined, invokeNodeInstanceSync: undefined,
    } : {});
    const worker = new r.addon.Worker();
    countArgumentMaps(r);
    const input = [7, 'public fixture', true, null, undefined, 3n,
      new Uint8Array([1, 2]), worker, () => {}];
    assert.equal(await r.addon.echo(...input), 7);
    assert.equal(await worker.run(...input), 7);
    assert.equal(r.context.argumentMapCount, 0);
    assert.equal(r.calls.length, 2);
    for (const args of r.calls) {
      assert.equal(args[6].kind, 'Uint8Array');
      assert.equal(args[7].__xenon_node_wire_type__, 'native_instance');
      assert.equal(args[8].__xenon_node_wire_type__, 'callback');
    }
  });

  test(`native ${asynchronous ? 'async' : 'sync'} object arguments retain independent traversal state`, async () => {
    const calls = [];
    const r = renderer(asynchronous ? {
      invokeNodeExportSync: undefined,
      invoke(_channel, request) { calls.push(request.arguments); return Promise.resolve(); },
    } : {
      invokeNodeExportSync(_module, _method, ...args) { calls.push(args); },
    });
    countArgumentMaps(r);
    const shared = {value: 42};
    const input = {left: shared, right: shared};
    input.self = input;
    await r.addon.echo(input, input);
    assert.equal(r.context.argumentMapCount, 2);
    const [first, second] = calls[0];
    assert.notEqual(first, second);
    for (const value of [first, second]) {
      assert.equal(value.self, value);
      assert.equal(value.left, value.right);
      assert.equal(value.left.value, 42);
    }
  });
}

test('renderer native arguments preserve every binary kind and active range', () => {
  const r = renderer();
  const values = vm.runInContext(`(() => {
    const backing = new ArrayBuffer(24);
    new Uint8Array(backing).set(Array.from({length: 24}, (_, i) => i + 1));
    const constructors = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array,
      Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array,
      BigInt64Array, BigUint64Array];
    if (typeof Float16Array === 'function') constructors.push(Float16Array);
    return [
      ['Buffer', Buffer.from(backing, 3, 5)],
      ['ArrayBuffer', backing], ['DataView', new DataView(backing, 5, 9)],
      ...constructors.map(C => [C.name, new C(backing, 8, 2)]),
    ];
  })()`, r.context);
  for (const [kind, input] of values) {
    const expected = bytes(input);
    const output = r.addon.echo(input);
    const wire = r.calls.at(-1)[0];
    assert.equal(wire.__xenon_node_wire_type__, 'binary');
    assert.equal(wire.kind, kind);
    assert.ok(ArrayBuffer.isView(wire.value));
    assert.deepEqual(Object.keys(wire).sort(), ['__xenon_node_wire_type__', 'kind', 'value']);
    assert.deepEqual(bytes(wire.value), expected);
    assert.equal(wire.value.byteOffset, 0);
    assert.equal(wire.value.buffer.byteLength, expected.length);
    assert.deepEqual(bytes(output), expected);
    assert.equal(kind === 'Buffer' ? r.context.Buffer.isBuffer(output) :
      Object.prototype.toString.call(output).slice(8, -1), kind === 'Buffer' ? true : kind);
    assert.notEqual(ArrayBuffer.isView(input) ? input.buffer : input,
      ArrayBuffer.isView(output) ? output.buffer : output);
  }
});

test('empty binary values remain binary in exports, instance calls and nested objects', () => {
  const r = renderer();
  const values = vm.runInContext(`[
    Buffer.alloc(0), new ArrayBuffer(0), new DataView(new ArrayBuffer(0)),
    new Uint8Array(0), new Uint32Array(0), new BigInt64Array(0),
  ]`, r.context);
  const worker = new r.addon.Worker();
  for (const value of values) {
    const result = worker.run({payload: [value]});
    assert.equal(result.payload[0].byteLength, 0);
    assert.equal(r.calls.at(-1)[0].payload[0].__xenon_node_wire_type__, 'binary');
    assert.equal(result.payload[0].constructor, value.constructor);
  }
});

test('binary detection uses intrinsic getters across realms and ignores spoofed properties', () => {
  const r = renderer();
  const foreign = vm.runInNewContext(`(() => {
    const view = new Uint16Array(new Uint8Array([91, 92, 1, 2, 3, 4, 93, 94]).buffer, 2, 2);
    for (const property of ['buffer', 'byteOffset', 'byteLength', 'constructor'])
      Object.defineProperty(view, property, {get() { throw Error('must use intrinsic'); }});
    Object.defineProperty(view, Symbol.toStringTag, {value: 'Float64Array'});
    return view;
  })()`);
  const result = r.addon.echo(foreign);
  assert.equal(r.calls[0][0].kind, 'Uint16Array');
  assert.deepEqual(bytes(result), [1, 2, 3, 4]);
  const foreignBuffer = vm.runInNewContext('new Uint8Array([4, 5, 6]).buffer');
  assert.deepEqual(bytes(r.addon.echo(foreignBuffer)), [4, 5, 6]);
  const fake = {constructor: {name: 'Uint8Array'}, byteLength: 3, [Symbol.toStringTag]: 'Uint8Array'};
  r.addon.echo(fake);
  assert.equal(r.calls.at(-1)[0].__xenon_node_wire_type__, undefined);
});

test('async addon calls snapshot later nested binary fields before unresolved handles', async () => {
  let resolveConstructor;
  const r = renderer({invokeNodeExportSync: undefined, constructNodeExportSync: undefined,
    invoke(channel, request) {
      if (channel.endsWith('construct-export'))
        return new Promise(resolve => { resolveConstructor = resolve; });
      r.calls.push(request.arguments);
      return Promise.resolve(request.arguments[0]);
    }});
  const worker = new r.addon.Worker();
  await new Promise(resolve => setImmediate(resolve));
  const payload = new Uint8Array([1, 2, 3]);
  const arrayPayload = new Uint8Array([4, 5]);
  const otherArg = new Uint8Array([6]);
  const pending = r.addon.echo({worker, payload, array: [worker, arrayPayload]}, otherArg);
  payload.fill(99); arrayPayload.fill(98); otherArg.fill(97);
  resolveConstructor({instance_id: 2, owner_token: 'async-owner'});
  const result = await pending;
  assert.deepEqual(bytes(result.payload), [1, 2, 3]);
  assert.deepEqual(bytes(result.array[1]), [4, 5]);
  assert.deepEqual(bytes(r.calls[0][1].value), [6]);
  assert.equal(r.calls[0][0].worker.__xenon_node_wire_type__, 'native_instance');
});

test('native callback arguments and receivers restore binary tags without object enumeration', () => {
  const r = renderer({invokeNodeExportSync(_module, _method, callback) {
    r.calls.push(callback); return undefined;
  }});
  let result;
  r.addon.echo(function(value) { result = {value, receiver: this}; });
  const descriptor = r.calls[0];
  r.dispatch('__xenon:node-addon:callback', [descriptor.callback_id,
    [{payload: binaryTag('Buffer', new Uint8Array([1, 2]))}],
    binaryTag('Uint16Array', new Uint8Array([3, 4]))]);
  assert.ok(r.context.Buffer.isBuffer(result.value.payload));
  assert.deepEqual(bytes(result.value.payload), [1, 2]);
  assert.equal(Object.prototype.toString.call(result.receiver), '[object Uint16Array]');
  assert.deepEqual(bytes(result.receiver), [3, 4]);
  assert.deepEqual(r.errors, []);
});

test('async instance methods snapshot binary arguments before waiting for their receiver', async () => {
  let resolveConstructor;
  const r = renderer({constructNodeExportSync: undefined,
    invokeNodeInstanceSync: undefined,
    invoke(channel, request) {
      if (channel.endsWith('construct-export'))
        return new Promise(resolve => { resolveConstructor = resolve; });
      r.calls.push(request.arguments);
      return Promise.resolve(request.arguments[0]);
    }});
  const worker = new r.addon.Worker();
  await new Promise(resolve => setImmediate(resolve));
  const input = new Uint8Array([3, 4, 5]);
  const pending = worker.run(input);
  input.fill(99);
  resolveConstructor({instance_id: 4, owner_token: 'pending-receiver'});
  assert.deepEqual(bytes(await pending), [3, 4, 5]);
});

test('raw native ArrayBuffer and views remain unchanged during return adoption', () => {
  let response;
  const r = renderer({invokeNodeExportSync() { return {response}; }});
  for (const raw of [new Uint8Array([1, 2]), new DataView(new ArrayBuffer(2)), new ArrayBuffer(0)]) {
    response = raw;
    assert.equal(r.addon.echo().response, raw);
  }
});

test('invalid native binary kinds, payloads and element lengths fail explicitly', async () => {
  const invalid = [
    binaryTag('UnknownArray', new Uint8Array(0)),
    binaryTag('__proto__', new Uint8Array(0)),
    binaryTag('Uint16Array', new Uint8Array(3)),
    binaryTag('Float64Array', new Uint8Array(7)),
    binaryTag('Buffer', [1, 2]),
    binaryTag('Buffer', new Uint16Array(2)),
    binaryTag('ArrayBuffer', {0: 1, byteLength: 1}),
    binaryTag('Uint8Array', null),
  ];
  for (const value of invalid) {
    const r = renderer({invokeNodeExportSync: () => value});
    assert.throws(() => r.addon.echo(), /invalid binary/);
    const asyncRenderer = renderer({invokeNodeExportSync: undefined,
      invoke: () => Promise.resolve(value)});
    await assert.rejects(asyncRenderer.addon.echo(), /invalid binary/);
  }
});

test('detached buffers and bare shared buffers do not silently become ordinary objects', async () => {
  const r = renderer();
  const detached = new ArrayBuffer(4);
  const view = new Uint8Array(detached);
  structuredClone(detached, {transfer: [detached]});
  assert.throws(() => r.addon.echo(detached), /detached/i);
  assert.throws(() => r.addon.echo(view), /detached/i);
  assert.throws(() => r.addon.echo(new SharedArrayBuffer(3)), /SharedArrayBuffer/);
  assert.deepEqual(bytes(r.addon.echo(new Uint8Array(new SharedArrayBuffer(3)))), [0, 0, 0]);
  const a = renderer({invokeNodeExportSync: undefined});
  await assert.rejects(a.addon.echo(detached), /detached/i);
});

test('Mojo serializes payload bytes in binaryValue and validates received bytes', async () => {
  const r = renderer({}, true);
  const mojo = r.context.__binaryMojoTest;
  assert.ok(mojo);
  const input = new Uint8Array([90, 1, 2, 3, 91]).subarray(1, 4);
  const wired = mojo.toMojoInvokeArgs([input])[0].value.dictionaryValue.storage;
  assert.equal(wired.kind.stringValue, 'Uint8Array');
  assert.deepEqual(bytes(wired.value.binaryValue), [1, 2, 3]);
  assert.equal(wired.value.dictionaryValue, undefined);
  const restored = mojo.adoptNativeReturn(mojo.valueFromMojo({dictionaryValue: {storage: wired}}));
  assert.deepEqual(bytes(restored), [1, 2, 3]);
  assert.deepEqual(bytes(mojo.valueFromMojo({binaryValue: []})), []);
  assert.deepEqual(bytes(mojo.valueFromMojo({binaryValue: [0, 255]})), [0, 255]);
  for (const invalid of [[-1], [256], [0.5], ['1'], {}, null]) {
    if (invalid === null) continue; // An unset Mojo union field is not a binary value.
    assert.throws(() => mojo.valueFromMojo({binaryValue: invalid}), /invalid Mojo binary/);
  }
  const pending = mojo.toMojoInvokeArgsAsync([Promise.resolve(1), input]);
  input.fill(77);
  assert.deepEqual(bytes((await pending)[1].value.dictionaryValue.storage.value.binaryValue), [1, 2, 3]);
});

test('Mojo invoke-result callbacks adopt binary tags into renderer Buffer values', () => {
  const r = renderer({}, true);
  const mojo = r.context.__binaryMojoTest;
  let callbackResult;
  const callback = mojo.toMojoInvokeArgs([value => { callbackResult = value; }])[0];
  let onResult;
  mojo.attachMojoListeners({
    nodeInvokeResult: {addListener(listener) { onResult = listener; }},
    nodeCallbackInvoked: {addListener() {}},
    nodeConstructResult: {addListener() {}},
  });
  let resolved = false;
  mojo.pendingMojoInvokes.set(7, {
    resolve() { resolved = true; }, reject(error) { throw error; },
  });
  onResult(7, true, {nullValue: 0}, [{callbackId: callback.callbackId,
    value: mojo.valueToMojo(binaryTag('Buffer', new Uint8Array([8, 9])))}]);
  assert.equal(resolved, true);
  assert.ok(r.context.Buffer.isBuffer(callbackResult));
  assert.deepEqual(bytes(callbackResult), [8, 9]);
});
