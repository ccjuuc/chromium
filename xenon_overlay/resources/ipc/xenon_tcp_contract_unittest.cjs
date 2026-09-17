// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const vm = require('node:vm');

function fixture(kind) {
  const sent = [];
  const send = (channel, payload) => sent.push({channel, payload});
  const context = vm.createContext({EventEmitter, Buffer, Uint8Array, queueMicrotask,
    setTimeout, clearTimeout, process: {platform: 'win32'}, __xenonPlatform: 'win32',
    transport: {send}, __xenonNetSend: send, console: {info() {}}});
  vm.runInContext('let dispatchXenonNet;\n' + readBootstrapPart('main/async_context.js') +
    readBootstrapPart(`${kind}/net.js`) + '\nglobalThis.net = netModule; ' +
    'globalThis.deliver = dispatchXenonNet;', context);
  return {net: context.net, sent, deliver: (event, payload) =>
    context.deliver('__xenon:net:' + event, payload)};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind} TCP listen/address/connect always use native transport`, () => {
    const {net, sent, deliver} = fixture(kind);
    let listening = false;
    const server = net.createServer().unref().listen(0, '127.0.0.1', () => { listening = true; });
    assert.equal(listening, false);
    assert.equal(server.address(), null);
    assert.equal(sent[0].payload.port, 0);
    const address = {address: '127.0.0.1', port: 41000, family: 'IPv4'};
    deliver('listening', {serverId: sent[0].payload.serverId, address});
    assert.equal(listening, true);
    assert.equal(server.address(), address);
    const socket = net.connect(41000, '127.0.0.1');
    assert.equal(sent.at(-1).channel, '__xenon:net:connect');
    assert.equal(sent.at(-1).payload.port, 41000);
    assert.equal(socket.connecting, true);
    socket.destroy();
    server.close();
  });

  test(`${kind} TCP write and end callbacks wait for OS completion, and reads survive end`, () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect(1234, '127.0.0.1');
    const id = sent[0].payload.fromId;
    const events = [];
    deliver('connected', {toId: id, peerId: 'native-peer',
      local: {address: '127.0.0.1', port: 40000, family: 'IPv4'},
      remote: {address: '127.0.0.1', port: 1234, family: 'IPv4'}});
    socket.on('data', bytes => events.push(bytes.toString()));
    socket.write(Buffer.from([0, 255]), error => { assert.ifError(error); events.push('write'); });
    const write = sent.at(-1).payload;
    socket.end('tail', () => events.push('finish'));
    assert.deepEqual(events, []);
    assert.equal(sent.at(-1).channel, '__xenon:net:end');
    deliver('written', {toId: id, writeId: write.writeId});
    assert.deepEqual(events, ['write']);
    deliver('finish', {toId: id});
    deliver('data', {toId: id, wire: {t: 's', d: 'reply'}});
    assert.deepEqual(events, ['write', 'finish', 'reply']);
    assert.equal(socket.remotePort, 1234);
    assert.equal(socket.address().port, 40000);
    socket.destroy();
  });

  test(`${kind} queued binary write snapshots flush before a pending end`, () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect({port: 1234, host: '::1'});
    const id = sent[0].payload.fromId;
    const bytes = Buffer.from([1, 2, 3]);
    socket.write(bytes);
    socket.end();
    bytes.fill(9);
    deliver('connected', {toId: id, peerId: 'native-peer'});
    assert.equal(sent[1].channel, '__xenon:net:data');
    assert.deepEqual([...Buffer.from(sent[1].payload.wire.d, 'base64')], [1, 2, 3]);
    assert.equal(sent[2].channel, '__xenon:net:end');
    socket.destroy();
  });

  test(`${kind} failed native TCP bind emits the OS error without reporting listening`, () => {
    const {net, sent, deliver} = fixture(kind);
    const server = net.createServer().listen(1234);
    let failure;
    server.on('error', error => { failure = error.code; });
    deliver('error', {serverId: sent[0].payload.serverId, code: 'EADDRINUSE'});
    assert.equal(failure, 'EADDRINUSE');
    assert.equal(server.listening, false);
    assert.equal(server.address(), null);
  });

  test(`${kind} connect failure completes every queued write callback once`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect(1234);
    const id = sent[0].payload.fromId;
    const failures = [];
    const events = [];
    socket.on('error', error => events.push(error.code));
    socket.on('close', hadError => events.push(hadError));
    socket.write('first', error => failures.push(['first', error.code]));
    socket.write(Buffer.from([0, 255]), error => failures.push(['binary', error.code]));
    deliver('error', {toId: id, code: 'ECONNREFUSED'});
    deliver('close', {toId: id});
    socket.destroy();
    await Promise.resolve();
    assert.deepEqual(events, ['ECONNREFUSED', true]);
    assert.deepEqual(failures, [
      ['first', 'ERR_STREAM_DESTROYED'], ['binary', 'ERR_STREAM_DESTROYED'],
    ]);
    assert.equal(socket.destroyed, true);
  });

  test(`${kind} asynchronous write failure reports error and close even without a write callback`, () => {
    for (const withCallback of [false, true]) {
      const {net, sent, deliver} = fixture(kind);
      const socket = net.connect(1234);
      const id = sent[0].payload.fromId;
      deliver('connected', {toId: id, peerId: 'native-peer'});
      const events = [];
      socket.on('error', error => events.push(['error', error.code]));
      socket.on('close', hadError => events.push(['close', hadError]));
      socket.write('payload', withCallback ?
        error => events.push(['write', error.code]) : undefined);
      if (withCallback) {
        deliver('written', {toId: id, writeId: sent.at(-1).payload.writeId, code: 'EPIPE'});
      }
      // The native bridge sends the socket error independently of write ACKs.
      deliver('error', {toId: id, code: 'EPIPE'});
      deliver('close', {toId: id});
      assert.deepEqual(events, [
        ...(withCallback ? [['write', 'EPIPE']] : []),
        ['error', 'EPIPE'], ['close', true],
      ]);
    }
  });

  test(`${kind} a Socket reconnects after EOF without carrying ended or paused state`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect(1234);
    const firstId = sent[0].payload.fromId;
    deliver('connected', {toId: firstId, peerId: 'first-peer'});
    socket.end();
    deliver('finish', {toId: firstId});
    deliver('end', {toId: firstId});
    deliver('close', {toId: firstId});
    assert.equal(socket.writableEnded, true);
    assert.equal(socket.readableEnded, true);
    socket.connect(2345);
    const nextId = sent.at(-1).payload.fromId;
    assert.notEqual(nextId, firstId);
    assert.equal(socket.writableEnded, false);
    assert.equal(socket.writableFinished, false);
    assert.equal(socket.readableEnded, false);
    assert.equal(socket.isPaused(), false);
    // The previous connection's queued auto-end must not end the new one.
    await Promise.resolve();
    assert.equal(socket.writableEnded, false);
    const events = [];
    socket.on('data', bytes => events.push(bytes.toString()));
    socket.on('finish', () => events.push('finish'));
    assert.equal(socket.write('new request'), true);
    socket.end();
    deliver('connected', {toId: nextId, peerId: 'second-peer'});
    assert.equal(sent.at(-2).channel, '__xenon:net:data');
    assert.equal(sent.at(-2).payload.toId, 'second-peer');
    assert.equal(sent.at(-1).channel, '__xenon:net:end');
    assert.equal(sent.at(-1).payload.toId, 'second-peer');
    deliver('data', {toId: firstId, wire: {t: 's', d: 'stale'}});
    deliver('data', {toId: nextId, wire: {t: 's', d: 'new reply'}});
    deliver('finish', {toId: nextId});
    assert.deepEqual(events, ['new reply', 'finish']);
    socket.pause().destroy();
    socket.connect(3456);
    assert.equal(socket.isPaused(), false);
    socket.destroy();
  });

  test(`${kind} unsupported requested endpoint options fail before native submission`, () => {
    const {net, sent} = fixture(kind);
    for (const options of [
      {ipv6Only: true}, {reusePort: true}, {exclusive: true},
      {backlog: 64},
      {signal: new AbortController().signal},
    ]) {
      assert.throws(() => net.createServer().listen({port: 0, ...options}),
        {code: 'ERR_NOT_SUPPORTED'}, JSON.stringify(Object.keys(options)));
    }
    for (const args of [[0, 32], [0, '::1', 32], [{port: 0}, 32],
      ['\\\\.\\pipe\\custom-backlog', 32]]) {
      assert.throws(() => net.createServer().listen(...args), {code: 'ERR_NOT_SUPPORTED'});
    }
    for (const options of [
      {localAddress: '127.0.0.1'}, {localPort: 32000}, {family: 6}, {hints: 32},
      {lookup() {}}, {autoSelectFamily: true}, {autoSelectFamilyAttemptTimeout: 100},
      {noDelay: true}, {keepAlive: true}, {keepAliveInitialDelay: 1000},
      {timeout: 500}, {signal: new AbortController().signal},
    ]) {
      assert.throws(() => net.connect({port: 1234, ...options}),
        {code: 'ERR_NOT_SUPPORTED'}, JSON.stringify(Object.keys(options)));
    }
    for (const options of [{pauseOnConnect: true}, {highWaterMark: 1024},
      {noDelay: true}, {keepAlive: true}]) {
      assert.throws(() => net.createServer(options), {code: 'ERR_NOT_SUPPORTED'});
    }
    assert.deepEqual(sent, []);
  });

  test(`${kind} pipe listener permissions reach native while TCP ignores them`, () => {
    const {net, sent, deliver} = fixture(kind);
    for (const [readableAll, writableAll] of [[true, false], [false, true],
      [true, true], [false, false], ['true', 1]]) {
      const server = net.createServer().listen({path: '\\\\.\\pipe\\permission-contract',
        readableAll, writableAll});
      const request = sent.at(-1);
      assert.equal(request.channel, '__xenon:net:listen');
      assert.equal(request.payload.readableAll, readableAll === true);
      assert.equal(request.payload.writableAll, writableAll === true);
      assert.equal(server.listening, false);
      let failure;
      server.once('error', error => { failure = error.code; });
      deliver('error', {serverId: request.payload.serverId, code: 'EACCES'});
      assert.equal(failure, 'EACCES');
      assert.equal(server.listening, false);
    }
    const server = net.createServer().listen({port: 0, readableAll: true, writableAll: true});
    assert.equal(sent.at(-1).payload.port, 0);
    assert.equal('readableAll' in sent.at(-1).payload, false);
    assert.equal('writableAll' in sent.at(-1).payload, false);
    server.close();
  });

  test(`${kind} default-valued endpoint options and supported half-open state remain usable`, () => {
    const {net, sent, deliver} = fixture(kind);
    const server = net.createServer({noDelay: false, keepAlive: false, pauseOnConnect: false})
      .listen({port: 0, host: '::1', backlog: 0, ipv6Only: false, reusePort: false,
        exclusive: false, readableAll: false, writableAll: false, applicationTag: 'fixture'});
    assert.equal(sent[0].payload.port, 0);
    const socket = net.connect({port: 1234, host: '127.0.0.1', localPort: 0,
      localAddress: '', family: 0, hints: 0, autoSelectFamily: false,
      keepAlive: false, noDelay: false, timeout: 0, allowHalfOpen: true});
    const id = sent[1].payload.fromId;
    deliver('connected', {toId: id, peerId: 'native-peer'});
    assert.equal(socket.allowHalfOpen, true);
    socket.destroy();
    server.close();
  });

  test(`${kind} paused native reads retain data, EOF and close until resume`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect(1234);
    const id = sent[0].payload.fromId;
    deliver('connected', {toId: id, peerId: 'native-peer'});
    const events = [];
    socket.on('data', bytes => events.push(bytes.toString()));
    socket.on('end', () => events.push('end'));
    socket.on('close', () => events.push('close'));
    assert.equal(socket.pause(), socket);
    assert.equal(socket.isPaused(), true);
    assert.equal(socket.readableFlowing, false);
    assert.equal(sent.at(-1).channel, '__xenon:net:pause');
    assert.equal(sent.at(-1).payload.toId, 'native-peer');
    assert.equal(sent.at(-1).payload.fromId, id);
    for (const data of ['first', 'second']) deliver('data', {toId: id, wire: {t: 's', d: data}});
    deliver('end', {toId: id});
    deliver('close', {toId: id});
    assert.deepEqual(events, []);
    assert.equal(socket.readableLength, 11);
    assert.equal(socket.readableEnded, false);
    assert.equal(socket.destroyed, false);
    assert.equal(socket.resume(), socket);
    assert.deepEqual(events, []);
    await Promise.resolve();
    assert.deepEqual(events, ['first', 'second', 'end', 'close']);
    assert.equal(socket.readableLength, 0);
    assert.equal(socket.readableEnded, true);
    assert.equal(socket.destroyed, true);
    assert.equal(sent.some(entry => entry.channel === '__xenon:net:resume'), false);
  });

  test(`${kind} resumed data listener can pause again without dropping or reordering bytes`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect(1234);
    const id = sent[0].payload.fromId;
    deliver('connected', {toId: id, peerId: 'native-peer'});
    const events = [];
    socket.on('data', bytes => {
      events.push(bytes.toString());
      if (events.length === 1) socket.pause();
    });
    socket.pause();
    for (const data of ['a', 'b', 'c']) deliver('data', {toId: id, wire: {t: 's', d: data}});
    socket.resume();
    await Promise.resolve();
    assert.deepEqual(events, ['a']);
    assert.equal(socket.isPaused(), true);
    assert.equal(socket.readableLength, 2);
    assert.equal(sent.some(entry => entry.channel === '__xenon:net:resume'), false);
    socket.resume();
    await Promise.resolve();
    assert.deepEqual(events, ['a', 'b', 'c']);
    assert.equal(socket.isPaused(), false);
    assert.equal(socket.readableFlowing, true);
    assert.equal(sent.at(-1).channel, '__xenon:net:resume');
    assert.equal(sent.at(-1).payload.toId, 'native-peer');
    assert.equal(sent.at(-1).payload.fromId, id);
    socket.destroy();
  });

  test(`${kind} pause before connect reaches native before callbacks and destroy discards reads`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect(1234, () => {
      assert.equal(sent.at(-1).channel, '__xenon:net:pause');
    }).pause();
    const id = sent[0].payload.fromId;
    assert.equal(sent.length, 1);
    deliver('connected', {toId: id, peerId: 'native-peer'});
    const events = [];
    socket.on('data', () => events.push('data'));
    socket.on('end', () => events.push('end'));
    socket.on('close', () => events.push('close'));
    deliver('data', {toId: id, wire: {t: 's', d: 'discard'}});
    assert.equal(socket.readableLength, 7);
    socket.resume();
    socket.destroy();
    await Promise.resolve();
    assert.deepEqual(events, ['close']);
    assert.equal(socket.readableLength, 0);
    assert.equal(sent.some(entry => entry.channel === '__xenon:net:resume'), false);
  });

  test(`${kind} locally paired pipes queue paused binary data before peer end without native reads`, async () => {
    const {net, sent, deliver} = fixture(kind);
    let incoming;
    const server = net.createServer(socket => { incoming = socket; })
      .listen('\\\\.\\pipe\\paused-contract');
    deliver('listening', {serverId: sent[0].payload.serverId});
    const client = net.connect('\\\\.\\pipe\\paused-contract');
    await Promise.resolve();
    const events = [];
    incoming.pause();
    incoming.on('data', bytes => events.push([...bytes]));
    incoming.on('end', () => events.push('end'));
    incoming.on('close', () => events.push('close'));
    const bytes = Buffer.from([0, 255]);
    client.write(bytes);
    bytes.fill(9);
    client.end();
    assert.deepEqual(events, []);
    incoming.resume();
    await Promise.resolve();
    assert.deepEqual(events, [[0, 255], 'end', 'close']);
    assert.equal(sent.some(entry => /:(pause|resume)$/.test(entry.channel)), false);
    server.close();
  });
}
