// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

// Deliberately synthetic values: tests must neither inspect a real account nor
// change the host's process priorities. Only the native boundary is replaced.
const user = {username: 'fixture-用户', uid: 123, gid: 456,
  homedir: '/native/fixture-home', shell: '/bin/fixture-shell'};
const nativeValues = {
  hostname: 'fixture-host', release: '6.12.fixture', version: 'Fixture OS build',
  type: 'Fixture_OS', machine: 'fixture-machine', homedir: user.homedir,
  tmpdir: '/native/fixture-temp', userInfo: user,
  cpus: [{model: 'Fixture CPU', speed: 1234,
    times: {user: 101, nice: 2, sys: 33, idle: 4004, irq: 5}}],
  totalmem: 33 * 1024 ** 3, freemem: 7 * 1024 ** 3, uptime: 9876.25,
  loadavg: [0.25, 0.5, 0.75],
  networkInterfaces: {fixture: [{address: '192.0.2.1', netmask: '255.255.255.0',
    family: 'IPv4', mac: '00:00:00:00:00:00', internal: false,
    cidr: '192.0.2.1/24'}]},
};

function createRuntime(kind, options = {}) {
  const platform = options.platform ?? 'win32';
  const arch = options.arch ?? 'x64';
  const calls = [];
  const perform = request => {
    calls.push(JSON.parse(JSON.stringify(request)));
    if (options.backend) return options.backend(request);
    if (Object.hasOwn(nativeValues, request.method)) {
      return structuredClone(nativeValues[request.method]);
    }
    throw new Error('ERR_NOT_SUPPORTED: os.' + request.method);
  };
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    __xenonPaths: {},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: '/fixture', exeDir: '/fixture',
        execPath: '/fixture/host', platform, arch, endianness: 'LE',
        ...options.runtimeConfig}),
      setDispatchHandler() {},
      sendSync(channel, request) {
        assert.equal(channel, '__xenon:os');
        return perform(request);
      },
    },
    __xenonPlatform: platform, __xenonArch: arch, __xenonEndianness: 'LE',
    __xenonOsRelease: nativeValues.release, __xenonOsCall: perform,
    __xenonAppPath: '/fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture',
    __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: '/fixture/host', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
    ...options.globals,
  });
  const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
  vm.runInContext(readFileSync(filename, 'utf8'), context, {filename});
  const os = kind === 'main' ? context.__xenonOs : context.require('os');
  const pathModule = kind === 'main' ? context.__xenonPath : context.require('path');
  return {context, os, pathModule, calls};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind} os queries forward native results instead of fabricated system data`, () => {
    const {os, calls} = createRuntime(kind);
    assert.equal(calls.length, 0, 'bootstrap does not eagerly query the system');
    for (const method of Object.keys(nativeValues)) {
      assert.deepEqual(JSON.parse(JSON.stringify(os[method]())), nativeValues[method], method);
    }
    assert.deepEqual(calls, Object.keys(nativeValues).map(method => ({method})));
  });

  test(`${kind} os memory and CPU snapshots are queried on every call`, () => {
    const revisions = {freemem: 0, cpus: 0};
    const {os} = createRuntime(kind, {backend({method}) {
      const revision = ++revisions[method];
      if (method === 'freemem') return 4096 - revision;
      if (method === 'cpus') return [{model: 'Fixture', speed: revision,
        times: {user: revision, nice: 0, sys: 0, idle: revision * 10, irq: 0}}];
      throw new Error('Unexpected method');
    }});
    assert.equal(os.freemem(), 4095);
    assert.equal(os.freemem(), 4094);
    const first = os.cpus();
    const second = os.cpus();
    assert.equal(first[0].times.user, 1);
    assert.equal(second[0].times.user, 2);
    assert.notEqual(first, second);
  });

  test(`${kind} os platform and architecture match process and select native path conventions`, () => {
    for (const platform of ['win32', 'linux', 'darwin']) {
      const {context, os, pathModule, calls} = createRuntime(kind,
          {platform, arch: 'arm64'});
      assert.equal(os.platform(), platform);
      assert.equal(os.platform(), context.process.platform);
      assert.equal(os.arch(), 'arm64');
      assert.equal(os.arch(), context.process.arch);
      assert.equal(os.endianness(), 'LE');
      assert.equal(os.EOL, platform === 'win32' ? '\r\n' : '\n');
      assert.equal(pathModule.sep, platform === 'win32' ? '\\' : '/');
      assert.equal(pathModule.join('fixture', 'child'),
          platform === 'win32' ? 'fixture\\child' : 'fixture/child');
      assert.equal(calls.length, 0, 'immutable metadata needs no synchronous IPC');
    }
  });

  test(`${kind} os byte order uses injected metadata and fails explicitly when absent`, () => {
    const be = createRuntime(kind, {runtimeConfig: {endianness: 'BE'},
      globals: {__xenonEndianness: 'BE'}});
    assert.equal(be.os.endianness(), 'BE');
    const absent = createRuntime(kind, {runtimeConfig: {endianness: undefined},
      globals: {__xenonEndianness: undefined}});
    assert.throws(() => absent.os.endianness(), {code: 'ERR_NOT_SUPPORTED'});
  });

  test(`${kind} os homedir honors the platform environment while userInfo stays native`, () => {
    for (const platform of ['win32', 'linux']) {
      const {context, os} = createRuntime(kind, {platform});
      const env = context.process.env;
      env.USERPROFILE = 'C:\\override-home';
      env.HOME = '/override-home';
      assert.equal(os.homedir(), platform === 'win32' ? env.USERPROFILE : env.HOME);
      assert.equal(os.userInfo().homedir, user.homedir);
      delete env.USERPROFILE;
      delete env.HOME;
      assert.equal(os.homedir(), user.homedir);
    }
  });

  test(`${kind} os tmpdir follows platform environment precedence and preserves roots`, () => {
    for (const platform of ['win32', 'linux']) {
      const {context, os} = createRuntime(kind, {platform});
      const env = context.process.env;
      const precedence = platform === 'win32' ? ['TEMP', 'TMP'] : ['TMPDIR', 'TMP', 'TEMP'];
      env.TMPDIR = platform === 'win32' ? 'C:\\ignored-tmpdir' : '/tmpdir/';
      env.TMP = platform === 'win32' ? 'C:\\tmp\\' : '/tmp/';
      env.TEMP = platform === 'win32' ? 'C:\\temp\\' : '/temp/';
      for (const key of precedence) {
        assert.equal(os.tmpdir(), env[key].replace(/[\\/]$/, ''));
        delete env[key];
      }
      assert.equal(os.tmpdir(), nativeValues.tmpdir);
      env[precedence[0]] = platform === 'win32' ? 'C:\\' : '/';
      assert.equal(os.tmpdir(), env[precedence[0]]);
    }
  });

  test(`${kind} os userInfo encodes native string fields and keeps uid and gid numeric`, () => {
    const {context, os} = createRuntime(kind);
    for (const encoding of ['buffer', 'BUFFER']) {
      const value = os.userInfo({encoding});
      for (const key of ['username', 'homedir', 'shell']) {
        assert.equal(context.Buffer.isBuffer(value[key]), true);
        assert.equal(value[key].toString(), user[key]);
      }
      assert.equal(value.uid, user.uid);
      assert.equal(value.gid, user.gid);
    }
    for (const encoding of ['utf8', 'utf-8', 'hex', 'base64', 'base64url',
      'ascii', 'latin1', 'binary', 'utf16le', 'ucs2']) {
      const value = os.userInfo({encoding});
      for (const key of ['username', 'homedir', 'shell']) {
        assert.equal(value[key], Buffer.from(user[key]).toString(encoding));
      }
    }
  });

  test(`${kind} os userInfo keeps Node option defaults and preserves option getter failures`, () => {
    const {os} = createRuntime(kind);
    for (const options of [undefined, null, 1, 'x', [], {},
      {encoding: null}, {encoding: 3}, {encoding: 'unknown-fixture-encoding'}]) {
      assert.deepEqual(JSON.parse(JSON.stringify(os.userInfo(options))), user);
    }
    const failure = new Error('Fixture encoding getter failed');
    assert.throws(() => os.userInfo({get encoding() { throw failure; }}),
        error => error === failure);
  });

  test(`${kind} os userInfo preserves a native null shell when returning buffers`, () => {
    const {os} = createRuntime(kind, {backend({method}) {
      assert.equal(method, 'userInfo');
      return {...user, shell: null};
    }});
    assert.equal(os.userInfo({encoding: 'buffer'}).shell, null);
    assert.equal(os.userInfo({encoding: 'hex'}).shell, null);
  });

  test(`${kind} os backend error codes survive the native boundary`, () => {
    const {os} = createRuntime(kind, {backend() {
      throw new Error('EACCES: fixture native query denied');
    }});
    assert.throws(() => os.hostname(), {code: 'EACCES'});
    assert.throws(() => os.cpus(), {code: 'EACCES'});
  });

  test(`${kind} os unsupported native capabilities fail instead of inventing success`, () => {
    const {os} = createRuntime(kind);
    for (const method of ['availableParallelism', 'getPriority', 'setPriority']) {
      assert.equal(typeof os[method], 'function');
      assert.throws(() => os[method](), {code: 'ERR_NOT_SUPPORTED'});
    }
  });
}

test('main and renderer os expose the same supported surface', () => {
  const main = createRuntime('main').os;
  const renderer = createRuntime('renderer').os;
  assert.deepEqual(Object.keys(main).sort(), Object.keys(renderer).sort());
});

test('renderer os and node:os resolve to one module instance', () => {
  const {context, os} = createRuntime('renderer');
  assert.equal(context.require('os'), os);
  assert.equal(context.require('node:os'), os);
});

test('renderer rejects missing platform and architecture instead of assuming Windows x64', () => {
  for (const runtimeConfig of [{platform: undefined}, {platform: ''},
    {arch: undefined}, {arch: ''}]) {
    assert.throws(() => createRuntime('renderer', {runtimeConfig}),
        {code: 'ERR_NOT_SUPPORTED'});
  }
});
