// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const plain = value => JSON.parse(JSON.stringify(value));

function fixture(platform = 'win32') {
  const calls = [], pending = [];
  let ready = true, answer, failure;
  const context = vm.createContext({Buffer});
  vm.runInContext(['power_save_blocker.js', 'safe_storage.js'].map(name =>
    fs.readFileSync(path.join(__dirname, 'main', name), 'utf8')).join('\n'), context);
  const callHost = request => {
    calls.push(plain(request));
    if (failure) throw failure;
    return answer;
  };
  const safe = context.createSafeStorageApi({Buffer, platform,
    isReady: () => ready, callHost,
    callHostAsync: request => {
      calls.push(plain(request));
      return new Promise((resolve, reject) => pending.push({resolve, reject}));
    },
  });
  return {safe, power: context.createPowerSaveBlockerApi({callHost}), calls, pending,
    setReady: value => { ready = value; }, setAnswer: value => { answer = value; },
    fail: value => { failure = value; }};
}

test('powerSaveBlocker uses native IDs and forwards actual lifecycle results', () => {
  const f = fixture();
  f.setAnswer(37);
  assert.equal(f.power.start('prevent-display-sleep'), 37);
  f.setAnswer(true);
  assert.equal(f.power.isStarted(37), true);
  assert.equal(f.power.stop(37), true);
  f.setAnswer(false);
  assert.equal(f.power.stop(37), false);
  assert.equal(f.power.isStarted(37), false);
  assert.deepEqual(f.calls, [{method:'start',type:'prevent-display-sleep'},
    {method:'isStarted',id:37},{method:'stop',id:37},{method:'stop',id:37},
    {method:'isStarted',id:37}]);
  const failure = Object.assign(Error('native failed'), {code:'ERR_RESOURCE_UNAVAILABLE'});
  f.fail(failure);
  assert.throws(() => f.power.start('prevent-app-suspension'), error => error === failure);
});

test('powerSaveBlocker validates arguments before acquiring native resources', () => {
  const f = fixture();
  for (const value of [undefined,null,1,'other',{},new String('prevent-display-sleep')])
    assert.throws(() => f.power.start(value), {code:'ERR_INVALID_ARG_TYPE'});
  for (const value of [undefined,null,'1',NaN,Infinity,1.5,2 ** 31]) {
    assert.throws(() => f.power.stop(value), {code:'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => f.power.isStarted(value), {code:'ERR_INVALID_ARG_TYPE'});
  }
  assert.deepEqual(f.calls, []);
});

test('safeStorage never replaces native availability with an optimistic value', () => {
  const f = fixture();
  f.setAnswer(false); assert.equal(f.safe.isEncryptionAvailable(), false);
  f.setAnswer(true); assert.equal(f.safe.isEncryptionAvailable(), true);
  assert.equal('getSelectedStorageBackend' in f.safe, false);
  f.setReady(false);
  assert.equal(f.safe.isEncryptionAvailable(), true);
  const linux = fixture('linux'); linux.setReady(false);
  assert.equal(linux.safe.isEncryptionAvailable(), false);
  assert.equal(linux.safe.getSelectedStorageBackend(), 'unknown');
  assert.throws(() => linux.safe.encryptString('secret'), {code:'ERR_NOT_READY'});
  assert.deepEqual(linux.calls, []);
  linux.setReady(true); linux.setAnswer('basic_text');
  assert.equal(linux.safe.getSelectedStorageBackend(), 'basic_text');
});

test('safeStorage preserves binary ciphertext and UTF-8 plaintext through native transport', () => {
  const f = fixture(), bytes = Buffer.from([0,255,1,3,128,0]);
  f.setAnswer(bytes.toString('base64'));
  const encrypted = f.safe.encryptString('中文\0tail');
  assert.ok(Buffer.isBuffer(encrypted)); assert.deepEqual(encrypted, bytes);
  f.setAnswer('中文\0tail');
  assert.equal(f.safe.decryptString(bytes), '中文\0tail');
  assert.deepEqual(f.calls, [{method:'encryptString',text:'中文\0tail'},
    {method:'decryptString',ciphertext:bytes.toString('base64')}]);
  const failure = Object.assign(Error('wrong key'), {code:'ERR_DECRYPTION_FAILED'});
  f.fail(failure);
  assert.throws(() => f.safe.decryptString(bytes), error => error === failure);
});

test('safeStorage validates plaintext, Buffer identity and explicit plaintext opt-in', async () => {
  const f = fixture();
  for (const value of [undefined,null,42,{},new String('text')]) {
    assert.throws(() => f.safe.encryptString(value), {code:'ERR_INVALID_ARG_TYPE'});
    await assert.rejects(f.safe.encryptStringAsync(value), {code:'ERR_INVALID_ARG_TYPE'});
  }
  for (const value of [undefined,null,'text',new Uint8Array([1]),{toString:()=>''}]) {
    assert.throws(() => f.safe.decryptString(value), {code:'ERR_INVALID_ARG_TYPE'});
    await assert.rejects(f.safe.decryptStringAsync(value), {code:'ERR_INVALID_ARG_TYPE'});
  }
  for (const value of [undefined,null,0,1,'true'])
    assert.throws(() => f.safe.setUsePlainTextEncryption(value), {code:'ERR_INVALID_ARG_TYPE'});
  assert.deepEqual(f.calls, []);
  f.setAnswer(true);
  assert.equal(f.safe.setUsePlainTextEncryption(true), undefined);
  assert.equal(f.safe.setUsePlainTextEncryption(false), undefined);
  assert.deepEqual(f.calls, [{method:'setUsePlainTextEncryption',allow:true},
    {method:'setUsePlainTextEncryption',allow:false}]);
});

test('safeStorage async methods await the native operation and retain decryption flags', async () => {
  const f = fixture(), bytes = Buffer.from([0,128,255]);
  let settled = false;
  const encryption = f.safe.encryptStringAsync('secret').then(value => {
    settled = true; return value;
  });
  await Promise.resolve(); assert.equal(settled, false);
  f.pending.shift().resolve(bytes.toString('base64'));
  assert.deepEqual(await encryption, bytes);
  const decryption = f.safe.decryptStringAsync(bytes);
  f.pending.shift().resolve({result:'secret',shouldReEncrypt:true});
  assert.deepEqual(plain(await decryption), {result:'secret',shouldReEncrypt:true});
  const available = f.safe.isAsyncEncryptionAvailable();
  f.pending.shift().resolve(false); assert.equal(await available, false);
  const cancelled = f.safe.encryptStringAsync('secret');
  const failure = Object.assign(Error('owner exited'), {code:'ERR_ABORTED'});
  f.pending.shift().reject(failure);
  await assert.rejects(cancelled, error => error === failure);
});

test('safeStorage async APIs do not contact the host before app readiness', async () => {
  for (const platform of ['win32','darwin','linux']) {
    const f = fixture(platform); f.setReady(false);
    assert.equal(await f.safe.isAsyncEncryptionAvailable(), false);
    await assert.rejects(f.safe.encryptStringAsync('secret'), {code:'ERR_NOT_READY'});
    await assert.rejects(f.safe.decryptStringAsync(Buffer.alloc(0)), {code:'ERR_NOT_READY'});
    assert.deepEqual(f.calls, []);
  }
});

test('full main bootstrap publishes real Buffer contracts and separate async host transport', async () => {
  const calls = [], ciphertext = Buffer.from([0,128,255,7]);
  const context = vm.createContext({TextEncoder,TextDecoder,URL,URLSearchParams,queueMicrotask,
    setTimeout,clearTimeout,setInterval,clearInterval,atob,btoa,performance,
    __xenonPlatform:'win32',__xenonArch:'x64',__xenonEndianness:'LE',__xenonOsRelease:'10',
    __xenonAppPath:'C:\\fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',__xenonExecPath:process.execPath,
    __xenonPid:process.pid,__xenonEnv:{},__xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
    __xenonBrowserWindowCall: (id, command, request) => {
      assert.equal(id, 0); calls.push({command,...plain(request),async:false});
      if (command === 'powerSaveBlocker') return request.method === 'start' ? 71 : true;
      assert.equal(command, 'safeStorage');
      if (request.method === 'isEncryptionAvailable') return true;
      if (request.method === 'encryptString') return ciphertext.toString('base64');
      return 'plaintext';
    },
    __xenonBrowserWindowCallAsync: async (id, command, request) => {
      assert.equal(id, 0); assert.equal(command, 'safeStorage');
      calls.push({command,...plain(request),async:true});
      if (request.method === 'encryptStringAsync') return ciphertext.toString('base64');
      if (request.method === 'decryptStringAsync') return {result:'plaintext',shouldReEncrypt:false};
      return true;
    },
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')), context);
  const {powerSaveBlocker, safeStorage} = context.__xenonElectron;
  assert.equal(powerSaveBlocker.start('prevent-display-sleep'), 71);
  assert.equal(powerSaveBlocker.stop(71), true);
  const sync = safeStorage.encryptString('plaintext');
  assert.equal(context.Buffer.isBuffer(sync), true);
  assert.deepEqual(Array.from(sync), Array.from(ciphertext));
  assert.equal(safeStorage.decryptString(sync), 'plaintext');
  context.__xenonMarkAppReady();
  assert.equal(await safeStorage.isAsyncEncryptionAvailable(), true);
  const asynchronous = await safeStorage.encryptStringAsync('plaintext');
  assert.equal(context.Buffer.isBuffer(asynchronous), true);
  assert.deepEqual(Array.from(asynchronous), Array.from(ciphertext));
  assert.deepEqual(plain(await safeStorage.decryptStringAsync(asynchronous)),
    {result:'plaintext',shouldReEncrypt:false});
  assert.deepEqual(calls.map(call => [call.method,call.async]), [
    ['start',false],['stop',false],['encryptString',false],['decryptString',false],
    ['isAsyncEncryptionAvailable',true],['encryptStringAsync',true],['decryptStringAsync',true],
  ]);
});
