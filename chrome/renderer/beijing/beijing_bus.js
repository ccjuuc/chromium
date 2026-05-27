// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// EventEmitter-style adapter for window.beijing (Mojo publish/subscribe).
(function(global) {
  'use strict';

  const EventEmitter = global.EventEmitter3;
  if (!EventEmitter) {
    throw new Error('beijing_bus.js requires eventemitter3.js');
  }

  class BeijingBus {
    constructor(native = global.beijing) {
      if (!native) {
        throw new Error('BeijingBus requires window.beijing');
      }
      this._native = native;
      this._emitter = new EventEmitter();
      this._remoteTopics = new Set();
    }

    on(topic, listener, context) {
      this._ensureRemoteTopic(topic);
      return this._emitter.on(topic, listener, context);
    }

    once(topic, listener, context) {
      this._ensureRemoteTopic(topic);
      return this._emitter.once(topic, listener, context);
    }

    off(topic, listener, context) {
      this._emitter.off(topic, listener, context);
      if (this._emitter.listenerCount(topic) === 0) {
        this._teardownRemoteTopic(topic);
      }
      return this;
    }

    removeAllListeners(topic) {
      if (topic !== undefined) {
        this._emitter.removeAllListeners(topic);
        this._teardownRemoteTopic(topic);
      } else {
        const topics = this._emitter.eventNames();
        this._emitter.removeAllListeners();
        topics.forEach((t) => this._teardownRemoteTopic(t));
      }
      return this;
    }

    listenerCount(topic) {
      return this._emitter.listenerCount(topic);
    }

    emitLocal(topic, ...args) {
      return this._emitter.emit(topic, ...args);
    }

    publish(topic, payload) {
      this._native.publish(topic, payload);
    }

    _ensureRemoteTopic(topic) {
      if (this._remoteTopics.has(topic)) {
        return;
      }

      this._native.subscribe(topic, (payload) => {
        this._emitter.emit(topic, payload);
      });
      this._remoteTopics.add(topic);
    }

    _teardownRemoteTopic(topic) {
      if (!this._remoteTopics.has(topic)) {
        return;
      }
      if (typeof this._native.unsubscribe === 'function') {
        this._native.unsubscribe(topic);
      }
      this._remoteTopics.delete(topic);
    }
  }

  function createBeijingBus() {
    if (!global.beijing) {
      return null;
    }
    return new BeijingBus(global.beijing);
  }

  global.BeijingBus = BeijingBus;
  global.beijingBus = createBeijingBus();
})(window);
