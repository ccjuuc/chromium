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

const fields = ['dev', 'mode', 'nlink', 'uid', 'gid', 'rdev', 'blksize', 'ino',
  'size', 'blocks', 'atimeMs', 'mtimeMs', 'ctimeMs', 'birthtimeMs'];
const typeMethods = ['isFile', 'isDirectory', 'isBlockDevice', 'isCharacterDevice',
  'isSymbolicLink', 'isFIFO', 'isSocket'];

function runtime(t, kind, suppliedPayload, platform = process.platform) {
  const directory = realFs.mkdtempSync(path.join(os.tmpdir(), 'xenon-stats-'));
  t.after(() => realFs.rmSync(directory, {recursive: true, force: true}));
  const calls = [];
  const perform = request => {
    calls.push({...request});
    assert.ok(['stat', 'lstat'].includes(request.operation));
    if (suppliedPayload) return {...suppliedPayload};
    const stat = realFs[request.operation + 'Sync'](request.path, {bigint: request.bigint});
    return Object.fromEntries([
      ...(request.bigint ? [...fields.slice(0, 10), 'atimeNs', 'mtimeNs', 'ctimeNs', 'birthtimeNs'] : fields)
        .map(field => [field, request.bigint ? String(stat[field]) : stat[field]]),
      ...typeMethods.map(method => [method, stat[method]()]),
    ]);
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    __xenonPaths: {platform, arch: process.arch, endianness: os.endianness()},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: directory, execPath: process.execPath,
        exeDir: path.dirname(process.execPath)}),
      setDispatchHandler() {},
      sendSync: (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        return perform(request);
      },
      invoke: async (channel, request) => {
        assert.equal(channel, '__xenon:fs');
        return perform(request);
      },
    },
    __xenonFsCall: perform,
    __xenonFsCallAsync: async request => perform(request),
    __xenonPlatform: platform, __xenonArch: process.arch,
    __xenonEndianness: os.endianness(), __xenonOsRelease: os.release(),
    __xenonAppPath: directory, __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: process.execPath, __xenonPid: process.pid, __xenonEnv: {},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${kind}_bootstrap.js`)), context);
  return {directory, calls, context,
    fs: kind === 'main' ? context.__xenonFs : context.require('fs')};
}

function assertStats(actual, expected, Stats) {
  assert.ok(actual instanceof Stats);
  assert.equal(actual.constructor, Stats);
  for (const field of fields) assert.equal(actual[field], expected[field], field);
  for (const field of ['atime', 'mtime', 'ctime', 'birthtime']) {
    assert.equal(actual[field].getTime(), expected[field].getTime(), field);
    assert.equal(actual[field], actual[field], 'Date access retains object identity');
  }
  for (const method of typeMethods) {
    assert.equal(actual[method](), expected[method](), method);
  }
}

function callbackResult(invoke) {
  let returned = false;
  return new Promise((resolve, reject) => {
    assert.equal(invoke((error, value) => {
      try {
        assert.equal(returned, true, 'stat callbacks must be asynchronous');
        if (error) reject(error);
        else resolve(value);
      } catch (failure) { reject(failure); }
    }), undefined);
    returned = true;
  });
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: BigInt stat/lstat preserve native integer and nanosecond payloads through every overload`, async t => {
    const {directory, fs, calls} = runtime(t, kind);
    const filename = path.join(directory, 'bigint.txt');
    realFs.writeFileSync(filename, 'precise metadata');
    const expected = realFs.statSync(filename, {bigint: true});
    function verify(actual) {
      assert.equal(actual instanceof fs.Stats, false);
      assert.equal(actual.constructor.name, expected.constructor.name);
      for (const field of [...fields, 'atimeNs', 'mtimeNs', 'ctimeNs', 'birthtimeNs'])
        assert.equal(actual[field], expected[field], field);
      for (const method of typeMethods) assert.equal(actual[method](), expected[method](), method);
      for (const field of ['atime', 'mtime', 'ctime', 'birthtime']) {
        assert.equal(Object.hasOwn(actual, field), false);
        assert.equal(actual[field].getTime(), expected[field].getTime());
        assert.equal(actual[field], actual[field]);
        assert.deepEqual(Object.getOwnPropertyDescriptor(actual, field), {
          value: actual[field], writable: true, enumerable: true, configurable: true});
      }
    }
    for (const method of ['stat', 'lstat']) {
      verify(fs[method + 'Sync'](filename, {bigint: true}));
      verify(await fs.promises[method](filename, {bigint: true}));
      verify(await callbackResult(done => fs[method](filename, {bigint: true}, done)));
      assert.equal(calls.at(-1).bigint, true);
      for (const bigint of [false, 1, 'true', null, {}])
        assert.equal(typeof fs[method + 'Sync'](filename, {bigint}).size, 'number');
    }
  });

  test(`${kind}: BigInt Stats do not round large integers or negative nanoseconds through Number`, t => {
    const payload = {mode:'33188', ino:'9007199254740993', size:'18446744073709551615',
      mtimeNs:'1794455573756347601', atimeNs:'-500001'};
    const {directory, fs} = runtime(t, kind, payload);
    const stat = fs.statSync(directory, {bigint:true});
    assert.equal(stat.ino, 9007199254740993n);
    assert.equal(stat.size, 18446744073709551615n);
    assert.equal(stat.mtimeNs, 1794455573756347601n);
    assert.equal(stat.mtimeMs, 1794455573756n);
    assert.equal(stat.atimeNs, -500001n); assert.equal(stat.atimeMs, 0n);
    assert.equal(stat.atime.getTime(), 0);
    assert.equal(stat.isFile(), true);
    assert.equal(stat.dev, undefined);
    assert.equal(stat.birthtimeNs, undefined);
    assert.ok(Number.isNaN(stat.birthtime.getTime()));
    const NodeBigIntStats = realFs.statSync(__filename, {bigint:true}).constructor;
    for (const ns of [-1n, -500001n, -1500001n, -1999999n, 1500001n]) {
      const {fs: precise} = runtime(t, kind, {mtimeNs:String(ns)});
      const actual = precise.statSync(directory, {bigint:true});
      const oracle = new NodeBigIntStats(...Array(10).fill(0n), ns, ns, ns, ns);
      assert.equal(actual.mtimeMs, oracle.mtimeMs);
      assert.equal(actual.mtimeNs, oracle.mtimeNs);
      assert.equal(actual.mtime.getTime(), oracle.mtime.getTime());
    }
  });

  test(`${kind}: BigInt Stats reject rounded legacy transport fields`, t => {
    const {directory, fs} = runtime(t, kind, {size: 9007199254740992});
    assert.throws(() => fs.statSync(directory, {bigint:true}), {code:'ERR_IPC_INVALID_RESPONSE'});
  });

  test(`${kind}: Stats constructor preserves missing metadata and positional fields`, t => {
    const {fs} = runtime(t, kind);
    assert.equal(typeof fs.Stats, 'function');
    const unknown = new fs.Stats();
    for (const field of fields) assert.equal(unknown[field], undefined, field);
    for (const method of typeMethods) assert.equal(unknown[method](), false, method);
    for (const field of ['atime', 'mtime', 'ctime', 'birthtime']) {
      assert.ok(Number.isNaN(unknown[field].getTime()), field);
    }
    const input = [1, 0o100640, 2, 3, 4, 5, 4096, 6, 19, 8, 10.5, 11.5, 12.5, 13.5];
    const actual = new fs.Stats(...input);
    fields.forEach((field, index) => assert.equal(actual[field], input[index], field));
    assert.equal(actual.isFile(), true);
    assert.equal(actual.isDirectory(), false);
    assert.equal(actual.atime.getTime(), 11);
    assert.equal(actual.mtime.getTime(), 12);
    actual.mtime = new Date(100);
    assert.equal(actual.mtime.getTime(), 100);
  });

  test(`${kind}: stat returns Stats with the real file and directory metadata`, t => {
    const {directory, fs} = runtime(t, kind);
    const filename = path.join(directory, 'payload.txt');
    realFs.writeFileSync(filename, 'actual file bytes');
    for (const entry of [filename, directory]) {
      assertStats(fs.statSync(entry), realFs.statSync(entry), fs.Stats);
    }
    assert.throws(() => fs.statSync(path.join(directory, 'missing')), {code: 'ENOENT'});
  });

  test(`${kind}: callback and Promise stat and lstat return Stats asynchronously`, async t => {
    const {directory, fs} = runtime(t, kind);
    const filename = path.join(directory, 'payload.txt');
    realFs.writeFileSync(filename, 'metadata');
    const expected = realFs.statSync(filename);
    for (const method of ['stat', 'lstat']) {
      assertStats(await callbackResult(done => fs[method](filename, done)), expected, fs.Stats);
      assertStats(await fs.promises[method](filename), expected, fs.Stats);
      await assert.rejects(fs.promises[method](path.join(directory, 'missing')), {code: 'ENOENT'});
    }
  });

  test(`${kind}: lstat keeps a real link distinct from its directory target`, async t => {
    const {directory, fs, calls} = runtime(t, kind);
    const target = path.join(directory, 'target');
    const link = path.join(directory, 'link');
    realFs.mkdirSync(target);
    realFs.symlinkSync(target, link, process.platform === 'win32' ? 'junction' : 'dir');
    const expectedLink = realFs.lstatSync(link);
    assertStats(fs.lstatSync(link), expectedLink, fs.Stats);
    assert.equal(calls.at(-1).operation, 'lstat');
    assert.equal(fs.lstatSync(link).isSymbolicLink(), true);
    assert.equal(fs.lstatSync(link).isDirectory(), false);
    assert.equal(fs.statSync(link).isDirectory(), true);
    assertStats(await fs.promises.lstat(link), expectedLink, fs.Stats);
    assertStats(await callbackResult(done => fs.lstat(link, done)), expectedLink, fs.Stats);
  });

  test(`${kind}: all stat type methods honor mode bits and ignore permission bits`, t => {
    const {fs} = runtime(t, kind);
    const types = {
      isFile: 'S_IFREG', isDirectory: 'S_IFDIR', isCharacterDevice: 'S_IFCHR',
      isBlockDevice: 'S_IFBLK', isFIFO: 'S_IFIFO', isSymbolicLink: 'S_IFLNK',
      isSocket: 'S_IFSOCK',
    };
    assert.equal(fs.constants.S_IFMT, realFs.constants.S_IFMT);
    for (const [expected, constant] of Object.entries(types)) {
      assert.equal(fs.constants[constant], realFs.constants[constant], constant);
      const type = {S_IFBLK: 0o060000, S_IFSOCK: 0o140000}[constant] || fs.constants[constant];
      const stat = new fs.Stats(undefined, type | 0o765);
      for (const method of typeMethods) {
        const unsupported = process.platform === 'win32' &&
          ['isBlockDevice', 'isFIFO', 'isSocket'].includes(method);
        assert.equal(stat[method](), !unsupported && method === expected, method);
      }
      stat.mode = fs.constants.S_IFREG | 0o400;
      assert.equal(stat.isFile(), true);
      assert.equal(stat.isDirectory(), false);
    }
  });

  test(`${kind}: POSIX Stats classify block devices, FIFOs and sockets by their actual mode`, t => {
    for (const platform of ['linux', 'darwin']) {
      const {fs} = runtime(t, kind, undefined, platform);
      for (const [method, constant, bits] of [
        ['isBlockDevice', 'S_IFBLK', 0o060000],
        ['isFIFO', 'S_IFIFO', 0o010000],
        ['isSocket', 'S_IFSOCK', 0o140000],
      ]) {
        assert.equal(fs.constants[constant], bits);
        const stat = new fs.Stats(undefined, bits | 0o600);
        for (const check of typeMethods) assert.equal(stat[check](), check === method);
      }
    }
  });

  test(`${kind}: partial bridge metadata does not invent permissions, timestamps or inode data`, t => {
    const {directory, fs} = runtime(t, kind, {isDirectory: true, size: 0, mtimeMs: 0});
    const stat = fs.statSync(directory);
    assert.ok(stat instanceof fs.Stats);
    assert.equal(stat.isDirectory(), true);
    assert.equal(stat.isFile(), false);
    assert.equal(stat.size, 0);
    assert.equal(stat.mtime.getTime(), 0);
    for (const field of fields.filter(field => !['size', 'mtimeMs'].includes(field))) {
      assert.equal(stat[field], undefined, field);
    }
    assert.ok(Number.isNaN(stat.birthtime.getTime()));
    stat.mode = fs.constants.S_IFREG | 0o600;
    assert.equal(stat.isFile(), true);
    assert.equal(stat.isDirectory(), false);
  });
}

test('renderer: virtual mount stat and lstat also use the public Stats class', async t => {
  const {fs, calls} = runtime(t, 'renderer');
  const filename = 'chrome://fixture/package.json';
  const directory = 'chrome://fixture';
  assert.ok(fs.statSync(directory) instanceof fs.Stats);
  assert.equal(fs.statSync(directory).isDirectory(), true);
  assert.ok(fs.lstatSync(filename) instanceof fs.Stats);
  assert.equal(fs.lstatSync(filename).isFile(), true);
  assert.equal(fs.statSync(filename).mode, undefined);
  assert.ok((await fs.promises.stat(filename)) instanceof fs.Stats);
  assert.ok((await fs.promises.lstat(filename)) instanceof fs.Stats);
  assert.ok((await callbackResult(done => fs.stat(filename, done))) instanceof fs.Stats);
  assert.equal(calls.length, 0, 'virtual mounts do not call the native filesystem');
  const small = fs.statSync(filename);
  for (const stat of [fs.statSync(filename, {bigint:true}),
    await fs.promises.lstat(filename, {bigint:true}),
    await callbackResult(done => fs.stat(filename, {bigint:true}, done))]) {
    assert.equal(stat.constructor.name, 'BigIntStats');
    assert.equal(stat instanceof fs.Stats, false);
    assert.equal(stat.size, BigInt(small.size));
    assert.equal(stat.mtimeNs, 0n); assert.equal(stat.mtimeMs, 0n);
    assert.equal(stat.isFile(), true); assert.equal(stat.mode, undefined);
    assert.equal(stat.birthtimeNs, undefined);
  }
});
