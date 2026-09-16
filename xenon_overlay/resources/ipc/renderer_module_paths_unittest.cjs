// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run with: node --test xenon_overlay/resources/ipc/renderer_module_paths_unittest.cjs
const assert = require('node:assert/strict');
const {existsSync, readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const bootstrapSource = readFileSync(process.env.XENON_TEST_BOOTSTRAP ||
    path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js'), 'utf8');

function createPathRenderer(files, aliases = new Map(), appRoot = 'C:\\test-app', runtime = {}) {
  const reads = [];
  const realpaths = [];
  const nativeLoads = [];
  const operations = [];
  const dirs = new Map();
  const normalizedFiles = new Map();
  for (const [filename, contents] of files) {
    const normalized = path.win32.normalize(filename);
    normalizedFiles.set(normalized.toLowerCase(), {filename: normalized, contents});
    for (let dir = path.win32.dirname(normalized);;) {
      dirs.set(dir.toLowerCase(), dir);
      const parent = path.win32.dirname(dir);
      if (parent === dir) break;
      dir = parent;
    }
  }
  const prefixAliases = [...aliases].sort((a, b) => b[0].length - a[0].length);
  function realEntry(requested) {
    let filename = path.win32.normalize(requested);
    for (const [alias, target] of prefixAliases) {
      if (filename.toLowerCase() === alias.toLowerCase() ||
          filename.toLowerCase().startsWith(alias.toLowerCase() + '\\')) {
        filename = target + filename.slice(alias.length);
        break;
      }
    }
    const file = normalizedFiles.get(filename.toLowerCase());
    if (file) return {...file, isFile: true, isDirectory: false};
    const dir = dirs.get(filename.toLowerCase());
    if (dir) return {filename: dir, isFile: false, isDirectory: true};
    throw Object.assign(new Error('ENOENT: no such file or directory'), {code: 'ENOENT'});
  }
  const transport = {
    getRuntimeConfig: () => ({
      appPath: appRoot,
      exeDir: appRoot,
      execPath: appRoot + '\\host.exe',
      documentPath: appRoot + '\\index.html',
      ...runtime.config,
    }),
    setDispatchHandler() {},
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      operations.push({operation: request.operation, path: request.path});
      const entry = realEntry(request.path);
      switch (request.operation) {
        case 'access': return undefined;
        case 'stat': return {isFile: entry.isFile, isDirectory: entry.isDirectory};
        case 'realpath': {
          realpaths.push(request.path);
          // The production Browser bridge returns an ASAR member's virtual
          // path, since it cannot be normalized as a standalone OS file.
          return /\.asar(?:[\\/]|$)/i.test(request.path) ?
              path.win32.normalize(request.path) : entry.filename;
        }
        case 'read_file':
          reads.push(entry.filename);
          return Buffer.from(entry.contents).toString('base64');
        default: throw new Error('Unexpected filesystem operation: ' + request.operation);
      }
    },
    requireNodeModuleSync(filename) {
      nativeLoads.push(filename);
      return [{name: 'version', kind: 'value', value: {intValue: 42}}];
    },
  };
  const context = vm.createContext({
    xenonIpcRenderer: transport,
    TextEncoder,
    TextDecoder,
    URL,
    URLSearchParams,
    queueMicrotask,
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    atob,
    btoa,
    location: runtime.location || {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(bootstrapSource, context);
  return {context, reads, realpaths, nativeLoads, operations,
    setFile(filename, contents) {
      filename = path.win32.normalize(filename);
      normalizedFiles.set(filename.toLowerCase(), {filename, contents});
      for (let dir = path.win32.dirname(filename);;) {
        dirs.set(dir.toLowerCase(), dir);
        const parent = path.win32.dirname(dir);
        if (parent === dir) break;
        dir = parent;
      }
    },
    removeFile(filename) {
      normalizedFiles.delete(path.win32.normalize(filename).toLowerCase());
    },
    setAlias(alias, target) {
      const entry = prefixAliases.find(entry => entry[0] === alias);
      if (entry) entry[1] = target;
      else prefixAliases.push([alias, target]);
    },
  };
}

test('disk aliases and Windows casing share the canonical CommonJS cache key', () => {
  const canonical = 'C:\\test-app\\Module.js';
  const {context, reads} = createPathRenderer(new Map([
    [canonical, 'globalThis.loads = (globalThis.loads || 0) + 1; module.exports = {};'],
  ]), new Map([['C:\\test-app\\alias.js', canonical]]));
  const first = context.require('./Module.js');
  assert.equal(context.require('./alias.js'), first);
  assert.equal(context.require('./MODULE.JS'), first);
  assert.equal(context.require.resolve('./alias.js'), canonical);
  assert.equal(context.loads, 1);
  assert.deepEqual(reads, [canonical]);
});

test('native aliases use one canonical module path and one export object', () => {
  const canonical = 'C:\\test-app\\Fixture.node';
  const {context, nativeLoads} = createPathRenderer(new Map([
    [canonical, 'native placeholder'],
  ]), new Map([['C:\\test-app\\alias.node', canonical]]));
  const first = context.require('./Fixture.node');
  assert.equal(context.require('./alias.node'), first);
  assert.equal(context.require('./FIXTURE.NODE'), first);
  assert.equal(context.require.resolve('./alias.node'), canonical);
  assert.deepEqual(nativeLoads, [canonical]);
});

test('a symlink outside the permitted roots fails before reading or fallback', () => {
  const {context, reads} = createPathRenderer(new Map([
    ['C:\\test-app\\inside.js', 'module.exports = true;'],
    ['C:\\test-app\\secret.js', 'module.exports = "wrong fallback";'],
    ['C:\\outside\\secret.js', 'module.exports = "outside";'],
  ]), new Map([['C:\\test-app\\secret', 'C:\\outside\\secret.js']]));
  assert.throws(() => context.require('./secret'), {code: 'MODULE_NOT_FOUND'});
  assert.throws(() => context.require.resolve('./secret'), {code: 'MODULE_NOT_FOUND'});
  assert.deepEqual(reads, []);
});

test('package metadata behind an escaping directory symlink is never read', () => {
  const {context, reads} = createPathRenderer(new Map([
    ['C:\\test-app\\inside.js', 'module.exports = true;'],
    ['C:\\outside\\pkg\\package.json', '{"main":"private.js"}'],
    ['C:\\outside\\pkg\\private.js', 'module.exports = true;'],
  ]), new Map([
    ['C:\\test-app\\node_modules\\pkg', 'C:\\outside\\pkg'],
  ]));
  assert.throws(() => context.require('pkg'), {code: 'MODULE_NOT_FOUND'});
  assert.deepEqual(reads, []);
});

test('an explicitly configured root symlink permits its canonical descendants', () => {
  const canonical = 'D:\\installed-app\\entry.js';
  const {context} = createPathRenderer(new Map([
    [canonical, 'module.exports = require("dependency");'],
    ['D:\\installed-app\\node_modules\\dependency\\index.json', '42'],
  ]), new Map([['C:\\app-link', 'D:\\installed-app']]), 'C:\\app-link');
  assert.equal(context.require('./entry.js'), 42);
  assert.equal(context.require.resolve('./entry.js'), canonical);
});

test('ASAR modules retain the virtual realpath supplied by the Browser bridge', () => {
  const appRoot = 'C:\\test-app\\application.asar';
  const canonical = appRoot + '\\entry.js';
  const {context, reads, realpaths} = createPathRenderer(new Map([
    [canonical, 'module.exports = require("./data.json").value;'],
    [appRoot + '\\data.json', '{"value":42}'],
  ]), new Map(), appRoot);
  assert.equal(context.require('./entry.js'), 42);
  assert.equal(context.require.resolve('./entry.js'), canonical);
  assert.deepEqual(reads, [canonical, appRoot + '\\data.json']);
  assert.ok(realpaths.includes(canonical));
});

test('a sibling with the same root prefix is outside the permitted roots', () => {
  const {context, reads, nativeLoads} = createPathRenderer(new Map([
    ['C:\\test-app\\inside.js', 'module.exports = true;'],
    ['C:\\test-app-other\\outside.node', 'native placeholder'],
  ]));
  assert.throws(() => context.require('C:\\test-app-other\\outside.node'),
                {code: 'MODULE_NOT_FOUND'});
  assert.deepEqual(reads, []);
  assert.deepEqual(nativeLoads, []);
});

test('a cold require resolves its file once and warm requires perform no filesystem IPC', () => {
  const filename = 'C:\\test-app\\entry.js';
  const {context, operations} = createPathRenderer(new Map([
    [filename, 'module.exports = {ready: true};'],
  ]));
  const first = context.require('./entry.js');
  assert.equal(operations.filter(op => op.operation === 'stat' && op.path === filename).length, 1);
  assert.equal(operations.filter(op => op.operation === 'realpath' && op.path === filename).length, 1);
  const coldOperations = operations.length;
  for (let count = 0; count < 1000; ++count) {
    assert.equal(context.require('./entry.js'), first);
    assert.equal(context.require.resolve('./entry.js'), filename);
  }
  assert.equal(operations.length, coldOperations);
});

test('deleting a module cache record rechecks a retargeted symlink against roots', () => {
  const filename = 'C:\\test-app\\entry.js';
  const alias = 'C:\\test-app\\alias.js';
  const harness = createPathRenderer(new Map([
    [filename, 'module.exports = true;'],
    ['C:\\outside\\private.js', 'module.exports = "outside";'],
  ]), new Map([[alias, filename]]));
  assert.equal(harness.context.require('./alias.js'), true);
  delete harness.context.require.cache[filename];
  harness.setAlias(alias, 'C:\\outside\\private.js');
  assert.throws(() => harness.context.require('./alias.js'), {code: 'MODULE_NOT_FOUND'});
  assert.deepEqual(harness.reads, [filename]);
});

test('failed evaluation and resolution retry using the current files', () => {
  const filename = 'C:\\test-app\\retry.js';
  const harness = createPathRenderer(new Map([
    [filename, 'throw new Error("first load failed");'],
  ]));
  assert.throws(() => harness.context.require('./retry'), /first load failed/);
  harness.removeFile(filename);
  harness.setFile('C:\\test-app\\retry.json', '42');
  assert.equal(harness.context.require('./retry'), 42);
  assert.throws(() => harness.context.require('./created-later'), {code: 'MODULE_NOT_FOUND'});
  harness.setFile('C:\\test-app\\created-later.js', 'module.exports = 73;');
  assert.equal(harness.context.require('./created-later'), 73);
});

test('require.resolve does not retain an unloaded path after it disappears', () => {
  const filename = 'C:\\test-app\\unloaded.js';
  const harness = createPathRenderer(new Map([[filename, 'module.exports = true;']]));
  assert.equal(harness.context.require.resolve('./unloaded'), filename);
  harness.removeFile(filename);
  assert.throws(() => harness.context.require('./unloaded'), {code: 'MODULE_NOT_FOUND'});
  harness.setFile('C:\\test-app\\unloaded.json', '91');
  assert.equal(harness.context.require('./unloaded'), 91);
});

function stackFilename(context, filename) {
  return vm.runInContext(`(() => {
    const prior = Error.prepareStackTrace;
    try {
      Error.prepareStackTrace = (_error, frames) => {
        if (typeof frames[0].isEval !== 'function' ||
            typeof frames[0].getThis !== 'function') throw new Error('CallSite shape lost');
        return frames[0].getFileName();
      };
      const error = {};
      Error.captureStackTrace(error);
      return error.stack;
    } finally { Error.prepareStackTrace = prior; }
  })()`, context, {filename});
}

test('document scripts receive declared module filenames before any require', () => {
  const root = 'C:\\test-app';
  const {context} = createPathRenderer(new Map([[root + '\\package.json', '{}']]),
      new Map(), root, {config: {rendererUrlMappings: [
        {sourcePathPrefix: root + '\\out\\main-renderer', targetBaseUrl: 'chrome://player/'},
        {sourcePathPrefix: root + '\\out\\settings', targetBaseUrl: 'chrome://player/settings/'},
      ]}});
  assert.equal(stackFilename(context, 'chrome://player/static/js/881.js?version=2'),
               root + '\\out\\main-renderer\\static\\js\\881.js');
  assert.equal(stackFilename(context, 'chrome://player/settings/chunk.js'),
               root + '\\out\\settings\\chunk.js');
  assert.equal(stackFilename(context, 'chrome://unrelated/static/js/881.js'),
               'chrome://unrelated/static/js/881.js');
  assert.equal(typeof vm.runInContext('new Error("ordinary").stack', context), 'string');
});

test('module source mapping rejects encoded separators and preserves canonical root checks', () => {
  const root = 'C:\\test-app';
  const outside = 'C:\\outside';
  const harness = createPathRenderer(new Map([
    [root + '\\package.json', '{}'], [outside + '\\private.js', 'module.exports = 1;'],
  ]), new Map(), root, {config: {rendererUrlMappings: [
    {sourcePathPrefix: outside, targetBaseUrl: 'chrome://player/'},
  ]}});
  for (const url of ['chrome://player/a%2f..%2fprivate.js',
                     'chrome://player/a%5c..%5cprivate.js']) {
    assert.equal(stackFilename(harness.context, url), url);
  }
  assert.throws(() => harness.context.require('./private.js', 'chrome://player/index.html'),
                {code: 'MODULE_NOT_FOUND'});
  assert.deepEqual(harness.reads, []);
});

test('old hosts map only the matching document origin and preserve nested page paths', () => {
  const root = 'C:\\test-app';
  const {context} = createPathRenderer(new Map([[root + '\\package.json', '{}']]),
      new Map(), root, {
        config: {documentPath: root + '\\dialogs\\index.html'},
        location: {href: 'chrome://player/dialogs/index.html', protocol: 'chrome:',
                   hostname: 'player', pathname: '/dialogs/index.html'},
      });
  assert.equal(stackFilename(context, 'chrome://player/static/chunk.js'),
               root + '\\static\\chunk.js');
  assert.equal(stackFilename(context, 'https://player/static/chunk.js'),
               'https://player/static/chunk.js');
});

const playerRoot = path.resolve(__dirname, '../../../out/Release_64/xenon_player');
const actualBundlePath = path.join(playerRoot, 'frontend/static/js/58.js');
const hasPlayerRuntime = existsSync(path.join(playerRoot, 'main/package.json')) &&
    existsSync(path.join(playerRoot, 'frontend/index.html'));
for (const app of ['player', 'thunder']) {
  test(`actual bundled bindings resolves ${app} SQLite from its declared package`,
       {skip: !hasPlayerRuntime}, () => {
    // Exercise the shipped webpack bindings factory itself. The numeric module
    // IDs are dependencies of this exact bundle, not loader special cases.
    assert.ok(existsSync(actualBundlePath),
        'Installed PLE is missing its SQLite consumer bundle: ' + actualBundlePath);
    const root = 'C:\\runtime\\' + app;
    const packageRoot = root + '\\resources\\app';
    const addon = packageRoot + '\\Release\\node_sqlite3.node';
    const harness = createPathRenderer(new Map([
      [root + '\\package.json', '{}'],
      [packageRoot + '\\package.json', '{"name":"real-application"}'],
      [addon, 'native placeholder'],
      [root + '\\Release\\node_sqlite3.node', 'wrong same basename'],
    ]), new Map(), root, {config: {
      documentPath: packageRoot + '\\out\\main-renderer\\index.html',
      rendererUrlMappings: [{sourcePathPrefix: packageRoot + '\\out\\main-renderer',
                             targetBaseUrl: `chrome://${app}/`}],
    }});
    const {context, nativeLoads} = harness;
    vm.runInContext(readFileSync(actualBundlePath, 'utf8'), context,
        {filename: `chrome://${app}/static/js/58.js`});
    assert.equal(typeof context.webpackChunk_electron_main_renderer.at(-1)[1][57353],
        'function', 'packaged bindings factory is present');
    const value = vm.runInContext(`(() => {
      const module = {exports: {}};
      const dependencies = id => id === 57147 ? require('fs') :
          id === 71017 ? require('path') : require('url').fileURLToPath;
      const factory = global.webpackChunk_electron_main_renderer.at(-1)[1][57353];
      factory(module, module.exports, dependencies);
      globalThis.actualBundledBindings = module.exports;
      return module.exports('node_sqlite3');
    })()`, context, {filename: `chrome://${app}/static/js/58.js`});
    assert.equal(value.version, 42);
    assert.deepEqual(nativeLoads, [addon]);
    assert.equal(context.__filename, packageRoot + '\\out\\main-renderer\\index.html');
    harness.removeFile(addon);
    delete context.require.cache[addon];
    assert.throws(() => context.actualBundledBindings('node_sqlite3'),
                  /Could not locate the bindings file/);
    assert.deepEqual(nativeLoads, [addon], 'a same-name addon in another package is never substituted');
  });
}
