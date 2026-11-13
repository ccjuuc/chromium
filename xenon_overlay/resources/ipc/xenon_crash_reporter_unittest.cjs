// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

function isolated({main = true, local, host} = {}) {
  const calls = [];
  const context = vm.createContext({
    options: {
      main, app: {getName: () => 'Fixture app', getVersion: () => '1.2.3'},
      callLocal: (operation, details) => {
        calls.push({where: 'local', operation, details: JSON.parse(JSON.stringify(details))});
        return local?.(operation, details);
      },
      callHost: (operation, details) => {
        calls.push({where: 'host', operation, details: JSON.parse(JSON.stringify(details))});
        return host?.(operation, details);
      },
    },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'common/crash_reporter.js'), 'utf8') +
      ';globalThis.api = createCrashReporter(options);', context);
  return {api: context.api, calls};
}

test('start accepts existing application options and only records local annotations', () => {
  const {api, calls} = isolated();
  // Same configuration shape as the unchanged MarkText release. Its URL and
  // false flag are intentionally not sent to a host configuration endpoint.
  assert.equal(api.start({
    companyName: 'MarkText', productName: 'MarkText',
    submitURL: 'http://0.0.0.0/', uploadToServer: false, compress: true,
  }), undefined);
  assert.deepEqual(calls, [{where: 'local', operation: 'start', details: {
    productName: 'MarkText', companyName: 'MarkText', appVersion: '1.2.3',
    extra: {}, globalExtra: {},
  }}]);
});

test('host upload policy remains real after either compatibility setter value', () => {
  let enabled = true;
  const {api, calls} = isolated({host: operation => {
    assert.equal(operation, 'crashReporter.getUploadToServer'); return enabled;
  }});
  assert.equal(api.setUploadToServer(false), undefined);
  assert.equal(api.getUploadToServer(), true);
  enabled = false;
  assert.equal(api.setUploadToServer(true), undefined);
  assert.equal(api.getUploadToServer(), false);
  assert.ok(calls.every(call => call.operation === 'crashReporter.getUploadToServer'));
  assert.throws(() => api.setUploadToServer('false'), {code: 'ERR_INVALID_ARG_TYPE'});
});

test('nonempty globalExtra and invalid options fail before any native mutation', () => {
  const {api, calls} = isolated();
  for (const options of [null, [], 'value'])
    assert.throws(() => api.start(options), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => api.start({globalExtra: {all: 'processes'}}), {code: 'ERR_NOT_SUPPORTED'});
  assert.throws(() => api.start({extra: {invalid: 42}}), {code: 'ERR_INVALID_ARG_TYPE'});
  for (const key of ['uploadToServer', 'compress', 'rateLimit', 'ignoreSystemCrashHandler'])
    assert.throws(() => api.start({[key]: 1}), {code: 'ERR_INVALID_ARG_TYPE'});
  for (const key of ['companyName', 'productName', 'submitURL'])
    assert.throws(() => api.start({[key]: 42}), {code: 'ERR_INVALID_ARG_TYPE'});
  assert.throws(() => api.start({extra: {invalid: 'a\0b'}}), {code: 'ERR_INVALID_ARG_VALUE'});
  assert.equal(calls.length, 0);
});

test('parameters use the local native binding and preserve names such as __proto__', () => {
  const parameters = JSON.parse('{"__proto__":"safe","test":"value"}');
  const {api, calls} = isolated({local: operation => {
    if (operation === 'getParameters') return {...parameters};
  }});
  api.start({extra: parameters});
  api.addExtraParameter('__proto__', 'new value');
  assert.deepEqual(api.getParameters(), parameters);
  api.removeExtraParameter('__proto__');
  assert.deepEqual(calls.map(call => call.operation),
      ['start', 'addExtraParameter', 'getParameters', 'removeExtraParameter']);
  assert.equal(Object.getPrototypeOf(calls[0].details.extra), Object.prototype);
  assert.equal(calls[0].details.extra.__proto__, 'safe');
});

test('uploaded reports preserve host IDs and dates; no report produces null', () => {
  let reports = [{id: 'server-report', date: 1700000000123}];
  const {api} = isolated({host: operation => {
    assert.equal(operation, 'crashReporter.getUploadedReports'); return reports;
  }});
  const [report] = api.getUploadedReports();
  assert.equal(report.id, 'server-report');
  assert.equal(Object.prototype.toString.call(report.date), '[object Date]');
  assert.equal(report.date.getTime(), 1700000000123);
  assert.equal(api.getLastCrashReport().id, 'server-report');
  reports = [];
  assert.equal(api.getLastCrashReport(), null);
});

test('renderer exposes process parameters only and propagates native failures', () => {
  const error = new Error('ERR_OUT_OF_RANGE: native annotation capacity reached');
  const {api} = isolated({main: false, local() { throw error; }});
  assert.deepEqual(Object.keys(api).sort(),
      ['addExtraParameter', 'getParameters', 'removeExtraParameter']);
  assert.throws(() => api.addExtraParameter('key', 'value'), candidate =>
    candidate === error && candidate.code === 'ERR_OUT_OF_RANGE');
  assert.throws(() => api.removeExtraParameter(42), {code: 'ERR_INVALID_ARG_TYPE'});
});

for (const kind of ['main', 'renderer']) {
  test(`${kind}: production bootstrap exports crashReporter and binds annotations locally`, () => {
    const calls = [];
    const local = request => {
      calls.push(JSON.parse(JSON.stringify(request)));
      if (request.operation === 'getParameters') return {native: kind};
    };
    const context = vm.createContext({
      TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
      setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
      console: {log() {}, warn() {}, error() {}},
      __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
      __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
      __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
      __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
      __xenonEnv: {}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
      __xenonCrashReporterCall: local,
      __xenonBrowserWindowCall() { throw new Error('Unexpected Browser call'); },
      __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
      location: {protocol: 'chrome:', hostname: 'fixture', search: ''},
      xenonIpcRenderer: {
        getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture'}),
        setDispatchHandler() {}, crashReporterCall: local,
        sendSync() { throw new Error('Unexpected Browser call'); },
      },
    });
    const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
    vm.runInContext(readBootstrap(filename), context, {filename});
    const {crashReporter} = kind === 'main' ? context.__xenonElectron : context.require('electron');
    crashReporter.addExtraParameter('key', 'value');
    assert.deepEqual(crashReporter.getParameters(), {native: kind});
    assert.deepEqual(calls, [
      {operation: 'addExtraParameter', key: 'key', value: 'value'},
      {operation: 'getParameters'},
    ]);
    if (kind === 'renderer') assert.equal(crashReporter.start, undefined);
  });
}
