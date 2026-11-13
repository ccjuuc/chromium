// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const nativeEvents = require('node:events');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');

function loadEvents(side) {
  const context = vm.createContext({queueMicrotask});
  vm.runInContext([
    readBootstrapPart(`${side}/events.js`),
    readBootstrapPart('common/events_helpers.js'),
    'globalThis.eventsModule = EventEmitter;',
  ].join('\n'), context);
  return context.eventsModule;
}

for (const [side, events] of [
  ['Node', nativeEvents], ['main', loadEvents('main')],
  ['renderer', loadEvents('renderer')],
]) {
  test(`${side}: events.once returns all arguments and removes listeners`, async () => {
    const emitter = new events.EventEmitter();
    const controller = new AbortController();
    const value = {answer: 42};
    const pending = events.once(emitter, 'ready', {signal: controller.signal});
    assert.equal(emitter.listenerCount('ready'), 1);
    assert.equal(emitter.listenerCount('error'), 1);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 1);
    emitter.emit('ready', 7, value);
    assert.deepEqual(Array.from(await pending), [7, value]);
    assert.equal(emitter.listenerCount('ready'), 0);
    assert.equal(emitter.listenerCount('error'), 0);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${side}: events.once rejects emitter errors and cleans up`, async () => {
    const emitter = new events.EventEmitter();
    const controller = new AbortController();
    const pending = events.once(emitter, 'ready', {signal: controller.signal});
    const failure = new Error('failed before ready');
    emitter.emit('error', failure);
    await assert.rejects(pending, error => error === failure);
    assert.equal(emitter.listenerCount('ready'), 0);
    assert.equal(emitter.listenerCount('error'), 0);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${side}: waiting for error resolves instead of rejecting`, async () => {
    const emitter = new events.EventEmitter();
    const failure = new Error('expected event');
    const pending = events.once(emitter, 'error');
    assert.equal(emitter.listenerCount('error'), 1);
    emitter.emit('error', failure, 9);
    assert.deepEqual(Array.from(await pending), [failure, 9]);
    assert.equal(emitter.listenerCount('error'), 0);
  });

  test(`${side}: cancellation preserves AbortError cause and removes listeners`, async () => {
    const emitter = new events.EventEmitter();
    const controller = new AbortController();
    const reason = new Error('cancelled by caller');
    const pending = events.once(emitter, 'ready', {signal: controller.signal});
    controller.abort(reason);
    await assert.rejects(pending, error => error.name === 'AbortError' &&
        error.code === 'ABORT_ERR' && error.cause === reason);
    assert.equal(emitter.listenerCount('ready'), 0);
    assert.equal(emitter.listenerCount('error'), 0);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${side}: pre-aborted signals reject before registering listeners`, async () => {
    const emitter = new events.EventEmitter();
    const signal = AbortSignal.abort('already stopped');
    await assert.rejects(events.once(emitter, 'ready', {signal}),
        {name: 'AbortError', code: 'ABORT_ERR', cause: 'already stopped'});
    assert.equal(emitter.eventNames().length, 0);
    assert.equal(nativeEvents.getEventListeners(signal, 'abort').length, 0);
  });

  test(`${side}: EventTarget resolves the event without special error handling`, async () => {
    const target = new EventTarget();
    const controller = new AbortController();
    const pending = events.once(target, 'ready', {signal: controller.signal});
    assert.equal(nativeEvents.getEventListeners(target, 'error').length, 0);
    target.dispatchEvent(new Event('error'));
    const event = new Event('ready');
    target.dispatchEvent(event);
    assert.deepEqual(Array.from(await pending), [event]);
    assert.equal(nativeEvents.getEventListeners(target, 'ready').length, 0);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${side}: EventTarget cancellation removes its event listener`, async () => {
    const target = new EventTarget();
    const controller = new AbortController();
    const pending = events.once(target, 'ready', {signal: controller.signal});
    controller.abort();
    await assert.rejects(pending, {name: 'AbortError', code: 'ABORT_ERR'});
    assert.equal(nativeEvents.getEventListeners(target, 'ready').length, 0);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${side}: options and signal validation rejects asynchronously`, async () => {
    for (const options of [null, 1, false, 'bad', [], () => {},
      {signal: null}, {signal: 1}, {signal: {}}, {signal: 'bad'}]) {
      const emitter = new events.EventEmitter();
      let pending;
      assert.doesNotThrow(() => { pending = events.once(emitter, 'ready', options); });
      await assert.rejects(pending, {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
      assert.equal(emitter.eventNames().length, 0);
    }
    await assert.rejects(events.once({}, 'ready'),
        {name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE'});
  });

  test(`${side}: events.once supports symbol event names`, async () => {
    const emitter = new events.EventEmitter();
    const event = Symbol('ready');
    const pending = events.once(emitter, event);
    emitter.emit(event, 3);
    assert.deepEqual(Array.from(await pending), [3]);
    assert.equal(emitter.eventNames().length, 0);
  });
}

for (const side of ['main', 'renderer']) {
  test(`${side}: cancellation during registration cannot leave a pending waiter`, async () => {
    const events = loadEvents(side);
    const emitter = new events.EventEmitter();
    const controller = new AbortController();
    emitter.on('newListener', name => {
      if (name === 'ready')
        controller.abort('registration race');
    });
    await assert.rejects(events.once(emitter, 'ready', {signal: controller.signal}),
        {name: 'AbortError', code: 'ABORT_ERR', cause: 'registration race'});
    assert.equal(emitter.listenerCount('ready'), 0);
    assert.equal(emitter.listenerCount('error'), 0);
    assert.equal(nativeEvents.getEventListeners(controller.signal, 'abort').length, 0);
  });

  test(`${side}: a registration error cleans up earlier listeners`, async () => {
    const events = loadEvents(side);
    const emitter = new events.EventEmitter();
    const failure = new Error('error-listener registration failed');
    emitter.on('newListener', name => {
      if (name === 'error')
        throw failure;
    });
    await assert.rejects(events.once(emitter, 'ready'), error => error === failure);
    assert.equal(emitter.listenerCount('ready'), 0);
    assert.equal(emitter.listenerCount('error'), 0);
  });
}
