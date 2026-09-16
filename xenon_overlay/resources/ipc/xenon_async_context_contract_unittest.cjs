// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {promiseHooks} = require('node:v8');
const vm = require('node:vm');

function createRuntime(kind, t, withHooks = true) {
  let stopHooks;
  let dispatch;
  let realmPromise;
  const install = (init, before, after) => {
    // Real V8 lifecycle hooks, including intrinsic async/await. The native
    // binding scopes these to its context; Node's test API needs this filter.
    const owned = new WeakSet();
    stopHooks = promiseHooks.createHook({
      init(promise) {
        if (!(promise instanceof realmPromise)) return;
        owned.add(promise);
        init(promise);
      },
      before(promise) { if (owned.has(promise)) before(promise); },
      after(promise) { if (owned.has(promise)) after(promise); },
    });
  };
  const context = vm.createContext({
    __xenonPaths: {platform: 'win32', arch: 'x64', endianness: 'LE'},
    TextEncoder, TextDecoder, URL, URLSearchParams, queueMicrotask,
    setTimeout, clearTimeout, setInterval, clearInterval, setImmediate,
    clearImmediate, atob, btoa, console: {log() {}, warn() {}, error() {}},
    __xenonPlatform: 'win32', __xenonArch: 'x64', __xenonEndianness: 'LE', __xenonOsRelease: '10.0',
    __xenonAppPath: 'C:\\fixture', __xenonRendererBaseUrl: '',
    __xenonRendererUrlMappings: [], __xenonAppName: 'fixture',
    __xenonAppVersion: '1', __xenonUserAgent: '',
    __xenonExecPath: 'C:\\fixture\\host.exe', __xenonPid: 1, __xenonEnv: {},
    __xenonChromeVersion: '142', __xenonV8Version: '', __xenonGetPath: () => '',
    __xenonInstallAsyncContextHooks: withHooks ? install : undefined,
    location: {protocol: 'chrome:', hostname: 'xenon-player-electron', search: ''},
    xenonIpcRenderer: {
      getRuntimeConfig: () => ({appPath: 'C:\\fixture', exeDir: 'C:\\fixture',
        execPath: 'C:\\fixture\\host.exe'}),
      installAsyncContextHooks: withHooks ? install : undefined,
      setDispatchHandler(callback) { dispatch = callback; },
      sendSync(channel, options) {
        if (channel === '__xenon:renderer-web-preferences') return {};
        if (options.operation === 'exists') return false;
        throw new Error('ENOENT');
      },
    },
  });
  realmPromise = vm.runInContext('Promise', context);
  const filename = path.join(__dirname, `xenon_ipc_${kind}_bootstrap.js`);
  vm.runInContext(readFileSync(filename, 'utf8'), context, {filename});
  context.hooks = kind === 'main' ? context.__xenonAsyncHooks :
      context.require('node:async_hooks');
  context.events = kind === 'main' ? context.__xenonElectron.ipcMain :
      context.require('electron').ipcRenderer;
  t.after(() => stopHooks?.());
  return {
    context,
    evaluate(source) { return vm.runInContext(source, context); },
    dispatch(channel, args = []) {
      if (kind === 'main') context.__xenonDispatchSend({id: 1}, channel, args);
      else dispatch(channel, args);
    },
  };
}

test('renderer native callbacks retain registration scope across IPC and release', t => {
  const runtime = createRuntime('renderer', t);
  const transport = runtime.context.xenonIpcRenderer;
  let callbackId;
  transport.sendSync = (_channel, options) => {
    if (options.operation === 'realpath') return options.path;
    if (options.operation === 'stat') return {isFile: true, isDirectory: false};
    if (options.operation === 'exists') return true;
    throw new Error('Unexpected filesystem request');
  };
  transport.requireNodeModuleSync = () => [{name: 'register', kind: 'function'}];
  transport.invokeNodeExportSync = (_module, _name, callback) => {
    callbackId = callback.callback_id;
  };
  runtime.evaluate(`
    globalThis.storage = new hooks.AsyncLocalStorage();
    globalThis.observed = [];
    storage.run('native-callback', () => {
      require('C:\\\\fixture\\\\fixture.node').register(function(value) {
        observed.push([storage.getStore(), value, this.name]);
      });
    });
  `);
  assert.equal(typeof callbackId, 'number');
  runtime.dispatch('__xenon:node-addon:callback', [callbackId, [42], {name: 'receiver'}]);
  assert.deepEqual(JSON.parse(JSON.stringify(runtime.context.observed)),
      [['native-callback', 42, 'receiver']]);
  runtime.dispatch('__xenon:node-addon:callback-released', [callbackId]);
  runtime.dispatch('__xenon:node-addon:callback', [callbackId, [43], {name: 'receiver'}]);
  assert.equal(runtime.context.observed.length, 1);
  assert.equal(runtime.evaluate('storage.getStore()'), undefined);
});

for (const kind of ['main', 'renderer']) {
  test(`${kind} ALS preserves native await, concurrent branches and rejection`, async t => {
    const runtime = createRuntime(kind, t);
    const result = await runtime.evaluate(`(async () => {
      const storage = new hooks.AsyncLocalStorage();
      let release;
      const gate = new Promise(resolve => { release = resolve; });
      const first = storage.run('first', async () => {
        await gate;
        const before = storage.getStore();
        const nested = await storage.run('nested', async () => {
          await Promise.resolve();
          return storage.getStore();
        });
        return [before, nested, storage.getStore()];
      });
      const second = storage.run('second', async () => {
        await Promise.reject(new Error('expected')).catch(() => {});
        release();
        return storage.getStore();
      });
      return [await first, await second, storage.getStore()];
    })()`);
    assert.deepEqual(JSON.parse(JSON.stringify(result)),
        [['first', 'nested', 'first'], 'second', null]);
  });

  test(`${kind} ALS propagates timers, arguments, microtasks and nextTick`, async t => {
    const runtime = createRuntime(kind, t);
    const result = await runtime.evaluate(`(async () => {
      const storage = new hooks.AsyncLocalStorage();
      return storage.run('scheduled', async () => {
        const timer = await new Promise(resolve => setTimeout(value =>
            resolve([storage.getStore(), value]), 0, 42));
        const microtask = await new Promise(resolve =>
            queueMicrotask(() => resolve(storage.getStore())));
        const tick = await new Promise(resolve => process.nextTick(() =>
            resolve(storage.getStore())));
        return [timer, microtask, tick, storage.getStore()];
      });
    })()`);
    assert.deepEqual(JSON.parse(JSON.stringify(result)),
        [['scheduled', 42], 'scheduled', 'scheduled', 'scheduled']);
  });

  test(`${kind} ALS snapshot isolates later instances and bind preserves receiver`, async t => {
    const runtime = createRuntime(kind, t);
    const result = await runtime.evaluate(`(async () => {
      const first = new hooks.AsyncLocalStorage();
      const snapshot = first.run('captured', () => hooks.AsyncLocalStorage.snapshot());
      const second = new hooks.AsyncLocalStorage();
      const values = second.run('later', () => snapshot(() =>
          [first.getStore(), second.getStore()]));
      const bound = first.run('bound', () => hooks.AsyncLocalStorage.bind(
          function(suffix) { return [this.name, first.getStore(), suffix]; }));
      const resource = first.run('resource', () => new hooks.AsyncResource('fixture'));
      const resourceBound = resource.bind(function() { return [this.name, first.getStore()]; });
      return [values, bound.call({name: 'receiver'}, 7),
        resourceBound.call({name: 'resource-receiver'}),
        await resource.runInAsyncScope(async () => {
          await Promise.resolve(); return first.getStore();
        })];
    })()`);
    assert.deepEqual(JSON.parse(JSON.stringify(result)),
        [['captured', null], ['receiver', 'bound', 7],
          ['resource-receiver', 'resource'], 'resource']);
  });

  test(`${kind} ALS disable invalidates pending stores and supports reuse`, async t => {
    const runtime = createRuntime(kind, t);
    const result = await runtime.evaluate(`(async () => {
      const storage = new hooks.AsyncLocalStorage({defaultValue: 'default', name: 'fixture'});
      let release;
      const pending = storage.run('old', async () => {
        await new Promise(resolve => { release = resolve; });
        return storage.getStore();
      });
      const snapshot = storage.run('old', () => hooks.AsyncLocalStorage.snapshot());
      storage.disable();
      const reused = storage.run('new', () => storage.getStore());
      release();
      return [await pending, reused, snapshot(() => storage.getStore()),
        storage.getStore(), storage.exit(() => storage.getStore()), storage.name];
    })()`);
    assert.deepEqual(JSON.parse(JSON.stringify(result)),
        ['default', 'new', 'default', 'default', null, 'fixture']);
  });

  test(`${kind} ALS restores scope after throws and isolates independent IPC`, async t => {
    const runtime = createRuntime(kind, t);
    runtime.evaluate(`
      globalThis.storage = new hooks.AsyncLocalStorage();
      globalThis.observed = [];
      events.on('first', () => {
        storage.enterWith('message');
        queueMicrotask(() => observed.push(storage.getStore()));
      });
      events.on('second', () => observed.push(storage.getStore()));
    `);
    runtime.dispatch('first');
    runtime.dispatch('second');
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(Array.from(runtime.context.observed), [undefined, 'message']);
    assert.equal(runtime.evaluate(`(() => {
      try { storage.run('throw', () => { throw new Error('expected'); }); } catch {}
      return storage.getStore();
    })()`), undefined);
  });

  test(`${kind} async_hooks rejects unavailable lifecycle APIs and missing native support`, t => {
    const runtime = createRuntime(kind, t);
    for (const call of ['hooks.createHook({})', 'hooks.executionAsyncId()',
      'hooks.triggerAsyncId()', 'hooks.executionAsyncResource()',
      'new hooks.AsyncResource("test").asyncId()',
      'new hooks.AsyncResource("test").triggerAsyncId()',
      'new hooks.AsyncResource("test").emitDestroy()']) {
      assert.throws(() => runtime.evaluate(call), {code: 'ERR_NOT_SUPPORTED'});
    }
    const unsupported = createRuntime(kind, t, false);
    assert.equal(unsupported.evaluate('new hooks.AsyncLocalStorage().getStore()'), undefined);
    assert.throws(() => unsupported.evaluate(
        'new hooks.AsyncLocalStorage().run(1, () => 2)'), {code: 'ERR_NOT_SUPPORTED'});
  });
}
