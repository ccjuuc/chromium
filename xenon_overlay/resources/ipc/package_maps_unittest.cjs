// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const {createRequire} = require('node:module');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {fileURLToPath, pathToFileURL} = require('node:url');
const vm = require('node:vm');

const context = vm.createContext({URL});
vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/package_maps.js'), 'utf8'), context);
const resolve = vm.runInContext('resolvePackageMap', context);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-package-maps-'));
test.after(() => fs.rmSync(root, {recursive: true, force: true}));
let fixtureId = 0;

function fixture(config, files = ['index.cjs', 'other.cjs', 'feature/a.cjs', 'specific/a.cjs', 'space file.cjs']) {
  const directory = path.join(root, String(++fixtureId));
  const packageDirectory = path.join(directory, 'node_modules', 'fixture');
  fs.mkdirSync(packageDirectory, {recursive: true});
  fs.writeFileSync(path.join(packageDirectory, 'package.json'), JSON.stringify({name: 'fixture', ...config}));
  for (const filename of files) {
    const destination = path.join(packageDirectory, filename);
    fs.mkdirSync(path.dirname(destination), {recursive: true});
    fs.writeFileSync(destination, 'module.exports = ' + JSON.stringify(filename));
  }
  return {directory, packageDirectory, require: createRequire(path.join(directory, 'main.cjs'))};
}

function outcome(operation) {
  try { return {value: operation()}; }
  catch (error) { return {code: error.code || error.name}; }
}

function checkExports(config, key = '.') {
  const app = fixture(config);
  const request = key === '.' ? 'fixture' : 'fixture/' + key.slice(2);
  const actual = outcome(() => {
    const target = resolve(config, key, ['node', 'require', 'node-addons']);
    const filename = fileURLToPath(new URL(target, pathToFileURL(path.join(app.packageDirectory, 'package.json'))));
    if (!fs.existsSync(filename)) throw Object.assign(new Error('missing target'), {code: 'MODULE_NOT_FOUND'});
    return filename;
  });
  assert.deepEqual(actual, outcome(() => app.require.resolve(request)), JSON.stringify({config, key}));
  return actual;
}

test('package exports match Node for strings, ordered conditions and nested fallthrough', () => {
  for (const exports of [
    './index.cjs',
    {require: './index.cjs', import: './other.cjs'},
    {default: './other.cjs', require: './index.cjs'},
    {node: {import: './other.cjs'}, require: './index.cjs'},
    {'node-addons': './index.cjs', default: './other.cjs'},
    {node: null, default: './index.cjs'},
    {browser: './index.cjs'},
    {'0': './other.cjs', default: './index.cjs'},
    {'.': './index.cjs', require: './other.cjs'},
  ]) checkExports({exports});
  assert.equal(resolve({exports: {import: './other.cjs', require: './index.cjs'}},
      '.', ['node', 'import']), './other.cjs');
});

test('package export arrays match Node without filesystem fallback or bypassing null targets', () => {
  for (const exports of [
    ['../invalid.cjs', './index.cjs'],
    [null, './index.cjs'],
    [{browser: './other.cjs'}, './index.cjs'],
    ['./absent.cjs', './index.cjs'],
    ['../invalid.cjs', null],
    [null, '../invalid.cjs'],
    [],
    {'.': null},
  ]) checkExports({exports});
});

test('package subpaths select exact and most specific wildcard exports with Node errors', () => {
  const config = {exports: {
    '.': './index.cjs',
    './features/*': './feature/*.cjs',
    './features/a': './other.cjs',
    './features/*.cjs': './specific/*.cjs',
    './private/*': null,
  }};
  for (const key of ['.', './features/a', './features/a.cjs', './private/a', './unlisted']) {
    checkExports(config, key);
  }
  checkExports({exports: {'./*': './feature/*.cjs', './feature/*': './specific/*.cjs'}}, './feature/a');
  checkExports({exports: {'.': './space%20file.cjs'}});
  checkExports({exports: {'.': './index.cjs?version=1#tag'}});
});

test('package targets and wildcard substitutions reject traversal with Node error codes', () => {
  for (const target of ['../index.cjs', '/index.cjs', 'file:///index.cjs',
    './feature/../index.cjs', './%2e%2e/index.cjs', './node_modules/index.cjs',
    './%6eode_modules/index.cjs', './feature%2fa.cjs', './feature%5ca.cjs']) {
    checkExports({exports: target});
  }
  for (const key of ['./../index', './%2e%2e/index', './node_modules/index', './a%2fb']) {
    checkExports({exports: {'./*': './feature/*.cjs'}}, key);
  }
});

test('imports maps match Node for private aliases, patterns and external packages', () => {
  const config = {imports: {
    '#local': {require: './index.cjs', import: './other.cjs'},
    '#feature/*': './feature/*.cjs',
    '#external': 'fixture',
    '#hidden': null,
    '#url': 'node:fs',
  }};
  const app = fixture({exports: './index.cjs'});
  fs.writeFileSync(path.join(app.directory, 'package.json'), JSON.stringify(config));
  fs.writeFileSync(path.join(app.directory, 'index.cjs'), 'module.exports = 1');
  fs.mkdirSync(path.join(app.directory, 'feature'));
  fs.writeFileSync(path.join(app.directory, 'feature', 'a.cjs'), 'module.exports = 2');
  for (const key of ['#local', '#feature/a', '#external', '#hidden', '#url', '#missing', '#', '#local/']) {
    const actual = outcome(() => {
      const target = resolve(config, key, ['node', 'require'], true);
      return target.startsWith('./') ? fileURLToPath(new URL(target,
          pathToFileURL(path.join(app.directory, 'package.json')))) : app.require.resolve(target);
    });
    assert.deepEqual(actual, outcome(() => app.require.resolve(key)), key);
  }
});

test('published zod and electron-log maps select require and default targets accurately', () => {
  // Relevant entries from zod 4.6.7 and electron-log 5.4.4 in the unchanged
  // Motrix 2.4.1 / MarkText 0.23.0 release fixtures. Custom source/type/browser
  // conditions must not shadow the main runtime's require/default selection.
  const zod = {exports: {'.': {'@zod/source': './src/index.ts', types: './index.d.cts',
    import: './index.js', require: './index.cjs'},
    './v4': {'@zod/source': './src/v4/index.ts', types: './v4/index.d.cts',
      import: './v4/index.js', require: './v4/index.cjs'}}};
  const electronLog = {exports: {'.': {types: './src/index.d.ts',
    browser: './src/renderer/index.js', default: './src/index.js'}, './*': './*',
    './main': {types: './main.d.ts', default: './main.js'},
    './renderer': {types: './renderer.d.ts', default: './renderer.js'}, './preload': './preload.js'}};
  assert.equal(resolve(zod, '.', ['node', 'require']), './index.cjs');
  assert.equal(resolve(zod, './v4', ['node', 'import']), './v4/index.js');
  assert.equal(resolve(electronLog, '.', ['node', 'require']), './src/index.js');
  assert.equal(resolve(electronLog, './main', ['node', 'require']), './main.js');
  assert.equal(resolve(electronLog, './src/main/index.js', ['node', 'require']), './src/main/index.js');
});
