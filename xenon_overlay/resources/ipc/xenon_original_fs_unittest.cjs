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

function runtime(t, kind) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-original-fs-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const archive = path.join(directory, 'app.asar');
  const physical = Buffer.from([0, 255, 1, 128, 12, 99, 31, 44, 10, 0]);
  realFs.writeFileSync(archive, physical);
  const calls = [];
  let releaseReads;
  const gate = new Promise(resolve => { releaseReads = resolve; });
  const perform = (request, data) => {
    calls.push({...request});
    const file = request.path;
    const virtual = request.noAsar !== true && file === archive;
    switch (request.operation) {
      case 'read_file': {
        // This fixture models the bridge routing; real ASAR parsing and raw
        // byte identity are exercised by the native integration suite.
        if (virtual) throw new Error('EISDIR: illegal operation on a directory');
        const bytes = realFs.readFileSync(file);
        return Uint8Array.from(bytes.subarray(request.start || 0,
            request.end === undefined ? bytes.length : request.end + 1));
      }
      case 'stat': case 'lstat': {
        const stat = realFs.statSync(file);
        return {isFile: !virtual && stat.isFile(),
          isDirectory: virtual || stat.isDirectory(), size: virtual ? 0 : stat.size};
      }
      case 'exists': return realFs.existsSync(file);
      case 'write_file': realFs.writeFileSync(file, Buffer.from(data ?? request.data)); return null;
      case 'copy_file': realFs.copyFileSync(file, request.destination); return null;
      default: throw new Error('Unexpected operation: ' + request.operation);
    }
  };
  const performAsync = async (request, data) => {
    await gate;
    return perform(request, data);
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
      sendSync(channel, request) {
        assert.equal(channel, '__xenon:fs');
        return perform(request);
      },
      invoke(channel, request) {
        assert.equal(channel, '__xenon:fs');
        return performAsync(request);
      },
    },
    __xenonFsCall: perform, __xenonFsCallAsync: performAsync,
    __xenonPlatform: process.platform, __xenonArch: process.arch,
    __xenonEndianness: os.endianness(), __xenonOsRelease: os.release(),
    __xenonAppPath: directory, __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${kind}_bootstrap.js`)), context);
  const load = name => kind === 'main' ?
    context[name === 'fs' ? '__xenonFs' : '__xenonOriginalFs'] : context.require(name);
  return {directory, archive, physical, calls, context, load, releaseReads,
    Buffer: kind === 'main' ? context.Buffer : context.require('buffer').Buffer,
    fs: load('fs'), original: load('original-fs')};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: original-fs has stable identity and a separate physical archive view`, t => {
    const {original, fs, load, archive, physical, calls, context} = runtime(t, kind);
    assert.equal(original, load('original-fs'));
    if (kind === 'renderer') {
      assert.equal(original, context.require('node:original-fs'));
      assert.equal(original.promises, context.require('original-fs/promises'));
      assert.equal(original.promises, context.require('node:original-fs/promises'));
    } else {
      assert.equal(original.promises, context.__xenonOriginalFsPromises);
    }
    assert.notEqual(original, fs);
    assert.notEqual(original.promises, fs.promises);
    assert.notEqual(original.Stats, fs.Stats);
    fs.Stats.prototype.fromNormalFs = true;
    assert.equal(original.statSync(archive).fromNormalFs, undefined);
    calls.length = 0;
    assert.equal(original.statSync(archive).isFile(), true);
    assert.equal(fs.statSync(archive).isDirectory(), true);
    assert.deepEqual(Buffer.from(original.readFileSync(archive)), physical);
    assert.throws(() => fs.readFileSync(archive), {code: 'EISDIR'});
    assert.deepEqual(calls.map(call => call.noAsar), [true, undefined, true, undefined]);
  });

  test(`${kind}: pending raw reads never change normal fs or another request`, async t => {
    const {original, fs, archive, physical, releaseReads, calls} = runtime(t, kind);
    const pending = original.promises.readFile(archive);
    const callback = new Promise((resolve, reject) => original.readFile(archive,
        (error, bytes) => error ? reject(error) : resolve(bytes)));
    assert.equal(fs.statSync(archive).isDirectory(), true);
    const virtual = fs.promises.stat(archive);
    releaseReads();
    const [bytes, callbackBytes, stat] = await Promise.all([pending, callback, virtual]);
    assert.deepEqual(Buffer.from(bytes), physical);
    assert.deepEqual(Buffer.from(callbackBytes), physical);
    assert.equal(stat.isDirectory(), true);
    assert.equal(calls.filter(call => call.operation === 'read_file')
        .every(call => call.noAsar === true), true);
    assert.equal(calls.filter(call => call.operation === 'stat')
        .every(call => call.noAsar === undefined), true);
  });

  test(`${kind}: original-fs validates paths and decodes file URLs before transport`, async t => {
    const fixture = runtime(t, kind);
    const {original, archive, physical, directory, releaseReads, calls} = fixture;
    const fileUrl = filename => new fixture.context.URL(pathToFileURL(filename).href);
    const url = fileUrl(archive);
    assert.deepEqual(Buffer.from(original.readFileSync(url)), physical);
    assert.deepEqual(Buffer.from(original.readFileSync(fixture.Buffer.from(archive))), physical);
    const target = path.join(directory, 'a space # and 中文.asar');
    original.copyFileSync(url, fileUrl(target));
    assert.deepEqual(realFs.readFileSync(target), physical);
    const asyncTarget = path.join(directory, 'async.asar');
    releaseReads();
    await original.promises.copyFile(fileUrl(target), fileUrl(asyncTarget));
    assert.deepEqual(realFs.readFileSync(asyncTarget), physical);
    const before = calls.length;
    for (const bad of [null, undefined, {}, [], ...(kind === 'main' ? [] : [17])]) {
      assert.throws(() => original.readFileSync(bad), {code: 'ERR_INVALID_ARG_TYPE'});
    }
    assert.throws(() => original.readFileSync('bad\0path'), {code: 'ERR_INVALID_ARG_VALUE'});
    assert.equal(await new Promise(resolve => original.exists(null, resolve)), false);
    assert.throws(() => original.exists(null, null), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.equal(calls.length, before, 'invalid paths must never reach the native bridge');
    assert.equal(calls[0].path, archive);
    assert.equal(calls[2].destination, target);
  });

  test(`${kind}: original-fs streams preserve raw ranges and writes transport their bytes`, async t => {
    const {original, archive, physical, directory, calls, releaseReads} = runtime(t, kind);
    const chunks = [];
    const stream = original.createReadStream(archive, {start: 2, end: 6, highWaterMark: 2});
    const done = new Promise((resolve, reject) => {
      stream.on('data', chunk => chunks.push(Buffer.from(chunk)));
      stream.on('error', reject); stream.on('close', resolve);
    });
    releaseReads();
    await done;
    assert.deepEqual(Buffer.concat(chunks), physical.subarray(2, 7));
    const target = path.join(directory, 'copy.asar');
    await original.promises.writeFile(target, physical);
    assert.deepEqual(realFs.readFileSync(target), physical);
    const final = path.join(directory, 'final.asar');
    original.copyFileSync(target, final);
    assert.deepEqual(realFs.readFileSync(final), physical);
    assert.equal(calls.every(call => call.noAsar === true), true);
    const read = calls.find(call => call.operation === 'read_file');
    assert.equal(read.start, 2);
    assert.equal(read.end, 6);
  });
}
