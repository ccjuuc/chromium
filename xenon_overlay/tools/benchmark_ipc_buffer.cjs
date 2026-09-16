// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Usage: node --js-base-64 xenon_overlay/tools/benchmark_ipc_buffer.cjs baseline.js [results.json]
// JS-only benchmark: no Chromium, Mojo, disk/network workload or native addons.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const os = require('node:os');
const assert = require('node:assert/strict');
const {performance} = require('node:perf_hooks');

const currentPath = path.resolve(__dirname, '../resources/ipc/xenon_ipc_renderer_bootstrap.js');
const baselinePath = process.argv[2];
if (!baselinePath) throw new Error('Pass the unmodified renderer bootstrap as the baseline path');

function createContext(sourcePath) {
  const context = vm.createContext(vm.constants.DONT_CONTEXTIFY);
  Object.assign(context, {TextEncoder, TextDecoder, URL, URLSearchParams,
    queueMicrotask, atob, btoa, performance,
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
    xenonIpcRenderer: {getRuntimeConfig: () => ({appPath: 'C:\\fixture',
      exeDir: 'C:\\fixture', execPath: 'C:\\fixture\\host.exe'}), setDispatchHandler() {}},
  });
  vm.runInContext(fs.readFileSync(sourcePath, 'utf8'), context, {filename: sourcePath});
  vm.runInContext(`
    globalThis.setup = size => {
      globalThis.bytes = Buffer.alloc(size);
      for (let i = 0; i < size; ++i) bytes[i] = (i * 73 + 19) & 255;
      globalThis.encoded = bytes.toString('base64');
    };
    globalThis.measure = (operation, iterations) => {
      let checksum = 0;
      const start = performance.now();
      for (let i = 0; i < iterations; ++i) {
        const value = operation === 'encode' ? bytes.toString('base64') : Buffer.from(encoded, 'base64');
        checksum += value.length;
        if (value.length) checksum += operation === 'encode' ? value.charCodeAt(i % value.length) : value[i % value.length];
      }
      return {msPerOp: (performance.now() - start) / iterations, checksum};
    };`, context);
  return context;
}

const contexts = {before: createContext(baselinePath), after: createContext(currentPath)};
const result = {date: new Date().toISOString(), node: process.version,
  v8: process.versions.v8, flags: process.execArgv, cpu: os.cpus()[0].model,
  nativeBase64Available: vm.runInContext('typeof Uint8Array.prototype.toBase64 === "function"', contexts.after),
  methodology: 'Unmodified baseline and current production renderer bootstrap, same Node process, DONT_CONTEXTIFY globals. Seven alternating rounds after warmup, per-round means and their median. Native transport stubbed, no IPC or I/O measured. Node experimental Base64 feature, when enabled, may differ from shipping Chromium.',
  baselinePath: path.resolve(baselinePath), currentPath, results: []};
const median = values => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
for (const size of [65536, 1048576]) {
  for (const context of Object.values(contexts)) {
    context.setup(size);
    assert.equal(context.encoded, Buffer.from(context.bytes).toString('base64'));
    assert.deepEqual(Buffer.from(vm.runInContext("Buffer.from(encoded, 'base64')", context)),
                     Buffer.from(context.bytes));
  }
  for (const operation of ['encode', 'decode']) {
    const iterations = size === 65536 ? 30 : 5;
    for (const context of Object.values(contexts)) context.measure(operation, iterations);
    const samples = {before: [], after: []};
    for (let round = 0; round < 7; ++round) {
      for (const name of round % 2 ? ['after', 'before'] : ['before', 'after']) {
        const measurement = contexts[name].measure(operation, iterations);
        assert.ok(measurement.checksum > 0);
        samples[name].push(measurement.msPerOp);
      }
    }
    const entry = {size, operation, beforeMs: median(samples.before),
      afterMs: median(samples.after), samples};
    entry.speedup = entry.beforeMs / entry.afterMs;
    result.results.push(entry);
  }
}
const json = JSON.stringify(result, null, 2);
if (process.argv[3]) fs.writeFileSync(process.argv[3], json + '\n');
console.log(json);
