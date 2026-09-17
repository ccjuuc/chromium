// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Shared HTTP framing over real Node TCP sockets. This does not replace the
// Browser/native transport integration probe.
const assert = require('node:assert/strict');
const {EventEmitter, once} = require('node:events');
const fs = require('node:fs');
const nodeHttp = require('node:http');
const net = require('node:net');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return {promise, resolve};
}

async function bounded(promise, message = 'TCP operation timed out') {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(message)), 1500);
    })]);
  } finally { clearTimeout(timer); }
}

function load(kind) {
  if (kind === 'node-http') return nodeHttp;
  const context = vm.createContext({Buffer, EventEmitter, Uint8Array, ArrayBuffer,
    SharedArrayBuffer, TextEncoder, TextDecoder, atob, btoa, console,
    queueMicrotask, setTimeout, clearTimeout});
  if (kind !== 'node') {
    for (const name of ['events', 'buffer']) {
      vm.runInContext(fs.readFileSync(path.join(__dirname, kind, `${name}.js`), 'utf8'), context);
    }
  }
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/http_server.js'), 'utf8'), context);
  return vm.runInContext('createHttpServerModule', context)(net);
}

function loadWired(kind) {
  const response = {statusCode: 200, statusMessage: 'OK', headers: {},
    httpVersion: '1.1', bodyBase64: Buffer.from('client fixture').toString('base64')};
  const context = vm.createContext({Buffer, EventEmitter, Uint8Array, ArrayBuffer,
    SharedArrayBuffer, TextEncoder, TextDecoder, atob, btoa, console, URL, URLSearchParams,
    queueMicrotask, setTimeout, clearTimeout, netModule: net,
    __xenonHttpRequest: () => ({id: 1, promise: Promise.resolve(response)}),
    __xenonHttpAbort() {}, transport: {invoke: async () => response},
    builtinModuleNames: new Set(['http', 'https']), builtinModuleCache: new Map(),
    hostedCjsCache: Object.create(null)});
  for (const name of ['events', 'buffer']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, kind, `${name}.js`), 'utf8'), context);
  }
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/http_server.js'), 'utf8'), context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, kind,
    kind === 'main' ? 'http.js' : 'modules.js'), 'utf8'), context);
  return kind === 'main' ? vm.runInContext('({http: httpModule, https: httpsModule})', context) :
    {http: context.require('http'), https: context.require('https')};
}

async function fixture(t, kind, listener, options = {}) {
  const server = (typeof kind === 'string' ? load(kind) : kind).createServer(options, listener);
  const clients = new Set();
  t.after(async () => {
    for (const client of clients) client.destroy();
    server.closeAllConnections();
    if (server.listening) {
      await bounded(new Promise(resolve => server.close(resolve)), 'server cleanup timed out');
    }
  });
  server.listen(0, '127.0.0.1');
  await bounded(once(server, 'listening'));
  const connect = async () => {
    const accepted = once(server, 'connection');
    const client = net.createConnection(server.address().port, '127.0.0.1');
    clients.add(client);
    const output = [];
    client.on('data', data => output.push(data));
    client.on('error', () => {}); // Malformed requests may reset a closing peer.
    const closed = new Promise(resolve => client.once('close', resolve));
    await bounded(once(client, 'connect'));
    const [peer] = await bounded(accepted);
    return {client, peer, closed, raw: () => Buffer.concat(output),
      // Wait for each frame to reach the server before sending the next one;
      // TCP coalescing cannot turn the split-byte fixtures into one packet.
      frame: async data => {
        const received = once(peer, 'data');
        client.write(data);
        await bounded(received);
      },
      until: async predicate => {
        if (predicate(Buffer.concat(output))) return;
        await bounded(new Promise((resolve, reject) => {
          const inspect = () => {
            if (!predicate(Buffer.concat(output))) return;
            cleanup(); resolve();
          };
          const closedEarly = () => { cleanup(); reject(new Error('peer closed before response')); };
          const cleanup = () => { client.off('data', inspect); client.off('close', closedEarly); };
          client.on('data', inspect); client.on('close', closedEarly);
        }));
      }};
  };
  return {server, connect};
}

function responseBody(raw) {
  const end = raw.indexOf('\r\n\r\n');
  assert.notEqual(end, -1, 'response contains headers');
  assert.match(raw.subarray(0, end).toString(), /^HTTP\/1\.1 200 /);
  return raw.subarray(end + 4);
}

for (const kind of ['node', 'main', 'renderer']) {
  test(`${kind}: deferred body consumer preserves split UTF-8 bytes and end order`, async t => {
    const received = deferred();
    let request, response;
    const {connect} = await fixture(t, kind, (req, res) => {
      request = req; response = res; received.resolve();
    });
    const wire = await connect();
    const text = 'A中🙂尾B';
    const body = Buffer.from(text);
    await wire.frame(`POST /unicode HTTP/1.1\r\nHost: fixture\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`);
    await bounded(received.promise);
    for (const byte of body) await wire.frame(Buffer.from([byte]));
    assert.equal(request.complete, true);
    const events = [], chunks = [];
    request.setEncoding('utf8');
    request.on('data', chunk => { chunks.push(chunk); events.push('data'); });
    request.on('end', () => { events.push('end'); response.end(chunks.join('')); });
    request.on('close', () => events.push('close'));
    assert.deepEqual(events, [], 'attaching data listener must not drain inline');
    await bounded(wire.closed);
    assert.equal(chunks.join(''), text);
    assert.deepEqual(events.slice(-2), ['end', 'close']);
    assert.equal(events.filter(name => name === 'end').length, 1);
    assert.equal(request.readableEnded, true);
    assert.deepEqual(responseBody(wire.raw()), body);
    assert.match(wire.raw().toString(), new RegExp(`Content-Length: ${body.length}\\r\\n`, 'i'));
  });

  test(`${kind}: chunked body and trailers survive framing boundaries`, async t => {
    let request;
    const chunks = [];
    const {connect} = await fixture(t, kind, (req, res) => {
      request = req;
      req.on('data', data => chunks.push(Buffer.from(data)));
      req.on('end', () => res.end(Buffer.concat(chunks)));
    });
    const wire = await connect();
    await wire.frame('POST /chunked HTTP/1.1\r\nHost: fixture\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n');
    for (const frame of ['2;fixture=yes\r', '\n', 'ab', '\r', '\n3\r', '\n', 'cde', '\r\n0\r\n', 'X-Trailer: done\r', '\n\r', '\n']) {
      await wire.frame(frame);
    }
    await bounded(wire.closed);
    assert.equal(responseBody(wire.raw()).toString(), 'abcde');
    assert.equal(request.complete, true);
    assert.equal(request.trailers['x-trailer'], 'done');
    assert.deepEqual(Array.from(request.rawTrailers), ['X-Trailer', 'done']);
  });

  test(`${kind}: keep-alive serial requests retain a single connection`, async t => {
    const paths = [];
    const {server, connect} = await fixture(t, kind, (req, res) => {
      paths.push(req.url); res.end(`body:${req.url}`);
    });
    const wire = await connect();
    await wire.frame('GET /one HTTP/1.1\r\nHost: fixture\r\n\r\n');
    await wire.until(raw => raw.includes('body:/one'));
    assert.equal(server._connections.size, 1);
    await wire.frame('GET /two HTTP/1.1\r\nHost: fixture\r\nConnection: close\r\n\r\n');
    await bounded(wire.closed);
    assert.deepEqual(paths, ['/one', '/two']);
    assert.equal((wire.raw().toString().match(/HTTP\/1\.1 200 /g) || []).length, 2);
    assert.ok(wire.raw().indexOf('body:/one') < wire.raw().indexOf('body:/two'));
  });

  test(`${kind}: pipelined responses wait for earlier asynchronous response`, async t => {
    const first = deferred();
    const paths = [];
    const {connect} = await fixture(t, kind, (req, res) => {
      paths.push(req.url);
      if (req.url === '/one') first.promise.then(() => { res.write('first'); res.end('-done'); });
      else res.end('second');
    });
    const wire = await connect();
    await wire.frame('GET /one HTTP/1.1\r\nHost: fixture\r\n\r\nGET /two HTTP/1.1\r\nHost: fixture\r\nConnection: close\r\n\r\n');
    assert.deepEqual(paths, ['/one']);
    first.resolve();
    await bounded(wire.closed);
    const output = wire.raw().toString();
    assert.deepEqual(paths, ['/one', '/two']);
    assert.match(output, /Transfer-Encoding: chunked/i);
    const firstBody = output.indexOf('5\r\nfirst\r\n5\r\n-done\r\n0\r\n\r\n');
    assert.notEqual(firstBody, -1);
    assert.ok(firstBody < output.indexOf('second'));
    assert.equal((output.match(/HTTP\/1\.1 200 /g) || []).length, 2);
  });

  test(`${kind}: an unread large body pauses TCP at the request high water mark`, async t => {
    const requestSeen = deferred();
    let request, response;
    const {connect} = await fixture(t, kind, (req, res) => {
      request = req; response = res; requestSeen.resolve();
    });
    const wire = await connect();
    const paused = once(wire.peer, 'pause');
    const body = Buffer.alloc(2 * 1024 * 1024, 0x6b);
    wire.client.write(`POST /slow-consumer HTTP/1.1\r\nHost: fixture\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`);
    wire.client.write(body);
    await bounded(Promise.all([requestSeen.promise, paused]));
    assert.equal(request.readableFlowing, null);
    assert.equal(wire.peer.isPaused(), true);
    assert.equal(request.complete, false);
    assert.equal(request.readableLength, request.readableHighWaterMark);
    const received = [];
    request.on('data', chunk => received.push(Buffer.from(chunk)));
    request.on('end', () => response.end('consumed'));
    await bounded(wire.closed, 'resuming an unread request did not resume TCP');
    assert.deepEqual(Buffer.concat(received), body);
    assert.equal(request.readableLength, 0);
    assert.equal(responseBody(wire.raw()).toString(), 'consumed');
  });

  test(`${kind}: explicit pause survives listener attachment and resumes buffered parser data`, async t => {
    const requestSeen = deferred();
    let request;
    const chunks = [];
    const {connect} = await fixture(t, kind, (req, res) => {
      request = req; req.pause();
      req.on('data', chunk => chunks.push(Buffer.from(chunk)));
      req.on('end', () => res.end(Buffer.concat(chunks)));
      requestSeen.resolve();
    });
    const wire = await connect();
    await wire.frame('POST /paused HTTP/1.1\r\nHost: fixture\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello');
    await bounded(requestSeen.promise);
    assert.equal(request.readableFlowing, false);
    assert.equal(wire.peer.isPaused(), true);
    assert.deepEqual(chunks, []);
    request.resume();
    await bounded(wire.closed);
    assert.equal(responseBody(wire.raw()).toString(), 'hello');
  });

  test(`${kind}: repeated consumer pauses retain body bytes and release the next pipelined request`, async t => {
    const timers = new Set();
    t.after(() => timers.forEach(clearTimeout));
    const paths = [], chunks = [];
    let firstEnded = false, pauses = 0, maxQueued = 0;
    const body = Buffer.alloc(1024 * 1024, 0x79);
    const {connect} = await fixture(t, kind, (req, res) => {
      paths.push(req.url);
      if (req.url === '/next') {
        assert.equal(firstEnded, true);
        res.end('second'); return;
      }
      req.on('data', chunk => {
        chunks.push(Buffer.from(chunk));
        req.pause(); ++pauses;
        maxQueued = Math.max(maxQueued, req.readableLength);
        const timer = setTimeout(() => { timers.delete(timer); req.resume(); }, 1);
        timers.add(timer);
      });
      req.on('end', () => { firstEnded = true; res.end('first'); });
    });
    const wire = await connect();
    wire.client.write(`POST /body HTTP/1.1\r\nHost: fixture\r\nContent-Length: ${body.length}\r\n\r\n`);
    wire.client.write(body);
    wire.client.write('GET /next HTTP/1.1\r\nHost: fixture\r\nConnection: close\r\n\r\n');
    await bounded(wire.closed, 'consumer backpressure deadlocked pipelined requests');
    assert.deepEqual(Buffer.concat(chunks), body);
    assert.deepEqual(paths, ['/body', '/next']);
    assert.ok(pauses > 1, 'the consumer paused across multiple TCP body chunks');
    assert.ok(maxQueued <= 65536);
    const raw = wire.raw().toString();
    assert.ok(raw.indexOf('first') < raw.indexOf('second'));
    assert.equal((raw.match(/HTTP\/1\.1 200 /g) || []).length, 2);
  });

  test(`${kind}: pending asynchronous response bounds pipelined input and resumes it in order`, async t => {
    const requestSeen = deferred();
    let firstResponse;
    const paths = [], chunks = [];
    const body = Buffer.alloc(1024 * 1024, 0x70);
    const {server, connect} = await fixture(t, kind, (req, res) => {
      paths.push(req.url);
      if (req.url === '/first') {
        firstResponse = res; requestSeen.resolve(); return;
      }
      req.on('data', chunk => chunks.push(Buffer.from(chunk)));
      req.on('end', () => res.end('second'));
    });
    const wire = await connect();
    const paused = once(wire.peer, 'pause');
    wire.client.write('GET /first HTTP/1.1\r\nHost: fixture\r\n\r\n');
    wire.client.write(`POST /second HTTP/1.1\r\nHost: fixture\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`);
    wire.client.write(body);
    await bounded(Promise.all([requestSeen.promise, paused]));
    assert.deepEqual(paths, ['/first']);
    assert.equal(wire.peer.isPaused(), true);
    const state = [...server._connections.values()][0];
    assert.ok(state.data.length < 2 * 65536, 'pending parser bytes remain bounded to TCP chunks');
    firstResponse.end('first');
    await bounded(wire.closed, 'finishing a response did not release pipelined TCP input');
    assert.deepEqual(paths, ['/first', '/second']);
    assert.deepEqual(Buffer.concat(chunks), body);
    const raw = wire.raw().toString();
    assert.ok(raw.indexOf('first') < raw.indexOf('second'));
  });
}

for (const [label, fields] of [
  ['negative length', 'Content-Length: -1'],
  ['nondecimal length', 'Content-Length: 0x10'],
  ['unsafe length', 'Content-Length: 9007199254740992'],
  ['duplicate length', 'Content-Length: 1\r\nContent-Length: 1'],
  ['length plus transfer', 'Content-Length: 1\r\nTransfer-Encoding: chunked'],
  ['unsupported transfer', 'Transfer-Encoding: gzip'],
  ['repeated transfer', 'Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked'],
]) {
  test(`framing: rejects ${label} before dispatch`, async t => {
    let requests = 0;
    const {connect} = await fixture(t, 'node', () => ++requests);
    const wire = await connect();
    wire.client.write(`POST /bad HTTP/1.1\r\nHost: fixture\r\n${fields}\r\n\r\n`);
    await bounded(wire.closed);
    assert.match(wire.raw().toString(), /^HTTP\/1\.1 400 /);
    assert.equal(requests, 0);
  });
}

for (const terminated of [false, true]) {
  test(`header limit: rejects oversized ${terminated ? 'complete' : 'partial'} headers`, async t => {
    let requests = 0;
    const {connect} = await fixture(t, 'node', () => ++requests, {maxHeaderSize: 96});
    const wire = await connect();
    wire.client.write('GET / HTTP/1.1\r\nX-Long: ' + 'a'.repeat(120) + (terminated ? '\r\n\r\n' : ''));
    await bounded(wire.closed);
    assert.match(wire.raw().toString(), /^HTTP\/1\.1 431 /);
    assert.equal(requests, 0);
  });
}

for (const [label, trailers] of [
  ['single', 'X-Large: ' + 'x'.repeat(200) + '\r\n'],
  ['cumulative', 'X-Trailer: xxxxxxxxxx\r\n'.repeat(9)],
]) {
  test(`header limit: rejects ${label} oversized trailers`, async t => {
    let ended = false;
    const {connect} = await fixture(t, 'node', (req, res) => {
      req.resume(); req.on('end', () => { ended = true; res.end('accepted'); });
    }, {maxHeaderSize: 128});
    const wire = await connect();
    wire.client.write('POST / HTTP/1.1\r\nHost: fixture\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n' + trailers + '\r\n');
    await bounded(wire.closed);
    assert.match(wire.raw().toString(), /^HTTP\/1\.1 431 /);
    assert.equal(ended, false);
  });
}

test('requestTimeout stops once the request is complete, before an async response', async t => {
  const timers = [];
  t.after(() => timers.forEach(clearTimeout));
  const {connect} = await fixture(t, 'node', (req, res) => {
    req.resume();
    req.on('end', () => timers.push(setTimeout(() => res.end('slow response'), 75)));
  }, {requestTimeout: 25, headersTimeout: 25});
  const wire = await connect();
  await wire.frame('GET /slow HTTP/1.1\r\nHost: fixture\r\nConnection: close\r\n\r\n');
  await bounded(wire.closed);
  assert.equal(responseBody(wire.raw()).toString(), 'slow response');
});

test('close waits for an active response then closes its keep-alive socket', async t => {
  const requestSeen = deferred();
  let response;
  const {server, connect} = await fixture(t, 'node', (_req, res) => {
    response = res; requestSeen.resolve();
  });
  const wire = await connect();
  await wire.frame('GET /active HTTP/1.1\r\nHost: fixture\r\n\r\n');
  await bounded(requestSeen.promise);
  let closes = 0;
  const closed = new Promise(resolve => server.close(() => { ++closes; resolve(); }));
  assert.equal(closes, 0);
  response.end('finished');
  await bounded(Promise.all([closed, wire.closed]), 'active HTTP server close leaked a keep-alive socket');
  assert.equal(closes, 1);
  assert.equal(responseBody(wire.raw()).toString(), 'finished');
  assert.doesNotMatch(wire.raw().toString(), /408 Request Timeout/);
});

test('close closes an idle keep-alive connection', async t => {
  const {server, connect} = await fixture(t, 'node', (_req, res) => res.end('idle'));
  const wire = await connect();
  await wire.frame('GET /idle HTTP/1.1\r\nHost: fixture\r\n\r\n');
  await wire.until(raw => raw.includes('idle'));
  await bounded(Promise.all([wire.closed, new Promise(resolve => server.close(resolve))]));
  assert.equal(server.listening, false);
});

test('peer abort closes incomplete request and response and removes connection', async t => {
  const aborted = deferred(), responseClosed = deferred();
  let request, response, ends = 0, closes = 0;
  const {server, connect} = await fixture(t, 'node', (req, res) => {
    request = req; response = res;
    req.on('data', () => {});
    req.on('end', () => ++ends);
    req.on('aborted', () => aborted.resolve());
    req.on('close', () => ++closes);
    res.on('close', () => responseClosed.resolve());
  });
  const wire = await connect();
  await wire.frame('POST /abort HTTP/1.1\r\nHost: fixture\r\nContent-Length: 20\r\n\r\nabc');
  wire.client.destroy();
  await bounded(Promise.all([wire.closed, aborted.promise, responseClosed.promise]));
  assert.equal(request.complete, false);
  assert.equal(request.aborted, true);
  assert.equal(request.destroyed, true);
  assert.equal(response.writableFinished, false);
  assert.equal(ends, 0);
  assert.equal(closes, 1);
  assert.equal(server._connections.size, 0);
});

for (const kind of ['node-http', 'node', 'main', 'renderer']) {
  test(`${kind}: header controls cannot be hidden by optional whitespace trimming`, async t => {
    let requests = 0;
    const {connect} = await fixture(t, kind, (req, res) => { ++requests; res.end('unexpected'); });
    for (const fields of ['Content-Length: \x0b1', 'Content-Length: 1\x0c',
      'Transfer-Encoding: \x0bchunked', 'X-Fixture: value\x0b']) {
      const wire = await connect();
      wire.client.write(`POST /invalid-header HTTP/1.1\r\nHost: fixture\r\n${fields}\r\nConnection: close\r\n\r\nx`);
      await bounded(wire.closed);
      assert.match(wire.raw().toString(), /^HTTP\/1\.1 400 /);
    }
    assert.equal(requests, 0);
  });

  test(`${kind}: SP and HTAB remain valid optional header whitespace`, async t => {
    const {connect} = await fixture(t, kind, (req, res) => {
      req.resume(); req.on('end', () => res.end(req.headers['content-length']));
    });
    const wire = await connect();
    wire.client.write('POST /ows HTTP/1.1\r\nHost: fixture\r\nContent-Length: \t 1 \t\r\nConnection: close\r\n\r\nx');
    await bounded(wire.closed);
    assert.equal(responseBody(wire.raw()).toString(), '1');
  });

  test(`${kind}: peer abort closes a complete but unread request exactly once`, async t => {
    let request, ends = 0, closes = 0, aborts = 0;
    const requestClosed = deferred(), responseClosed = deferred();
    const {connect} = await fixture(t, kind, (req, res) => {
      request = req;
      req.on('error', () => {}); // Node reports ECONNRESET for unread requests.
      req.on('end', () => ++ends);
      req.on('aborted', () => ++aborts);
      req.on('close', () => { ++closes; requestClosed.resolve(); });
      res.on('close', () => responseClosed.resolve());
    });
    const wire = await connect();
    await wire.frame('POST /unread HTTP/1.1\r\nHost: fixture\r\nContent-Length: 3\r\n\r\nabc');
    assert.equal(request.complete, true);
    assert.equal(request.readableEnded, false);
    wire.client.destroy();
    await bounded(Promise.all([wire.closed, requestClosed.promise, responseClosed.promise]));
    assert.equal(request.aborted, true);
    assert.equal(request.destroyed, true);
    assert.equal(request.readable, false);
    assert.equal(aborts, 1);
    assert.equal(closes, 1);
    assert.equal(ends, 0);
    if (kind !== 'node-http') assert.equal(request.readableLength, 0, 'release bridge-owned queued bytes');
    request.resume();
    await Promise.resolve();
    assert.equal(ends, 0);
    assert.equal(closes, 1);
  });

  test(`${kind}: supported byte encodings preserve split non-ASCII bytes`, async t => {
    for (const encoding of ['ASCII', 'latin1', 'binary', 'HEX']) {
      const chunks = [];
      const body = Buffer.from([0, 127, 128, 255]);
      const {connect} = await fixture(t, kind, (req, res) => {
        req.setEncoding(encoding);
        req.on('data', text => chunks.push(text));
        req.on('end', () => res.end('decoded'));
      });
      const wire = await connect();
      await wire.frame(`POST /byte-encoding HTTP/1.1\r\nHost: fixture\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`);
      for (const byte of body) await wire.frame(Buffer.from([byte]));
      await bounded(wire.closed);
      assert.equal(chunks.join(''), body.toString(encoding));
    }
  });
}

test('Node reference preserves split UTF-16LE and base64 stream encoding', async t => {
  for (const encoding of ['utf16le', 'base64']) {
    const chunks = [], body = Buffer.from('中🙂', 'utf16le');
    const {connect} = await fixture(t, 'node-http', (req, res) => {
      req.setEncoding(encoding); req.on('data', chunk => chunks.push(chunk));
      req.on('end', () => res.end('decoded'));
    });
    const wire = await connect();
    await wire.frame(`POST /stateful HTTP/1.1\r\nHost: fixture\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`);
    for (const byte of body) await wire.frame(Buffer.from([byte]));
    await bounded(wire.closed);
    assert.equal(chunks.join(''), body.toString(encoding));
  }
});

for (const kind of ['node', 'main', 'renderer']) {
  test(`${kind}: unimplemented stateful request encodings fail explicitly`, () => {
    const socket = new net.Socket();
    const request = new (load(kind).IncomingMessage)(socket);
    try {
      request.setEncoding('utf8');
      for (const encoding of ['utf16le', 'UTF-16LE', 'ucs2', 'ucs-2', 'base64', 'base64url']) {
        assert.throws(() => request.setEncoding(encoding), {code: 'ERR_NOT_SUPPORTED'});
        assert.equal(request._encoding, 'utf8', 'failed configuration keeps the previous decoder');
      }
      assert.throws(() => request.setEncoding('not-an-encoding'), {code: 'ERR_UNKNOWN_ENCODING'});
    } finally { socket.destroy(); }
  });
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: wired HTTP clients and servers share the public IncomingMessage prototype`, async t => {
    const api = loadWired(kind), marker = Symbol('public IncomingMessage prototype');
    api.http.IncomingMessage.prototype[marker] = 'inherited';
    const socket = new net.Socket();
    const constructed = new api.http.IncomingMessage(socket);
    assert.equal(constructed.socket, socket);
    assert.equal(constructed.resume(), constructed);
    await Promise.resolve(); // A public constructor has no parser flow callback.
    socket.destroy();
    const {connect} = await fixture(t, api.http, (req, res) => {
      assert.ok(req instanceof api.http.IncomingMessage);
      assert.equal(req[marker], 'inherited');
      assert.ok(res instanceof api.http.ServerResponse);
      res.end('server fixture');
    });
    const wire = await connect();
    wire.client.write('GET /identity HTTP/1.1\r\nHost: fixture\r\nConnection: close\r\n\r\n');
    await bounded(wire.closed);
    assert.equal(responseBody(wire.raw()).toString(), 'server fixture');
    // Client transport uses a synthetic response; these assertions concern
    // the real public module wiring and object identity, not network I/O.
    for (const protocol of ['http', 'https']) {
      const text = await bounded(new Promise((resolve, reject) => {
        const request = api[protocol].get(`${protocol}://fixture.invalid/`, incoming => {
          try {
            assert.ok(incoming instanceof api.http.IncomingMessage);
            assert.equal(incoming[marker], 'inherited');
            incoming.setEncoding('utf8');
            const chunks = [];
            incoming.on('data', chunk => chunks.push(chunk));
            incoming.once('end', () => resolve(chunks.join('')));
          } catch (error) { reject(error); }
        });
        request.on('error', reject);
      }));
      assert.equal(text, 'client fixture');
    }
  });
}
