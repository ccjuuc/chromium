// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(() => {
  'use strict';

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

  // Promise hooks are scoped to this V8 context. Blink owns the isolate's
  // continuation-preserved embedder data, so never overwrite that slot.
  let currentAsyncContext;
  let asyncContextHooksInstalled = false;
  const promiseAsyncContexts = new WeakMap();
  const asyncContextStack = [];
  const installAsyncContextHooks = globalThis.__xenonInstallAsyncContextHooks;
  function unsupportedAsyncHooks(method) {
    const error = new Error('async_hooks.' + method + ' is not supported');
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }
  function ensureAsyncContextHooks() {
    if (asyncContextHooksInstalled) return;
    if (typeof installAsyncContextHooks !== 'function') {
      unsupportedAsyncHooks('AsyncLocalStorage (native Promise hooks unavailable)');
    }
    installAsyncContextHooks(
        promise => {
          if (currentAsyncContext !== undefined) {
            promiseAsyncContexts.set(promise, currentAsyncContext);
          }
        },
        promise => {
          asyncContextStack.push(currentAsyncContext);
          currentAsyncContext = promiseAsyncContexts.get(promise);
        },
        () => { currentAsyncContext = asyncContextStack.pop(); });
    asyncContextHooksInstalled = true;
  }
  function runWithAsyncContext(context, callback, thisArg, args) {
    const previous = currentAsyncContext;
    currentAsyncContext = context;
    try {
      return Reflect.apply(callback, thisArg, args);
    } finally {
      currentAsyncContext = previous;
    }
  }
  function captureAsyncCallback(callback) {
    const context = currentAsyncContext;
    return function(...args) {
      return runWithAsyncContext(context, callback, this, args);
    };
  }
  // Browser timers and microtasks are host tasks, not Promise reactions. Keep
  // their registration context explicitly; ids, cancellation and arguments
  // continue to come from the real host implementation.
  for (const name of ['setTimeout', 'setInterval', 'setImmediate', 'queueMicrotask']) {
    const schedule = globalThis[name];
    if (typeof schedule !== 'function') continue;
    globalThis[name] = function(callback, ...args) {
      const wrapped = typeof callback === 'function' && asyncContextHooksInstalled ?
          captureAsyncCallback(callback) : callback;
      return Reflect.apply(schedule, this, [wrapped, ...args]);
    };
  }
  class AsyncLocalStorage {
    constructor(options = {}) {
      if (options === null || typeof options !== 'object') {
        const error = new TypeError('The "options" argument must be an object');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (options.onPropagate !== undefined) {
        unsupportedAsyncHooks('AsyncLocalStorage onPropagate');
      }
      this.name = options.name === undefined ? '' : String(options.name);
      this.defaultValue_ = options.defaultValue;
      this.key_ = undefined;
    }
    disable() {
      if (this.key_ !== undefined && currentAsyncContext?.has(this.key_)) {
        currentAsyncContext = new Map(currentAsyncContext);
        currentAsyncContext.delete(this.key_);
      }
      // Existing Promise frames may outlive disable(), but can no longer return
      // this instance's old store. No registry strongly retains ALS instances.
      this.key_ = undefined;
    }
    enterWith(store) {
      ensureAsyncContextHooks();
      this.key_ ??= Symbol();
      currentAsyncContext = new Map(currentAsyncContext);
      currentAsyncContext.set(this.key_, store);
    }
    getStore() {
      return this.key_ !== undefined && currentAsyncContext?.has(this.key_) ?
          currentAsyncContext.get(this.key_) : this.defaultValue_;
    }
    run(store, callback, ...args) {
      validateListener(callback);
      ensureAsyncContextHooks();
      this.key_ ??= Symbol();
      const context = new Map(currentAsyncContext);
      context.set(this.key_, store);
      return runWithAsyncContext(context, callback, undefined, args);
    }
    exit(callback, ...args) {
      return this.run(undefined, callback, ...args);
    }
    static snapshot() {
      ensureAsyncContextHooks();
      const context = currentAsyncContext;
      return (callback, ...args) => {
        validateListener(callback);
        return runWithAsyncContext(context, callback, undefined, args);
      };
    }
    static bind(callback) {
      validateListener(callback);
      ensureAsyncContextHooks();
      return captureAsyncCallback(callback);
    }
  }
  class AsyncResource {
    constructor(type, options) {
      if (typeof type !== 'string') {
        const error = new TypeError('The "type" argument must be a string');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (options !== undefined) {
        unsupportedAsyncHooks('AsyncResource options');
      }
      this.type = type;
      this.snapshot_ = AsyncLocalStorage.snapshot();
    }
    runInAsyncScope(callback, thisArg, ...args) {
      validateListener(callback);
      return this.snapshot_(() => Reflect.apply(callback, thisArg, args));
    }
    bind(callback, thisArg) {
      validateListener(callback);
      const resource = this;
      const hasThisArg = arguments.length > 1;
      return function(...args) {
        return resource.runInAsyncScope(callback, hasThisArg ? thisArg : this, ...args);
      };
    }
    emitDestroy() { return unsupportedAsyncHooks('AsyncResource.emitDestroy'); }
    asyncId() { return unsupportedAsyncHooks('AsyncResource.asyncId'); }
    triggerAsyncId() { return unsupportedAsyncHooks('AsyncResource.triggerAsyncId'); }
    static bind(callback, type = 'bound-anonymous-fn', thisArg) {
      const resource = new AsyncResource(type);
      return arguments.length > 2 ? resource.bind(callback, thisArg) :
          resource.bind(callback);
    }
  }
  const asyncHooksModule = {
    AsyncLocalStorage,
    AsyncResource,
    createHook: () => unsupportedAsyncHooks('createHook'),
    executionAsyncId: () => unsupportedAsyncHooks('executionAsyncId'),
    triggerAsyncId: () => unsupportedAsyncHooks('triggerAsyncId'),
    executionAsyncResource: () => unsupportedAsyncHooks('executionAsyncResource'),
  };

  function decodeFormComponent(value) {
    try {
      return decodeURIComponent(String(value).replace(/\+/g, ' '));
    } catch {
      return String(value).replace(/\+/g, ' ');
    }
  }
  function encodeFormComponent(value) {
    return encodeURIComponent(String(value))
        .replace(/%20/g, '+')
        .replace(/!/g, '%21')
        .replace(/'/g, '%27')
        .replace(/\(/g, '%28')
        .replace(/\)/g, '%29')
        .replace(/~/g, '%7E');
  }
  class URLSearchParams {
    constructor(init = '', onUpdate = null) {
      this._pairs = [];
      this._onUpdate = typeof onUpdate === 'function' ? onUpdate : null;
      this._replace(init);
    }
    _replace(init) {
      this._pairs = [];
      if (init instanceof URLSearchParams) {
        this._pairs = init._pairs.map(pair => [...pair]);
      } else if (typeof init === 'string') {
        const value = init.startsWith('?') ? init.slice(1) : init;
        for (const field of value.split('&')) {
          if (!field) continue;
          const separator = field.indexOf('=');
          const name = separator < 0 ? field : field.slice(0, separator);
          const entryValue = separator < 0 ? '' : field.slice(separator + 1);
          this._pairs.push([
            decodeFormComponent(name), decodeFormComponent(entryValue)]);
        }
      } else if (init && typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) {
          if (!pair || typeof pair[Symbol.iterator] !== 'function') {
            throw new TypeError('Each query pair must be iterable');
          }
          const values = [...pair];
          if (values.length !== 2) {
            throw new TypeError('Each query pair must contain two items');
          }
          this._pairs.push([String(values[0]), String(values[1])]);
        }
      } else if (init && typeof init === 'object') {
        for (const [name, value] of Object.entries(init)) {
          this._pairs.push([String(name), String(value)]);
        }
      }
    }
    _changed() {
      if (this._onUpdate) this._onUpdate(this.toString());
    }
    append(name, value) {
      this._pairs.push([String(name), String(value)]);
      this._changed();
    }
    delete(name, value) {
      const key = String(name);
      const hasValue = arguments.length > 1;
      const expected = String(value);
      this._pairs = this._pairs.filter(
          pair => pair[0] !== key || (hasValue && pair[1] !== expected));
      this._changed();
    }
    get(name) {
      const key = String(name);
      const pair = this._pairs.find(item => item[0] === key);
      return pair ? pair[1] : null;
    }
    getAll(name) {
      const key = String(name);
      return this._pairs.filter(item => item[0] === key).map(item => item[1]);
    }
    has(name, value) {
      const key = String(name);
      const hasValue = arguments.length > 1;
      const expected = String(value);
      return this._pairs.some(
          pair => pair[0] === key && (!hasValue || pair[1] === expected));
    }
    set(name, value) {
      const key = String(name);
      const entryValue = String(value);
      const first = this._pairs.findIndex(item => item[0] === key);
      if (first < 0) {
        this._pairs.push([key, entryValue]);
      } else {
        this._pairs[first][1] = entryValue;
        this._pairs = this._pairs.filter(
            (item, index) => item[0] !== key || index === first);
      }
      this._changed();
    }
    sort() {
      this._pairs.sort((left, right) => left[0].localeCompare(right[0]));
      this._changed();
    }
    entries() { return this._pairs.map(pair => [...pair])[Symbol.iterator](); }
    keys() { return this._pairs.map(pair => pair[0])[Symbol.iterator](); }
    values() { return this._pairs.map(pair => pair[1])[Symbol.iterator](); }
    forEach(callback, thisArg) {
      for (const [name, value] of this._pairs) {
        callback.call(thisArg, value, name, this);
      }
    }
    get size() { return this._pairs.length; }
    toString() {
      return this._pairs.map(pair =>
        `${encodeFormComponent(pair[0])}=${encodeFormComponent(pair[1])}`
      ).join('&');
    }
    [Symbol.iterator]() { return this.entries(); }
  }
  globalThis.URLSearchParams = URLSearchParams;

  class URL {
    constructor(input, base) {
      let href = String(input ?? '');
      if (base && !/^[A-Za-z][A-Za-z0-9+.-]*:/.test(href)) {
        const resolved = new URL(base);
        const dir = resolved.pathname.replace(/\/[^/]*$/, '/') || '/';
        href = resolved.protocol + '//' + resolved.host +
            (href.startsWith('/') ? href : dir + href);
      }
      href = href.replace(/\\/g, '/');
      if (/^file:\/\/[A-Za-z]:/.test(href)) {
        href = 'file:///' + href.slice('file://'.length);
      }
      this.username = '';
      this.password = '';
      this.hostname = '';
      this.port = '';
      this.pathname = '/';
      this._search = '';
      this._hash = '';
      this.searchParams = new URLSearchParams('', value => {
        this._search = value ? `?${value}` : '';
        this._recompose();
      });
      this._assign(href);
    }
    _assign(href) {
      const hashIdx = href.indexOf('#');
      if (hashIdx >= 0) {
        this._hash = href.slice(hashIdx);
        href = href.slice(0, hashIdx);
      } else {
        this._hash = '';
      }
      const searchIdx = href.indexOf('?');
      if (searchIdx >= 0) {
        this._search = href.slice(searchIdx);
        href = href.slice(0, searchIdx);
      } else {
        this._search = '';
      }
      this.searchParams._replace(this._search);
      const protoIdx = href.indexOf(':');
      this.protocol = protoIdx >= 0 ? href.slice(0, protoIdx + 1) : '';
      let rest = protoIdx >= 0 ? href.slice(protoIdx + 1) : href;
      this.hostname = '';
      this.port = '';
      if (rest.startsWith('//')) {
        rest = rest.slice(2);
        const slash = rest.indexOf('/');
        const authority = slash >= 0 ? rest.slice(0, slash) : rest;
        this.pathname = slash >= 0 ? rest.slice(slash) : '/';
        const at = authority.lastIndexOf('@');
        const hostport = at >= 0 ? authority.slice(at + 1) : authority;
        const colon = hostport.lastIndexOf(':');
        const bracket = hostport.indexOf(']');
        if (hostport.startsWith('[') && bracket >= 0) {
          this.hostname = hostport.slice(0, bracket + 1);
          this.port = hostport.slice(bracket + 2);
        } else if (colon >= 0) {
          this.hostname = hostport.slice(0, colon);
          this.port = hostport.slice(colon + 1);
        } else {
          this.hostname = hostport;
        }
      } else {
        this.pathname = rest || '/';
      }
      this._recompose();
    }
    get host() {
      return this.port ? `${this.hostname}:${this.port}` : this.hostname;
    }
    set host(value) {
      const text = String(value || '');
      const colon = text.lastIndexOf(':');
      if (colon >= 0 && !text.startsWith('[')) {
        this.hostname = text.slice(0, colon);
        this.port = text.slice(colon + 1);
      } else {
        this.hostname = text;
        this.port = '';
      }
      this._recompose();
    }
    get search() { return this._search; }
    set search(value) {
      const text = String(value || '');
      this._search = !text || text.startsWith('?') ? text : `?${text}`;
      this.searchParams._replace(this._search);
      this._recompose();
    }
    get hash() { return this._hash; }
    set hash(value) {
      const text = String(value || '');
      this._hash = !text || text.startsWith('#') ? text : `#${text}`;
      this._recompose();
    }
    get origin() { return `${this.protocol}//${this.host}`; }
    _recompose() {
      const hasAuthority = this.hostname !== '' || this.protocol === 'file:' ||
          this.protocol === 'chrome:' || this.protocol === 'http:' ||
          this.protocol === 'https:';
      let path = this.pathname || '';
      if (hasAuthority && path && !path.startsWith('/')) {
        path = `/${path}`;
      }
      this.href = `${this.protocol}${hasAuthority ? '//' + this.host : ''}${path}${this._search}${this._hash}`;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
  }
  globalThis.URL = URL;

  const invokeHandlers = new Map();
  const ipcMain = new EventEmitter();
  ipcMain.handle = (channel, handler) => {
    validateListener(handler);
    if (invokeHandlers.has(channel)) {
      throw new Error(`Attempted to register a second handler for '${channel}'`);
    }
    invokeHandlers.set(channel, handler);
  };
  ipcMain.handleOnce = (channel, handler) => {
    ipcMain.handle(channel, (event, ...args) => {
      ipcMain.removeHandler(channel);
      return handler(event, ...args);
    });
  };
  ipcMain.removeHandler = channel => invokeHandlers.delete(channel);

  function createEvent(sender) {
    const event = {
      processId: sender.processId,
      frameId: sender.frameId,
      sender: getSenderWebContents(sender),
      senderFrame: {
        processId: sender.processId,
        routingId: sender.frameId,
      },
      ports: [],
      reply(channel, ...args) {
        __xenonSendToRenderer(sender.endpointId, channel, args);
      },
      returnValue: undefined,
    };
    return event;
  }

  let dispatchXenonNet = null;
  globalThis.__xenonDispatchSend = (sender, channel, args) => {
    const list = Array.isArray(args) ? args : (args === undefined ? [] : [args]);
    if (channel === '__xenon:preload-error') {
      const event = createEvent(sender);
      const error = new Error(String(list[1] || 'Preload script failed'));
      event.sender.emit('preload-error', {sender: event.sender}, list[0], error);
      return;
    }
    if (typeof channel === 'string' && channel.startsWith('__xenon:net:') &&
        dispatchXenonNet && dispatchXenonNet(channel, list[0], sender)) {
      return;
    }
    ipcMain.emit(channel, createEvent(sender), ...list);
  };
  globalThis.__xenonDispatchInvoke = (sender, channel, args) => {
    const handler = invokeHandlers.get(channel);
    if (!handler) {
      return Promise.reject(new Error(`No handler registered for '${channel}'`));
    }
    try {
      return Promise.resolve(handler(createEvent(sender), ...args));
    } catch (error) {
      return Promise.reject(error);
    }
  };
  globalThis.__xenonDispatchSync = (sender, channel, args) => {
    const event = createEvent(sender);
    if (channel === '__xenon:renderer-web-preferences') {
      const endpoint = webContentsState.get(event.sender).endpoints.get(sender.endpointId);
      if (endpoint && typeof args[0] === 'boolean') endpoint.isMainFrame = args[0];
      return event.sender.getLastWebPreferences();
    }
    ipcMain.emit(channel, event, ...args);
    if (event.returnValue && typeof event.returnValue.then === 'function') {
      throw new Error(`Synchronous handler for '${channel}' returned a Promise`);
    }
    return event.returnValue;
  };

  const posix = {
    sep: '/',
    delimiter: ':',
    isAbsolute(path) {
      return typeof path === 'string' && path.startsWith('/');
    },
    normalize(path) {
      path = String(path || '.');
      if (!path) return '.';
      const isAbs = path.startsWith('/');
      const trailingSlash = path.endsWith('/') && path.length > 1;
      const parts = [];
      for (const segment of path.split('/')) {
        if (!segment || segment === '.') continue;
        if (segment === '..') {
          if (parts.length && parts[parts.length - 1] !== '..') {
            parts.pop();
          } else if (!isAbs) {
            parts.push('..');
          }
        } else {
          parts.push(segment);
        }
      }
      let result = (isAbs ? '/' : '') + parts.join('/');
      if (!result) return isAbs ? '/' : '.';
      if (trailingSlash && !result.endsWith('/')) result += '/';
      return result;
    },
    join(...paths) {
      const joined = paths.filter(p => typeof p === 'string' && p.length > 0).join('/');
      return posix.normalize(joined || '.');
    },
    resolve(...paths) {
      let resolved = '';
      for (let i = paths.length - 1; i >= 0; --i) {
        const p = String(paths[i] || '');
        if (!p) continue;
        resolved = resolved ? p + '/' + resolved : p;
        if (posix.isAbsolute(p)) break;
      }
      if (!posix.isAbsolute(resolved)) {
        resolved = '/' + resolved;
      }
      return posix.normalize(resolved);
    },
    dirname(path) {
      path = posix.normalize(path);
      if (path === '/') return '/';
      const idx = path.lastIndexOf('/');
      if (idx < 0) return '.';
      if (idx === 0) return '/';
      return path.slice(0, idx);
    },
    basename(path, ext) {
      path = String(path || '');
      const idx = path.lastIndexOf('/');
      let base = idx < 0 ? path : path.slice(idx + 1);
      if (ext && base.endsWith(String(ext))) {
        base = base.slice(0, -String(ext).length);
      }
      return base;
    },
    extname(path) {
      const base = posix.basename(path);
      const idx = base.lastIndexOf('.');
      return idx <= 0 ? '' : base.slice(idx);
    },
    parse(path) {
      path = String(path || '');
      const isAbs = posix.isAbsolute(path);
      const root = isAbs ? '/' : '';
      const base = posix.basename(path);
      const ext = posix.extname(path);
      const dir = posix.dirname(path);
      const name = ext ? base.slice(0, -ext.length) : base;
      return { root, dir, base, ext, name };
    },
    format(obj) {
      if (!obj || typeof obj !== 'object') return '';
      const dir = obj.dir || obj.root || '';
      const base = obj.base || `${obj.name || ''}${obj.ext || ''}`;
      if (!dir) return base;
      return dir.endsWith('/') ? dir + base : dir + '/' + base;
    },
    relative(from, to) {
      const fromParts = posix.resolve(from).split('/').filter(Boolean);
      const toParts = posix.resolve(to).split('/').filter(Boolean);
      let common = 0;
      while (common < fromParts.length && common < toParts.length &&
             fromParts[common] === toParts[common]) {
        ++common;
      }
      const up = new Array(fromParts.length - common).fill('..');
      return [...up, ...toParts.slice(common)].join('/') || '.';
    },
    toNamespacedPath: p => String(p),
  };

  const win32 = {
    sep: '\\',
    delimiter: ';',
    isAbsolute(path) {
      path = String(path || '');
      if (!path) return false;
      return /^[A-Za-z]:[\\/]|^[\\/]{2}[^\\/]+[\\/][^\\/]+|^[\\/]/.test(path);
    },
    normalize(path) {
      path = String(path || '.').replace(/\//g, '\\');
      if (!path) return '.';
      const isUnc = /^\\\\/.test(path);
      const driveMatch = path.match(/^[A-Za-z]:/);
      let device = '';
      if (driveMatch) {
        device = driveMatch[0];
        path = path.slice(2);
      } else if (isUnc) {
        const match = path.match(/^(\\\\[^\\]+\\[^\\]+)/);
        if (match) {
          device = match[0];
          path = path.slice(device.length);
        }
      }
      const isAbs = path.startsWith('\\');
      const parts = [];
      for (const segment of path.split('\\')) {
        if (!segment || segment === '.') continue;
        if (segment === '..') {
          if (parts.length && parts[parts.length - 1] !== '..') {
            parts.pop();
          } else if (!isAbs && !device) {
            parts.push('..');
          }
        } else {
          parts.push(segment);
        }
      }
      let result = device;
      if (isAbs) result += '\\';
      result += parts.join('\\');
      if (!result) return isAbs ? '\\' : (device ? device + (isAbs ? '\\' : '.') : '.');
      return result;
    },
    join(...paths) {
      const joined = paths.filter(p => typeof p === 'string' && p.length > 0)
                          .map(p => String(p).replace(/\//g, '\\'))
                          .join('\\');
      return win32.normalize(joined || '.');
    },
    resolve(...paths) {
      let resolvedDevice = '';
      let resolvedTail = '';
      let resolvedAbsolute = false;
      for (let i = paths.length - 1; i >= 0; --i) {
        let p = String(paths[i] || '').replace(/\//g, '\\');
        if (!p) continue;
        let device = '';
        const driveMatch = p.match(/^[A-Za-z]:/);
        if (driveMatch) {
          device = driveMatch[0];
          p = p.slice(2);
        } else if (/^\\\\/.test(p)) {
          const match = p.match(/^(\\\\[^\\]+\\[^\\]+)/);
          if (match) {
            device = match[0];
            p = p.slice(device.length);
          }
        }
        const isAbs = p.startsWith('\\');
        if (device && resolvedDevice && device.toLowerCase() !== resolvedDevice.toLowerCase()) {
          continue;
        }
        if (!resolvedDevice && device) {
          resolvedDevice = device;
        }
        if (!resolvedAbsolute) {
          resolvedTail = p + (resolvedTail ? '\\' + resolvedTail : '');
          if (isAbs) {
            resolvedAbsolute = true;
          }
        }
        if (resolvedDevice && resolvedAbsolute) break;
      }
      if (!resolvedAbsolute) {
        const cwd = String(__xenonAppPath || '.').replace(/\//g, '\\');
        return win32.normalize(cwd + '\\' + resolvedTail);
      }
      return win32.normalize(resolvedDevice + resolvedTail);
    },
    dirname(path) {
      path = win32.normalize(path);
      const rootMatch = path.match(/^([A-Za-z]:\\|\\\\[^\\]+\\[^\\]+\\|\\|[A-Za-z]:)/);
      const root = rootMatch ? rootMatch[0] : '';
      const tail = root ? path.slice(root.length) : path;
      const idx = tail.lastIndexOf('\\');
      if (idx < 0) return root || '.';
      return root + tail.slice(0, idx);
    },
    basename(path, ext) {
      path = String(path || '').replace(/\//g, '\\');
      const idx = path.lastIndexOf('\\');
      let base = idx < 0 ? path : path.slice(idx + 1);
      if (base.endsWith(':') && /^[A-Za-z]:$/.test(base)) base = '';
      if (ext && base.endsWith(String(ext))) {
        base = base.slice(0, -String(ext).length);
      }
      return base;
    },
    extname(path) {
      const base = win32.basename(path);
      const idx = base.lastIndexOf('.');
      return idx <= 0 ? '' : base.slice(idx);
    },
    parse(path) {
      path = String(path || '').replace(/\//g, '\\');
      const rootMatch = path.match(/^([A-Za-z]:\\|\\\\[^\\]+\\[^\\]+\\|\\|[A-Za-z]:)/);
      const root = rootMatch ? rootMatch[0] : '';
      const dir = win32.dirname(path);
      const base = win32.basename(path);
      const ext = win32.extname(path);
      const name = ext ? base.slice(0, -ext.length) : base;
      return { root, dir, base, ext, name };
    },
    format(obj) {
      if (!obj || typeof obj !== 'object') return '';
      const dir = obj.dir || obj.root || '';
      const base = obj.base || `${obj.name || ''}${obj.ext || ''}`;
      if (!dir) return base;
      return dir.endsWith('\\') ? dir + base : dir + '\\' + base;
    },
    relative(from, to) {
      const fromResolved = win32.resolve(from);
      const toResolved = win32.resolve(to);
      const fromParts = fromResolved.split('\\').filter(Boolean);
      const toParts = toResolved.split('\\').filter(Boolean);
      let common = 0;
      while (common < fromParts.length && common < toParts.length &&
             fromParts[common].toLowerCase() === toParts[common].toLowerCase()) {
        ++common;
      }
      const up = new Array(fromParts.length - common).fill('..');
      return [...up, ...toParts.slice(common)].join('\\') || '.';
    },
    toNamespacedPath: p => String(p),
  };

  posix.win32 = win32;
  posix.posix = posix;
  win32.win32 = win32;
  win32.posix = posix;

  const isWindows = __xenonPlatform === 'win32';
  const pathModule = isWindows ? win32 : posix;

  const osPlatform = __xenonPlatform;
  const osArch = __xenonArch;
  const osEndianness = typeof __xenonEndianness === 'string' ? __xenonEndianness : '';
  const osNativeCall = request => {
    if (typeof __xenonOsCall !== 'function') {
      throw Object.assign(new Error('Native OS queries are unavailable'),
                          {code: 'ERR_NOT_SUPPORTED'});
    }
    return __xenonOsCall(request);
  };
  function osQuery(method) {
    try {
      return osNativeCall({method});
    } catch (error) {
      // Private IPC preserves the native message; restore its Node error code.
      const match = /^(ERR_[A-Z_]+|E[A-Z0-9_]+):/.exec(String(error?.message || error));
      if (match && !error.code) error.code = match[1];
      throw error;
    }
  }

  function osUserInfo(options) {
    // Node treats absent/unrecognized encodings as UTF-8.
    const requested = options?.encoding;
    const encoding = typeof requested === 'string' ? requested.toLowerCase() : 'utf8';
    const result = osQuery('userInfo');
    if (!['buffer', 'hex', 'base64', 'base64url', 'ascii', 'latin1', 'binary',
          'utf16le', 'utf-16le', 'ucs2', 'ucs-2'].includes(encoding)) {
      return result;
    }
    for (const key of ['username', 'homedir', 'shell']) {
      if (result[key] === null) continue;
      const bytes = Buffer.from(result[key], 'utf8');
      if (encoding === 'buffer') {
        result[key] = bytes;
      } else if (['hex', 'base64', 'base64url'].includes(encoding)) {
        result[key] = bytes.toString(encoding);
      } else if (['ascii', 'latin1', 'binary'].includes(encoding)) {
        result[key] = Array.from(bytes, byte =>
            String.fromCharCode(encoding === 'ascii' ? byte & 0x7f : byte)).join('');
      } else if (['utf16le', 'utf-16le', 'ucs2', 'ucs-2'].includes(encoding)) {
        // Decode pairs directly to preserve lone UTF-16 code units, as Buffer does.
        let value = '';
        for (let i = 0; i + 1 < bytes.length; i += 2) {
          value += String.fromCharCode(bytes[i] | (bytes[i + 1] << 8));
        }
        result[key] = value;
      }
    }
    return result;
  }

  const osModule = {
    platform: () => osPlatform,
    arch: () => osArch,
    endianness() {
      if (osEndianness !== 'LE' && osEndianness !== 'BE') {
        throw Object.assign(new Error('OS byte order metadata is unavailable'),
                            {code: 'ERR_NOT_SUPPORTED'});
      }
      return osEndianness;
    },
    homedir() {
      const value = globalThis.process.env[osPlatform === 'win32' ? 'USERPROFILE' : 'HOME'];
      return value === undefined ? osQuery('homedir') : String(value);
    },
    tmpdir() {
      const env = globalThis.process.env;
      const value = osPlatform === 'win32' ? env.TEMP || env.TMP :
          env.TMPDIR || env.TMP || env.TEMP;
      const directory = value ? String(value) : osQuery('tmpdir');
      if (osPlatform === 'win32') {
        return directory.length > 1 && directory.endsWith('\\') &&
            !directory.endsWith(':\\') ? directory.slice(0, -1) : directory;
      }
      return directory.length > 1 && directory.endsWith('/') ?
          directory.slice(0, -1) : directory;
    },
    userInfo: osUserInfo,
    EOL: osPlatform === 'win32' ? '\r\n' : '\n',
    devNull: osPlatform === 'win32' ? '\\\\.\\nul' : '/dev/null',
  };
  // Query mutable system state on every call. Requiring os never enumerates
  // CPUs, users or interfaces, and never adds a synchronous startup round trip.
  for (const method of ['type', 'release', 'version', 'machine', 'hostname',
                        'cpus', 'totalmem', 'freemem', 'uptime', 'loadavg',
                        'networkInterfaces', 'availableParallelism',
                        'getPriority', 'setPriority']) {
    osModule[method] = () => osQuery(method);
  }

  let appReady = false;
  let resolveAppReady;
  const appReadyPromise = new Promise(resolve => { resolveAppReady = resolve; });
  const app = new EventEmitter();
  let appExitState = 'running';
  let appQuitPhase = '';
  let appQuitAdvancing = false;
  let beforeQuitEmitted = false;
  let willQuitEmitted = false;
  let quitEmitted = false;

  function requireAppExitHost() {
    if (typeof globalThis.__xenonExitApp !== 'function' ||
        (typeof globalThis.__xenonCanExitApp === 'function' &&
         !globalThis.__xenonCanExitApp())) {
      return electronUnsupported('app exit');
    }
    return globalThis.__xenonExitApp;
  }

  function stopAppEvents() {
    appEventsStopped = true;
    pendingAppEvents.length = 0;
  }

  function cancelAppQuit() {
    if (appExitState !== 'quitting') return;
    appExitState = 'running';
    appQuitPhase = '';
    beforeQuitEmitted = false;
    willQuitEmitted = false;
    appEventsStopped = false;
  }

  function finishAppExit(code, exitHost) {
    if (quitEmitted) return;
    quitEmitted = true;
    appExitState = 'stopped';
    stopAppEvents();
    try {
      app.emit('quit', createBrowserWindowEvent(app), code);
    } finally {
      try {
        processEmitter.emit('exit', code);
      } finally {
        if (exitHost) exitHost(code);
      }
    }
  }

  function destroyAllAppWindows() {
    let firstError;
    for (const window of BrowserWindow.getAllWindows()) {
      try {
        window.destroy();
      } catch (error) {
        firstError ||= error;
      }
    }
    if (firstError) throw firstError;
  }

  function forceAppExit(code, exitHost, notifyShutdown) {
    if (appExitState === 'stopped' || appExitState === 'exiting') return;
    appExitState = 'exiting';
    stopAppEvents();
    try {
      if (notifyShutdown && !beforeQuitEmitted) {
        beforeQuitEmitted = true;
        app.emit('before-quit', createBrowserWindowEvent(app, true));
      }
    } finally {
      try {
        destroyAllAppWindows();
      } finally {
        try {
          if (notifyShutdown && !willQuitEmitted) {
            willQuitEmitted = true;
            app.emit('will-quit', createBrowserWindowEvent(app, true));
          }
        } finally {
          finishAppExit(code, exitHost);
        }
      }
    }
  }

  function continueAppQuit() {
    if (appExitState !== 'quitting' || appQuitPhase !== 'closing' ||
        appQuitAdvancing) return;
    appQuitAdvancing = true;
    try {
      for (const window of BrowserWindow.getAllWindows()) {
        if (appExitState !== 'quitting') return;
        // app.quit() can be called inside this window's close listener. Let
        // that original request finish before inspecting whether it was vetoed.
        if (window._closeRequested) return;
        window.close();
        if (appExitState !== 'quitting') return;
        if (!window.isDestroyed()) {
          cancelAppQuit();
          return;
        }
      }
      appQuitPhase = 'will-quit';
      willQuitEmitted = true;
      const event = createBrowserWindowEvent(app, true);
      app.emit('will-quit', event);
      if (appExitState !== 'quitting') return;
      if (event.defaultPrevented) {
        cancelAppQuit();
        return;
      }
      finishAppExit(0, requireAppExitHost());
    } catch (error) {
      cancelAppQuit();
      throw error;
    } finally {
      appQuitAdvancing = false;
    }
  }
  app.isPackaged = Boolean(__xenonRendererBaseUrl) ||
      (Array.isArray(globalThis.__xenonRendererUrlMappings) &&
       globalThis.__xenonRendererUrlMappings.length > 0);
  app.getAppPath = () => __xenonAppPath;
  app.getPath = name => __xenonGetPath(String(name));
  app.getVersion = () => __xenonAppVersion;
  app.getName = () => __xenonAppName;
  app.whenReady = () => appReadyPromise;
  app.isReady = () => appReady;
  app.disableHardwareAcceleration = () => {};
  app.requestSingleInstanceLock = () => electronUnsupported('app.requestSingleInstanceLock');
  app.releaseSingleInstanceLock = () => electronUnsupported('app.releaseSingleInstanceLock');
  app.setAppUserModelId = () => {};
  app.setAsDefaultProtocolClient = () => electronUnsupported('app.setAsDefaultProtocolClient');
  app.removeAsDefaultProtocolClient = () => electronUnsupported('app.removeAsDefaultProtocolClient');
  app.addRecentDocument = () => {};
  app.clearRecentDocuments = () => {};
  app.focus = () => {};
  app.quit = () => {
    if (appExitState !== 'running') return;
    // Check capability before any cleanup, so an unavailable host cannot leave
    // a live container whose SDK and windows have already been shut down.
    requireAppExitHost();
    appExitState = 'quitting';
    appQuitPhase = 'before-quit';
    stopAppEvents();
    beforeQuitEmitted = true;
    try {
      const event = createBrowserWindowEvent(app, true);
      app.emit('before-quit', event);
      if (appExitState !== 'quitting') return;
      if (event.defaultPrevented) {
        cancelAppQuit();
        return;
      }
      appQuitPhase = 'closing';
      continueAppQuit();
    } catch (error) {
      cancelAppQuit();
      throw error;
    }
  };
  app.exit = (code = 0) => {
    if (appExitState === 'stopped' || appExitState === 'exiting') return;
    if (typeof code !== 'number' || !Number.isInteger(code)) {
      throw Object.assign(new TypeError('Exit code must be an integer'),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (code < -2147483648 || code > 2147483647) {
      throw Object.assign(new RangeError('Exit code must fit a signed 32-bit integer'),
                          {code: 'ERR_OUT_OF_RANGE'});
    }
    forceAppExit(code, requireAppExitHost(), false);
  };
  app.commandLine = {appendSwitch() {}, appendArgument() {}, hasSwitch() { return false; }};

  class DownloadItem extends EventEmitter {
    constructor(url = '', filename = '') {
      super();
      this._url = String(url || '');
      this._filename = String(filename || '');
      this._state = 'progressing';
      this._totalBytes = 0;
      this._receivedBytes = 0;
      this._paused = false;
      this._savePath = '';
    }
    getURL() { return this._url; }
    getFilename() { return this._filename; }
    getState() { return this._state; }
    isPaused() { return this._paused; }
    getTotalBytes() { return this._totalBytes; }
    getReceivedBytes() { return this._receivedBytes; }
    getContentDisposition() { return ''; }
    getMimeType() { return ''; }
    hasUserGesture() { return true; }
    getSavePath() { return this._savePath; }
    setSavePath(path) { this._savePath = String(path || ''); }
    pause() {
      this._paused = true;
      this.emit('updated', { sender: this }, 'interrupted');
    }
    resume() {
      this._paused = false;
      this.emit('updated', { sender: this }, 'progressing');
    }
    cancel() {
      this._state = 'cancelled';
      this.emit('updated', { sender: this }, 'cancelled');
      this.emit('done', { sender: this }, 'cancelled');
    }
  }

  function electronUnsupported(method) {
    const error = new Error(`Electron ${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }

  function callElectronApi(operation, details = {}) {
    if (typeof __xenonBrowserWindowCall !== 'function') {
      return electronUnsupported(operation);
    }
    try {
      const request = {operation};
      for (const [key, value] of Object.entries(details)) {
        if (value !== undefined) request[key] = value;
      }
      return __xenonBrowserWindowCall(0, 'electron-api', request);
    } catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  }

  class Session extends EventEmitter {
    constructor(partition = '') {
      super();
      this.partition = String(partition || '');
      this._userAgent = '';
      this._downloadPath = '';
      this.webRequest = Object.fromEntries([
        'onBeforeRequest', 'onBeforeSendHeaders', 'onSendHeaders',
        'onHeadersReceived', 'onResponseStarted', 'onBeforeRedirect',
        'onCompleted', 'onErrorOccurred',
      ].map(method => [method, () => electronUnsupported(`webRequest.${method}`)]));
      this.cookies = Object.fromEntries(['get', 'set', 'remove', 'flushStore']
          .map(method => [method, async () => electronUnsupported(`cookies.${method}`)]));
      this.protocol = Object.fromEntries([
        'registerFileProtocol', 'registerBufferProtocol', 'registerStringProtocol',
        'registerHttpProtocol', 'registerStreamProtocol', 'unregisterProtocol',
        'isProtocolRegistered', 'interceptFileProtocol', 'interceptStringProtocol',
        'interceptBufferProtocol', 'interceptHttpProtocol', 'interceptStreamProtocol',
        'uninterceptProtocol', 'isProtocolIntercepted',
      ].map(method => [method, () => electronUnsupported(`protocol.${method}`)]));
    }
    getUserAgent() {
      return this._userAgent || String(__xenonUserAgent || '');
    }
    setUserAgent(userAgent) {
      this._userAgent = String(userAgent || '');
    }
    async setProxy(config) { return electronUnsupported('session.setProxy'); }
    async resolveProxy(url) { return electronUnsupported('session.resolveProxy'); }
    async clearCache() { return electronUnsupported('session.clearCache'); }
    async clearStorageData(options) { return electronUnsupported('session.clearStorageData'); }
    async clearAuthCache() { return electronUnsupported('session.clearAuthCache'); }
    async clearHostResolverCache() { return electronUnsupported('session.clearHostResolverCache'); }
    setDownloadPath(p) { return electronUnsupported('session.setDownloadPath'); }
    enableNetworkEmulation(options) { return electronUnsupported('session.enableNetworkEmulation'); }
    disableNetworkEmulation() { return electronUnsupported('session.disableNetworkEmulation'); }
    setCertificateVerifyProc(proc) { return electronUnsupported('session.setCertificateVerifyProc'); }
    setPermissionRequestHandler(handler) { return electronUnsupported('session.setPermissionRequestHandler'); }
    setPermissionCheckHandler(handler) { return electronUnsupported('session.setPermissionCheckHandler'); }
    async getBlobData(identifier) { return electronUnsupported('session.getBlobData'); }
    createInterruptedDownload(options) { return electronUnsupported('session.createInterruptedDownload'); }
  }

  const sessionMap = new Map();
  function getSession(partition = '') {
    const key = String(partition || '');
    let sess = sessionMap.get(key);
    if (!sess) {
      sess = new Session(key);
      sessionMap.set(key, sess);
    }
    return sess;
  }
  const defaultSession = getSession('');
  const session = {
    defaultSession,
    fromPartition: (partition) => getSession(partition),
  };

  globalThis.__xenonDispatchWillDownload = (url, filename, windowId) => {
    const owner = BrowserWindow.fromId(Number(windowId));
    const contents = owner ? owner.webContents : null;
    const sess = contents?.session || defaultSession;
    const item = new DownloadItem(url, filename);
    const event = {
      defaultPrevented: false,
      preventDefault() { this.defaultPrevented = true; }
    };
    sess.emit('will-download', event, item, contents);
    return item;
  };

  // Keep transport state out of the public object inspected by remote bridges.
  const webContentsState = new WeakMap();
  const allWebContents = new Map();
  const rendererWebContents = new Map();
  const guestWebContents = new Map();
  const pendingGuestScripts = new Map();
  let nextGuestScriptId = 1;
  let nextWebContentsId = 1;

  function getSenderWebContents(sender) {
    let contents = rendererWebContents.get(sender.endpointId);
    if (!contents) {
      const owner = BrowserWindow.fromId(Number(sender.windowId));
      contents = guestWebContents.get(Number(sender.windowId)) ||
          (owner ? owner.webContents : new WebContents(null));
      rendererWebContents.set(sender.endpointId, contents);
      webContentsState.get(contents).endpoints.set(sender.endpointId, sender);
    }
    return contents;
  }

  function getMainEndpoint(contents) {
    // The document host only admits primary main frames. During navigation,
    // prefer the newly registered document over an endpoint being torn down.
    return [...webContentsState.get(contents).endpoints.values()]
        .filter(endpoint => endpoint.isMainFrame !== false).at(-1);
  }

  function destroyWebContents(contents) {
    const state = webContentsState.get(contents);
    if (state.destroyed) return;
    state.destroyed = true;
    for (const [id, pending] of pendingGuestScripts) {
      if (pending.contents !== contents) continue;
      clearTimeout(pending.timer);
      pendingGuestScripts.delete(id);
      pending.reject(new Error('WebContents was destroyed'));
    }
    const processIds = new Set();
    for (const endpoint of state.endpoints.values()) {
      rendererWebContents.delete(endpoint.endpointId);
      processIds.add(endpoint.processId);
    }
    state.endpoints.clear();
    allWebContents.delete(contents.id);
    for (const processId of processIds) {
      contents.emit('render-view-deleted', {sender: contents}, processId);
    }
    contents.emit('destroyed');
  }

  globalThis.__xenonDispatchRendererEvent = (sender, attached) => {
    if (attached) {
      getSenderWebContents(sender);
      return;
    }
    const contents = rendererWebContents.get(sender.endpointId);
    if (!contents) return;
    rendererWebContents.delete(sender.endpointId);
    const state = webContentsState.get(contents);
    state.endpoints.delete(sender.endpointId);
    if (![...state.endpoints.values()].some(
        endpoint => endpoint.processId === sender.processId)) {
      contents.emit('render-view-deleted', {sender: contents}, sender.processId);
    }
    // A document disconnect is not destruction of its owning BrowserWindow.
    if (!contents._owner && !contents._guest && state.endpoints.size === 0) {
      destroyWebContents(contents);
    }
  };

  class WebContents extends EventEmitter {
    constructor(owner) {
      super();
      this.id = nextWebContentsId++;
      this._owner = owner;
      webContentsState.set(this, {endpoints: new Map(), destroyed: false});
      allWebContents.set(this.id, this);
      this.session = defaultSession;
    }
    send(channel, ...args) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const endpoint = getMainEndpoint(this);
      if (endpoint) __xenonSendToRenderer(endpoint.endpointId, channel, args);
    }
    sendToFrame(frameId, channel, ...args) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const [processId, routingId] = Array.isArray(frameId) ? frameId :
          [this.processId, frameId];
      const endpoint = [...webContentsState.get(this).endpoints.values()].find(
          item => item.processId === processId && item.frameId === routingId);
      if (!endpoint) return false;
      __xenonSendToRenderer(endpoint.endpointId, channel, args);
      return true;
    }
    get processId() { return getMainEndpoint(this)?.processId || 0; }
    get frameId() { return getMainEndpoint(this)?.frameId || 0; }
    getProcessId() { return this.processId; }
    getOwnerBrowserWindow() { return this._owner; }
    getLastWebPreferences() {
      return this._guest ? {...this._guest.preferences} :
          this._owner ? {...this._owner._webPreferences} : null;
    }
    isDestroyed() { return webContentsState.get(this).destroyed; }
    getURL() { return this._url || ''; }
    getTitle() { return this._pageTitle || ''; }
    loadURL(url, options) {
      if (options && typeof options === 'object' && options.userAgent) {
        this.setUserAgent(options.userAgent);
      }
      this._url = mapRendererUrl(String(url || ''));
      const nativeId = this._guest?.nativeId || this._owner?.id;
      if (nativeId && typeof __xenonLoadBrowserWindowURL === 'function') {
        __xenonLoadBrowserWindowURL(nativeId, this._url);
      }
      return Promise.resolve();
    }
    executeJavaScript(code) {
      if (this._guest) {
        return new Promise((resolve, reject) => {
          if (this.isDestroyed() || !getMainEndpoint(this)) {
            reject(new Error('Guest renderer is unavailable'));
            return;
          }
          const id = nextGuestScriptId++;
          const timer = setTimeout(() => {
            pendingGuestScripts.delete(id);
            reject(new Error('Guest script evaluation timed out'));
          }, 30000);
          pendingGuestScripts.set(id, {contents: this, resolve, reject, timer});
          this.send('__xenon:execute-javascript', id, String(code));
        });
      }
      if (this._owner) {
        callBrowserWindow(
            this._owner, 'execute-javascript', {code: String(code || '')});
      }
      return Promise.resolve();
    }
    openDevTools() {}
    closeDevTools() {}
    isDevToolsOpened() { return false; }
    setWindowOpenHandler(handler) {
      if (typeof handler !== 'function') throw new TypeError('Expected a function');
      webContentsState.get(this).windowOpenHandler = handler;
    }
    setUserAgent(userAgent) {
      this._userAgent = String(userAgent || '');
      if (!this._guest && this.session && typeof this.session.setUserAgent === 'function') {
        this.session.setUserAgent(this._userAgent);
      }
      if (this._owner || this._guest) {
        callBrowserWindow(
            this._owner || {id: this._guest.nativeId}, 'set-user-agent', {value: this._userAgent});
      }
    }
    getUserAgent() {
      if (this._owner || this._guest) {
        const value = callBrowserWindow(this._owner || {id: this._guest.nativeId}, 'get-user-agent');
        if (typeof value === 'string') {
          this._userAgent = value;
        }
      }
      if (!this._userAgent && this.session &&
          typeof this.session.getUserAgent === 'function') {
        this._userAgent = this.session.getUserAgent();
      }
      return this._userAgent || '';
    }
    toggleDevTools() {}
  }
  ipcMain.handle('__xenon:register-guest', (event, nativeId, preferences) => {
    if (!Number.isInteger(nativeId) || nativeId >= 0 || guestWebContents.has(nativeId)) {
      throw new Error('Invalid guest WebContents identity');
    }
    const contents = new WebContents(null);
    contents._guest = {nativeId, host: event.sender, preferences: {...preferences}};
    guestWebContents.set(nativeId, contents);
    event.sender.emit('did-attach-webview', {sender: event.sender}, contents);
    return contents.id;
  });
  ipcMain.handle('__xenon:guest-call', (event, id, method, args = []) => {
    const contents = allWebContents.get(id);
    if (!contents?._guest || contents._guest.host !== event.sender) {
      throw new Error('Guest WebContents does not belong to this document');
    }
    if (['loadURL', 'send', 'sendToFrame', 'setUserAgent', 'executeJavaScript'].includes(method)) {
      return contents[method](...args);
    }
    const commands = {reload: 'reload', stop: 'stop', goBack: 'go-back', goForward: 'go-forward'};
    if (commands[method]) return callBrowserWindow({id: contents._guest.nativeId}, commands[method]);
    throw new Error('Unsupported guest method: ' + method);
  });
  ipcMain.on('__xenon:send-to-host', (event, channel, args) => {
    const contents = event.sender;
    if (!contents._guest) throw new Error('sendToHost requires a guest WebContents');
    contents._guest.host.send('__xenon:guest-event', contents.id, 'ipc-message',
        {channel, args, frameId: event.frameId});
  });
  ipcMain.on('__xenon:execute-javascript-result', (event, id, success, value) => {
    const pending = pendingGuestScripts.get(id);
    if (!pending || pending.contents !== event.sender) return;
    clearTimeout(pending.timer);
    pendingGuestScripts.delete(id);
    if (success) pending.resolve(value);
    else pending.reject(new Error(String(value)));
  });
  function mapRendererUrl(url) {
    const base = String(__xenonRendererBaseUrl || '');
    const mappings = Array.isArray(__xenonRendererUrlMappings) ?
        __xenonRendererUrlMappings : [];
    const raw = String(url || '');
    const normalized = raw.replace(/\\/g, '/');
    let protocol = '';
    let hostname = '';
    let search = '';
    let hash = '';
    try {
      const parsed = new URL(normalized);
      protocol = parsed.protocol;
      hostname = parsed.hostname;
      search = parsed.search;
      hash = parsed.hash;
    } catch (e) {
      const hashIdx = normalized.indexOf('#');
      const withoutHash = hashIdx >= 0 ? normalized.slice(0, hashIdx) : normalized;
      const searchIdx = withoutHash.indexOf('?');
      if (searchIdx >= 0) {
        search = withoutHash.slice(searchIdx);
      }
      if (hashIdx >= 0) {
        hash = normalized.slice(hashIdx);
      }
    }
    if (normalized.startsWith('file:') || protocol === 'file:') {
      let sourcePath = normalized;
      try {
        sourcePath = decodeURIComponent(new URL(normalized).pathname)
            .replace(/^\/([A-Za-z]:)/, '$1');
      } catch (e) {
        sourcePath = decodeURIComponent(
            normalized.replace(/^file:\/\/\/?/i, ''));
      }
      sourcePath = sourcePath.replace(/\\/g, '/');
      const fold = value => __xenonPlatform === 'win32' ?
          value.toLowerCase() : value;
      const foldedSource = fold(sourcePath);
      for (const mapping of mappings) {
        const sourcePrefix = String(mapping?.sourcePathPrefix || '')
            .replace(/\\/g, '/').replace(/\/$/, '');
        const targetBase = String(mapping?.targetBaseUrl || '');
        const foldedPrefix = fold(sourcePrefix);
        if (!sourcePrefix || !targetBase ||
            (foldedSource !== foldedPrefix &&
             !foldedSource.startsWith(foldedPrefix + '/'))) {
          continue;
        }
        const relativePath = sourcePath.slice(sourcePrefix.length)
            .replace(/^\/+/, '') || 'index.html';
        const mapped = new URL(
            relativePath,
            targetBase.endsWith('/') ? targetBase : targetBase + '/');
        mapped.search = search;
        mapped.hash = hash;
        return mapped.toString();
      }
    }
    if (base && (normalized.startsWith('file:') || protocol === 'file:' ||
                 hostname === 'localhost')) {
      const mapped = new URL(base.endsWith('/') ? base + 'index.html' : base);
      mapped.search = search;
      mapped.hash = hash;
      return mapped.toString();
    }
    return url;
  }
  const browserWindows = [];
  let browserWindowCloseDepth = 0;
  let allBrowserWindowsClosed = true;

  function finishBrowserWindowClose(window, closeNative) {
    if (window._destroyed) return;
    window._destroyed = true;
    window._visible = false;
    ++browserWindowCloseDepth;
    try {
      try {
        destroyWebContents(window.webContents);
      } finally {
        if (closeNative && typeof __xenonCloseBrowserWindow === 'function') {
          __xenonCloseBrowserWindow(window.id);
        }
      }
    } finally {
      try {
        window.emit('closed');
      } finally {
        --browserWindowCloseDepth;
        // A closed listener can destroy a paired window or create its
        // replacement. Inspect the final live set after that cascade finishes.
        if (browserWindowCloseDepth === 0 && !allBrowserWindowsClosed &&
            !browserWindows.some(item => !item._destroyed)) {
          allBrowserWindowsClosed = true;
          if (appExitState === 'running') app.emit('window-all-closed');
        }
      }
    }
  }

  function callBrowserWindow(window, command, details = {}) {
    if (typeof __xenonBrowserWindowCall !== 'function') {
      return electronUnsupported(`BrowserWindow.${command}`);
    }
    try {
      return __xenonBrowserWindowCall(window.id, command, details);
    } catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  }
  function createBrowserWindowEvent(window, cancellable = false) {
    return {
      sender: window,
      returnValue: undefined,
      defaultPrevented: false,
      preventDefault() {
        if (cancellable) {
          this.defaultPrevented = true;
          this.returnValue = false;
        }
      },
    };
  }
  class BrowserWindow extends EventEmitter {
    constructor(options = {}) {
      super();
      if (appExitState !== 'running') {
        throw Object.assign(new Error('Cannot create a BrowserWindow while the application exits'),
                            {code: 'ERR_APP_QUITTING'});
      }
      this._webPreferences = Object.freeze({
        contextIsolation: true,
        nodeIntegration: false,
        ...options.webPreferences,
      });
      if (this._webPreferences.preload &&
          (typeof this._webPreferences.preload !== 'string' ||
           !pathModule.isAbsolute(this._webPreferences.preload))) {
        throw new TypeError('BrowserWindow preload must be an absolute path');
      }
      const title = options.title === undefined ? 'Electron' : String(options.title);
      if (typeof __xenonCreateBrowserWindow !== 'function') {
        return electronUnsupported('BrowserWindow creation');
      }
      const created =
          __xenonCreateBrowserWindow({
                  width: options.width || 800,
                  height: options.height || 600,
                  show: options.show !== false,
                  frame: options.frame !== false,
                  transparent: Boolean(options.transparent),
                  parentId: options.parent && options.parent.id
                      ? options.parent.id
                      : 0,
                  title,
                });
      this.id = created.id;
      this._hwnd = created.hwnd || '0';
      this._parent = options.parent || null;
      this.webContents = new WebContents(this);
      this._visible = options.show !== false;
      this._minimized = false;
      this._maximized = false;
      this._fullscreen = false;
      this._destroyed = false;
      this._resizable = options.resizable !== false;
      this._movable = options.movable !== false;
      this._maximizable = options.maximizable !== false;
      this._minimizable = options.minimizable !== false;
      this._closable = options.closable !== false;
      this._focusable = options.focusable !== false;
      this._enabled = true;
      this._opacity = 1;
      this._hasShadow = options.hasShadow !== false;
      this._bounds = {
        x: options.x != null ? Number(options.x) : 0,
        y: options.y != null ? Number(options.y) : 0,
        width: Number(options.width) || 800,
        height: Number(options.height) || 600,
      };
      this._minSize = {width: 0, height: 0};
      this._maxSize = {width: 0, height: 0};
      this._title = title;
      this._backgroundColor = '#000000';
      if (options.backgroundColor !== undefined) {
        this.setBackgroundColor(options.backgroundColor);
      }
      this._windowMessageHooks = new Map();
      if (options.alwaysOnTop) {
        this.setAlwaysOnTop(true);
      }
      // Electron: omit x/y → center; `center: true` also centers. Child windows
      // with a parent keep the host-synced parent bounds unless positioned.
      if (options.x != null || options.y != null) {
        const actual = callBrowserWindow(this, 'set-bounds', this._bounds);
        if (actual && typeof actual === 'object') this._bounds = actual;
      } else if (options.center === true || !this._parent) {
        callBrowserWindow(this, 'center');
        const actual = callBrowserWindow(this, 'get-bounds');
        if (actual && typeof actual === 'object') this._bounds = actual;
      } else {
        const actual = callBrowserWindow(this, 'get-bounds');
        if (actual && typeof actual === 'object') this._bounds = actual;
      }
      browserWindows.push(this);
      allBrowserWindowsClosed = false;
    }
    static getAllWindows() { return browserWindows.filter(item => !item._destroyed); }
    static getFocusedWindow() { return BrowserWindow.getAllWindows().find(window => window.isFocused()) || null; }
    static fromId(id) { return browserWindows.find(item => item.id === id) || null; }
    static fromWebContents(contents) {
      if (!contents) return null;
      if (contents._owner instanceof BrowserWindow) return contents._owner;
      if (contents instanceof WebContents && contents._owner) {
        return contents._owner;
      }
      const windowId = Number(
          contents.__xenonWindowId ?? contents.windowId ?? 0);
      const matched = browserWindows.find(item => item.webContents === contents ||
          (windowId > 0 && item.id === windowId)) || null;
      if (!matched && typeof __xenonLog === 'function') {
        __xenonLog('BrowserWindow.fromWebContents miss windowId=' + windowId +
                   ' windows=' + browserWindows.map(item => item.id).join(','));
      }
      return matched;
    }
    getNativeWindowHandle() {
      const buf = Buffer.alloc(8);
      try {
        buf.writeBigUInt64LE(BigInt(this._hwnd), 0);
      } catch (e) {
        buf.writeUInt32LE(Number(this._hwnd) >>> 0, 0);
      }
      return buf;
    }
    getParentWindow() { return this._parent; }
    setParentWindow(parent) {
      if (parent !== null && !(parent instanceof BrowserWindow)) {
        throw new TypeError('parent must be a BrowserWindow or null');
      }
      for (let ancestor = parent; ancestor; ancestor = ancestor._parent) {
        if (ancestor === this) throw new Error('BrowserWindow parent cycle');
      }
      if (this._destroyed || parent?._destroyed) throw new Error('Object has been destroyed');
      callBrowserWindow(this, 'set-parent-window', {parentId: parent?.id || 0});
      this._parent = parent;
    }
    isModal() { return false; }
    moveTop() { callBrowserWindow(this, 'move-top'); }
    setProgressBar(progress, options = {}) {
      if (typeof progress !== 'number' || !Number.isFinite(progress)) {
        throw new TypeError('progress must be a finite number');
      }
      callBrowserWindow(this, 'set-progress-bar', {progress, mode: options.mode});
    }
    getChildWindows() {
      return BrowserWindow.getAllWindows().filter(item => item._parent === this);
    }
    get resizable() { return this._resizable; }
    set resizable(value) { this.setResizable(value); }
    get movable() { return this._movable; }
    set movable(value) { this.setMovable(value); }
    get maximizable() { return this._maximizable; }
    set maximizable(value) { this.setMaximizable(value); }
    get minimizable() { return this._minimizable; }
    set minimizable(value) { this.setMinimizable(value); }
    get closable() { return this._closable; }
    set closable(value) { this.setClosable(value); }
    get focusable() { return this._focusable; }
    set focusable(value) { this.setFocusable(value); }
    loadURL(url, options) { return this.webContents.loadURL(url, options); }
    loadFile(file) { return this.webContents.loadURL(file); }
    show() {
      if (typeof __xenonBrowserWindowCall === 'function') {
        callBrowserWindow(this, 'show');
      } else {
        this._visible = true;
        this.emit('show');
      }
    }
    showInactive() {
      if (typeof __xenonBrowserWindowCall === 'function') {
        callBrowserWindow(this, 'show-inactive');
      } else {
        this.show();
      }
    }
    hide() {
      if (typeof __xenonBrowserWindowCall === 'function') {
        callBrowserWindow(this, 'hide');
      } else {
        this._visible = false;
        this.emit('hide');
      }
    }
    isVisible() {
      const value = callBrowserWindow(this, 'is-visible');
      return typeof value === 'boolean' ? value : this._visible;
    }
    focus() { callBrowserWindow(this, 'focus'); }
    blur() { callBrowserWindow(this, 'blur'); }
    isFocused() {
      const value = callBrowserWindow(this, 'is-focused');
      return typeof value === 'boolean' ? value : false;
    }
    minimize() { callBrowserWindow(this, 'minimize'); }
    isMinimized() {
      const value = callBrowserWindow(this, 'is-minimized');
      return typeof value === 'boolean' ? value : this._minimized;
    }
    maximize() { callBrowserWindow(this, 'maximize'); }
    unmaximize() { callBrowserWindow(this, 'unmaximize'); }
    isMaximized() {
      const value = callBrowserWindow(this, 'is-maximized');
      return typeof value === 'boolean' ? value : this._maximized;
    }
    restore() { callBrowserWindow(this, 'restore'); }
    setFullScreen(value) {
      callBrowserWindow(this, 'set-fullscreen', {value: Boolean(value)});
    }
    isFullScreen() {
      const value = callBrowserWindow(this, 'is-fullscreen');
      return typeof value === 'boolean' ? value : this._fullscreen;
    }
    setAlwaysOnTop(value) {
      callBrowserWindow(this, 'set-always-on-top', {value: Boolean(value)});
    }
    isAlwaysOnTop() { return callBrowserWindow(this, 'is-always-on-top'); }
    setSkipTaskbar() {}
    setTitle(title) {
      const value = String(title ?? '');
      callBrowserWindow(this, 'set-title', {title: value});
      this._title = value;
    }
    getTitle() { return this._title; }
    setBounds(bounds) {
      if (!bounds || typeof bounds !== 'object') return;
      const actual = callBrowserWindow(this, 'set-bounds', bounds);
      this._bounds = actual && typeof actual === 'object' ? actual : {
          x: bounds.x == null ? this._bounds.x : Number(bounds.x),
          y: bounds.y == null ? this._bounds.y : Number(bounds.y),
          width: bounds.width == null ? this._bounds.width : Number(bounds.width),
          height: bounds.height == null ? this._bounds.height : Number(bounds.height),
        };
    }
    getBounds() {
      const actual = callBrowserWindow(this, 'get-bounds');
      if (actual && typeof actual === 'object') this._bounds = actual;
      return {...this._bounds};
    }
    getContentBounds() { return this.getBounds(); }
    setContentBounds(bounds) { this.setBounds(bounds); }
    getNormalBounds() {
      const actual = callBrowserWindow(this, 'get-normal-bounds');
      return actual && typeof actual === 'object' ? {...actual} : this.getBounds();
    }
    setSize(width, height) { this.setBounds({...this._bounds, width, height}); }
    getSize() { return [this._bounds.width, this._bounds.height]; }
    setContentSize(width, height) { this.setSize(width, height); }
    getContentSize() { return this.getSize(); }
    setPosition(x, y) { this.setBounds({...this._bounds, x, y}); }
    getPosition() { return [this._bounds.x, this._bounds.y]; }
    center() { callBrowserWindow(this, 'center'); }
    setMinimumSize(width, height) {
      this._minSize = {width: Number(width) || 0, height: Number(height) || 0};
    }
    getMinimumSize() { return [this._minSize.width, this._minSize.height]; }
    setMaximumSize(width, height) {
      this._maxSize = {width: Number(width) || 0, height: Number(height) || 0};
    }
    getMaximumSize() { return [this._maxSize.width, this._maxSize.height]; }
    setResizable(value) { this._resizable = Boolean(value); }
    isResizable() { return this._resizable; }
    setMovable(value) { this._movable = Boolean(value); }
    isMovable() { return this._movable; }
    setMaximizable(value) { this._maximizable = Boolean(value); }
    isMaximizable() { return this._maximizable; }
    setMinimizable(value) { this._minimizable = Boolean(value); }
    isMinimizable() { return this._minimizable; }
    setClosable(value) { this._closable = Boolean(value); }
    isClosable() { return this._closable; }
    setFocusable(value) { this._focusable = Boolean(value); }
    isFocusable() { return this._focusable; }
    setEnabled(value) {
      this._enabled = Boolean(value);
      callBrowserWindow(this, 'set-enabled', {value: this._enabled});
    }
    isEnabled() {
      const value = callBrowserWindow(this, 'is-enabled');
      return typeof value === 'boolean' ? value : this._enabled;
    }
    setOpacity(value) {
      this._opacity = Number(value);
      callBrowserWindow(this, 'set-opacity', {value: this._opacity});
    }
    getOpacity() { return this._opacity; }
    setHasShadow(value) { this._hasShadow = Boolean(value); }
    hasShadow() { return this._hasShadow; }
    setBackgroundColor(color) {
      if (typeof __xenonBrowserWindowCall !== 'function') {
        const error = new Error('BrowserWindow background color host is unavailable');
        error.code = 'ERR_NOT_SUPPORTED';
        throw error;
      }
      callBrowserWindow(this, 'set-background-color', {color});
      this._backgroundColor = color;
    }
    getBackgroundColor() { return this._backgroundColor; }
    setAspectRatio() {}
    setKiosk() {}
    isKiosk() { return false; }
    isNormal() { return !this._maximized && !this._minimized && !this._fullscreen; }
    setSimpleFullScreen(value) { this.setFullScreen(value); }
    isSimpleFullScreen() { return this.isFullScreen(); }
    isFullScreenable() { return true; }
    setFullScreenable() {}
    setVisibleOnAllWorkspaces() {}
    isVisibleOnAllWorkspaces() { return false; }
    setWindowButtonVisibility() {}
    setDocumentEdited() {}
    isDocumentEdited() { return false; }
    setRepresentedFilename() {}
    getRepresentedFilename() { return ''; }
    setMenu() {}
    removeMenu() {}
    setMenuBarVisibility() {}
    setAutoHideMenuBar() {}
    isMenuBarVisible() { return false; }
    setIcon() {}
    setOverlayIcon() {}
    setShape(rects) {
      callBrowserWindow(this, 'set-shape', {
        rects: Array.isArray(rects) ? rects : [],
      });
    }
    flashFrame() {}
    setContentProtection() {}
    setAppDetails() {}
    setThumbnailClip() {}
    setThumbnailToolTip() {}
    close() {
      if (this._destroyed || this._closeRequested) return;
      this._closeRequested = true;
      const event = createBrowserWindowEvent(this, true);
      try {
        this.emit('close', event);
        // Renderer beforeunload/unload is not yet part of this host close path.
        if (!event.defaultPrevented) this.destroy();
      } catch (error) {
        cancelAppQuit();
        throw error;
      } finally {
        this._closeRequested = false;
        if (!this._destroyed && event.defaultPrevented) cancelAppQuit();
        continueAppQuit();
      }
    }
    destroy() {
      finishBrowserWindowClose(this, true);
    }
    isDestroyed() { return this._destroyed; }
    isWindowMessageHooked(message) {
      return this._windowMessageHooks.has(Number(message));
    }
    hookWindowMessage(message, callback) {
      const key = Number(message);
      if (!Number.isInteger(key) || typeof callback !== 'function') return;
      this._windowMessageHooks.set(key, callback);
      callBrowserWindow(this, 'hook-window-message', {message: key});
    }
    unhookWindowMessage(message) {
      const key = Number(message);
      this._windowMessageHooks.delete(key);
      callBrowserWindow(this, 'unhook-window-message', {message: key});
    }
    setIgnoreMouseEvents(value) {
      callBrowserWindow(this, 'set-ignore-mouse-events', {
        value: Boolean(value),
      });
    }
  }

  const nativeMenuClicks = new Map();
  let activeNativeMenu = null;
  globalThis.__xenonDispatchBrowserWindowEvent =
      (windowId, eventName, details = null) => {
    const guest = guestWebContents.get(Number(windowId));
    if (guest && eventName.startsWith('guest-')) {
      const name = eventName.slice('guest-'.length);
      if (details?.url) guest._url = details.url;
      if (name === 'new-window') {
        const handler = webContentsState.get(guest).windowOpenHandler;
        const decision = handler ? handler(details) :
            {action: guest._guest.preferences.allowPopups ? 'allow' : 'deny'};
        if (decision?.action === 'allow') {
          const popup = new BrowserWindow(decision.overrideBrowserWindowOptions || {});
          popup.loadURL(details.url);
        }
      }
      if (!guest._guest.host.isDestroyed())
        guest._guest.host.send('__xenon:guest-event', guest.id, name, details || {});
      if (name === 'destroyed') {
        guestWebContents.delete(Number(windowId));
        destroyWebContents(guest);
      } else {
        guest.emit(name, {sender: guest}, details);
      }
      return;
    }
    const win = BrowserWindow.fromId(Number(windowId));
    if (!win) return;
    if (eventName === 'close-requested') {
      win.close();
      return;
    }
    if (eventName === 'native-menu-command') {
      const click = nativeMenuClicks.get(Number(details && details.commandId));
      if (typeof click === 'function') {
        try {
          click();
        } catch (error) {
          console.error(error);
        }
      }
      return;
    }
    if (eventName === 'native-menu-closed') {
      const menu = activeNativeMenu;
      activeNativeMenu = null;
      nativeMenuClicks.clear();
      if (menu) {
        menu.emit('menu-will-close');
      }
      return;
    }
    if (eventName === 'web-contents-page-title-updated' && details) {
      const title = String(details.title ?? '');
      const explicitSet = Boolean(details.explicitSet);
      const contents = win.webContents;
      contents._pageTitle = title;
      const event = createBrowserWindowEvent(win, true);
      win.emit('page-title-updated', event, title, explicitSet);
      if (!event.defaultPrevented && !win.isDestroyed()) {
        win.setTitle(title);
      }
      if (!contents.isDestroyed()) {
        contents.emit('page-title-updated',
            createBrowserWindowEvent(contents), title, explicitSet);
      }
      return;
    }
    const webContentsEventPrefix = 'web-contents-';
    if (eventName.startsWith(webContentsEventPrefix)) {
      win.webContents.emit(
          eventName.slice(webContentsEventPrefix.length),
          createBrowserWindowEvent(win.webContents), details);
      return;
    }
    if (eventName === 'bounds-changed' && details) {
      const old = win._bounds;
      const moved = old.x !== details.x || old.y !== details.y;
      const resized = old.width !== details.width ||
          old.height !== details.height;
      win._bounds = {...details};
      if (moved) win.emit('move', createBrowserWindowEvent(win));
      if (resized) win.emit('resize', createBrowserWindowEvent(win));
      return;
    }
    if (eventName === 'state-changed' && details) {
      const wasMinimized = win._minimized;
      const wasMaximized = win._maximized;
      const wasFullscreen = win._fullscreen;
      win._minimized = Boolean(details.minimized);
      win._maximized = Boolean(details.maximized);
      win._fullscreen = Boolean(details.fullscreen);
      if (!wasMinimized && win._minimized) {
        win.emit('minimize', createBrowserWindowEvent(win));
      }
      if (wasMinimized && !win._minimized) {
        win.emit('restore', createBrowserWindowEvent(win));
      }
      if (!wasMaximized && win._maximized) {
        win.emit('maximize', createBrowserWindowEvent(win));
      }
      if (wasMaximized && !win._maximized) {
        win.emit('unmaximize', createBrowserWindowEvent(win));
      }
      if (!wasFullscreen && win._fullscreen) {
        win.emit('enter-full-screen', createBrowserWindowEvent(win));
      }
      if (wasFullscreen && !win._fullscreen) {
        win.emit('leave-full-screen', createBrowserWindowEvent(win));
      }
      return;
    }
    if (eventName === 'window-message' && details) {
      const callback = win._windowMessageHooks.get(Number(details.message));
      if (!callback) return;
      const wParam = Buffer.alloc(8);
      const lParam = Buffer.alloc(8);
      wParam.writeBigUInt64LE(BigInt.asUintN(64, BigInt(details.wParam)), 0);
      lParam.writeBigUInt64LE(BigInt.asUintN(64, BigInt(details.lParam)), 0);
      callback(wParam, lParam);
      return;
    }
    if (eventName === 'show' || eventName === 'hide') {
      const visible = eventName === 'show';
      if (win._visible !== visible) {
        win._visible = visible;
        win.emit(eventName, createBrowserWindowEvent(win));
      }
      return;
    }
    if (eventName === 'closed') {
      finishBrowserWindowClose(win, false);
      return;
    }
    win.emit(eventName, createBrowserWindowEvent(win), details);
  };

  function serializeNativeImage(icon) {
    if (!icon) return undefined;
    if (typeof icon === 'string' && icon) return icon;
    if (typeof icon.path === 'string' && icon.path) return icon.path;
    return undefined;
  }
  function serializeMenuTemplate(items, state) {
    const serialized = [];
    for (const item of items || []) {
      if (!item) continue;
      const type = item.type || (item.submenu ? 'submenu' : 'normal');
      if (type === 'separator') {
        serialized.push({type: 'separator'});
        continue;
      }
      const id = ++state.nextId;
      if (typeof item.click === 'function') {
        state.clicks.set(id, item.click.bind(item));
      }
      const entry = {
        id,
        type: (type === 'submenu' || item.submenu) ? 'submenu' : type,
        label: String(item.label || ''),
        enabled: item.enabled !== false,
        visible: item.visible !== false,
        checked: Boolean(item.checked),
      };
      const icon = serializeNativeImage(item.icon);
      if (icon) entry.icon = icon;
      if (entry.type === 'radio' && item.groupId != null) {
        entry.groupId = Number(item.groupId) || 0;
      }
      if (entry.type === 'submenu') {
        const submenu = Array.isArray(item.submenu)
            ? item.submenu
            : (item.submenu && Array.isArray(item.submenu.items)
                   ? item.submenu.items : []);
        entry.submenu = serializeMenuTemplate(submenu, state);
      }
      serialized.push(entry);
    }
    return serialized;
  }
  class Tray extends EventEmitter {
    setImage() {}
    setToolTip() {}
    setContextMenu() {}
    destroy() {}
  }
  class Menu extends EventEmitter {
    static buildFromTemplate(template) { return new Menu(template); }
    static setApplicationMenu() {}
    static getApplicationMenu() { return null; }
    constructor(template = []) {
      super();
      this.items = template;
    }
    popup(options = {}) {
      const window = (options && options.window) ||
          BrowserWindow.getFocusedWindow();
      if (!window || window._destroyed) return;
      nativeMenuClicks.clear();
      activeNativeMenu = this;
      const payload = {
        items: serializeMenuTemplate(this.items, {
          nextId: 0,
          clicks: nativeMenuClicks,
        }),
      };
      if (options && Number.isFinite(options.x) && Number.isFinite(options.y)) {
        payload.x = Math.round(options.x);
        payload.y = Math.round(options.y);
      }
      callBrowserWindow(window, 'popup-menu', payload);
    }
    closePopup() {}
  }
  const nativeTheme = new EventEmitter();
  nativeTheme.themeSource = 'system';
  Object.defineProperty(nativeTheme, 'shouldUseDarkColors', {get: () => false});
  const powerMonitor = new EventEmitter();
  const autoUpdater = new EventEmitter();
  autoUpdater.checkForUpdates = () => Promise.resolve(null);
  const electronModule = {
    app,
    ipcMain,
    BrowserWindow,
    webContents: {
      fromId: id => allWebContents.get(Number(id)),
      getAllWebContents: () => [...allWebContents.values()],
    },
    shell: {
      openExternal: async (url, options = {}) => {
        callElectronApi('shell.openExternal', {url, options});
      },
      openPath: async path => callElectronApi('shell.openPath', {path}),
      showItemInFolder: path => { callElectronApi('shell.showItemInFolder', {path}); },
    },
    dialog: {
      showOpenDialog: async (winOrOpts, maybeOpts) => {
        const opts = (maybeOpts && typeof maybeOpts === 'object') ? maybeOpts : (winOrOpts && typeof winOrOpts === 'object' ? winOrOpts : {});
        if (typeof __xenonShowOpenDialog !== 'function') return electronUnsupported('dialog.showOpenDialog');
        const filePaths = __xenonShowOpenDialog(opts);
        return { canceled: filePaths.length === 0, filePaths };
      },
      showOpenDialogSync: (winOrOpts, maybeOpts) => {
        const opts = (maybeOpts && typeof maybeOpts === 'object') ? maybeOpts : (winOrOpts && typeof winOrOpts === 'object' ? winOrOpts : {});
        if (typeof __xenonShowOpenDialog !== 'function') return electronUnsupported('dialog.showOpenDialogSync');
        const filePaths = __xenonShowOpenDialog(opts);
        return filePaths.length > 0 ? filePaths : undefined;
      },
      showSaveDialog: async () => electronUnsupported('dialog.showSaveDialog'),
      showSaveDialogSync: () => electronUnsupported('dialog.showSaveDialogSync'),
      showMessageBox: async () => electronUnsupported('dialog.showMessageBox'),
      showMessageBoxSync: () => electronUnsupported('dialog.showMessageBoxSync'),
    },
    Tray,
    Menu,
    nativeTheme,
    systemPreferences: {isAeroGlassEnabled: () => false},
    powerMonitor,
    session,
    globalShortcut: {
      register: () => electronUnsupported('globalShortcut.register'),
      registerAll: () => electronUnsupported('globalShortcut.registerAll'),
      // No registration can succeed in this runtime, so cleanup is a valid no-op.
      unregister() {}, unregisterAll() {}, isRegistered: () => false,
    },
    nativeImage: {createEmpty: () => ({}), createFromPath: path => ({path})},
    clipboard: {
      readText: type => callElectronApi('clipboard.readText', {type}),
      writeText: (text, type) => { callElectronApi('clipboard.writeText', {text, type}); },
      readHTML: type => callElectronApi('clipboard.readHTML', {type}),
      writeHTML: (markup, type) => { callElectronApi('clipboard.writeHTML', {markup, type}); },
      clear: type => { callElectronApi('clipboard.clear', {type}); },
    },
    screen: (() => {
      const call = (method, options = {}) => {
        if (typeof __xenonBrowserWindowCall !== 'function') {
          const error = new Error('Electron screen host is unavailable');
          error.code = 'ERR_NOT_SUPPORTED';
          throw error;
        }
        // Screen queries have no BrowserWindow. The host reserves id 0 for
        // these application-wide reads on its UI thread.
        return __xenonBrowserWindowCall(0, 'screen', {method, ...options});
      };
      return Object.assign(new EventEmitter(), {
        getPrimaryDisplay: () => call('getPrimaryDisplay'),
        getAllDisplays: () => call('getAllDisplays'),
        getDisplayMatching: rect => call('getDisplayMatching', {rect}),
        getDisplayNearestPoint: point => call('getDisplayNearestPoint', {point}),
        getCursorScreenPoint: () => call('getCursorScreenPoint'),
      });
    })(),
    autoUpdater,
  };
  electronModule.default = electronModule;

  // =========================================================================
  // Node.js Built-in Modules (Buffer, fs, crypto, util, url, stream, etc.)
  // =========================================================================

  const nativeToBase64 = Uint8Array.prototype.toBase64;
  const nativeFromBase64 = Uint8Array.fromBase64;
  const nativeBase64HandlesViews = !nativeToBase64 ||
      nativeToBase64.call(new Uint8Array([0, 255]).subarray(1)) === '/w==';
  const arrayBufferByteLength = Object.getOwnPropertyDescriptor(
      ArrayBuffer.prototype, 'byteLength').get;
  const sharedArrayBufferByteLength = typeof SharedArrayBuffer === 'function' ?
      Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, 'byteLength').get : null;

  function decodeBase64(value) {
    if (nativeFromBase64) {
      try {
        return nativeFromBase64(value);
      } catch (error) {
        if (!(error instanceof SyntaxError)) throw error;
      }
    }
    let normalized = value.split('=', 1)[0].replace(/-/g, '+')
        .replace(/_/g, '/').replace(/[^A-Za-z0-9+/]/g, '');
    if (normalized.length % 4 === 1) normalized = normalized.slice(0, -1);
    if (nativeFromBase64) return nativeFromBase64(normalized);
    const binary = atob(normalized);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; ++i) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }

  function encodeBase64(bytes, urlSafe) {
    if (nativeToBase64) {
      const view = nativeBase64HandlesViews || bytes.byteOffset === 0 ?
          bytes : new Uint8Array(bytes);
      return nativeToBase64.call(view, urlSafe ?
          {alphabet: 'base64url', omitPadding: true} : undefined);
    }
    const chunks = [];
    for (let offset = 0; offset < bytes.length; offset += 8192) {
      chunks.push(String.fromCharCode.apply(
          null, bytes.subarray(offset, offset + 8192)));
    }
    const encoded = btoa(chunks.join(''));
    return urlSafe ? encoded.replace(/\+/g, '-').replace(/\//g, '_')
        .replace(/=+$/, '') : encoded;
  }

  class Buffer extends Uint8Array {
    static byteLength(value, encoding) {
      if (typeof value !== 'string') {
        if (ArrayBuffer.isView(value)) return value.byteLength;
        // Intrinsic getters also recognize ArrayBuffers from another realm.
        try { return arrayBufferByteLength.call(value); } catch (_) {}
        if (sharedArrayBufferByteLength) {
          try { return sharedArrayBufferByteLength.call(value); } catch (_) {}
        }
        const error = new TypeError('value must be a string, Buffer, or ArrayBuffer');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      const length = value.length;
      switch (typeof encoding === 'string' ? encoding.toLowerCase() : '') {
        case 'ascii': case 'latin1': case 'binary': return length;
        case 'utf16le': case 'utf-16le': case 'ucs2': case 'ucs-2':
          return length * 2;
        case 'hex': return Math.floor(length / 2);
        case 'base64': case 'base64url': {
          // Node estimates from the encoded length, including any whitespace.
          let unpadded = length;
          if (unpadded && value.charCodeAt(unpadded - 1) === 61) --unpadded;
          if (unpadded && value.charCodeAt(unpadded - 1) === 61) --unpadded;
          return Math.floor(unpadded * 3 / 4);
        }
      }
      // UTF-8 is also Node's fallback for an unknown encoding. Count directly
      // so Content-Length does not allocate an encoded copy of the request.
      let bytes = 0;
      for (let i = 0; i < length; ++i) {
        const code = value.charCodeAt(i);
        if (code < 0x80) ++bytes;
        else if (code < 0x800) bytes += 2;
        else if (code >= 0xd800 && code <= 0xdbff && i + 1 < length &&
                 value.charCodeAt(i + 1) >= 0xdc00 && value.charCodeAt(i + 1) <= 0xdfff) {
          bytes += 4;
          ++i;
        } else bytes += 3;
      }
      return bytes;
    }
    static from(data, encoding, length) {
      if (typeof data === 'string') {
        if (encoding === 'hex') {
          const bytes = [];
          for (let i = 0; i < data.length; i += 2) {
            bytes.push(parseInt(data.substr(i, 2), 16));
          }
          return new Buffer(bytes);
        } else if (encoding === 'base64' || encoding === 'base64url') {
          const bytes = decodeBase64(data);
          return new Buffer(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        }
        const encoder = new TextEncoder();
        return new Buffer(encoder.encode(data));
      }
      if (data instanceof ArrayBuffer) {
        let offset = encoding === undefined ? 0 : +encoding;
        if (Number.isNaN(offset)) offset = 0;
        const available = data.byteLength - offset;
        if (length !== undefined) {
          length = +length;
          if (!(length > 0)) length = 0;
        }
        if (available < 0 || (length !== undefined && length > available)) {
          const error = new RangeError('Buffer offset or length is outside the ArrayBuffer');
          error.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
          throw error;
        }
        return new Buffer(data, offset, length);
      }
      if (Array.isArray(data) || ArrayBuffer.isView(data)) {
        return new Buffer(data);
      }
      return new Buffer(0);
    }
    static alloc(size, fill = 0) {
      const b = new Buffer(size);
      if (fill) b.fill(fill);
      return b;
    }
    static allocUnsafe(size) { return new Buffer(size); }
    static isBuffer(obj) { return obj instanceof Buffer; }
    static concat(list, totalLength) {
      if (!Array.isArray(list)) return new Buffer(0);
      if (totalLength === undefined) {
        totalLength = list.reduce((acc, cur) => acc + (cur ? cur.length : 0), 0);
      }
      const result = new Buffer(totalLength);
      let offset = 0;
      for (const item of list) {
        if (item) {
          result.set(item, offset);
          offset += item.length;
        }
      }
      return result;
    }
    toString(encoding = 'utf8', start = 0, end = this.length) {
      const slice = this.subarray(start, end);
      if (encoding === 'hex') {
        return Array.from(slice).map(b => b.toString(16).padStart(2, '0')).join('');
      } else if (encoding === 'base64' || encoding === 'base64url') {
        return encodeBase64(slice, encoding === 'base64url');
      }
      return new TextDecoder().decode(slice);
    }
    slice(start, end) { return this.subarray(start, end); }
    _view() {
      return new DataView(this.buffer, this.byteOffset, this.byteLength);
    }
    writeUInt32LE(value, offset = 0) {
      this._view().setUint32(offset, value >>> 0, true);
      return offset + 4;
    }
    writeBigUInt64LE(value, offset = 0) {
      this._view().setBigUint64(offset, BigInt(value), true);
      return offset + 8;
    }
    readUInt32LE(offset = 0) {
      return this._view().getUint32(offset, true);
    }
    readInt32LE(offset = 0) {
      return this._view().getInt32(offset, true);
    }
    readInt16LE(offset = 0) {
      const value = this.readUIntLE(offset, 2);
      return value & 0x8000 ? value - 0x10000 : value;
    }
    readBigUInt64LE(offset = 0) {
      return this._view().getBigUint64(offset, true);
    }
    readBigInt64LE(offset = 0) {
      return this._view().getBigInt64(offset, true);
    }
    readUIntLE(offset, byteLength) {
      if (offset === undefined || typeof byteLength !== 'number') {
        const error = new TypeError('offset and byteLength must be numbers');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (!Number.isInteger(byteLength) || byteLength < 1 || byteLength > 6) {
        const error = new RangeError('byteLength must be an integer from 1 to 6');
        error.code = 'ERR_OUT_OF_RANGE';
        throw error;
      }
      if (typeof offset !== 'number') {
        const error = new TypeError('offset must be a number');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      if (this[offset] === undefined || this[offset + byteLength - 1] === undefined) {
        const error = new RangeError('offset is outside the bounds of the Buffer');
        error.code = Math.floor(offset) === offset && this.length < byteLength ?
            'ERR_BUFFER_OUT_OF_BOUNDS' : 'ERR_OUT_OF_RANGE';
        throw error;
      }
      let value = 0;
      for (let i = byteLength - 1; i >= 0; --i) {
        value = value * 256 + this[offset + i];
      }
      return value;
    }
  }
  Object.defineProperty(Buffer.prototype, 'readUintLE', {
    value: Buffer.prototype.readUIntLE, configurable: true, writable: true,
  });
  globalThis.Buffer = Buffer;

  const fsConstants = {
    F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1,
  };
  const fsError = (error, request) => {
    const match = /^([A-Z][A-Z0-9_]+):/.exec(error.message || '');
    if (match) error.code = match[1];
    error.path = request.path;
    const syscall = /, ([a-z_]+) '/.exec(error.message || '');
    if (syscall) error.syscall = syscall[1];
    return error;
  };
  const fsRequest = (operation, path, options = {}) =>
    ({...options, operation, path: String(path)});
  const fsCallSync = (request, data) => {
    try {
      const value = __xenonFsCall(request, data);
      return value === null ? undefined : value;
    } catch (error) {
      throw fsError(error, request);
    }
  };
  const fsCallAsync = async (request, data) => {
    try {
      const value = await __xenonFsCallAsync(request, data);
      return value === null ? undefined : value;
    } catch (error) {
      throw fsError(error, request);
    }
  };
  const fsUnsupported = method => {
    const error = new Error(`fs.${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  };
  const fsValidateCallback = callback => {
    if (typeof callback !== 'function') {
      const error = new TypeError('The callback argument must be a function');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
  };
  const fsCallback = (callback, operation) => {
    fsValidateCallback(callback);
    // Separate fulfillment/rejection callbacks ensure a user callback throwing
    // does not cause the same callback to be invoked for a second time.
    operation().then(value => callback(null, value), error => callback(error));
  };
  const fsEncoding = options =>
    typeof options === 'string' ? options : options?.encoding;
  const fsReadResult = (bytes, options) => {
    const buffer = Buffer.from(bytes);
    const encoding = fsEncoding(options);
    return encoding ? buffer.toString(encoding) : buffer;
  };
  const fsWriteData = (data, options) => {
    if (typeof data === 'string') return Buffer.from(data, fsEncoding(options));
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    const error = new TypeError('File data must be a string, Buffer, or an ArrayBuffer view');
    error.code = 'ERR_INVALID_ARG_TYPE';
    throw error;
  };
  const fsWriteRequest = (path, options) => {
    const flag = typeof options === 'object' ? options?.flag : undefined;
    if (flag !== undefined && flag !== 'w' && flag !== 'a') {
      return fsUnsupported('writeFile with flag ' + flag);
    }
    return fsRequest(flag === 'a' ? 'append_file' : 'write_file', path);
  };
  const fsStatResult = stat => ({
    isFile: () => stat.isFile,
    isDirectory: () => stat.isDirectory,
    isSymbolicLink: () => stat.isSymbolicLink,
    size: stat.size,
    mtime: new Date(stat.mtimeMs),
    mtimeMs: stat.mtimeMs,
    birthtime: new Date(stat.birthtimeMs),
    birthtimeMs: stat.birthtimeMs,
    mode: stat.isDirectory ? 0o777 : 0o666,
  });

  const fsPromises = {
    async readFile(path, options) {
      return fsReadResult(await fsCallAsync(
        fsRequest('read_file', path, {returnBytes: true})), options);
    },
    async writeFile(path, data, options) {
      return fsCallAsync(fsWriteRequest(path, options), fsWriteData(data, options));
    },
    async appendFile(path, data, options) {
      const appendOptions = typeof options === 'string' ? {encoding: options, flag: 'a'} :
          {flag: 'a', ...options};
      return fsCallAsync(fsWriteRequest(path, appendOptions), fsWriteData(data, appendOptions));
    },
    async stat(path) {
      return fsStatResult(await fsCallAsync(fsRequest('stat', path)));
    },
    async lstat(path) {
      return fsStatResult(await fsCallAsync(fsRequest('lstat', path)));
    },
    async readdir(path) { return fsCallAsync(fsRequest('readdir', path)); },
    async mkdir(path, options) {
      return fsCallAsync(fsRequest('mkdir', path, {recursive: Boolean(options?.recursive)}));
    },
    async unlink(path) { return fsCallAsync(fsRequest('unlink', path)); },
    async rm(path, options) {
      return fsCallAsync(fsRequest('rm', path, {
        recursive: Boolean(options?.recursive), force: Boolean(options?.force),
      }));
    },
    async access(path, mode = 0) {
      if (mode !== 0) return fsUnsupported('access with permission mode');
      return fsCallAsync(fsRequest('access', path));
    },
    async realpath(path) { return fsCallAsync(fsRequest('realpath', path)); },
    async copyFile(src, dest, flags = 0) {
      if (flags !== 0) return fsUnsupported('copyFile with flags');
      return fsCallAsync(fsRequest('copy_file', src, {destination: String(dest)}));
    },
    async rename(oldPath, newPath) {
      return fsCallAsync(fsRequest('rename', oldPath, {destination: String(newPath)}));
    },
    async open() { return fsUnsupported('promises.open'); },
    async chmod() { return fsUnsupported('promises.chmod'); },
    async chown() { return fsUnsupported('promises.chown'); },
  };

  const fsModule = {
    constants: fsConstants,
    promises: fsPromises,
    existsSync(path) {
      try { return Boolean(fsCallSync(fsRequest('exists', path))); }
      catch { return false; }
    },
    readFileSync(path, options) {
      return fsReadResult(fsCallSync(
        fsRequest('read_file', path, {returnBytes: true})), options);
    },
    writeFileSync(path, data, options) {
      return fsCallSync(fsWriteRequest(path, options), fsWriteData(data, options));
    },
    statSync(path) { return fsStatResult(fsCallSync(fsRequest('stat', path))); },
    lstatSync(path) { return fsStatResult(fsCallSync(fsRequest('lstat', path))); },
    readdirSync(path) { return fsCallSync(fsRequest('readdir', path)); },
    mkdirSync(path, options) {
      return fsCallSync(fsRequest('mkdir', path, {recursive: Boolean(options?.recursive)}));
    },
    unlinkSync(path) { return fsCallSync(fsRequest('unlink', path)); },
    rmdirSync(path, options) {
      return fsCallSync(fsRequest('rmdir', path, {recursive: Boolean(options?.recursive)}));
    },
    rmSync(path, options) {
      return fsCallSync(fsRequest('rm', path, {
        recursive: Boolean(options?.recursive), force: Boolean(options?.force),
      }));
    },
    accessSync(path, mode = 0) {
      if (mode !== 0) return fsUnsupported('accessSync with permission mode');
      return fsCallSync(fsRequest('access', path));
    },
    readFile(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.readFile(path, options));
    },
    writeFile(path, data, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.writeFile(path, data, options));
    },
    stat(path, callback) { fsCallback(callback, () => fsPromises.stat(path)); },
    lstat(path, callback) { fsCallback(callback, () => fsPromises.lstat(path)); },
    readdir(path, callback) { fsCallback(callback, () => fsPromises.readdir(path)); },
    mkdir(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.mkdir(path, options));
    },
    unlink(path, callback) { fsCallback(callback, () => fsPromises.unlink(path)); },
    rm(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.rm(path, options));
    },
    rmdir(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsCallAsync(
        fsRequest('rmdir', path, {recursive: Boolean(options?.recursive)})));
    },
    access(path, mode, callback) {
      if (typeof mode === 'function') { callback = mode; mode = undefined; }
      fsCallback(callback, () => fsPromises.access(path, mode));
    },
    exists(path, callback) {
      fsValidateCallback(callback);
      fsCallback((error, value) => callback(!error && Boolean(value)),
        () => fsCallAsync(fsRequest('exists', path)));
    },
    copyFile(src, dest, flags, callback) {
      if (typeof flags === 'function') { callback = flags; flags = undefined; }
      fsCallback(callback, () => fsPromises.copyFile(src, dest, flags));
    },
    copyFileSync(src, dest, flags = 0) {
      if (flags !== 0) return fsUnsupported('copyFileSync with flags');
      return fsCallSync(fsRequest('copy_file', src, {destination: String(dest)}));
    },
    rename(oldPath, newPath, callback) {
      fsCallback(callback, () => fsPromises.rename(oldPath, newPath));
    },
    renameSync(oldPath, newPath) {
      return fsCallSync(fsRequest('rename', oldPath, {destination: String(newPath)}));
    },
  };

  const realpathFn = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(callback, () => fsPromises.realpath(path, options));
  };
  realpathFn.native = realpathFn;
  fsModule.realpath = realpathFn;
  const realpathSyncFn = path => fsCallSync(fsRequest('realpath', path));
  realpathSyncFn.native = realpathSyncFn;
  fsModule.realpathSync = realpathSyncFn;

  // Do not fabricate descriptors, streams, watchers, or successful I/O.
  for (const method of [
    'watch', 'watchFile', 'unwatchFile',
    'chmodSync', 'chownSync', 'openSync', 'closeSync', 'readSync', 'writeSync',
  ]) {
    fsModule[method] = () => fsUnsupported(method);
  }
  for (const method of ['chmod', 'chown', 'open', 'close', 'read', 'write']) {
    fsModule[method] = (...args) => fsCallback(args[args.length - 1],
      async () => fsUnsupported(method));
  }

  // ReadStream currently reads through the existing whole-file worker bridge,
  // then delivers bounded chunks. No OS descriptor or fake open event is exposed.
  class FileReadStream extends Readable {
    constructor(path, options = {}) {
      super();
      if (typeof options === 'string') options = {encoding: options};
      if (!options || typeof options !== 'object') throw new TypeError('Invalid ReadStream options');
      if (options.fd != null || options.fs || (options.flags && options.flags !== 'r') ||
          options.autoClose === false) return fsUnsupported('ReadStream options');
      this.path = path;
      this.pending = true;
      this.readable = true;
      this.readableEnded = false;
      this.destroyed = false;
      this.closed = false;
      this.bytesRead = 0;
      this._flowing = null;
      this._queued = false;
      this._resumeQueued = false;
      this._bytes = null;
      this._offset = 0;
      this._encoding = null;
      this._decoder = {needed: 0};
      this._emitClose = options.emitClose !== false;
      this._chunkSize = options.highWaterMark === undefined ? 65536 : options.highWaterMark;
      const start = options.start === undefined ? 0 : options.start;
      const end = options.end === undefined ? Infinity : options.end;
      if (!Number.isSafeInteger(start) || start < 0 ||
          (end !== Infinity && (!Number.isSafeInteger(end) || end < start)) ||
          !Number.isSafeInteger(this._chunkSize) || this._chunkSize <= 0) {
        const error = new RangeError('Invalid ReadStream range or highWaterMark');
        error.code = 'ERR_OUT_OF_RANGE';
        throw error;
      }
      if (options.encoding) this.setEncoding(options.encoding);
      this._signal = options.signal;
      this._abort = () => {
        const error = new Error('The operation was aborted');
        error.name = 'AbortError';
        error.code = 'ABORT_ERR';
        this.destroy(error);
      };
      if (this._signal) {
        if (this._signal.aborted) queueMicrotask(this._abort);
        else this._signal.addEventListener('abort', this._abort, {once: true});
      }
      Promise.resolve().then(() => this.destroyed ? null : fsModule.promises.readFile(path))
          .then(bytes => {
            if (this.destroyed) return;
            this.pending = false;
            this._bytes = (Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes)).subarray(start,
                end === Infinity ? bytes.length : Math.min(bytes.length, end + 1));
            this.emit('ready');
            this._schedule();
          }, error => this.destroy(error));
    }
    on(name, listener) {
      super.on(name, listener);
      if (name === 'data' && this._flowing !== false) this.resume();
      return this;
    }
    once(name, listener) {
      super.once(name, listener);
      if (name === 'data' && this._flowing !== false) this.resume();
      return this;
    }
    setEncoding(encoding) {
      if (!['utf8', 'utf-8'].includes(String(encoding).toLowerCase()))
        return fsUnsupported('ReadStream encoding ' + encoding);
      this._encoding = 'utf8';
      return this;
    }
    pause() {
      if (this._flowing !== false) {
        this._flowing = false;
        this.emit('pause');
      }
      return this;
    }
    resume() {
      if (this.destroyed || this.readableEnded) return this;
      if (!this._flowing && !this._resumeQueued) {
        this._resumeQueued = true;
        queueMicrotask(() => {
          this._resumeQueued = false;
          if (this._flowing && !this.destroyed) this.emit('resume');
        });
      }
      this._flowing = true;
      this._schedule();
      return this;
    }
    isPaused() { return this._flowing === false; }
    _decodeUtf8(bytes, final = false) {
      const state = this._decoder;
      let result = '';
      for (let i = 0; i < bytes.length; ++i) {
        const byte = bytes[i];
        if (!state.needed) {
          if (byte <= 0x7f) { result += String.fromCharCode(byte); continue; }
          state.seen = 0;
          state.lower = 0x80;
          state.upper = 0xbf;
          if (byte >= 0xc2 && byte <= 0xdf) {
            state.needed = 1; state.point = byte & 0x1f;
          } else if (byte >= 0xe0 && byte <= 0xef) {
            state.needed = 2; state.point = byte & 0x0f;
            if (byte === 0xe0) state.lower = 0xa0;
            if (byte === 0xed) state.upper = 0x9f;
          } else if (byte >= 0xf0 && byte <= 0xf4) {
            state.needed = 3; state.point = byte & 7;
            if (byte === 0xf0) state.lower = 0x90;
            if (byte === 0xf4) state.upper = 0x8f;
          } else {
            result += '\ufffd';
          }
        } else if (byte < state.lower || byte > state.upper) {
          state.needed = 0;
          result += '\ufffd';
          --i;
        } else {
          state.lower = 0x80;
          state.upper = 0xbf;
          state.point = (state.point << 6) | (byte & 0x3f);
          if (++state.seen === state.needed) {
            result += String.fromCodePoint(state.point);
            state.needed = 0;
          }
        }
      }
      if (final && state.needed) {
        state.needed = 0;
        result += '\ufffd';
      }
      return result;
    }
    _schedule() {
      if (this._queued || !this._flowing || !this._bytes || this.destroyed) return;
      this._queued = true;
      queueMicrotask(() => {
        this._queued = false;
        if (!this._flowing || !this._bytes || this.destroyed) return;
        if (this._offset >= this._bytes.length) {
          if (this._encoding) {
            const tail = this._decodeUtf8(new Uint8Array(0), true);
            if (tail) this.emit('data', tail);
            if (!this._flowing || this.destroyed) return;
          }
          this.readableEnded = true;
          this.readable = false;
          this._bytes = null;
          this.emit('end');
          this.destroy();
          return;
        }
        const end = Math.min(this._offset + this._chunkSize, this._bytes.length);
        const bytes = this._bytes.subarray(this._offset, end);
        this._offset = end;
        this.bytesRead += bytes.length;
        // Decode incrementally: the main bootstrap's TextDecoder fallback is
        // not streaming, and per-chunk TextDecoder would also strip BOMs.
        const chunk = this._encoding ? this._decodeUtf8(bytes) : bytes;
        if (chunk.length) this.emit('data', chunk);
        this._schedule();
      });
    }
    destroy(error) {
      if (this.destroyed) return this;
      this.destroyed = true;
      this.pending = false;
      this.readable = false;
      this._bytes = null;
      if (this._signal) this._signal.removeEventListener('abort', this._abort);
      queueMicrotask(() => {
        try { if (error) this.emit('error', error); }
        finally {
          this.closed = true;
          if (this._emitClose) this.emit('close');
        }
      });
      return this;
    }
    close(callback) {
      if (callback) {
        if (this.closed) queueMicrotask(callback);
        else this.once('close', callback);
      }
      return this.destroy();
    }
  }
  fsModule.ReadStream = FileReadStream;
  fsModule.createReadStream = (path, options) => new FileReadStream(path, options);

  // Path-based writer over the real asynchronous filesystem bridge. Each
  // operation opens/closes its file; no persistent descriptor or open event is
  // invented. Descriptor-based writes and positioned writes remain unsupported.
  class FileWriteStream extends Writable {
    constructor(path, options = {}) {
      super();
      if (typeof options === 'string') options = {encoding: options};
      if (!options || typeof options !== 'object') throw new TypeError('Invalid WriteStream options');
      if (options.fd != null || options.fs || options.start !== undefined ||
          options.autoClose === false || options.flush || options.objectMode ||
          (options.mode !== undefined && options.mode !== 0o666)) {
        return fsUnsupported('WriteStream options');
      }
      const flags = options.flags === undefined ? 'w' : options.flags;
      if (flags !== 'w' && flags !== 'a') return fsUnsupported('WriteStream flags ' + flags);
      const highWaterMark = options.highWaterMark === undefined ? 16384 : options.highWaterMark;
      if (!Number.isSafeInteger(highWaterMark) || highWaterMark < 0) {
        throw Object.assign(new RangeError('Invalid WriteStream highWaterMark'), {code: 'ERR_OUT_OF_RANGE'});
      }
      this.path = path;
      this.fd = null;
      this.pending = true;
      this.writable = true;
      this.writableEnded = false;
      this.writableFinished = false;
      this.destroyed = false;
      this.closed = false;
      this.errored = null;
      this.bytesWritten = 0;
      this.writableLength = 0;
      this.writableHighWaterMark = highWaterMark;
      this.writableNeedDrain = false;
      this.writableCorked = 0;
      this._writes = [];
      this._endCallbacks = [];
      this._closeCallbacks = [];
      this._busy = true;
      this._closeScheduled = false;
      this._finishScheduled = false;
      this._emitClose = options.emitClose !== false;
      this.setDefaultEncoding(options.encoding === undefined ? 'utf8' : options.encoding);
      this._signal = options.signal;
      this._abort = () => this.destroy(Object.assign(new Error('The operation was aborted'),
          {name: 'AbortError', code: 'ABORT_ERR'}));
      if (this._signal) {
        if (typeof this._signal.addEventListener !== 'function' ||
            typeof this._signal.removeEventListener !== 'function') {
          throw Object.assign(new TypeError('signal must be an AbortSignal'), {code: 'ERR_INVALID_ARG_TYPE'});
        }
        if (this._signal.aborted) queueMicrotask(this._abort);
        else this._signal.addEventListener('abort', this._abort, {once: true});
      }
      Promise.resolve().then(() => {
        if (this.destroyed) return;
        return flags === 'a' ? fsModule.promises.appendFile(path, Buffer.alloc(0)) :
                               fsModule.promises.writeFile(path, Buffer.alloc(0));
      }).then(() => {
        this._busy = false;
        this.pending = false;
        if (this.destroyed) return this._completeClose();
        // A completed create/truncate is observable, without fabricating an fd.
        this.emit('ready');
        this._pump();
      }, error => {
        this._busy = false;
        this.pending = false;
        this.destroy(error);
      });
    }
    _callback(callback, error) {
      if (callback) queueMicrotask(() => callback(error));
    }
    _validateCallback(callback) {
      if (callback !== undefined) fsValidateCallback(callback);
    }
    _error(code, message) {
      return Object.assign(new Error(message), {code});
    }
    setDefaultEncoding(encoding) {
      if (typeof encoding !== 'string' ||
          !['utf8', 'utf-8', 'hex', 'base64', 'base64url', 'ascii', 'latin1',
            'binary', 'ucs2', 'ucs-2', 'utf16le', 'utf-16le'].includes(encoding.toLowerCase())) {
        throw this._error('ERR_UNKNOWN_ENCODING', 'Unknown encoding: ' + encoding);
      }
      this._defaultEncoding = encoding;
      return this;
    }
    write(chunk, encoding, callback) {
      if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
      this._validateCallback(callback);
      if (this.destroyed) {
        this._callback(callback, this.errored || this._error('ERR_STREAM_DESTROYED', 'Cannot write after destroy'));
        return false;
      }
      if (this.writableEnded) {
        const error = this._error('ERR_STREAM_WRITE_AFTER_END', 'write after end');
        this._callback(callback, error);
        this.destroy(error);
        return false;
      }
      let bytes;
      if (typeof chunk === 'string') {
        const selected = encoding || this._defaultEncoding;
        const previous = this._defaultEncoding;
        this.setDefaultEncoding(selected);
        this._defaultEncoding = previous;
        bytes = Buffer.from(chunk, selected);
      } else if (ArrayBuffer.isView(chunk)) {
        bytes = Buffer.from(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
      } else {
        throw Object.assign(new TypeError('WriteStream data must be a string or ArrayBuffer view'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      this._writes.push({bytes, callback});
      this.writableLength += bytes.length;
      const accepted = this.writableLength < this.writableHighWaterMark;
      if (!accepted) this.writableNeedDrain = true;
      this._pump();
      return accepted;
    }
    _pump() {
      if (this._busy || this.pending || this.destroyed || this.writableCorked) return;
      const entry = this._writes.shift();
      if (!entry) {
        if (this.writableEnded) this._finish();
        return;
      }
      this._busy = true;
      // Byte storage is snapped at write(), before user code can mutate it.
      Promise.resolve().then(() => fsModule.promises.appendFile(this.path, entry.bytes)).then(() => {
        this._busy = false;
        this.bytesWritten += entry.bytes.length;
        this.writableLength -= entry.bytes.length;
        this._callback(entry.callback, this.destroyed ?
            (this.errored || this._error('ERR_STREAM_DESTROYED', 'Stream was destroyed')) : undefined);
        if (this.destroyed) return this._completeClose();
        if (this.writableNeedDrain && this.writableLength === 0 && !this.writableEnded) {
          this.writableNeedDrain = false;
          queueMicrotask(() => {
            if (!this.destroyed && !this.writableEnded && this.writableLength === 0) this.emit('drain');
          });
        }
        this._pump();
      }, error => {
        this._busy = false;
        this.writableLength -= entry.bytes.length;
        this._callback(entry.callback, error);
        this.destroy(error);
      });
    }
    end(chunk, encoding, callback) {
      if (typeof chunk === 'function') { callback = chunk; chunk = undefined; encoding = undefined; }
      else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
      this._validateCallback(callback);
      if (this.destroyed) {
        this._callback(callback, this.errored ||
            (this.writableFinished ? undefined : this._error('ERR_STREAM_DESTROYED', 'Stream was destroyed')));
        return this;
      }
      if (this.writableFinished && (chunk === undefined || chunk === null)) {
        this._callback(callback);
        return this;
      }
      if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
      if (callback) this._endCallbacks.push(callback);
      this.writableEnded = true;
      this.writable = false;
      this.writableCorked = 0;
      this._pump();
      return this;
    }
    _finish() {
      if (this._finishScheduled || this.writableFinished) return;
      this._finishScheduled = true;
      queueMicrotask(() => {
        if (this.destroyed) return;
        this.emit('prefinish');
        if (this.destroyed) return;
        this.writableFinished = true;
        this.writableNeedDrain = false;
        for (const callback of this._endCallbacks.splice(0)) this._callback(callback);
        queueMicrotask(() => {
          if (this.destroyed) return;
          try { this.emit('finish'); }
          finally { this.destroy(); }
        });
      });
    }
    cork() { ++this.writableCorked; }
    uncork() {
      if (this.writableCorked) --this.writableCorked;
      this._pump();
    }
    destroy(error) {
      if (this.destroyed) {
        if (error && !this.errored) this.errored = error;
        this._completeClose();
        return this;
      }
      this.destroyed = true;
      this.writable = false;
      this.errored = error || null;
      if (this._signal) this._signal.removeEventListener('abort', this._abort);
      this._completeClose();
      return this;
    }
    _completeClose() {
      // The native bridge cannot cancel an already submitted file operation.
      // close waits for it, so no write can complete after the close event.
      if (this._busy || this._closeScheduled) return;
      this._closeScheduled = true;
      this.pending = false;
      const error = this.errored || this._error('ERR_STREAM_DESTROYED', 'Stream was destroyed');
      for (const entry of this._writes.splice(0)) this._callback(entry.callback, error);
      for (const callback of this._endCallbacks.splice(0)) this._callback(callback, error);
      this.writableLength = 0;
      this.writableNeedDrain = false;
      queueMicrotask(() => {
        try {
          if (this.errored) this.emit('error', this.errored);
        } finally {
          this.closed = true;
          for (const callback of this._closeCallbacks.splice(0)) this._callback(callback);
          if (this._emitClose) this.emit('close');
        }
      });
    }
    close(callback) {
      this._validateCallback(callback);
      if (callback) {
        if (this.closed) this._callback(callback);
        else this._closeCallbacks.push(callback);
      }
      if (!this.destroyed) this.end();
      return this;
    }
  }
  fsModule.WriteStream = FileWriteStream;
  fsModule.createWriteStream = (path, options) => new FileWriteStream(path, options);


  const utilModule = {
    TextEncoder: typeof TextEncoder !== 'undefined' ? TextEncoder : class TextEncoder {
      encode(str) {
        str = String(str || '');
        const utf8 = unescape(encodeURIComponent(str));
        const arr = new Uint8Array(utf8.length);
        for (let i = 0; i < utf8.length; i++) arr[i] = utf8.charCodeAt(i);
        return arr;
      }
    },
    TextDecoder: typeof TextDecoder !== 'undefined' ? TextDecoder : class TextDecoder {
      constructor(encoding = 'utf-8') { this.encoding = encoding; }
      decode(arr) {
        if (!arr) return '';
        let str = '';
        const bytes = arr instanceof Uint8Array ? arr : new Uint8Array(arr);
        for (let i = 0; i < bytes.length; i++) str += String.fromCharCode(bytes[i]);
        try { return decodeURIComponent(escape(str)); } catch { return str; }
      }
    },
    promisify(fn) {
      return (...args) => new Promise((resolve, reject) => {
        fn(...args, (err, res) => {
          if (err) return reject(err);
          resolve(res);
        });
      });
    },
    callbackify(fn) {
      return (...args) => {
        const cb = args.pop();
        fn(...args).then(res => cb(null, res), err => cb(err));
      };
    },
    inherits(ctor, superCtor) {
      if (typeof ctor !== 'function' || typeof superCtor !== 'function') {
        throw new TypeError('The constructor and super constructor must be functions');
      }
      ctor.super_ = superCtor;
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
    format(fmt, ...args) {
      if (typeof fmt !== 'string') return [fmt, ...args].map(String).join(' ');
      let i = 0;
      return fmt.replace(/%[sdjifoO%]/g, m => {
        if (m === '%%') return '%';
        if (i >= args.length) return m;
        const arg = args[i++];
        if (m === '%j') return JSON.stringify(arg);
        return String(arg);
      });
    },
    inspect(obj) {
      try { return JSON.stringify(obj, null, 2); } catch { return String(obj); }
    },
    deprecate(fn, msg) { return fn; },
    isArray: Array.isArray,
    isBoolean: (v) => typeof v === 'boolean',
    isBuffer: (v) => typeof Buffer !== 'undefined' && Buffer.isBuffer && Buffer.isBuffer(v),
    isDate: (v) => v instanceof Date,
    isError: (v) => v instanceof Error,
    isFunction: (v) => typeof v === 'function',
    isNull: (v) => v === null,
    isNullOrUndefined: (v) => v == null,
    isNumber: (v) => typeof v === 'number',
    isObject: (v) => typeof v === 'object' && v !== null,
    isPrimitive: (v) => v === null || (typeof v !== 'object' && typeof v !== 'function'),
    isRegExp: (v) => v instanceof RegExp,
    isString: (v) => typeof v === 'string',
    isSymbol: (v) => typeof v === 'symbol',
    isUndefined: (v) => v === undefined,
    types: {
      isPromise: v => v && typeof v.then === 'function',
      isDate: v => v instanceof Date,
      isRegExp: v => v instanceof RegExp,
      isNativeError: v => v instanceof Error,
      isBuffer: v => typeof Buffer !== 'undefined' && Buffer.isBuffer && Buffer.isBuffer(v),
      isArrayBuffer: v => v instanceof ArrayBuffer,
      isUint8Array: v => v instanceof Uint8Array,
    },
  };

  function createCipherObject(algorithm, key, iv, encrypt) {
    const normalizedAlgorithm = String(algorithm || '').toLowerCase();
    const keyBuffer = Buffer.isBuffer(key) ? Buffer.from(key) : Buffer.from(key);
    const ivBuffer = iv == null ? Buffer.alloc(0) : Buffer.from(iv);
    const chunks = [];
    let autoPadding = true;
    let finalized = false;
    return {
      update(data, inputEncoding, outputEncoding) {
        if (finalized) {
          throw new Error('Trying to add data in unsupported state');
        }
        chunks.push(typeof data === 'string' ?
            Buffer.from(data, inputEncoding || 'utf8') : Buffer.from(data));
        return outputEncoding ? '' : Buffer.alloc(0);
      },
      final(outputEncoding) {
        if (finalized) {
          throw new Error('Invalid state');
        }
        finalized = true;
        const result = Buffer.from(__xenonCryptoCipher(
            normalizedAlgorithm, Boolean(encrypt), keyBuffer.toString('base64'),
            ivBuffer.toString('base64'),
            Buffer.concat(chunks).toString('base64'), autoPadding), 'base64');
        return outputEncoding ? result.toString(outputEncoding) : result;
      },
      setAutoPadding(value = true) {
        if (finalized) {
          throw new Error('Invalid state');
        }
        autoPadding = Boolean(value);
        return this;
      },
    };
  }

  function cryptoArgumentError(message) {
    return Object.assign(new TypeError(message), {code: 'ERR_INVALID_ARG_TYPE'});
  }
  function digestEncoding(encoding) {
    if (typeof encoding !== 'string') throw cryptoArgumentError('Encoding must be a string');
    const normalized = encoding.toLowerCase();
    if (normalized === 'utf-8') return 'utf8';
    if (['utf8', 'hex', 'base64', 'base64url'].includes(normalized)) return normalized;
    throw Object.assign(new Error('Unsupported digest encoding: ' + encoding),
                        {code: 'ERR_NOT_SUPPORTED'});
  }
  function cryptoBytes(value, encoding) {
    if (typeof value === 'string') {
      const normalized = digestEncoding(encoding === undefined ? 'utf8' : encoding);
      // Match Node's partial hex decoding, including an unmatched final nibble.
      if (normalized === 'hex') value = value.match(/^(?:[0-9a-fA-F]{2})*/)[0];
      return Buffer.from(value, normalized);
    }
    if (ArrayBuffer.isView(value)) {
      return Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    }
    if (value instanceof ArrayBuffer) return Buffer.from(new Uint8Array(value));
    throw cryptoArgumentError('Crypto data must be a string or binary buffer');
  }
  function createDigestObject(algorithm, key) {
    if (typeof algorithm !== 'string') throw cryptoArgumentError('Algorithm must be a string');
    const normalized = algorithm.toLowerCase().replace(/-/g, '');
    if (!['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512'].includes(normalized)) {
      throw Object.assign(new Error('Unsupported digest algorithm: ' + algorithm),
                          {code: 'ERR_NOT_SUPPORTED'});
    }
    const keyBytes = key === undefined ? null : cryptoBytes(key);
    let chunks = [];
    let finalized = false;
    function checkState() {
      if (finalized) throw Object.assign(new Error('Digest already called'), {code: 'ERR_CRYPTO_HASH_FINALIZED'});
    }
    return {
      update(data, encoding) {
        checkState();
        chunks.push(cryptoBytes(data, encoding));
        return this;
      },
      digest(encoding) {
        checkState();
        const outputEncoding = encoding === undefined ? undefined : digestEncoding(encoding);
        finalized = true;
        const input = Buffer.concat(chunks);
        chunks = [];
        const result = Buffer.from(__xenonCryptoDigest(normalized, input, keyBytes));
        return outputEncoding ? result.toString(outputEncoding) : result;
      },
    };
  }
  const cryptoModule = {
    createCipheriv(algorithm, key, iv) {
      return createCipherObject(algorithm, key, iv, true);
    },
    createDecipheriv(algorithm, key, iv) {
      return createCipherObject(algorithm, key, iv, false);
    },
    createHash(algorithm) {
      return createDigestObject(algorithm);
    },
    createHmac(algorithm, key) {
      if (key === undefined) throw cryptoArgumentError('HMAC requires a key');
      return createDigestObject(algorithm, key);
    },
    randomBytes(size, callback) {
      if (typeof size !== 'number') throw cryptoArgumentError('Size must be a number');
      if (!Number.isInteger(size) || size < 0 || size > 0x7fffffff) {
        throw Object.assign(new RangeError('Size is out of range'), {code: 'ERR_OUT_OF_RANGE'});
      }
      if (callback !== undefined && typeof callback !== 'function') {
        throw cryptoArgumentError('Callback must be a function');
      }
      const generate = () => Buffer.from(__xenonCryptoRandom(size));
      if (callback === undefined) return generate();
      queueMicrotask(() => {
        let bytes;
        try { bytes = generate(); } catch (error) { callback(error); return; }
        callback(null, bytes);
      });
    },
    randomUUID() {
      const bytes = cryptoModule.randomBytes(16);
      bytes[6] = (bytes[6] & 15) | 64;
      bytes[8] = (bytes[8] & 63) | 128;
      const hex = bytes.toString('hex');
      return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
    },
  };

  // Reserved transport used by sandboxed renderers to expose the same
  // synchronous Node cipher contract as the utility main context.
  ipcMain.on('__xenon:crypto', (event, request) => {
    if (!request || request.operation !== 'cipher') {
      throw new TypeError('Invalid crypto request');
    }
    event.returnValue = __xenonCryptoCipher(
        String(request.algorithm || '').toLowerCase(),
        Boolean(request.encrypt), request.keyBase64 || '',
        request.ivBase64 || '', request.dataBase64 || '',
        request.autoPadding !== false);
  });

  const urlModule = {
    URL,
    URLSearchParams: typeof URLSearchParams !== 'undefined' ? URLSearchParams : class {},
    pathToFileURL(p) {
      let str = String(p).replace(/\\/g, '/');
      if (!str.startsWith('/')) str = '/' + str;
      return new URL('file://' + str);
    },
    fileURLToPath(u) {
      const str = typeof u === 'string' ? u : u.href;
      return decodeURIComponent(str.replace(/^file:\/\/\/?/, ''));
    },
    parse(urlStr) {
      try {
        const u = new URL(urlStr);
        return {
          protocol: u.protocol,
          host: u.host,
          hostname: u.hostname,
          port: u.port,
          pathname: u.pathname,
          search: u.search,
          query: u.search ? u.search.slice(1) : '',
          hash: u.hash,
          href: u.href,
        };
      } catch {
        return { href: urlStr };
      }
    },
    format(obj) {
      return obj.href || (obj.protocol ? `${obj.protocol}//${obj.host || ''}${obj.pathname || ''}${obj.search || ''}` : '');
    },
  };

  function streamUnsupportedError(method) {
    const error = new Error(`stream.${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    return error;
  }
  function streamValidateCallback(callback) {
    if (typeof callback !== 'function') {
      const error = new TypeError('The callback argument must be a function');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
  }
  function streamFailOperation(method, callback) {
    const error = streamUnsupportedError(method);
    if (callback === undefined) throw error;
    streamValidateCallback(callback);
    queueMicrotask(() => callback(error));
  }

  // Keep the callable constructors and EventEmitter inheritance used by
  // userland stream implementations. These base classes do not implement an
  // I/O queue: operations must fail explicitly until a subclass supplies one.
  function Stream() {
    if (!(this instanceof Stream)) return new Stream();
    EventEmitter.call(this);
  }
  Object.setPrototypeOf(Stream.prototype, EventEmitter.prototype);
  Object.setPrototypeOf(Stream, EventEmitter);
  Stream.prototype.pipe = function() { throw streamUnsupportedError('pipe'); };

  function Readable(options) {
    if (!(this instanceof Readable)) return new Readable(options);
    Stream.call(this);
    if (options) {
      if (typeof options.read === 'function') this._read = options.read;
      if (typeof options.destroy === 'function') this._destroy = options.destroy;
    }
  }
  Object.setPrototypeOf(Readable.prototype, Stream.prototype);
  Object.setPrototypeOf(Readable, Stream);
  Readable.prototype.read = function() { throw streamUnsupportedError('Readable.read'); };
  Readable.prototype.push = function() { throw streamUnsupportedError('Readable.push'); };

  function Writable(options) {
    if (!(this instanceof Writable)) return new Writable(options);
    Stream.call(this);
    if (options && typeof options.write === 'function') this._write = options.write;
  }
  Object.setPrototypeOf(Writable.prototype, Stream.prototype);
  Object.setPrototypeOf(Writable, Stream);
  Writable.prototype.write = function(chunk, encoding, callback) {
    if (typeof encoding === 'function') callback = encoding;
    streamFailOperation('Writable.write', callback);
    return false;
  };
  Writable.prototype.end = function(chunk, encoding, callback) {
    if (typeof chunk === 'function') callback = chunk;
    else if (typeof encoding === 'function') callback = encoding;
    streamFailOperation('Writable.end', callback);
    return this;
  };

  function Duplex(options) {
    if (!(this instanceof Duplex)) return new Duplex(options);
    Readable.call(this, options);
    if (options && typeof options.write === 'function') this._write = options.write;
  }
  Object.setPrototypeOf(Duplex.prototype, Readable.prototype);
  Object.setPrototypeOf(Duplex, Readable);
  Duplex.prototype.write = Writable.prototype.write;
  Duplex.prototype.end = Writable.prototype.end;

  function Transform(options) {
    if (!(this instanceof Transform)) return new Transform(options);
    Duplex.call(this, options);
    if (options && typeof options.transform === 'function') {
      this._transform = options.transform;
    }
  }
  Object.setPrototypeOf(Transform.prototype, Duplex.prototype);
  Object.setPrototypeOf(Transform, Duplex);
  Transform.prototype._transform = function(chunk, encoding, callback) {
    streamFailOperation('Transform._transform', callback);
  };

  function PassThrough(options) {
    if (!(this instanceof PassThrough)) return new PassThrough(options);
    Transform.call(this, options);
  }
  Object.setPrototypeOf(PassThrough.prototype, Transform.prototype);
  Object.setPrototypeOf(PassThrough, Transform);

  Stream.Stream = Stream;
  Stream.Readable = Readable;
  Stream.Writable = Writable;
  Stream.Duplex = Duplex;
  Stream.Transform = Transform;
  Stream.PassThrough = PassThrough;
  Stream.EventEmitter = EventEmitter;
  Stream.pipeline = (...args) => {
    const callback = args.pop();
    streamValidateCallback(callback);
    streamFailOperation('pipeline', callback);
    const streams = Array.isArray(args[0]) ? args[0] : args;
    return streams[streams.length - 1];
  };
  Stream.finished = (stream, options, callback) => {
    if (typeof options === 'function') callback = options;
    streamValidateCallback(callback);
    let active = true;
    queueMicrotask(() => {
      if (active) callback(streamUnsupportedError('finished'));
    });
    return () => { active = false; };
  };
  const streamModule = Stream;

  class StringDecoder {
    constructor(encoding = 'utf8') { this.encoding = encoding; }
    write(buf) { return Buffer.from(buf).toString(this.encoding); }
    end(buf) { return buf ? this.write(buf) : ''; }
  }
  const stringDecoderModule = { StringDecoder };

  // Expose the module independently of process creation. No process, PID, exit
  // status or successful callback may be fabricated without an OS operation.
  const childProcessModule = (() => {
    const unavailable = operation => {
      const error = new Error(`child_process.${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    class ChildProcess extends EventEmitter {
      constructor() { super(); unavailable('ChildProcess'); }
      spawn() { return unavailable('ChildProcess.spawn'); }
      kill() { return unavailable('ChildProcess.kill'); }
      send() { return unavailable('ChildProcess.send'); }
      disconnect() { return unavailable('ChildProcess.disconnect'); }
    }
    return {
      ChildProcess,
      _forkChild: () => unavailable('_forkChild'),
      spawn: () => unavailable('spawn'),
      spawnSync: () => unavailable('spawnSync'),
      exec: () => unavailable('exec'),
      execSync: () => unavailable('execSync'),
      execFile: () => unavailable('execFile'),
      execFileSync: () => unavailable('execFileSync'),
      fork: () => unavailable('fork'),
    };
  })();

  function assert(condition, message) {
    if (!condition) throw new Error(message || 'Assertion failed');
  }
  assert.ok = assert;
  assert.strictEqual = (a, b, m) => { if (a !== b) throw new Error(m || `Expected ${a} === ${b}`); };
  assert.deepStrictEqual = (a, b, m) => assert.strictEqual(JSON.stringify(a), JSON.stringify(b), m);
  assert.equal = assert.strictEqual;
  assert.notEqual = (a, b, m) => { if (a === b) throw new Error(m || `Expected ${a} !== ${b}`); };

  function createZlibModule(call) {
    const constants = {
      Z_NO_FLUSH: 0, Z_PARTIAL_FLUSH: 1, Z_SYNC_FLUSH: 2, Z_FULL_FLUSH: 3,
      Z_FINISH: 4, Z_BLOCK: 5, Z_TREES: 6, Z_OK: 0, Z_STREAM_END: 1,
      Z_NEED_DICT: 2, Z_ERRNO: -1, Z_STREAM_ERROR: -2, Z_DATA_ERROR: -3,
      Z_MEM_ERROR: -4, Z_BUF_ERROR: -5, Z_VERSION_ERROR: -6,
      Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9,
      Z_DEFAULT_COMPRESSION: -1, Z_FILTERED: 1, Z_HUFFMAN_ONLY: 2, Z_RLE: 3,
      Z_FIXED: 4, Z_DEFAULT_STRATEGY: 0, Z_MIN_WINDOWBITS: 8, Z_MAX_WINDOWBITS: 15,
      Z_DEFAULT_WINDOWBITS: 15, Z_MIN_CHUNK: 64, Z_DEFAULT_CHUNK: 16384,
      Z_MIN_MEMLEVEL: 1, Z_MAX_MEMLEVEL: 9, Z_DEFAULT_MEMLEVEL: 8,
      Z_MIN_LEVEL: -1, Z_MAX_LEVEL: 9, Z_DEFAULT_LEVEL: -1,
    };
    function codedError(code, message, Type = Error) {
      const error = new Type(message);
      error.code = code;
      return error;
    }
    function nativeError(error) {
      if (error && error.code) return error;
      const message = String(error && error.message || error);
      const code = /(?:^|\b)(Z_[A-Z_]+|ERR_[A-Z_]+):/.exec(message);
      return codedError(code ? code[1] : 'ERR_OPERATION_FAILED', message);
    }
    function bytes(input) {
      if (typeof input === 'string') return Buffer.from(input);
      if (ArrayBuffer.isView(input)) {
        return Buffer.from(new Uint8Array(input.buffer, input.byteOffset, input.byteLength));
      }
      if (input instanceof ArrayBuffer) return Buffer.from(new Uint8Array(input));
      throw codedError('ERR_INVALID_ARG_TYPE', 'zlib input must be a string or binary buffer', TypeError);
    }
    function options(value) {
      if (value === undefined || value === null) return {};
      if (typeof value !== 'object' || Array.isArray(value)) {
        throw codedError('ERR_INVALID_ARG_TYPE', 'zlib options must be an object', TypeError);
      }
      const result = {};
      const ranges = {level: [-1, 9], windowBits: [9, 15], memLevel: [1, 9],
        strategy: [0, 4], chunkSize: [64, 2147483647], maxOutputLength: [1, 67108864]};
      for (const [key, setting] of Object.entries(value)) {
        const value = setting;
        if (value === undefined) continue;
        if (key === 'flush' || key === 'finishFlush') {
          if (value !== (key === 'flush' ? 0 : 4)) {
            throw codedError('ERR_NOT_SUPPORTED', 'Partial zlib flush is not supported');
          }
        } else if (!ranges[key]) {
          throw codedError('ERR_NOT_SUPPORTED', 'Unsupported zlib option: ' + key);
        } else if (!Number.isInteger(value) || value < ranges[key][0] || value > ranges[key][1]) {
          throw codedError('ERR_OUT_OF_RANGE', 'Invalid zlib option: ' + key, RangeError);
        }
        result[key] = value;
      }
      return result;
    }
    const codes = {};
    for (const name of ['Z_OK', 'Z_STREAM_END', 'Z_NEED_DICT', 'Z_ERRNO',
        'Z_STREAM_ERROR', 'Z_DATA_ERROR', 'Z_MEM_ERROR', 'Z_BUF_ERROR', 'Z_VERSION_ERROR']) {
      codes[name] = constants[name];
      codes[constants[name]] = name;
    }
    const result = Object.assign({constants, codes}, constants);
    for (const name of ['gzip', 'gunzip', 'deflate', 'inflate', 'deflateRaw', 'inflateRaw', 'unzip']) {
      result[name + 'Sync'] = (input, opts) => {
        const data = bytes(input), settings = options(opts);
        try { return Buffer.from(call(name, data, settings, false)); }
        catch (error) { throw nativeError(error); }
      };
      result[name] = (input, opts, callback) => {
        if (typeof opts === 'function') { callback = opts; opts = undefined; }
        if (typeof callback !== 'function') {
          throw codedError('ERR_INVALID_ARG_TYPE', 'zlib callback must be a function', TypeError);
        }
        const data = bytes(input), settings = options(opts);
        let pending;
        try { pending = call(name, data, settings, true); }
        catch (error) { queueMicrotask(() => callback(nativeError(error))); return; }
        Promise.resolve(pending).then(value => callback(null, Buffer.from(value)),
                                      error => callback(nativeError(error)));
      };
    }
    for (const name of ['Gzip', 'Gunzip', 'Deflate', 'Inflate', 'DeflateRaw', 'InflateRaw', 'Unzip',
        'BrotliCompress', 'BrotliDecompress']) {
      const unsupported = function() {
        throw codedError('ERR_NOT_SUPPORTED', 'zlib.' + name + ' streaming is not supported');
      };
      result[name] = unsupported;
      result['create' + name] = unsupported;
    }
    for (const name of ['brotliCompress', 'brotliDecompress']) {
      result[name + 'Sync'] = () => {
        throw codedError('ERR_NOT_SUPPORTED', 'zlib.' + name + ' is not supported');
      };
      result[name] = (input, opts, callback) => {
        if (typeof opts === 'function') callback = opts;
        if (typeof callback !== 'function') {
          throw codedError('ERR_INVALID_ARG_TYPE', 'zlib callback must be a function', TypeError);
        }
        queueMicrotask(() => callback(codedError('ERR_NOT_SUPPORTED', 'zlib.' + name + ' is not supported')));
      };
    }
    return result;
  }
  const zlibModule = createZlibModule((operation, data, options, asynchronous) =>
      __xenonZlibCall(operation, data, options, asynchronous));
  const constantsModule = Object.assign({}, fsConstants, zlibModule.constants);
  ipcMain.on('__xenon:zlib', (event, request) => {
    event.returnValue = Buffer.from(__xenonZlibCall(request.operation,
        Buffer.from(request.dataBase64, 'base64'), request.options || {}, false))
        .toString('base64');
  });
  ipcMain.handle('__xenon:zlib', async (_event, request) => {
    const result = await __xenonZlibCall(request.operation,
        Buffer.from(request.dataBase64, 'base64'), request.options || {}, true);
    return Buffer.from(result).toString('base64');
  });
  const netModule = (() => {
    function normalizeNetPath(value) {
      let path = String(value || '');
      path = path.replace(/\//g, '\\');
      if (typeof __xenonPlatform === 'string' && __xenonPlatform === 'win32') {
        path = path.toLowerCase();
      }
      return path;
    }
    function netPathFromListenOrConnect(args) {
      const copy = args.slice();
      if (typeof copy[copy.length - 1] === 'function') {
        copy.pop();
      }
      const first = copy[0];
      if (first && typeof first === 'object') {
        return first.path || first.handle || '';
      }
      if (typeof first === 'string') {
        return first;
      }
      if (typeof first === 'number') {
        return `tcp:${copy[1] || '127.0.0.1'}:${first}`;
      }
      return '';
    }
    function bytesToNetWire(data) {
      if (typeof data === 'string') {
        return {t: 's', d: data};
      }
      const u8 = data instanceof Uint8Array ? data : Buffer.from(String(data));
      return {t: 'b64', d: Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString('base64')};
    }
    function netWireToBytes(wire) {
      if (!wire || wire.t === 's') {
        return Buffer.from(String(wire && wire.d || ''), 'utf8');
      }
      if (wire.t === 'b64') {
        return Buffer.from(String(wire.d || ''), 'base64');
      }
      const raw = String(wire.d || '');
      const u8 = new Uint8Array(raw.length);
      for (let i = 0; i < raw.length; i++) {
        u8[i] = raw.charCodeAt(i) & 0xff;
      }
      return Buffer.from(u8);
    }

    const netServers = new Map();
    const nativeNetServers = new Map();
    const netSockets = new Map();
    let nextNetSocketId = 1;
    let nextNetServerId = 1;

    function isNamedPipePath(path) {
      return /^\\\\\.\\pipe\\/i.test(String(path || ''));
    }

    function sendXenonNet(channel, payload, endpointId) {
      if (typeof __xenonNetSend === 'function') {
        __xenonNetSend(channel, payload);
        return;
      }
      const error = new Error('The native net transport is unavailable');
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    }
    // Socket and server callbacks belong to their connect/listen resource,
    // even when delivered by a later IPC message or the other local endpoint.
    function callNetCallback(resource, callback, args = []) {
      return runWithAsyncContext(resource._asyncContext, callback, resource, args);
    }
    function emitNetEvent(resource, event, ...args) {
      return callNetCallback(resource, resource.emit, [event, ...args]);
    }

    function allocNetSocket(socket) {
      socket._id = 'm-' + (nextNetSocketId++);
      netSockets.set(socket._id, socket);
      return socket._id;
    }
    function flushPendingNetWrites(socket) {
      const pending = socket._pendingWrites.splice(0);
      for (const data of pending) {
        socket.write(data);
      }
    }
    function closeNetSocket(socket, fromPeer) {
      if (!socket || socket._closed) {
        return;
      }
      socket._closed = true;
      socket._connected = false;
      socket.connecting = false;
      socket._connectCb = null;
      socket._pendingWrites.length = 0;
      if (socket._id) {
        netSockets.delete(socket._id);
      }
      if (socket._peer && !fromPeer) {
        closeNetSocket(socket._peer, true);
      }
      socket._peer = null;
      if (!fromPeer && socket._peerId) {
        sendXenonNet(
            '__xenon:net:close', {toId: socket._peerId, fromId: socket._id},
            socket._endpointId);
      }
      emitNetEvent(socket, 'end');
      emitNetEvent(socket, 'close');
    }
    function deliverNetBytes(socket, data) {
      const buf = Buffer.isBuffer(data) || data instanceof Uint8Array ?
          Buffer.from(data) : Buffer.from(String(data));
      emitNetEvent(socket, 'data', buf);
    }

    const module = {
      Socket: class extends EventEmitter {
        constructor() {
          super();
          this._connected = false;
          this._closed = false;
          this._peer = null;
          this._peerId = null;
          this._id = null;
          this._connectCb = null;
          this._endpointId = '*';
          this._pendingWrites = [];
          this.connecting = false;
          this.destroyed = false;
        }
        connect(...args) {
          this._asyncContext = currentAsyncContext;
          const cb = typeof args[args.length - 1] === 'function' ?
              args[args.length - 1] : null;
          this._connectCb = cb;
          this.connecting = true;
          const path = normalizeNetPath(netPathFromListenOrConnect(args));
          allocNetSocket(this);
          const server = netServers.get(path);
          if (server && !isNamedPipePath(path)) {
            const incoming = new module.Socket();
            incoming._asyncContext = server._asyncContext;
            incoming._connected = true;
            incoming._peer = this;
            allocNetSocket(incoming);
            this._peer = incoming;
            this._connected = true;
            this.connecting = false;
            queueMicrotask(() => {
              if (this._closed || incoming._closed) return;
              emitNetEvent(server, 'connection', incoming);
              if (this._closed || incoming._closed) return;
              emitNetEvent(this, 'connect');
              if (!this._closed && this._connectCb) {
                const callback = this._connectCb;
                this._connectCb = null;
                callNetCallback(this, callback);
              }
            });
            return this;
          }
          console.info('[xenon-net] connect', path);
          sendXenonNet('__xenon:net:connect', {path, fromId: this._id});
          return this;
        }
        write(data) {
          if (this._closed) return false;
          if (this._peer) {
            deliverNetBytes(this._peer, data);
            return true;
          }
          if (this._peerId) {
            sendXenonNet('__xenon:net:data', {
              toId: this._peerId,
              fromId: this._id,
              wire: bytesToNetWire(data),
            }, this._endpointId);
            return true;
          }
          if (this.connecting && !this._closed) {
            this._pendingWrites.push(
                Buffer.isBuffer(data) || data instanceof Uint8Array ?
                    Buffer.from(data) : data);
            return true;
          }
          return false;
        }
        end(data) {
          if (data !== undefined) {
            this.write(data);
          }
          closeNetSocket(this, false);
          return this;
        }
        destroy() {
          this.destroyed = true;
          closeNetSocket(this, false);
        }
        setTimeout() {}
        setNoDelay() {}
        setKeepAlive() {}
        ref() { return this; }
        unref() { return this; }
      },
      Server: class extends EventEmitter {
        listen(...args) {
          this._asyncContext = currentAsyncContext;
          const cb = typeof args[args.length - 1] === 'function' ?
              args[args.length - 1] : null;
          this._path = normalizeNetPath(netPathFromListenOrConnect(args));
          netServers.set(this._path, this);
          if (isNamedPipePath(this._path)) {
            this._closing = false;
            this._nativeId = 'server-m-' + (nextNetServerId++);
            this._listenCb = cb;
            nativeNetServers.set(this._nativeId, this);
            sendXenonNet('__xenon:net:listen', {
              serverId: this._nativeId,
              path: this._path,
            });
            return this;
          }
          queueMicrotask(() => {
            emitNetEvent(this, 'listening');
            if (cb) {
              callNetCallback(this, cb);
            }
          });
          return this;
        }
        close(cb) {
          if (this._path) {
            netServers.delete(this._path);
          }
          if (this._nativeId) {
            this._closing = true;
            this._closeCb = typeof cb === 'function' ? cb : null;
            this._listenCb = null;
            sendXenonNet('__xenon:net:unlisten', {serverId: this._nativeId});
            return this;
          }
          if (typeof cb === 'function') {
            callNetCallback(this, cb);
          }
          emitNetEvent(this, 'close');
          return this;
        }
        address() {
          return this._path || {port: 0, address: '127.0.0.1', family: 'IPv4'};
        }
        ref() { return this; }
        unref() { return this; }
      },
      createServer: (...args) => {
        const server = new module.Server();
        if (typeof args[0] === 'function') {
          server.on('connection', args[0]);
        }
        return server;
      },
      connect: (...args) => {
        const socket = new module.Socket();
        socket.connect(...args);
        return socket;
      },
      createConnection: (...args) => module.connect(...args),
      isIP: () => 0,
      isIPv4: () => false,
      isIPv6: () => false,
    };

    dispatchXenonNet = (channel, payload, sender) => {
      const msg = payload || {};
      const endpointId = sender && sender.endpointId ? sender.endpointId : '*';
      if (channel === '__xenon:net:listening') {
        const server = nativeNetServers.get(msg.serverId);
        if (server && !server._closing) {
          emitNetEvent(server, 'listening');
          if (server._listenCb) {
            const callback = server._listenCb;
            server._listenCb = null;
            callNetCallback(server, callback);
          }
        }
        return true;
      }
      if (channel === '__xenon:net:connection') {
        const server = nativeNetServers.get(msg.serverId);
        if (!msg.socketId) return true;
        if (!server || server._closing) {
          sendXenonNet('__xenon:net:close', {toId: msg.socketId});
          return true;
        }
        const incoming = new module.Socket();
        incoming._asyncContext = server._asyncContext;
        incoming._id = msg.socketId;
        incoming._peerId = msg.socketId;
        incoming._connected = true;
        netSockets.set(incoming._id, incoming);
        emitNetEvent(server, 'connection', incoming);
        return true;
      }
      if (channel === '__xenon:net:server-closed') {
        const server = nativeNetServers.get(msg.serverId);
        if (server) {
          nativeNetServers.delete(msg.serverId);
          server._nativeId = null;
          const callback = server._closeCb;
          server._closeCb = null;
          if (callback) callNetCallback(server, callback);
          emitNetEvent(server, 'close');
        }
        return true;
      }
      if (channel === '__xenon:net:connect') {
        const server = netServers.get(normalizeNetPath(msg.path));
        if (!server) {
          sendXenonNet('__xenon:net:error', {
            toId: msg.fromId,
            code: 'ECONNREFUSED',
            path: msg.path,
          }, endpointId);
          return true;
        }
        const incoming = new module.Socket();
        incoming._asyncContext = server._asyncContext;
        incoming._connected = true;
        incoming._peerId = msg.fromId;
        incoming._endpointId = endpointId;
        allocNetSocket(incoming);
        // A real net.Server exposes the accepted socket before the client can
        // observe connect. node-net-ipc installs its data parser here.
        emitNetEvent(server, 'connection', incoming);
        sendXenonNet('__xenon:net:connected', {
          toId: msg.fromId,
          peerId: incoming._id,
        }, endpointId);
        return true;
      }
      if (channel === '__xenon:net:connected') {
        const socket = netSockets.get(msg.toId);
        if (!socket) {
          if (msg.peerId) {
            sendXenonNet('__xenon:net:close', {toId: msg.peerId});
          }
          return true;
        }
        if (socket._connected) {
          return true;
        }
        socket._peerId = msg.peerId;
        socket._endpointId = endpointId;
        socket._connected = true;
        socket.connecting = false;
        emitNetEvent(socket, 'connect');
        if (!socket._closed && socket._connectCb) {
          const callback = socket._connectCb;
          socket._connectCb = null;
          callNetCallback(socket, callback);
        }
        // net.Socket.write() is allowed while connecting. Flush only after
        // connect listeners have run so protocol clients see normal ordering.
        flushPendingNetWrites(socket);
        return true;
      }
      if (channel === '__xenon:net:data') {
        const socket = netSockets.get(msg.toId);
        if (socket) {
          deliverNetBytes(socket, netWireToBytes(msg.wire));
        }
        return true;
      }
      if (channel === '__xenon:net:close') {
        const socket = netSockets.get(msg.toId);
        if (socket) {
          closeNetSocket(socket, true);
        }
        return true;
      }
      if (channel === '__xenon:net:error') {
        if (msg.serverId) {
          const server = nativeNetServers.get(msg.serverId);
          if (server) {
            netServers.delete(server._path);
            nativeNetServers.delete(msg.serverId);
            server._nativeId = null;
            const error = new Error(msg.code || 'net error');
            error.code = msg.code;
            emitNetEvent(server, 'error', error);
          }
          return true;
        }
        const socket = netSockets.get(msg.toId);
        if (socket && !socket._closed) {
          const err = new Error(msg.code || 'net error');
          err.code = msg.code;
          socket.connecting = false;
          socket._pendingWrites.length = 0;
          emitNetEvent(socket, 'error', err);
          closeNetSocket(socket, false);
        }
        return true;
      }
      return false;
    };

    return module;
  })();
  function createMainNetwork(nativeRequest, nativeAbort) {
    function error(code, message, name = 'Error') {
      const result = new Error(message); result.code = code; result.name = name; return result;
    }
    const unsupported = method => { throw error('ERR_NOT_SUPPORTED', method + ' is not supported'); };
    function networkError(value) {
      if (value && value.code) return value;
      const message = String(value && value.message || value);
      const code = /(?:ABORT_ERR|ERR_[A-Z_]+|EINVAL)/.exec(message);
      return error(code ? code[0] : 'ERR_NETWORK', message);
    }
    function binary(value) {
      if (value == null) return null;
      if (typeof value === 'string') return Buffer.from(value);
      if (ArrayBuffer.isView(value)) return Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      if (value instanceof ArrayBuffer) return Buffer.from(new Uint8Array(value));
      if (value instanceof URLSearchParams) return Buffer.from(value.toString());
      return unsupported('Streaming, Blob and FormData request bodies');
    }
    class Headers {
      constructor(init) {
        this._values = new Map();
        if (init == null) return;
        if (typeof init[Symbol.iterator] === 'function') {
          for (const pair of init) {
            if (!pair || pair.length !== 2) throw new TypeError('Invalid header entry');
            this.append(pair[0], pair[1]);
          }
        } else {
          for (const [key, value] of Object.entries(init)) this.append(key, value);
        }
      }
      _name(name) {
        name = String(name).toLowerCase();
        if (!/^[!#$%&'*+.^_\x60|~0-9a-z-]+$/.test(name)) throw new TypeError('Invalid header name');
        return name;
      }
      _value(value) {
        value = String(value).trim();
        if (/[\r\n\0]/.test(value)) throw new TypeError('Invalid header value');
        return value;
      }
      append(name, value) {
        name = this._name(name); value = this._value(value);
        this._values.set(name, this._values.has(name) ? this._values.get(name) + ', ' + value : value);
      }
      set(name, value) { this._values.set(this._name(name), this._value(value)); }
      get(name) { return this._values.get(this._name(name)) ?? null; }
      has(name) { return this._values.has(this._name(name)); }
      delete(name) { this._values.delete(this._name(name)); }
      *entries() { yield* [...this._values].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0); }
      *keys() { for (const [key] of this.entries()) yield key; }
      *values() { for (const [, value] of this.entries()) yield value; }
      forEach(callback, self) { for (const [key, value] of this.entries()) callback.call(self, value, key, this); }
      [Symbol.iterator]() { return this.entries(); }
      get [Symbol.toStringTag]() { return 'Headers'; }
    }
    class AbortSignal extends EventEmitter {
      constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
      addEventListener(name, callback, options) {
        if (options && options.once) this.once(name, callback); else this.on(name, callback);
      }
      removeEventListener(name, callback) { this.removeListener(name, callback); }
      throwIfAborted() { if (this.aborted) throw this.reason; }
      static abort(reason) { const controller = new AbortController(); controller.abort(reason); return controller.signal; }
      static timeout(delay) {
        if (!Number.isInteger(delay) || delay < 0) throw new RangeError('Invalid timeout');
        const controller = new AbortController();
        setTimeout(() => controller.abort(error('ETIMEDOUT', 'The operation timed out', 'TimeoutError')), delay);
        return controller.signal;
      }
      get [Symbol.toStringTag]() { return 'AbortSignal'; }
    }
    class AbortController {
      constructor() { this.signal = new AbortSignal(); }
      abort(reason = error('ABORT_ERR', 'The operation was aborted', 'AbortError')) {
        const signal = this.signal;
        if (signal.aborted) return;
        signal.aborted = true; signal.reason = reason;
        const event = {type: 'abort', target: signal};
        signal.emit('abort', event);
        if (typeof signal.onabort === 'function') signal.onabort.call(signal, event);
      }
      get [Symbol.toStringTag]() { return 'AbortController'; }
    }
    class Body {
      _setBody(value) {
        this._bytes = binary(value);
        if (this._bytes && this._bytes.length > 32 * 1024 * 1024) throw error('ERR_BUFFER_TOO_LARGE', 'HTTP upload exceeds 32 MiB');
        this.bodyUsed = false;
      }
      _consume() {
        if (this.bodyUsed) throw new TypeError('Body has already been consumed');
        this.bodyUsed = true;
        return this._bytes || Buffer.alloc(0);
      }
      async text() {
        // Fetch uses the WHATWG UTF-8 decoder, including BOM removal and
        // replacement of malformed sequences. Buffer.toString keeps the BOM.
        const bytes = this._consume();
        const text = [];
        let i = bytes.length >= 3 && bytes[0] === 239 && bytes[1] === 187 && bytes[2] === 191 ? 3 : 0;
        while (i < bytes.length) {
          const first = bytes[i++];
          if (first < 128) { text.push(String.fromCharCode(first)); continue; }
          let needed = first >= 194 && first <= 223 ? 1 : first >= 224 && first <= 239 ? 2 : first >= 240 && first <= 244 ? 3 : 0;
          if (!needed) { text.push('\uFFFD'); continue; }
          let code = first & (needed === 1 ? 31 : needed === 2 ? 15 : 7);
          let valid = true;
          for (let part = 0; part < needed; ++part) {
            const next = bytes[i];
            const low = part === 0 && first === 224 ? 160 : part === 0 && first === 240 ? 144 : 128;
            const high = part === 0 && first === 237 ? 159 : part === 0 && first === 244 ? 143 : 191;
            if (next === undefined || next < low || next > high) { valid = false; break; }
            ++i; code = (code << 6) | (next & 63);
          }
          text.push(valid ? String.fromCodePoint(code) : '\uFFFD');
        }
        return text.join('');
      }
      async json() { return JSON.parse(await this.text()); }
      async arrayBuffer() {
        const value = this._consume();
        return value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength);
      }
      get body() { return unsupported('Streaming response bodies'); }
    }
    class Request extends Body {
      constructor(input, init = {}) {
        super();
        const source = input instanceof Request ? input : null;
        if (source && source.bodyUsed) throw new TypeError('Request body has already been consumed');
        this.url = String(source ? source.url : input);
        const parsed = new URL(this.url);
        if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') throw new TypeError('fetch requires an HTTP(S) URL');
        this.url = parsed.href;
        this.method = String(init.method || (source && source.method) || 'GET').toUpperCase();
        if (!/^[!#$%&'*+.^_\x60|~0-9a-z-]+$/i.test(this.method)) throw new TypeError('Invalid HTTP method');
        this.headers = new Headers(init.headers === undefined && source ? source.headers : init.headers);
        const body = init.body === undefined && source ? source._bytes : init.body;
        if (body != null && (this.method === 'GET' || this.method === 'HEAD')) throw new TypeError('GET and HEAD requests cannot have a body');
        this._setBody(body);
        if (!this.headers.has('content-type') && typeof body === 'string') this.headers.set('content-type', 'text/plain;charset=UTF-8');
        if (!this.headers.has('content-type') && body instanceof URLSearchParams) this.headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');
        this.signal = init.signal === undefined && source ? source.signal : init.signal;
        if (this.signal != null && typeof this.signal.addEventListener !== 'function') throw new TypeError('Invalid AbortSignal');
        this.redirect = init.redirect || (source && source.redirect) || 'follow';
        if (!['follow', 'error'].includes(this.redirect)) unsupported('fetch redirect mode ' + this.redirect);
        this._credentials = init.credentials || (source && source.credentials) || 'same-origin';
        if (!['omit', 'include', 'same-origin'].includes(this._credentials)) throw new TypeError('Invalid credentials mode');
        if (init.integrity) unsupported('fetch integrity');
      }
      clone() { return new Request(this); }
      get credentials() { return this._credentials; }
      get [Symbol.toStringTag]() { return 'Request'; }
    }
    class Response extends Body {
      constructor(body = null, init = {}) {
        super();
        this.status = init.status === undefined ? 200 : Number(init.status);
        if (!Number.isInteger(this.status) || this.status < 200 || this.status > 599) throw new RangeError('Invalid response status');
        this.statusText = String(init.statusText || '');
        this.headers = new Headers(init.headers);
        this.url = init.url || '';
        this.redirected = Boolean(init.redirected);
        this.type = 'basic';
        this._setBody(body);
      }
      get ok() { return this.status >= 200 && this.status < 300; }
      clone() {
        if (this.bodyUsed) throw new TypeError('Response body has already been consumed');
        return new Response(this._bytes, this);
      }
      get [Symbol.toStringTag]() { return 'Response'; }
    }
    function fetch(input, init) {
      let request;
      try { request = new Request(input, init); }
      catch (failure) { return Promise.reject(failure); }
      const signal = request.signal;
      if (signal && signal.aborted) return Promise.reject(signal.reason || error('ABORT_ERR', 'The operation was aborted', 'AbortError'));
      if (input instanceof Request) input.bodyUsed = true;
      return new Promise((resolve, reject) => {
        let operation;
        let finished = false;
        const cleanup = () => { if (signal) signal.removeEventListener('abort', abort); };
        const fail = reason => { if (finished) return; finished = true; cleanup(); reject(reason); };
        const abort = () => {
          fail(signal.reason || error('ABORT_ERR', 'The operation was aborted', 'AbortError'));
          if (operation) nativeAbort(operation.id);
        };
        if (signal) signal.addEventListener('abort', abort, {once: true});
        try {
          operation = nativeRequest({url: request.url, method: request.method,
            headers: Object.fromEntries(request.headers),
            bodyBase64: request._bytes ? request._bytes.toString('base64') : '',
            useSessionCookies: request.credentials === 'include', redirect: request.redirect,
            timeoutMs: 300000});
          Promise.resolve(operation.promise).then(value => {
            if (finished) return;
            let response;
            try {
              response = new Response(Buffer.from(value.bodyBase64 || '', 'base64'), {
                status: value.statusCode, statusText: value.statusMessage,
                headers: value.headers, url: value.finalUrl || request.url,
                redirected: !!value.finalUrl && value.finalUrl !== request.url});
            } catch (failure) { fail(failure); return; }
            finished = true; cleanup(); resolve(response);
          }, failure => fail(networkError(failure)));
        } catch (failure) { fail(networkError(failure)); }
      });
    }
    class IncomingMessage extends EventEmitter {
      constructor(response) {
        super();
        this.statusCode = response.statusCode;
        this.statusMessage = response.statusMessage || '';
        this.headers = Object.fromEntries(new Headers(response.headers));
        // Chromium has already decoded compressed response bodies. Expose
        // headers matching the delivered bytes so Node consumers do not unzip
        // them twice. Fetch retains the original response headers separately.
        if (this.headers['content-encoding']) {
          delete this.headers['content-encoding'];
          delete this.headers['content-length'];
        }
        this.rawHeaders = Object.entries(this.headers).flat();
        this.url = response.finalUrl || '';
        this.httpVersion = response.httpVersion || '';
        this.complete = false; this.readableEnded = false; this.destroyed = false;
        this._data = Buffer.from(response.bodyBase64 || '', 'base64');
        this._flowing = false; this._scheduled = false; this._delivered = false;
      }
      on(name, listener) {
        super.on(name, listener);
        if (name === 'data' && !this._paused) { this._flowing = true; this._pump(); }
        return this;
      }
      addListener(name, listener) { return this.on(name, listener); }
      setEncoding(encoding) { this._encoding = encoding || 'utf8'; return this; }
      pause() { this._paused = true; this._flowing = false; return this; }
      resume() { this._paused = false; this._flowing = true; this._pump(); return this; }
      _pump() {
        if (this._scheduled || this.destroyed || this.readableEnded) return;
        this._scheduled = true;
        queueMicrotask(() => {
          this._scheduled = false;
          if (!this._flowing || this.destroyed || this.readableEnded) return;
          if (!this._delivered) {
            this._delivered = true;
            if (this._data.length) this.emit('data', this._encoding ? this._data.toString(this._encoding) : this._data);
            this._data = Buffer.alloc(0);
            this._pump();
            return;
          }
          this.complete = true; this.readableEnded = true;
          this.emit('end'); this._close();
        });
      }
      pipe(destination, options = {}) {
        this.on('data', chunk => {
          if (destination.write(chunk) === false) {
            this.pause(); destination.once('drain', () => this.resume());
          }
        });
        if (options.end !== false) this.once('end', () => destination.end());
        destination.emit('pipe', this);
        return destination;
      }
      destroy(reason) {
        if (this.destroyed) return this;
        this.destroyed = true; this._data = Buffer.alloc(0);
        queueMicrotask(() => { if (reason) this.emit('error', reason); this._close(); });
        return this;
      }
      _close() { if (!this._closed) { this._closed = true; this.emit('close'); } }
    }
    function httpModuleFor(protocol) {
      class ClientRequest extends EventEmitter {
        constructor(input, options, callback) {
          super();
          if (typeof options === 'function') { callback = options; options = undefined; }
          let settings = {};
          let url;
          if (typeof input === 'string' || input instanceof URL) url = new URL(String(input));
          else settings = {...input};
          settings = {...settings, ...(options || {})};
          if (!url) {
            const host = settings.hostname || settings.host || 'localhost';
            const port = settings.port ? ':' + settings.port : '';
            url = new URL((settings.protocol || protocol) + '//' + host + port + (settings.path || '/'));
          } else if (settings.path) url = new URL(settings.path, url.href);
          if (url.protocol !== protocol) throw error('ERR_INVALID_PROTOCOL', 'Protocol ' + url.protocol + ' is not supported by this module');
          this._url = url.href;
          this.method = String(settings.method || 'GET').toUpperCase();
          this._headers = new Headers(settings.headers);
          if (settings.auth && !this._headers.has('authorization')) this._headers.set('authorization', 'Basic ' + Buffer.from(String(settings.auth)).toString('base64'));
          this._chunks = []; this._length = 0; this._sent = false;
          this.destroyed = false; this.aborted = false; this.writableEnded = false;
          this._closed = false; this._timeout = Number(settings.timeout) || 0;
          if (callback) this.once('response', callback);
          if (settings.signal) {
            this._signal = settings.signal;
            this._onAbort = () => this.destroy(settings.signal.reason || error('ABORT_ERR', 'The operation was aborted', 'AbortError'));
            if (settings.signal.aborted) queueMicrotask(this._onAbort);
            else settings.signal.addEventListener('abort', this._onAbort, {once: true});
          }
        }
        setHeader(name, value) { if (this._sent) throw error('ERR_HTTP_HEADERS_SENT', 'Headers already sent'); this._headers.set(name, value); return this; }
        getHeader(name) { return this._headers.get(name) ?? undefined; }
        getHeaders() { return Object.fromEntries(this._headers); }
        hasHeader(name) { return this._headers.has(name); }
        removeHeader(name) { if (this._sent) throw error('ERR_HTTP_HEADERS_SENT', 'Headers already sent'); this._headers.delete(name); }
        write(chunk, encoding, callback) {
          if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
          if (this.writableEnded || this.destroyed) throw error('ERR_STREAM_WRITE_AFTER_END', 'write after end');
          const data = typeof chunk === 'string' ? Buffer.from(chunk, encoding) : binary(chunk);
          if (!data) throw new TypeError('Invalid HTTP body chunk');
          if (this._length + data.length > 32 * 1024 * 1024) throw error('ERR_BUFFER_TOO_LARGE', 'HTTP upload exceeds 32 MiB');
          this._chunks.push(data); this._length += data.length;
          if (callback) queueMicrotask(callback);
          return true;
        }
        end(chunk, encoding, callback) {
          if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
          else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
          if (this.writableEnded || this.destroyed) return this;
          if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
          this.writableEnded = true; this._sent = true;
          if (callback) this.once('finish', callback);
          const request = {url: this._url, method: this.method, headers: this.getHeaders(),
            bodyBase64: Buffer.concat(this._chunks).toString('base64'), timeoutMs: 300000,
            redirect: 'error'};
          this._chunks.length = 0;
          try {
            this._operation = nativeRequest(request);
            this._armTimeout();
            queueMicrotask(() => { if (!this.destroyed) this.emit('finish'); });
            Promise.resolve(this._operation.promise).then(value => {
              if (this.destroyed) return;
              clearTimeout(this._timer);
              this._operation = null;
              const incoming = new IncomingMessage(value);
              this.res = incoming;
              incoming.once('end', () => this._close());
              this.emit('response', incoming);
              if (!incoming._data.length) incoming.resume();
            }, reason => { if (!this.destroyed) this.destroy(networkError(reason)); });
          } catch (reason) { this.destroy(networkError(reason)); }
          return this;
        }
        _armTimeout() {
          clearTimeout(this._timer);
          if (this._timeout > 0 && this._sent && !this.destroyed) this._timer = setTimeout(() => this.emit('timeout'), this._timeout);
        }
        setTimeout(delay, callback) {
          if (!Number.isFinite(delay) || delay < 0) throw new RangeError('Invalid timeout');
          this._timeout = delay;
          if (callback) this.once('timeout', callback);
          this._armTimeout(); return this;
        }
        _close() {
          if (this._closed) return;
          this._closed = true; clearTimeout(this._timer);
          if (this._signal) this._signal.removeEventListener('abort', this._onAbort);
          this.emit('close');
        }
        destroy(reason) {
          if (this.destroyed) return this;
          this.destroyed = true; this._chunks.length = 0; clearTimeout(this._timer);
          if (this._operation) nativeAbort(this._operation.id);
          if (this.res) this.res.destroy();
          queueMicrotask(() => { if (reason) this.emit('error', reason); this._close(); });
          return this;
        }
        abort() { if (this.aborted) return; this.aborted = true; this.emit('abort'); this.destroy(); }
        setNoDelay() { return unsupported('Per-request TCP no-delay settings'); }
        setSocketKeepAlive() { return unsupported('Per-request TCP keepalive settings'); }
        flushHeaders() { return unsupported('Streaming request headers'); }
      }
      class Agent {
        constructor(options = {}) {
          if (options.rejectUnauthorized === false || options.ca || options.cert || options.key || options.proxy) unsupported('Custom HTTP Agent TLS/proxy settings');
          this.options = {...options}; this.protocol = protocol;
        }
        destroy() {}
      }
      const result = {ClientRequest, IncomingMessage, Agent, globalAgent: new Agent(),
        request: (input, options, callback) => new ClientRequest(input, options, callback),
        get(input, options, callback) { const request = new ClientRequest(input, options, callback); request.end(); return request; },
        createServer: () => unsupported('HTTP createServer')};
      return result;
    }
    return {http: httpModuleFor('http:'), https: httpModuleFor('https:'),
      globals: {fetch, Headers, Request, Response, AbortController, AbortSignal}};
  }

  const mainNetwork = createMainNetwork(
      request => __xenonHttpRequest(request), id => __xenonHttpAbort(id));
  const httpModule = mainNetwork.http;
  const httpsModule = mainNetwork.https;
  Object.assign(globalThis, mainNetwork.globals);
  // HTTP/2 protocol constants are data, not evidence of an available session.
  // Keep this module distinct from HTTP/1 and fail when transport is requested.
  const http2Module = (() => {
    const unavailable = operation => {
      const error = new Error(`http2.${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    class Http2ServerRequest extends EventEmitter {
      constructor() { super(); unavailable('Http2ServerRequest'); }
    }
    class Http2ServerResponse extends EventEmitter {
      constructor() { super(); unavailable('Http2ServerResponse'); }
    }
    const constants = {
      HTTP2_HEADER_SCHEME: ':scheme',
      HTTP2_HEADER_METHOD: ':method',
      HTTP2_HEADER_PATH: ':path',
      HTTP2_HEADER_STATUS: ':status',
      HTTP2_HEADER_AUTHORITY: ':authority',
    };
    return {
      constants,
      sensitiveHeaders: Symbol('sensitiveHeaders'),
      Http2ServerRequest, Http2ServerResponse,
      connect: () => unavailable('connect'),
      createServer: () => unavailable('createServer'),
      createSecureServer: () => unavailable('createSecureServer'),
      performServerHandshake: () => unavailable('performServerHandshake'),
      getPackedSettings: () => unavailable('getPackedSettings'),
      getUnpackedSettings: () => unavailable('getUnpackedSettings'),
      getDefaultSettings: () => Object.assign(Object.create(null), {
        headerTableSize: 4096, enablePush: true, initialWindowSize: 65535,
        maxFrameSize: 16384, maxConcurrentStreams: 4294967295,
        maxHeaderSize: 65535, maxHeaderListSize: 65535,
        enableConnectProtocol: false,
      }),
    };
  })();
  const ttyModule = { isatty: () => false };
  // These modules are importable for dependency discovery. TLS operations require
  // a TLS transport; a plain net socket must never stand in for encryption.
  const tlsModule = (() => {
    const unavailable = operation => {
      const error = new Error(`TLS ${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    class TLSSocket extends EventEmitter {
      constructor() { super(); unavailable('TLSSocket'); }
    }
    class Server extends EventEmitter {
      constructor() { super(); unavailable('Server'); }
    }
    class SecureContext {
      constructor() { unavailable('SecureContext'); }
    }
    return {
      TLSSocket, Server, SecureContext,
      connect: () => unavailable('connect'),
      createServer: () => unavailable('createServer'),
      createSecureContext: () => unavailable('createSecureContext'),
      checkServerIdentity: () => unavailable('checkServerIdentity'),
      getCiphers: () => unavailable('getCiphers'),
    };
  })();

  // Non-terminal readline consumes real stream data and preserves UTF-8 and
  // CRLF boundaries across chunks. Interactive terminal editing is separate.
  const readlineModule = (() => {
    const unsupported = operation => {
      const error = new Error(`readline ${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    const invalid = name => {
      const error = new TypeError(`Invalid readline ${name}`);
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    };
    // The main isolate's TextDecoder fallback is not incremental, so keep the
    // UTF-8 decoder state here instead of losing split multibyte characters.
    function decodeChunk(state, chunk) {
      const bytes = ArrayBuffer.isView(chunk) ?
          new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength) :
          new Uint8Array(chunk);
      let result = '';
      for (let i = 0; i < bytes.length; ++i) {
        const byte = bytes[i];
        if (!state.needed) {
          if (byte <= 0x7f) { result += String.fromCharCode(byte); continue; }
          state.seen = 0;
          state.lower = 0x80;
          state.upper = 0xbf;
          if (byte >= 0xc2 && byte <= 0xdf) {
            state.needed = 1; state.point = byte & 0x1f;
          } else if (byte >= 0xe0 && byte <= 0xef) {
            state.needed = 2; state.point = byte & 0x0f;
            if (byte === 0xe0) state.lower = 0xa0;
            if (byte === 0xed) state.upper = 0x9f;
          } else if (byte >= 0xf0 && byte <= 0xf4) {
            state.needed = 3; state.point = byte & 7;
            if (byte === 0xf0) state.lower = 0x90;
            if (byte === 0xf4) state.upper = 0x8f;
          } else {
            result += '\ufffd';
          }
        } else if (byte < state.lower || byte > state.upper) {
          state.needed = 0;
          result += '\ufffd';
          --i;
        } else {
          state.lower = 0x80;
          state.upper = 0xbf;
          state.point = (state.point << 6) | (byte & 0x3f);
          if (++state.seen === state.needed) {
            result += String.fromCodePoint(state.point);
            state.needed = 0;
          }
        }
      }
      return result;
    }
    class Interface extends EventEmitter {
      constructor(options, output, completer, terminal) {
        super();
        if (options && typeof options.on === 'function') {
          options = {input: options, output, completer, terminal};
        }
        options = options || {};
        const input = options.input;
        if (!input || typeof input.on !== 'function' ||
            typeof input.removeListener !== 'function') invalid('input');
        this.input = input;
        this.output = options.output;
        this.terminal = options.terminal === undefined ?
            !!(this.output && this.output.isTTY) : !!options.terminal;
        if (this.terminal) unsupported('terminal editing');
        this.closed = false;
        this.paused = false;
        this.line = '';
        this._prompt = options.prompt === undefined ? '> ' : String(options.prompt);
        this._question = null;
        this._decoder = {needed: 0};
        this._lastCR = null;
        this._crlfDelay = Math.max(100, Number(options.crlfDelay) || 100);
        this._onData = chunk => {
          if (this.closed) return;
          const text = typeof chunk === 'string' ? chunk :
              decodeChunk(this._decoder, chunk);
          this._consume(text);
        };
        this._onEnd = () => {
          if (this.closed) return;
          // Node readline emits its buffered text without flushing an
          // incomplete UTF-8 byte sequence when the input ends.
          this._decoder.needed = 0;
          if (this.line) {
            const line = this.line;
            this.line = '';
            this._emitLine(line);
          }
          this.close();
        };
        this._onError = error => this.emit('error', error);
        input.on('data', this._onData);
        input.on('end', this._onEnd);
        input.on('error', this._onError);
        this._signal = options.signal;
        this._onAbort = () => this.close();
        if (this._signal) {
          if (this._signal.aborted) queueMicrotask(this._onAbort);
          else this._signal.addEventListener('abort', this._onAbort, {once: true});
        }
        if (typeof input.resume === 'function') input.resume();
      }
      _emitLine(line) {
        if (this.closed) return;
        if (this._question) {
          const callback = this._question;
          this._question = null;
          callback(line);
        } else {
          this.emit('line', line);
        }
      }
      _consume(text) {
        for (const char of text) {
          if (this.closed) break;
          if (char === '\n' && this._lastCR !== null &&
              Date.now() - this._lastCR <= this._crlfDelay) {
            this._lastCR = null;
            continue;
          }
          this._lastCR = null;
          if (char === '\r' || char === '\n') {
            const line = this.line;
            this.line = '';
            if (char === '\r') this._lastCR = Date.now();
            this._emitLine(line);
          } else {
            this.line += char;
          }
        }
      }
      close() {
        if (this.closed) return;
        this.closed = true;
        this._question = null;
        this.input.removeListener('data', this._onData);
        this.input.removeListener('end', this._onEnd);
        this.input.removeListener('error', this._onError);
        if (this._signal) this._signal.removeEventListener('abort', this._onAbort);
        if (typeof this.input.pause === 'function') this.input.pause();
        this.emit('close');
      }
      pause() {
        if (!this.paused) {
          this.paused = true;
          if (typeof this.input.pause === 'function') this.input.pause();
          this.emit('pause');
        }
        return this;
      }
      resume() {
        if (this.paused && !this.closed) {
          this.paused = false;
          if (typeof this.input.resume === 'function') this.input.resume();
          this.emit('resume');
        }
        return this;
      }
      setPrompt(prompt) { this._prompt = String(prompt); }
      getPrompt() { return this._prompt; }
      prompt() {
        if (this.closed) return;
        this.resume();
        if (this.output) this.output.write(this._prompt);
      }
      question(query, options, callback) {
        if (typeof options === 'function') callback = options;
        else if (options && options.signal) unsupported('question AbortSignal');
        if (typeof callback !== 'function') invalid('question callback');
        if (this.closed) {
          const error = new Error('readline was closed');
          error.code = 'ERR_USE_AFTER_CLOSE';
          throw error;
        }
        if (this._question) return;
        this._question = callback;
        this.resume();
        if (this.output) this.output.write(String(query));
      }
      write(data, key) {
        if (key !== undefined) unsupported('keypress editing');
        this.resume();
        this._onData(data);
      }
    }
    return {
      Interface,
      createInterface: (...args) => new Interface(...args),
      emitKeypressEvents: () => unsupported('emitKeypressEvents'),
      clearLine: () => unsupported('clearLine'),
      clearScreenDown: () => unsupported('clearScreenDown'),
      cursorTo: () => unsupported('cursorTo'),
      moveCursor: () => unsupported('moveCursor'),
    };
  })();
  function unsupportedDnsError(hostname) {
    const error = new Error(
        `DNS resolution is unavailable in the hosted runtime: ${hostname}`);
    error.code = 'ENOTSUP';
    error.syscall = 'getaddrinfo';
    error.hostname = String(hostname || '');
    return error;
  }
  const dnsModule = {
    lookup(hostname, options, callback) {
      if (typeof options === 'function') {
        callback = options;
      }
      if (typeof callback === 'function') {
        queueMicrotask(() => callback(unsupportedDnsError(hostname)));
      }
    },
    resolve4(hostname, callback) {
      if (typeof callback === 'function') {
        queueMicrotask(() => callback(unsupportedDnsError(hostname)));
      }
    },
    resolve6(hostname, callback) {
      if (typeof callback === 'function') {
        queueMicrotask(() => callback(unsupportedDnsError(hostname)));
      }
    },
    resolve(hostname, rrtype, callback) {
      if (typeof rrtype === 'function') {
        callback = rrtype;
      }
      if (typeof callback === 'function') {
        queueMicrotask(() => callback(unsupportedDnsError(hostname)));
      }
    },
    promises: {
      lookup: async hostname => { throw unsupportedDnsError(hostname); },
      resolve4: async hostname => { throw unsupportedDnsError(hostname); },
      resolve6: async hostname => { throw unsupportedDnsError(hostname); },
    },
  };
  const querystringModule = {
    parse(str) {
      const out = {};
      String(str || '').replace(/^\?/, '').split('&').forEach(pair => {
        if (!pair) return;
        const idx = pair.indexOf('=');
        const key = decodeURIComponent(idx < 0 ? pair : pair.slice(0, idx));
        const val = decodeURIComponent(idx < 0 ? '' : pair.slice(idx + 1));
        out[key] = val;
      });
      return out;
    },
    stringify(obj) {
      return Object.keys(obj || {}).map(k =>
          encodeURIComponent(k) + '=' + encodeURIComponent(obj[k] == null ? '' : obj[k])
      ).join('&');
    },
    escape: encodeURIComponent,
    unescape: decodeURIComponent,
  };
  const timersModule = {
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    setImmediate: (fn, ...args) => setTimeout(fn, 0, ...args),
    clearImmediate: clearTimeout,
  };
  const performanceTimeOrigin = Date.now();
  let lastPerformanceNow = 0;
  const performanceModule = {
    timeOrigin: performanceTimeOrigin,
    now() {
      // Date is the only clock exposed by a plain V8 context. Clamp elapsed
      // time so the Node-compatible clock remains monotonic if the wall clock
      // is adjusted while the hosted application is running.
      lastPerformanceNow = Math.max(
          lastPerformanceNow, Date.now() - performanceTimeOrigin);
      return lastPerformanceNow;
    },
    toJSON() {
      return {timeOrigin: performanceTimeOrigin};
    },
  };
  const perfHooksModule = {performance: performanceModule};

  const processHrtime = time => {
    let nanoseconds = BigInt(Math.floor(performanceModule.now() * 1e6));
    if (time !== undefined) {
      if (!Array.isArray(time) || time.length !== 2) {
        throw new TypeError('The "time" argument must be an Array');
      }
      nanoseconds -= BigInt(time[0]) * 1000000000n + BigInt(time[1]);
    }
    return [
      Number(nanoseconds / 1000000000n),
      Number(nanoseconds % 1000000000n),
    ];
  };
  processHrtime.bigint = () =>
    BigInt(Math.floor(performanceModule.now() * 1e6));

  globalThis.__xenonEvents = EventEmitter;
  globalThis.__xenonAsyncHooks = asyncHooksModule;
  globalThis.__xenonPath = pathModule;
  globalThis.__xenonPathWin32 = win32;
  globalThis.__xenonPathPosix = posix;
  globalThis.__xenonOs = osModule;
  globalThis.__xenonElectron = electronModule;
  globalThis.__xenonFs = fsModule;
  globalThis.__xenonFsPromises = fsPromises;
  globalThis.__xenonBuffer = {Buffer};
  globalThis.__xenonCrypto = cryptoModule;
  globalThis.__xenonUtil = utilModule;
  globalThis.__xenonUrl = urlModule;
  globalThis.__xenonStream = streamModule;
  globalThis.__xenonStringDecoder = stringDecoderModule;
  globalThis.__xenonChildProcess = childProcessModule;
  globalThis.__xenonAssert = assert;
  globalThis.__xenonConstants = constantsModule;
  globalThis.__xenonZlib = zlibModule;
  globalThis.__xenonNet = netModule;
  globalThis.__xenonTls = tlsModule;
  globalThis.__xenonHttp = httpModule;
  globalThis.__xenonHttp2 = http2Module;
  globalThis.__xenonHttps = httpsModule;
  globalThis.__xenonTty = ttyModule;
  globalThis.__xenonReadline = readlineModule;
  globalThis.__xenonDns = dnsModule;
  globalThis.__xenonQuerystring = querystringModule;
  globalThis.__xenonTimers = timersModule;
  globalThis.__xenonPerfHooks = perfHooksModule;
  globalThis.performance = performanceModule;

  const pendingAppEvents = [];
  let appEventsReady = false;
  let appEventsStopped = false;
  let dispatchingAppEvents = false;
  function dispatchPendingAppEvents() {
    if (!appEventsReady || appEventsStopped || dispatchingAppEvents) return;
    dispatchingAppEvents = true;
    try {
      while (!appEventsStopped && pendingAppEvents.length > 0) {
        const [eventName, args] = pendingAppEvents.shift();
        app.emit(eventName, createBrowserWindowEvent(app), ...args);
      }
    } finally {
      dispatchingAppEvents = false;
    }
  }
  globalThis.__xenonDispatchAppEvent = (eventName, args = []) => {
    if (appEventsStopped) return;
    if (typeof eventName !== 'string' || !Array.isArray(args)) {
      throw new TypeError('Application events require a name and argument array');
    }
    pendingAppEvents.push([eventName, args.slice()]);
    dispatchPendingAppEvents();
  };
  globalThis.__xenonMarkAppReady = () => {
    if (appReady || appExitState !== 'running') return;
    appReady = true;
    resolveAppReady();
    try {
      app.emit('ready', {}, {});
    } finally {
      // Applications commonly install their activate listener in whenReady().
      // Let those promise callbacks run before delivering an early activation.
      queueMicrotask(() => {
        if (appEventsStopped) return;
        appEventsReady = true;
        dispatchPendingAppEvents();
      });
    }
  };
  globalThis.__xenonShutdownApp = () => {
    // Native teardown already owns container termination. Notify and clean up
    // once without recursively requesting another native exit.
    forceAppExit(0, null, true);
  };
  const hostedExecPath = String(__xenonExecPath || '');
  const processEmitter = new EventEmitter();
  globalThis.process = {
    // Electron main-process apps expect process.type === 'browser'.
    type: 'browser',
    argv: [hostedExecPath],
    execPath: hostedExecPath,
    pid: __xenonPid,
    platform: __xenonPlatform,
    arch: __xenonArch,
    env: __xenonEnv,
    stdin: { fd: 0, isTTY: false, read() { return null; }, on() { return this; } },
    stdout: { fd: 1, isTTY: false, write(data) { return true; }, on() { return this; } },
    stderr: { fd: 2, isTTY: false, write(data) { return true; }, on() { return this; } },
    versions: {
      electron: '0.0.0-compat',
      chrome: __xenonChromeVersion,
      node: '0.0.0-compat',
      v8: __xenonV8Version,
    },
    version: 'v0.0.0-compat',
    cwd: () => __xenonAppPath,
    // Electron process.getSystemVersion() — OS release string.
    getSystemVersion: () =>
        (typeof __xenonOsRelease === 'string' && __xenonOsRelease) ||
        (osModule.release && osModule.release()) ||
        '',
    _linkedBinding: (name) => {
      if (name === 'electron_common_v8_util' || name === 'v8_util') {
        return {
          getHiddenValue: (obj, key) => (obj ? obj['__v8_' + key] : undefined),
          setHiddenValue: (obj, key, val) => {
            if (obj) {
              obj['__v8_' + key] = val;
            }
          },
          deleteHiddenValue: (obj, key) => {
            if (obj) {
              delete obj['__v8_' + key];
            }
          },
          requestGarbageCollectionForTesting: () => {},
        };
      }
      return {};
    },
    electronBinding: (name) => globalThis.process._linkedBinding(
        'electron_common_' + name),
    uptime: () => performanceModule.now() / 1000,
    hrtime: processHrtime,
    nextTick(callback, ...args) {
      if (typeof callback !== 'function') {
        const error = new TypeError('The "callback" argument must be a function');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      queueMicrotask(() => callback(...args));
    },
    on: (evt, fn) => processEmitter.on(evt, fn),
    addListener: (evt, fn) => processEmitter.addListener(evt, fn),
    once: (evt, fn) => processEmitter.once(evt, fn),
    emit: (evt, ...args) => processEmitter.emit(evt, ...args),
    removeListener: (evt, fn) => processEmitter.removeListener(evt, fn),
    off: (evt, fn) => processEmitter.off(evt, fn),
    exit: (code = 0) => app.exit(code),
  };
  process.on('unhandledRejection', err => {
    const message = (err && err.stack) || String(err);
    if (typeof __xenonLog === 'function') {
      __xenonLog('unhandledRejection ' + message);
    }
  });
  process.on('uncaughtException', err => {
    const message = (err && err.stack) || String(err);
    if (typeof __xenonLog === 'function') {
      __xenonLog('uncaughtException ' + message);
    }
  });
  globalThis.global = globalThis;
  globalThis.GLOBAL = globalThis;
  globalThis.root = globalThis;
  globalThis.TextEncoder = utilModule.TextEncoder;
  globalThis.TextDecoder = utilModule.TextDecoder;
  globalThis.setImmediate = typeof setImmediate === 'function' ? setImmediate : (fn, ...args) => setTimeout(fn, 0, ...args);
  globalThis.clearImmediate = typeof clearImmediate === 'function' ? clearImmediate : id => clearTimeout(id);

  globalThis.console = globalThis.console || {
    log() {}, info() {}, warn() {}, error() {}, debug() {}, trace() {},
  };
  // Incoming IPC/window events begin a new task. enterWith() from one message
  // must not leak into the next; Promise/timer continuations keep their frame.
  for (const name of ['__xenonDispatchSend', '__xenonDispatchInvoke',
    '__xenonDispatchSync', '__xenonDispatchBrowserWindowEvent',
    '__xenonDispatchRendererEvent', '__xenonDispatchWillDownload']) {
    const dispatch = globalThis[name];
    globalThis[name] = (...args) =>
        runWithAsyncContext(undefined, dispatch, undefined, args);
  }
})();
