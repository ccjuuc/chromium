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
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-access-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const file = path.join(directory, 'a space 中文.txt');
  realFs.writeFileSync(file, 'unchanged access fixture');
  const calls = [];
  const perform = request => {
    calls.push({...request});
    assert.equal(request.operation, 'access');
    assert.equal(request.noAsar, original ? true : undefined);
    realFs.accessSync(request.path, request.mode);
    return null;
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'access-fixture', search: ''},
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
  test(`${name}: access preserves modes, file URLs, bytes and callback timing`, async t => {
    const {fs, file, directory, context, calls, BufferType} = fixture(t, kind, original);
    assert.equal(fs.accessSync(file, fs.constants.R_OK | fs.constants.W_OK), undefined);
    const url = new context.URL(pathToFileURL(file).href);
    assert.equal(await fs.promises.access(url, fs.constants.W_OK), undefined);
    assert.equal(fs.accessSync(BufferType.from(file), null), undefined);
    let returned = false;
    const pending = new Promise((resolve, reject) => {
      assert.equal(fs.access(file, fs.constants.R_OK, (...args) => {
        try { assert.equal(returned, true); assert.deepEqual(args, [null]); resolve(); }
        catch(error) { reject(error); }
      }), undefined);
    });
    returned = true;
    await pending;
    assert.deepEqual(calls.map(call => call.mode), [6, 2, 0, 4]);
    assert.deepEqual(calls.map(call => call.path), [file, file, file, file]);
    await new Promise((resolve, reject) => fs.access(directory, error => error ? reject(error) : resolve()));
    assert.equal(calls.at(-1).mode, 0);
    assert.equal(realFs.readFileSync(file, 'utf8'), 'unchanged access fixture');
  });

  test(`${name}: invalid modes fail before submission and Promise errors reject`, async t => {
    const {fs, file, calls} = fixture(t, kind, original);
    for (const mode of ['2', false, {}, 2n, NaN, Infinity, -1, 8]) {
      let reference;
      try { realFs.accessSync(file, mode); } catch(error) { reference = error; }
      assert.ok(reference);
      const expected = {name: reference.name, code: reference.code};
      assert.throws(() => fs.accessSync(file, mode), expected);
      assert.throws(() => fs.access(file, mode, () => assert.fail('invalid mode callback')), expected);
      let promise;
      assert.doesNotThrow(() => { promise = fs.promises.access(file, mode); });
      await assert.rejects(promise, expected);
    }
    for (const bad of [null, undefined, {}, [], 17]) {
      assert.throws(() => fs.accessSync(bad), {code: 'ERR_INVALID_ARG_TYPE'});
    }
    assert.throws(() => fs.accessSync(file + '\0suffix'), {code: 'ERR_INVALID_ARG_VALUE'});
    assert.throws(() => fs.access(file, 0, null), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.deepEqual(calls, []);
    for (const mode of [undefined, null, -0.5, 1.9, 7.9]) {
      let reference;
      try { realFs.accessSync(file, mode); } catch(error) { reference = error; }
      if (reference) assert.throws(() => fs.accessSync(file, mode), {code: reference.code});
      else fs.accessSync(file, mode);
      assert.equal(calls.at(-1).mode, mode == null ? 0 : Math.trunc(mode) || 0);
    }
  });

  test(`${name}: missing and denied files preserve real OS errors`, async t => {
    const {fs, file, directory} = fixture(t, kind, original);
    const missing = path.join(directory, 'missing');
    assert.throws(() => fs.accessSync(missing, fs.constants.W_OK), {code: 'ENOENT', path: missing});
    await assert.rejects(fs.promises.access(missing), {code: 'ENOENT', path: missing});
    const received = await new Promise(resolve => fs.access(missing, (...args) => resolve(args)));
    assert.equal(received.length, 1);
    assert.equal(received[0].code, 'ENOENT');
    realFs.chmodSync(file, 0o400);
    t.after(() => { if (realFs.existsSync(file)) realFs.chmodSync(file, 0o600); });
    let denied;
    try { realFs.accessSync(file, realFs.constants.W_OK); } catch(error) { denied = error; }
    if (denied) {
      assert.throws(() => fs.accessSync(file, fs.constants.W_OK), {code: denied.code, path: file});
      await assert.rejects(fs.promises.access(file, fs.constants.W_OK), {code: denied.code, path: file});
    } else {
      // POSIX root may write mode-0400 files; preserve the actual host result.
      assert.equal(fs.accessSync(file, fs.constants.W_OK), undefined);
    }
    assert.equal(fs.accessSync(file, fs.constants.R_OK), undefined);
    realFs.chmodSync(file, 0o600);
  });
}

test('renderer virtual read-only resources deny writes and memory files retain their own access', async t => {
  const {fs, calls} = fixture(t, 'renderer', false);
  const resource = 'chrome://access-fixture/package.json';
  const memory = 'chrome://access-fixture/memory.txt';
  fs.accessSync(resource, fs.constants.R_OK);
  assert.throws(() => fs.accessSync(resource, fs.constants.W_OK), {code: 'EACCES'});
  await assert.rejects(fs.promises.access(resource, fs.constants.W_OK), {code: 'EACCES'});
  fs.writeFileSync(memory, 'value');
  assert.equal(await fs.promises.access(memory, fs.constants.R_OK | fs.constants.W_OK), undefined);
  assert.deepEqual(calls, []);
});
