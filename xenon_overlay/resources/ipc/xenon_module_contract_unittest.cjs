// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const nativeModule = require('node:module');
const path = require('node:path');
const url = require('node:url');
const test = require('node:test');
const vm = require('node:vm');

function loadModule(pathImplementation = path, urlImplementation = url) {
  const bindings = [];
  const context = vm.createContext({pathImplementation, urlImplementation,
    bindFilename(filename) {
      bindings.push(filename);
      return nativeModule.createRequire(filename);
    }});
  vm.runInContext(readBootstrapPart('common/module.js') +
      '\nglobalThis.module = createModuleModule(pathImplementation, urlImplementation, bindFilename);',
      context);
  return {module: context.module, bindings};
}

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
