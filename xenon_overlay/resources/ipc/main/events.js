  if (typeof queueMicrotask !== 'function') {
    globalThis.queueMicrotask = callback => Promise.resolve().then(callback);
  }

  function EventEmitter() {
    if (!(this instanceof EventEmitter)) {
      return new EventEmitter();
    }
    this._events = new Map();
    this._maxListeners = undefined;
  }
  // Native addon wrappers can copy these methods without calling the
  // constructor. Keep their listener state local to each receiver.
  function eventMap(emitter) {
    if (!Object.prototype.hasOwnProperty.call(emitter, '_events') ||
        !(emitter._events instanceof Map)) {
      emitter._events = new Map();
    }
    return emitter._events;
  }
  function validateListener(listener) {
    if (typeof listener !== 'function') {
      throw new TypeError('The "listener" argument must be a function');
    }
  }
  EventEmitter.prototype.on = function(name, listener) {
    validateListener(listener);
    if (name !== 'newListener')
      this.emit('newListener', name, listener);
    const listeners = eventMap(this).get(name) || [];
    listeners.push(listener);
    eventMap(this).set(name, listeners);
    return this;
  };
  EventEmitter.prototype.addListener = EventEmitter.prototype.on;
  EventEmitter.prototype.once = function(name, listener) {
    validateListener(listener);
    const wrapped = (...args) => {
      this.removeListener(name, wrapped);
      return listener.apply(this, args);
    };
    wrapped.listener = listener;
    if (name !== 'newListener')
      this.emit('newListener', name, listener);
    const listeners = eventMap(this).get(name) || [];
    listeners.push(wrapped);
    eventMap(this).set(name, listeners);
    return this;
  };
  EventEmitter.prototype.emit = function(name, ...args) {
    const listeners = eventMap(this).get(name);
    if (!listeners || listeners.length === 0) {
      if (name === 'error') {
        const error = args[0];
        throw error instanceof Error ?
            error :
            new Error(
                'Unhandled error.' +
                (error === undefined ? '' : ` (${error})`));
      }
      return false;
    }
    for (const listener of [...listeners]) listener.apply(this, args);
    return true;
  };
  EventEmitter.prototype.removeListener = function(name, listener) {
    validateListener(listener);
    const listeners = eventMap(this).get(name);
    if (!listeners)
      return this;
    let index = -1;
    for (let i = listeners.length - 1; i >= 0; --i) {
      if (listeners[i] === listener || listeners[i].listener === listener) {
        index = i;
        break;
      }
    }
    if (index < 0)
      return this;
    const removed = listeners[index].listener || listeners[index];
    const filtered = listeners.slice();
    filtered.splice(index, 1);
    if (filtered.length)
      eventMap(this).set(name, filtered);
    else
      eventMap(this).delete(name);
    if (name !== 'removeListener')
      this.emit('removeListener', name, removed);
    return this;
  };
  EventEmitter.prototype.off = EventEmitter.prototype.removeListener;
  EventEmitter.prototype.removeAllListeners = function(name) {
    if (name === undefined) {
      if (!eventMap(this).has('removeListener')) {
        eventMap(this).clear();
        return this;
      }
      for (const eventName of [...eventMap(this).keys()]) {
        if (eventName !== 'removeListener')
          this.removeAllListeners(eventName);
      }
      this.removeAllListeners('removeListener');
      return this;
    }
    const listeners = eventMap(this).get(name);
    if (!listeners)
      return this;
    for (let i = listeners.length - 1; i >= 0; --i) {
      this.removeListener(name, listeners[i]);
    }
    return this;
  };
  EventEmitter.prototype.listeners = function(name) {
    return (eventMap(this).get(name) || [])
        .map(listener => listener.listener || listener);
  };
  EventEmitter.prototype.rawListeners = function(name) {
    return [...(eventMap(this).get(name) || [])];
  };
  EventEmitter.prototype.listenerCount = function(name, listener) {
    const listeners = eventMap(this).get(name) || [];
    if (listener === undefined)
      return listeners.length;
    validateListener(listener);
    return listeners
        .filter(item => item === listener || item.listener === listener)
        .length;
  };
  EventEmitter.prototype.prependListener = function(name, listener) {
    validateListener(listener);
    if (name !== 'newListener')
      this.emit('newListener', name, listener);
    const listeners = eventMap(this).get(name) || [];
    listeners.unshift(listener);
    eventMap(this).set(name, listeners);
    return this;
  };
  EventEmitter.prototype.prependOnceListener = function(name, listener) {
    validateListener(listener);
    const wrapped = (...args) => {
      this.removeListener(name, wrapped);
      return listener.apply(this, args);
    };
    wrapped.listener = listener;
    if (name !== 'newListener')
      this.emit('newListener', name, listener);
    const listeners = eventMap(this).get(name) || [];
    listeners.unshift(wrapped);
    eventMap(this).set(name, listeners);
    return this;
  };
  EventEmitter.prototype.eventNames = function() {
    return [...eventMap(this).keys()];
  };
  EventEmitter.prototype.setMaxListeners = function(n) {
    if (typeof n !== 'number' || n < 0 || Number.isNaN(n)) {
      throw new RangeError('The value of "n" is out of range');
    }
    this._maxListeners = n;
    return this;
  };
  EventEmitter.prototype.getMaxListeners = function() {
    return this._maxListeners === undefined ? EventEmitter.defaultMaxListeners :
                                              this._maxListeners;
  };
  EventEmitter.listenerCount = (emitter, name) => emitter.listenerCount(name);
  EventEmitter.EventEmitter = EventEmitter;
  EventEmitter.default = EventEmitter;
  EventEmitter.defaultMaxListeners = 10;
