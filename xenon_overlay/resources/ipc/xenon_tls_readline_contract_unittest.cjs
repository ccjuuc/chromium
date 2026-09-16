// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const nativeReadline = require('node:readline');
const {Readable} = require('node:stream');
const test = require('node:test');
const vm = require('node:vm');

function loadModules(side) {
  const source = readFileSync(path.join(__dirname,
      `xenon_ipc_${side}_bootstrap.js`), 'utf8');
  const emitterStart = source.indexOf('  function EventEmitter()');
  const emitterEndText = '  EventEmitter.defaultMaxListeners = 10;';
  const emitterEnd = source.indexOf(emitterEndText, emitterStart) + emitterEndText.length;
  const start = source.indexOf('  const tlsModule =');
  const readlineStart = source.indexOf('  const readlineModule =', start);
  const end = source.indexOf('\n  })();', readlineStart) + '\n  })();'.length;
  assert.ok(emitterStart >= 0 && start >= 0 && end > readlineStart);
  // The main isolate has no native incremental TextDecoder. Exercise the
  // production decoder without giving this VM the host's decoder as a crutch.
  const context = vm.createContext({queueMicrotask});
  vm.runInContext(source.slice(emitterStart, emitterEnd) + '\n' +
      source.slice(start, end) + '\nglobalThis.modules = {tlsModule, readlineModule};', context);
  return context.modules;
}

function inputStream() {
  const input = new EventEmitter();
  input.pauseCount = input.resumeCount = 0;
  input.pause = () => { ++input.pauseCount; return input; };
  input.resume = () => { ++input.resumeCount; return input; };
  return input;
}

async function readLines(readline, chunks) {
  const input = Readable.from(chunks);
  const lines = [];
  const rl = readline.createInterface({input, crlfDelay: Infinity});
  rl.on('line', line => lines.push(line));
  await new Promise((resolve, reject) => {
    rl.on('close', resolve);
    rl.on('error', reject);
  });
  return lines;
}

for (const side of ['main', 'renderer']) {
  test(`${side} TLS exposes independent callable APIs and never fakes encryption`, () => {
    const {tlsModule: tls} = loadModules(side);
    let called = 0;
    for (const name of ['connect', 'createServer', 'createSecureContext',
                        'checkServerIdentity', 'getCiphers']) {
      assert.equal(typeof tls[name], 'function');
      assert.throws(() => tls[name]({}, () => ++called), {code: 'ERR_NOT_SUPPORTED'});
    }
    for (const name of ['TLSSocket', 'Server', 'SecureContext']) {
      assert.equal(typeof tls[name], 'function');
      assert.throws(() => new tls[name](), {code: 'ERR_NOT_SUPPORTED'});
    }
    assert.equal(called, 0);
  });

  test(`${side} readline matches Node for UTF-8, CRLF and final partial lines`, async () => {
    const {readlineModule: readline} = loadModules(side);
    const bytes = Buffer.concat([Buffer.from('a\r\n中文😀\n\rthird\rlast'),
                                Buffer.from([0xf0, 0x9f])]);
    for (const size of [1, 2, 3, 7, 128]) {
      const chunks = [];
      for (let i = 0; i < bytes.length; i += size) chunks.push(bytes.subarray(i, i + size));
      assert.deepEqual(await readLines(readline, chunks), await readLines(nativeReadline, chunks));
    }
  });

  test(`${side} readline matches Node replacement handling for malformed UTF-8`, async () => {
    const {readlineModule: readline} = loadModules(side);
    const bytes = Buffer.from([0xef, 0xbb, 0xbf, 10, 0xc0, 0xaf, 10,
        0xe0, 0x80, 0x80, 10, 0xed, 0xa0, 0x80, 10, 0xf4, 0x90, 0x80, 0x80,
        10, 0xe2, 0x28, 0xa1, 10, 0xf0, 0x9f]);
    const chunks = Array.from(bytes, byte => Buffer.from([byte]));
    assert.deepEqual(await readLines(readline, chunks), await readLines(nativeReadline, chunks));
  });

  test(`${side} readline emits no fabricated lines for empty or terminated streams`, async () => {
    const {readlineModule: readline} = loadModules(side);
    for (const chunks of [[], [''], ['one\n'], ['one\r', '\n'], ['\n\n']]) {
      assert.deepEqual(await readLines(readline, chunks), await readLines(nativeReadline, chunks));
    }
  });

  test(`${side} readline question waits for real input and preserves subsequent line events`, () => {
    const {readlineModule: readline} = loadModules(side);
    const input = inputStream();
    const writes = [];
    const rl = readline.createInterface({input, output: {write: value => writes.push(value)}});
    const answers = [], lines = [];
    rl.on('line', line => lines.push(line));
    rl.question('name? ', value => answers.push(value));
    assert.deepEqual(answers, []);
    input.emit('data', Buffer.from('Alice\nnext\n'));
    assert.deepEqual(answers, ['Alice']);
    assert.deepEqual(lines, ['next']);
    assert.deepEqual(writes, ['name? ']);
    rl.close();
    assert.throws(() => rl.question('again?', () => {}), {code: 'ERR_USE_AFTER_CLOSE'});
  });

  test(`${side} readline pause, resume and close delegate and remove listeners exactly once`, () => {
    const {readlineModule: readline} = loadModules(side);
    const input = inputStream();
    const rl = readline.createInterface(input);
    const events = [];
    for (const name of ['pause', 'resume', 'close', 'line']) rl.on(name, () => events.push(name));
    assert.equal(rl.pause(), rl);
    rl.pause();
    assert.equal(rl.resume(), rl);
    rl.resume();
    rl.close();
    rl.close();
    input.emit('data', Buffer.from('ignored\n'));
    assert.deepEqual(events, ['pause', 'resume', 'close']);
    assert.equal(input.pauseCount, 2);
    assert.equal(input.resumeCount, 2);
    for (const name of ['data', 'end', 'error']) assert.equal(input.listenerCount(name), 0);
  });

  test(`${side} readline forwards the original input error and supports cancellation`, async () => {
    const {readlineModule: readline} = loadModules(side);
    const input = inputStream();
    const controller = new AbortController();
    const rl = readline.createInterface({input, signal: controller.signal});
    const error = new Error('read failed');
    let observed;
    rl.on('error', value => observed = value);
    input.emit('error', error);
    assert.equal(observed, error);
    controller.abort();
    assert.equal(rl.closed, true);
    assert.equal(input.listenerCount('data'), 0);
    const alreadyAborted = readline.createInterface({input, signal: controller.signal});
    let closes = 0;
    alreadyAborted.on('close', () => ++closes);
    await Promise.resolve();
    assert.equal(closes, 1);
  });

  test(`${side} readline rejects unavailable terminal editing at the operation`, () => {
    const {readlineModule: readline} = loadModules(side);
    assert.equal(typeof readline.createInterface, 'function');
    assert.throws(() => readline.createInterface({input: inputStream(), terminal: true}),
                  {code: 'ERR_NOT_SUPPORTED'});
    assert.throws(() => readline.createInterface({}), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => readline.emitKeypressEvents(inputStream()), {code: 'ERR_NOT_SUPPORTED'});
  });
}
