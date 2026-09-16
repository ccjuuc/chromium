// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const filename = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const source = readFileSync(filename, 'utf8');

function fixture() {
  let resolve, reject;
  const pending = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  const requests = [];
  const timers = new Map();
  let nextTimer = 1;
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    setTimeout(callback) { const id = nextTimer++; timers.set(id, callback); return id; },
    clearTimeout(id) { timers.delete(id); },
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}), setDispatchHandler() {},
      invoke(channel, request) {
        assert.equal(channel, '__xenon:net-request');
        requests.push(request);
        return pending;
      },
    },
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(source, context, {filename});
  return {http: context.require('http'), requests, timers, reject,
    respond() { resolve({statusCode: 200, bodyBase64: 'YWJj'}); },
    expire() {
      const callbacks = Array.from(timers.values());
      timers.clear();
      for (const callback of callbacks) callback();
    },
  };
}

const settle = () => new Promise(resolve => setImmediate(resolve));

for (const operation of ['abort', 'destroy']) {
  test(`HTTP ${operation} in response callback suppresses queued data and end`, async () => {
    const runtime = fixture();
    const events = [];
    const req = runtime.http.get('http://fixture.invalid', incoming => {
      events.push('response');
      incoming.on('data', () => events.push('data'));
      incoming.on('end', () => events.push('end'));
      req[operation]();
    });
    req.on('error', error => events.push(error.code || 'error'));
    await settle();
    runtime.respond();
    await settle();
    assert.deepEqual(events, ['response']);
  });
}

test('HTTP destruction in data callback suppresses subsequent successful end', async () => {
  const runtime = fixture();
  const events = [];
  const req = runtime.http.get('http://fixture.invalid', incoming => {
    incoming.on('data', () => { events.push('data'); req.destroy(); });
    incoming.on('end', () => events.push('end'));
  });
  await settle();
  runtime.respond();
  await settle();
  assert.deepEqual(events, ['data']);
});

test('HTTP abort before end cannot restart timers or submit buffered writes', async () => {
  const runtime = fixture();
  const events = [];
  const req = runtime.http.request({hostname: 'fixture.invalid', method: 'POST', timeout: 10});
  req.on('timeout', () => events.push('timeout'));
  req.on('error', () => events.push('error'));
  req.write('queued');
  req.abort();
  assert.equal(req.write('after abort'), false);
  req.end('after abort');
  req.setTimeout(10);
  runtime.expire();
  await settle();
  assert.equal(runtime.requests.length, 0);
  assert.equal(runtime.timers.size, 0);
  assert.deepEqual(events, []);
});

test('HTTP abort in flight ignores both late success and late failure', async () => {
  for (const failure of [false, true]) {
    const runtime = fixture();
    const events = [];
    const req = runtime.http.get('http://fixture.invalid', () => events.push('response'));
    req.on('error', () => events.push('error'));
    req.setTimeout(10);
    await settle();
    req.abort();
    if (failure) runtime.reject(new Error('late failure')); else runtime.respond();
    await settle();
    assert.equal(runtime.requests.length, 1);
    assert.equal(runtime.timers.size, 0);
    assert.deepEqual(events, []);
  }
});

test('HTTP timeout suppresses late response delivery and cannot be restarted', async () => {
  const runtime = fixture();
  const events = [];
  const req = runtime.http.get('http://fixture.invalid', () => events.push('response'));
  req.on('timeout', () => events.push('timeout'));
  req.on('error', error => events.push(error.code));
  req.setTimeout(10);
  await settle();
  runtime.expire();
  req.setTimeout(10);
  runtime.expire();
  runtime.respond();
  await settle();
  assert.deepEqual(events, ['timeout', 'ETIMEDOUT']);
});
