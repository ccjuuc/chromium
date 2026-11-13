'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const timers = require('node:timers');
const nodePromises = require('node:timers/promises');
const factory = vm.runInThisContext(fs.readFileSync(
    path.join(__dirname, 'common/timers_promises.js'), 'utf8') + '\ncreateTimersPromises;');
const hosted = factory(timers);

for (const [name, api] of [['node', nodePromises], ['hosted', hosted]]) {
  test(`${name}: timeout/immediate/scheduler values and asynchronous ordering`, async () => {
    const order = [];
    const value = {};
    const pending = api.setTimeout(1, value).then(result => { order.push('timer'); return result; });
    order.push('sync');
    assert.equal(await pending, value);
    assert.deepEqual(order, ['sync', 'timer']);
    assert.equal(await api.setImmediate(value), value);
    assert.equal(await api.scheduler.wait(1), undefined);
    assert.equal(await api.scheduler.yield(), undefined);
  });
  test(`${name}: abort before scheduling and while waiting preserves cause`, async () => {
    const controller = new AbortController();
    const reason = new Error('cancel');
    const pending = api.setTimeout(60000, 'value', {signal: controller.signal});
    controller.abort(reason);
    const check = error => error.name === 'AbortError' && error.code === 'ABORT_ERR' && error.cause === reason;
    await assert.rejects(pending, check);
    await assert.rejects(api.setImmediate(null, {signal: controller.signal}), check);
  });
  test(`${name}: interval iterates and aborts a pending next`, async () => {
    const controller = new AbortController();
    const iterator = api.setInterval(1, 42, {signal: controller.signal});
    assert.deepEqual(await iterator.next(), {done: false, value: 42});
    const next = iterator.next();
    controller.abort();
    await assert.rejects(next, {code: 'ABORT_ERR'});
    assert.deepEqual(await iterator.next(), {done: true, value: undefined});
  });
  test(`${name}: rejects invalid options`, async () => {
    for (const options of [null, 1, {ref: 1}, {signal: null}]) {
      await assert.rejects(api.setTimeout(1, null, options), {code: 'ERR_INVALID_ARG_TYPE'});
      await assert.rejects(api.setInterval(1, null, options).next(), {code: 'ERR_INVALID_ARG_TYPE'});
    }
  });
}
test('hosted: iterator return and unsupported unref release native timers', async () => {
  const active = new Set();
  let clears = 0;
  const counted = factory({
    ...timers,
    setInterval(fn, delay) { const timer = timers.setInterval(fn, delay); active.add(timer); return timer; },
    clearInterval(timer) { ++clears; active.delete(timer); timers.clearInterval(timer); },
  });
  for await (const value of counted.setInterval(1, 42)) { assert.equal(value, 42); break; }
  assert.equal(active.size, 0);
  assert.equal(clears, 1);
  let cancelled;
  const browser = factory({...timers, setTimeout: () => 17, clearTimeout: id => { cancelled = id; }});
  await assert.rejects(browser.setTimeout(1, null, {ref: false}), {code: 'ERR_NOT_SUPPORTED'});
  assert.equal(cancelled, 17);
});
