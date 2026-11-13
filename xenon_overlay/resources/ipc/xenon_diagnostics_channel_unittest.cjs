// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const {execFileSync} = require('node:child_process');
const {AsyncLocalStorage} = require('node:async_hooks');
const nodeDiagnostics = require('node:diagnostics_channel');
const {readBootstrap} = require('./bootstrap_test_support.cjs');
const source = fs.readFileSync(path.join(__dirname, 'main/diagnostics_channel.js'), 'utf8');
let nextName = 0;
const plain = value => JSON.parse(JSON.stringify(value));
function fixture() {
  const errors = [];
  const context = vm.createContext({});
  vm.runInContext(source, context);
  return {api: context.createDiagnosticsChannel(error => errors.push(error)), errors};
}
async function compare(run) {
  const name = 'xenon-diagnostics-unit-' + process.pid + '-' + ++nextName;
  const {api} = fixture();
  const expected = await run(nodeDiagnostics, name);
  const actual = await run(api, name);
  assert.deepEqual(plain(actual), plain(expected));
}

test('named and symbol channels preserve identity, constructor registration and subscribers', async () => {
  await compare((dc, name) => {
    const channel = dc.channel(name), symbol = Symbol('diagnostic'), events = [];
    const listener = (value, channelName) => events.push([value, channelName]);
    const before = dc.hasSubscribers(name);
    dc.subscribe(name, listener); channel.publish({value: 42});
    const active = channel.hasSubscribers;
    const first = dc.unsubscribe(name, listener), second = dc.unsubscribe(name, listener);
    const same = dc.channel(name) === channel;
    const sameSymbol = dc.channel(symbol) === dc.channel(symbol);
    const replacement = new dc.Channel(name);
    return {before, active, first, second, same, sameSymbol,
      registered: dc.channel(name) === replacement, instance: replacement instanceof dc.Channel,
      after: dc.hasSubscribers(name), unknown: dc.hasSubscribers({}), events};
  });
});

test('publishing snapshots duplicate subscriptions and changes take effect on the next publication', async () => {
  await compare((dc, name) => {
    const channel = dc.channel(name), events = [];
    const later = value => events.push('later:' + value);
    const second = value => events.push('second:' + value);
    const first = value => { events.push('first:' + value); channel.unsubscribe(second); channel.subscribe(later); };
    channel.subscribe(first); channel.subscribe(second); channel.subscribe(second);
    channel.publish(1); channel.publish(2);
    const unsubscribed = [channel.unsubscribe(second), channel.unsubscribe(second)];
    channel.unsubscribe(first); while(channel.unsubscribe(later)) {}
    return {events, unsubscribed, active: channel.hasSubscribers};
  });
});

test('bindStore and runStores nest real AsyncLocalStorage contexts and restore the caller', async () => {
  await compare(async (dc, name) => {
    const channel = dc.channel(name), a = new AsyncLocalStorage(), b = new AsyncLocalStorage(), events = [];
    const subscriber = data => events.push(['publish', data.id, a.getStore(), b.getStore()]);
    channel.subscribe(subscriber);
    channel.bindStore(a, data => { events.push(['transform-a', b.getStore()]); return data.id; });
    channel.bindStore(b, data => { events.push(['transform-b', a.getStore()]); return 'b:' + data.id; });
    const receiver = {base: 3};
    const result = await channel.runStores({id: 7}, async function(value) {
      events.push(['inside', this === receiver, a.getStore(), b.getStore()]);
      await Promise.resolve(); events.push(['after-await', a.getStore(), b.getStore()]);
      return this.base + value;
    }, receiver, 4);
    events.push(['outside', a.getStore(), b.getStore()]);
    channel.bindStore(a, data => data.id * 2);
    const replacement = channel.runStores({id: 5}, () => a.getStore());
    const unbound = [channel.unbindStore(a), channel.unbindStore(a), channel.unbindStore(b)];
    channel.unsubscribe(subscriber);
    return {events, result, replacement, unbound, active: channel.hasSubscribers};
  });
});

test('a bound store alone activates a channel and default transform receives the original message', async () => {
  await compare((dc, name) => {
    const channel = dc.channel(name), store = new AsyncLocalStorage(), data = {id: 1};
    channel.bindStore(store);
    const active = dc.hasSubscribers(name);
    const original = channel.runStores(data, () => store.getStore() === data);
    channel.publish({id: 2});
    const outside = store.getStore();
    channel.unbindStore(store);
    return {active, original, outside, final: channel.hasSubscribers};
  });
});

function traceListeners(trace, events) {
  const handlers = {};
  for (const name of ['start', 'end', 'asyncStart', 'asyncEnd', 'error']) handlers[name] = context =>
    events.push({event: name, result: context.result, error: context.error?.message, tag: context.tag});
  trace.subscribe(handlers);
  return handlers;
}
test('tracingChannel traceSync publishes results and errors in Node order with callback receiver', async () => {
  await compare((dc, name) => {
    const trace = dc.tracingChannel(name), events = [], handlers = traceListeners(trace, events);
    const result = trace.traceSync(function(value) { return this.offset + value; }, {tag: 'ok'}, {offset: 3}, 9);
    const original = new Error('sync error'); let sameError;
    try { trace.traceSync(() => { throw original; }, {tag: 'failure'}); }
    catch (error) { sameError = error === original; }
    const unsubscribe = trace.unsubscribe(handlers);
    return {result, sameError, unsubscribe, events, active: trace.hasSubscribers};
  });
});

test('tracePromise covers native promises, thenables, rejections and synchronous throws', async () => {
  await compare(async (dc, name) => {
    const trace = dc.tracingChannel(name), events = [], handlers = traceListeners(trace, events);
    const result = await trace.tracePromise(function(value) { return Promise.resolve(this.offset + value); }, {tag: 'promise'}, {offset: 2}, 4);
    const thenable = await trace.tracePromise(() => ({then(resolve) { resolve(8); }}), {tag: 'thenable'});
    const rejection = new Error('rejected'); let sameRejection, synchronous;
    try { await trace.tracePromise(() => Promise.reject(rejection), {tag: 'reject'}); }
    catch (error) { sameRejection = error === rejection; }
    try { trace.tracePromise(() => { throw new Error('thrown'); }, {tag: 'throw'}); }
    catch (error) { synchronous = error.message; }
    trace.unsubscribe(handlers);
    return {result, thenable, sameRejection, synchronous, events};
  });
});

test('traceCallback restores callback receiver, all arguments, bound stores and asynchronous event order', async () => {
  await compare(async (dc, name) => {
    const trace = dc.tracingChannel(name), events = [], handlers = traceListeners(trace, events);
    const store = new AsyncLocalStorage(), receiver = {tag: 'receiver'}, callbackReceiver = {tag: 'callback'};
    trace.asyncStart.bindStore(store, context => context.tag);
    const result = await new Promise(resolve => {
      const returned = trace.traceCallback(function(first, callback, last) {
        events.push({event: 'function', receiver: this === receiver, first, last});
        queueMicrotask(() => callback.call(callbackReceiver, null, 42, 'extra'));
        return 7;
      }, 1, {tag: 'callback-context'}, receiver, 'first', function(...args) {
        events.push({event: 'callback', receiver: this === callbackReceiver, args, store: store.getStore()});
        resolve('done'); return 'ignored';
      }, 'last');
      events.push({event: 'return', returned});
    });
    trace.asyncStart.unbindStore(store); trace.unsubscribe(handlers);
    return {result, events, outside: store.getStore()};
  });
});

test('traceCallback observes error callbacks and synchronous callback exceptions', async () => {
  await compare((dc, name) => {
    const trace = dc.tracingChannel(name), events = [], handlers = traceListeners(trace, events);
    trace.traceCallback(callback => callback(new Error('callback error')), -1, {tag: 'error'}, null,
      error => events.push({event: 'user', error: error.message}));
    let thrown;
    try { trace.traceCallback(callback => callback(null, 3), -1, {tag: 'throws'}, null, () => { throw new Error('user throw'); }); }
    catch(error) { thrown = error.message; }
    trace.unsubscribe(handlers);return {events, thrown};
  });
});

test('inactive tracing preserves original return value and does not require a callback', async () => {
  await compare((dc, name) => {
    const trace = dc.tracingChannel(name), context = {};
    const promise = Promise.resolve(7);
    return {sync: trace.traceSync(value => value, context, null, 3),
      promiseIdentity: trace.tracePromise(() => promise) === promise,
      plain: trace.tracePromise(() => 5), callback: trace.traceCallback(value => value, -1, context, null, 9),
      unchanged: Object.keys(context), active: trace.hasSubscribers};
  });
});

test('tracing accepts existing channels and enforces the actual input contract', async () => {
  await compare((dc, name) => {
    const channels = Object.fromEntries(['start','end','asyncStart','asyncEnd','error'].map(key => [key, dc.channel(name + ':' + key)]));
    const trace = dc.tracingChannel(channels), errors = [];
    for(const input of [null, 1, {}, Symbol('invalid')]) {
      try { dc.tracingChannel(input); } catch(error) { errors.push(error.code); }
    }
    for(const input of [null, 1, {}]) { try { dc.channel(input); } catch(error) { errors.push(error.code); } }
    try { trace.start.subscribe(1); } catch(error) { errors.push(error.code); }
    return {identity: trace.start === channels.start, enumerable: Object.keys(trace), errors};
  });
});

test('subscriber and transform failures are reported asynchronously without stopping publication or runStores', () => {
  const program = `const dc=require('node:diagnostics_channel');const events=[];process.on('uncaughtException',e=>events.push('error:'+e.message));const c=dc.channel('failure-reference');c.subscribe(()=>{throw Error('subscriber')});c.subscribe(()=>events.push('second'));c.bindStore({run(){events.push('unexpected-run')}},()=>{throw Error('transform')});events.push(c.runStores({},()=>{events.push('function');return 7}));setImmediate(()=>console.log(JSON.stringify(events)));`;
  const expected = JSON.parse(execFileSync(process.execPath, ['-e', program], {encoding:'utf8'}).trim());
  const f = fixture(), events = [], channel = f.api.channel('failure-reference');
  channel.subscribe(() => { throw Error('subscriber'); }); channel.subscribe(() => events.push('second'));
  channel.bindStore({run() { events.push('unexpected-run'); }}, () => { throw Error('transform'); });
  events.push(channel.runStores({}, () => { events.push('function'); return 7; }));
  for(const error of f.errors)events.push('error:' + error.message);
  assert.deepEqual(events, expected);
});

test('complete bootstrap exposes diagnostics and asynchronously delivers subscriber failures to process', async () => {
  const context = vm.createContext({TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, atob, btoa,
    console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE',
    __xenonOsRelease: '', __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture', __xenonAppVersion: '1',
    __xenonUserAgent: '', __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1,
    __xenonEnv: {}, __xenonChromeVersion: '153', __xenonV8Version: '', __xenonGetPath: () => '',
  });
  const filename = path.join(__dirname, 'xenon_ipc_main_bootstrap.js');
  vm.runInContext(readBootstrap(filename), context, {filename});
  const observed = [], dc = context.__xenonDiagnosticsChannel;
  context.process.on('uncaughtException', error => observed.push(error.message));
  dc.subscribe('bootstrap', () => { throw new Error('diagnostic failure'); });
  dc.subscribe('bootstrap', () => observed.push('continued'));
  dc.channel('bootstrap').publish({});
  assert.deepEqual(observed, ['continued']);
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(observed, ['continued', 'diagnostic failure']);
});
