// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const util = require('node:util');
const {Console: NativeConsole} = require('node:console');
const {Writable} = require('node:stream');
const {EventEmitter} = require('node:events');
const test = require('node:test');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const factory = vm.runInThisContext(fs.readFileSync(path.join(__dirname,
    'common/console.js'), 'utf8') + ';createNodeConsole');
function captureStream(output) {
  return new Writable({write(chunk, encoding, done) { output.push(chunk.toString()); done(); }});
}
function host(options = {}) {
  const out = [], err = [], warnings = [];
  let milliseconds = 0;
  const stdout = captureStream(out), stderr = captureStream(err);
  const module = factory({util, stdout, stderr, now: () => milliseconds,
    emitWarning: message => warnings.push(message), ...options});
  return {module, Console: module.Console, out, err, warnings, stdout, stderr,
    setTime: n => { milliseconds = n; }};
}
function exercise(Console, {stdout, stderr}) {
  const c = new Console({stdout, stderr, colorMode: false, groupIndentation: 3});
  c.log();
  c.log('literal 100%');
  c.log('%s %d %i %f %j %%', 'word', '3', '7x', '1.5x', {n: 2});
  const circular = {name: 'circle'}; circular.self = circular;
  c.info('multiple', {a: 1}, circular, 2n, Symbol('value'));
  c.debug('%o', [1, 2]);
  c.warn('warning', 7);
  c.error('error %s', 'text');
  c.assert(true, 'invisible');
  c.assert(false);
  c.assert(false, 'number %d', 5);
  c.assert(false, {reason: 'fixture'});
  c.dir({nested: {value: 1}}, {depth: 0});
  c.group('group', 1);
  c.log('line one\nline two');
  c.groupCollapsed();
  c.dirxml('xml', {value: 2});
  c.groupEnd(); c.groupEnd(); c.groupEnd();
  c.log('unindented');
  c.count(); c.count(); c.count('x'); c.countReset(); c.count();
  const {log} = c; log('bound', 9);
  c.clear();
  return {keys: Object.keys(c), instanceof: c instanceof Console,
    returnValue: c.log('return'), name: log.name, length: Console.length};
}
test('Console writes, formatting, assertions, grouping, bound methods and counts match Node', () => {
  const a = host(), b = host();
  assert.deepEqual(exercise(a.Console, a), exercise(NativeConsole, b));
  assert.deepEqual(a.out, b.out);
  assert.deepEqual(a.err, b.err);
});

test('constructor validation and callable Console match Node', () => {
  const a = host();
  const cases = [undefined, null, 1, {}, {stdout: a.stdout, stderr: null},
    {stdout: a.stdout, colorMode: 'invalid'}, {stdout: a.stdout, groupIndentation: -1},
    {stdout: a.stdout, groupIndentation: 1.5}, {stdout: a.stdout, groupIndentation: '2'},
    {stdout: a.stdout, groupIndentation: 1001}, {stdout: a.stdout, inspectOptions: null},
    {stdout: a.stdout, inspectOptions: []},
    {stdout: a.stdout, colorMode: false, inspectOptions: {colors: true}}];
  const outcomes = Console => cases.map(options => {
    try { new Console(options); return null; }
    catch (error) { return {name: error.name, code: error.code}; }
  });
  assert.deepEqual(outcomes(a.Console), outcomes(NativeConsole));
  assert.equal(a.Console(a.stdout) instanceof a.Console, true);
});

test('Console subclass overrides are bound per instance, and stream options reach util', () => {
  for (const Base of [host().Console, NativeConsole]) {
    class Sub extends Base { log(value) { this.seen = value; } }
    const out = [], stdout = captureStream(out);
    const instance = new Sub(stdout);
    const {log} = instance; log('subclass');
    assert.equal(instance.seen, 'subclass');
  }
  const a = host(), b = host();
  for (const [Console, capture] of [[a.Console, a], [NativeConsole, b]]) {
    const inspectOptions = new Map([[capture.stdout, {depth: 0}], [capture.stderr, {depth: 2}]]);
    const c = new Console({stdout: capture.stdout, stderr: capture.stderr, inspectOptions});
    c.log({a: {b: 1}}); c.warn({a: {b: 1}});
  }
  assert.deepEqual(a.out, b.out); assert.deepEqual(a.err, b.err);
});

test('timer output and missing/duplicate warnings match Node without wall-clock sleeps', () => {
  function run(mode) {
    const a = host();
    const c = mode === 'host' ? a.module : new NativeConsole(a.stdout, a.stderr);
    let time = 0;
    const originalHrtime = process.hrtime, originalWarning = process.emitWarning;
    process.hrtime = previous => {
      let ns = Math.round(time * 1e6) - (previous ? previous[0] * 1e9 + previous[1] : 0);
      return [Math.floor(ns / 1e9), ns % 1e9];
    };
    process.emitWarning = message => a.warnings.push(message);
    const set = value => { time = value; a.setTime(value); };
    try {
      c.time(); c.time();
      set(1.23456); c.timeLog(undefined, 'extra', 7);
      set(1234.567); c.timeEnd(); c.timeEnd();
      c.time('long'); set(61234.567); c.timeLog('long');
      set(3661234.567); c.timeEnd('long');
      c.countReset('missing'); c.count(1); c.countReset(1); c.countReset('1');
      return {out:a.out, warnings:a.warnings};
    } finally { process.hrtime = originalHrtime; process.emitWarning = originalWarning; }
  }
  assert.deepEqual(run('host'), run('native'));
});

test('table common array/object/Map/Set output and property filters match Node', () => {
  const a = host(), b = host();
  const values = [[{a: 1, b: 'x'}, {a: 2}], [1, 2], {x: 3, y: {a: 2}},
    new Map([['a', 1]]), new Set(['a', 2]), [], null, 'text'];
  for (const value of values) { a.module.table(value); new NativeConsole(b.stdout).table(value); }
  a.module.table([{a:1,b:2},{b:3}], ['b']);
  new NativeConsole(b.stdout).table([{a:1,b:2},{b:3}], ['b']);
  assert.deepEqual(a.out, b.out);
  assert.throws(() => a.module.table({}, 'bad'), {code:'ERR_INVALID_ARG_TYPE'});
});

test('ignoreErrors handles synchronous and asynchronous Writable failures like Node', async () => {
  for (const Console of [host().Console, NativeConsole]) {
    const error = new Error('write failed');
    const stream = new EventEmitter();
    stream.write = () => { throw error; };
    new Console({stdout:stream}).log('ignored');
    assert.throws(() => new Console({stdout:stream,ignoreErrors:false}).log('throws'), e => e === error);
    assert.equal(stream.listenerCount('error'), 0);
    const asyncStream = new Writable({write(chunk, encoding, callback) {
      queueMicrotask(() => callback(error));
    }});
    new Console(asyncStream).error('async failure');
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(asyncStream.listenerCount('error'), 0);
  }
});

test('trace has a real stack; clear writes terminal escapes only for an actual TTY', () => {
  const a = host();
  a.module.trace('marker %s', 'value');
  assert.match(a.err[0], /^Trace: marker value\n\s+at /);
  assert.doesNotMatch(a.err[0], /at Console\.trace/);
  a.module.clear(); assert.deepEqual(a.out, []);
  a.stdout.isTTY = true;
  a.module.clear(); assert.deepEqual(a.out, ['\x1b[1;1H\x1b[0J']);
});

function bootstrap(kind) {
  const logs = [];
  const nativeConsole = {log: text => logs.push(['out',text]),
    warn() {}, error: text => logs.push(['err',text])};
  const context = vm.createContext({
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask, performance,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: nativeConsole,
    __xenonLog: text => logs.push(['host',text]),
    __xenonPaths: {platform:'win32',arch:'x64',endianness:'LE',pid:37},
    location:{protocol:'file:',pathname:'/C:/fixture/index.html'},
    xenonIpcRenderer:{getRuntimeConfig:()=>({appPath:'C:\\fixture',exeDir:'C:\\fixture',execPath:'C:\\fixture\\host.exe'}),setDispatchHandler(){}},
    __xenonPlatform:'win32',__xenonArch:'x64',__xenonEndianness:'LE',__xenonOsRelease:'fixture',
    __xenonAppPath:'C:\\fixture',__xenonRendererBaseUrl:'',__xenonRendererUrlMappings:[],
    __xenonAppName:'fixture',__xenonAppVersion:'1',__xenonUserAgent:'',
    __xenonExecPath:'C:\\fixture\\host.exe',__xenonPid:37,__xenonEnv:{},
    __xenonChromeVersion:'153',__xenonV8Version:'',__xenonGetPath:()=>'',
  });
  vm.runInContext(readBootstrap(path.join(__dirname,`xenon_ipc_${kind}_bootstrap.js`)),context);
  const module = kind === 'main' ? context.__xenonConsole : context.require('node:console');
  return {context,module,logs,nativeConsole};
}
for (const kind of ['main','renderer']) {
  test(`${kind}: builtin/global wiring writes to the host, preserves redirection and emits warnings`, async () => {
    const {context,module,logs,nativeConsole} = bootstrap(kind);
    if (kind === 'main') assert.equal(context.console,module);
    else {
      assert.equal(context.console,nativeConsole);
      assert.equal(context.require('console'),module);
      assert.equal(context.require('module').isBuiltin('node:console'),true);
    }
    module.log('visible'); module.error('problem');
    assert.deepEqual(logs.map(row=>row[1].trimEnd()),['visible','problem']);
    const out=[],err=[];
    context.process.stdout.write = text=>out.push(text);
    context.process.stderr.write = text=>err.push(text);
    module.log(); module.count('item'); module.error('redirected');
    assert.deepEqual(out,['\n','item: 1\n']); assert.deepEqual(err,['redirected\n']);
    const warnings=[]; context.process.on('warning',warning=>warnings.push(warning.message));
    module.countReset('missing');
    await new Promise(resolve=>queueMicrotask(resolve));
    assert.deepEqual(warnings,["Count for 'missing' does not exist"]);
    assert.match(err[1],/Count for 'missing' does not exist/);
  });
}
