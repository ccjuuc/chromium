// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const {isDeepStrictEqual} = require('node:util');
const vm = require('node:vm');

function createRuntime(host) {
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '10.0', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  if (host) context.__xenonRegisterSchemesAsPrivileged = host;
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readBootstrap(filename), context, {filename});
  return {context, ...context.__xenonElectron};
}

function declaration() {
  return [{scheme: 'fixture-view', privileges: {
    standard: true, secure: true, supportFetchAPI: true, corsEnabled: true,
  }}];
}

test('each privileged scheme registration validates the original declaration with the host', () => {
  const calls = [];
  const {protocol} = createRuntime(value => calls.push(value));
  const schemes = declaration();
  Object.freeze(schemes[0].privileges);
  Object.freeze(schemes[0]);
  Object.freeze(schemes);
  assert.equal(protocol.registerSchemesAsPrivileged(schemes), undefined);
  assert.equal(calls.length, 1);
  assert.equal(calls[0], schemes);
  assert.equal(protocol.registerSchemesAsPrivileged(schemes), undefined);
  assert.equal(calls.length, 2);
  assert.equal(calls[1], schemes);
});

test('independent modules can register crx and miniapp before readiness', () => {
  const calls = [];
  const startup = require('../thunder_2025.xenon.json').privilegedSchemes;
  const {context, protocol} = createRuntime(value => {
    for (const requested of value) {
      const declared = startup.find(entry => entry.scheme === requested.scheme);
      assert.deepEqual(requested, declared, 'request must match the startup descriptor');
    }
    calls.push(value);
  });
  const crx = [{scheme: 'crx', privileges: {bypassCSP: true}}];
  const miniapp = [{scheme: 'miniapp', privileges: {
    standard: true, secure: true, supportFetchAPI: true, corsEnabled: true,
  }}];
  protocol.registerSchemesAsPrivileged(crx);
  protocol.registerSchemesAsPrivileged(miniapp);
  assert.deepEqual(calls, [crx, miniapp]);
  context.__xenonMarkAppReady();
  assert.throws(() => protocol.registerSchemesAsPrivileged(crx),
      /before app is ready/);
  assert.equal(calls.length, 2);
});

test('scheme registration rejects invalid arguments without consuming registration', () => {
  const calls = [];
  const {protocol} = createRuntime(value => calls.push(value));
  for (const invalid of [
    undefined, null, {}, 'fixture-view', [null], [[]], [{}], [{scheme: 4}],
    [{scheme: ''}], [{scheme: '1fixture'}], [{scheme: 'fixture:'}],
    [{scheme: 'fixture/view'}], new Array(1),
    [{scheme: 'fixture', privileges: null}],
    [{scheme: 'fixture', privileges: []}],
    [{scheme: 'fixture', privileges: {secure: 'true'}}],
    [{scheme: 'fixture', privileges: {unknownCapability: true}}],
    [{scheme: 'fixture'}, {scheme: 'FIXTURE'}],
  ]) {
    assert.throws(() => protocol.registerSchemesAsPrivileged(invalid),
        {name: 'TypeError'});
  }
  assert.equal(calls.length, 0);
  const schemes = [{scheme: 'fixture+archive.v2'}, {scheme: 'fixture-empty', privileges: {}}];
  protocol.registerSchemesAsPrivileged(schemes);
  assert.equal(calls[0], schemes);
});

test('scheme registration fails explicitly when the native host is unavailable', () => {
  const {context, protocol} = createRuntime();
  assert.throws(() => protocol.registerSchemesAsPrivileged(declaration()),
      {code: 'ERR_NOT_SUPPORTED'});
  let calls = 0;
  context.__xenonRegisterSchemesAsPrivileged = () => ++calls;
  protocol.registerSchemesAsPrivileged(declaration());
  assert.equal(calls, 1);
});

test('host rejection propagates unchanged and leaves registration retryable', () => {
  const failure = new Error('Declared privileges do not match startup configuration');
  let calls = 0;
  const {protocol} = createRuntime(() => {
    if (++calls === 1) throw failure;
  });
  assert.throws(() => protocol.registerSchemesAsPrivileged(declaration()),
      error => error === failure);
  protocol.registerSchemesAsPrivileged(declaration());
  assert.equal(calls, 2);
  protocol.registerSchemesAsPrivileged(declaration());
  assert.equal(calls, 3);
});

test('later registration still propagates host rejection of conflicting privileges', () => {
  const failure = new Error('Scheme privileges were not declared at browser startup');
  const calls = [];
  const allowed = [{scheme: 'crx', privileges: {bypassCSP: true}}];
  const frozenStartup = structuredClone(allowed);
  const {protocol} = createRuntime(value => {
    calls.push(value);
    for (const requested of value) {
      const declared = frozenStartup.find(entry => entry.scheme === requested.scheme);
      if (!isDeepStrictEqual(requested, declared)) throw failure;
    }
  });
  const conflicting = [{scheme: 'crx', privileges: {bypassCSP: false}}];
  const undeclared = [{scheme: 'other-crx', privileges: {bypassCSP: true}}];
  protocol.registerSchemesAsPrivileged(allowed);
  assert.throws(() => protocol.registerSchemesAsPrivileged(conflicting),
      error => error === failure);
  assert.throws(() => protocol.registerSchemesAsPrivileged(undeclared),
      error => error === failure);
  protocol.registerSchemesAsPrivileged(allowed);
  assert.deepEqual(calls, [allowed, conflicting, undeclared, allowed]);
});

test('ready event listeners cannot register privileged schemes', () => {
  let calls = 0;
  const {context, app, protocol} = createRuntime(() => ++calls);
  app.once('ready', () => {
    assert.equal(app.isReady(), true);
    assert.throws(() => protocol.registerSchemesAsPrivileged(declaration()),
        /before app is ready/);
  });
  context.__xenonMarkAppReady();
  assert.throws(() => protocol.registerSchemesAsPrivileged(declaration()),
      /before app is ready/);
  assert.equal(calls, 0);
});

test('the global protocol delegates default-session handlers only after readiness', async () => {
  const {context, protocol, session} = createRuntime(() => {});
  const handlers = session.defaultSession.protocol;
  const otherHandlers = session.fromPartition('persist:other').protocol;
  assert.notEqual(protocol, handlers);
  assert.notEqual(handlers, otherHandlers);
  assert.equal(handlers.registerSchemesAsPrivileged, undefined);
  assert.equal(protocol.registerBufferProtocol, undefined);
  assert.equal('registerBufferProtocol' in protocol, false);
  assert.equal(await Promise.resolve(protocol), protocol);
  protocol.registerSchemesAsPrivileged(declaration());
  context.__xenonMarkAppReady();
  assert.equal('registerBufferProtocol' in protocol, true);
  assert.equal(protocol.unknownCapability, undefined);
  assert.equal(await Promise.resolve(protocol), protocol);
  for (const name of Object.keys(handlers).filter(name =>
      !['handle', 'unhandle', 'isProtocolHandled'].includes(name))) {
    for (const target of [protocol, handlers, otherHandlers]) {
      assert.throws(() => target[name]('fixture-view', () => {}),
          {code: 'ERR_NOT_SUPPORTED'});
    }
  }
  const args = ['fixture-view', () => {}];
  const expected = {};
  handlers.registerBufferProtocol = function(...actual) {
    assert.equal(this, handlers);
    assert.deepEqual(actual, args);
    return expected;
  };
  const detached = protocol.registerBufferProtocol;
  assert.equal(detached(...args), expected);
  assert.throws(() => otherHandlers.registerBufferProtocol(...args),
      {code: 'ERR_NOT_SUPPORTED'});
});

function handledRuntime() {
  const runtime = createRuntime(() => {});
  runtime.calls = [];
  runtime.context.__xenonBrowserWindowCall = (id, command, details) => {
    assert.equal(id, 0);
    assert.equal(command, 'protocol-handler');
    runtime.calls.push(structuredClone(details));
  };
  runtime.protocol.registerSchemesAsPrivileged([{scheme: 'fixture-view'}]);
  runtime.context.__xenonMarkAppReady();
  return runtime;
}

test('protocol requests preserve binary responses, status and headers within their Session', async () => {
  const {context, calls, protocol, session} = handledRuntime();
  vm.runInContext(`__xenonElectron.protocol.handle('fixture-view', request => {
    if (request.url !== 'fixture-view://icon/a' || request.method !== 'GET') throw Error('bad request');
    return new Response(new Uint8Array([0, 255, 128]), {status: 201, headers: {'Content-Type': 'image/png'}});
  });`, context);
  assert.equal(protocol.isProtocolHandled('FIXTURE-VIEW'), true);
  assert.equal(session.fromPartition('persist:other').protocol.isProtocolHandled('fixture-view'), false);
  const result = await context.__xenonDispatchProtocol('', 'request-1',
      {url: 'fixture-view://icon/a', method: 'GET', headers: []});
  assert.equal(result.status, 201);
  assert.deepEqual([...result.body], [0, 255, 128]);
  assert.deepEqual(JSON.parse(JSON.stringify(result.headers)), [['content-type', 'image/png']]);
  await assert.rejects(context.__xenonDispatchProtocol('persist:other', 'request-2',
      {url: 'fixture-view://icon/a', method: 'GET', headers: []}), /no handler/);
  protocol.unhandle('fixture-view');
  assert.equal(protocol.isProtocolHandled('fixture-view'), false);
  assert.deepEqual(calls.map(call => call.handled), [true, false]);
  await assert.rejects(context.__xenonDispatchProtocol('', 'request-3',
      {url: 'fixture-view://icon/a', method: 'GET', headers: []}), /no handler/);
});

test('protocol registration is transactional and rejects undeclared schemes and duplicates', () => {
  const {context, protocol} = handledRuntime();
  assert.throws(() => protocol.handle('https', () => {}), /not declared/);
  context.__xenonBrowserWindowCall = () => { throw new Error('host rejected'); };
  assert.throws(() => protocol.handle('fixture-view', () => {}), /host rejected/);
  assert.equal(protocol.isProtocolHandled('fixture-view'), false);
  context.__xenonBrowserWindowCall = () => {};
  protocol.handle('fixture-view', () => {});
  assert.throws(() => protocol.handle('fixture-view', () => {}), /already handled/);
});

test('protocol handler errors and non-Response results fail explicitly', async () => {
  const {context, protocol} = handledRuntime();
  for (const body of ['throw new Error("handler failed")', 'return {status:200}']) {
    vm.runInContext(`__xenonElectron.protocol.handle('fixture-view', () => {${body}})`, context);
    await assert.rejects(context.__xenonDispatchProtocol('', body,
        {url: 'fixture-view://icon/a', method: 'GET', headers: []}), /handler failed|must return a Response/);
    protocol.unhandle('fixture-view');
  }
});

test('cancelling a protocol request aborts its signal and releases its identity', async () => {
  const {context} = handledRuntime();
  vm.runInContext(`__xenonElectron.protocol.handle('fixture-view', request => {
    globalThis.protocolSignal = request.signal;
    return new Promise(() => {});
  });`, context);
  const result = context.__xenonDispatchProtocol('', 'cancel-me',
      {url: 'fixture-view://icon/a', method: 'GET', headers: []});
  await Promise.resolve();
  context.__xenonCancelProtocol('cancel-me');
  await assert.rejects(result, /aborted/);
  assert.equal(context.protocolSignal.aborted, true);
  // Cancellation is idempotent even after the pending operation was removed.
  context.__xenonCancelProtocol('cancel-me');
});

test('protocol response metadata and body bounds fail before crossing IPC', async () => {
  const {context, protocol} = handledRuntime();
  for (const [body, expected] of [
    ['const response = new Response(); response.statusText = "bad\\r\\nheader"; return response', /Invalid protocol response status/],
    ['return new Response(null, {headers: {"x-large": "x".repeat(65537)}})', /headers exceed/],
    ['return new Response(new Uint8Array(32 * 1024 * 1024 + 1))', /32 MiB/],
  ]) {
    vm.runInContext(`__xenonElectron.protocol.handle('fixture-view', () => {${body}})`, context);
    await assert.rejects(context.__xenonDispatchProtocol('', 'bounds',
        {url: 'fixture-view://icon/a', method: 'GET', headers: []}), expected);
    protocol.unhandle('fixture-view');
  }
});
