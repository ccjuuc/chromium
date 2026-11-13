// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const {EventEmitter} = require('node:events');
const plain = value => JSON.parse(JSON.stringify(value));
const turn = () => new Promise(resolve => setImmediate(resolve));
const source = fs.readFileSync(path.join(__dirname, 'main/electron_net.js'), 'utf8');
const httpSource = fs.readFileSync(path.join(__dirname, 'main/http.js'), 'utf8');

function fixture() {
  const starts = [], controls = [], authentications = [], sessions = new Map();
  let nextId = 0, online = false, ready = true;
  class StreamBase extends EventEmitter {}
  const context = vm.createContext({Buffer, URL, URLSearchParams, Blob,
    EventEmitter, EventTarget, Event, setTimeout, clearTimeout, queueMicrotask,
    streamModule: require('node:stream'),
    netModule: {}, createHttpServerModule: () => ({IncomingMessage: StreamBase}),
    __xenonHttpRequest: () => { throw new Error('Electron must not use Node HTTP'); },
    __xenonHttpAbort: () => { throw new Error('Electron must not abort Node HTTP'); },
    __xenonElectronNetStart: options => { starts.push(plain(options)); return ++nextId; },
    __xenonElectronNetControl: (id, command) => { controls.push({id, command}); return true; },
    __xenonElectronNetOnline: () => online,
    __xenonElectronNetAuth: function(id, challengeId, username, password) {
      if (arguments.length !== 2 && (typeof username !== 'string' || typeof password !== 'string'))
        throw new TypeError('Native authentication requires omitted credentials or two strings');
      authentications.push({id, challengeId, username, password}); return true;
    },
  });
  vm.runInContext(httpSource + '\n' + source, context);
  function getSession(partition = '') {
    if (!sessions.has(partition)) sessions.set(partition, Object.freeze({partition}));
    return sessions.get(partition);
  }
  const api = context.createElectronNet({getSession, isSession: value => [...sessions.values()].includes(value), isReady: () => ready});
  const dispatch = event => context.__xenonDispatchElectronNetEvent(event);
  return {context, api, net: api.net, starts, controls, authentications, getSession, dispatch,
    setReady: value => { ready = value; }, setOnline: value => { online = value; },
    response(id = nextId, options = {}) {
      dispatch({id, type: 'response', statusCode: 200, statusMessage: 'OK', httpVersion: '1.1',
        url: 'https://fixture.test/data', headers: {'content-type': ['text/plain']},
        rawHeaders: ['Content-Type', 'text/plain'], ...options});
    },
    data: (value, id = nextId) => dispatch({id, type: 'data', bodyBase64: Buffer.from(value).toString('base64')}),
    end: (id = nextId) => dispatch({id, type: 'end'}),
  };
}

test('request preserves Session, copied headers and upload bytes on the Chromium transport', async () => {
  const f = fixture();
  const headers = {'X-Multi': ['one', 'two']};
  const selected = f.getSession('persist:selected');
  const request = f.net.request({hostname: 'fixture.test', protocol: 'https:', port: 8443,
    path: '/upload?one=1', method: 'POST', session: selected, partition: 'ignored', headers,
    useSessionCookies: true, cache: 'no-store'});
  headers['X-Multi'].push('changed');
  let finished = 0;
  request.once('finish', () => finished++);
  request.setHeader('X-Set', 2);
  request.write('first-');
  assert.equal(f.starts.length, 0);
  request.end(Buffer.from('last'));
  await turn();
  assert.equal(finished, 1);
  assert.deepEqual(f.starts[0], {url: 'https://fixture.test:8443/upload?one=1', method: 'POST',
    headers: {'x-multi': ['one', 'two'], 'x-set': '2'}, redirect: 'follow', cache: 'no-store',
    partition: 'persist:selected', useSessionCookies: true, bypassCustomProtocolHandlers: false,
    bodyBase64: Buffer.from('first-last').toString('base64')});
  assert.throws(() => request.setHeader('Late', 'no'), {code: 'ERR_HTTP_HEADERS_SENT'});
  request.abort(); await turn();
});

test('response is delivered at headers and pause/resume controls native read credits', async () => {
  const f = fixture(), chunks = [], events = [];
  const request = f.net.request('https://fixture.test/data');
  request.on('response', response => {
    events.push('response');
    assert.equal(response.statusCode, 201);
    assert.deepEqual(plain(response.headers['set-cookie']), ['a=1', 'b=2']);
    response.on('data', chunk => { chunks.push(chunk.toString()); response.pause(); });
    response.on('end', () => events.push('end'));
  });
  request.end(); await turn();
  f.response(1, {statusCode: 201, statusMessage: 'Created', headers: {'set-cookie': ['a=1', 'b=2']}});
  assert.deepEqual(events, ['response']);
  await turn();
  assert.deepEqual(f.controls, [{id: 1, command: 'resume'}]);
  f.data('first'); await turn();
  assert.deepEqual(chunks, ['first']);
  assert.equal(f.controls.length, 1);
  request.res.resume(); await turn();
  assert.equal(f.controls.length, 2);
  f.data('second'); await turn();
  request.res.resume(); await turn();
  f.end(); await turn();
  assert.deepEqual(chunks, ['first', 'second']);
  assert.deepEqual(events, ['response', 'end']);
  assert.equal(request.res.complete, true);
  assert.equal(request.closed, true);
});

test('response UTF-8 decoder preserves split characters across native chunks', async () => {
  const f = fixture(), chunks = [];
  const request = f.net.request('https://fixture.test/data', response => {
    response.setEncoding('utf8');
    response.on('data', value => chunks.push(value));
  });
  request.end(); f.response(); await turn();
  const utf8 = Buffer.from('A😀中Z');
  for (const chunk of [utf8.subarray(0, 3), utf8.subarray(3, 6), utf8.subarray(6)]) {
    f.data(chunk); await turn();
  }
  f.end(); await turn();
  assert.equal(chunks.join(''), 'A😀中Z');
});

test('async iteration consumes actual chunks with bounded native credits', async () => {
  const f = fixture();
  let response;
  const request = f.net.request('https://fixture.test/data', value => { response = value; });
  request.end(); f.response();
  assert.equal(f.controls.length, 0);
  const iterator = response[Symbol.asyncIterator]();
  const first = iterator.next(); await turn();
  assert.equal(f.controls.length, 1);
  f.data('one');
  assert.equal((await first).value.toString(), 'one');
  const second = iterator.next(); await turn();
  assert.equal(f.controls.length, 2);
  f.data('two');
  assert.equal((await second).value.toString(), 'two');
  const final = iterator.next(); await turn(); f.end();
  assert.equal((await final).done, true);
});

test('pipe honors downstream backpressure and pauses when the destination closes', async () => {
  const f = fixture(), chunks = [];
  const destination = new EventEmitter();
  destination.write = chunk => { chunks.push(chunk.toString()); return false; };
  destination.end = () => {};
  const request = f.net.request('https://fixture.test/data', response => response.pipe(destination));
  request.end(); f.response(); await turn();
  f.data('one'); await turn();
  assert.deepEqual(chunks, ['one']);
  assert.equal(f.controls.length, 1);
  destination.emit('drain'); await turn();
  assert.equal(f.controls.length, 2);
  destination.emit('close'); f.data('two'); await turn();
  assert.deepEqual(chunks, ['one']);
  assert.equal(f.controls.length, 2);
  assert.equal(request.res.isPaused(), true);
  assert.equal(destination.listenerCount('drain'), 0);
  request.abort(); await turn();
});

test('abort terminates pending requests once and ignores late native events', async () => {
  const f = fixture(), events = [];
  const unsent = f.net.request('https://fixture.test/unsent');
  unsent.on('abort', () => events.push('unsent-abort'));
  unsent.on('close', () => events.push('unsent-close'));
  unsent.abort(); unsent.end(); await turn();
  assert.equal(f.starts.length, 0);
  const request = f.net.request('https://fixture.test/data');
  request.on('abort', () => events.push('abort'));
  request.on('close', () => events.push('close'));
  request.end(); f.response();
  request.res.on('aborted', () => events.push('response-aborted'));
  request.res.on('data', () => events.push('late-data'));
  request.abort(); request.abort(); await turn();
  f.data('ignored'); f.end(); await turn();
  assert.deepEqual(events, ['unsent-abort', 'unsent-close', 'abort', 'response-aborted', 'close']);
  assert.deepEqual(f.controls.filter(item => item.command === 'abort'), [{id: 1, command: 'abort'}]);
});

test('native errors preserve codes and close both request and response', async () => {
  const f = fixture(), events = [];
  const request = f.net.request('https://fixture.test/data');
  request.on('error', error => events.push('request:' + error.code));
  request.on('close', () => events.push('close'));
  request.end(); f.response();
  request.res.on('error', error => events.push('response:' + error.code));
  f.dispatch({id: 1, type: 'error', code: 'ERR_CONNECTION_RESET', message: 'Connection reset'});
  await turn();
  assert.deepEqual(events, ['response:ERR_CONNECTION_RESET', 'request:ERR_CONNECTION_RESET', 'close']);
  assert.equal(request.res.destroyed, true);
});

test('manual redirects require synchronous followRedirect and error redirects fail', async () => {
  for (const mode of ['manual', 'follow', 'error', 'manual-cancel']) {
    const f = fixture(), redirects = [], errors = [];
    const request = f.net.request({url: 'https://fixture.test/start', redirect: mode === 'manual-cancel' ? 'manual' : mode});
    request.on('redirect', (...args) => { redirects.push(args); if (mode === 'manual') request.followRedirect(); });
    request.on('error', error => errors.push(error));
    request.end();
    f.dispatch({id: 1, type: 'redirect', statusCode: 302, method: 'GET',
      url: 'https://fixture.test/next', headers: {location: ['https://fixture.test/next']}});
    assert.equal(redirects.length, mode === 'error' ? 0 : 1);
    await turn();
    const cancelled = mode === 'error' || mode === 'manual-cancel';
    assert.equal(f.controls[0].command, cancelled ? 'abort' : 'follow-redirect');
    assert.equal(errors.length, cancelled ? 1 : 0);
    assert.throws(() => request.followRedirect(), {code: 'ERR_INVALID_STATE'});
    request.abort(); await turn();
  }
});

test('authentication awaits the actual login callback, replies once and drops callbacks after abort', async () => {
  const f = fixture(); let callback;
  const request = f.net.request('https://fixture.test/auth');
  request.on('login', (info, reply) => { assert.equal(info.realm, 'fixture'); callback = reply; });
  request.end();
  const challenge = {id: 1, type: 'login', challengeId: 'challenge-1', authInfo: {
    isProxy: false, scheme: 'basic', host: 'fixture.test', port: 443, realm: 'fixture'}};
  f.dispatch(challenge); await turn();
  assert.equal(f.authentications.length, 0);
  callback('fixture-user', 'fixture-password'); callback('duplicate', 'ignored');
  assert.deepEqual(f.authentications, [{id: 1, challengeId: 'challenge-1', username: 'fixture-user', password: 'fixture-password'}]);
  f.dispatch({...challenge, challengeId: 'challenge-2'});
  request.abort(); await turn(); callback('late', 'ignored');
  assert.equal(f.authentications.length, 1);
});

test('authentication without a listener or with invalid credentials is cancelled', async () => {
  const f = fixture();
  const first = f.net.request('https://fixture.test/auth'); first.end();
  f.dispatch({id: 1, type: 'login', challengeId: 1, authInfo: {}});
  assert.deepEqual(f.authentications[0], {id: 1, challengeId: 1, username: undefined, password: undefined});
  first.abort(); await turn();
  const second = f.net.request('https://fixture.test/auth');
  second.on('login', (_info, callback) => callback(42, 'invalid')); second.end();
  assert.throws(() => f.dispatch({id: 2, type: 'login', challengeId: 2, authInfo: {}}), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.equal(f.authentications.length, 2);
  assert.equal(f.authentications[1].username, undefined);
  second.abort(); await turn();
});

test('fetch resolves at headers and consumes the Session network stream only on demand', async () => {
  const f = fixture();
  const pending = f.api.fetchSession(f.getSession('persist:fetch'), 'https://fixture.test/data',
    {credentials: 'include', cache: 'no-store', bypassCustomProtocolHandlers: true});
  f.response();
  const response = await pending;
  assert.equal(response.status, 200);
  assert.equal(response.url, 'https://fixture.test/data');
  assert.equal(response.bodyUsed, false);
  assert.equal(f.controls.length, 0);
  assert.equal(f.starts[0].partition, 'persist:fetch');
  assert.equal(f.starts[0].credentials, 'include');
  assert.equal(f.starts[0].bypassCustomProtocolHandlers, true);
  const text = response.text(); await turn();
  assert.equal(f.controls.length, 1);
  f.data('hello '); await turn(); f.data('world'); await turn(); f.end();
  assert.equal(await text, 'hello world');
  assert.equal(response.bodyUsed, true);
  await assert.rejects(response.text(), /already been consumed/);
  assert.throws(() => response.body, {code: 'ERR_NOT_SUPPORTED'});
});

test('fetch cancellation rejects before headers and after headers during body consumption', async () => {
  for (const afterHeaders of [false, true]) {
    const f = fixture();
    const controller = new f.context.AbortController();
    const pending = f.net.fetch('https://fixture.test/data', {signal: controller.signal});
    let observed;
    if (afterHeaders) { f.response(); observed = (await pending).text(); }
    else observed = pending;
    const rejected = assert.rejects(observed, {name: 'AbortError', code: 'ABORT_ERR'});
    controller.abort(); await rejected; await turn();
    assert.equal(f.controls.filter(item => item.command === 'abort').length, 1);
    f.data('late'); f.end();
  }
});

test('fetch handles Blob uploads and JSON responses without using Node HTTP', async () => {
  const f = fixture();
  const pending = f.net.fetch('https://fixture.test/data', {method: 'POST', body: new Blob(['request'], {type: 'text/custom'})});
  await turn();
  assert.equal(Buffer.from(f.starts[0].bodyBase64, 'base64').toString(), 'request');
  assert.equal(f.starts[0].headers['content-type'], 'text/custom');
  f.response(); const response = await pending;
  const parsed = response.json(); await turn(); f.data('{"ok":'); await turn(); f.data('true}'); await turn(); f.end();
  assert.deepEqual(plain(await parsed), {ok: true});
});

test('unsupported capabilities, invalid headers and unavailable startup fail explicitly', async () => {
  const f = fixture();
  f.setReady(false);
  assert.throws(() => f.net.request('https://fixture.test'), {code: 'ERR_NOT_READY'});
  f.setReady(true);
  for (const options of [{url: 'file:///private/file'}, {url: 'https://fixture.test', rejectUnauthorized: false},
    {url: 'https://fixture.test', headers: {'X-Bad': 'one\r\ntwo'}}])
    assert.throws(() => f.net.request(options));
  assert.throws(() => f.net.request({url: 'https://fixture.test', session: {partition: 'forged'}}), {code: 'ERR_INVALID_ARG_TYPE'});
  const request = f.net.request('https://fixture.test');
  assert.throws(() => { request.chunkedEncoding = true; }, {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => request.write(Buffer.alloc(32 * 1024 * 1024 + 1)), {code: 'ERR_BUFFER_TOO_LARGE'});
  await assert.rejects(f.net.fetch('https://fixture.test', {redirect: 'manual'}), {code: 'ERR_NOT_SUPPORTED'});
  await assert.rejects(f.net.fetch('file:///private/file'), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(f.starts.length, 0);
});

test('host with port and explicit credentials retain their standard meanings', async () => {
  const f = fixture();
  const request = f.net.request({host: 'fixture.test:8080', path: '/one',
    credentials: 'omit', useSessionCookies: true});
  request.end();
  assert.equal(f.starts[0].url, 'http://fixture.test:8080/one');
  assert.equal(f.starts[0].credentials, 'omit');
  assert.equal(f.starts[0].useSessionCookies, true);
  request.abort(); await turn();
});

test('native start failure and invalid response metadata reject without leaving pending fetches', async () => {
  const f = fixture();
  const originalStart = f.context.__xenonElectronNetStart;
  f.context.__xenonElectronNetStart = () => { throw new Error('ERR_ACCESS_DENIED: unavailable Session'); };
  await assert.rejects(f.net.fetch('https://fixture.test'), {code: 'ERR_ACCESS_DENIED'});
  f.context.__xenonElectronNetStart = originalStart;
  const pending = f.net.fetch('https://fixture.test');
  const rejected = assert.rejects(pending, /Invalid response status/);
  f.response(1, {statusCode: 101});
  await rejected; await turn();
  assert.deepEqual(f.controls, [{id: 1, command: 'abort'}]);
});

test('online state reflects native changes and shutdown aborts all live requests', async () => {
  const f = fixture(), errors = [];
  assert.equal(f.net.isOnline(), false);
  f.setOnline(true); assert.equal(f.net.online, true);
  for (let index = 0; index < 2; ++index) {
    const request = f.net.request('https://fixture.test/' + index);
    request.on('error', error => errors.push(error.code)); request.end();
  }
  f.context.__xenonShutdownElectronNet(); await turn();
  assert.deepEqual(errors, ['ABORT_ERR', 'ABORT_ERR']);
  assert.deepEqual(f.controls.map(item => item.command), ['abort', 'abort']);
  f.end(1); f.end(2);
});

test('complete bootstrap wires electron.net and session.fetch to isolated native requests', async () => {
  const starts = [], controls = [];
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
    __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
    __xenonEnv: {}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonElectronNetStart: options => { starts.push(plain(options)); return starts.length; },
    __xenonElectronNetControl: (id, command) => { controls.push({id, command}); return true; },
    __xenonElectronNetOnline: () => true,
  });
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readBootstrap(filename), context, {filename});
  const electron = context.__xenonElectron;
  assert.throws(() => electron.net.request('https://fixture.test'), {code: 'ERR_NOT_READY'});
  context.__xenonMarkAppReady();
  const pending = electron.session.fromPartition('persist:full-bootstrap').fetch('https://fixture.test/data');
  assert.equal(starts[0].partition, 'persist:full-bootstrap');
  context.__xenonDispatchElectronNetEvent({id: 1, type: 'response', statusCode: 200,
    statusMessage: 'OK', url: 'https://fixture.test/data', headers: {}, rawHeaders: []});
  const response = await pending;
  assert.equal(response instanceof context.Response, true);
  const body = response.text(); await turn();
  assert.deepEqual(controls, [{id: 1, command: 'resume'}]);
  context.__xenonDispatchElectronNetEvent({id: 1, type: 'data', bodyBase64: Buffer.from('bootstrap-body').toString('base64')});
  await turn(); context.__xenonDispatchElectronNetEvent({id: 1, type: 'end'});
  assert.equal(await body, 'bootstrap-body');
  assert.equal(electron.net.isOnline(), true);
  context.__xenonShutdownElectronNet();
});
