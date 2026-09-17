// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Usage: node benchmark_renderer_bootstrap.cjs before.js [results.json]
// Count bridge calls and JS allocations using the full-bootstrap test fixtures.
// This is not a Chromium startup or native addon throughput benchmark.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {createRequire} = require('node:module');
const path = require('node:path');
const vm = require('node:vm');

const ipcDirectory = path.resolve(__dirname, '../resources/ipc');
function fixture(filename, boundary, exportedName) {
  const fixturePath = path.join(ipcDirectory, filename);
  const source = fs.readFileSync(fixturePath, 'utf8');
  const end = source.indexOf(boundary);
  assert.ok(end > 0, 'Test fixture boundary must exist');
  return new Function('require', '__dirname', source.slice(0, end) +
    '\nreturn ' + exportedName + ';')(createRequire(fixturePath), ipcDirectory);
}

const results = [];
const previousSource = process.env.XENON_TEST_BOOTSTRAP;
try {
  for (const [label, source] of [
    ['before', path.resolve(process.argv[2])],
    ['after', path.join(ipcDirectory, 'xenon_ipc_renderer_bootstrap.js')],
  ]) {
    process.env.XENON_TEST_BOOTSTRAP = source;
    const createPathRenderer = fixture('renderer_module_paths_unittest.cjs',
      "test('disk aliases", 'createPathRenderer');
    const files = new Map();
    const packages = 100;
    for (let i = 0; i < packages; ++i) {
      const directory = `C:\\test-app\\node_modules\\fixture-${i}`;
      files.set(directory + '\\package.json', JSON.stringify({main: 'entry.js'}));
      files.set(directory + '\\entry.js', `module.exports = {index: ${i}};`);
    }
    const modules = createPathRenderer(files);
    const loaded = Array.from({length: packages}, (_, i) => modules.context.require(`fixture-${i}`));
    const coldFilesystemIpc = modules.operations.length;
    const packageJsonReads = modules.reads.filter(name => name.endsWith('\\package.json')).length;
    const coldFileReads = modules.reads.length;
    for (let i = 0; i < 1000; ++i)
      assert.equal(modules.context.require(`fixture-${i % packages}`), loaded[i % packages]);

    const {renderer, countArgumentMaps} = fixture('renderer_native_binary_unittest.cjs',
      'for (const asynchronous', '{renderer, countArgumentMaps}');
    const native = renderer();
    countArgumentMaps(native);
    native.context.fixtureAddon = native.addon;
    const nativeCalls = 10000;
    const checksum = vm.runInContext(`(() => {
      let sum = 0;
      for (let i = 0; i < ${nativeCalls}; ++i)
        sum += fixtureAddon.echo(i, 'public fixture', true);
      return sum;
    })()`, native.context);
    assert.equal(checksum, nativeCalls * (nativeCalls - 1) / 2);
    assert.equal(native.calls.length, nativeCalls);
    results.push({label, packages, coldFilesystemIpc, packageJsonReads, coldFileReads,
      warmFilesystemIpc: modules.operations.length - coldFilesystemIpc,
      nativeCalls, primitiveArgumentMaps: native.context.argumentMapCount});
  }
} finally {
  if (previousSource === undefined) delete process.env.XENON_TEST_BOOTSTRAP;
  else process.env.XENON_TEST_BOOTSTRAP = previousSource;
}
if (process.argv[3]) fs.writeFileSync(process.argv[3], JSON.stringify(results, null, 2) + '\n');
for (const result of results) console.log(JSON.stringify(result));
