// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const {existsSync, readFileSync, statSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = readBootstrap(path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js'));
const playerRoot = path.resolve(__dirname, '../../../out/Release_64/xenon_player');
const bundlePath = process.env.XENON_TEST_PLAYER_BUNDLE;
const hasPlayerRuntime = existsSync(path.join(playerRoot, 'resources/app/package.json')) &&
    existsSync(path.join(playerRoot, 'resources/app/out.asar'));
const playerBundleSkip = bundlePath === undefined ?
    'requires XENON_TEST_PLAYER_BUNDLE from the matching packaged application' : false;
const addonPath = 'C:\\test-app\\node_sqlite3.node';
const methods = {
  Database: ['close', 'exec', 'wait', 'configure', 'serialize', 'parallelize', 'interrupt'],
  Statement: ['bind', 'run', 'get', 'all', 'each', 'reset', 'finalize'],
  Backup: ['step', 'finish'],
};

// Execute the packaged application's actual sqlite3.js and trace.js factories.
// Only native transport is replaced; this checks the JS/native bridge contract,
// not SQLite's SQL engine (covered separately by real-addon tests).
function sqliteRenderer(metadataMode = 'exact') {
  assert.ok(bundlePath && existsSync(bundlePath) && statSync(bundlePath).isFile(),
      'XENON_TEST_PLAYER_BUNDLE must name a regular bundle file: ' + bundlePath);
  assert.ok(hasPlayerRuntime,
      'Installed PLE requires resources/app/package.json and resources/app/out.asar');
  let dispatch, nextId = 1;
  const instances = new Map(), calls = [], callbackErrors = [];
  function wire(id) {
    const instance = instances.get(id);
    return {__xenon_node_wire_type__: 'native_instance', module_path: addonPath,
      instance_id: id, owner_token: 'sqlite-owner', class_name: instance.className,
      prototype: methods[instance.className].map(name => ({name, kind: 'function'})),
      fields: {...instance.fields}};
  }
  function callback(descriptor, args, id) {
    if (descriptor?.__xenon_node_wire_type__ !== 'callback') return;
    queueMicrotask(() => dispatch('__xenon:node-addon:callback',
      [descriptor.callback_id, args, wire(id)]));
  }
  function construct(_module, className, prototype, ...args) {
    const id = nextId++;
    const fields = className === 'Database' ? {filename: args[0], open: args[0] !== 'missing'} : {};
    instances.set(id, {className, fields, prototype});
    calls.push({operation: 'construct', className, prototype, id});
    const error = fields.open === false ? {code: 'SQLITE_CANTOPEN', errno: 14,
      message: 'SQLITE_CANTOPEN: unable to open database file'} : null;
    callback(args.at(-1), [error], id);
    if (className === 'Database' && !error) callback(prototype.emit, ['open'], id);
    return {instance_id: id, owner_token: 'sqlite-owner'};
  }
  function describeClass(name) {
    return {name, kind: 'class', children: [
      {name: 'prototype', kind: 'object', enumerable: false, writable: false},
    ], prototype: methods[name].map(name =>
      ({name, kind: 'function', enumerable: false, writable: true}))};
  }
  const transport = {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app', exeDir: 'C:\\test-app',
      execPath: 'C:\\test-app\\host.exe'}),
    setDispatchHandler(value) { dispatch = value; },
    sendSync(_channel, request) {
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'stat' && request.path === addonPath)
        return {isFile: true, isDirectory: false};
      throw new Error('ENOENT');
    },
    requireNodeModuleSync: () => metadataMode === 'legacy' ?
      Object.keys(methods).map(describeClass) :
      {kind: 'object', children: Object.keys(methods).map(name =>
        ({name, kind: 'class', enumerable: true, writable: true}))},
    ...(metadataMode === 'legacy' ? {} : {
      inspectNodeExportSync(_module, exportPath) {
        assert.ok(Object.hasOwn(methods, exportPath), 'Unexpected export inspection: ' + exportPath);
        calls.push({operation: 'inspect', className: exportPath});
        return describeClass(exportPath);
      },
    }),
    constructNodeExportSync: (module, name, ...args) => construct(module, name, {}, ...args),
    constructNodeExportWithPrototypeSync: construct,
    invokeNodeInstanceSync(_module, selector, method, ...args) {
      const id = selector.instanceId;
      const instance = instances.get(id);
      calls.push({operation: method, id});
      if (method === 'run') Object.assign(instance.fields, {lastID: 42, changes: 1});
      if (method === 'close') instance.fields.open = false;
      callback(args.at(-1), [null], id);
      return wire(id);
    },
    inspectNodeInstanceMemberSync(_module, selector, name) {
      const instance = instances.get(selector.instanceId);
      return Object.hasOwn(instance.fields, name) ?
        {kind: 'value', value: instance.fields[name]} : {kind: 'undefined'};
    },
    releaseNodeInstance() {},
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    xenonIpcRenderer: transport, TextEncoder, TextDecoder,
    URL, URLSearchParams, atob, btoa, queueMicrotask, setTimeout, clearTimeout,
    console: {log() {}, warn() {}, error(...args) { callbackErrors.push(args); }},
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''}});
  vm.runInContext(source, context);
  vm.runInContext(readFileSync(bundlePath, 'utf8'), context, {filename: '58.js'});
  const factories = context.webpackChunk_electron_main_renderer.at(-1)[1];
  const addon = context.require(addonPath);
  const moduleCache = new Map();
  function requireBundled(id) {
    if (id === 42522) return addon;
    if (id === 71017) return context.require('path');
    if (id === 82361) return context.require('events');
    if (id === 73837) return context.require('util');
    assert.ok(id === 67312 || id === 74219, 'Unexpected bundled dependency: ' + id);
    if (!moduleCache.has(id)) {
      const module = {exports: {}};
      moduleCache.set(id, module);
      factories[id](module, module.exports, requireBundled);
    }
    return moduleCache.get(id).exports;
  }
  return {sqlite: requireBundled(67312), calls, callbackErrors};
}

test('packaged SQLite supports verbose, open events, callback receivers and run/finalize chains',
    {skip: playerBundleSkip}, async () => {
  const {sqlite, calls, callbackErrors} = sqliteRenderer();
  sqlite.verbose();
  let db;
  let openEvent = false;
  const opened = new Promise(resolve => {
    db = new sqlite.Database(':memory:', function(error) {
      assert.equal(error, null);
      assert.equal(this, db);
      resolve();
    });
    db.on('open', () => { openEvent = true; });
  });
  await opened;
  assert.equal(openEvent, true);
  const overlay = calls.find(call => call.operation === 'construct').prototype;
  assert.ok(calls.some(call => call.operation === 'inspect' && call.className === 'Database'),
      'The current host supplies a shallow root followed by exact constructor metadata');
  assert.equal(overlay.emit.__xenon_node_wire_type__, 'callback');
  assert.equal(Object.hasOwn(overlay, 'exec'), false, 'Native overrides stay local');
  await new Promise(resolve => {
    assert.equal(db.exec('CREATE TABLE example(value)', function(error) {
      assert.equal(error, null);
      assert.equal(this, db);
      resolve();
    }), db);
  });
  await new Promise(resolve => {
    assert.equal(db.run('INSERT INTO example VALUES (?)', 1, function(error) {
      assert.equal(error, null);
      assert.equal(this.lastID, 42);
      assert.equal(this.changes, 1);
      assert.ok(this instanceof sqlite.Statement);
      resolve();
    }), db);
  });
  assert.ok(calls.some(call => call.operation === 'finalize'));
  await new Promise(resolve => db.close(function(error) {
    assert.equal(error, null);
    assert.equal(this, db);
    resolve();
  }));
  assert.deepEqual(callbackErrors, []);
});

test('packaged SQLite retains prototype additions with the legacy array transport',
    {skip: playerBundleSkip}, async () => {
  const {sqlite, calls, callbackErrors} = sqliteRenderer('legacy');
  let db;
  await new Promise(resolve => {
    db = new sqlite.Database(':memory:', function(error) {
      assert.equal(error, null);
      assert.equal(this, db);
      resolve();
    });
  });
  assert.equal(calls.find(call => call.operation === 'construct').prototype.emit
      .__xenon_node_wire_type__, 'callback');
  assert.deepEqual(callbackErrors, []);
});

test('packaged SQLite verbose prepare preserves its native bind method and callback receiver',
    {skip: playerBundleSkip}, async () => {
  const {sqlite, calls, callbackErrors} = sqliteRenderer();
  assert.equal(typeof sqlite.Statement.prototype.bind, 'function');
  sqlite.verbose();
  let db;
  await new Promise(resolve => {
    db = new sqlite.Database(':memory:', function(error) {
      assert.equal(error, null);
      resolve();
    });
  });
  let statement;
  await new Promise(resolve => {
    statement = db.prepare('SELECT 1', undefined, function(error) {
      assert.equal(error, null);
      assert.equal(this, statement);
      assert.ok(this instanceof sqlite.Statement);
      resolve();
    });
    assert.ok(statement instanceof sqlite.Statement);
  });
  assert.ok(calls.some(call => call.operation === 'bind'));
  const overlay = calls.find(call => call.operation === 'construct' &&
      call.className === 'Statement').prototype;
  assert.equal(Object.hasOwn(overlay, 'bind'), false,
      'The verbose wrapper must call the original native bind without an overlay recursion');
  await new Promise(resolve => statement.finalize(resolve));
  await new Promise(resolve => db.close(resolve));
  assert.deepEqual(callbackErrors, []);
});

test('packaged SQLite preserves open failure and Database callback context',
    {skip: playerBundleSkip}, async () => {
  const {sqlite, callbackErrors} = sqliteRenderer();
  let db;
  await new Promise(resolve => {
    db = new sqlite.Database('missing', function(error) {
      assert.equal(this, db);
      assert.equal(error.code, 'SQLITE_CANTOPEN');
      assert.equal(error.errno, 14);
      resolve();
    });
  });
  assert.deepEqual(callbackErrors, []);
});
