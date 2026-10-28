// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const realFs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function runtime(t) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-symlink-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const calls = [];
  const record = request => {
    calls.push({...request});
    if (request.operation === 'readlink') assert.equal(request.returnBytes, true);
  };
  // Only the production native-call boundary is replaced. Every submitted
  // operation runs against the real OS filesystem, including its failures.
  const performSync = request => {
    record(request);
    switch (request.operation) {
      case 'symlink': return realFs.symlinkSync(request.target, request.path, request.type);
      case 'readlink': return Uint8Array.from(realFs.readlinkSync(request.path, {encoding: 'buffer'}));
      case 'unlink': return realFs.unlinkSync(request.path);
      default: throw new Error('Unexpected filesystem operation: ' + request.operation);
    }
  };
  const performAsync = async request => {
    record(request);
    switch (request.operation) {
      case 'symlink': return realFs.promises.symlink(request.target, request.path, request.type);
      case 'readlink': return Uint8Array.from(await realFs.promises.readlink(request.path, {encoding: 'buffer'}));
      case 'unlink': return realFs.promises.unlink(request.path);
      default: throw new Error('Unexpected filesystem operation: ' + request.operation);
    }
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonFsCall: performSync, __xenonFsCallAsync: performAsync,
    __xenonPlatform: process.platform, __xenonArch: process.arch,
    __xenonEndianness: os.endianness(), __xenonOsRelease: os.release(),
    __xenonAppPath: directory, __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')), context);
  return {directory, calls, context, fs: context.__xenonFs};
}

function callbackCall(operation) {
  let returned = false;
  let count = 0;
  return new Promise((resolve, reject) => {
    const result = operation((error, value) => {
      try {
        assert.equal(returned, true, 'callback must be asynchronous');
        assert.equal(++count, 1, 'callback must run exactly once');
        if (error) reject(error);
        else resolve(value);
      } catch (failure) {
        reject(failure);
      }
    });
    assert.equal(result, undefined);
    returned = true;
  });
}

// The native Windows implementation reports ENOSYS. These integrations cover
// the POSIX operation without requiring Windows symlink privileges in Node.
const posix = {skip: process.platform === 'win32'};

test('main: sync symlink preserves a dangling literal lock target and real EEXIST', posix, t => {
  const {directory, calls, fs} = runtime(t);
  const link = path.join(directory, 'singleton lock');
  const target = `th-hostname-${process.pid}`;
  assert.equal(fs.symlinkSync(target, link), undefined);
  assert.equal(realFs.lstatSync(link).isSymbolicLink(), true);
  assert.throws(() => realFs.statSync(link), {code: 'ENOENT'});
  assert.equal(fs.readlinkSync(link), target);
  assert.deepEqual(calls.slice(0, 2), [
    {operation: 'symlink', path: link, target},
    {operation: 'readlink', path: link, returnBytes: true},
  ]);
  assert.throws(() => fs.symlinkSync('replacement', link), {code: 'EEXIST'});
  assert.equal(realFs.readlinkSync(link), target);
  assert.equal(fs.unlinkSync(link), undefined);
  assert.throws(() => realFs.lstatSync(link), {code: 'ENOENT'});
  assert.throws(() => fs.readlinkSync(link), {code: 'ENOENT', path: link});
});

test('main: relative targets and all valid symlink types reach POSIX unchanged', posix, t => {
  const {directory, calls, fs} = runtime(t);
  const target = '../missing//parent/../literal-target';
  for (const [index, type] of [undefined, null, 'file', 'dir', 'junction'].entries()) {
    const link = path.join(directory, String(index));
    fs.symlinkSync(target, link, type);
    assert.equal(realFs.readlinkSync(link), target);
    assert.equal(calls.at(-1).target, target);
    if (type == null) assert.equal(Object.hasOwn(calls.at(-1), 'type'), false);
    else assert.equal(calls.at(-1).type, type);
    fs.unlinkSync(link);
  }
});

test('main: readlink defaults to UTF-8 and encodes native bytes like Node', posix, t => {
  const {directory, context, fs} = runtime(t);
  const link = path.join(directory, 'encoded');
  realFs.symlinkSync('主机-é😀-payload', link);
  for (const options of [undefined, null, {}, {encoding: null}, '', 'utf8', 'UTF-8',
    'hex', {encoding: 'HEX'}, 'base64', 'base64url', 'latin1', 'binary', 'ascii',
    'ucs2', 'ucs-2', 'utf16le', 'utf-16le']) {
    assert.equal(fs.readlinkSync(link, options), realFs.readlinkSync(link, options),
        JSON.stringify(options));
  }
  for (const options of ['buffer', {encoding: 'buffer'}]) {
    const result = fs.readlinkSync(link, options);
    assert.equal(context.Buffer.isBuffer(result), true);
    assert.deepEqual(Buffer.from(result), realFs.readlinkSync(link, {encoding: 'buffer'}));
  }
  const binaryLink = path.join(directory, 'binary-target');
  realFs.symlinkSync(Buffer.from([0xff, 0xc3, 0x28, 0x61]), binaryLink);
  assert.deepEqual(Buffer.from(fs.readlinkSync(binaryLink, 'buffer')),
      realFs.readlinkSync(binaryLink, 'buffer'));
  assert.equal(fs.readlinkSync(binaryLink), realFs.readlinkSync(binaryLink));
});

test('main: callback APIs perform real symlink, readlink, errors and unlink asynchronously', posix, async t => {
  const {directory, context, fs} = runtime(t);
  const link = path.join(directory, 'callback');
  const target = 'callback-host-pid';
  assert.equal(await callbackCall(callback => fs.symlink(target, link, callback)), undefined);
  assert.equal(realFs.lstatSync(link).isSymbolicLink(), true);
  assert.equal(await callbackCall(callback => fs.readlink(link, callback)), target);
  const result = await callbackCall(callback => fs.readlink(link, {encoding: 'buffer'}, callback));
  assert.equal(context.Buffer.isBuffer(result), true);
  assert.deepEqual(Buffer.from(result), Buffer.from(target));
  await assert.rejects(callbackCall(callback => fs.symlink('another', link, 'file', callback)),
      {code: 'EEXIST'});
  assert.equal(realFs.readlinkSync(link), target);
  await callbackCall(callback => fs.unlink(link, callback));
  await assert.rejects(callbackCall(callback => fs.readlink(link, callback)), {code: 'ENOENT'});
});

test('main: Promise APIs preserve real dangling symlinks and propagate filesystem failures', posix, async t => {
  const {directory, context, calls, fs} = runtime(t);
  const link = path.join(directory, 'promise');
  const target = '../not-created/promise-host-pid';
  assert.equal(await fs.promises.symlink(target, link, 'dir'), undefined);
  assert.equal(realFs.readlinkSync(link), target);
  assert.deepEqual(calls[0], {operation: 'symlink', path: link, target, type: 'dir'});
  assert.equal(await fs.promises.readlink(link), target);
  const result = await fs.promises.readlink(link, 'buffer');
  assert.equal(context.Buffer.isBuffer(result), true);
  assert.deepEqual(Buffer.from(result), Buffer.from(target));
  await assert.rejects(fs.promises.symlink('another', link), {code: 'EEXIST'});
  await fs.promises.unlink(link);
  await assert.rejects(fs.promises.readlink(link), {code: 'ENOENT'});
  await assert.rejects(fs.promises.symlink(target, path.join(directory, 'missing', 'link')),
      {code: 'ENOENT'});
  const regular = path.join(directory, 'regular');
  realFs.writeFileSync(regular, 'not a link');
  await assert.rejects(fs.promises.readlink(regular), {code: 'EINVAL'});
  assert.throws(() => fs.readlinkSync(regular), {code: 'EINVAL'});
});

test('main: Buffer and file URL link paths retain ordinary filesystem semantics', posix, t => {
  const {directory, context, fs} = runtime(t);
  const link = path.join(directory, 'buffer path');
  const bufferTarget = context.Buffer.from('buffer-host-pid');
  const bufferPath = context.Buffer.from(link);
  fs.symlinkSync(bufferTarget, bufferPath);
  assert.equal(fs.readlinkSync(bufferPath), 'buffer-host-pid');
  fs.unlinkSync(link);
  const fileURL = new context.URL(require('node:url').pathToFileURL(link).href);
  fs.symlinkSync('url-host-pid', fileURL);
  assert.equal(fs.readlinkSync(fileURL), 'url-host-pid');
});

test('main: invalid target/path/type/encoding fail before submitting native operations', async t => {
  const {directory, context, calls, fs} = runtime(t);
  const link = path.join(directory, 'never-created');
  for (const target of ['bad\0target', context.Buffer.from('bad\0target')]) {
    assert.throws(() => fs.symlinkSync(target, link), {code: 'ERR_INVALID_ARG_VALUE'});
    await assert.rejects(fs.promises.symlink(target, link), {code: 'ERR_INVALID_ARG_VALUE'});
    await assert.rejects(callbackCall(callback => fs.symlink(target, link, callback)),
        {code: 'ERR_INVALID_ARG_VALUE'});
  }
  for (const invalidPath of [link + '\0suffix', context.Buffer.from(link + '\0suffix')]) {
    assert.throws(() => fs.symlinkSync('target', invalidPath), {code: 'ERR_INVALID_ARG_VALUE'});
    assert.throws(() => fs.readlinkSync(invalidPath), {code: 'ERR_INVALID_ARG_VALUE'});
    await assert.rejects(fs.promises.readlink(invalidPath), {code: 'ERR_INVALID_ARG_VALUE'});
  }
  for (const type of ['invalid', 1, {}]) {
    assert.throws(() => fs.symlinkSync('target', link, type), {code: 'ERR_INVALID_ARG_VALUE'});
    await assert.rejects(fs.promises.symlink('target', link, type),
        {code: 'ERR_INVALID_ARG_VALUE'});
  }
  for (const options of ['invalid', 'BUFFER', {encoding: 'invalid'}, {encoding: 42},
    {encoding: 0}, {encoding: false}]) {
    assert.throws(() => fs.readlinkSync(link, options), {code: 'ERR_INVALID_ARG_VALUE'});
    await assert.rejects(fs.promises.readlink(link, options), {code: 'ERR_INVALID_ARG_VALUE'});
  }
  for (const value of [null, undefined, 42, {}]) {
    assert.throws(() => fs.symlinkSync(value, link), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => fs.readlinkSync(value), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  const nonUtf8 = context.Buffer.from([0xff]);
  assert.throws(() => fs.symlinkSync(nonUtf8, link), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => fs.readlinkSync(nonUtf8), {code: 'ERR_NOT_SUPPORTED'});
  for (const callback of [undefined, null, 42]) {
    assert.throws(() => fs.symlink('target', link, callback), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => fs.readlink(link, callback), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  assert.deepEqual(calls, []);
  assert.equal(realFs.existsSync(link), false);
});
