// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const nodeTimers = require('node:timers');
const {setTimeout: wait} = require('node:timers/promises');
const factorySource = fs.readFileSync(path.join(__dirname, 'common/timers.js'), 'utf8');
const createNodeTimers = vm.runInThisContext(factorySource + '\ncreateNodeTimers;');
const createImmediateTaskScheduler = vm.runInThisContext(factorySource + '\ncreateImmediateTaskScheduler;');

for (const [name, api] of [['node', nodeTimers], ['hosted', createNodeTimers(nodeTimers)]]) {
  test(`${name}: Timeout identity, callback receiver, arguments and module cancellation`, async () => {
    let handle;
    const fired = new Promise(resolve => {
      handle = api.setTimeout(function(a, b) { resolve([this === handle, a, b]); }, 1, 7, 'eight');
      assert.equal(handle.constructor.name, 'Timeout');
      assert.equal(handle.ref(), handle);
      assert.equal(handle.hasRef(), true);
      assert.equal(handle.unref(), handle);
      assert.equal(handle.hasRef(), false);
      handle.ref();
      assert.equal(typeof +handle, 'number');
    });
    assert.deepEqual(await fired, [true, 7, 'eight']);
    let unexpected = 0;
    for (const convert of [v => v, v => +v, v => String(+v)]) {
      api.clearInterval(convert(api.setTimeout(() => ++unexpected, 1)));
    }
    await wait(10);
    assert.equal(unexpected, 0);
  });
  test(`${name}: refresh within a callback retains its primitive cancellation identity`, async () => {
    let calls = 0;
    let timer;
    const identity = await new Promise(resolve => {
      timer = api.setTimeout(function() {
        ++calls;
        const before = +this;
        assert.equal(this.refresh(), this);
        const after = +this;
        api.clearTimeout(before);
        resolve([before, after]);
      }, 1);
    });
    assert.equal(identity[0], identity[1]);
    await wait(10);
    assert.equal(calls, 1);
    assert.equal(timer.refresh(), timer, 'canceled timer stays canceled');
    await wait(5);
    assert.equal(calls, 1);
  });
  test(`${name}: expired refresh fires again but close and dispose prevent revival`, async () => {
    let calls = 0, resolve;
    const timer = api.setTimeout(() => { ++calls; resolve(); }, 1);
    await new Promise(done => { resolve = done; });
    const oldId = +timer;
    const again = new Promise(done => { resolve = done; });
    timer.refresh();
    assert.notEqual(+timer, oldId);
    await again;
    assert.equal(calls, 2);
    assert.equal(timer.close(), timer);
    timer.refresh();
    const disposed = api.setInterval(() => ++calls, 1);
    assert.equal(disposed[Symbol.dispose](), undefined);
    disposed.refresh();
    await wait(10);
    assert.equal(calls, 2);
  });
  test(`${name}: primitive cancellation during callback prevents refresh`, async () => {
    let calls = 0;
    await new Promise(resolve => {
      api.setTimeout(function() {
        ++calls;
        api.clearTimeout(+this);
        this.refresh();
        resolve();
      }, 1);
    });
    await wait(10);
    assert.equal(calls, 1);
  });
  test(`${name}: interval refresh does not duplicate registrations`, async () => {
    let calls = 0;
    await new Promise(resolve => {
      api.setInterval(function() {
        if (++calls === 1) this.refresh();
        else { api.clearTimeout(this); resolve(); }
      }, 2);
    });
    await wait(10);
    assert.equal(calls, 2);
  });
  test(`${name}: clearTimeout does not coerce arbitrary objects or noncanonical strings`, async () => {
    let coerced = false;
    api.clearTimeout({valueOf() { coerced = true; return 1; }});
    assert.equal(coerced, false);
    let timer;
    const fired = new Promise(resolve => { timer = api.setTimeout(resolve, 1); });
    api.clearTimeout('0' + +timer);
    await fired;
  });
  test(`${name}: Immediate cancels and loses its reference after callback or disposal`, async () => {
    let calls = 0;
    await new Promise(resolve => {
      const immediate = api.setImmediate(function(value) {
        ++calls;
        assert.equal(this, immediate);
        assert.equal(value, 42);
        assert.equal(immediate.hasRef(), false);
        immediate.ref();
        assert.equal(immediate.hasRef(), false);
        resolve();
      }, 42);
      assert.equal(immediate.constructor.name, 'Immediate');
      assert.equal(immediate.unref(), immediate);
      assert.equal(immediate.hasRef(), false);
      immediate.ref();
      assert.equal(immediate.hasRef(), true);
    });
    const canceled = api.setImmediate(() => ++calls);
    canceled[Symbol.dispose]();
    assert.equal(canceled.hasRef(), false);
    canceled.ref();
    assert.equal(canceled.hasRef(), false);
    await wait(5);
    assert.equal(calls, 1);
  });
  test(`${name}: callback and numeric coercion validation match Node`, () => {
    for (const method of ['setTimeout', 'setInterval', 'setImmediate']) {
      for (const callback of [undefined, null, 1, 'callback', {}]) {
        assert.throws(() => api[method](callback), {code: 'ERR_INVALID_ARG_TYPE'});
      }
    }
    for (const delay of [1n, Symbol('delay')]) {
      assert.throws(() => api.setTimeout(() => {}, delay), TypeError);
    }
  });
}

test('native references are updated for refresh and canceled timers leave no active host entry', () => {
  const entries = new Map();
  let id = 0;
  const api = createNodeTimers({
    setTimeout(fn, delay) { const key = ++id; entries.set(key, {fn, delay, ref: true}); return key; },
    clearTimeout(key) { entries.delete(key); },
    setReferenced(key, value) { entries.get(key).ref = value; },
  });
  const timer = api.setTimeout(() => {}, '25');
  assert.equal(entries.get(1).delay, 25);
  timer.unref();
  assert.equal(entries.get(1).ref, false);
  timer.refresh();
  assert.equal(entries.has(1), false);
  assert.equal(entries.get(2).ref, false);
  timer.ref();
  assert.equal(entries.get(2).ref, true);
  api.clearTimeout(timer);
  timer.ref(); timer.unref(); timer.refresh();
  assert.equal(entries.size, 0);
});

test('renderer Immediate uses a cancellable message task without DOM timers or microtasks', async () => {
  const {MessageChannel} = require('node:worker_threads');
  const scheduler = createImmediateTaskScheduler(MessageChannel);
  const api = createNodeTimers({
    ...scheduler,
    setTimeout() { throw Error('Immediate must not enter DOM timer clamping'); },
    clearTimeout() { throw Error('Immediate must cancel its own task'); },
  });
  const order = [];
  api.clearImmediate(api.setImmediate(() => order.push('canceled')));
  await new Promise(resolve => {
    api.setImmediate(() => {
      order.push('first');
      api.setImmediate(() => { order.push('nested'); resolve(); });
    });
    api.setImmediate(() => order.push('second'));
    queueMicrotask(() => order.push('microtask'));
    order.push('sync');
  });
  assert.deepEqual(order, ['sync', 'microtask', 'first', 'second', 'nested']);
});
