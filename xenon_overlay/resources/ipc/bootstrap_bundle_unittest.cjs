// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const test = require('node:test');
const vm = require('node:vm');
const {buildBootstrap} = require('../../tools/bundle_ipc_bootstrap.cjs');

function fixture(t, files) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon bootstrap '));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  for (const [name, source] of Object.entries(files)) {
    const filename = path.join(directory, name);
    fs.mkdirSync(path.dirname(filename), {recursive: true});
    fs.writeFileSync(filename, source);
  }
  return {directory, entry: path.join(directory, 'entry.js')};
}

test('composition preserves hoisting, lexical identity and installation order', t => {
  const {entry} = fixture(t, {
    'entry.js': "(() => {\n  'use strict';\n" +
        '  // @include "parts/consumer.js"\n' +
        '  // @include "parts/provider.js"\n})();\n',
    'parts/consumer.js': '  const value = create();\r\n' +
        '  const saved = value;\r\n  value.order.push(1);\r\n',
    'parts/provider.js': '  function create() { return {order: []}; }\n' +
        '  value.order.push(2);\n  globalThis.result = {value, saved};\n',
  });
  const first = buildBootstrap(entry);
  assert.deepEqual(first, buildBootstrap(entry));
  assert.equal(first.inputs.length, 3);
  assert.equal(first.source.includes('@include'), false);
  assert.equal(first.source.includes('\r'), false);
  const context = vm.createContext({});
  vm.runInContext(first.source, context);
  assert.equal(context.result.value, context.result.saved);
  assert.deepEqual(Array.from(context.result.value.order), [1, 2]);
  assert.equal('value' in context, false);
});

test('only the entry declares includes, so adding a source has one registration point', t => {
  const {entry} = fixture(t, {
    'entry.js': '// @include "parts/outer.js"\n',
    'parts/outer.js': '// @include "common/value.js"\n',
    'common/value.js': 'globalThis.value = 1;\n',
  });
  assert.throws(() => buildBootstrap(entry), /includes belong in the entry only/);
});

for (const [label, files, error] of [
  ['cycle', {'entry.js': '// @include "entry.js"\n'}, /Cyclic/],
  ['duplicate', {'entry.js': '// @include "part.js"\n// @include "part.js"\n',
    'part.js': 'const a = 1;\n'}, /Duplicate/],
  ['traversal', {'entry.js': '// @include "../part.js"\n'}, /Invalid/],
  ['malformed directive', {'entry.js': '// @include part.js\n'}, /Malformed/],
  ['missing fragment', {'entry.js': '// @include "missing.js"\n'}, /ENOENT/],
  ['missing newline', {'entry.js': '// @include "part.js"\n',
    'part.js': 'const a = 1;'}, /newline/],
  ['invalid JavaScript', {'entry.js': 'const value = ;\n'}, /Unexpected token/],
]) {
  test(`invalid composition fails before packaging: ${label}`, t => {
    const {entry} = fixture(t, files);
    assert.throws(() => buildBootstrap(entry), error);
  });
}

test('production entry graphs cover every maintained fragment', () => {
  const inputPaths = new Set();
  for (const side of ['main', 'renderer']) {
    const result = buildBootstrap(path.join(__dirname,
        `xenon_ipc_${side}_bootstrap.js`));
    assert.equal(/^\s*\/\/\s*@include\b/m.test(result.source), false);
    result.inputs.forEach(filename => inputPaths.add(filename));
  }
  for (const group of ['main', 'renderer', 'common']) {
    for (const filename of fs.readdirSync(path.join(__dirname, group))) {
      if (filename.endsWith('.js')) {
        assert.ok(inputPaths.has(path.join(__dirname, group, filename)),
            `Unused bootstrap fragment: ${group}/${filename}`);
      }
    }
  }
});

test('CLI emits the same script and tracks dependencies in paths with spaces', t => {
  const {directory, entry} = fixture(t, {
    'entry.js': '// @include "part.js"\n',
    'part.js': 'globalThis.value = 1;\n',
  });
  const outputDirectory = path.join(directory, 'generated $ #');
  const output = path.join(outputDirectory, 'entry.js');
  const depfile = path.join(outputDirectory, 'bootstrap.d');
  const args = [path.resolve(__dirname, '../../tools/bundle_ipc_bootstrap.cjs'),
    '--entry', entry, '--output-dir', outputDirectory, '--depfile', depfile];
  const run = () => {
    const result = spawnSync(process.execPath, args,
        {cwd: directory, encoding: 'utf8', windowsHide: true});
    assert.equal(result.status, 0, result.stderr);
  };
  run();
  assert.equal(fs.readFileSync(output, 'utf8'), buildBootstrap(entry).source);
  const dependencies = fs.readFileSync(depfile, 'utf8');
  assert.ok(dependencies.startsWith('generated\\ $$\\ \\#/entry.js: '));
  assert.match(dependencies, / entry\.js part\.js\n$/);
  const oldTime = new Date('2000-01-01T00:00:00Z');
  fs.utimesSync(output, oldTime, oldTime);
  run();
  assert.equal(fs.statSync(output).mtimeMs, oldTime.getTime(),
      'Unchanged output must not trigger downstream resource rebuilds');
  fs.writeFileSync(path.join(directory, 'part.js'), 'globalThis.value = 2;\n');
  run();
  assert.equal(fs.readFileSync(output, 'utf8'), 'globalThis.value = 2;\n');
});
