// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {readBootstrap} = require('./bootstrap_test_support.cjs');
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const bootstrapPath = path.join(__dirname, 'xenon_ipc_renderer_bootstrap.js');
const bootstrapSource = readBootstrap(bootstrapPath);
const addonPath = 'C:\\test-app\\fixture.node';

function renderer(overrides) {
  const calls = {sync: 0, instance: 0, async: 0};
  const transport = {
    getRuntimeConfig: () => ({appPath: 'C:\\test-app',
      exeDir: 'C:\\test-app', execPath: 'C:\\test-app\\host.exe'}),
    setDispatchHandler() {},
    sendSync(channel, request) {
      assert.equal(channel, '__xenon:fs');
      if (request.operation === 'realpath') return request.path;
      if (request.operation === 'exists') return request.path === addonPath;
      assert.equal(request.operation, 'stat');
      if (request.path !== addonPath) {
        throw new Error('ENOENT: ' + request.path);
      }
      return {isFile: true, isDirectory: false, size: 1, mtimeMs: 0};
    },
    requireNodeModuleSync: () => [
      {name: 'work', kind: 'function'},
      {name: 'Handle', kind: 'class', prototype: [{name: 'work', kind: 'function'}]},
    ],
    constructNodeExportSync: () => 7,
    invokeNodeExportSync(...args) {
      ++calls.sync;
      return overrides.export(...args);
    },
    invokeNodeInstanceSync(...args) {
      ++calls.instance;
      return overrides.instance(...args);
    },
    invoke(...args) {
      ++calls.async;
      if (overrides.async) return overrides.async(...args);
      throw new Error('A native operation must not be invoked a second time');
    },
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    xenonIpcRenderer: transport,
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, atob, btoa,
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    console: {log() {}, warn() {}, error() {}},
  });
  vm.runInContext(bootstrapSource, context, {filename: bootstrapPath});
  return {addon: context.require(addonPath), calls};
}

test('pending export continues once and later synchronous returns stay synchronous', async () => {
  let finish;
  let next = new Promise(resolve => { finish = resolve; });
  const {addon, calls} = renderer({export: () => next});
  const result = addon.work();
  assert.equal(typeof result.then, 'function');
  assert.deepEqual(calls, {sync: 1, instance: 0, async: 0});
  finish({__xenon_node_wire_type__: 'bigint', value: '42'});
  assert.equal(await result, 42n);
  next = 19;
  assert.equal(addon.work(), 19);
  assert.deepEqual(calls, {sync: 2, instance: 0, async: 0});
});

test('concurrent export operations settle independently without retries', async () => {
  const finish = [];
  const {addon, calls} = renderer({export: () =>
    new Promise(resolve => { finish.push(resolve); })});
  const first = addon.work();
  const second = addon.work();
  finish[1](2);
  assert.equal(await second, 2);
  finish[0](1);
  assert.equal(await first, 1);
  assert.deepEqual(calls, {sync: 2, instance: 0, async: 0});
});

test('pending instance method adopts native handles from the original result', async () => {
  let finish;
  const {addon, calls} = renderer({instance: () =>
    new Promise(resolve => { finish = resolve; })});
  const handle = new addon.Handle();
  const result = handle.work();
  finish({__xenon_node_wire_type__: 'native_instance', module_path: addonPath,
    instance_id: 23, prototype: []});
  assert.equal((await result).__instanceId, 23);
  assert.deepEqual(calls, {sync: 0, instance: 1, async: 0});
});

test('pending rejection propagates without retrying an operation', async () => {
  const error = new Error('Native Promise connection was closed');
  const {addon, calls} = renderer({export: () => Promise.reject(error),
    instance: () => Promise.reject(error)});
  await assert.rejects(addon.work(), value => value === error);
  await assert.rejects(new addon.Handle().work(), value => value === error);
  assert.deepEqual(calls, {sync: 1, instance: 1, async: 0});
});

test('error text does not authorize re-executing native code', () => {
  const error = new Error('Native export returned a pending Promise');
  const {addon, calls} = renderer({export() { throw error; }});
  assert.throws(() => addon.work(), value => value === error);
  assert.deepEqual(calls, {sync: 1, instance: 0, async: 0});
});

test('callback-bearing calls serialize observer ids and preserve synchronous returns', () => {
  const {addon, calls} = renderer({export(_path, _name, callback) {
    assert.equal(callback.__xenon_node_wire_type__, 'callback');
    assert.equal(typeof callback.callback_id, 'number');
    return 42;
  }});
  assert.equal(addon.work(() => {}), 42);
  assert.deepEqual(calls, {sync: 1, instance: 0, async: 0});
});
