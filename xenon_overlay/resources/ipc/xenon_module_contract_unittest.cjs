// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const {spawnSync} = require('node:child_process');
const nativeModule = require('node:module');
const path = require('node:path');
const url = require('node:url');
const test = require('node:test');
const vm = require('node:vm');

function loadModule(pathImplementation = path, urlImplementation = url) {
  const bindings = [];
  const context = vm.createContext({pathImplementation, urlImplementation,
    isBuiltin: nativeModule.isBuiltin,
    bindFilename(filename) {
      bindings.push(filename);
      return nativeModule.createRequire(filename);
    }});
  vm.runInContext(readBootstrapPart('common/module.js') +
      '\nglobalThis.module = createModuleModule(pathImplementation, urlImplementation, bindFilename, isBuiltin);',
      context);
  return {module: context.module, bindings};
}

test('module isBuiltin checks the registered loader names without coercion', () => {
  const {module, bindings} = loadModule();
  for (const name of [undefined, null, true, 3, {}, [], Symbol('fs'),
    'fs', 'node:fs', 'fs/promises', 'node:stream/promises', 'node:test',
    'electron', 'node:electron', 'node:missing', 'missing', 'node:node:fs',
    '', 'fs\0', 'FS', 'fs/', './fs']) {
    assert.equal(module.isBuiltin(name), nativeModule.isBuiltin(name));
  }
  assert.equal(bindings.length, 0, 'queries must never load a module');
});

test('module lookup helpers preserve Node path ordering and parent paths', () => {
  const {module} = loadModule();
  for (const directory of [process.cwd(), path.parse(process.cwd()).root,
    path.join(process.cwd(), 'node_modules', 'pkg'),
    path.join(process.cwd(), 'NODE_MODULES', 'pkg'), '']) {
    assert.deepEqual(Array.from(module._nodeModulePaths(directory)), nativeModule._nodeModulePaths(directory));
  }
  for (const invalid of [undefined, null, 3, {}, []]) {
    assert.throws(() => module._nodeModulePaths(invalid), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  const parent = {filename: __filename, id: __filename, paths: [path.join(__dirname, 'vendor'), __dirname]};
  for (const request of ['fs', 'node:fs', '.', '..', './file', '../file', 'package', '/absolute']) {
    const expected = nativeModule._resolveLookupPaths(request, parent);
    // The host intentionally has no implicit global/user module roots.
    const scoped = expected?.filter(item => !nativeModule.globalPaths.includes(item));
    assert.deepEqual(module._resolveLookupPaths(request, parent) === null ? null :
        Array.from(module._resolveLookupPaths(request, parent)), scoped ?? null);
  }
  const paths = module._resolveLookupPaths('package', parent);
  paths.push('not-shared');
  assert.equal(parent.paths.length, 2);
  module.globalPaths.push('not-an-authorized-global-root');
  assert.deepEqual(Array.from(module._resolveLookupPaths('package', parent)), parent.paths);
});

test('module directory lookup supports both Windows and POSIX path rules', () => {
  const windows = loadModule(path.win32).module;
  assert.deepEqual(Array.from(windows._nodeModulePaths('C:\\app\\node_modules\\pkg')), [
    'C:\\app\\node_modules\\pkg\\node_modules', 'C:\\app\\node_modules', 'C:\\node_modules',
  ]);
  const posix = loadModule(path.posix).module;
  assert.deepEqual(Array.from(posix._nodeModulePaths('/app/node_modules/pkg')), [
    '/app/node_modules/pkg/node_modules', '/app/node_modules', '/node_modules',
  ]);
  assert.deepEqual(Array.from(posix._resolveLookupPaths('./file', {filename: '/app/main.js', id: 'main'})), ['/app']);
  assert.deepEqual(Array.from(posix._resolveLookupPaths('./file', {filename: '/app/main.js'})), ['.']);
});

test('module compile-cache status uses Node constants and reports disabled honestly', () => {
  const {module} = loadModule();
  assert.deepEqual({...module.constants.compileCacheStatus},
                   {...nativeModule.constants.compileCacheStatus});
  assert.equal(module.getCompileCacheDir(), undefined);
  for (const options of [undefined, '/cache/modules', {directory: '/cache/modules'}]) {
    const result = module.enableCompileCache(options);
    assert.equal(result.status, module.constants.compileCacheStatus.DISABLED);
    assert.match(result.message, /not supported/);
    assert.equal(Object.hasOwn(result, 'directory'), false);
    assert.equal(module.getCompileCacheDir(), undefined);
  }
  assert.equal(Object.prototype.propertyIsEnumerable.call(
      module, 'getCompileCacheDir'), true);
  assert.equal(Object.prototype.propertyIsEnumerable.call(
      module, 'enableCompileCache'), true);
  assert.equal(Object.prototype.propertyIsEnumerable.call(
      module, 'flushCompileCache'), true);
});

test('disabled compile-cache queries and flush match native Node', () => {
  function observeCache(module) {
    const beforeUndefined = module.getCompileCacheDir() === undefined;
    const flushBeforeUndefined = module.flushCompileCache() === undefined;
    const status = module.enableCompileCache().status;
    const flushAfterUndefined = module.flushCompileCache() === undefined;
    return {beforeUndefined, flushBeforeUndefined, status, flushAfterUndefined,
      afterUndefined: module.getCompileCacheDir() === undefined};
  }
  const native = spawnSync(process.execPath, ['-e',
    `console.log(JSON.stringify((${observeCache})(require('node:module'))));`], {
    env: {...process.env, NODE_DISABLE_COMPILE_CACHE: '1'}, encoding: 'utf8',
  });
  assert.equal(native.status, 0, native.stderr);
  const {module} = loadModule();
  assert.deepEqual(observeCache(module), JSON.parse(native.stdout));
});

test('createRequire matches Node argument errors before invoking the loader', () => {
  const {module, bindings} = loadModule();
  for (const filename of [undefined, null, false, 7, {}, {href: 'file:///x.js'},
    Buffer.from(__filename), '', './entry.js', 'entry.js', 'https://example.test/a.js',
    new URL('https://example.test/a.js'), 'file:///C:/a%2fb.js',
    new URL('file:///C:/a%2fb.js')]) {
    let expected;
    try { nativeModule.createRequire(filename); } catch (error) { expected = error; }
    assert.ok(expected, 'fixture must be rejected by Node');
    assert.throws(() => module.createRequire(filename),
                  {name: expected.name, code: expected.code});
  }
  assert.equal(bindings.length, 0);
});

test('createRequire returns the bound loader with real resolution and shared cache', () => {
  const {module} = loadModule();
  for (const filename of [__filename, url.pathToFileURL(__filename),
    url.pathToFileURL(__filename).href]) {
    const bound = module.createRequire(filename);
    assert.equal(bound('./bootstrap_test_support.cjs'), require('./bootstrap_test_support.cjs'));
    assert.equal(bound.resolve('./bootstrap_test_support.cjs'), require.resolve('./bootstrap_test_support.cjs'));
    assert.equal(bound.cache, require.cache);
    assert.equal(bound('node:path'), path);
    assert.equal(bound.resolve('node:path'), 'node:path');
    assert.throws(() => bound('./missing-module-fixture'), {code: 'MODULE_NOT_FOUND'});
  }
});

test('createRequire preserves filename versus trailing-directory semantics', () => {
  const {module, bindings} = loadModule();
  const fromDirectory = module.createRequire(__dirname + path.sep);
  assert.equal(fromDirectory.resolve('./bootstrap_test_support.cjs'),
               require.resolve('./bootstrap_test_support.cjs'));
  assert.equal(bindings[0], path.join(__dirname, 'noop.js'));
  module.createRequire(__dirname);
  assert.equal(bindings[1], __dirname);
});

test('createRequire converts Windows drive and UNC file URLs without losing the parent', () => {
  const {module, bindings} = loadModule(path.win32);
  module.createRequire(new URL('file:///C:/app%20dir/main.js'));
  module.createRequire('file://server/share/app/main.js');
  module.createRequire('file:///C:/app/');
  assert.deepEqual(bindings, ['C:\\app dir\\main.js',
    '\\\\server\\share\\app\\main.js', 'C:\\app\\noop.js']);
  assert.throws(() => module.createRequire('file:///C:/app%5cmain.js'),
                {code: 'ERR_INVALID_ARG_VALUE'});
});

test('createRequire honors POSIX file URLs and rejects nonlocal hosts', () => {
  const {module, bindings} = loadModule(path.posix);
  module.createRequire('file:///app%20dir/main.js');
  module.createRequire(new URL('file:///app/'));
  assert.deepEqual(bindings, ['/app dir/main.js', '/app/noop.js']);
  assert.throws(() => module.createRequire('file://server/app/main.js'),
                {code: 'ERR_INVALID_ARG_VALUE'});
});

for (const platform of ['win32', 'linux']) {
  test(`main ${platform}: createRequire preserves special characters in file URL parents`, () => {
    const bindings = [];
    const context = vm.createContext({__xenonPlatform: platform,
      __xenonArch: 'x64', Buffer, queueMicrotask,
      bindFilename: filename => { bindings.push(filename); return () => {}; }});
    vm.runInContext([
      readBootstrapPart('main/events.js'), readBootstrapPart('main/platform.js'),
      readBootstrapPart('common/promisify.js'),
      readBootstrapPart('main/builtins.js'), readBootstrapPart('common/module.js'),
      'globalThis.module = createModuleModule(pathModule, urlModule, bindFilename);',
      'globalThis.toFileURL = urlModule.pathToFileURL;',
    ].join('\n'), context);
    const parents = platform === 'win32' ?
        ['C:\\app#one\\main.js', 'C:\\app%20name\\main.js',
          '\\\\server\\share\\app#one\\main.js'] :
        ['/app#one/main.js', '/app%20name/main.js', '/app?one/back\\slash.js'];
    for (const parent of parents) {
      const fileURL = context.toFileURL(parent);
      assert.equal(fileURL.href,
                   url.pathToFileURL(parent, {windows: platform === 'win32'}).href);
      context.module.createRequire(fileURL);
      context.module.createRequire(fileURL.href);
      assert.equal(bindings.at(-1), parent);
      assert.equal(bindings.at(-2), parent);
    }
  });

  test(`main ${platform}: production URL conversion supplies native filenames to createRequire`, () => {
    const bindings = [];
    const context = vm.createContext({__xenonPlatform: platform,
      __xenonArch: 'x64', Buffer, queueMicrotask,
      bindFilename: filename => { bindings.push(filename); return () => {}; }});
    vm.runInContext([
      readBootstrapPart('main/events.js'), readBootstrapPart('main/platform.js'),
      readBootstrapPart('common/promisify.js'),
      readBootstrapPart('main/builtins.js'), readBootstrapPart('common/module.js'),
      'globalThis.module = createModuleModule(pathModule, urlModule, bindFilename);',
    ].join('\n'), context);
    const root = platform === 'win32' ? 'file:///C:/app%20dir/' : 'file:///app%20dir/';
    context.module.createRequire(root + 'main.js');
    vm.runInContext(`module.createRequire(new URL(${JSON.stringify(root)}));`, context);
    assert.deepEqual(bindings, platform === 'win32' ?
        ['C:\\app dir\\main.js', 'C:\\app dir\\noop.js'] :
        ['/app dir/main.js', '/app dir/noop.js']);
    assert.throws(() => context.module.createRequire(root + 'bad%2fname.js'),
                  {code: 'ERR_INVALID_ARG_VALUE'});
    assert.throws(() => context.module.createRequire('https://example.test/main.js'),
                  {code: 'ERR_INVALID_ARG_VALUE'});
    assert.throws(() => context.module.createRequire('file://user@server/share/main.js'),
                  {code: 'ERR_INVALID_ARG_VALUE'});
    if (platform === 'win32') {
      context.module.createRequire('file://server/share/main.js');
      assert.equal(bindings.at(-1), '\\\\server\\share\\main.js');
      context.module.createRequire('file://xn--bcher-kva/share/main.js');
      assert.equal(bindings.at(-1), '\\\\bücher\\share\\main.js');
      assert.throws(() => context.module.createRequire('file://xn--/share/main.js'),
                    {code: 'ERR_INVALID_ARG_VALUE'});
    } else {
      assert.throws(() => context.module.createRequire('file://server/share/main.js'),
                    {code: 'ERR_INVALID_ARG_VALUE'});
    }
  });
}
