// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';
// Usage: node [--js-base-64] benchmark_fs_transport.cjs baseline.js [results.json]
// Measures JS adaptation and a modeled native byte boundary; no disk or Mojo.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {performance} = require('node:perf_hooks');
const {readBootstrap} = require('../resources/ipc/bootstrap_test_support.cjs');

const baselinePath = process.argv[2];
if (!baselinePath) throw new Error('Pass the unmodified renderer bootstrap baseline path');
const currentPath = path.resolve(__dirname, '../resources/ipc/xenon_ipc_renderer_bootstrap.js');

function runtime(sourcePath) {
  const context = vm.createContext(vm.constants.DONT_CONTEXTIFY);
  let counts, file;
  const reset = () => { counts = {calls: 0, payloadBytes: 0,
    jsBase64Encodes: 0, jsBase64Decodes: 0, bridgeBase64Encodes: 0, bridgeBase64Decodes: 0}; };
  const perform = (channel, request) => {
    assert.equal(channel, '__xenon:fs');
    ++counts.calls;
    if (request.operation === 'read_file') {
      if (request.returnBytes) {
        counts.payloadBytes += file.length;
        const result = new context.ArrayBuffer(file.length);
        new context.Uint8Array(result).set(file);
        return result;
      }
      ++counts.bridgeBase64Encodes;
      const result = file.toString('base64');
      counts.payloadBytes += Buffer.byteLength(result);
      return result;
    }
    assert.equal(request.operation, 'write_file');
    if (request.data) {
      counts.payloadBytes += request.data.byteLength;
      file = Buffer.from(request.data);
    } else {
      ++counts.bridgeBase64Decodes;
      counts.payloadBytes += Buffer.byteLength(request.dataBase64);
      file = Buffer.from(request.dataBase64, 'base64');
    }
  };
  Object.assign(context, {
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
    console: {log() {}, warn() {}, error() {}},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}),
      setDispatchHandler() {}, sendSync: perform,
      invoke: (channel, request) => Promise.resolve(perform(channel, request)),
    },
  });
  vm.runInContext(readBootstrap(sourcePath), context, {filename: sourcePath});
  const from = context.Buffer.from, toString = context.Buffer.prototype.toString;
  context.Buffer.from = function(value, encoding, ...rest) {
    if (encoding === 'base64') ++counts.jsBase64Decodes;
    return from.call(this, value, encoding, ...rest);
  };
  context.Buffer.prototype.toString = function(encoding, ...rest) {
    if (encoding === 'base64') ++counts.jsBase64Encodes;
    return toString.call(this, encoding, ...rest);
  };
  const module = context.require('fs');
  const filename = 'C:\\fixture\\benchmark.bin';
  reset();
  return {
    nativeBase64Available: vm.runInContext('typeof Uint8Array.prototype.toBase64 === "function"', context),
    setup(bytes) { file = Buffer.from(bytes); return context.Buffer.from(bytes); },
    async measure(operation, input, iterations) {
      reset();
      let result;
      const start = performance.now();
      for (let i = 0; i < iterations; ++i) {
        if (operation === 'readSync') result = module.readFileSync(filename);
        else if (operation === 'writeSync') module.writeFileSync(filename, input);
        else if (operation === 'readPromise') result = await module.promises.readFile(filename);
        else await module.promises.writeFile(filename, input);
      }
      const msPerOp = (performance.now() - start) / iterations;
      assert.deepEqual(Buffer.from(result || file), Buffer.from(input));
      return {msPerOp, perOperation: Object.fromEntries(
        Object.entries(counts).map(([key, value]) => [key, value / iterations]))};
    },
  };
}

(async () => {
  const runtimes = {before: runtime(path.resolve(baselinePath)), after: runtime(currentPath)};
  const result = {node: process.version, v8: process.versions.v8, flags: process.execArgv,
    nativeBase64Available: runtimes.after.nativeBase64Available,
    methodology: 'Production renderer FS entrypoints, in-memory native boundary with the existing BLOB/base64 request contract. Reports observed payload bytes excluding request metadata and actual encoding call counts. Seven alternating rounds after warmup; timing includes JS adaptation and modeled native conversion, excludes real Mojo, disk, Chromium and app latency.',
    results: []};
  const median = values => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
  for (const size of [65536, 1048576]) {
    const bytes = Buffer.alloc(size);
    for (let i = 0; i < size; ++i) bytes[i] = (i * 73 + 19) & 255;
    const inputs = Object.fromEntries(Object.entries(runtimes).map(([name, value]) => [name, value.setup(bytes)]));
    for (const operation of ['readSync', 'writeSync', 'readPromise', 'writePromise']) {
      const iterations = size === 65536 ? 20 : 5;
      for (const name of ['before', 'after']) await runtimes[name].measure(operation, inputs[name], iterations);
      const samples = {before: [], after: []}, counts = {};
      for (let round = 0; round < 7; ++round) {
        for (const name of round % 2 ? ['after', 'before'] : ['before', 'after']) {
          const measurement = await runtimes[name].measure(operation, inputs[name], iterations);
          samples[name].push(measurement.msPerOp);
          counts[name] = measurement.perOperation;
        }
      }
      result.results.push({size, operation, beforeMs: median(samples.before),
        afterMs: median(samples.after), counts, samples});
    }
  }
  const json = JSON.stringify(result, null, 2) + '\n';
  if (process.argv[3]) fs.writeFileSync(process.argv[3], json);
  process.stdout.write(json);
})().catch(error => { console.error(error); process.exitCode = 1; });
