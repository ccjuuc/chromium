// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

const source = fs.readFileSync(path.join(__dirname, 'main/web_request.js'), 'utf8');
const plain = value => JSON.parse(JSON.stringify(value));
function fixture(call) {
  const registrations = [];
  const timers = new Map();
  let timerId = 0;
  const context = vm.createContext({
    setTimeout: callback => { timers.set(++timerId, callback); return timerId; },
    clearTimeout: id => timers.delete(id),
    __xenonBrowserWindowCall: (windowId, command, details) => {
      assert.equal(windowId, 0);
      assert.equal(command, 'web-request-handler');
      registrations.push(plain(details));
      return call?.(details);
    },
  });
  vm.runInContext(source, context);
  return {
    context, registrations, timers,
    session: partition => context.createSessionWebRequest(partition),
    dispatch: (registration = registrations.at(-1), details = {}, requestId = 'request-1') =>
      context.__xenonDispatchWebRequest({partition: registration.partition,
        event: registration.event, registrationId: registration.registrationId, requestId, details}),
    release: id => context.__xenonReleaseWebRequestRegistration(id),
    cancel: id => context.__xenonCancelWebRequest(id),
    shutdown: () => context.__xenonShutdownWebRequests(),
  };
}

test('registration forwards Session and copied filters for native URLPattern validation', () => {
  const f = fixture();
  const request = f.session('persist:one');
  const filter = {urls: ['file://*', 'file://*'], excludeUrls: ['file:///private/*'], types: ['script']};
  assert.equal(request.onBeforeRequest(filter, () => {}), undefined);
  filter.urls.push('https://later.test/*');
  assert.deepEqual(f.registrations[0], {partition: 'persist:one', event: 'onBeforeRequest',
    registrationId: '1', enabled: true,
    filter: {urls: ['file://*'], excludeUrls: ['file:///private/*'], types: ['script']}});
  request.onHeadersReceived(() => {});
  assert.deepEqual(f.registrations[1].filter, {urls: ['<all_urls>'], excludeUrls: [], types: []});
  request.onBeforeRequest({urls: []}, () => {});
  assert.deepEqual(f.registrations[2].filter.urls, ['<all_urls>']);
});

test('a listener returning undefined or a fulfilled Promise waits for its callback', async () => {
  for (const returned of [undefined, Promise.resolve({cancel: true})]) {
    const f = fixture();
    let callback;
    f.session('').onBeforeRequest((_details, next) => { callback = next; return returned; });
    let settled = false;
    const pending = f.dispatch().then(value => { settled = true; return value; });
    await Promise.resolve(); await Promise.resolve();
    assert.equal(settled, false);
    callback({cancel: true});
    assert.deepEqual(plain(await pending), {cancel: true});
    assert.equal(f.timers.size, 0);
  }
});

test('responses preserve actual header arrays and snapshot changes at callback time', async () => {
  const f = fixture();
  const headers = {'Content-Security-Policy': ["script-src 'none'"], 'Set-Cookie': ['a=1', 'b=2']};
  f.session('').onHeadersReceived((details, callback) => {
    assert.equal(details.statusCode, 200);
    callback({responseHeaders: {...details.responseHeaders, ...headers}, statusLine: 'HTTP/1.1 201 Created'});
    headers['Set-Cookie'].push('c=3');
    callback({cancel: true});
  });
  const result = await f.dispatch(undefined, {statusCode: 200, responseHeaders: {
    'X-Original': ['kept'], 'content-security-policy': ["script-src 'self'"]}});
  assert.deepEqual(plain(result), {responseHeaders: {'x-original': ['kept'],
    'content-security-policy': ["script-src 'none'"], 'set-cookie': ['a=1', 'b=2']},
    statusLine: 'HTTP/1.1 201 Created'});
  assert.equal(f.timers.size, 0);
});

test('request headers wait for callbacks and copy replacements using case-insensitive last spelling', async () => {
  const f = fixture();
  const api = f.session('persist:request-headers');
  const headers = {'X-Kept': 'original', 'X-Replace': 'old', Cookie: 'a=1; b=2'};
  let complete;
  api.onBeforeSendHeaders({urls: ['https://headers.test/*']}, (details, callback) => {
    assert.deepEqual(plain(details.requestHeaders), headers);
    complete = callback;
    return Promise.resolve();
  });
  assert.equal(f.registrations[0].event, 'onBeforeSendHeaders');
  let settled = false;
  const pending = f.dispatch(undefined, {requestHeaders: headers}).then(value => {
    settled = true;
    return value;
  });
  await Promise.resolve(); await Promise.resolve();
  assert.equal(settled, false);
  const replacement = {...headers, 'x-replace': 'new', 'X-Empty': '', 'X-Tab': 'a\tb'};
  complete({cancel: false, requestHeaders: replacement});
  replacement.Cookie = 'changed after callback';
  complete({cancel: true});
  assert.deepEqual(plain(await pending), {cancel: false, requestHeaders: {
    'x-kept': 'original', 'x-replace': 'new', cookie: 'a=1; b=2',
    'x-empty': '', 'x-tab': 'a\tb',
  }});
  assert.equal(f.timers.size, 0);
});

test('request header omission, empty replacement, cancellation and revocation remain distinct', async () => {
  const f = fixture();
  const api = f.session('');
  for (const response of [{}, {requestHeaders: {}}, {cancel: true}]) {
    api.onBeforeSendHeaders((_details, callback) => callback(response));
    assert.deepEqual(plain(await f.dispatch(undefined, {requestHeaders: {Cookie: 'keep=1'}})), response);
  }
  api.onBeforeSendHeaders(null);
  const removed = f.registrations.at(-1);
  assert.equal(removed.event, 'onBeforeSendHeaders');
  assert.equal(removed.enabled, false);
  f.release(removed.registrationId);
  await assert.rejects(f.dispatch(removed), {code: 'ERR_ACCESS_DENIED'});
});

test('invalid request headers and response fields fail without silently dropping authentication headers', async () => {
  for (const response of [
    {requestHeaders: null}, {requestHeaders: []},
    {requestHeaders: {'': 'value'}}, {requestHeaders: {'bad name': 'value'}},
    {requestHeaders: {'bad\r\nname': 'value'}},
    {requestHeaders: {Authorization: ['one', 'two']}},
    {requestHeaders: {Authorization: 42}},
    {requestHeaders: {'X-Test': 'before\0after'}},
    {requestHeaders: {'X-Test': 'before\rafter'}},
    {requestHeaders: {'X-Test': 'before\nafter'}},
    {responseHeaders: {}}, {statusLine: 'HTTP/1.1 200 OK'},
    {redirectURL: 'https://wrong-event.test/'},
  ]) {
    const f = fixture();
    f.session('').onBeforeSendHeaders((_details, callback) => callback(response));
    await assert.rejects(f.dispatch(), error => /^ERR_/.test(error.code));
    assert.equal(f.timers.size, 0);
  }
  for (const event of ['onBeforeRequest', 'onHeadersReceived']) {
    const f = fixture();
    f.session('')[event]((_details, callback) => callback({requestHeaders: {Authorization: 'wrong stage'}}));
    await assert.rejects(f.dispatch(), {code: 'ERR_NOT_SUPPORTED'});
  }
});

test('replacement and null keep already queued requests bound to their original listener', async () => {
  const f = fixture();
  const api = f.session('partition');
  let oldCallback;
  api.onBeforeRequest((_details, callback) => { oldCallback = callback; });
  const old = f.registrations.at(-1);
  api.onBeforeRequest((_details, callback) => callback({redirectURL: 'https://new.test/'}));
  const current = f.registrations.at(-1);
  const inFlight = f.dispatch(old, {}, 'old');
  f.release(old.registrationId);
  oldCallback({cancel: true});
  assert.deepEqual(plain(await inFlight), {cancel: true});
  assert.deepEqual(plain(await f.dispatch(current)), {redirectURL: 'https://new.test/'});
  api.onBeforeRequest(null);
  assert.equal(f.registrations.at(-1).enabled, false);
  assert.equal(f.registrations.at(-1).registrationId, current.registrationId);
  f.release(current.registrationId);
  await assert.rejects(f.dispatch(current), {code: 'ERR_ACCESS_DENIED'});
});

test('rejected registration keeps previous listener and releases the rejected candidate', async () => {
  let fail = false;
  const failure = Object.assign(new Error('invalid native URLPattern'), {code: 'ERR_INVALID_ARG_VALUE'});
  const f = fixture(() => { if (fail) throw failure; });
  const api = f.session('');
  api.onBeforeRequest((_details, callback) => callback({cancel: true}));
  const old = f.registrations.at(-1);
  fail = true;
  assert.throws(() => api.onBeforeRequest({urls: ['invalid']}, () => {}), error => error === failure);
  assert.deepEqual(plain(await f.dispatch(old)), {cancel: true});
  await assert.rejects(f.dispatch(f.registrations.at(-1)), {code: 'ERR_ACCESS_DENIED'});
});

test('dispatch cannot reuse an identity or route a registration to another Session or event', async () => {
  const f = fixture();
  f.session('one').onBeforeRequest(() => {});
  const registration = f.registrations.at(-1);
  await assert.rejects(f.dispatch({...registration, partition: 'two'}), {code: 'ERR_ACCESS_DENIED'});
  await assert.rejects(f.dispatch({...registration, event: 'onHeadersReceived'}), {code: 'ERR_ACCESS_DENIED'});
  const pending = f.dispatch();
  const cancelled = assert.rejects(pending, {code: 'ABORT_ERR'});
  await assert.rejects(f.dispatch(), {code: 'ERR_INVALID_ARG_VALUE'});
  f.cancel('request-1');
  await cancelled;
  assert.equal(f.timers.size, 0);
});

test('cancellation, shutdown and timeout reject pending work without late success', async () => {
  for (const mode of ['cancel', 'shutdown', 'timeout']) {
    const f = fixture();
    let callback;
    f.session('').onBeforeRequest((_details, next) => { callback = next; });
    const result = assert.rejects(f.dispatch(), {code: mode === 'timeout' ? 'ERR_TIMED_OUT' : 'ABORT_ERR'});
    if (mode === 'cancel') f.cancel('request-1');
    else if (mode === 'shutdown') f.shutdown();
    else [...f.timers.values()][0]();
    callback({});
    await result;
    assert.equal(f.timers.size, 0);
  }
});

test('synchronous throws and rejected async listeners fail rather than allowing requests', async () => {
  for (const error of [new Error('listener failure'), undefined, null, false, 0]) {
    for (const listener of [() => { throw error; }, async () => { throw error; }]) {
      const f = fixture();
      f.session('').onBeforeRequest(listener);
      await assert.rejects(f.dispatch(), value => value === error);
      assert.equal(f.timers.size, 0);
    }
  }
});

test('invalid filters, listeners and unsupported events fail explicitly', () => {
  const f = fixture();
  const api = f.session('');
  for (const filter of [null, [], {}, {urls: [42]}, {urls: ['x\0y']}])
    assert.throws(() => api.onBeforeRequest(filter, () => {}), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => api.onBeforeRequest({urls: ['*://*/*'], types: ['wrong']}, () => {}), {code: 'ERR_INVALID_ARG_VALUE'});
  assert.throws(() => api.onBeforeRequest({urls: ['*://*/*'], extra: true}, () => {}), {code: 'ERR_NOT_SUPPORTED'});
  for (const listener of [undefined, false, 1, {}])
    assert.throws(() => api.onBeforeRequest(listener), {code: 'ERR_INVALID_ARG_TYPE'});
  for (const event of ['onSendHeaders', 'onResponseStarted',
    'onBeforeRedirect', 'onCompleted', 'onErrorOccurred'])
    assert.throws(() => api[event](() => {}), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(f.registrations.length, 0);
});

test('malformed callback results and response-header injection fail closed', async () => {
  for (const response of [undefined, null, [], {cancel: 'false'},
    {responseHeaders: {'bad\r\nname': 'value'}}, {responseHeaders: {'X-Test': ['a\r\nb']}},
    {responseHeaders: {'X-Test': 42}}, {statusLine: 'HTTP/1.1 200 OK\nInjected: 1'},
    {redirectURL: 'https://wrong-event.test/'}, {unknown: true}]) {
    const f = fixture();
    f.session('').onHeadersReceived((_details, callback) => callback(response));
    await assert.rejects(f.dispatch(), error => /^ERR_/.test(error.code));
    assert.equal(f.timers.size, 0);
  }
});

test('complete bootstrap exposes per-Session webRequest through the native registration route', async () => {
  const registrations = [];
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
    __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
    __xenonEnv: {}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonBrowserWindowCall: (windowId, command, request) => {
      assert.equal(windowId, 0);
      assert.equal(command, 'web-request-handler');
      registrations.push(plain(request));
    },
  });
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readBootstrap(filename), context, {filename});
  const sessions = context.__xenonElectron.session;
  assert.equal(sessions.fromPartition(''), sessions.defaultSession);
  assert.equal(sessions.fromPartition('one'), sessions.fromPartition('one'));
  const api = sessions.fromPartition('one').webRequest;
  api.onHeadersReceived((details, callback) => callback({responseHeaders: {
    ...details.responseHeaders, 'Content-Security-Policy': "default-src 'self'",
  }}));
  const registration = registrations.at(-1);
  assert.equal(registration.partition, 'one');
  const result = await context.__xenonDispatchWebRequest({...registration,
    requestId: 'native-1', details: {responseHeaders: {'x-kept': ['yes']}}});
  assert.deepEqual(plain(result), {responseHeaders: {
    'x-kept': ['yes'], 'content-security-policy': ["default-src 'self'"],
  }});
  api.onHeadersReceived(null);
  assert.equal(registrations.at(-1).enabled, false);
  api.onBeforeSendHeaders((details, callback) => callback({requestHeaders: {
    ...details.requestHeaders, 'X-Added': 'actual request',
  }}));
  const requestRegistration = registrations.at(-1);
  assert.equal(requestRegistration.event, 'onBeforeSendHeaders');
  assert.deepEqual(plain(await context.__xenonDispatchWebRequest({...requestRegistration,
    requestId: 'native-2', details: {requestHeaders: {Cookie: 'from-network=1'}}})),
  {requestHeaders: {cookie: 'from-network=1', 'x-added': 'actual request'}});
  context.__xenonShutdownWebRequests();
});
