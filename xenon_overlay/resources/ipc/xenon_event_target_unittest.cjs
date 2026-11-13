// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const test = require('node:test');
const vm = require('node:vm');
const {readBootstrapPart} = require('./bootstrap_test_support.cjs');
const source = readBootstrapPart('main/event_target.js');

function classes(reportError = error => { throw error; }) {
  return vm.runInNewContext(source + '\ncreateMainEventClasses(reportError, now)',
      {reportError, now: () => performance.now()});
}

for (const [side, api] of [['Node', {Event, CustomEvent, EventTarget}], ['main', classes()]]) {
  const {Event, CustomEvent, EventTarget} = api;

  test(`${side}: Event and CustomEvent retain their own contracts`, () => {
    const event = new Event('change', {bubbles: true, cancelable: true, composed: true});
    assert.equal(event.type, 'change');
    assert.equal(event.bubbles, true);
    assert.equal(event.cancelable, true);
    assert.equal(event.composed, true);
    assert.equal(event.target, null);
    assert.equal(event.currentTarget, null);
    assert.equal(event.isTrusted, false);
    assert.equal(event.eventPhase, Event.NONE);
    assert.equal(Event.AT_TARGET, 2);
    assert.ok(Number.isFinite(event.timeStamp) && event.timeStamp >= 0);
    assert.equal(Object.prototype.toString.call(event), '[object Event]');
    const detail = {answer: 42};
    const custom = new CustomEvent('message', {detail});
    assert.ok(custom instanceof Event);
    assert.equal(custom.detail, detail);
    assert.equal(new CustomEvent('message').detail, null);
    assert.equal(Object.prototype.toString.call(custom), '[object CustomEvent]');
    assert.equal(new Event(undefined).type, 'undefined');
    assert.throws(() => new Event(), {code: 'ERR_MISSING_ARGS'});
    assert.throws(() => new CustomEvent(), {code: 'ERR_MISSING_ARGS'});
    assert.throws(() => new Event(Symbol('type')), {name: 'TypeError'});
    assert.throws(() => new Event('x', 1), {code: 'ERR_INVALID_ARG_TYPE'});
  });

  test(`${side}: dispatch sets target and phase only while calling listeners`, () => {
    const target = new EventTarget();
    const event = new Event('ready');
    let calls = 0;
    target.addEventListener('ready', function(received) {
      ++calls;
      assert.equal(this, target);
      assert.equal(received, event);
      assert.equal(event.target, target);
      assert.equal(event.srcElement, target);
      assert.equal(event.currentTarget, target);
      assert.equal(event.eventPhase, Event.AT_TARGET);
      assert.deepEqual(Array.from(event.composedPath()), [target]);
    });
    assert.equal(target.dispatchEvent(event), true);
    assert.equal(calls, 1);
    assert.equal(event.target, target);
    assert.equal(event.currentTarget, null);
    assert.equal(event.eventPhase, Event.NONE);
    assert.equal(event.composedPath().length, 0);
    assert.equal(typeof target.emit, 'undefined');
    assert.equal(target.dispatchEvent(new Event('error')), true);
  });

  test(`${side}: duplicate identity includes capture, and once removes before reentry`, () => {
    const target = new EventTarget();
    let ordinary = 0;
    function listener() { ++ordinary; }
    target.addEventListener('x', listener);
    target.addEventListener('x', listener, {once: true});
    target.addEventListener('x', listener, {capture: true});
    target.dispatchEvent(new Event('x'));
    assert.equal(ordinary, 2);
    target.removeEventListener('x', listener, {capture: true});
    target.dispatchEvent(new Event('x'));
    assert.equal(ordinary, 3);
    target.removeEventListener('x', listener);
    let once = 0;
    target.addEventListener('x', () => {
      ++once;
      target.dispatchEvent(new Event('x'));
    }, {once: true});
    target.dispatchEvent(new Event('x'));
    target.dispatchEvent(new Event('x'));
    assert.equal(once, 1);
  });

  test(`${side}: object callbacks are resolved at dispatch and keep object receiver`, () => {
    const target = new EventTarget();
    const observed = [];
    const listener = {handleEvent(event) { observed.push(this === listener, event.type); }};
    target.addEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    listener.handleEvent = function() { observed.push(this === listener, 'changed'); };
    target.dispatchEvent(new Event('x'));
    target.removeEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    assert.deepEqual(observed, [true, 'x', true, 'changed']);
    target.addEventListener('x', {});
    assert.equal(target.dispatchEvent(new Event('x')), true);
  });

  test(`${side}: changes to listener lists preserve in-flight ordering`, () => {
    const target = new EventTarget();
    const observed = [];
    const removed = () => observed.push('removed');
    const added = () => observed.push('added');
    target.addEventListener('x', () => {
      observed.push('first');
      target.removeEventListener('x', removed);
      target.addEventListener('x', added);
    });
    target.addEventListener('x', removed);
    target.addEventListener('x', () => observed.push('last'));
    target.dispatchEvent(new Event('x'));
    assert.deepEqual(observed, ['first', 'last', 'added']);
    const other = new EventTarget();
    const late = [];
    other.addEventListener('x', () => {
      late.push('only');
      other.addEventListener('x', () => late.push('too late'));
    });
    other.dispatchEvent(new Event('x'));
    assert.deepEqual(late, ['only']);
  });

  test(`${side}: propagation flags and cancelation have distinct behavior`, () => {
    const target = new EventTarget();
    const calls = [];
    target.addEventListener('x', event => { calls.push(1); event.stopPropagation(); });
    target.addEventListener('x', event => { calls.push(2); event.preventDefault(); });
    const event = new Event('x', {cancelable: true});
    assert.equal(target.dispatchEvent(event), false);
    assert.deepEqual(calls, [1, 2]);
    assert.equal(event.cancelBubble, true);
    event.cancelBubble = false;
    event.returnValue = true;
    assert.equal(event.cancelBubble, true);
    assert.equal(event.defaultPrevented, true);
    assert.equal(event.returnValue, false);
    const stopped = new Event('x');
    stopped.stopImmediatePropagation();
    target.dispatchEvent(stopped);
    assert.deepEqual(calls, [1, 2]);
    const immediate = new EventTarget();
    immediate.addEventListener('x', value => { calls.push(3); value.stopImmediatePropagation(); });
    immediate.addEventListener('x', () => calls.push(4));
    immediate.dispatchEvent(new Event('x'));
    assert.deepEqual(calls, [1, 2, 3]);
  });

  test(`${side}: passive listeners cannot prevent a default action`, () => {
    const target = new EventTarget();
    const passive = new Event('x', {cancelable: true});
    target.addEventListener('x', event => {
      event.preventDefault();
      event.returnValue = false;
    }, {passive: true});
    assert.equal(target.dispatchEvent(passive), true);
    assert.equal(passive.defaultPrevented, false);
    const noncancelable = new Event('x');
    noncancelable.preventDefault();
    assert.equal(noncancelable.defaultPrevented, false);
  });

  test(`${side}: recursion fails synchronously without corrupting the event`, () => {
    const target = new EventTarget();
    const event = new Event('x');
    let errorCode;
    target.addEventListener('x', value => {
      try { target.dispatchEvent(value); } catch (error) { errorCode = error.code; }
      value.initEvent('changed');
      assert.equal(value.type, 'x');
    });
    target.dispatchEvent(event);
    assert.equal(errorCode, 'ERR_EVENT_RECURSION');
    event.initEvent('changed', true, true);
    assert.equal(event.type, 'changed');
    assert.equal(event.bubbles, true);
    assert.equal(event.cancelable, true);
  });

  test(`${side}: external AbortSignal cancels registered listeners`, () => {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    target.addEventListener('x', () => ++calls, {signal: controller.signal});
    target.dispatchEvent(new Event('x'));
    controller.abort();
    target.dispatchEvent(new Event('x'));
    target.addEventListener('x', () => ++calls, {signal: controller.signal});
    target.dispatchEvent(new Event('x'));
    assert.equal(calls, 1);
    assert.throws(() => target.addEventListener('x', () => {}, {signal: {}}),
        {code: 'ERR_INVALID_ARG_TYPE'});
  });

  test(`${side}: invalid receivers and event-shaped objects are rejected`, () => {
    const target = new EventTarget();
    for (const value of [{type: 'x'}, null, undefined, 1])
      assert.throws(() => target.dispatchEvent(value), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => target.dispatchEvent(), {code: 'ERR_MISSING_ARGS'});
    assert.throws(() => target.addEventListener('x'), {code: 'ERR_MISSING_ARGS'});
    assert.throws(() => target.addEventListener('x', 1), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => target.removeEventListener('x', 1), {code: 'ERR_INVALID_ARG_TYPE'});
    assert.throws(() => EventTarget.prototype.dispatchEvent.call({}, new Event('x')),
        {code: 'ERR_INVALID_THIS'});
    assert.throws(() => Event.prototype.preventDefault.call({}), {code: 'ERR_INVALID_THIS'});
    let read = false;
    target.addEventListener('x', null, {get once() { read = true; return true; }});
    assert.equal(read, true);
  });
}

test('main: thrown and rejected listener errors follow Node asynchronous reporting', () => {
  const scenario = `
    const order = [];
    process.on('uncaughtException', error => order.push(error.message));
    const target = new EventTarget();
    target.addEventListener('x', () => { throw new Error('thrown'); });
    target.addEventListener('x', () => Promise.reject(new Error('rejected')));
    target.addEventListener('x', () => ({get then() { throw new Error('then getter'); }}));
    target.addEventListener('x', () => order.push('last listener'));
    order.push(target.dispatchEvent(new Event('x')));
    process.once('beforeExit', () => process.stdout.write(JSON.stringify(order)));
  `;
  const expected = execFileSync(process.execPath, ['-e', scenario], {encoding: 'utf8'});
  const actual = execFileSync(process.execPath, ['-e', source + `
    Object.assign(globalThis, createMainEventClasses(error => {
      queueMicrotask(() => process.emit('uncaughtException', error, 'uncaughtException'));
    }, () => performance.now()));
  ` + scenario], {encoding: 'utf8'});
  assert.deepEqual(JSON.parse(actual), JSON.parse(expected));
});

test('main: boolean capture removal and cleanup do not leak canceled listeners', () => {
  const {Event, EventTarget} = classes();
  const target = new EventTarget();
  let calls = 0;
  const listener = () => ++calls;
  target.addEventListener('x', listener, true);
  target.removeEventListener('x', listener, true);
  target.dispatchEvent(new Event('x'));
  assert.equal(calls, 0);
  const controller = new AbortController();
  target.addEventListener('x', listener, {signal: controller.signal, once: true});
  target.dispatchEvent(new Event('x'));
  assert.equal(require('node:events').getEventListeners(controller.signal, 'abort').length, 0);
  assert.equal(calls, 1);
});
