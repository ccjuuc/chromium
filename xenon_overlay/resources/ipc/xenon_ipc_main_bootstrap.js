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
  }
  EventEmitter.prototype.on = function(name, listener) {
    const listeners = this._events.get(name) || [];
    listeners.push(listener);
    this._events.set(name, listeners);
    return this;
  };
  EventEmitter.prototype.addListener = function(name, listener) {
    return this.on(name, listener);
  };
  EventEmitter.prototype.once = function(name, listener) {
    const wrapped = (...args) => {
      this.removeListener(name, wrapped);
      return listener.apply(this, args);
    };
    wrapped.listener = listener;
    return this.on(name, wrapped);
  };
  EventEmitter.prototype.emit = function(name, ...args) {
    const listeners = this._events.get(name);
    if (!listeners || listeners.length === 0) return false;
    for (const listener of [...listeners]) listener.apply(this, args);
    return true;
  };
  EventEmitter.prototype.removeListener = function(name, listener) {
    const listeners = this._events.get(name);
    if (!listeners) return this;
    const filtered = listeners.filter(
        item => item !== listener && item.listener !== listener);
    if (filtered.length) this._events.set(name, filtered);
    else this._events.delete(name);
    return this;
  };
  EventEmitter.prototype.off = function(name, listener) {
    return this.removeListener(name, listener);
  };
  EventEmitter.prototype.removeAllListeners = function(name) {
    if (name === undefined) this._events.clear();
    else this._events.delete(name);
    return this;
  };
  EventEmitter.prototype.listeners = function(name) {
    return [...(this._events.get(name) || [])];
  };
  EventEmitter.prototype.listenerCount = function(name) {
    return (this._events.get(name) || []).length;
  };
  EventEmitter.prototype.setMaxListeners = function(n) { return this; };
  EventEmitter.prototype.getMaxListeners = function() { return 10; };
  EventEmitter.EventEmitter = EventEmitter;
  EventEmitter.default = EventEmitter;
  EventEmitter.defaultMaxListeners = 10;

  const asyncLocalStorageInstances = new Set();
  class AsyncLocalStorage {
    constructor() {
      this.enabled_ = true;
      this.store_ = undefined;
      asyncLocalStorageInstances.add(this);
    }
    disable() {
      this.enabled_ = false;
      this.store_ = undefined;
    }
    enterWith(store) {
      this.enabled_ = true;
      this.store_ = store;
    }
    getStore() {
      return this.enabled_ ? this.store_ : undefined;
    }
    run(store, callback, ...args) {
      if (typeof callback !== 'function') {
        throw new TypeError('The "callback" argument must be a function');
      }
      const previousEnabled = this.enabled_;
      const previousStore = this.store_;
      this.enabled_ = true;
      this.store_ = store;
      try {
        return callback(...args);
      } finally {
        this.enabled_ = previousEnabled;
        this.store_ = previousStore;
      }
    }
    exit(callback, ...args) {
      return this.run(undefined, callback, ...args);
    }
    static snapshot() {
      const captured = [...asyncLocalStorageInstances].map(storage => ({
        storage,
        enabled: storage.enabled_,
        store: storage.store_,
      }));
      return (callback, ...args) => {
        const previous = captured.map(({storage}) => ({
          storage,
          enabled: storage.enabled_,
          store: storage.store_,
        }));
        for (const item of captured) {
          item.storage.enabled_ = item.enabled;
          item.storage.store_ = item.store;
        }
        try {
          return callback(...args);
        } finally {
          for (const item of previous) {
            item.storage.enabled_ = item.enabled;
            item.storage.store_ = item.store;
          }
        }
      };
    }
    static bind(callback) {
      const snapshot = AsyncLocalStorage.snapshot();
      return function(...args) {
        return snapshot(() => callback.apply(this, args));
      };
    }
  }
  class AsyncResource {
    constructor(type) {
      this.type = String(type || 'AsyncResource');
      this.snapshot_ = AsyncLocalStorage.snapshot();
    }
    runInAsyncScope(callback, thisArg, ...args) {
      return this.snapshot_(() => callback.apply(thisArg, args));
    }
    bind(callback, thisArg) {
      return (...args) => this.runInAsyncScope(callback, thisArg, ...args);
    }
    emitDestroy() { return this; }
    asyncId() { return 0; }
    triggerAsyncId() { return 0; }
    static bind(callback, type, thisArg) {
      return new AsyncResource(type).bind(callback, thisArg);
    }
  }
  const asyncHooksModule = {
    AsyncLocalStorage,
    AsyncResource,
    createHook: () => ({
      enable() { return this; },
      disable() { return this; },
    }),
    executionAsyncId: () => 0,
    triggerAsyncId: () => 0,
    executionAsyncResource: () => null,
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

  const osModule = {
    networkInterfaces: () => __xenonNetworkInterfaces(),
    platform: () => __xenonPlatform,
    arch: () => __xenonArch,
    release: () => __xenonOsRelease,
    type: () => isWindows ? 'Windows_NT' : __xenonPlatform,
    homedir: () => __xenonGetPath('home') || __xenonEnv.USERPROFILE || __xenonEnv.HOME || __xenonAppPath,
    tmpdir: () => __xenonGetPath('temp') || __xenonEnv.TEMP || __xenonEnv.TMP || __xenonAppPath,
    hostname: () => __xenonEnv.COMPUTERNAME || '',
    EOL: isWindows ? '\r\n' : '\n',
  };

  let appReady = false;
  let resolveAppReady;
  const appReadyPromise = new Promise(resolve => { resolveAppReady = resolve; });
  const app = new EventEmitter();
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
  app.requestSingleInstanceLock = () => true;
  app.releaseSingleInstanceLock = () => {};
  app.setAppUserModelId = () => {};
  app.setAsDefaultProtocolClient = () => true;
  app.removeAsDefaultProtocolClient = () => true;
  app.addRecentDocument = () => {};
  app.clearRecentDocuments = () => {};
  app.focus = () => {};
  app.quit = () => app.emit('before-quit', {});
  app.exit = () => app.emit('quit', {}, 0);
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

  class Session extends EventEmitter {
    constructor(partition = '') {
      super();
      this.partition = String(partition || '');
      this._userAgent = '';
      this._downloadPath = '';
      this.webRequest = {
        onBeforeRequest: (filter, listener) => {},
        onBeforeSendHeaders: (filter, listener) => {},
        onSendHeaders: (filter, listener) => {},
        onHeadersReceived: (filter, listener) => {},
        onResponseStarted: (filter, listener) => {},
        onBeforeRedirect: (filter, listener) => {},
        onCompleted: (filter, listener) => {},
        onErrorOccurred: (filter, listener) => {},
      };
      this.cookies = {
        get: (filter) => Promise.resolve([]),
        set: (details) => Promise.resolve(),
        remove: (url, name) => Promise.resolve(),
        flushStore: () => Promise.resolve(),
      };
      this.protocol = {
        registerFileProtocol: (scheme, handler) => true,
        registerBufferProtocol: (scheme, handler) => true,
        registerStringProtocol: (scheme, handler) => true,
        registerHttpProtocol: (scheme, handler) => true,
        registerStreamProtocol: (scheme, handler) => true,
        unregisterProtocol: (scheme) => true,
        isProtocolRegistered: (scheme) => false,
        interceptFileProtocol: (scheme, handler) => {},
        interceptStringProtocol: (scheme, handler) => {},
        interceptBufferProtocol: (scheme, handler) => {},
        interceptHttpProtocol: (scheme, handler) => {},
        interceptStreamProtocol: (scheme, handler) => {},
        uninterceptProtocol: (scheme) => {},
        isProtocolIntercepted: (scheme) => false,
      };
    }
    getUserAgent() {
      return this._userAgent || String(__xenonUserAgent || '');
    }
    setUserAgent(userAgent) {
      this._userAgent = String(userAgent || '');
    }
    setProxy(config) { return Promise.resolve(); }
    resolveProxy(url) { return Promise.resolve('DIRECT'); }
    clearCache() { return Promise.resolve(); }
    clearStorageData(options) { return Promise.resolve(); }
    clearAuthCache() { return Promise.resolve(); }
    clearHostResolverCache() { return Promise.resolve(); }
    setDownloadPath(p) { this._downloadPath = String(p || ''); }
    enableNetworkEmulation(options) {}
    disableNetworkEmulation() {}
    setCertificateVerifyProc(proc) {}
    setPermissionRequestHandler(handler) {}
    setPermissionCheckHandler(handler) {}
    getBlobData(identifier) { return Promise.resolve(Buffer.alloc(0)); }
    createInterruptedDownload(options) {}
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
  function callBrowserWindow(window, command, details = {}) {
    if (typeof __xenonBrowserWindowCall !== 'function') return undefined;
    return __xenonBrowserWindowCall(window.id, command, details);
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
      const created =
          (typeof __xenonCreateBrowserWindow === 'function')
              ? __xenonCreateBrowserWindow({
                  width: options.width || 800,
                  height: options.height || 600,
                  show: options.show !== false,
                  frame: options.frame !== false,
                  transparent: Boolean(options.transparent),
                  parentId: options.parent && options.parent.id
                      ? options.parent.id
                      : 0,
                  title: String(options.title || ''),
                })
              : {id: browserWindows.length + 1, hwnd: '0'};
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
      this._title = String(options.title || '');
      this._backgroundColor = options.backgroundColor || '#000000';
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
      const window = new Proxy(this, {
        get(target, prop, receiver) {
          if (typeof prop === 'string' && !(prop in target) &&
              target[prop] === undefined) {
            if (prop.startsWith('is') || prop.startsWith('has')) {
              return () => false;
            }
            return () => undefined;
          }
          return Reflect.get(target, prop, receiver);
        },
      });
      this.webContents._owner = window;
      browserWindows.push(window);
      return window;
    }
    static getAllWindows() { return browserWindows.filter(item => !item._destroyed); }
    static getFocusedWindow() { return BrowserWindow.getAllWindows().at(-1) || null; }
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
    setSkipTaskbar() {}
    setTitle(title) {
      this._title = String(title || '');
      callBrowserWindow(this, 'set-title', {title: this._title});
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
    setBackgroundColor(color) { this._backgroundColor = color; }
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
      const event = createBrowserWindowEvent(this, true);
      this.emit('close', event);
      if (!event.defaultPrevented) this.destroy();
    }
    destroy() {
      if (this._destroyed) return;
      this._destroyed = true;
      destroyWebContents(this.webContents);
      if (typeof __xenonCloseBrowserWindow === 'function') {
        __xenonCloseBrowserWindow(this.id);
      }
      this.emit('closed');
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
      if (!win._destroyed) {
        win._destroyed = true;
        destroyWebContents(win.webContents);
        win.emit('closed');
      }
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
    shell: {openExternal: () => Promise.resolve(), showItemInFolder() {},
            openPath: () => Promise.resolve('')},
    dialog: {
      showOpenDialog: async (winOrOpts, maybeOpts) => {
        const opts = (maybeOpts && typeof maybeOpts === 'object') ? maybeOpts : (winOrOpts && typeof winOrOpts === 'object' ? winOrOpts : {});
        const filePaths = (typeof __xenonShowOpenDialog === 'function') ? __xenonShowOpenDialog(opts) : [];
        return { canceled: filePaths.length === 0, filePaths };
      },
      showOpenDialogSync: (winOrOpts, maybeOpts) => {
        const opts = (maybeOpts && typeof maybeOpts === 'object') ? maybeOpts : (winOrOpts && typeof winOrOpts === 'object' ? winOrOpts : {});
        const filePaths = (typeof __xenonShowOpenDialog === 'function') ? __xenonShowOpenDialog(opts) : [];
        return filePaths.length > 0 ? filePaths : undefined;
      },
      showSaveDialog: async () => ({ canceled: true }),
      showMessageBox: async () => ({ response: 0 }),
    },
    Tray,
    Menu,
    nativeTheme,
    systemPreferences: {isAeroGlassEnabled: () => false},
    powerMonitor,
    session,
    globalShortcut: {register: () => true, unregister() {}, unregisterAll() {}},
    nativeImage: {createEmpty: () => ({}), createFromPath: path => ({path})},
    clipboard: {readText: () => '', writeText() {}, clear() {}},
    screen: (() => {
      const display = {
        id: 1,
        bounds: {x: 0, y: 0, width: 1920, height: 1080},
        workArea: {x: 0, y: 0, width: 1920, height: 1080},
        workAreaSize: {width: 1920, height: 1080},
        size: {width: 1920, height: 1080},
        scaleFactor: 1,
      };
      return {
        getPrimaryDisplay: () => display,
        getAllDisplays: () => [display],
        getDisplayMatching: () => display,
        getDisplayNearestPoint: () => display,
        getCursorScreenPoint: () => ({x: 0, y: 0}),
        on() {},
        off() {},
        addListener() {},
        removeListener() {},
      };
    })(),
    autoUpdater,
  };
  electronModule.default = electronModule;

  // =========================================================================
  // Node.js Built-in Modules (Buffer, fs, crypto, util, url, stream, etc.)
  // =========================================================================

  class Buffer extends Uint8Array {
    static from(data, encoding) {
      if (typeof data === 'string') {
        if (encoding === 'hex') {
          const bytes = [];
          for (let i = 0; i < data.length; i += 2) {
            bytes.push(parseInt(data.substr(i, 2), 16));
          }
          return new Buffer(bytes);
        } else if (encoding === 'base64') {
          const bin = typeof atob === 'function' ? atob(data) : '';
          const bytes = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; ++i) bytes[i] = bin.charCodeAt(i);
          return new Buffer(bytes);
        }
        const encoder = new TextEncoder();
        return new Buffer(encoder.encode(data));
      }
      if (Array.isArray(data) || data instanceof Uint8Array || data instanceof ArrayBuffer) {
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
    static isBuffer(obj) { return obj instanceof Buffer || obj instanceof Uint8Array; }
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
      } else if (encoding === 'base64') {
        let bin = '';
        for (let i = 0; i < slice.length; ++i) bin += String.fromCharCode(slice[i]);
        return typeof btoa === 'function' ? btoa(bin) : '';
      }
      return new TextDecoder().decode(slice);
    }
    slice(start, end) { return new Buffer(this.subarray(start, end)); }
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
    readBigUInt64LE(offset = 0) {
      return this._view().getBigUint64(offset, true);
    }
    readBigInt64LE(offset = 0) {
      return this._view().getBigInt64(offset, true);
    }
    readUIntLE(offset = 0, byteLength = 4) {
      if (byteLength === 8) {
        return this.readBigUInt64LE(offset);
      }
      if (byteLength === 4) {
        return this.readUInt32LE(offset);
      }
      if (byteLength === 2) {
        return this._view().getUint16(offset, true);
      }
      if (byteLength === 1) {
        return this[offset];
      }
      let value = 0;
      for (let i = 0; i < byteLength; ++i) {
        value += this[offset + i] * (2 ** (8 * i));
      }
      return value;
    }
  }
  globalThis.Buffer = Buffer;

  const fsConstants = {
    F_OK: 0,
    R_OK: 4,
    W_OK: 2,
    X_OK: 1,
  };

  const fsModule = {
    constants: fsConstants,
    existsSync(path) {
      return Boolean(__xenonFsExists(String(path)));
    },
    readFileSync(path, options) {
      const encoding = typeof options === 'string' ? options : options?.encoding;
      const content = __xenonFsReadFile(String(path));
      if (content === undefined) {
        throw new Error(`ENOENT: no such file or directory, open '${path}'`);
      }
      if (encoding === 'utf8' || encoding === 'utf-8') {
        return content;
      }
      return Buffer.from(content);
    },
    writeFileSync(path, data, options) {
      let text = data;
      if (Buffer.isBuffer(data)) {
        text = data.toString('utf8');
      } else if (typeof data !== 'string') {
        text = String(data);
      }
      if (!__xenonFsWriteFile(String(path), text)) {
        throw new Error(`EACCES: permission denied, open '${path}'`);
      }
    },
    statSync(path) {
      const stat = __xenonFsStat(String(path));
      if (!stat.exists) {
        throw new Error(`ENOENT: no such file or directory, stat '${path}'`);
      }
      return {
        isFile: () => stat.isFile,
        isDirectory: () => stat.isDirectory,
        isSymbolicLink: () => false,
        size: stat.size,
        mtime: new Date(stat.mtimeMs),
        mtimeMs: stat.mtimeMs,
        birthtime: new Date(stat.birthtimeMs),
        birthtimeMs: stat.birthtimeMs,
        mode: stat.isDirectory ? 0o777 : 0o666,
      };
    },
    lstatSync(path) {
      return fsModule.statSync(path);
    },
    readdirSync(path) {
      return __xenonFsReaddir(String(path));
    },
    mkdirSync(path, options) {
      return __xenonFsMkdir(String(path));
    },
    unlinkSync(path) {
      return __xenonFsUnlink(String(path));
    },
    rmdirSync(path) {
      return __xenonFsUnlink(String(path));
    },
    rmSync(path) {
      return __xenonFsUnlink(String(path));
    },
    accessSync(path, mode) {
      if (!fsModule.existsSync(path)) {
        throw new Error(`ENOENT: no such file or directory, access '${path}'`);
      }
    },
    readFile(path, options, callback) {
      if (typeof options === 'function') {
        callback = options;
        options = {};
      }
      process.nextTick(() => {
        try {
          const res = fsModule.readFileSync(path, options);
          callback(null, res);
        } catch (err) {
          callback(err);
        }
      });
    },
    writeFile(path, data, options, callback) {
      if (typeof options === 'function') {
        callback = options;
        options = {};
      }
      process.nextTick(() => {
        try {
          fsModule.writeFileSync(path, data, options);
          callback(null);
        } catch (err) {
          callback(err);
        }
      });
    },
    stat(path, callback) {
      process.nextTick(() => {
        try {
          callback(null, fsModule.statSync(path));
        } catch (err) {
          callback(err);
        }
      });
    },
    readdir(path, callback) {
      process.nextTick(() => {
        try {
          callback(null, fsModule.readdirSync(path));
        } catch (err) {
          callback(err);
        }
      });
    },
    mkdir(path, options, callback) {
      if (typeof options === 'function') {
        callback = options;
      }
      process.nextTick(() => {
        try {
          fsModule.mkdirSync(path);
          callback(null);
        } catch (err) {
          callback(err);
        }
      });
    },
    unlink(path, callback) {
      process.nextTick(() => {
        try {
          fsModule.unlinkSync(path);
          callback(null);
        } catch (err) {
          callback(err);
        }
      });
    },
    exists(path, callback) {
      process.nextTick(() => callback(fsModule.existsSync(path)));
    },
  };

  const realpathFn = (p, options, callback) => {
    if (typeof options === 'function') { callback = options; }
    if (callback) process.nextTick(() => callback(null, String(p)));
    return String(p);
  };
  realpathFn.native = realpathFn;

  const realpathSyncFn = (p, options) => String(p);
  realpathSyncFn.native = realpathSyncFn;

  fsModule.realpath = realpathFn;
  fsModule.realpathSync = realpathSyncFn;
  fsModule.createReadStream = (p, opts) => new Readable();
  fsModule.createWriteStream = (p, opts) => new Writable();
  fsModule.watch = (p, opts, listener) => new EventEmitter();
  fsModule.watchFile = (p, opts, listener) => {};
  fsModule.unwatchFile = (p, listener) => {};
  fsModule.chmod = (p, m, cb) => { if (cb) cb(null); };
  fsModule.chmodSync = (p, m) => {};
  fsModule.chown = (p, u, g, cb) => { if (cb) cb(null); };
  fsModule.chownSync = (p, u, g) => {};
  fsModule.copyFile = (src, dest, flags, cb) => {
    if (typeof flags === 'function') { cb = flags; }
    try { fsModule.copyFileSync(src, dest, flags); if (cb) cb(null); } catch (e) { if (cb) cb(e); }
  };
  fsModule.copyFileSync = (src, dest, flags) => {
    const data = fsModule.readFileSync(src);
    fsModule.writeFileSync(dest, data);
  };
  fsModule.rename = (oldPath, newPath, cb) => {
    try { fsModule.renameSync(oldPath, newPath); if (cb) cb(null); } catch (e) { if (cb) cb(e); }
  };
  fsModule.renameSync = (oldPath, newPath) => {
    fsModule.copyFileSync(oldPath, newPath);
    try { fsModule.unlinkSync(oldPath); } catch {}
  };
  fsModule.open = (p, flags, mode, cb) => {
    if (typeof mode === 'function') cb = mode;
    if (cb) process.nextTick(() => cb(null, 3));
  };
  fsModule.openSync = (p, flags, mode) => 3;
  fsModule.close = (fd, cb) => { if (cb) process.nextTick(() => cb(null)); };
  fsModule.closeSync = fd => {};
  fsModule.read = (fd, buf, off, len, pos, cb) => { if (cb) process.nextTick(() => cb(null, 0, buf)); };
  fsModule.readSync = (fd, buf, off, len, pos) => 0;
  fsModule.write = (fd, buf, off, len, pos, cb) => { if (cb) process.nextTick(() => cb(null, buf ? buf.length : 0, buf)); };
  fsModule.writeSync = (fd, buf, off, len, pos) => buf ? buf.length : 0;

  const fsPromises = {
    readFile: async (path, options) => fsModule.readFileSync(path, options),
    writeFile: async (path, data, options) => fsModule.writeFileSync(path, data, options),
    stat: async path => fsModule.statSync(path),
    lstat: async path => fsModule.lstatSync(path),
    readdir: async path => fsModule.readdirSync(path),
    mkdir: async (path, options) => fsModule.mkdirSync(path, options),
    unlink: async path => fsModule.unlinkSync(path),
    rm: async (path, options) => fsModule.rmSync(path),
    access: async (path, mode) => fsModule.accessSync(path, mode),
    realpath: async (path, options) => String(path),
    copyFile: async (src, dest, flags) => fsModule.copyFileSync(src, dest, flags),
    rename: async (oldPath, newPath) => fsModule.renameSync(oldPath, newPath),
    open: async (path, flags, mode) => ({ fd: 3, close: async () => {}, readFile: async () => Buffer.alloc(0), writeFile: async () => {} }),
  };
  fsModule.promises = fsPromises;

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

  const cryptoModule = {
    createCipheriv(algorithm, key, iv) {
      return createCipherObject(algorithm, key, iv, true);
    },
    createDecipheriv(algorithm, key, iv) {
      return createCipherObject(algorithm, key, iv, false);
    },
    createHash(algorithm) {
      let data = '';
      return {
        update(chunk) {
          if (typeof chunk === 'string') data += chunk;
          else if (Buffer.isBuffer(chunk)) data += chunk.toString('utf8');
          return this;
        },
        digest(encoding = 'hex') {
          let hash = 0;
          for (let i = 0; i < data.length; i++) {
            hash = ((hash << 5) - hash) + data.charCodeAt(i);
            hash |= 0;
          }
          const hex = Math.abs(hash).toString(16).padStart(32, '0');
          if (encoding === 'hex') return hex;
          return Buffer.from(hex, 'hex');
        },
      };
    },
    createHmac(algorithm, key) {
      return cryptoModule.createHash(algorithm);
    },
    randomBytes(size) {
      const buf = Buffer.alloc(size);
      for (let i = 0; i < size; ++i) buf[i] = Math.floor(Math.random() * 256);
      return buf;
    },
    randomUUID() {
      return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
        const r = Math.random() * 16 | 0;
        const v = c === 'x' ? r : (r & 0x3 | 0x8);
        return v.toString(16);
      });
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

  function Stream() {
    if (!(this instanceof Stream)) {
      return new Stream();
    }
    EventEmitter.call(this);
  }
  Object.setPrototypeOf(Stream.prototype, EventEmitter.prototype);
  Object.setPrototypeOf(Stream, EventEmitter);
  Stream.prototype.pipe = function(dest) { return dest; };

  function Readable(options) {
    if (!(this instanceof Readable)) {
      return new Readable(options);
    }
    Stream.call(this);
  }
  Object.setPrototypeOf(Readable.prototype, Stream.prototype);
  Object.setPrototypeOf(Readable, Stream);
  Readable.prototype.read = function() { return null; };
  Readable.prototype.push = function() { return false; };

  function Writable(options) {
    if (!(this instanceof Writable)) {
      return new Writable(options);
    }
    Stream.call(this);
  }
  Object.setPrototypeOf(Writable.prototype, Stream.prototype);
  Object.setPrototypeOf(Writable, Stream);
  Writable.prototype.write = function(chunk, encoding, cb) {
    if (typeof encoding === 'function') {
      cb = encoding;
    }
    if (cb) {
      cb();
    }
    return true;
  };
  Writable.prototype.end = function(chunk, encoding, cb) {
    if (typeof encoding === 'function') {
      cb = encoding;
    } else if (typeof chunk === 'function') {
      cb = chunk;
    }
    if (cb) {
      cb();
    }
    this.emit('finish');
  };

  function Transform(options) {
    if (!(this instanceof Transform)) {
      return new Transform(options);
    }
    Readable.call(this, options);
  }
  Object.setPrototypeOf(Transform.prototype, Readable.prototype);
  Object.setPrototypeOf(Transform, Readable);
  Transform.prototype._transform = function(chunk, encoding, cb) {
    if (cb) {
      cb();
    }
  };

  function PassThrough(options) {
    if (!(this instanceof PassThrough)) {
      return new PassThrough(options);
    }
    Transform.call(this, options);
  }
  Object.setPrototypeOf(PassThrough.prototype, Transform.prototype);
  Object.setPrototypeOf(PassThrough, Transform);

  // Node's `require('stream')` is the Stream constructor with classes
  // attached as properties (readable-stream does `module.exports = require('stream')`
  // then `Parent.call(this)`).
  Stream.Stream = Stream;
  Stream.Readable = Readable;
  Stream.Writable = Writable;
  Stream.Duplex = Readable;
  Stream.Transform = Transform;
  Stream.PassThrough = PassThrough;
  Stream.EventEmitter = EventEmitter;
  Stream.pipeline = (...args) => {
    const cb = typeof args[args.length - 1] === 'function' ? args.pop() : () => {};
    cb(null);
  };
  Stream.finished = (stream, cb) => { if (cb) cb(null); };
  const streamModule = Stream;

  class StringDecoder {
    constructor(encoding = 'utf8') { this.encoding = encoding; }
    write(buf) { return Buffer.from(buf).toString(this.encoding); }
    end(buf) { return buf ? this.write(buf) : ''; }
  }
  const stringDecoderModule = { StringDecoder };

  const childProcessModule = {
    spawn: () => {
      const cp = new EventEmitter();
      cp.stdout = new Readable();
      cp.stderr = new Readable();
      cp.stdin = new Writable();
      cp.pid = 0;
      cp.kill = () => true;
      return cp;
    },
    exec: (cmd, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) process.nextTick(() => cb(null, '', ''));
    },
    execFile: (file, args, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (typeof args === 'function') { cb = args; }
      if (cb) process.nextTick(() => cb(null, '', ''));
    },
    fork: () => childProcessModule.spawn(),
  };

  function assert(condition, message) {
    if (!condition) throw new Error(message || 'Assertion failed');
  }
  assert.ok = assert;
  assert.strictEqual = (a, b, m) => { if (a !== b) throw new Error(m || `Expected ${a} === ${b}`); };
  assert.deepStrictEqual = (a, b, m) => assert.strictEqual(JSON.stringify(a), JSON.stringify(b), m);
  assert.equal = assert.strictEqual;
  assert.notEqual = (a, b, m) => { if (a === b) throw new Error(m || `Expected ${a} !== ${b}`); };

  const zlibConstants = {
    Z_NO_FLUSH: 0,
    Z_PARTIAL_FLUSH: 1,
    Z_SYNC_FLUSH: 2,
    Z_FULL_FLUSH: 3,
    Z_FINISH: 4,
    Z_BLOCK: 5,
    Z_TREES: 6,
    Z_OK: 0,
    Z_STREAM_END: 1,
    Z_NEED_DICT: 2,
    Z_ERRNO: -1,
    Z_STREAM_ERROR: -2,
    Z_DATA_ERROR: -3,
    Z_MEM_ERROR: -4,
    Z_BUF_ERROR: -5,
    Z_VERSION_ERROR: -6,
    Z_NO_COMPRESSION: 0,
    Z_BEST_SPEED: 1,
    Z_BEST_COMPRESSION: 9,
    Z_DEFAULT_COMPRESSION: -1,
    Z_FILTERED: 1,
    Z_HUFFMAN_ONLY: 2,
    Z_RLE: 3,
    Z_FIXED: 4,
    Z_DEFAULT_STRATEGY: 0,
  };
  const constantsModule = Object.assign({}, fsConstants, zlibConstants);
  const zlibModule = Object.assign({
    constants: zlibConstants,
    codes: {
      Z_OK: 0,
      Z_STREAM_END: 1,
      Z_NEED_DICT: 2,
      Z_ERRNO: -1,
      Z_STREAM_ERROR: -2,
      Z_DATA_ERROR: -3,
      Z_MEM_ERROR: -4,
      Z_BUF_ERROR: -5,
      Z_VERSION_ERROR: -6,
    },
    createGzip: () => new Transform(),
    createGunzip: () => new Transform(),
    createDeflate: () => new Transform(),
    createInflate: () => new Transform(),
    createDeflateRaw: () => new Transform(),
    createInflateRaw: () => new Transform(),
    createUnzip: () => new Transform(),
    gzipSync: buf => buf,
    gunzipSync: buf => buf,
    deflateSync: buf => buf,
    inflateSync: buf => buf,
    deflateRawSync: buf => buf,
    inflateRawSync: buf => buf,
    unzipSync: buf => buf,
    gzip: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
    gunzip: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
    deflate: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
    inflate: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
    deflateRaw: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
    inflateRaw: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
    unzip: (buf, opts, cb) => {
      if (typeof opts === 'function') { cb = opts; }
      if (cb) Promise.resolve().then(() => cb(null, buf));
    },
  }, zlibConstants);
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
      let raw = '';
      for (let i = 0; i < u8.length; i++) {
        raw += String.fromCharCode(u8[i]);
      }
      return {t: 'b', d: raw};
    }
    function netWireToBytes(wire) {
      if (!wire || wire.t === 's') {
        return Buffer.from(String(wire && wire.d || ''), 'utf8');
      }
      const raw = String(wire.d || '');
      const u8 = new Uint8Array(raw.length);
      for (let i = 0; i < raw.length; i++) {
        u8[i] = raw.charCodeAt(i) & 0xff;
      }
      return Buffer.from(u8);
    }

    const netServers = new Map();
    const netSockets = new Map();
    let nextNetSocketId = 1;

    function sendXenonNet(channel, payload, endpointId) {
      if (typeof __xenonSendToRenderer === 'function') {
        __xenonSendToRenderer(endpointId || '*', channel, [payload]);
      }
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
      socket.emit('end');
      socket.emit('close');
    }
    function deliverNetBytes(socket, data) {
      const buf = Buffer.isBuffer(data) || data instanceof Uint8Array ?
          Buffer.from(data) : Buffer.from(String(data));
      socket.emit('data', buf);
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
          const cb = typeof args[args.length - 1] === 'function' ?
              args[args.length - 1] : null;
          this._connectCb = cb;
          this.connecting = true;
          const path = normalizeNetPath(netPathFromListenOrConnect(args));
          allocNetSocket(this);
          const server = netServers.get(path);
          if (server) {
            const incoming = new module.Socket();
            incoming._connected = true;
            incoming._peer = this;
            allocNetSocket(incoming);
            this._peer = incoming;
            this._connected = true;
            this.connecting = false;
            queueMicrotask(() => {
              server.emit('connection', incoming);
              this.emit('connect');
              if (this._connectCb) {
                this._connectCb();
                this._connectCb = null;
              }
            });
            return this;
          }
          console.info('[xenon-net] connect', path);
          sendXenonNet('__xenon:net:connect', {path, fromId: this._id});
          return this;
        }
        write(data) {
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
          const cb = typeof args[args.length - 1] === 'function' ?
              args[args.length - 1] : null;
          this._path = normalizeNetPath(netPathFromListenOrConnect(args));
          netServers.set(this._path, this);
          queueMicrotask(() => {
            this.emit('listening');
            if (cb) {
              cb();
            }
          });
          return this;
        }
        close(cb) {
          if (this._path) {
            netServers.delete(this._path);
          }
          if (typeof cb === 'function') {
            cb();
          }
          this.emit('close');
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
        incoming._connected = true;
        incoming._peerId = msg.fromId;
        incoming._endpointId = endpointId;
        allocNetSocket(incoming);
        // A real net.Server exposes the accepted socket before the client can
        // observe connect. node-net-ipc installs its data parser here.
        server.emit('connection', incoming);
        sendXenonNet('__xenon:net:connected', {
          toId: msg.fromId,
          peerId: incoming._id,
        }, endpointId);
        return true;
      }
      if (channel === '__xenon:net:connected') {
        const socket = netSockets.get(msg.toId);
        if (!socket || socket._connected) {
          return true;
        }
        socket._peerId = msg.peerId;
        socket._endpointId = endpointId;
        socket._connected = true;
        socket.connecting = false;
        socket.emit('connect');
        if (socket._connectCb) {
          socket._connectCb();
          socket._connectCb = null;
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
        const socket = netSockets.get(msg.toId);
        if (socket && !socket._connected) {
          const err = new Error(msg.code || 'net error');
          err.code = msg.code;
          socket.connecting = false;
          socket._pendingWrites.length = 0;
          socket.emit('error', err);
        }
        return true;
      }
      return false;
    };

    return module;
  })();
  const httpModule = {
    createServer: () => new EventEmitter(),
    request: () => {
      const req = new EventEmitter();
      req.write = () => {};
      req.end = () => {};
      return req;
    },
    get: (url, cb) => {
      const req = httpModule.request();
      if (cb) {
        const res = new EventEmitter();
        res.statusCode = 200;
        res.headers = {};
        process.nextTick(() => cb(res));
      }
      return req;
    },
    Agent: class {},
  };
  const httpsModule = Object.assign({}, httpModule);
  const ttyModule = { isatty: () => false };
  const readlineModule = {
    createInterface: () => new EventEmitter(),
  };
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
  globalThis.__xenonHttp = httpModule;
  globalThis.__xenonHttps = httpsModule;
  globalThis.__xenonTty = ttyModule;
  globalThis.__xenonReadline = readlineModule;
  globalThis.__xenonDns = dnsModule;
  globalThis.__xenonQuerystring = querystringModule;
  globalThis.__xenonTimers = timersModule;
  globalThis.__xenonPerfHooks = perfHooksModule;
  globalThis.performance = performanceModule;

  globalThis.__xenonMarkAppReady = () => {
    if (appReady) return;
    appReady = true;
    resolveAppReady();
    app.emit('ready', {}, {});
  };
  globalThis.__xenonShutdownApp = () => {
    app.emit('before-quit', {});
    app.emit('will-quit', {});
    app.emit('quit', {}, 0);
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
      chrome: __xenonChromeVersion || '142',
      node: '0.0.0-compat',
      v8: __xenonV8Version || '13.0',
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
    exit: (code = 0) => globalThis.__xenonShutdownApp(),
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
})();
