const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const {isIP} = require('node:net');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function runtime(override) {
  const code = fs.readFileSync(path.join(__dirname, 'main/dgram.js'), 'utf8');
  const factory = vm.runInNewContext(code + '\ncreateDgramModule', {queueMicrotask, Buffer});
  let emit, nextId = 1;
  const calls = [], states = new Map();
  const module = factory({EventEmitter, Buffer, isIP, dispatch: value => { emit = value; },
    callNative(request) {
      calls.push(request);
      if (override) { const result = override(request, value => emit(value)); if (result !== undefined) return result; }
      if (request.op === 'create') { const id = nextId++; states.set(id, {}); return {value: id}; }
      if (request.op === 'bind') states.get(request.id).local = {address: request.address, port: request.port || 44444, family: 'IPv4'};
      if (request.op === 'address') return {value: states.get(request.id).local};
      if (request.op === 'remoteAddress') return {value: states.get(request.id).remote};
      if (request.op === 'connect') states.get(request.id).remote = {address: request.address, port: request.port, family: 'IPv4'};
      if (request.op === 'lookup') queueMicrotask(() => emit({id: request.id, type: 'lookup', lookupId: request.lookupId, result: {address: '127.0.0.1'}}));
      if (request.op === 'send') queueMicrotask(() => emit({id: request.id, type: 'sent', sendId: request.sendId, bytes: request.bytes.length}));
      if (request.op === 'close') queueMicrotask(() => emit({id: request.id, type: 'close'}));
      return {value: null};
    }});
  return {module, calls, emit: value => emit(value)};
}
const bind = socket => new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(0, '127.0.0.1', resolve); });
const close = socket => new Promise(resolve => socket.close(resolve));

test('create validates before allocating and propagates native availability', () => {
  const {module, calls} = runtime();
  for (const option of [undefined, 'tcp', {}, {type: 'udp4', lookup: 2}, {type: 'udp4', recvBufferSize: -1}])
    assert.throws(() => module.createSocket(option));
  assert.equal(calls.length, 0);
  const unavailable = runtime(() => ({error: 'ERR_NOT_SUPPORTED: unavailable'}));
  assert.throws(() => unavailable.module.createSocket('udp4'), {code: 'ERR_NOT_SUPPORTED'});
});

test('implicit bind and native hostname lookup precede binary send acknowledgement', async () => {
  const {module, calls} = runtime();
  const socket = module.createSocket('udp4');
  const order = [];
  socket.on('listening', () => order.push('listening'));
  let returned = false;
  const sent = new Promise((resolve, reject) => {
    assert.equal(socket.send([Buffer.from([0, 255]), new Uint8Array([128])], 5678, 'localhost', (err, count) => {
      assert.equal(returned, true); if (err) reject(err); else { order.push('sent'); resolve(count); }
    }), undefined);
  });
  returned = true;
  assert.equal(await sent, 3);
  assert.deepEqual(order, ['listening', 'sent']);
  assert.deepEqual(calls.map(value => value.op), ['create', 'bind', 'lookup', 'send']);
  assert.deepEqual([...calls.at(-1).bytes], [0, 255, 128]);
  assert.equal(calls.at(-1).port, 5678);
  assert.equal(calls.at(-1).address, '127.0.0.1');
  await close(socket);
});

test('send callbacks receive OS errors and malformed calls cannot submit bytes', async () => {
  const {module, calls} = runtime(request => request.op === 'send' ? {error: 'EMSGSIZE: send too large'} : undefined);
  const socket = module.createSocket('udp4');
  for (const invoke of [() => socket.send({}, 10), () => socket.send('a', -1),
    () => socket.send('abc', -1, 2, 10), () => socket.send('abc', 2, 3, 10)]) assert.throws(invoke);
  assert.equal(calls.length, 1);
  const err = await new Promise(resolve => socket.send('x', 10, resolve));
  assert.equal(err.code, 'EMSGSIZE');
  await close(socket);
});

test('connected datagrams omit a destination and support native disconnect', async () => {
  const {module, calls} = runtime();
  const socket = module.createSocket('udp4');
  await new Promise(resolve => socket.connect(1234, '127.0.0.1', resolve));
  assert.equal(socket.remoteAddress().port, 1234);
  assert.throws(() => socket.connect(4444), {code: 'ERR_SOCKET_DGRAM_IS_CONNECTED'});
  assert.equal(await new Promise((resolve, reject) => socket.send('abcd', 1, 2, (err, count) => err ? reject(err) : resolve(count))), 2);
  const sent = calls.find(value => value.op === 'send');
  assert.equal(sent.address, undefined); assert.equal(sent.port, undefined);
  assert.equal(sent.bytes.toString(), 'bc');
  socket.disconnect();
  assert.throws(() => socket.remoteAddress(), {code: 'ERR_SOCKET_DGRAM_NOT_CONNECTED'});
  assert.ok(calls.some(value => value.op === 'disconnect'));
  await close(socket);
});

test('close cancels pending hostname sends and late lookup cannot revive the socket', async () => {
  let lookup;
  const {module, calls, emit} = runtime(request => {
    if (request.op === 'lookup') { lookup = request; return {value: null}; }
  });
  const socket = module.createSocket('udp4');
  await bind(socket);
  let callbacks = 0;
  const finished = new Promise(resolve => socket.send('pending', 55, 'delayed.test', err => { ++callbacks; resolve(err); }));
  assert.ok(lookup);
  await close(socket);
  assert.equal((await finished).code, 'ERR_SOCKET_DGRAM_NOT_RUNNING');
  emit({id: lookup.id, type: 'lookup', lookupId: lookup.lookupId, result: {address: '127.0.0.1'}});
  assert.equal(callbacks, 1);
  assert.equal(calls.some(value => value.op === 'send'), false);
  assert.throws(() => socket.send('late', 55), {code: 'ERR_SOCKET_DGRAM_NOT_RUNNING'});
});

test('bind failure is observable, retryable and does not emit listening', async () => {
  let first = true;
  const {module} = runtime(request => {
    if (request.op === 'bind' && first) { first = false; return {error: 'EADDRINUSE: bind failed'}; }
  });
  const socket = module.createSocket('udp4');
  let listening = 0;
  socket.on('listening', () => ++listening);
  const failed = new Promise(resolve => socket.once('error', resolve));
  socket.bind(5555);
  assert.equal((await failed).code, 'EADDRINUSE'); assert.equal(listening, 0);
  await bind(socket); assert.equal(listening, 1);
  await close(socket);
});

test('multicast and socket options preserve arguments and native failure', async () => {
  const {module, calls} = runtime(request => request.op === 'dropMembership' ? {error: 'EADDRNOTAVAIL: membership missing'} : undefined);
  const socket = module.createSocket({type: 'udp4', reuseAddr: true, recvBufferSize: 32768});
  assert.throws(() => socket.address(), {code: 'ERR_SOCKET_DGRAM_NOT_RUNNING'});
  await bind(socket);
  assert.equal(calls.find(value => value.op === 'bind').reuseAddr, true);
  socket.addMembership('239.255.1.1', '127.0.0.1');
  socket.setMulticastInterface('127.0.0.1'); socket.setMulticastTTL(5); socket.setMulticastLoopback(true);
  socket.setBroadcast(true); socket.setTTL(60); socket.unref().ref();
  assert.throws(() => socket.dropMembership('239.255.1.1'), {code: 'EADDRNOTAVAIL'});
  assert.equal(calls.find(value => value.op === 'addMembership').group, '239.255.1.1');
  assert.equal(calls.find(value => value.op === 'setRecvBufferSize').value, 32768);
  assert.throws(() => socket.setTTL(256), {code: 'ERR_OUT_OF_RANGE'});
  await close(socket);
});

test('incoming datagrams stay separate and expose Buffer bytes with rinfo', async () => {
  const {module, emit} = runtime();
  const messages = [];
  const socket = module.createSocket('udp4', (bytes, rinfo) => messages.push({bytes, rinfo}));
  await bind(socket);
  const rinfo = {address: '127.0.0.1', family: 'IPv4', port: 89, size: 2};
  emit({id: 1, type: 'message', bytes: new Uint8Array([0, 255]), rinfo});
  emit({id: 1, type: 'message', bytes: new Uint8Array(), rinfo: {...rinfo, size: 0}});
  assert.equal(messages.length, 2); assert.ok(Buffer.isBuffer(messages[0].bytes));
  assert.deepEqual([...messages[0].bytes], [0, 255]); assert.equal(messages[1].bytes.length, 0);
  await close(socket);
});

test('main bootstrap carries datagram bytes separately from JSON metadata', async () => {
  const payloads = [];
  const context = vm.createContext({TextEncoder, TextDecoder, URL, URLSearchParams,
    queueMicrotask, setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}}, __xenonPlatform: 'win32',
    __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonDgramCall(metadata, binary) {
      assert.equal(Object.hasOwn(metadata, 'bytes'), false);
      if (metadata.op === 'create') return {value: 1};
      if (metadata.op === 'send') {
        assert.ok(ArrayBuffer.isView(binary)); payloads.push([...binary]);
        queueMicrotask(() => context.__xenonDgramEvent({id: 1, type: 'sent', sendId: metadata.sendId, bytes: binary.byteLength}));
      }
      if (metadata.op === 'close') queueMicrotask(() => context.__xenonDgramEvent({id: 1, type: 'close'}));
      return {value: null};
    }});
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')), context);
  const socket = context.__xenonDgram.createSocket('udp4');
  const count = await new Promise((resolve, reject) => socket.send(new Uint8Array([0, 255, 128]), 55,
    '127.0.0.1', (err, size) => err ? reject(err) : resolve(size)));
  assert.equal(count, 3); assert.deepEqual(payloads, [[0, 255, 128]]);
  await close(socket);
});
