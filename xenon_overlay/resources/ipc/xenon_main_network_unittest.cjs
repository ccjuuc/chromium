// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {readFileSync} = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const zlib = require('node:zlib');
const source = readFileSync(path.join(__dirname, 'xenon_ipc_main_bootstrap.js'), 'utf8');
const factory = source.slice(source.indexOf('  function createMainNetwork('),
    source.indexOf('  const mainNetwork = createMainNetwork('));

async function fixture(t) {
  let pendingId = 1, cancellations = 0;
  const active = new Map();
  const seen = [];
  const server = http.createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    seen.push({url: request.url, method: request.method, headers: request.headers,
      body: Buffer.concat(chunks)});
    if (request.url === '/hang') return;
    if (request.url === '/redirect') { response.writeHead(302, {location: '/binary'}); response.end(); return; }
    if (request.url === '/echo') { response.end(Buffer.concat(chunks)); return; }
    if (request.url === '/gzip') {
      const compressed = zlib.gzipSync('decoded bytes');
      response.writeHead(200, {'content-encoding': 'gzip', 'content-length': compressed.length});
      response.end(compressed); return;
    }
    response.writeHead(418, {'x-fixture': 'real-response'});
    response.end(Buffer.from([0, 255, 128, 65]));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => { server.closeAllConnections(); return new Promise(resolve => server.close(resolve)); });
  const context = vm.createContext({Buffer, URL, URLSearchParams, EventEmitter,
    queueMicrotask, setTimeout, clearTimeout,
    nativeRequest(request) {
      const id = pendingId++;
      const controller = new globalThis.AbortController();
      active.set(id, controller);
      const promise = globalThis.fetch(request.url, {method: request.method,
        headers: request.headers, signal: controller.signal, redirect: request.redirect,
        body: ['GET', 'HEAD'].includes(request.method) ? undefined : Buffer.from(request.bodyBase64, 'base64'),
      }).then(async response => ({statusCode: response.status, statusMessage: response.statusText,
        headers: Object.fromEntries(response.headers), finalUrl: response.url, httpVersion: '1.1',
        bodyBase64: Buffer.from(await response.arrayBuffer()).toString('base64')}))
          .finally(() => active.delete(id));
      return {id, promise};
    },
    nativeAbort(id) { ++cancellations; active.get(id)?.abort(); },
  });
  const api = vm.runInContext(factory + '\ncreateMainNetwork(nativeRequest, nativeAbort)', context);
  return {api, seen, active, cancellations: () => cancellations,
    url: 'http://127.0.0.1:' + server.address().port};
}

test('main fetch sends real headers and preserves HTTP errors and binary bodies', async t => {
  const {api, url, seen} = await fixture(t);
  const response = await api.globals.fetch(url + '/binary', {headers: {'X-Request': 'yes'}});
  assert.equal(response.status, 418);
  assert.equal(response.ok, false);
  assert.equal(response.headers.get('X-Fixture'), 'real-response');
  assert.equal(seen[0].headers['x-request'], 'yes');
  const clone = response.clone();
  assert.equal(Buffer.from(await response.arrayBuffer()).toString('hex'), '00ff8041');
  assert.equal(Buffer.from(await clone.arrayBuffer()).toString('hex'), '00ff8041');
  await assert.rejects(response.text(), {name: 'TypeError'});
});

test('main fetch Request matches Axios capability checks and snapshots POST input', async t => {
  const {api, url} = await fixture(t);
  const {Request, Response, Headers, fetch} = api.globals;
  const input = Buffer.from([0, 255, 128, 65]);
  const request = new Request(url + '/echo', {method: 'post', body: input,
    headers: new Headers({'Content-Type': 'application/octet-stream'}), duplex: 'half'});
  input.fill(9);
  assert.equal(Object.prototype.toString.call(request), '[object Request]');
  assert.equal('credentials' in Request.prototype, true);
  assert.equal(typeof new Response().arrayBuffer, 'function');
  const response = await fetch(request);
  assert.equal(Buffer.from(await response.arrayBuffer()).toString('hex'), '00ff8041');
  assert.equal(request.bodyUsed, true);
  await assert.rejects(fetch(request), {name: 'TypeError'});
});

test('main fetch follows real redirects and rejects unsupported redirect mode', async t => {
  const {api, url} = await fixture(t);
  const response = await api.globals.fetch(url + '/redirect');
  assert.equal(response.redirected, true);
  assert.equal(response.url, url + '/binary');
  await assert.rejects(api.globals.fetch(url + '/redirect', {redirect: 'error'}));
  await assert.rejects(api.globals.fetch(url + '/redirect', {redirect: 'manual'}), {code: 'ERR_NOT_SUPPORTED'});
});

test('main AbortController cancels the real active fetch and preserves reason', async t => {
  const {api, url, seen, active, cancellations} = await fixture(t);
  const controller = new api.globals.AbortController();
  const promise = api.globals.fetch(url + '/hang', {signal: controller.signal});
  while (!seen.length) await new Promise(resolve => setTimeout(resolve, 1));
  const reason = new Error('cancelled by caller');
  controller.abort(reason);
  await assert.rejects(promise, value => value === reason);
  await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal(cancellations(), 1);
  assert.equal(active.size, 0);
  await assert.rejects(api.globals.fetch(url, {signal: controller.signal}), value => value === reason);
  assert.equal(cancellations(), 1);
});

test('main HTTP get emits actual status, data and completion in order', async t => {
  const {api, url} = await fixture(t);
  const order = [];
  const bytes = await new Promise((resolve, reject) => {
    const request = api.http.get(url + '/binary', response => {
      order.push('response'); assert.equal(response.statusCode, 418);
      const data = [];
      response.on('data', chunk => { order.push('data'); data.push(chunk); });
      response.on('end', () => { order.push('end'); resolve(Buffer.concat(data)); });
    });
    request.on('finish', () => order.push('finish'));
    request.on('error', reject);
  });
  assert.equal(bytes.toString('hex'), '00ff8041');
  assert.deepEqual(order, ['finish', 'response', 'data', 'end']);
});

test('main HTTP POST snapshots writes and timeout cancellation never fabricates response', async t => {
  const {api, url, cancellations} = await fixture(t);
  const input = Buffer.from([255, 0, 128]);
  const result = new Promise((resolve, reject) => {
    const request = api.http.request(url + '/echo', {method: 'POST'}, response => {
      response.on('data', resolve);
    });
    request.on('error', reject); request.write(input); input.fill(0); request.end();
  });
  assert.equal(Buffer.from(await result).toString('hex'), 'ff0080');
  await new Promise(resolve => {
    const request = api.http.get(url + '/hang');
    request.on('response', () => assert.fail('fabricated response'));
    request.on('error', error => { assert.equal(error.code, 'ETIMEDOUT'); resolve(); });
    request.setTimeout(5, () => request.destroy(Object.assign(new Error('timeout'), {code: 'ETIMEDOUT'})));
  });
  assert.equal(cancellations(), 1);
});

test('main fetch decodes UTF-8 BOM and malformed bytes like the platform Response', async t => {
  const {api} = await fixture(t);
  for (const hex of ['efbbbf7b7d', 'ff61', 'e180', 'e0a080', 'eda080', 'f0908080', 'f4908080']) {
    const bytes = Buffer.from(hex, 'hex');
    assert.equal(await new api.globals.Response(bytes).text(), await new Response(bytes).text(), hex);
  }
  assert.equal(JSON.stringify(await new api.globals.Response(Buffer.from('efbbbf7b7d', 'hex')).json()), '{}');
});

test('main HTTP end listener destruction emits close exactly once', async t => {
  const {api, url} = await fixture(t);
  let closed = 0;
  await new Promise((resolve, reject) => {
    const request = api.http.get(url + '/binary', response => {
      response.on('close', () => ++closed);
      response.on('data', () => {});
      response.on('end', () => { response.destroy(); request.destroy(); setTimeout(resolve, 0); });
    }).on('error', reject);
  });
  assert.equal(closed, 1);
});

test('main HTTP decoded gzip body has matching headers while fetch retains wire headers', async t => {
  const {api, url} = await fixture(t);
  const response = await api.globals.fetch(url + '/gzip');
  assert.equal(response.headers.get('content-encoding'), 'gzip');
  assert.equal(await response.text(), 'decoded bytes');
  await new Promise((resolve, reject) => api.http.get(url + '/gzip', incoming => {
    assert.equal(incoming.headers['content-encoding'], undefined);
    assert.equal(incoming.headers['content-length'], undefined);
    incoming.setEncoding('utf8');
    incoming.on('data', value => assert.equal(value, 'decoded bytes'));
    incoming.on('end', resolve);
  }).on('error', reject));
});
