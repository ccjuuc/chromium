// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const test = require('node:test');
const vm = require('node:vm');
const lexer = require('./third_party/cjs_module_lexer/lexer.js');
const {buildBootstrap} = require('../../tools/bundle_ipc_bootstrap.cjs');

test('CommonJS named export parsing does not execute application code', () => {
  const result = lexer.parse(`
    throw new Error('Never execute during linking');
    exports.value = 1;
    module.exports.other = 2;
    Object.defineProperty(exports, 'getter', {get() { return sideEffect(); }});
    module.exports = require('./child.cjs');
    const text = "exports.notReal = 1";
    // exports.comment = 1;
  `);
  assert.deepEqual(result.exports.sort(), ['other', 'value']);
  assert.deepEqual(result.reexports, ['./child.cjs']);
});

test('bootstrap includes the unmodified lexer in a private CommonJS scope', () => {
  const source = buildBootstrap(path.join(__dirname, 'xenon_ipc_main_bootstrap.js')).source;
  const end = source.indexOf('globalThis.__xenonParseCommonJsExports = commonJsExportLexer.parse;');
  assert.notEqual(end, -1);
  const prefix = source.slice(0, end) +
      'globalThis.__xenonParseCommonJsExports = commonJsExportLexer.parse;})();';
  const context = vm.createContext({});
  vm.runInContext(prefix, context);
  assert.deepEqual(Array.from(context.__xenonParseCommonJsExports('exports.answer=42').exports), ['answer']);
  assert.equal(context.module, undefined);
  assert.equal(context.source, undefined);
});

test('statically detected namespaces match real Node for standard CommonJS forms', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-cjs-names-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const source = `
    exports.alpha = 1;
    module.exports.beta = 2;
    Object.defineProperty(exports, 'gamma', {value: 3});
    module.exports['delta'] = 4;
    const computed = 'runtimeOnly';
    exports[computed] = 5;
  `;
  fs.writeFileSync(path.join(root, 'fixture.cjs'), source);
  const result = spawnSync(process.execPath,
      ['--input-type=module', '-e', "import * as ns from './fixture.cjs';console.log(JSON.stringify(Object.keys(ns)))"],
      {cwd: root, encoding: 'utf8', windowsHide: true});
  assert.equal(result.status, 0, result.stderr);
  const nodeNames = JSON.parse(result.stdout).filter(name =>
    name !== 'default' && name !== 'module.exports').sort();
  assert.deepEqual(lexer.parse(source).exports.sort(), nodeNames);
});
