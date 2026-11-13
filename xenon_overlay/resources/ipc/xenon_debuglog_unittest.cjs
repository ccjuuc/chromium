// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrap} = require('./bootstrap_test_support.cjs');

// The oracle runs in a fresh Node process because NODE_DEBUG is captured before
// user code, not at the first util.debuglog() call or the first enabled read.
function contract(debuglog, log, warnings) {
  const events = [];
  const capture = fn => {
    try { return {value: fn()}; }
    catch (error) { return {error: error.name, code: error.code}; }
  };
  const sections = ['fixture', 'FiXtUrE', 'fixture-extra', 'prefix-fixture',
    'ab.cd', 'a+b', 'x|y', 'a?b', ' a', '', 'HTTP', 'HTTP2'];
  const enabled = sections.map(section => debuglog(section).enabled);
  const categories = [];
  for (const section of ['fixture', 'FIXTURE', 'disabled-one', 'disabled-two']) {
    const logger = debuglog(section, optimized => {
      categories.push(optimized);
      events.push(['callback', section, optimized.enabled]);
    });
    events.push(['created', section]);
    logger('value %d', 17);
    logger('second');
  }
  const identities = categories.map(value =>
    categories.map(other => value === other));
  let callbacks = 0;
  const tested = debuglog('tested', () => ++callbacks);
  const descriptor = Object.getOwnPropertyDescriptor(tested, 'enabled');
  const testedEnabled = [tested.enabled, tested.enabled];
  const beforeLog = callbacks;
  tested('after-getter');
  tested('again');
  let coerced = 0;
  const lazy = debuglog({toString() { ++coerced; return 'fixture'; }},
      () => events.push(['lazy-callback']));
  const beforeCoercion = coerced;
  const lazyEnabled = [lazy.enabled, lazy.enabled];
  lazy('coerced');
  const coercionCount = coerced;
  const errors = [undefined, null, Symbol('category'), 42, {}, ['fixture']]
      .map(section => {
        let logger;
        return {create: capture(() => { logger = debuglog(section); return typeof logger; }),
          enabled: capture(() => logger.enabled), invoke: capture(() => logger('test'))};
      });
  const throws = debuglog('fixture', () => { throw new Error('callback'); });
  const callbackFailures = [capture(() => throws('first')),
    capture(() => throws('second'))];
  const ignoredCallback = capture(() => debuglog('fixture', 123)('non-function'));
  const beforeMutation = debuglog('after-env-change').enabled;
  process.env.NODE_DEBUG = 'after-env-change';
  const afterMutation = debuglog('after-env-change').enabled;
  // Passing format arguments through untouched is the logger's responsibility;
  // the factory receives Node's formatter so this test isolates that contract.
  const circular = {name: 'value'};
  circular.self = circular;
  debuglog('fixture')('%s %d %i %f %j %O', 'text', '12', '7x', '1.5x', circular, 9n);
  debuglog('http')('http-message');
  debuglog('http')('http-again');
  debuglog('http2')('http2-message');
  return {enabled, events, identities, callbacks, beforeLog, testedEnabled,
    descriptor: {enumerable: descriptor.enumerable, configurable: descriptor.configurable,
      setter: typeof descriptor.set}, beforeCoercion, lazyEnabled, coercionCount,
    errors, callbackFailures, ignoredCallback, beforeMutation, afterMutation,
    distinctWrappers: debuglog('fixture') !== debuglog('FIXTURE'),
    name: debuglog.name, length: debuglog.length, log, warnings};
}

function runContract(mode, nodeDebug) {
  const script = `
    const fs = require('node:fs');
    const util = require('node:util');
    const vm = require('node:vm');
    const log = [], warnings = [];
    const write = text => { log.push(text.replaceAll(String(process.pid), '<pid>')); return true; };
    process.stderr.write = write;
    process.emitWarning = message => warnings.push(message);
    const factory = vm.runInThisContext(fs.readFileSync(process.argv[1], 'utf8') + ';createNodeDebuglog');
    const logger = process.argv[2] === 'native' ? util.debuglog : factory({
      debugEnv: process.env.NODE_DEBUG, getPid: () => process.pid,
      format: util.format, write, emitWarning: message => warnings.push(message)});
    const contract = ${contract.toString()};
    process.stdout.write(JSON.stringify(contract(logger, log, warnings)));
  `;
  const result = spawnSync(process.execPath, ['-e', script,
    path.join(__dirname, 'common/util_debuglog.js'), mode], {
    env: {...process.env, NODE_DEBUG: nodeDebug, FORCE_COLOR: '0', NO_COLOR: '1'},
    encoding: 'utf8', timeout: 10000,
  });
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout);
}

for (const pattern of ['', 'fixture', 'FIX*', 'fixture,HTTP,http2', '*',
  'ab.cd,a+b,x|y,a?b', 'fixture tested', 'fixture,tested', '-fixture', ',fixture,']) {
  test(`debuglog matches Node startup pattern ${JSON.stringify(pattern)} and lazy/cache contracts`, () => {
    assert.deepEqual(runContract('host', pattern), runContract('native', pattern));
  });
}

function bootstrap(kind, nodeDebug = 'boot-fixture') {
  const log = [];
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error(text) { log.push(text); }},
    __xenonLog: text => log.push(text),
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE', pid: 37},
    location: {protocol: 'file:', pathname: '/C:/fixture/index.html'},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe', nodeDebug}),
      setDispatchHandler() {},
    },
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: 'fixture', __xenonAppPath: 'C:\\fixture',
    __xenonRendererBaseUrl: '', __xenonRendererUrlMappings: [],
    __xenonAppName: 'fixture', __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 37,
    __xenonEnv: {NODE_DEBUG: nodeDebug},
    __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  vm.runInContext(readBootstrap(path.join(__dirname,
      `xenon_ipc_${kind}_bootstrap.js`)), context);
  const hosted = kind === 'main' ? context.__xenonUtil : context.require('node:util');
  return {context, hosted, log};
}

for (const kind of ['main', 'renderer']) {
  test(`${kind}: debug aliases, startup snapshot and real host sink`, () => {
    const {context, hosted, log} = bootstrap(kind);
    assert.equal(hosted.debug, hosted.debuglog);
    assert.equal(hosted.debuglog('boot-fixture').enabled, true);
    hosted.debuglog('boot-fixture')('ready');
    assert.deepEqual(log.map(line => line.trimEnd()), ['BOOT-FIXTURE 37: ready']);
    context.process.env.NODE_DEBUG = 'different';
    assert.equal(hosted.debuglog('different').enabled, false);
    assert.equal(hosted.debuglog('boot-fixture').enabled, true);
    hosted.debuglog('disabled')({toString() { assert.fail('disabled payload was formatted'); }});
    assert.equal(log.length, 1);
    const redirected = [];
    context.process.stderr.write = text => redirected.push(text);
    hosted.debuglog('boot-fixture')('redirected');
    assert.deepEqual(redirected, ['BOOT-FIXTURE 37: redirected\n']);
  });

  test(`${kind}: stderr writes reach host and callbacks run after the write`, async () => {
    const {context, log} = bootstrap(kind, '');
    let called = false;
    assert.equal(context.process.stderr.isTTY, false);
    assert.equal(context.process.stderr.fd, 2);
    assert.equal(context.process.stderr.write('visible', () => { called = true; }), true);
    assert.equal(called, false);
    assert.deepEqual(log, ['visible']);
    await new Promise(resolve => queueMicrotask(resolve));
    assert.equal(called, true);
    assert.throws(() => context.process.stderr.write({}), {code: 'ERR_INVALID_ARG_TYPE'});
  });
}
