// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

const source = fs.readFileSync(path.join(__dirname, 'main/mac_player.js'), 'utf8');
const wireKey = '__xenon_node_wire_type__';
const modulePath = '/fixture/Thunder.app/playercontrol.node';
const plain = value => JSON.parse(JSON.stringify(value));
const method = name => ({name, kind: 'function', enumerable: false, children: []});
const descriptor = {version: 1, adapterId: 'xunlei.mac-player', adapterVersion: 1};
const reply = (result, error = '', acceptedCallbackIds = []) =>
  ({protocolVersion: 1, result, error, acceptedCallbackIds});
const native = (id = 1, extras = {}) => ({[wireKey]: 'native_instance',
  instance_id: id, module_path: modulePath, owner_token: 'owner-generation',
  prototype: [method('play'), method('getMedia')], fields: {}, ...extras});

function load(handler = () => undefined, platform = 'darwin', extraGlobals = {}) {
  const requests = [];
  const errors = [];
  const context = vm.createContext({
    Buffer, __xenonPlatform: platform, electronModule: {},
    captureAsyncCallback: callback => callback,
    console: {error: (...args) => errors.push(args)},
    __xenonBrowserWindowCall: (id, command, request) => {
      assert.equal(id, 0);
      assert.equal(command, 'mac-player');
      requests.push(request);
      if (request.operation === 'capability') return reply(descriptor);
      if (request.operation === 'load') return reply({kind: 'object',
        children: [method('invoke'), {name: 'Stack', kind: 'class'}]});
      if (request.operation === 'describe') {
        if (request.exportPath === 'Stack') return reply({kind: 'class', children: [
          {name: 'prototype', kind: 'object'},
          {name: 'name', kind: 'string', hasValue: true, value: 'Stack'},
        ]});
        if (request.exportPath === 'Stack.prototype') return reply({kind: 'object', children: []});
        return reply(method(request.exportPath));
      }
      const result = handler(request);
      return result?.protocolVersion === 1 ? result :
        reply(result, '', request.callbackIds || []);
    },
    ...extraGlobals,
  });
  vm.runInContext(source, context);
  return {context, get capability() { return context.electronModule.xenonMacPlayer; },
    requests, errors, run: code => vm.runInContext(code, context),
    dispatch: details => context.__xenonDispatchMacPlayerEvent(details)};
}

test('capability is optional on other platforms and when transport is absent', () => {
  assert.equal(load(undefined, 'win32').capability, undefined);
  assert.equal(load(undefined, 'linux').capability, undefined);
  assert.equal(load(undefined, 'darwin', {
    __xenonBrowserWindowCall: undefined,
  }).capability, undefined);
  assert.equal(load().capability.version, 1);
  assert.equal(load().capability.adapterId, 'xunlei.mac-player');
  assert.equal(load().capability.adapterVersion, 1);
  assert.equal(Object.isFrozen(load().capability), true);
});

test('capability discovery is lazy and caches an unsupported trusted bundle', () => {
  let probes = 0;
  const fixture = load(undefined, 'darwin', {
    __xenonBrowserWindowCall: (_id, _command, request) => {
      assert.equal(request.operation, 'capability');
      probes++;
      return reply(null);
    },
  });
  assert.equal(probes, 0);
  assert.equal(fixture.capability, undefined);
  assert.equal(fixture.capability, undefined);
  assert.equal(probes, 1);
});

test('discovery preserves protocol and transport errors and allows retry', () => {
  let probes = 0;
  const fixture = load(undefined, 'darwin', {
    __xenonBrowserWindowCall: () => {
      probes++;
      if (probes === 1) throw new Error('ERR_IPC_DISCONNECTED: offline');
      if (probes === 2) return reply({version: 1});
      return reply(descriptor);
    },
  });
  assert.throws(() => fixture.capability, {code: 'ERR_IPC_DISCONNECTED'});
  assert.throws(() => fixture.capability, {code: 'ERR_INVALID_STATE'});
  assert.equal(fixture.capability.adapterId, descriptor.adapterId);
  assert.equal(fixture.capability.adapterVersion, descriptor.adapterVersion);
  assert.equal(probes, 3);
  assert.equal(fixture.capability, fixture.capability);
  assert.equal(probes, 3);
});

test('load caches the one approved addon and attaches only integer window IDs', () => {
  const fixture = load(() => true);
  const addon = fixture.capability.getAddon(modulePath);
  assert.equal(fixture.capability.getAddon(modulePath), addon);
  assert.equal(fixture.requests.length, 2);
  assert.throws(() => fixture.capability.getAddon('/another.node'));
  for (const invalid of ['', null, 1, 'bad\0path']) {
    assert.throws(() => fixture.capability.getAddon(invalid));
  }
  for (const invalid of [0, -1, 1.2, '1', null, NaN, 2147483648]) {
    assert.throws(() => fixture.capability.attachWindow(invalid));
  }
  fixture.capability.attachWindow(7);
  fixture.capability.detachWindow(7);
  assert.deepEqual(plain(fixture.requests.slice(2)), [
    {operation: 'attach', windowId: 7}, {operation: 'detach', windowId: 7},
  ]);
});

test('native object identity, constructor prototype and reference arguments survive round trips', () => {
  const fixture = load(request => {
    if (request.operation === 'construct') return native();
    if (request.method === 'getMedia') return native(2, {fields: {duration: 123}});
    return native(2);
  });
  const addon = fixture.capability.getAddon(modulePath);
  const stack = new addon.Stack();
  assert.equal(stack instanceof addon.Stack, true);
  const media = stack.getMedia();
  assert.equal(media.duration, 123);
  assert.equal(stack.getMedia(), media);
  assert.equal(stack.play({media}), media);
  const arg = fixture.requests.at(-1).args[0].media;
  assert.deepEqual(plain(arg), {[wireKey]: 'native_instance', instance_id: 2,
    module_path: modulePath, owner_token: 'owner-generation'});
  const detachedMethod = stack.play;
  assert.equal(detachedMethod(), media);
});

test('nested callbacks receive native receiver and one-shot completion functions', () => {
  const fixture = load(request => request.operation === 'invoke-instance' ? 42 : undefined);
  const addon = fixture.capability.getAddon(modulePath);
  let callbackThis;
  let completion;
  addon.invoke({request(_url, done) { callbackThis = this; completion = done; }});
  const callbackId = fixture.requests.at(-1).args[0].request.callback_id;
  fixture.dispatch({type: 'callback', callbackId, receiver: native(9), args: [
    'https://fixture.invalid/', native(10, {[wireKey]: 'native_function'}),
  ]});
  assert.equal(typeof callbackThis.play, 'function');
  assert.throws(() => completion(Symbol('invalid local argument')),
      {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(completion({status: 200}), 42);
  const call = fixture.requests.at(-1);
  assert.equal(call.operation, 'invoke-instance');
  assert.equal(call.instanceId, 10);
  assert.equal(call.token, 'owner-generation');
  assert.equal(call.method, 'call');
  assert.deepEqual(plain(call.args), [{[wireKey]: 'undefined'}, {status: 200}]);
  assert.throws(() => completion({status: 200}), {code: 'ERR_INVALID_STATE'});
  assert.throws(() => addon.invoke(completion), {code: 'ERR_INVALID_STATE'});
  fixture.dispatch({type: 'release-callback', callbackId});
  completion = null;
  fixture.dispatch({type: 'callback', callbackId, receiver: null, args: []});
  assert.equal(completion, null);
});

test('serialization preserves BigInt, special numbers and selected binary bytes', () => {
  const fixture = load(request => request.args[0]);
  fixture.context.addon = fixture.capability.getAddon(modulePath);
  const value = fixture.run(`addon.invoke({
    n: 18446744073709551615n, absent: undefined, nan: NaN, zero: -0,
    bytes: new Uint16Array(new Uint16Array([11, 22, 33]).buffer, 2, 1),
    date: new Date(12345),
  })`);
  assert.equal(value.n, 18446744073709551615n);
  assert.equal(value.absent, undefined);
  assert.equal(Number.isNaN(value.nan), true);
  assert.equal(Object.is(value.zero, -0), true);
  assert.deepEqual(Array.from(value.bytes), [22]);
  assert.equal(value.bytes.buffer.byteLength, 2);
  assert.equal(value.date.getTime(), 12345);
  assert.equal(fixture.requests.at(-1).args[0].bytes.value.byteLength, 2);
});

test('invalid arguments fail before IPC and partial callback registration is discarded', () => {
  const fixture = load();
  fixture.context.addon = fixture.capability.getAddon(modulePath);
  // Resolve lazy exports before comparing request counts.
  fixture.run('addon.invoke');
  const before = fixture.requests.length;
  for (const expression of [
    'Symbol("x")', 'new Set([1])', 'new SharedArrayBuffer(4)',
    '({__xenon_node_wire_type__: "native_instance", instance_id: 1})',
    '(()=>{const value={};value.self=value;return value})()',
    '(()=>{let value={};for(let n=0;n<40;n++)value={value};return value})()',
  ]) {
    assert.throws(() => fixture.run(`addon.invoke(${expression})`));
  }
  fixture.context.called = 0;
  assert.throws(() => fixture.run('addon.invoke([() => called++, Symbol("bad")])'));
  fixture.dispatch({type: 'callback', callbackId: 1, args: [], receiver: null});
  assert.equal(fixture.context.called, 0);
  assert.equal(fixture.requests.length, before);
});

test('reset invalidates old exports, handles, callbacks and deferred finalizers', () => {
  const held = [];
  class FakeFinalizer {
    constructor(callback) { this.callback = callback; }
    register(_target, record) { held.push(() => this.callback(record)); }
  }
  const fixture = load(request => request.operation === 'construct' ? native() : undefined,
      'darwin', {FinalizationRegistry: FakeFinalizer});
  const addon = fixture.capability.getAddon(modulePath);
  const stack = new addon.Stack();
  let called = false;
  addon.invoke(() => { called = true; });
  const callbackId = fixture.requests.at(-1).args[0].callback_id;
  fixture.dispatch({type: 'reset'});
  const before = fixture.requests.length;
  assert.throws(() => stack.play(), {code: 'ERR_INVALID_STATE'});
  assert.throws(() => addon.invoke(), {code: 'ERR_INVALID_STATE'});
  held[0]();
  fixture.dispatch({type: 'callback', callbackId, args: []});
  assert.equal(called, false);
  assert.equal(fixture.requests.length, before);
  assert.notEqual(fixture.capability.getAddon(modulePath), addon);
});

test('handle finalization releases only its exact owner token', () => {
  const held = [];
  class FakeFinalizer {
    constructor(callback) { this.callback = callback; }
    register(_target, record) { held.push(() => this.callback(record)); }
  }
  const fixture = load(request => request.operation === 'construct' ? native() : undefined,
      'darwin', {FinalizationRegistry: FakeFinalizer});
  const addon = fixture.capability.getAddon(modulePath);
  new addon.Stack();
  held[0]();
  assert.deepEqual(plain(fixture.requests.at(-1)), {
    operation: 'release', instanceId: 1, token: 'owner-generation',
  });
});

test('native errors retain their code and callback exceptions do not escape dispatch', () => {
  const fixture = load(request => reply(undefined, 'ERR_ACCESS_DENIED: wrong owner',
      request.callbackIds));
  const addon = fixture.capability.getAddon(modulePath);
  assert.throws(() => addon.invoke(() => { throw new Error('callback error'); }),
      {code: 'ERR_ACCESS_DENIED'});
  const callbackId = fixture.requests.at(-1).args[0].callback_id;
  assert.doesNotThrow(() => fixture.dispatch({type: 'callback', callbackId, args: []}));
  assert.equal(fixture.errors.length, 1);
});

test('subscriptions rejected before native conversion do not exhaust the callback quota', () => {
  let available = false;
  const fixture = load(request => {
    if (request.operation === 'construct') return native(1, {
      prototype: [method('attachProgressChangedEvent')],
    });
    return available ? reply(42, '', request.callbackIds) :
      reply(undefined, 'ERR_INVALID_STATE: native player instance is closed');
  });
  const stack = new (fixture.capability.getAddon(modulePath).Stack)();
  let calls = 0;
  const subscribe = () => stack.attachProgressChangedEvent(() => calls++);
  // Exercise more than the entire callback quota, as an application retrying
  // an event subscription on a stale native instance would do.
  for (let attempt = 0; attempt < 4100; attempt++) {
    assert.throws(subscribe, {code: 'ERR_INVALID_STATE'});
  }
  fixture.dispatch({type: 'callback', callbackId: 1, args: [], receiver: null});
  assert.equal(calls, 0);
  available = true;
  assert.equal(subscribe(), 42);
  const callbackId = fixture.requests.at(-1).callbackIds[0];
  fixture.dispatch({type: 'callback', callbackId, args: [], receiver: null});
  assert.equal(calls, 1);
});

test('successful operations that never convert callbacks release their allocation', () => {
  const fixture = load(() => reply(undefined));
  const addon = fixture.capability.getAddon(modulePath);
  let calls = 0;
  for (let attempt = 0; attempt < 4100; attempt++) addon.invoke(() => calls++);
  const callbackId = fixture.requests.at(-1).callbackIds[0];
  for (const id of [1, callbackId]) {
    fixture.dispatch({type: 'callback', callbackId: id, args: [], receiver: null});
  }
  assert.equal(calls, 0);
});

test('partial conversion retains only acknowledged callbacks after a native exception', () => {
  const fixture = load(request => reply(undefined, 'ERR_NATIVE_FAILURE: retained one',
      [request.callbackIds[1]]));
  const addon = fixture.capability.getAddon(modulePath);
  const called = [];
  assert.throws(() => addon.invoke(() => called.push('unused'),
      () => called.push('retained')), {code: 'ERR_NATIVE_FAILURE'});
  const ids = fixture.requests.at(-1).callbackIds;
  for (const callbackId of ids) {
    fixture.dispatch({type: 'callback', callbackId, args: [], receiver: null});
  }
  assert.deepEqual(called, ['retained']);
  fixture.dispatch({type: 'release-callback', callbackId: ids[1]});
  fixture.dispatch({type: 'callback', callbackId: ids[1], args: [], receiver: null});
  assert.deepEqual(called, ['retained']);
});

test('a missing transport reply cannot release a callback retained by native code', () => {
  const fixture = load(() => { throw new Error('ERR_IPC_DISCONNECTED: reply lost'); });
  const addon = fixture.capability.getAddon(modulePath);
  let calls = 0;
  assert.throws(() => addon.invoke(() => calls++), {code: 'ERR_IPC_DISCONNECTED'});
  const callbackId = fixture.requests.at(-1).callbackIds[0];
  fixture.dispatch({type: 'callback', callbackId, args: [], receiver: null});
  assert.equal(calls, 1);
});

test('malformed acknowledgements do not partially release allocated callbacks', () => {
  for (const invalidReply of [
    request => reply(undefined, '', [request.callbackIds[0], request.callbackIds[0]]),
    request => reply(undefined, '', [request.callbackIds[0], 123456]),
    () => reply(undefined, '', null),
    () => reply(undefined, null),
    () => ({protocolVersion: 1, error: '', acceptedCallbackIds: []}),
  ]) {
    const fixture = load(invalidReply);
    const addon = fixture.capability.getAddon(modulePath);
    let calls = 0;
    assert.throws(() => addon.invoke(() => calls++, () => calls++),
        {code: 'ERR_INVALID_STATE'});
    for (const callbackId of fixture.requests.at(-1).callbackIds) {
      fixture.dispatch({type: 'callback', callbackId, args: [], receiver: null});
    }
    assert.equal(calls, 2);
  }
});

test('the complete main bootstrap routes only owner-targeted player events', () => {
  let callbackId;
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'darwin', __xenonArch: 'arm64', __xenonEndianness: 'LE',
    __xenonOsRelease: 'fixture', __xenonAppPath: '/fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: '/fixture/host', __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonBrowserWindowCall: (id, command, request) => {
      assert.equal(id, 0);
      assert.equal(command, 'mac-player');
      if (request.operation === 'capability') return reply(descriptor);
      if (request.operation === 'load') return reply({kind: 'object',
        children: [method('invoke')]});
      if (request.operation === 'describe') return reply(method('invoke'));
      callbackId = request.args[0].callback_id;
      return reply(undefined, '', request.callbackIds);
    },
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      'xenon_ipc_main_bootstrap.js')), context);
  let received;
  context.__xenonElectron.xenonMacPlayer.getAddon(modulePath).invoke(value => {
    received = value;
  });
  const details = {type: 'callback', callbackId, args: [42], receiver: null};
  context.__xenonDispatchBrowserWindowEvent(999, '__xenon-mac-player-event', details);
  assert.equal(received, undefined);
  context.__xenonDispatchBrowserWindowEvent(0, '__xenon-mac-player-event', details);
  assert.equal(received, 42);
});

test('application shutdown permits native player cleanup while rejecting new windows', () => {
  const requests = [];
  const releases = [];
  class FakeFinalizer {
    constructor(callback) { this.callback = callback; }
    register(_target, record) { releases.push(() => this.callback(record)); }
  }
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    FinalizationRegistry: FakeFinalizer,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'darwin', __xenonArch: 'arm64', __xenonEndianness: 'LE',
    __xenonOsRelease: 'fixture', __xenonAppPath: '/fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: '/fixture/host', __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonBrowserWindowCall: (id, command, request) => {
      assert.equal(id, 0);
      assert.equal(command, 'mac-player');
      requests.push(plain(request));
      if (request.operation === 'capability') return reply(descriptor);
      if (request.operation === 'load') return reply({kind: 'object', children: [
        {name: 'Stack', kind: 'class'}, method('unInitAddon'),
      ]});
      if (request.operation === 'describe') {
        return reply(request.exportPath === 'Stack' ?
            {kind: 'class', children: []} : method(request.exportPath));
      }
      if (request.operation === 'construct') {
        return reply(native(1, {prototype: [method('closeMedia')]}));
      }
      return reply(undefined);
    },
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      'xenon_ipc_main_bootstrap.js')), context);
  const {app, BrowserWindow, xenonMacPlayer} = context.__xenonElectron;
  const addon = xenonMacPlayer.getAddon(modulePath);
  const stack = new addon.Stack();
  const phases = [];
  app.on('before-quit', () => {
    phases.push('before-quit');
    assert.throws(() => new BrowserWindow(), {code: 'ERR_APP_QUITTING'});
    stack.closeMedia();
    xenonMacPlayer.detachWindow(7);
  });
  app.on('will-quit', () => {
    phases.push('will-quit');
    addon.unInitAddon();
    releases[0]();
  });
  app.on('quit', () => phases.push('quit'));

  requests.length = 0;
  context.__xenonShutdownApp();
  assert.deepEqual(phases, ['before-quit', 'will-quit', 'quit']);
  assert.deepEqual(requests.filter(request => request.operation !== 'describe'), [
    {operation: 'invoke-instance', instanceId: 1, token: 'owner-generation',
      method: 'closeMedia', args: []},
    {operation: 'detach', windowId: 7},
    {operation: 'invoke', exportPath: 'unInitAddon', args: []},
    {operation: 'release', instanceId: 1, token: 'owner-generation'},
  ]);
  assert.throws(() => new BrowserWindow(), {code: 'ERR_APP_QUITTING'});
  context.__xenonShutdownApp();
  assert.deepEqual(phases, ['before-quit', 'will-quit', 'quit']);
});
