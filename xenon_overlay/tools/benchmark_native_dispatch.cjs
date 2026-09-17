// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Usage: node benchmark_native_dispatch.cjs before.js [results.json]
// Measures JS dispatch through complete renderer bootstraps with a mocked
// synchronous transport. These timings exclude Mojo, native code and startup.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {createRequire} = require('node:module');
const path = require('node:path');
const {performance} = require('node:perf_hooks');
const vm = require('node:vm');

const ipcDirectory = path.resolve(__dirname, '../resources/ipc');
const fixturePath = path.join(ipcDirectory, 'renderer_native_binary_unittest.cjs');
const fixtureSource = fs.readFileSync(fixturePath, 'utf8');
const boundary = fixtureSource.indexOf('for (const asynchronous');
assert.ok(boundary > 0, 'Native binary fixture boundary must exist');
const createFixture = new Function('require', '__dirname',
    fixtureSource.slice(0, boundary) + '\nreturn renderer;');
const iterations = 300000;
const samples = 10;
const scenarios = [
  ['oneNumber', 'addon.echo(i)'],
  ['threePrimitives', 'addon.echo(i, "public fixture", true)'],
  ['sixteenNumbers', 'addon.echo(i, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)'],
];

function createContext(source) {
  const previous = process.env.XENON_TEST_BOOTSTRAP;
  try {
    process.env.XENON_TEST_BOOTSTRAP = source;
    const renderer = createFixture(createRequire(fixturePath), ipcDirectory);
    let dispatches = 0;
    const {context, addon} = renderer({
      invokeNodeExportSync(_modulePath, _functionName, value) {
        ++dispatches;
        return value;
      },
    });
    context.addon = addon;
    return {context, dispatches: () => dispatches};
  } finally {
    if (previous === undefined) delete process.env.XENON_TEST_BOOTSTRAP;
    else process.env.XENON_TEST_BOOTSTRAP = previous;
  }
}

assert.ok(process.argv[2], 'Provide the previous full bootstrap or entry path');
const sources = [path.resolve(process.argv[2]),
  path.join(ipcDirectory, 'xenon_ipc_renderer_bootstrap.js')];
const results = [];
for (const [scenario, expression] of scenarios) {
  const fixtures = sources.map(createContext);
  const durations = [[], []];
  const script = new vm.Script(`(() => {
    let checksum = 0;
    for (let i = 0; i < ${iterations}; ++i) checksum += ${expression};
    return checksum;
  })()`);
  const checksum = iterations * (iterations - 1) / 2;
  for (const fixture of fixtures)
    assert.equal(script.runInContext(fixture.context), checksum);
  for (let sample = 0; sample < samples; ++sample) {
    // Alternate order to reduce warmup, thermal and background-work bias.
    for (const index of sample % 2 ? [1, 0] : [0, 1]) {
      const start = performance.now();
      const result = script.runInContext(fixtures[index].context);
      durations[index].push(performance.now() - start);
      assert.equal(result, checksum);
    }
  }
  for (let index = 0; index < fixtures.length; ++index) {
    assert.equal(fixtures[index].dispatches(), iterations * (samples + 1));
    const sorted = [...durations[index]].sort((a, b) => a - b);
    results.push({scenario, label: index ? 'after' : 'before', iterations,
      dispatchesPerSample: iterations, samples,
      medianMs: (sorted[samples / 2 - 1] + sorted[samples / 2]) / 2,
      samplesMs: durations[index]});
  }
}
const report = {node: process.version, platform: process.platform, arch: process.arch,
  scope: 'JavaScript dispatch only; mocked synchronous native transport', results};
if (process.argv[3]) fs.writeFileSync(process.argv[3], JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
