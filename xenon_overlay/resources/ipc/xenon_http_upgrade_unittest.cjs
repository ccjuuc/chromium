// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const net = require('node:net');
const nodeHttp = require('node:http');
const nodeHttps = require('node:https');
const test = require('node:test');
const vm = require('node:vm');

function runtime(kind) {
  const context = vm.createContext({Buffer, URL, URLSearchParams, EventEmitter,
    netModule: net, TextEncoder, TextDecoder, atob, btoa, console,
    queueMicrotask, setTimeout, clearTimeout, AbortController,
    __xenonPaths: {platform: process.platform, arch: process.arch, endianness: 'LE'},
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {}, invoke() { throw Error('Buffered transport must not handle an upgrade'); },
    },
  });
  if (kind === 'renderer') {
    vm.runInContext(readBootstrapPart('xenon_ipc_renderer_bootstrap.js'), context);
    return {http: context.require('http'), https: context.require('https')};
  }
  const source = readBootstrapPart('main/http.js');
  const platform = readBootstrapPart('main/platform.js');
  const urlBoundary = platform.indexOf('  const invokeHandlers = new Map();');
  assert.ok(urlBoundary > 0);
  // Exercise the same URL implementation used by the native main/Utility host.
  // A Node URL fixture hides loss of URL credentials before HTTP sees them.
  return vm.runInContext(platform.slice(0, urlBoundary) + readBootstrapPart('main/event_target.js') +
    '\nObject.assign(globalThis, createMainEventClasses(error => { throw error; }, () => Date.now()));\n' +
    readBootstrapPart('common/http_server.js') + readBootstrapPart('common/http_upgrade.js') +
    source.slice(0, source.indexOf('  const mainNetwork = createMainNetwork(')) +
    '\ncreateMainNetwork(() => { throw Error("Buffered transport must not handle an upgrade"); }, () => {})', context);
}

async function peer(t, handler) {
  let seen = '';
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('error', () => {});
    socket.once('close', () => sockets.delete(socket));
    let received = false;
    socket.on('data', bytes => {
      if (received) return;
      seen += bytes.toString();
      if (seen.includes('\r\n\r\n')) { received = true; handler(socket, seen); }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => {
    for (const socket of sockets) socket.destroy();
    return new Promise(resolve => server.close(resolve));
  });
  return {url: 'http://127.0.0.1:' + server.address().port, get seen() { return seen; }};
}

const responseHead = 'HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: x-echo\r\n\r\n';
function request(api, url, settings = {}, secure = false) {
  return (secure ? api.https : api.http).request(url, {
    // Use the public createConnection API to connect both facades to real OS
    // sockets in this test. Native bridge/stream behavior has separate tests.
    createConnection: options => net.connect(options),
    headers: {Connection: 'keep-alive, Upgrade', Upgrade: 'x-echo', Cookie: 'explicit=1'},
    ...settings,
  });
}

async function responseSnapshot(t, api, responseHeaders, settings = {}, userinfo = '', maxHeadersCount) {
  const server = await peer(t, socket => socket.end('HTTP/1.1 101 Switching Protocols\r\n' + responseHeaders + '\r\n'));
  const req = request(api, server.url.replace('http://', 'http://' + userinfo), settings);
  if (maxHeadersCount !== undefined) req.maxHeadersCount = maxHeadersCount;
  const result = await new Promise((resolve, reject) => {
    req.on('error', reject);
    const snapshot = (event, response) => ({event,
      headers: JSON.parse(JSON.stringify(response.headers)), rawHeaders: Array.from(response.rawHeaders)});
    req.on('upgrade', (response, socket) => { socket.destroy(); resolve(snapshot('upgrade', response)); });
    req.on('response', response => {
      response.on('end', () => resolve(snapshot('response', response))); response.resume();
    });
    req.end();
  });
  result.authorization = /^Authorization:([^\r\n]*)/im.exec(server.seen)?.[1].trim() || null;
  return result;
}

for (const kind of ['main', 'renderer']) {
  test(`${kind} split 101 transfers binary head and the active socket`, async t => {
    const server = await peer(t, socket => {
      socket.write(responseHead.slice(0, 15));
      setImmediate(() => {
        socket.write(Buffer.concat([Buffer.from(responseHead.slice(15)), Buffer.from([255, 0, 128])]));
        socket.on('data', bytes => socket.write(bytes));
      });
    });
    const req = request(runtime(kind), server.url + '/test?x=1');
    let closes = 0;
    req.on('close', () => ++closes);
    await new Promise((resolve, reject) => {
      req.on('error', reject).on('response', () => reject(Error('Unexpected response')));
      req.on('upgrade', (response, socket, head) => {
        assert.equal(response.statusCode, 101);
        assert.equal(head.toString('hex'), 'ff0080');
        assert.equal(response.socket, socket);
        socket.once('data', bytes => {
          assert.equal(bytes.toString(), 'client-payload'); socket.destroy(); resolve();
        });
        socket.write('client-payload');
      });
      req.end();
    });
    assert.match(server.seen, /GET \/test\?x=1 HTTP\/1.1/);
    assert.match(server.seen, /cookie: explicit=1/i);
    assert.equal(closes, 1);
  });

  test(`${kind} one-byte fragments preserve informational and upgrade responses`, async t => {
    const server = await peer(t, socket => {
      const bytes = Buffer.from('HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n' + responseHead);
      let offset = 0;
      function next() {
        if (offset === bytes.length) return;
        socket.write(bytes.subarray(offset, ++offset)); setImmediate(next);
      }
      next();
    });
    const req = request(runtime(kind), server.url);
    const information = [];
    req.on('information', response => information.push(response.statusCode));
    await new Promise((resolve, reject) => {
      req.on('error', reject).on('upgrade', (_response, socket, head) => {
        assert.equal(head.length, 0); socket.destroy(); resolve();
      });
      req.end();
    });
    assert.deepEqual(information, [103]);
  });

  for (const [name, body] of [
    ['length', 'Content-Length: 4\r\n\r\nnope'],
    ['chunked', 'Transfer-Encoding: chunked\r\n\r\n2\r\nno\r\n2\r\npe\r\n0\r\nX-Trail: yes\r\n\r\n'],
    ['EOF', 'Connection: close\r\n\r\nnope'],
  ]) {
    test(`${kind} non-101 ${name} response retains status, body and headers`, async t => {
      const server = await peer(t, socket => socket.end('HTTP/1.1 403 Forbidden\r\n' +
        'Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n' + body));
      const req = request(runtime(kind), server.url);
      await new Promise((resolve, reject) => {
        req.on('error', reject).on('upgrade', () => reject(Error('Unexpected upgrade')));
        req.on('response', response => {
          assert.equal(response.statusCode, 403);
          assert.deepEqual(Array.from(response.headers['set-cookie']), ['a=1', 'b=2']);
          const chunks = [];
          response.on('data', bytes => chunks.push(bytes));
          response.on('end', () => {
            assert.equal(Buffer.concat(chunks).toString(), 'nope');
            if (name === 'chunked') assert.equal(response.trailers['x-trail'], 'yes');
            resolve();
          });
        });
        req.end();
      });
    });
  }

  for (const [name, bytes, code] of [
    ['invalid status', 'NONSENSE\r\n\r\n', 'HPE_INVALID_CONSTANT'],
    ['ambiguous framing', 'HTTP/1.1 400 Bad\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n', 'HPE_UNEXPECTED_CONTENT_LENGTH'],
    ['header overflow', 'HTTP/1.1 400 Bad\r\nX: ' + 'a'.repeat(18000), 'HPE_HEADER_OVERFLOW'],
    ['truncated handshake', 'HTTP/1.1 101 Switching', 'ECONNRESET'],
  ]) {
    test(`${kind} rejects ${name} and closes the request exactly once`, async t => {
      const server = await peer(t, socket => socket.end(bytes));
      const req = request(runtime(kind), server.url);
      let errors = 0, closes = 0;
      await new Promise(resolve => {
        req.on('error', reason => { ++errors; assert.equal(reason.code, code); });
        req.on('close', () => { ++closes; resolve(); }); req.end();
      });
      assert.equal(errors, 1); assert.equal(closes, 1);
    });
  }

  test(`${kind} handshake timeout notifies until the caller cancels`, async t => {
    const server = await peer(t, () => {});
    const req = request(runtime(kind), server.url);
    let timeouts = 0;
    await new Promise(resolve => {
      req.on('error', reason => assert.equal(reason.code, 'ETIME'));
      req.on('timeout', () => {
        ++timeouts; req.destroy(Object.assign(Error('timeout'), {code: 'ETIME'}));
      });
      req.on('close', resolve); req.setTimeout(10); req.end();
    });
    assert.equal(timeouts, 1);
  });

  test(`${kind} cancel before connect never opens a socket`, async t => {
    const server = await peer(t, () => assert.fail('Canceled request reached server'));
    const req = request(runtime(kind), server.url);
    let closes = 0;
    req.on('close', () => ++closes); req.end(); req.destroy();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(closes, 1); assert.equal(server.seen, '');
  });

  test(`${kind} AbortSignal cancels an incomplete handshake with its reason`, async t => {
    const controller = new AbortController();
    let accepted;
    const connected = new Promise(resolve => { accepted = resolve; });
    const server = await peer(t, socket => {
      socket.write('HTTP/1.1 101 Switching'); accepted();
    });
    const req = request(runtime(kind), server.url, {signal: controller.signal});
    const reason = Object.assign(new Error('canceled handshake'), {code: 'ECANCELED'});
    let errors = 0;
    const closed = new Promise(resolve => {
      req.on('error', actual => { ++errors; assert.equal(actual, reason); });
      req.once('close', resolve);
    });
    req.end(); await connected; controller.abort(reason); await closed;
    assert.equal(errors, 1);
  });

  test(`${kind} an unhandled upgrade releases the socket and request`, async t => {
    let peerClosed;
    const remoteClosed = new Promise(resolve => { peerClosed = resolve; });
    const server = await peer(t, socket => {
      socket.once('close', peerClosed); socket.write(responseHead);
    });
    const req = request(runtime(kind), server.url);
    await new Promise((resolve, reject) => {
      req.on('error', reject).once('close', resolve); req.end();
    });
    await remoteClosed;
  });

  test(`${kind} non-101 paused response resumes without losing body bytes`, async t => {
    const body = Buffer.alloc(160 * 1024, 0xa5);
    const server = await peer(t, socket => socket.end(Buffer.concat([
      Buffer.from('HTTP/1.1 403 Forbidden\r\nContent-Length: ' + body.length + '\r\n\r\n'), body,
    ])));
    const req = request(runtime(kind), server.url);
    let finishes = 0;
    await new Promise((resolve, reject) => {
      req.on('error', reject).on('response', response => {
        response.pause();
        const chunks = [];
        response.on('data', bytes => chunks.push(bytes));
        response.on('end', () => { assert.deepEqual(Buffer.concat(chunks), body); resolve(); });
        setTimeout(() => response.resume(), 5);
      });
      req.end(() => ++finishes);
    });
    assert.equal(finishes, 1);
  });

  test(`${kind} header mutations and custom connection use the real endpoint`, async t => {
    const server = await peer(t, socket => socket.write(responseHead));
    let options;
    const req = request(runtime(kind), server.url, {headers: {}, createConnection(value) {
      options = value; return net.connect(value);
    }});
    req.setHeader('Upgrade', 'x-echo'); req.setHeader('Connection', 'Upgrade');
    await new Promise((resolve, reject) => {
      req.on('error', reject).on('upgrade', (_response, socket) => { socket.destroy(); resolve(); }); req.end();
    });
    assert.equal(options.host, '127.0.0.1'); assert.equal(String(options.port), new URL(server.url).port);
  });

  test(`${kind} TLS upgrade rejects before creating any plaintext socket`, async () => {
    let connections = 0;
    const req = request(runtime(kind), 'https://127.0.0.1:1', {
      createConnection() { ++connections; },
    }, true);
    await new Promise(resolve => {
      req.on('error', reason => assert.equal(reason.code, 'ERR_NOT_SUPPORTED')).on('close', resolve); req.end();
    });
    assert.equal(connections, 0);
  });

  test(`${kind} 101 detection, Basic auth and header limits match Node`, async t => {
    const api = runtime(kind), original = {http: nodeHttp, https: nodeHttps};
    const fullHeaders = 'Upgrade: x-echo\r\nConnection: Upgrade\r\nX-A: 1\r\nX-B: 2\r\n';
    const cases = [
      ['', {}, ''], ['Upgrade: x-echo\r\n', {}, ''], ['Connection: Upgrade\r\n', {}, ''],
      [fullHeaders, {}, 'us%65r:p%40ss@'],
      [fullHeaders, {auth: 'foo:bar'}, 'u:p@'],
      [fullHeaders, {auth: null}, 'u:p@'],
      [fullHeaders, {auth: 'foo:bar', headers: {Connection: 'Upgrade', Upgrade: 'x-echo', Authorization: 'Bearer explicit'}}, 'u:p@'],
      [fullHeaders, {maxHeadersCount: 1}, ''],
      [fullHeaders, {}, '', 1], [fullHeaders, {}, '', 0], [fullHeaders, {}, '', -1],
      [fullHeaders, {}, '', '2'], [fullHeaders, {}, '', 1.5],
      [fullHeaders, {maxHeaderSize: 0}, ''],
      [fullHeaders + Array.from({length: 40}, (_,i) => 'X-' + i + ': 1\r\n').join(''), {}, '', 1],
      [fullHeaders + Array.from({length: 1100}, (_,i) => 'X-' + i + ': 1\r\n').join(''), {}, ''],
    ];
    for (const args of cases) {
      const expected = await responseSnapshot(t, original, ...args);
      const actual = await responseSnapshot(t, api, ...args);
      assert.deepEqual(actual, expected, JSON.stringify(args));
    }
  });

  test(`${kind} invalid maxHeaderSize reports Node's error code`, async () => {
    for (const value of [-1, 1.5, '16', null, NaN, Infinity]) {
      let expected;
      try { nodeHttp.request({maxHeaderSize: value}); } catch (reason) { expected = reason.code; }
      assert.ok(expected);
      const req = request(runtime(kind), 'http://127.0.0.1:1', {maxHeaderSize: value});
      await new Promise(resolve => {
        req.on('error', reason => assert.equal(reason.code, expected)).on('close', resolve); req.end();
      });
    }
  });
}
