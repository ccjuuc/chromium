// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const realFs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function runtime(t, kind, moduleName) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-append-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const calls = [];
  const perform = (request, bytes, asynchronous) => {
    calls.push({...request, asynchronous});
    if (moduleName === 'original-fs') assert.equal(request.noAsar, true);
    assert.ok(['write_file', 'append_file'].includes(request.operation));
    const value = bytes ?? request.data;
    const snapshot = Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    const options = {flag: request.operation === 'append_file' ? 'a' : 'w'};
    return asynchronous ? realFs.promises.writeFile(request.path, snapshot, options) :
        realFs.writeFileSync(request.path, snapshot, options);
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    __xenonPaths: {platform: process.platform, arch: process.arch, endianness: os.endianness()},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: directory, execPath: process.execPath,
        exeDir: path.dirname(process.execPath)}),
      setDispatchHandler() {},
      sendSync: (_channel, request) => perform(request, undefined, false),
      invoke: (_channel, request) => perform(request, undefined, true),
    },
    __xenonFsCall: (request, bytes) => perform(request, bytes, false),
    __xenonFsCallAsync: (request, bytes) => perform(request, bytes, true),
    __xenonPlatform: process.platform, __xenonArch: process.arch,
    __xenonEndianness: os.endianness(), __xenonOsRelease: os.release(),
    __xenonAppPath: directory, __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`)), context);
  const fs = kind === 'renderer' ? context.require(moduleName) :
      context[moduleName === 'fs' ? '__xenonFs' : '__xenonOriginalFs'];
  return {fs, context, calls, filename: path.join(directory, 'append.bin')};
}

function callbackCall(operation) {
  let returned = false;
  let count = 0;
  return new Promise((resolve, reject) => {
    assert.equal(operation(error => {
      try {
        assert.ok(returned, 'callback must be asynchronous');
        assert.equal(++count, 1);
        error ? reject(error) : resolve();
      } catch (error) { reject(error); }
    }), undefined);
    returned = true;
  });
}

for (const kind of ['main', 'renderer']) {
  for (const moduleName of ['fs', 'original-fs']) {
    const label = `${kind} ${moduleName}`;
    test(`${label}: all append forms create files and preserve content with undefined flags`, async t => {
      const {fs, filename, calls} = runtime(t, kind, moduleName);
      assert.equal(fs.appendFileSync(filename, 'first', {flag: undefined}), undefined);
      await callbackCall(callback => fs.appendFile(filename, '+callback', {flag: undefined}, callback));
      assert.equal(await fs.promises.appendFile(filename, '+promise', {flag: undefined}), undefined);
      fs.appendFileSync(filename, '+null', {flag: null, flush: null});
      assert.equal(realFs.readFileSync(filename, 'utf8'), 'first+callback+promise+null');
      assert.ok(calls.every(call => call.operation === 'append_file'));
      fs.appendFileSync(filename, 'replace', {flag: 'w'});
      assert.equal(realFs.readFileSync(filename, 'utf8'), 'replace');
      await callbackCall(callback => fs.writeFile(filename, '+append', {flag: 'a'}, callback));
      assert.equal(realFs.readFileSync(filename, 'utf8'), 'replace+append');
    });

    test(`${label}: write encodings and exact typed-array subviews match Node bytes`, async t => {
      const {fs, filename, context} = runtime(t, kind, moduleName);
      fs.appendFileSync(filename, '00ff80', 'hex');
      await fs.promises.appendFile(filename, '\u00e9', {encoding: 'latin1'});
      await callbackCall(callback => fs.appendFile(filename, '中文', 'utf16le', callback));
      const data = vm.runInContext('new DataView(new Uint8Array([9, 255, 0, 128, 9]).buffer, 1, 3)', context);
      const pending = fs.promises.appendFile(filename, data);
      new Uint8Array(data.buffer).fill(42);
      await pending;
      assert.deepEqual(realFs.readFileSync(filename), Buffer.concat([
        Buffer.from('00ff80', 'hex'), Buffer.from('\u00e9', 'latin1'),
        Buffer.from('中文', 'utf16le'), Buffer.from([255, 0, 128]),
      ]));
      fs.writeFileSync(filename, 'ff41', {encoding: 'hex'});
      assert.deepEqual(realFs.readFileSync(filename), Buffer.from([255, 65]));
    });

    test(`${label}: invalid data or unsupported flags never overwrite existing files`, async t => {
      const {fs, filename, calls} = runtime(t, kind, moduleName);
      realFs.writeFileSync(filename, 'preserve');
      assert.throws(() => fs.appendFileSync(filename, 'x', {flag: 'ax'}), {code: 'ERR_NOT_SUPPORTED'});
      await assert.rejects(fs.promises.appendFile(filename, 'x', {flag: 'ax'}), {code: 'ERR_NOT_SUPPORTED'});
      await assert.rejects(callbackCall(callback => fs.appendFile(filename, 'x', {flag: 'ax'}, callback)),
          {code: 'ERR_NOT_SUPPORTED'});
      assert.throws(() => fs.appendFileSync(filename, {}), {code: 'ERR_INVALID_ARG_TYPE'});
      assert.throws(() => fs.appendFileSync(filename, 'x', {flush: true}), {code: 'ERR_NOT_SUPPORTED'});
      assert.throws(() => fs.appendFileSync(filename, 'x', {mode: 0o600}), {code: 'ERR_NOT_SUPPORTED'});
      assert.throws(() => fs.appendFile(filename, 'x', null), {code: 'ERR_INVALID_ARG_TYPE'});
      assert.deepEqual(calls, []);
      assert.equal(realFs.readFileSync(filename, 'utf8'), 'preserve');
    });
  }
}
