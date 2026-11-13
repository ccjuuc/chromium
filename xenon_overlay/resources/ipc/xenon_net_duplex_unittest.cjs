// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const tick = () => new Promise(resolve => setImmediate(resolve));

function fixture(kind) {
  const sent = [];
  let dispatch;
  const send = (channel, value) => sent.push({channel, value});
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, info() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', execPath: process.execPath}),
      setDispatchHandler(callback) { dispatch = callback; }, send,
    },
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '10', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonNetSend: send,
  });
  vm.runInContext(readBootstrap(path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`)), context);
  const net = kind === 'main' ? context.__xenonNet : context.require('net');
  const stream = kind === 'main' ? context.__xenonStream : context.require('stream');
  const deliver = (event, value) => kind === 'main' ?
    context.__xenonDispatchSend({endpointId: '@main'}, '__xenon:net:' + event, [value]) :
    dispatch('__xenon:net:' + event, [value]);
  const connected = options => {
    const socket = net.connect({port: 1234, host: '127.0.0.1', ...options});
    const id = sent.at(-1).value.fromId;
    deliver('connected', {toId: id, peerId: 'native-peer'});
    return {socket, id};
  };
  return {context, net, stream, sent, deliver, connected};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind} connect, listen and close callbacks follow ordinary EventEmitter registration order`, () => {
    const {net, sent, deliver} = fixture(kind);
    const events = [];
    const server = net.createServer().listen(0, () => events.push('listen-callback'));
    server.on('listening', () => events.push('listen-listener'));
    const serverId = sent[0].value.serverId;
    deliver('listening', {serverId});
    const socket = net.connect(1234, () => events.push('connect-callback'));
    socket.on('connect', () => events.push('connect-listener'));
    deliver('connected', {toId: sent.at(-1).value.fromId, peerId: 'native-peer'});
    server.close(() => events.push('close-callback'));
    server.on('close', () => events.push('close-listener'));
    deliver('server-closed', {serverId});
    assert.deepEqual(events, ['listen-callback', 'listen-listener',
      'connect-callback', 'connect-listener', 'close-callback', 'close-listener']);
    socket.destroy();
  });

  test(`${kind} reconnecting from close retains Node unread data and pause state without replaying write callbacks`, async () => {
    const {sent, deliver, connected} = fixture(kind);
    const {socket, id} = connected();
    const callbacks = [];
    const reads = [];
    socket.on('data', data => reads.push(data.toString()));
    socket.pause();
    deliver('data', {toId: id, wire: {t: 's', d: 'old buffered data'}});
    socket.write('pending', error => callbacks.push(error.code));
    const oldWriteId = sent.at(-1).value.writeId;
    socket.once('close', () => socket.connect(4321));
    socket.destroy();
    await tick();
    const newId = sent.at(-1).value.fromId;
    assert.notEqual(newId, id);
    assert.equal(socket.readableLength, 'old buffered data'.length);
    assert.equal(socket.writableLength, 0);
    assert.equal(socket.isPaused(), true);
    deliver('written', {toId: id, writeId: oldWriteId});
    deliver('connected', {toId: newId, peerId: 'second-peer'});
    deliver('data', {toId: newId, wire: {t: 's', d: 'new bytes'}});
    assert.deepEqual(reads, []);
    socket.resume();
    await tick();
    assert.deepEqual(reads, ['old buffered data', 'new bytes']);
    assert.deepEqual(callbacks, ['ERR_STREAM_DESTROYED']);
    socket.destroy();
  });

  test(`${kind} Socket is the exported Duplex and write pressure lasts until native acknowledgement`, async () => {
    const {stream, sent, deliver, connected} = fixture(kind);
    const {socket, id} = connected({writableHighWaterMark: 4});
    assert.ok(socket instanceof stream.Duplex);
    const events = [];
    socket.on('drain', () => events.push('drain'));
    assert.equal(socket.write('abcd', error => { assert.ifError(error); events.push('first'); }), false);
    const first = sent.at(-1).value;
    assert.equal(socket.write('ef', error => { assert.ifError(error); events.push('second'); }), false);
    assert.equal(socket.writableLength, 6);
    assert.equal(sent.filter(x => x.channel === '__xenon:net:data').length, 1);
    deliver('written', {toId: id, writeId: first.writeId});
    assert.equal(socket.writableLength, 2);
    assert.deepEqual(events, ['first']);
    deliver('written', {toId: id, writeId: sent.at(-1).value.writeId});
    await tick();
    assert.equal(socket.writableLength, 0);
    assert.deepEqual(events, ['first', 'drain', 'second']);
    assert.equal(socket.bytesWritten, 6);
    socket.destroy();
  });

  test(`${kind} cork and finalization preserve every byte and wait for both OS acknowledgements`, async () => {
    const {sent, deliver, connected} = fixture(kind);
    const {socket, id} = connected();
    const callbacks = [];
    socket.cork(); socket.cork();
    socket.write('header', error => { assert.ifError(error); callbacks.push('header'); });
    socket.write('body', error => { assert.ifError(error); callbacks.push('body'); });
    assert.equal(socket.writableCorked, 2);
    assert.equal(sent.filter(x => x.channel === '__xenon:net:data').length, 0);
    socket.uncork();
    assert.equal(sent.filter(x => x.channel === '__xenon:net:data').length, 0);
    socket.uncork();
    socket.end(() => callbacks.push('finish'));
    for (const expected of ['header', 'body']) {
      const write = sent.at(-1);
      assert.equal(write.channel, '__xenon:net:data');
      assert.equal(Buffer.from(write.value.wire.d, 'base64').toString(), expected);
      deliver('written', {toId: id, writeId: write.value.writeId});
    }
    assert.equal(sent.at(-1).channel, '__xenon:net:end');
    assert.deepEqual(callbacks, ['header', 'body']);
    deliver('finish', {toId: id});
    await tick();
    assert.deepEqual(callbacks, ['header', 'body', 'finish']);
    assert.equal(socket.writableFinished, true);
    socket.destroy();
  });

  test(`${kind} readable buffering supports unshift and bounded native reads`, async () => {
    const {sent, deliver, connected} = fixture(kind);
    const {socket, id} = connected({readableHighWaterMark: 4});
    deliver('data', {toId: id, wire: {t: 's', d: 'abcd'}});
    assert.equal(socket.readableLength, 4);
    assert.equal(sent.at(-1).channel, '__xenon:net:pause');
    assert.equal(socket.read(2).toString(), 'ab');
    socket.unshift(Buffer.from('XY'));
    assert.equal(socket.read(4).toString(), 'XYcd');
    await tick();
    assert.equal(sent.at(-1).channel, '__xenon:net:resume');
    assert.equal(socket.bytesRead, 4);
    socket.destroy();
  });

  test(`${kind} cancelling an in-flight write releases every callback once and ignores late acks`, async () => {
    const {sent, deliver, connected} = fixture(kind);
    const {socket, id} = connected();
    const completed = [];
    socket.on('error', () => assert.fail('destroy without error must not emit error'));
    socket.write('first', error => completed.push(['first', error.code]));
    const first = sent.at(-1).value.writeId;
    socket.write('second', error => completed.push(['second', error.code]));
    socket.end(error => completed.push(['end', error.code]));
    socket.destroy(); socket.destroy();
    deliver('written', {toId: id, writeId: first});
    deliver('finish', {toId: id});
    await tick();
    assert.deepEqual(completed, [['first', 'ERR_STREAM_DESTROYED'],
      ['second', 'ERR_STREAM_DESTROYED'], ['end', 'ERR_STREAM_DESTROYED']]);
    assert.equal(sent.filter(x => x.channel === '__xenon:net:close').length, 1);
  });

  test(`${kind} pending connect cancellation is explicit and stale connect replies are closed`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = net.connect({port: 1234});
    const id = sent[0].value.fromId;
    const completed = [];
    socket.write('pending', error => completed.push(error.code));
    socket.destroy();
    assert.equal(sent.at(-1).channel, '__xenon:net:cancel');
    assert.equal(sent.at(-1).value.fromId, id);
    deliver('connected', {toId: id, peerId: 'late-peer'});
    assert.equal(sent.at(-1).channel, '__xenon:net:close');
    await tick();
    assert.deepEqual(completed, ['ERR_SOCKET_CLOSED_BEFORE_CONNECTION']);
  });

  test(`${kind} TCP no-delay retains pre-connect settings and submits real native configuration`, () => {
    const {net, sent, deliver} = fixture(kind);
    const socket = new net.Socket();
    assert.equal(socket.setNoDelay(), socket);
    socket.connect(1234);
    const id = sent[0].value.fromId;
    assert.equal(sent.length, 1);
    deliver('connected', {toId: id, peerId: 'native-peer'});
    assert.equal(sent.at(-1).channel, '__xenon:net:no-delay');
    assert.equal(sent.at(-1).value.noDelay, true);
    socket.setNoDelay(false);
    assert.equal(sent.at(-1).value.noDelay, false);
    socket.destroy();
  });

  test(`${kind} a paused local pipe applies Duplex pressure and supports a reply after EOF`, async () => {
    const {net, sent, deliver} = fixture(kind);
    const pipe = String.raw`\\.\pipe\duplex-local-fixture`;
    let incoming;
    const server = net.createServer({allowHalfOpen: true, pauseOnConnect: true,
      highWaterMark: 4}, socket => { incoming = socket; }).listen(pipe);
    deliver('listening', {serverId: sent[0].value.serverId});
    const socket = net.connect({path: pipe, writableHighWaterMark: 4});
    await tick();
    const events = [];
    assert.equal(socket.write('abcdefgh', error => { assert.ifError(error); events.push('write'); }), false);
    socket.end(() => events.push('finish'));
    assert.equal(incoming.readableLength, 8);
    assert.deepEqual(events, []);
    const received = [];
    incoming.on('data', bytes => received.push(bytes.toString()));
    incoming.on('end', () => { events.push('server-end'); incoming.end('reply'); });
    socket.on('data', bytes => events.push(bytes.toString()));
    incoming.resume();
    await tick();
    assert.equal(received.join(''), 'abcdefgh');
    assert.ok(events.includes('write') && events.includes('finish') && events.includes('server-end'));
    assert.ok(events.includes('reply'));
    assert.equal(sent.some(x => /:(data|pause|resume)$/.test(x.channel)), false);
    socket.destroy(); incoming.destroy(); server.close();
  });
}
