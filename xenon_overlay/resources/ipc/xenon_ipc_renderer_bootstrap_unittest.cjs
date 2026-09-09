// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run with: node --test xenon_overlay/resources/ipc/xenon_ipc_renderer_bootstrap_unittest.cjs
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const bootstrapPath = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const bootstrapSource = readFileSync(bootstrapPath, 'utf8');
const addonPath = 'C:\\test-app\\fixture.node';

// Exercise the complete production bootstrap through require(), replacing
// only its native transport. No application modules or network are involved.
function createRenderer(exportsList = [], overrides = {}, withWindow = false) {
  const calls = [];
  const transport = {
    getRuntimeConfig: () => ({
      appPath: 'C:\\test-app',
      exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe',
    }),
    setDispatchHandler() {},
    sendSync(channel, options) {
      assert.equal(channel, '__xenon:fs');
      assert.equal(options.operation, 'exists');
      return options.path === addonPath;
    },
    requireNodeModuleSync(modulePath) {
      calls.push(['require', modulePath]);
      return exportsList;
    },
    invokeNodeExportSync(modulePath, exportPath, ...args) {
      calls.push(['invoke', modulePath, exportPath, ...args]);
      return 42;
    },
    constructNodeExportSync(modulePath, exportPath, ...args) {
      calls.push(['construct', modulePath, exportPath, ...args]);
      return 7;
    },
    ...overrides,
  };
  const context = vm.createContext({
    xenonIpcRenderer: transport,
    TextEncoder,
    TextDecoder,
    URL,
    URLSearchParams,
    queueMicrotask,
    atob,
    btoa,
    location: {
      protocol: 'chrome:',
      hostname: 'xenon-player-electron',
      search: '',
    },
    console: {log() {}, warn() {}, error() {}},
  });
  if (withWindow) {
    context.window = context;
    context.document = {readyState: 'loading', addEventListener() {}};
  }
  vm.runInContext(bootstrapSource, context, {filename: bootstrapPath});
  return {
    calls,
    context,
    load: () => context.require(addonPath),
  };
}

function createPreloadRenderer(preload, files, extraPreferences = {}) {
  const sent = [];
  const renderer = createRenderer([], {
    sendSync(channel, request) {
      if (channel === '__xenon:renderer-web-preferences') {
        return {preload, contextIsolation: false, ...extraPreferences};
      }
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'exists') return files.has(request.path);
      assert.equal(request.operation, 'read_file');
      assert.ok(files.has(request.path));
      return Buffer.from(files.get(request.path)).toString('base64');
    },
    send(...args) { sent.push(args); },
  }, true);
  return {...renderer, sent};
}

test('document preload runs before application code and once per document', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  const files = new Map([[preload, `
    globalThis.preloadRuns = (globalThis.preloadRuns || 0) + 1;
    globalThis.fixtureAPI = {send: () => require('electron').ipcRenderer.send('fixture:click')};
    globalThis.preloadLocation = [__filename, __dirname, process.contextIsolated];
  `]]);
  for (let document = 0; document < 2; ++document) {
    const {context, sent} = createPreloadRenderer(preload, files);
    assert.equal(context.preloadRuns, 1);
    assert.deepEqual(Array.from(context.preloadLocation),
        [preload, 'C:\\test-app\\preload', false]);
    context.fixtureAPI.send();
    assert.deepEqual(sent, [['fixture:click']]);
    vm.runInContext(bootstrapSource, context);
    assert.equal(context.preloadRuns, 1);
  }
});

test('preload CommonJS dependencies remain relative after evaluation and in ASAR paths', () => {
  for (const root of ['C:\\test-app', 'C:\\test-app\\resources.asar']) {
    const preload = root + '\\preload\\entry.cjs';
    const files = new Map([
      [preload, `globalThis.readFixture = require('./nested/module.js');`],
      [root + '\\preload\\nested\\module.js', `module.exports = () => require('../data.json').value;`],
      [root + '\\preload\\data.json', '{"value":42}'],
    ]);
    const {context, sent} = createPreloadRenderer(preload, files);
    assert.deepEqual(sent, []);
    assert.equal(context.readFixture(), 42);
  }
});

test('missing and throwing preloads report a real preload-error without aborting bootstrap', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  for (const files of [new Map(), new Map([[preload, `throw new Error('fixture failure');`]])]) {
    const {context, sent} = createPreloadRenderer(preload, files);
    assert.equal(typeof context.require('electron').ipcRenderer.send, 'function');
    assert.equal(sent.length, 1);
    assert.equal(sent[0][0], '__xenon:preload-error');
    assert.equal(sent[0][1], preload);
    assert.match(sent[0][2], /Cannot find module|fixture failure/);
  }
});

test('isolated preload is never silently executed in the main world', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  const {context, sent} = createPreloadRenderer(
      preload, new Map([[preload, 'globalThis.leaked = true;']]),
      {contextIsolation: true});
  assert.equal(context.leaked, undefined);
  assert.match(sent[0][2], /Isolated-world preloads/);
});

test('a preload exporting undefined still evaluates successfully and is cached', () => {
  const preload = 'C:\\test-app\\preload\\entry.js';
  const {context, sent} = createPreloadRenderer(preload, new Map([[preload,
    'globalThis.preloadRuns = (globalThis.preloadRuns || 0) + 1; module.exports = undefined;',
  ]]));
  assert.deepEqual(sent, []);
  assert.equal(context.require(preload), undefined);
  assert.equal(context.preloadRuns, 1);
});

test('renderer filename follows the HTML document, including ASAR directories', () => {
  const documentPath = 'C:\\app\\renderer.asar\\modal-renderer\\index.html';
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\app', documentPath}),
  });
  assert.equal(context.__filename, documentPath);
  assert.equal(context.__dirname, 'C:\\app\\renderer.asar\\modal-renderer');
  assert.equal(context.require('path').join(context.__dirname, '../preload/entry.js'),
      'C:\\app\\renderer.asar\\preload\\entry.js');
});

test('guest preload retains Node closures without exposing transport to remote page', () => {
  const preload = 'C:\\test-app\\preload.js';
  const source = `const ipc = require('electron').ipcRenderer;
    globalThis.fixtureNative = () => {
      ipc.sendToHost('fixture', require('./value.json').value);
      return process.type;
    };`;
  const sent = [];
  const files = new Map([[preload, source], ['C:\\test-app\\value.json', '{"value":42}']]);
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', isGuest: true, isMainFrame: true}),
    send(...args) { sent.push(args); },
    sendSync(channel, request) {
      if (channel === '__xenon:renderer-web-preferences')
        return {preload, contextIsolation: false, nodeIntegration: false};
      if (request.operation === 'exists') return files.has(request.path);
      return Buffer.from(files.get(request.path)).toString('base64');
    },
  }, true);
  for (const key of ['require', 'process', 'xenonIpcRenderer', '__xenonElectronIpc'])
    assert.equal(context[key], undefined);
  assert.equal(context.fixtureNative(), 'renderer');
  assert.equal(sent.length, 1);
  assert.deepEqual(sent[0].slice(0, 2), ['__xenon:send-to-host', 'fixture']);
  assert.deepEqual(Array.from(sent[0][2]), [42]);
});

test('application and ASAR paths use native fs while chrome resources stay virtual', () => {
  const files = new Map([
    ['C:\\test-app\\plugins\\config.json', '{"plugin":"cloud"}'],
    ['C:\\test-app\\renderer.asar\\assets\\config.json', '{"archive":true}'],
  ]);
  const nativeCalls = [];
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({
      appName: 'sample-app',
      appVersion: '2.3.4',
      appPath: 'C:\\test-app',
      exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe',
    }),
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      nativeCalls.push([request.operation, request.path]);
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'read_file') {
        if (!files.has(request.path)) throw new Error('ENOENT: missing fixture');
        return Buffer.from(files.get(request.path)).toString('base64');
      }
      throw new Error(`unexpected fs operation: ${request.operation}`);
    },
  });
  const fs = context.require('fs');
  for (const [filename, contents] of files) {
    assert.equal(fs.existsSync(filename), true);
    assert.equal(fs.readFileSync(filename, 'utf8'), contents);
  }
  // Synthetic package descriptor for virtual chrome mounts stays in memory
  // without calling the broker and reflects container identity.
  const virtualRoot = 'chrome:\\xenon-player-electron';
  const virtualPackage = virtualRoot + '\\package.json';
  assert.equal(fs.existsSync(virtualRoot), true);
  assert.equal(fs.statSync(virtualRoot).isDirectory(), true);
  assert.deepEqual(Array.from(fs.readdirSync(virtualRoot)), ['package.json']);
  assert.equal(
      fs.readFileSync(virtualPackage, 'utf8'),
      '{"name":"sample-app","version":"2.3.4"}');
  assert.throws(
      () => fs.readFileSync(virtualRoot + '\\static\\js\\package.json'),
      error => error.code === 'ENOENT');
  assert.throws(
      () => fs.readFileSync('chrome:\\thunder-2025\\package.json'),
      error => error.code === 'ENOENT');
  assert.throws(
      () => fs.writeFileSync(virtualPackage, '{}'),
      error => error.code === 'EROFS');
  assert.deepEqual(nativeCalls, [
    ['exists', 'C:\\test-app\\plugins\\config.json'],
    ['read_file', 'C:\\test-app\\plugins\\config.json'],
    ['exists', 'C:\\test-app\\renderer.asar\\assets\\config.json'],
    ['read_file', 'C:\\test-app\\renderer.asar\\assets\\config.json'],
  ]);
});

test('universal ipcRenderer.send and app.getName without business hooks', () => {
  const sent = [];
  const {context} = createRenderer([], {
    getRuntimeConfig: () => ({
      appName: 'demo-app',
      appVersion: '5.6.7',
      execPath: 'C:\\app\\demo.exe',
    }),
    send(...args) { sent.push(args); },
  });
  const electron = context.require('electron');
  assert.equal(electron.app.getName(), 'demo-app');
  assert.equal(electron.app.getVersion(), '5.6.7');

  // Verify business channels like AplayerWndBind are transparently forwarded
  // and do not mutate global state or check legacy hooks.
  electron.ipcRenderer.send('AplayerWndBind', 12345);
  assert.deepEqual(sent, [['AplayerWndBind', 12345]]);
  assert.equal(context.__xenonLastAplayerWnd__, undefined);
  assert.equal(context.__xenonPlayerHostApi__, undefined);
});

test('executable identity is document-scoped and is not renamed to the app name', () => {
  for (const [appName, execPath] of [
    ['DisplayName', 'C:\\apps\\one\\actual.exe'],
    ['Display Name', 'D:\\apps\\two\\launcher.exe'],
  ]) {
    const {context} = createRenderer([], {
      getRuntimeConfig: () => ({appName, execPath, appPath: 'C:\\test-app'}),
    });
    assert.equal(context.process.execPath, execPath);
    assert.equal(context.process.argv[0], execPath);
    assert.equal(context.require('electron').app.getPath('exe'), execPath);
  }
});

test('networkInterfaces returns fresh native IPv4 and IPv6 snapshots unchanged', () => {
  let reads = 0;
  const {context} = createRenderer([], {
    sendSync(channel) {
      assert.equal(channel, '__xenon:os-network-interfaces');
      ++reads;
      return {'fixture.adapter': [
        {address: '192.0.2.1', netmask: '255.255.255.0', family: 'IPv4',
          mac: '02:00:00:00:00:01', internal: false, cidr: '192.0.2.1/24'},
        {address: '::1', netmask: 'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff',
          family: 'IPv6', mac: '00:00:00:00:00:00', internal: true,
          cidr: '::1/128', scopeid: 0},
      ]};
    },
  });
  const os = context.require('node:os');
  const first = os.networkInterfaces();
  assert.equal(first['fixture.adapter'][0].family, 'IPv4');
  assert.equal(first['fixture.adapter'][1].scopeid, 0);
  assert.equal(first['fixture.adapter'][1].internal, true);
  first['fixture.adapter'].length = 0;
  assert.equal(os.networkInterfaces()['fixture.adapter'].length, 2);
  assert.equal(reads, 2);
});

test('networkInterfaces preserves an empty result and propagates native failures', () => {
  const failure = new Error('EIO: enumeration failed');
  let fail = false;
  const {context} = createRenderer([], {
    sendSync() {
      if (fail) throw failure;
      return {};
    },
  });
  assert.deepEqual(context.require('os').networkInterfaces(), {});
  fail = true;
  assert.throws(() => context.require('os').networkInterfaces(), e => e === failure);
});

test('missing exports remain undefined without mutating the export object', () => {
  const {load, calls} = createRenderer();
  const addon = load();
  const keys = Reflect.ownKeys(addon);
  for (const name of ['optionalFeature', 'OptionalClass', '', Symbol('missing')]) {
    assert.equal(addon[name], undefined);
    assert.equal(name in addon, false);
    assert.equal(Object.hasOwn(addon, name), false);
  }
  assert.deepEqual(Reflect.ownKeys(addon), keys);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('optional native calls use the caller fallback without invoking native code', () => {
  const {load, calls} = createRenderer();
  const addon = load();
  let fallbackCalls = 0;
  const fallback = () => ++fallbackCalls;
  assert.equal(addon.optionalFeature?.() ?? fallback(), 1);
  assert.equal(addon.optionalFeature?.() ?? fallback(), 2);
  assert.equal(fallbackCalls, 2);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('direct calls to missing exports fail locally without native dispatch', () => {
  const {load, calls} = createRenderer();
  const addon = load();
  assert.throws(() => addon.missing(), TypeError);
  assert.throws(() => new addon.Missing(), TypeError);
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('declared functions, classes, constants and nested exports still work', () => {
  const {load, calls} = createRenderer([
    {name: 'readValue', kind: 'function'},
    {name: 'Handle', kind: 'class', prototype: []},
    {name: 'version', kind: 'value', value: {stringValue: '1.0'}},
    {name: 'enabled', kind: 'value', value: {boolValue: false}},
    {name: 'nested', kind: 'object', children: [
      {name: 'readValue', kind: 'function'},
    ]},
  ]);
  const addon = load();
  assert.equal(addon.readValue(3), 42);
  assert.equal(addon.nested.readValue(4), 42);
  const handle = new addon.Handle(5);
  assert.equal(handle.__instanceId, 7);
  assert.equal(handle instanceof addon.Handle, true);
  assert.equal(addon.version, '1.0');
  assert.equal(addon.enabled, false);
  assert.equal(addon.nested.missing, undefined);
  assert.deepEqual(calls, [
    ['require', addonPath],
    ['invoke', addonPath, 'readValue', 3],
    ['invoke', addonPath, 'nested.readValue', 4],
    ['construct', addonPath, 'Handle', 5],
  ]);
});

test('declared native function errors still propagate instead of using a fallback', () => {
  const error = new Error('native operation failed');
  const {load} = createRenderer([{name: 'readValue', kind: 'function'}], {
    invokeNodeExportSync() { throw error; },
  });
  const addon = load();
  assert.throws(() => addon.readValue?.() ?? 'fallback', value => value === error);
});

test('module identity, readiness and existing interop aliases are preserved', async () => {
  const {load, calls} = createRenderer();
  const addon = load();
  assert.equal(load(), addon);
  assert.equal(await addon.__xenonReady, addon);
  assert.equal(await Promise.resolve(addon), addon);
  assert.equal(addon.default, addon);
  assert.equal(addon.__esModule, true);
  assert.equal(addon.then, undefined);
  addon.localValue = 9;
  assert.equal(load().localValue, 9);
  assert.equal(typeof addon.toString, 'function');
  assert.deepEqual(calls, [['require', addonPath]]);
});

test('failed loads throw synchronously and do not poison the module cache', () => {
  const error = new Error('addon load failed');
  let attempts = 0;
  const {load} = createRenderer([], {
    requireNodeModuleSync() {
      if (++attempts === 1) throw error;
      return [{name: 'version', kind: 'value', value: {intValue: 2}}];
    },
  });
  assert.throws(load, value => value === error);
  assert.equal(load().version, 2);
  assert.equal(attempts, 2);
});

test('nextTick forwards all arguments asynchronously and returns undefined', async () => {
  const {context} = createRenderer();
  const value = {reading: false};
  let received;
  const result = context.process.nextTick((...args) => {
    received = args;
  }, value, undefined, 'tail');
  assert.equal(received, undefined);
  await Promise.resolve();
  assert.deepEqual(received, [value, undefined, 'tail']);
  assert.equal(result, undefined);
});

test('nextTick preserves zero arguments and callback order', async () => {
  const {context} = createRenderer();
  const events = [];
  context.process.nextTick((...args) => {
    events.push(['first', args.length]);
    context.process.nextTick(() => events.push(['nested']));
  });
  context.process.nextTick(() => events.push(['second']));
  await Promise.resolve();
  await Promise.resolve();
  assert.deepEqual(events, [['first', 0], ['second'], ['nested']]);
});

test('nextTick rejects non-function callbacks synchronously', () => {
  const {context} = createRenderer();
  for (const callback of [undefined, null, 1, 'callback', {}]) {
    assert.throws(() => context.process.nextTick(callback), {
      name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE',
    });
  }
});

// The native backend has separate real-file SQLite tests. This transport
// fixture checks API scheduling and wire values without duplicating a SQL engine.
function createSqliteRenderer(respond = () => undefined) {
  const requests = [];
  let statementId = 0;
  const {context} = createRenderer([], {
    async invoke(channel, request) {
      assert.equal(channel, '__xenon:sqlite');
      requests.push(request);
      const result = respond(request);
      if (result !== undefined) return result;
      switch (request.operation) {
        case 'open': return 1;
        case 'prepare': return ++statementId;
        case 'run': return {lastID: 17, changes: 2};
        case 'all': return [];
        default: return null;
      }
    },
  });
  return {context, requests, sqlite: context.require('sqlite3')};
}
function drainDatabase(db) {
  return new Promise(resolve => db.wait(resolve));
}

test('Statement preparation and binding complete asynchronously with statement context', async () => {
  const {sqlite} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  const completions = [];
  const stmt = new sqlite.Statement(db, 'SELECT ?', function(err) {
    completions.push(['prepare', this, err]);
  });
  assert.equal(stmt.bind([9], function(err) {
    completions.push(['bind', this, err]);
  }), stmt);
  assert.equal(completions.length, 0);
  await drainDatabase(db);
  assert.deepEqual(completions, [
    ['prepare', stmt, null], ['bind', stmt, null],
  ]);
});

test('Database.prepare binds parameters and calls back exactly once', async () => {
  const {sqlite, requests} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  const completions = [];
  const stmt = db.prepare('SELECT ?', [9], function(err) {
    completions.push([this, err]);
  });
  assert.equal(completions.length, 0);
  await drainDatabase(db);
  assert.deepEqual(completions, [[stmt, null]]);
  assert.deepEqual(requests.map(r => r.operation), ['open', 'prepare', 'bind']);
  assert.equal(requests[1].sql, 'SELECT ?');
  assert.deepEqual(Array.from(requests[2].params), [9]);
});

for (const method of ['run', 'get', 'all']) {
  test(`Statement.${method} preserves ordering and leaves retained bindings native`, async () => {
    const {sqlite, requests} = createSqliteRenderer();
    const db = new sqlite.Database(':memory:');
    const stmt = new sqlite.Statement(db, 'SELECT ?');
    stmt.bind([9]);
    let callbackThis;
    stmt[method](function(err) {
      assert.equal(err, null);
      callbackThis = this;
    });
    await drainDatabase(db);
    assert.equal(callbackThis, stmt);
    if (method === 'run') {
      assert.equal(stmt.lastID, 17);
      assert.equal(stmt.changes, 2);
    }
    stmt[method](10);
    stmt[method]();
    stmt.reset();
    stmt[method]();
    stmt[method]([]);
    stmt.finalize();
    await drainDatabase(db);
    assert.deepEqual(requests.map(r => r.operation), [
      'open', 'prepare', 'bind', method, method, method, 'reset',
      method, method, 'finalize',
    ]);
    const executions = requests.filter(r => r.operation === method);
    assert.deepEqual(executions.map(r => r.params && Array.from(r.params)),
        [undefined, [10], undefined, undefined, []]);
  });
}

test('Statement.run propagates SQLite execution errors with statement context', async () => {
  const {sqlite} = createSqliteRenderer(request => {
    if (request.operation === 'run') throw new Error('SQLITE_CONSTRAINT: constraint failed');
  });
  const db = new sqlite.Database(':memory:');
  const stmt = new sqlite.Statement(db, 'SELECT ?');
  let completion;
  stmt.run(function(err) { completion = [this, err]; });
  await drainDatabase(db);
  assert.equal(completion[0], stmt);
  assert.equal(completion[1].code, 'SQLITE_CONSTRAINT');
  assert.equal(completion[1].errno, 19);
  assert.equal(stmt.lastID, undefined);
  assert.equal(stmt.changes, undefined);
});

test('Statement reset and finalize callbacks are asynchronous and error-first', async () => {
  const {sqlite} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  const stmt = new sqlite.Statement(db, 'SELECT ?');
  const completions = [];
  assert.equal(stmt.reset(function(err) {
    completions.push(['reset', this, err]);
  }), stmt);
  assert.equal(stmt.finalize(function(err) {
    completions.push(['finalize', this, err]);
  }), stmt);
  assert.equal(completions.length, 0);
  await drainDatabase(db);
  assert.deepEqual(completions, [
    ['reset', stmt, null], ['finalize', stmt, null],
  ]);
});

test('SQLite BLOB IPC preserves bytes including empty blobs and sliced buffers', async () => {
  const encoded = Buffer.from([0, 255, 1, 0]).toString('base64');
  const {sqlite, context, requests} = createSqliteRenderer(request => {
    if (request.operation === 'get') return {
      payload: {__xenon_sqlite_blob__: encoded},
      empty: {__xenon_sqlite_blob__: ''},
      optional: null,
    };
  });
  const db = new sqlite.Database(':memory:');
  const bytes = context.Buffer.from([3, 0, 255, 1, 0, 4]);
  const stmt = db.prepare('SELECT ?, ?', bytes.subarray(1, 5), context.Buffer.alloc(0));
  let row;
  stmt.get(function(err, result) {
    assert.equal(err, null);
    row = result;
  });
  await drainDatabase(db);
  const params = requests.find(r => r.operation === 'bind').params;
  assert.equal(params[0].__xenon_sqlite_blob__, encoded);
  assert.equal(params[1].__xenon_sqlite_blob__, '');
  assert.equal(context.Buffer.isBuffer(row.payload), true);
  assert.deepEqual(Array.from(row.payload), [0, 255, 1, 0]);
  assert.equal(row.empty.length, 0);
  assert.equal(row.optional, null);
});

test('Database wrappers finalize statements and each emits rows before completion', async () => {
  const {sqlite, requests} = createSqliteRenderer(request => {
    if (request.operation === 'all') return [{value: 2}, {value: 3}];
  });
  const db = new sqlite.Database(':memory:');
  const events = [];
  assert.equal(db.each('SELECT value FROM records', function(err, row) {
    assert.equal(err, null);
    events.push(row.value);
  }, function(err, count) {
    assert.equal(err, null);
    events.push(['complete', count]);
  }), db);
  await drainDatabase(db);
  assert.deepEqual(events, [2, 3, ['complete', 2]]);
  assert.deepEqual(requests.map(r => r.operation), ['open', 'prepare', 'all', 'finalize']);
});

test('Database open errors are reported and never become successful operations', async () => {
  const {sqlite} = createSqliteRenderer(request => {
    if (request.operation === 'open') throw new Error('SQLITE_CANTOPEN: cannot open database');
  });
  const errors = [];
  const db = new sqlite.Database('C:\\missing\\records.db', err => errors.push(err.code));
  db.exec('CREATE TABLE records(value)', err => errors.push(err.code));
  await drainDatabase(db);
  assert.equal(db.open, false);
  assert.deepEqual(errors, ['SQLITE_CANTOPEN', 'SQLITE_CANTOPEN']);
});

test('Database close waits for statements and updates open only after success', async () => {
  let closeCalls = 0;
  const {sqlite} = createSqliteRenderer(request => {
    if (request.operation === 'close' && ++closeCalls === 1) {
      throw new Error('SQLITE_BUSY: unfinalized statements');
    }
  });
  const db = new sqlite.Database(':memory:');
  const states = [];
  db.close(err => states.push([err.code, db.open]));
  db.close(function(err) {
    assert.equal(this, db);
    states.push([err, db.open]);
  });
  await drainDatabase(db);
  assert.deepEqual(states, [['SQLITE_BUSY', true], [null, false]]);
});

test('Database.prepare reports invalid SQL once and drops queued operations', async () => {
  const {sqlite, requests} = createSqliteRenderer(request => {
    if (request.operation === 'prepare') throw new Error('SQLITE_ERROR: invalid SQL');
  });
  const db = new sqlite.Database(':memory:');
  const completions = [];
  db.prepare('invalid SQL', [1], err => completions.push(err.code)).finalize();
  await drainDatabase(db);
  assert.deepEqual(completions, ['SQLITE_ERROR']);
  assert.deepEqual(requests.map(r => r.operation), ['open', 'prepare']);
});

test('SQLite named parameters preserve names and BLOB values', async () => {
  const {sqlite, context, requests} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  db.run('INSERT INTO records VALUES ($id, $data)', {
    $id: 1, $data: context.Buffer.from([0, 255]),
  });
  await drainDatabase(db);
  const params = requests.find(r => r.operation === 'run').params;
  assert.deepEqual(Object.keys(params), ['$id', '$data']);
  assert.equal(params.$id, 1);
  assert.equal(params.$data.__xenon_sqlite_blob__, 'AP8=');
});

test('SQLite distinguishes optional undefined parameters from SQL null', async () => {
  const {sqlite, requests} = createSqliteRenderer();
  const db = new sqlite.Database(':memory:');
  db.prepare('SELECT 1', undefined, () => {}).finalize();
  db.prepare('SELECT ?', null, () => {}).finalize();
  await drainDatabase(db);
  const bindings = requests.filter(r => r.operation === 'bind');
  assert.equal(bindings[0].params[0].__xenon_sqlite_undefined__, true);
  assert.equal(bindings[1].params[0], null);
});

test('CommonJS bindings formatter restoration keeps default Error.stack a string', () => {
  const source = `module.exports = function() {
    const previous = Error.prepareStackTrace;
    Error.prepareStackTrace = (_error, sites) => sites;
    const customIsArray = Array.isArray(new Error().stack);
    const saved = Error.prepareStackTrace;
    Error.prepareStackTrace = () => 'temporary';
    Error.prepareStackTrace = saved;
    const sameFormatter = Error.prepareStackTrace === saved;
    Error.prepareStackTrace = previous;
    return {customIsArray, sameFormatter, stackType: typeof new Error().stack};
  };`;
  const files = new Map();
  const {context} = createRenderer([], {
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'exists') return files.has(request.path);
      if (request.operation === 'write_file') {
        files.set(request.path,
            Buffer.from(request.dataBase64, 'base64').toString('utf8'));
        return true;
      }
      if (request.operation === 'read_file') {
        assert.ok(files.has(request.path));
        return Buffer.from(files.get(request.path)).toString('base64');
      }
      throw new Error(`unexpected fs operation: ${request.operation}`);
    },
  });
  context.require('fs').writeFileSync('C:\\test-app\\stack-fixture.cjs', source, 'utf8');
  const result = context.require('C:\\test-app\\stack-fixture.cjs')();
  assert.equal(result.customIsArray, true);
  assert.equal(result.sameFormatter, true);
  assert.equal(result.stackType, 'string');
});
