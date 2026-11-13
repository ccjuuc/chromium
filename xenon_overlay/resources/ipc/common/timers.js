// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Posted messages use a task source independent of nested DOM timer clamping.
// Keep the ports private and release them when the last task is canceled/run.
function createImmediateTaskScheduler(MessageChannel) {
  let channel;
  let nextId = 1;
  const pending = new Map();
  function closeIfIdle() {
    if (pending.size || !channel) return;
    channel.port1.close();
    channel.port2.close();
    channel = undefined;
  }
  return {
    setImmediate(callback) {
      if (!channel) {
        channel = new MessageChannel();
        channel.port1.onmessage = event => {
          const task = pending.get(event.data);
          pending.delete(event.data);
          try { task?.(); }
          finally { closeIfIdle(); }
        };
      }
      const id = nextId++;
      pending.set(id, callback);
      channel.port2.postMessage(id);
      return id;
    },
    clearImmediate(id) {
      pending.delete(id);
      closeIfIdle();
    },
  };
}

// Keep Node handles separate from browser timer ids. Renderers install this
// module only for require('timers'); the web platform globals stay unchanged.
function createNodeTimers(host) {
  const states = new WeakMap();
  const active = new Map();
  let nextId = 1;

  function setReferenced(state, referenced) {
    if (state.immediate && state.nativeId === undefined) return;
    state.referenced = referenced;
    if (state.nativeId === undefined) return;
    if (host.setReferenced) host.setReferenced(state.nativeId, referenced);
    else if (referenced) state.nativeId?.ref?.();
    else state.nativeId?.unref?.();
  }
  function stop(state) {
    if (state.nativeId !== undefined) {
      const clear = state.interval ? host.clearInterval :
          state.immediate && host.clearImmediate ? host.clearImmediate : host.clearTimeout;
      clear(state.nativeId);
      state.nativeId = undefined;
    }
    active.delete(state.id);
  }
  function cancel(handle) {
    const state = states.get(handle);
    if (!state) return;
    stop(state);
    state.callback = null;
    state.args = null;
    if (state.immediate) state.referenced = false;
  }
  function start(handle, state) {
    active.set(state.id, handle);
    const invoke = () => {
      // A canceled callback must not run even if the host had already queued it.
      if (!state.callback) return;
      if (!state.interval) {
        state.nativeId = undefined;
      }
      const callback = state.callback;
      const args = state.args;
      if (state.immediate) {
        state.callback = null;
        state.args = null;
        state.referenced = false;
      }
      state.firing = true;
      try {
        Reflect.apply(callback, handle, args);
      } finally {
        state.firing = false;
        if (!state.interval && state.nativeId === undefined) active.delete(state.id);
      }
    };
    state.nativeId = state.immediate && host.setImmediate ? host.setImmediate(invoke) :
        (state.interval ? host.setInterval : host.setTimeout)(invoke, state.delay);
    if (!state.referenced) setReferenced(state, false);
  }
  class Timeout {
    constructor(callback, delay, args, interval) {
      const state = {id: nextId++, callback, delay, args, interval,
        immediate: false, referenced: true, nativeId: undefined};
      states.set(this, state);
      start(this, state);
    }
    ref() { setReferenced(states.get(this), true); return this; }
    unref() { setReferenced(states.get(this), false); return this; }
    hasRef() { return states.get(this).referenced; }
    refresh() {
      const state = states.get(this);
      if (state.callback) {
        const expired = state.nativeId === undefined && !state.firing;
        stop(state);
        // Node gives an expired, reactivated timer a fresh primitive identity.
        if (expired) state.id = nextId++;
        start(this, state);
      }
      return this;
    }
    close() { cancel(this); return this; }
    [Symbol.toPrimitive]() { return states.get(this).id; }
    [Symbol.dispose]() { cancel(this); }
  }
  class Immediate {
    constructor(callback, args) {
      const state = {id: nextId++, callback, delay: 0, args, interval: false,
        immediate: true, referenced: true, nativeId: undefined};
      states.set(this, state);
      start(this, state);
    }
    ref() { setReferenced(states.get(this), true); return this; }
    unref() { setReferenced(states.get(this), false); return this; }
    hasRef() { return states.get(this).referenced; }
    [Symbol.dispose]() { cancel(this); }
  }
  function validate(callback) {
    if (typeof callback !== 'function') {
      throw Object.assign(new TypeError('The "callback" argument must be a function'),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
  }
  function normalizeDelay(delay) {
    const value = delay === undefined ? 1 : +delay;
    if (value > 2147483647) host.warn?.(`${value} does not fit into a 32-bit signed integer.\nTimeout duration was set to 1.`, 'TimeoutOverflowWarning');
    return value >= 1 && value <= 2147483647 ? Math.trunc(value) : 1;
  }
  function clearTimeout(handle) {
    if (typeof handle === 'number') handle = active.get(handle);
    else if (typeof handle === 'string' && String(+handle) === handle) handle = active.get(+handle);
    if (states.get(handle)?.immediate === false) cancel(handle);
  }
  return {
    setTimeout(callback, delay, ...args) {
      validate(callback);
      return new Timeout(callback, normalizeDelay(delay), args, false);
    },
    setInterval(callback, delay, ...args) {
      validate(callback);
      return new Timeout(callback, normalizeDelay(delay), args, true);
    },
    clearTimeout,
    clearInterval: clearTimeout,
    setImmediate(callback, ...args) {
      validate(callback);
      return new Immediate(callback, args);
    },
    clearImmediate(handle) {
      if (states.get(handle)?.immediate === true) cancel(handle);
    },
  };
}
