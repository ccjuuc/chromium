// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const realFs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {pathToFileURL} = require('node:url');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function fixture(t, kind, original) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-chmod-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const file = path.join(directory, 'a space 中文.txt');
  realFs.writeFileSync(file, 'unchanged chmod fixture');
  const calls = [];
  const perform = request => {
    calls.push({...request});
    assert.equal(request.operation, 'chmod');
    assert.equal(request.noAsar, original ? true : undefined);
    realFs.chmodSync(request.path, request.mode & 0o7777);
    return null;
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'chmod-fixture', search: ''},
    __xenonPaths: {platform: process.platform, arch: process.arch, endianness: os.endianness()},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: directory, execPath: process.execPath,
        exeDir: path.dirname(process.execPath)}),
      setDispatchHandler() {},
      sendSync(channel, request) { assert.equal(channel, '__xenon:fs'); return perform(request); },
      invoke(channel, request) {
        assert.equal(channel, '__xenon:fs');
        return Promise.resolve().then(() => perform(request));
      },
    },
    __xenonFsCall: perform, __xenonFsCallAsync: request => Promise.resolve().then(() => perform(request)),
    __xenonPlatform: process.platform, __xenonArch: process.arch,
    __xenonEndianness: os.endianness(), __xenonOsRelease: os.release(),
    __xenonAppPath: directory, __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`)), context);
  const fs = kind === 'main' ? context[original ? '__xenonOriginalFs' : '__xenonFs'] :
      context.require(original ? 'original-fs' : 'fs');
  const BufferType = kind === 'main' ? context.Buffer : context.require('buffer').Buffer;
  return {fs, file, directory, context, calls, BufferType};
}

for (const kind of ['main', 'renderer']) for (const original of [false, true]) {
  const name = `${kind} ${original ? 'original-fs' : 'fs'}`;
  test(`${name}: chmod changes real permissions and preserves callback shape`, async t => {
    const {fs, file, context, calls, BufferType} = fixture(t, kind, original);
    t.after(() => { if (realFs.existsSync(file)) realFs.chmodSync(file, 0o666); });
    assert.equal(fs.chmodSync(file, '400'), undefined);
    assert.equal(realFs.statSync(file).mode & 0o200, 0);
    const url = new context.URL(pathToFileURL(file).href);
    assert.equal(await fs.promises.chmod(url, 0o600), undefined);
    assert.equal(realFs.statSync(file).mode & 0o200, 0o200);
    let returned = false;
    const pending = new Promise((resolve, reject) => {
      assert.equal(fs.chmod(BufferType.from(file), '644', (...args) => {
        try { assert.equal(returned, true); assert.deepEqual(args, [null]); resolve(); }
        catch (error) { reject(error); }
      }), undefined);
    });
    returned = true;
    await pending;
    assert.deepEqual(calls.map(call => call.mode), [0o400, 0o600, 0o644]);
    assert.deepEqual(calls.map(call => call.path), [file, file, file]);
    assert.equal(realFs.readFileSync(file, 'utf8'), 'unchanged chmod fixture');
    // Public Node parsing accepts uint32; native libuv only consumes mode bits.
    // Avoid the reference Node build's IsInt32 assertion for this valid input.
    fs.chmodSync(file, 0xffffffff);
    assert.equal(calls.at(-1).mode, 0xffffffff);
    fs.chmodSync(file, 0o600);
  });

  test(`${name}: chmod rejects invalid arguments before submitting work`, async t => {
    const {fs, file, calls} = fixture(t, kind, original);
    for (const mode of [undefined, null, false, {}, 2n, '', '0o644', '888',
        '644junk', NaN, Infinity, -1, 0.5, 0x100000000]) {
      let reference;
      try { realFs.chmodSync(file, mode); } catch (error) { reference = error; }
      assert.ok(reference);
      const expected = {name: reference.name, code: reference.code};
      assert.throws(() => fs.chmodSync(file, mode), expected);
      assert.throws(() => fs.chmod(file, mode, () => assert.fail('invalid mode callback')), expected);
      let pending;
      assert.doesNotThrow(() => { pending = fs.promises.chmod(file, mode); });
      await assert.rejects(pending, expected);
    }
    for (const bad of [undefined, null, {}, 7]) {
      assert.throws(() => fs.chmodSync(bad, 0o600), {code: 'ERR_INVALID_ARG_TYPE'});
    }
    assert.throws(() => fs.chmodSync(file + '\0suffix', 0o400), {code: 'ERR_INVALID_ARG_VALUE'});
    assert.throws(() => fs.chmod(file, 0o400, null), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.deepEqual(calls, []);
    assert.notEqual(realFs.statSync(file).mode & 0o200, 0);
  });

  test(`${name}: chmod missing-file errors retain actual code, syscall and path`, async t => {
    const {fs, directory} = fixture(t, kind, original);
    const missing = path.join(directory, 'missing');
    const expected = {code: 'ENOENT', path: missing, syscall: 'chmod'};
    assert.throws(() => fs.chmodSync(missing, 0o600), expected);
    await assert.rejects(fs.promises.chmod(missing, 0o600), expected);
    let returned = false;
    const pending = new Promise(resolve => fs.chmod(missing, 0o600, (...args) => {
      assert.equal(returned, true);
      resolve(args);
    }));
    returned = true;
    const received = await pending;
    assert.equal(received.length, 1);
    for (const [key, value] of Object.entries(expected)) assert.equal(received[0][key], value);
    assert.equal(realFs.existsSync(missing), false);
  });
}

test('renderer synthetic mounts reject chmod without pretending permission changes', async t => {
  const {fs, calls} = fixture(t, 'renderer', false);
  const resource = 'chrome://chmod-fixture/package.json';
  const memory = 'chrome://chmod-fixture/memory.txt';
  fs.writeFileSync(memory, 'unchanged');
  for (const file of [resource, memory]) {
    assert.throws(() => fs.chmodSync(file, 0o400), {code: 'ERR_NOT_SUPPORTED'});
    await assert.rejects(fs.promises.chmod(file, 0o400), {code: 'ERR_NOT_SUPPORTED'});
    await new Promise((resolve, reject) => fs.chmod(file, 0o400, error => {
      try { assert.equal(error.code, 'ERR_NOT_SUPPORTED'); resolve(); }
      catch (failure) { reject(failure); }
    }));
  }
  assert.equal(fs.readFileSync(memory, 'utf8'), 'unchanged');
  assert.deepEqual(calls, []);
});
