// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Renderer-side compatibility layer for the generic Browser-process Electron
// container. It provides standard Electron and Node.js APIs (ipcRenderer, path,
// os, fs, events, buffer, util, process, and Mojo Node-API native addon loading).
(() => {
  'use strict';

  globalThis.global = globalThis;

  const transport = globalThis.xenonIpcRenderer;
  if (!transport || globalThis.__xenonElectronIpc) {
    return;
  }

  // A renderer process may host multiple Electron containers. Query metadata
  // from the current Document instead of using the process-wide fallback.
  const injectedPaths = Object.assign({}, globalThis.__xenonPaths || {});
  try {
    Object.assign(injectedPaths, transport.getRuntimeConfig());
  } catch (error) {
    console.warn('[xenon-ipc] runtime config unavailable:', error);
  }
  const runtimePlatform = injectedPaths.platform;
  const runtimeArch = injectedPaths.arch;
  if (typeof runtimePlatform !== 'string' || !runtimePlatform ||
      typeof runtimeArch !== 'string' || !runtimeArch) {
    throw Object.assign(new Error('Native platform and architecture metadata are unavailable'),
                        {code: 'ERR_NOT_SUPPORTED'});
  }
  const rawExecPath =
      String(injectedPaths.execPath || '');
  // Main and renderer receive the same validated executable identity. An app
  // display name is not a filesystem path and must never rename it here.
  const executableName = rawExecPath.split(/[\\/]/).pop().replace(/\.exe$/i, '');
  const hostedAppName = String(injectedPaths.appName || executableName || 'xenon');
  const hostedAppVersion = String(injectedPaths.appVersion || '1.0.0');
  const execPath = rawExecPath;
  const exeDir = injectedPaths.exeDir ||
      rawExecPath.replace(/[\\/][^\\/]+$/, '');
  const appPath = injectedPaths.appPath || exeDir;
  const userData = injectedPaths.userData || '';
  const appData = injectedPaths.appData || '';
  const localAppData = injectedPaths.localAppData || '';
  const homeDir = injectedPaths.home || '';
  const tempDir = injectedPaths.temp || '';

  globalThis.__filename = injectedPaths.documentPath || appPath + '\\index.js';
  globalThis.__dirname = globalThis.__filename.replace(/[\\/][^\\/]*$/, '');
  if (typeof window !== 'undefined') {
    window.global = window;
    window.__filename = globalThis.__filename;
    window.__dirname = globalThis.__dirname;
  }

  // --- 1. EventEmitter ---
  // Must be a callable function constructor: readable-stream does
  // `Stream.call(this)` / `EventEmitter.call(this)`. ES6 class constructors
  // throw "Class constructor ... without 'new'" / ".call is not a function".
  function EventEmitter() {
    if (!(this instanceof EventEmitter)) {
      return new EventEmitter();
    }
    this.events_ = new Map();
    this.maxListeners_ = undefined;
  }
  // Native addon wrappers can copy these methods without calling the
  // constructor. Keep their listener state local to each receiver.
  function eventMap(emitter) {
    if (!Object.prototype.hasOwnProperty.call(emitter, 'events_') ||
        !(emitter.events_ instanceof Map)) {
      emitter.events_ = new Map();
    }
    return emitter.events_;
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
    if (!listeners || !listeners.length) {
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
    for (const listener of [...listeners]) {
      listener.apply(this, args);
    }
    return true;
  };
  EventEmitter.prototype.removeListener = function(name, listener) {
    validateListener(listener);
    const listeners = eventMap(this).get(name);
    if (!listeners) {
      return this;
    }
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
    const remaining = listeners.slice();
    remaining.splice(index, 1);
    if (remaining.length) {
      eventMap(this).set(name, remaining);
    } else {
      eventMap(this).delete(name);
    }
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
    this.maxListeners_ = n;
    return this;
  };
  EventEmitter.prototype.getMaxListeners = function() {
    return this.maxListeners_ === undefined ? EventEmitter.defaultMaxListeners :
                                              this.maxListeners_;
  };
  EventEmitter.listenerCount = (emitter, name) => emitter.listenerCount(name);
  EventEmitter.EventEmitter = EventEmitter;
  EventEmitter.default = EventEmitter;
  EventEmitter.defaultMaxListeners = 10;

  // Node's async_hooks implementation is embedder-backed. Hosted renderers
  // still need its public context API for libraries such as OpenTelemetry.
  // Preserve the synchronous scope semantics; AsyncResource.bind() lets those
  // libraries carry the captured scope into callbacks they own.
  // Promise hooks are scoped to this V8 context. Blink owns the isolate's
  // continuation-preserved embedder data, so never overwrite that slot.
  let currentAsyncContext;
  let asyncContextHooksInstalled = false;
  const promiseAsyncContexts = new WeakMap();
  const asyncContextStack = [];
  const installAsyncContextHooks = transport.installAsyncContextHooks?.bind(transport);
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

  // --- 2. ipcRenderer ---
  const ipcRenderer = new EventEmitter();
  let dispatchXenonNet = null;
  let dispatchNodeAddon = null;

  ipcRenderer.send = (channel, ...args) => transport.send(channel, ...args);
  ipcRenderer.sendSync = (channel, ...args) =>
      transport.sendSync(channel, ...args);
  ipcRenderer.invoke = (channel, ...args) => transport.invoke(channel, ...args);
  ipcRenderer.postMessage = (channel, message, transfer) => {
    if (transfer !== undefined && !Array.isArray(transfer)) {
      throw new TypeError('The "transfer" argument must be an array');
    }
    if (transfer && transfer.length) {
      throw new TypeError('MessagePort transfer is not supported by this transport');
    }
    transport.postMessage(channel, message);
  };
  ipcRenderer.sendToHost = (channel, ...args) => {
    return transport.send('__xenon:send-to-host', channel, args);
  };

  const dispatchRendererMessage = (channel, args) => {
    const values = Array.isArray(args) ? args : [args];
    if (typeof channel === 'string' && channel.startsWith('__xenon:net:') &&
        dispatchXenonNet && dispatchXenonNet(channel, values[0])) {
      return;
    }
    if (typeof channel === 'string' &&
        channel.startsWith('__xenon:node-addon:') &&
        dispatchNodeAddon && dispatchNodeAddon(channel, values)) {
      return;
    }
    ipcRenderer.emit(channel, {sender: ipcRenderer, ports: []}, ...values);
  };
  transport.setDispatchHandler((...args) =>
      runWithAsyncContext(undefined, dispatchRendererMessage, undefined, args));

  function getAppPathByName(name) {
    switch (String(name || '')) {
      case 'exe':
        return execPath;
      case 'module':
      case 'app':
      case 'appPath':
        return appPath;
      case 'userData':
        return userData;
      case 'appData':
        return appData;
      case 'temp':
        return tempDir;
      case 'home':
        return homeDir;
      case 'desktop':
        return homeDir + '\\Desktop';
      case 'documents':
        return homeDir + '\\Documents';
      case 'downloads':
        return homeDir + '\\Downloads';
      default:
        return userData;
    }
  }

  const appApi = {
    getVersion: () => hostedAppVersion,
    getName: () => hostedAppName,
    getAppPath: () => appPath,
    getPath: (name) => getAppPathByName(name),
    isPackaged: true,
    whenReady: () => Promise.resolve(appApi),
    isReady: () => true,
  };

  function unsupportedElectronApi(name) {
    const error = new Error(`electron.${name} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }

  function throwHostApiError(error) {
    // IpcResult carries an error string. Recover the native code without
    // replacing an existing exception or treating a failure as an API result.
    if (error && !error.code && typeof error.message === 'string') {
      const code = /^([A-Z][A-Z_]+):/.exec(error.message);
      if (code) error.code = code[1];
    }
    throw error;
  }

  function callHostElectronApi(operation, args = {}) {
    if (typeof transport.sendSync !== 'function') {
      return unsupportedElectronApi(operation);
    }
    try {
      return transport.sendSync('__xenon:electron-api', {operation, ...args});
    } catch (error) {
      return throwHostApiError(error);
    }
  }

  async function invokeHostElectronApi(operation, args = {}) {
    if (typeof transport.invoke !== 'function') {
      return unsupportedElectronApi(operation);
    }
    try {
      return await transport.invoke('__xenon:electron-api', {operation, ...args});
    } catch (error) {
      return throwHostApiError(error);
    }
  }

  const dialogApi = {
    showOpenDialog: async (options) => {
      const filePaths = await showNativeOpenDialog(options);
      return {canceled: !filePaths.length, filePaths};
    },
    showOpenDialogSync: () => unsupportedElectronApi('dialog.showOpenDialogSync'),
    showSaveDialog: async () => unsupportedElectronApi('dialog.showSaveDialog'),
    showMessageBox: async () => unsupportedElectronApi('dialog.showMessageBox'),
  };

  const electron = {
    ipcRenderer,
    app: appApi,
    dialog: dialogApi,
    remote: {
      dialog: dialogApi,
      app: appApi,
      getCurrentWindow: () => unsupportedElectronApi('remote.getCurrentWindow'),
    },
    clipboard: {
      readText: (type = 'clipboard') => callHostElectronApi('clipboard.readText', {type}),
      writeText(text, type = 'clipboard') {
        callHostElectronApi('clipboard.writeText', {text, type});
      },
      readHTML: (type = 'clipboard') => callHostElectronApi('clipboard.readHTML', {type}),
      writeHTML(markup, type = 'clipboard') {
        callHostElectronApi('clipboard.writeHTML', {markup, type});
      },
      clear(type = 'clipboard') { callHostElectronApi('clipboard.clear', {type}); },
    },
    shell: {
      openExternal: (url, options = {}) =>
          invokeHostElectronApi('shell.openExternal', {url, options}),
      openPath: path => invokeHostElectronApi('shell.openPath', {path}),
      showItemInFolder(path) {
        callHostElectronApi('shell.showItemInFolder', {path});
      },
    },
    webFrame: {
      setZoomFactor: () => unsupportedElectronApi('webFrame.setZoomFactor'),
      getZoomFactor: () => unsupportedElectronApi('webFrame.getZoomFactor'),
      setZoomLevel: () => unsupportedElectronApi('webFrame.setZoomLevel'),
      getZoomLevel: () => unsupportedElectronApi('webFrame.getZoomLevel'),
    },
  };
  globalThis.__xenonElectronIpc = electron;
  // Guest preloads can explicitly expose their own API, but a page without
  // Node integration must never inherit this unrestricted convenience alias.
  if (!injectedPaths.isGuest && globalThis.electron === undefined) {
    globalThis.electron = electron;
  }

  // --- 3. Path Module (Win32 & POSIX) ---
  const win32 = {
    sep: '\\',
    delimiter: ';',
    isAbsolute(path) {
      return typeof path === 'string' &&
          (/^[a-zA-Z]:[\\/]/.test(path) || /^\\\\[^\\]+/.test(path));
    },
    normalize(path) {
      path = String(path || '.');
      if (!path) return '.';
      const isUnc = /^\\\\[^\\]+/.test(path);
      const driveMatch = path.match(/^([a-zA-Z]:)[\\/]?/);
      const prefix = driveMatch ? driveMatch[1] + '\\' : (isUnc ? '\\\\' : (path.startsWith('\\') ? '\\' : ''));
      let rest = driveMatch ? path.slice(driveMatch[0].length) : (isUnc ? path.slice(2) : path);
      rest = rest.replace(/\//g, '\\');
      const parts = [];
      for (const segment of rest.split('\\')) {
        if (!segment || segment === '.') continue;
        if (segment === '..') {
          if (parts.length && parts[parts.length - 1] !== '..') {
            parts.pop();
          } else if (!prefix) {
            parts.push('..');
          }
        } else {
          parts.push(segment);
        }
      }
      let result = prefix + parts.join('\\');
      return result || (prefix ? prefix : '.');
    },
    join(...paths) {
      const valid = paths.filter(p => typeof p === 'string' && p.length > 0);
      if (!valid.length) return '.';
      return win32.normalize(valid.join('\\'));
    },
    resolve(...paths) {
      let resolved = '';
      for (let i = paths.length - 1; i >= 0; i--) {
        const p = paths[i];
        if (typeof p !== 'string' || !p) continue;
        resolved = p + (resolved ? '\\' + resolved : '');
        if (win32.isAbsolute(resolved)) break;
      }
      if (!win32.isAbsolute(resolved)) {
        resolved = 'C:\\' + (resolved ? resolved : '');
      }
      return win32.normalize(resolved);
    },
    dirname(path) {
      path = win32.normalize(path);
      const idx = path.lastIndexOf('\\');
      if (idx === -1) return '.';
      if (idx === 2 && path[1] === ':') return path.slice(0, 3);
      if (idx === 0) return '\\';
      return path.slice(0, idx);
    },
    basename(path, ext) {
      path = String(path || '');
      const idx = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
      let base = idx >= 0 ? path.slice(idx + 1) : path;
      if (ext && base.endsWith(ext)) base = base.slice(0, -ext.length);
      return base;
    },
    extname(path) {
      const base = win32.basename(path);
      const dot = base.lastIndexOf('.');
      if (dot <= 0) return '';
      return base.slice(dot);
    },
    parse(path) {
      path = String(path || '');
      const root = win32.isAbsolute(path) ? path.slice(0, 3) : '';
      const dir = win32.dirname(path);
      const base = win32.basename(path);
      const ext = win32.extname(path);
      const name = ext ? base.slice(0, -ext.length) : base;
      return { root, dir, base, ext, name };
    },
    format(obj) {
      const dir = obj.dir || obj.root || '';
      const base = obj.base || ((obj.name || '') + (obj.ext || ''));
      if (!dir) return base;
      if (dir.endsWith('\\')) return dir + base;
      return dir + '\\' + base;
    }
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
      return result || (isAbs ? '/' : '.');
    },
    join(...paths) {
      const valid = paths.filter(p => typeof p === 'string' && p.length > 0);
      if (!valid.length) return '.';
      return posix.normalize(valid.join('/'));
    },
    resolve(...paths) {
      let resolved = '';
      for (let i = paths.length - 1; i >= 0; i--) {
        const p = paths[i];
        if (typeof p !== 'string' || !p) continue;
        resolved = p + (resolved ? '/' + resolved : '');
        if (posix.isAbsolute(resolved)) break;
      }
      if (!posix.isAbsolute(resolved)) resolved = '/' + resolved;
      return posix.normalize(resolved);
    },
    dirname(path) {
      path = posix.normalize(path);
      const idx = path.lastIndexOf('/');
      if (idx === -1) return '.';
      if (idx === 0) return '/';
      return path.slice(0, idx);
    },
    basename(path, ext) {
      path = String(path || '');
      const idx = path.lastIndexOf('/');
      let base = idx >= 0 ? path.slice(idx + 1) : path;
      if (ext && base.endsWith(ext)) base = base.slice(0, -ext.length);
      return base;
    },
    extname(path) {
      const base = posix.basename(path);
      const dot = base.lastIndexOf('.');
      if (dot <= 0) return '';
      return base.slice(dot);
    },
    parse(path) {
      path = String(path || '');
      const root = path.startsWith('/') ? '/' : '';
      const dir = posix.dirname(path);
      const base = posix.basename(path);
      const ext = posix.extname(path);
      const name = ext ? base.slice(0, -ext.length) : base;
      return { root, dir, base, ext, name };
    },
    format(obj) {
      const dir = obj.dir || obj.root || '';
      const base = obj.base || ((obj.name || '') + (obj.ext || ''));
      if (!dir) return base;
      if (dir.endsWith('/')) return dir + base;
      return dir + '/' + base;
    }
  };

  win32.win32 = posix.win32 = win32;
  win32.posix = posix.posix = posix;
  const pathModule = runtimePlatform === 'win32' ? win32 : posix;

  // --- 4. OS Module ---
  const osPlatform = runtimePlatform;
  const osArch = runtimeArch;
  const osEndianness = injectedPaths.endianness;
  const osNativeCall = request => transport.sendSync('__xenon:os', request);
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

  // --- 5. Buffer & Util ---
  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder();
  const nativeToBase64 = Uint8Array.prototype.toBase64;
  const nativeFromBase64 = Uint8Array.fromBase64;
  // Some older JS hosts expose an experimental API that ignores byteOffset.
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
    // Node Buffer accepts both alphabets, ignores non-alphabet characters and
    // stops at padding. Keep canonical input on V8's allocation-efficient path.
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
    // Older JS hosts lack the native API. Bounded chunks avoid both a long
    // chain of per-byte strings and the argument limit of one large apply().
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
    static from(value, encoding, length) {
      if (typeof value === 'string') {
        if (encoding === 'hex') {
          const match = value.match(/.{1,2}/g) || [];
          return new Buffer(match.map(byte => parseInt(byte, 16)));
        }
        if (encoding === 'base64' || encoding === 'base64url') {
          const bytes = decodeBase64(value);
          return new Buffer(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        }
        return new Buffer(textEncoder.encode(value).buffer);
      }
      if (ArrayBuffer.isView(value)) {
        // Typed arrays contribute elements, not their underlying byte layout.
        return new Buffer(value);
      }
      if (value instanceof ArrayBuffer) {
        let offset = encoding === undefined ? 0 : +encoding;
        if (Number.isNaN(offset)) offset = 0;
        const available = value.byteLength - offset;
        if (length !== undefined) {
          length = +length;
          if (!(length > 0)) length = 0;
        }
        if (available < 0 || (length !== undefined && length > available)) {
          const error = new RangeError('Buffer offset or length is outside the ArrayBuffer');
          error.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
          throw error;
        }
        return new Buffer(value, offset, length);
      }
      if (Array.isArray(value)) {
        return new Buffer(Uint8Array.from(value).buffer);
      }
      return new Buffer(0);
    }

    static alloc(size, fill = 0) {
      const buf = new Buffer(size);
      if (fill !== 0) buf.fill(fill);
      return buf;
    }

    static isBuffer(obj) {
      return obj instanceof Buffer;
    }

    static isEncoding(encoding) {
      // Node accepts primitive strings only; do not coerce application objects.
      if (typeof encoding !== 'string') return false;
      switch (encoding.toLowerCase()) {
        case 'utf8': case 'utf-8':
        case 'utf16le': case 'utf-16le': case 'ucs2': case 'ucs-2':
        case 'latin1': case 'binary': case 'ascii':
        case 'base64': case 'base64url': case 'hex':
          return true;
        default:
          return false;
      }
    }

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

    static concat(list, totalLength) {
      if (!Array.isArray(list)) throw new TypeError('list must be an Array');
      if (list.length === 0) return new Buffer(0);
      if (totalLength === undefined) {
        totalLength = list.reduce((acc, curr) => acc + curr.length, 0);
      }
      const result = new Buffer(totalLength);
      let offset = 0;
      for (const item of list) {
        result.set(item, offset);
        offset += item.length;
      }
      return result;
    }

    static allocUnsafe(size) {
      return new Buffer(size);
    }

    static allocUnsafeSlow(size) {
      return new Buffer(size);
    }

    toString(encoding = 'utf8', start = 0, end = this.length) {
      const view = this.subarray(start, end === undefined ? this.length : end);
      if (encoding === 'hex') {
        return Array.from(view).map(b => b.toString(16).padStart(2, '0')).join('');
      }
      if (encoding === 'base64' || encoding === 'base64url') {
        return encodeBase64(view, encoding === 'base64url');
      }
      return textDecoder.decode(view);
    }

    slice(start, end) {
      return this.subarray(start, end);
    }

    copy(target, targetStart = 0, sourceStart = 0, sourceEnd = this.length) {
      const start = targetStart >>> 0;
      const end = Math.min(this.length, sourceEnd >>> 0);
      let j = sourceStart >>> 0;
      for (let i = start; j < end && i < target.length; i++, j++) {
        target[i] = this[j];
      }
      return j - (sourceStart >>> 0);
    }

    write(string, offset = 0, length, encoding) {
      if (typeof offset === 'string') {
        encoding = offset;
        offset = 0;
        length = undefined;
      } else if (typeof length === 'string') {
        encoding = length;
        length = undefined;
      }
      const data = Buffer.from(String(string), encoding || 'utf8');
      const max = Math.min(
          data.length, length === undefined ? data.length : length,
          Math.max(0, this.length - offset));
      for (let i = 0; i < max; i++) {
        this[offset + i] = data[i];
      }
      return max;
    }

    writeUInt8(value, offset = 0) {
      this[offset] = value & 0xff;
      return offset + 1;
    }
    readUInt8(offset = 0) {
      return this[offset];
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
    writeInt8(value, offset = 0) {
      this[offset] = value & 0xff;
      return offset + 1;
    }
    readInt8(offset = 0) {
      const v = this[offset];
      return v & 0x80 ? v - 0x100 : v;
    }
    writeUInt16LE(value, offset = 0) {
      this[offset] = value & 0xff;
      this[offset + 1] = (value >>> 8) & 0xff;
      return offset + 2;
    }
    readUInt16LE(offset = 0) {
      return this[offset] | (this[offset + 1] << 8);
    }
    readInt16LE(offset = 0) {
      const value = this.readUIntLE(offset, 2);
      return value & 0x8000 ? value - 0x10000 : value;
    }
    writeUInt16BE(value, offset = 0) {
      this[offset] = (value >>> 8) & 0xff;
      this[offset + 1] = value & 0xff;
      return offset + 2;
    }
    readUInt16BE(offset = 0) {
      return (this[offset] << 8) | this[offset + 1];
    }
    writeUInt32LE(value, offset = 0) {
      this[offset] = value & 0xff;
      this[offset + 1] = (value >>> 8) & 0xff;
      this[offset + 2] = (value >>> 16) & 0xff;
      this[offset + 3] = (value >>> 24) & 0xff;
      return offset + 4;
    }
    readUInt32LE(offset = 0) {
      return (
          (this[offset] | (this[offset + 1] << 8) | (this[offset + 2] << 16) |
           (this[offset + 3] << 24)) >>>
          0);
    }
    writeUInt32BE(value, offset = 0) {
      this[offset] = (value >>> 24) & 0xff;
      this[offset + 1] = (value >>> 16) & 0xff;
      this[offset + 2] = (value >>> 8) & 0xff;
      this[offset + 3] = value & 0xff;
      return offset + 4;
    }
    readUInt32BE(offset = 0) {
      return (
          ((this[offset] << 24) | (this[offset + 1] << 16) |
           (this[offset + 2] << 8) | this[offset + 3]) >>>
          0);
    }
    writeInt32LE(value, offset = 0) {
      return this.writeUInt32LE(value, offset);
    }
    readInt32LE(offset = 0) {
      return this.readUInt32LE(offset) | 0;
    }
    writeInt32BE(value, offset = 0) {
      return this.writeUInt32BE(value, offset);
    }
    readInt32BE(offset = 0) {
      return this.readUInt32BE(offset) | 0;
    }
    writeDoubleLE(value, offset = 0) {
      new DataView(this.buffer, this.byteOffset + offset, 8).setFloat64(0, value, true);
      return offset + 8;
    }
    readDoubleLE(offset = 0) {
      return new DataView(this.buffer, this.byteOffset + offset, 8).getFloat64(0, true);
    }
    writeFloatLE(value, offset = 0) {
      new DataView(this.buffer, this.byteOffset + offset, 4).setFloat32(0, value, true);
      return offset + 4;
    }
    readFloatLE(offset = 0) {
      return new DataView(this.buffer, this.byteOffset + offset, 4).getFloat32(0, true);
    }
    fill(value, start = 0, end = this.length) {
      Uint8Array.prototype.fill.call(this, value, start, end);
      return this;
    }
    equals(other) {
      if (!other || other.length !== this.length) return false;
      for (let i = 0; i < this.length; i++) {
        if (this[i] !== other[i]) return false;
      }
      return true;
    }
  }

  Object.defineProperty(Buffer.prototype, 'readUintLE', {
    value: Buffer.prototype.readUIntLE, configurable: true, writable: true,
  });

  const utilModule = {
    promisify: fn => (...args) => new Promise((resolve, reject) => {
      fn(...args, (err, res) => err ? reject(err) : resolve(res));
    }),
    callbackify(fn) {
      return (...args) => {
        const cb = args.pop();
        fn(...args).then(res => cb(null, res), err => cb(err));
      };
    },
    format: (...args) => args.map(a => typeof a === 'object' ? JSON.stringify(a) : String(a)).join(' '),
    inspect: obj => {
      try { return JSON.stringify(obj, null, 2); } catch { return String(obj); }
    },
    inherits: (ctor, superCtor) => {
      if (typeof ctor !== 'function' || typeof superCtor !== 'function') {
        throw new TypeError('The constructor and super constructor must be functions');
      }
      ctor.super_ = superCtor;
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
    types: {
      isDate: (v) => v instanceof Date,
      isRegExp: (v) => v instanceof RegExp,
      isNativeError: (v) => v instanceof Error,
      isBuffer: (v) => Buffer.isBuffer(v),
      isArrayBuffer: (v) => v instanceof ArrayBuffer,
      isUint8Array: (v) => v instanceof Uint8Array,
      isPromise: (v) => !!v && typeof v.then === 'function',
    },
    isArray: Array.isArray,
    isBoolean: (v) => typeof v === 'boolean',
    isBuffer: (v) => Buffer.isBuffer(v),
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
    deprecate: (fn) => fn,
    TextEncoder,
    TextDecoder,
  };
  utilModule.default = utilModule;

  // --- 6. In-memory FS (do not claim every path exists) ---
  function normalizeFsPath(p) {
    let s = String(p || '').replace(/\//g, '\\');
    if (s.length > 3 && s.endsWith('\\')) {
      s = s.slice(0, -1);
    }
    return s;
  }

  function fsParentPath(p) {
    const i = p.lastIndexOf('\\');
    if (i <= 0) {
      return '';
    }
    if (p.length >= 2 && p[1] === ':' && i === 2) {
      return p.slice(0, 3);
    }
    return p.slice(0, i);
  }

  function fsErrno(code, op, p) {
    const err = new Error(`${code}: ${op} '${p}'`);
    err.code = code;
    return err;
  }

  const memFiles = new Map();

  function fsEnsureDir(p) {
    p = normalizeFsPath(p);
    if (!p) {
      return;
    }
    const parent = fsParentPath(p);
    if (parent && parent !== p && !memFiles.has(parent)) {
      fsEnsureDir(parent);
    }
    const existing = memFiles.get(p);
    if (existing && existing.type === 'file') {
      throw fsErrno('ENOTDIR', 'mkdir', p);
    }
    memFiles.set(p, {type: 'dir', data: null, mtimeMs: Date.now()});
  }

  function fsMakeStats(entry) {
    return {
      isFile: () => entry.type === 'file',
      isDirectory: () => entry.type === 'dir',
      isSymbolicLink: () => false,
      size: entry.data ? entry.data.length : 0,
      mtimeMs: entry.mtimeMs,
      mtime: new Date(entry.mtimeMs),
    };
  }

  function fsEncodingOf(encoding) {
    if (!encoding) {
      return null;
    }
    if (typeof encoding === 'string') {
      return encoding;
    }
    return encoding.encoding || null;
  }

  const fsConstants = {F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1};

  // Electron's node-integrated renderer exposes Node's real `fs` module. This
  // renderer remains Chromium-sandboxed, so Windows paths are brokered to the
  // Browser process (whose filesystem bridge also understands ASAR archives).
  // Only chrome:// resources use the synthetic mount. In particular, an
  // extracted application's appPath is a real filesystem root: treating it as
  // virtual hides its package metadata, configuration, and plugins.
  function fsUsesVirtualMount(path) {
    const normalized = normalizeFsPath(path).toLowerCase();
    return normalized.startsWith('chrome:\\');
  }

  const fsVirtualMountRoot = (() => {
    const pageLocation = globalThis.location;
    if (!pageLocation || String(pageLocation.protocol).toLowerCase() !== 'chrome:' ||
        !pageLocation.hostname) {
      return '';
    }
    return `chrome:\\${pageLocation.hostname}`;
  })();
  const fsVirtualPackagePath = fsVirtualMountRoot ?
      fsVirtualMountRoot + '\\package.json' : '';

  function fsIsVirtualRoot(normalizedPath) {
    return Boolean(fsVirtualMountRoot) &&
        normalizeFsPath(normalizedPath).toLowerCase() ===
            fsVirtualMountRoot.toLowerCase();
  }

  function fsIsVirtualPackageJson(normalizedPath) {
    return Boolean(fsVirtualPackagePath) &&
        normalizeFsPath(normalizedPath).toLowerCase() ===
            fsVirtualPackagePath.toLowerCase();
  }

  function fsGetVirtualEntry(normalizedPath) {
    const entry = memFiles.get(normalizedPath);
    if (entry) {
      return entry;
    }
    if (fsIsVirtualPackageJson(normalizedPath)) {
      const pkg = JSON.stringify({
        name: hostedAppName || 'app',
        version: hostedAppVersion,
      });
      return {
        type: 'file',
        data: Buffer.from(pkg, 'utf8'),
        mtimeMs: 0,
      };
    }
    if (fsIsVirtualRoot(normalizedPath)) {
      return {type: 'dir', data: null, mtimeMs: 0};
    }
    return null;
  }

  function fsNodeError(error, syscall, path) {
    const message = String(error && error.message || error || 'EIO: fs error');
    const match = /^([A-Z][A-Z0-9_]+):/.exec(message);
    const result = new Error(message);
    result.code = match ? match[1] : 'EIO';
    result.errno = result.code;
    result.syscall = syscall;
    result.path = String(path);
    return result;
  }

  function fsNativeSync(operation, path, extra = {}) {
    try {
      return transport.sendSync('__xenon:fs', {
        operation,
        path: normalizeFsPath(path),
        ...extra,
      });
    } catch (error) {
      throw fsNodeError(error, operation, path);
    }
  }

  function fsNativeAsync(operation, path, extra = {}) {
    return transport.invoke('__xenon:fs', {
      operation,
      path: normalizeFsPath(path),
      ...extra,
    }).catch(error => {
      throw fsNodeError(error, operation, path);
    });
  }

  function fsBytes(data) {
    return ArrayBuffer.isView(data) ?
        Buffer.from(new Uint8Array(data.buffer, data.byteOffset, data.byteLength)) :
        Buffer.from(String(data), 'utf8');
  }

  function fsNativeStats(stat) {
    return {
      isFile: () => Boolean(stat.isFile),
      isDirectory: () => Boolean(stat.isDirectory),
      isSymbolicLink: () => Boolean(stat.isSymbolicLink),
      size: Number(stat.size) || 0,
      mtimeMs: Number(stat.mtimeMs) || 0,
      mtime: new Date(Number(stat.mtimeMs) || 0),
      birthtimeMs: Number(stat.birthtimeMs) || 0,
      birthtime: new Date(Number(stat.birthtimeMs) || 0),
      mode: stat.isDirectory ? 0o777 : 0o666,
    };
  }

  function fsReadResult(encoded, options) {
    const buffer = Buffer.from(String(encoded || ''), 'base64');
    const encoding = fsEncodingOf(options);
    return encoding ? buffer.toString(encoding) : buffer;
  }

  function fsAsyncOperation(path, operation, extra, virtualCall, transform) {
    const promise = fsUsesVirtualMount(path) ?
        Promise.resolve().then(virtualCall) :
        fsNativeAsync(operation, path, extra);
    return transform ? promise.then(transform) : promise;
  }

  function fsValidateCallback(callback) {
    if (typeof callback !== 'function') {
      const error = new TypeError('The callback argument must be a function');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
  }

  function fsCallback(operation, callback, includeValue = false) {
    fsValidateCallback(callback);
    let promise;
    try {
      promise = operation();
    } catch (error) {
      promise = Promise.reject(error);
    }
    promise.then(
        value => includeValue ? callback(null, value) : callback(null),
        error => callback(error));
  }

  function fsUnsupported(method) {
    const error = new Error(`fs.${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }

  const fsModule = {
    constants: fsConstants,
    existsSync: (path) => {
      if (fsUsesVirtualMount(path)) {
        return Boolean(fsGetVirtualEntry(normalizeFsPath(path)));
      }
      return Boolean(fsNativeSync('exists', path));
    },
    statSync: (path) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeStats(fsNativeSync('stat', path));
      }
      const entry = fsGetVirtualEntry(normalizeFsPath(path));
      if (!entry) {
        throw fsErrno('ENOENT', 'stat', path);
      }
      return fsMakeStats(entry);
    },
    lstatSync: (path) => fsModule.statSync(path),
    readFileSync: (path, encoding) => {
      if (!fsUsesVirtualMount(path)) {
        return fsReadResult(fsNativeSync('read_file', path), encoding);
      }
      const entry = fsGetVirtualEntry(normalizeFsPath(path));
      if (!entry) {
        throw fsErrno('ENOENT', 'open', path);
      }
      if (entry.type !== 'file') {
        throw fsErrno('EISDIR', 'read', path);
      }
      const enc = fsEncodingOf(encoding);
      const buf = Buffer.from(entry.data || new Uint8Array(0));
      return enc ? buf.toString(enc) : buf;
    },
    writeFileSync: (path, data, _options) => {
      if (!fsUsesVirtualMount(path)) {
        fsNativeSync('write_file', path, {dataBase64: fsBytes(data).toString('base64')});
        return;
      }
      const p = normalizeFsPath(path);
      if (fsIsVirtualPackageJson(p)) {
        throw fsErrno('EROFS', 'open', path);
      }
      const parent = fsParentPath(p);
      if (parent) {
        fsEnsureDir(parent);
      }
      const bytes = fsBytes(data);
      memFiles.set(p, {type: 'file', data: bytes, mtimeMs: Date.now()});
    },
    mkdirSync: (path, opts) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('mkdir', path, {
          recursive: Boolean(opts && typeof opts === 'object' && opts.recursive),
        });
      }
      fsEnsureDir(normalizeFsPath(path));
    },
    readdirSync: (path) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('readdir', path);
      }
      const dir = normalizeFsPath(path);
      const entry = fsGetVirtualEntry(dir);
      if (!entry || entry.type !== 'dir') {
        throw fsErrno('ENOENT', 'scandir', path);
      }
      const prefix = dir.endsWith('\\') ? dir : dir + '\\';
      const names = new Set();
      if (fsIsVirtualRoot(dir)) {
        names.add('package.json');
      }
      for (const key of memFiles.keys()) {
        if (!key.startsWith(prefix)) {
          continue;
        }
        const rest = key.slice(prefix.length);
        if (rest && !rest.includes('\\')) {
          names.add(rest);
        }
      }
      return [...names];
    },
    unlinkSync: (path) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('unlink', path);
      }
      const p = normalizeFsPath(path);
      const entry = memFiles.get(p);
      if (!entry && fsIsVirtualPackageJson(p)) {
        throw fsErrno('EROFS', 'unlink', path);
      }
      if (!entry || entry.type !== 'file') {
        throw fsErrno('ENOENT', 'unlink', path);
      }
      memFiles.delete(p);
    },
    rmSync: (path, opts) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('rm', path, {
          recursive: Boolean(opts && opts.recursive),
          force: Boolean(opts && opts.force),
        });
      }
      const p = normalizeFsPath(path);
      if (!memFiles.has(p)) {
        if (fsGetVirtualEntry(p)) {
          throw fsErrno('EROFS', 'rm', path);
        }
        if (opts && opts.force) return;
        throw fsErrno('ENOENT', 'rm', path);
      }
      memFiles.delete(p);
    },
    rmdirSync: (path, opts) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('rmdir', path, {
          recursive: Boolean(opts && opts.recursive),
        });
      }
      return fsModule.rmSync(path, opts);
    },
    accessSync: (path, _mode) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('access', path);
      }
      const p = normalizeFsPath(path);
      if (!fsGetVirtualEntry(p)) {
        throw fsErrno('ENOENT', 'access', path);
      }
    },
    appendFileSync: (path, data) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('append_file', path, {
          dataBase64: fsBytes(data).toString('base64'),
        });
      }
      let prev = Buffer.alloc(0);
      try {
        prev = fsModule.readFileSync(path);
      } catch (err) {
        if (!err || err.code !== 'ENOENT') {
          throw err;
        }
      }
      const extra = fsBytes(data);
      fsModule.writeFileSync(path, Buffer.concat([prev, extra]));
    },
    renameSync: (oldPath, newPath) => {
      if (!fsUsesVirtualMount(oldPath) || !fsUsesVirtualMount(newPath)) {
        if (fsUsesVirtualMount(oldPath) !== fsUsesVirtualMount(newPath)) {
          throw fsErrno('EXDEV', 'rename', oldPath);
        }
        return fsNativeSync('rename', oldPath, {
          destination: normalizeFsPath(newPath),
        });
      }
      const from = normalizeFsPath(oldPath);
      const entry = memFiles.get(from);
      if (!entry && fsGetVirtualEntry(from)) {
        throw fsErrno('EROFS', 'rename', oldPath);
      }
      if (!entry) {
        throw fsErrno('ENOENT', 'rename', oldPath);
      }
      memFiles.set(normalizeFsPath(newPath), entry);
      memFiles.delete(from);
    },
    copyFileSync: (src, dest) => {
      if (!fsUsesVirtualMount(src) || !fsUsesVirtualMount(dest)) {
        if (fsUsesVirtualMount(src) !== fsUsesVirtualMount(dest)) {
          throw fsErrno('EXDEV', 'copyfile', src);
        }
        return fsNativeSync('copy_file', src, {
          destination: normalizeFsPath(dest),
        });
      }
      fsModule.writeFileSync(dest, fsModule.readFileSync(src));
    },
    promises: {},
  };

  fsModule.readFile = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    fsCallback(() => fsAsyncOperation(
        path, 'read_file', {}, () => fsModule.readFileSync(path, options),
        encoded => fsUsesVirtualMount(path) ? encoded : fsReadResult(encoded, options)),
        callback, true);
  };
  fsModule.writeFile = (path, data, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {dataBase64: fsBytes(data).toString('base64')};
    fsCallback(() => fsAsyncOperation(
        path, 'write_file', extra,
        () => fsModule.writeFileSync(path, data, options)), callback);
  };
  fsModule.stat = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'stat', {}, () => fsModule.statSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
        callback, true);
  };
  fsModule.lstat = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'lstat', {}, () => fsModule.lstatSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
        callback, true);
  };
  fsModule.readdir = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'readdir', {}, () => fsModule.readdirSync(path)), callback, true);
  };
  fsModule.mkdir = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {recursive: Boolean(options && options.recursive)};
    fsCallback(() => fsAsyncOperation(
        path, 'mkdir', extra, () => fsModule.mkdirSync(path, options)), callback);
  };
  fsModule.unlink = (path, callback) => fsCallback(() => fsAsyncOperation(
      path, 'unlink', {}, () => fsModule.unlinkSync(path)), callback);
  fsModule.rm = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {
      recursive: Boolean(options && options.recursive),
      force: Boolean(options && options.force),
    };
    fsCallback(() => fsAsyncOperation(
        path, 'rm', extra, () => fsModule.rmSync(path, options)), callback);
  };
  fsModule.rmdir = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {recursive: Boolean(options && options.recursive)};
    fsCallback(() => fsAsyncOperation(
        path, 'rmdir', extra, () => fsModule.rmdirSync(path, options)), callback);
  };
  fsModule.access = (path, mode, callback) => {
    if (typeof mode === 'function') callback = mode;
    fsCallback(() => fsAsyncOperation(
        path, 'access', {}, () => fsModule.accessSync(path, mode)), callback);
  };
  fsModule.appendFile = (path, data, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {dataBase64: fsBytes(data).toString('base64')};
    fsCallback(() => fsAsyncOperation(
        path, 'append_file', extra,
        () => fsModule.appendFileSync(path, data, options)), callback);
  };
  fsModule.rename = (oldPath, newPath, callback) => fsCallback(
      () => fsUsesVirtualMount(oldPath) ?
          Promise.resolve().then(() => fsModule.renameSync(oldPath, newPath)) :
          fsNativeAsync('rename', oldPath, {destination: normalizeFsPath(newPath)}),
      callback);
  fsModule.copyFile = (src, dest, flags, callback) => {
    if (typeof flags === 'function') callback = flags;
    fsCallback(() => fsUsesVirtualMount(src) ?
        Promise.resolve().then(() => fsModule.copyFileSync(src, dest)) :
        fsNativeAsync('copy_file', src, {destination: normalizeFsPath(dest)}),
        callback);
  };
  fsModule.exists = (path, callback) => {
    fsValidateCallback(callback);
    const promise = fsUsesVirtualMount(path) ?
        Promise.resolve(fsModule.existsSync(path)) :
        fsNativeAsync('exists', path);
    promise.then(value => callback(Boolean(value)), () => callback(false));
  };

  const realpathSync = (path) => fsUsesVirtualMount(path) ?
      String(path) : fsNativeSync('realpath', path);
  realpathSync.native = realpathSync;
  fsModule.realpathSync = realpathSync;
  const realpath = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'realpath', {}, () => String(path)), callback, true);
  };
  realpath.native = realpath;
  fsModule.realpath = realpath;

  fsModule.promises = {
    stat: (path) => fsAsyncOperation(
        path, 'stat', {}, () => fsModule.statSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
    lstat: (path) => fsAsyncOperation(
        path, 'lstat', {}, () => fsModule.lstatSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
    readFile: (path, options) => fsAsyncOperation(
        path, 'read_file', {}, () => fsModule.readFileSync(path, options),
        encoded => fsUsesVirtualMount(path) ? encoded : fsReadResult(encoded, options)),
    writeFile: (path, data, options) => fsAsyncOperation(
        path, 'write_file', {dataBase64: fsBytes(data).toString('base64')},
        () => fsModule.writeFileSync(path, data, options)),
    mkdir: (path, options) => fsAsyncOperation(
        path, 'mkdir', {recursive: Boolean(options && options.recursive)},
        () => fsModule.mkdirSync(path, options)),
    readdir: (path) => fsAsyncOperation(
        path, 'readdir', {}, () => fsModule.readdirSync(path)),
    unlink: (path) => fsAsyncOperation(
        path, 'unlink', {}, () => fsModule.unlinkSync(path)),
    rm: (path, options) => fsAsyncOperation(
        path, 'rm', {
          recursive: Boolean(options && options.recursive),
          force: Boolean(options && options.force),
        }, () => fsModule.rmSync(path, options)),
    rmdir: (path, options) => fsAsyncOperation(
        path, 'rmdir', {recursive: Boolean(options && options.recursive)},
        () => fsModule.rmdirSync(path, options)),
    access: (path, mode) => fsAsyncOperation(
        path, 'access', {}, () => fsModule.accessSync(path, mode)),
    appendFile: (path, data, options) => fsAsyncOperation(
        path, 'append_file', {dataBase64: fsBytes(data).toString('base64')},
        () => fsModule.appendFileSync(path, data, options)),
    rename: (oldPath, newPath) => fsUsesVirtualMount(oldPath) ?
        Promise.resolve().then(() => fsModule.renameSync(oldPath, newPath)) :
        fsNativeAsync('rename', oldPath, {destination: normalizeFsPath(newPath)}),
    copyFile: (src, dest) => fsUsesVirtualMount(src) ?
        Promise.resolve().then(() => fsModule.copyFileSync(src, dest)) :
        fsNativeAsync('copy_file', src, {destination: normalizeFsPath(dest)}),
    realpath: (path) => fsAsyncOperation(
        path, 'realpath', {}, () => String(path)),
  };
  // These APIs need descriptors, streaming, watchers, or permission support.
  // Do not return empty streams or report success without doing the operation.
  for (const method of [
    'watch', 'watchFile', 'unwatchFile',
    'chmodSync', 'chownSync', 'openSync', 'closeSync', 'readSync', 'writeSync',
  ]) {
    fsModule[method] = () => fsUnsupported(method);
  }
  for (const method of ['chmod', 'chown', 'open', 'close', 'read', 'write']) {
    fsModule[method] = (...args) => fsCallback(
        async () => fsUnsupported(method), args[args.length - 1]);
  }
  for (const method of ['chmod', 'chown', 'open']) {
    fsModule.promises[method] = async () => fsUnsupported('promises.' + method);
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


  fsModule.default = fsModule;

  // --- 7. Process Object ---
  const processEmitter = new EventEmitter();
  const process = globalThis.process = {
    isMainFrame: injectedPaths.isMainFrame !== false,
    platform: runtimePlatform,
    arch: runtimeArch,
    type: 'renderer',
    versions: {
      electron: '31.0.0',
      chrome: '142.0.0.0',
      node: '20.0.0',
      v8: '13.0.0.0',
    },
    version: 'v20.0.0',
    env: {
      NODE_ENV: 'production',
      APPDATA: appData,
      LOCALAPPDATA: localAppData,
      TEMP: tempDir,
      APP_BASE_DIR: exeDir,
      ...(homeDir ? {USERPROFILE: homeDir, HOME: homeDir} : {}),
    },
    pid: 1,
    execPath,
    argv: [execPath],
    cwd: () => userData,
    // Electron process.getSystemVersion() — OS release string.
    getSystemVersion: () => osModule.release(),
    nextTick(callback, ...args) {
      if (typeof callback !== 'function') {
        const error = new TypeError('The "callback" argument must be a function');
        error.code = 'ERR_INVALID_ARG_TYPE';
        throw error;
      }
      queueMicrotask(() => callback(...args));
    },
    contextId: '1',
    contextIsolated: false,
    sandboxed: false,
    _linkedBinding: (name) => {
      if (name === 'electron_common_v8_util' || name === 'v8_util') {
        return {
          getHiddenValue: (obj, key) => (obj ? obj['__v8_' + key] : undefined),
          setHiddenValue: (obj, key, val) => { if (obj) obj['__v8_' + key] = val; },
          deleteHiddenValue: (obj, key) => { if (obj) delete obj['__v8_' + key]; },
          requestGarbageCollectionForTesting: () => {},
        };
      }
      return {};
    },
    on: (evt, fn) => processEmitter.on(evt, fn),
    addListener: (evt, fn) => processEmitter.addListener(evt, fn),
    once: (evt, fn) => processEmitter.once(evt, fn),
    emit: (evt, ...args) => processEmitter.emit(evt, ...args),
    removeListener: (evt, fn) => processEmitter.removeListener(evt, fn),
    off: (evt, fn) => processEmitter.off(evt, fn),
  };

  // --- 8. Mojo Node-API Native Addon Dynamic Bridge ---
  let mojoBridgePromise = null;
  let mojoRouter = null;
  let mojoHandler = null;

  const pendingMojoInvokes = new Map();
  const pendingMojoConstructs = new Map();
  const mojoCallbacks = new Map();
  const nativeHandleSymbol = Symbol('xenon.nativeHandle');
  const nativePrototypeMethods = new WeakSet();
  const nativeProxyCache = new Map();
  const pendingNativeConstructors = new Set();
  let nextMojoRequestId = 1;
  let nextMojoCallbackId = 1;
  let nativeServiceGeneration = 0;
  // Serialization replaces native arguments with ids. Keep only the collected
  // handles alive until the corresponding async native operation completes.
  const pendingNativeHandleRoots = new Set();
  function retainNativeHandles(result, handles) {
    if (!handles || !result || typeof result.then !== 'function') return result;
    pendingNativeHandleRoots.add(handles);
    return Promise.resolve(result).then(value => {
      pendingNativeHandleRoots.delete(handles);
      return value;
    }, error => {
      pendingNativeHandleRoots.delete(handles);
      throw error;
    });
  }
  // The held record must never reference the handle or its proxy. Register the
  // handle, so an extracted instance method also keeps its native receiver live.
  const nativeHandleFinalizer = typeof FinalizationRegistry === 'function' &&
      typeof transport.releaseNodeInstance === 'function' ?
      new FinalizationRegistry(record => {
        const key = nativeProxyKey(record);
        const cached = key && nativeProxyCache.get(key);
        // A wire value can arrive after collection but before this finalizer.
        // A replacement wrapper then owns this same lease; do not release it.
        if (cached && cached.record !== record) return;
        if (key) nativeProxyCache.delete(key);
        try {
          transport.releaseNodeInstance(
              record.modulePath, record.instanceId, record.ownerToken);
        } catch (_error) {
          // Disconnected owners are reclaimed by the service. Cleanup must not
          // reconnect or turn a GC callback into an unhandled application error.
        }
      }) : null;

  function trackNativeHandle(handle) {
    if (!nativeHandleFinalizer || handle.tracked ||
        typeof handle.modulePath !== 'string' ||
        !Number.isInteger(handle.instanceId) ||
        typeof handle.ownerToken !== 'string' || !handle.ownerToken) return;
    const record = Object.freeze({
      modulePath: handle.modulePath,
      instanceId: handle.instanceId,
      ownerToken: handle.ownerToken,
    });
    nativeHandleFinalizer.register(handle, record);
    handle.finalizerRecord = record;
    handle.tracked = true;
  }

  function nativeProxyKey(handle) {
    if (typeof WeakRef !== 'function' || !handle.ownerToken ||
        !Number.isInteger(handle.instanceId)) return null;
    return JSON.stringify([handle.modulePath, handle.ownerToken, handle.instanceId]);
  }

  function rememberNativeProxy(handle, proxy) {
    const key = nativeProxyKey(handle);
    if (key) nativeProxyCache.set(key, {
      handle: new WeakRef(handle), proxy: new WeakRef(proxy),
      record: handle.finalizerRecord,
    });
  }

  function cachedNativeProxy(handle) {
    const key = nativeProxyKey(handle);
    return key && nativeProxyCache.get(key);
  }

  function invokeNativeCallback(callback, args, receiver) {
    const invoke = () => {
      try {
        callback.apply(adoptNativeReturn(receiver),
            args.map(arg => adoptNativeReturn(arg)));
      } catch (error) {
        console.error('[Xenon Node Callback Error]', error);
      }
    };
    // Native constructors can call back before their async IPC reply has
    // registered the wrapper. Wait for those replies before resolving `this`.
    if (receiver && receiver.__xenon_node_wire_type__ === 'native_instance' &&
        pendingNativeConstructors.size) {
      Promise.allSettled([...pendingNativeConstructors]).then(invoke);
    } else {
      invoke();
    }
  }

  function assignNativeInstanceResult(handle, result) {
    const id = result && typeof result === 'object' ? result.instance_id : result;
    if (!Number.isInteger(id)) {
      throw new TypeError('Native constructor returned an invalid instance id');
    }
    handle.instanceId = id;
    if (result && typeof result.owner_token === 'string') {
      handle.ownerToken = result.owner_token;
    }
    trackNativeHandle(handle);
  }

  function nativeInstanceSelector(handle, instanceId) {
    return handle.ownerToken ? {instanceId, ownerToken: handle.ownerToken} : instanceId;
  }

  function callbackWire(callback) {
    const callbackId = nextMojoCallbackId++;
    mojoCallbacks.set(callbackId, captureAsyncCallback(callback));
    return {
      __xenon_node_wire_type__: 'callback',
      callback_id: callbackId,
    };
  }

  dispatchNodeAddon = (channel, values) => {
    const callbackId = Number(values && values[0]);
    if (!Number.isInteger(callbackId)) {
      return true;
    }
    if (channel === '__xenon:node-addon:callback-released') {
      mojoCallbacks.delete(callbackId);
      return true;
    }
    if (channel !== '__xenon:node-addon:callback') {
      return false;
    }
    const callback = mojoCallbacks.get(callbackId);
    if (!callback) {
      return true;
    }
    const args = Array.isArray(values[1]) ? values[1] : [];
    invokeNativeCallback(callback, args, values[2]);
    return true;
  };

  function getNativeHandle(value) {
    if (!value || (typeof value !== 'object' && typeof value !== 'function')) {
      return null;
    }
    try {
      return value[nativeHandleSymbol] || null;
    } catch (_error) {
      return null;
    }
  }

  function nativeInstanceWire(handle, instanceId) {
    if (!Number.isInteger(instanceId)) {
      throw new TypeError('Native addon instance id is not resolved');
    }
    return {
      __xenon_node_wire_type__: 'native_instance',
      module_path: handle.modulePath,
      instance_id: instanceId,
      ...(handle.ownerToken ? {owner_token: handle.ownerToken} : {}),
    };
  }

  const nativeTypedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype);
  const nativeTypedArrayName = Object.getOwnPropertyDescriptor(
      nativeTypedArrayPrototype, Symbol.toStringTag).get;
  const nativeTypedArrayBuffer = Object.getOwnPropertyDescriptor(
      nativeTypedArrayPrototype, 'buffer').get;
  const nativeTypedArrayOffset = Object.getOwnPropertyDescriptor(
      nativeTypedArrayPrototype, 'byteOffset').get;
  const nativeTypedArrayLength = Object.getOwnPropertyDescriptor(
      nativeTypedArrayPrototype, 'byteLength').get;
  const nativeDataViewBuffer = Object.getOwnPropertyDescriptor(
      DataView.prototype, 'buffer').get;
  const nativeDataViewOffset = Object.getOwnPropertyDescriptor(
      DataView.prototype, 'byteOffset').get;
  const nativeDataViewLength = Object.getOwnPropertyDescriptor(
      DataView.prototype, 'byteLength').get;
  const nativeBinaryConstructors = new Map([
    ['Int8Array', Int8Array], ['Uint8Array', Uint8Array],
    ['Uint8ClampedArray', Uint8ClampedArray], ['Int16Array', Int16Array],
    ['Uint16Array', Uint16Array], ['Int32Array', Int32Array],
    ['Uint32Array', Uint32Array], ['Float32Array', Float32Array],
    ['Float64Array', Float64Array], ['BigInt64Array', BigInt64Array],
    ['BigUint64Array', BigUint64Array],
    ...(typeof Float16Array === 'function' ? [['Float16Array', Float16Array]] : []),
  ]);

  function nativeBinaryView(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
    if (ArrayBuffer.isView(value)) {
      // Intrinsic getters work across realms and ignore shadowed properties,
      // constructors and Symbol.toStringTag on application objects.
      const name = nativeTypedArrayName.call(value);
      if (name !== undefined) {
        return {
          kind: Object.prototype.isPrototypeOf.call(Buffer.prototype, value) ?
              'Buffer' : name,
          buffer: nativeTypedArrayBuffer.call(value),
          offset: nativeTypedArrayOffset.call(value),
          length: nativeTypedArrayLength.call(value),
        };
      }
      return {kind: 'DataView', buffer: nativeDataViewBuffer.call(value),
        offset: nativeDataViewOffset.call(value),
        length: nativeDataViewLength.call(value)};
    }
    let length;
    try { length = arrayBufferByteLength.call(value); } catch (_) { return null; }
    return {kind: 'ArrayBuffer', buffer: value, offset: 0, length};
  }

  function copyNativeBinary(view) {
    // Copy only the active range. No bytes outside a subview cross the bridge,
    // and later mutation cannot change an already submitted async argument.
    return new Uint8Array(new Uint8Array(view.buffer, view.offset, view.length));
  }

  function wireNativeBinary(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
    const view = nativeBinaryView(value);
    if (view) {
      return {__xenon_node_wire_type__: 'binary', kind: view.kind,
        value: copyNativeBinary(view)};
    }
    if (sharedArrayBufferByteLength) {
      let shared = false;
      try { sharedArrayBufferByteLength.call(value); shared = true; } catch (_) {}
      if (shared) throw new TypeError('Native addon arguments do not support SharedArrayBuffer');
    }
    return null;
  }

  function adoptNativeBinary(value) {
    const kind = value.kind;
    if (kind !== 'Buffer' && kind !== 'ArrayBuffer' && kind !== 'DataView' &&
        !nativeBinaryConstructors.has(kind)) {
      throw new TypeError('Native addon returned an invalid binary kind');
    }
    const view = nativeBinaryView(value.value);
    if (!view || (view.kind !== 'ArrayBuffer' && view.kind !== 'Uint8Array' &&
                  view.kind !== 'Buffer')) {
      throw new TypeError('Native addon returned an invalid binary payload');
    }
    const Constructor = nativeBinaryConstructors.get(kind);
    if (Constructor && view.length % Constructor.BYTES_PER_ELEMENT !== 0) {
      throw new RangeError('Native addon returned an invalid binary byte length');
    }
    const bytes = copyNativeBinary(view);
    if (kind === 'Buffer') return new Buffer(bytes.buffer);
    if (kind === 'ArrayBuffer') return bytes.buffer;
    if (kind === 'DataView') return new DataView(bytes.buffer);
    return new Constructor(bytes.buffer);
  }

  function wireNativeArgumentSync(value, seen, retainedHandles) {
    const binary = wireNativeBinary(value);
    if (binary) return binary;
    const handle = getNativeHandle(value);
    if (handle) {
      if (retainedHandles) retainedHandles.add(handle);
      assertNativeHandleLive(handle);
      return nativeInstanceWire(handle, handle.instanceId);
    }
    if (typeof value === 'function') {
      return callbackWire(value);
    }
    if (typeof value === 'bigint') {
      return {
        __xenon_node_wire_type__: 'bigint',
        value: value.toString(),
      };
    }
    if (value === undefined) {
      return {__xenon_node_wire_type__: 'undefined'};
    }
    if (!value || typeof value !== 'object') {
      return value;
    }
    // Leaf values need no traversal state. Allocate only when descending into
    // a container, retaining one independent map per top-level argument.
    seen ||= new Map();
    if (seen.has(value)) {
      return seen.get(value);
    }
    if (Array.isArray(value)) {
      const result = [];
      seen.set(value, result);
      for (const item of value) {
        result.push(wireNativeArgumentSync(item, seen, retainedHandles));
      }
      return result;
    }
    const result = {};
    seen.set(value, result);
    for (const [key, item] of Object.entries(value)) {
      result[key] = wireNativeArgumentSync(item, seen, retainedHandles);
    }
    return result;
  }

  async function wireNativeArgumentAsync(value, seen, retainedHandles) {
    const binary = wireNativeBinary(value);
    if (binary) return binary;
    const handle = getNativeHandle(value);
    if (handle) {
      if (retainedHandles) retainedHandles.add(handle);
      const instanceId = await resolvedHandleInstanceId(handle);
      return nativeInstanceWire(handle, instanceId);
    }
    if (typeof value === 'function') {
      return callbackWire(value);
    }
    if (typeof value === 'bigint') {
      return {
        __xenon_node_wire_type__: 'bigint',
        value: value.toString(),
      };
    }
    if (value === undefined) {
      return {__xenon_node_wire_type__: 'undefined'};
    }
    if (!value || typeof value !== 'object') {
      return value;
    }
    seen ||= new Map();
    if (seen.has(value)) {
      return seen.get(value);
    }
    if (Array.isArray(value)) {
      const result = [];
      seen.set(value, result);
      // Visit all branches before awaiting native instance ids: a later
      // binary field must be snapshotted at call time, too.
      const pending = value.map(item => wireNativeArgumentAsync(item, seen, retainedHandles));
      for (const item of await Promise.all(pending)) result.push(item);
      return result;
    }
    const result = {};
    seen.set(value, result);
    await Promise.all(Object.entries(value).map(async ([key, item]) => {
      result[key] = undefined;
      result[key] = await wireNativeArgumentAsync(item, seen, retainedHandles);
    }));
    return result;
  }

  function valueToMojo(value) {
    if (typeof value === 'bigint') {
      return valueToMojo({
        __xenon_node_wire_type__: 'bigint',
        value: value.toString(),
      });
    }
    if (value === undefined || value === null) return { nullValue: 0 };
    if (typeof value === 'boolean') return { boolValue: value };
    if (typeof value === 'number') {
      if (Number.isInteger(value) && value >= -2147483648 && value <= 2147483647) {
        return { intValue: value };
      }
      return { doubleValue: value };
    }
    if (typeof value === 'string') return { stringValue: value };
    const binary = nativeBinaryView(value);
    if (binary) return {binaryValue: copyNativeBinary(binary)};
    if (Array.isArray(value)) {
      return { listValue: { storage: value.map(valueToMojo) } };
    }
    if (typeof value === 'object') {
      const storage = {};
      for (const [k, v] of Object.entries(value)) {
        storage[k] = valueToMojo(v);
      }
      return { dictionaryValue: { storage } };
    }
    return { stringValue: String(value) };
  }

  function valueFromMojo(value) {
    if (value === undefined || value === null) return null;
    if (typeof value !== 'object') return value;
    if (value.stringValue !== null && value.stringValue !== undefined) return value.stringValue;
    if (value.intValue !== null && value.intValue !== undefined) return value.intValue;
    if (value.doubleValue !== null && value.doubleValue !== undefined) return value.doubleValue;
    if (value.boolValue !== null && value.boolValue !== undefined) return value.boolValue;
    if (value.nullValue !== null && value.nullValue !== undefined) return null;
    if (value.binaryValue !== null && value.binaryValue !== undefined) {
      const binary = nativeBinaryView(value.binaryValue);
      if (binary) return copyNativeBinary(binary);
      if (Array.isArray(value.binaryValue) && value.binaryValue.every(byte =>
          Number.isInteger(byte) && byte >= 0 && byte <= 255)) {
        return new Uint8Array(value.binaryValue);
      }
      throw new TypeError('Native addon returned invalid Mojo binary bytes');
    }
    if (value.listValue && value.listValue.storage) return value.listValue.storage.map(valueFromMojo);
    if (value.dictionaryValue && value.dictionaryValue.storage) {
      const res = {};
      for (const [k, v] of Object.entries(value.dictionaryValue.storage)) {
        res[k] = valueFromMojo(v);
      }
      return res;
    }
    return value;
  }

  function toMojoInvokeArgs(args) {
    return args.map(arg => {
      if (typeof arg === 'function') {
        const callbackId = nextMojoCallbackId++;
        mojoCallbacks.set(callbackId, captureAsyncCallback(arg));
        return { isCallback: true, callbackId, value: { nullValue: 0 } };
      }
      return {
        isCallback: false,
        callbackId: 0,
        value: valueToMojo(wireNativeArgumentSync(arg)),
      };
    });
  }

  async function toMojoInvokeArgsAsync(args) {
    return Promise.all(args.map(async arg => {
      if (typeof arg === 'function') {
        const callbackId = nextMojoCallbackId++;
        mojoCallbacks.set(callbackId, captureAsyncCallback(arg));
        return {
          isCallback: true,
          callbackId,
          value: {nullValue: 0},
        };
      }
      return {
        isCallback: false,
        callbackId: 0,
        value: valueToMojo(await wireNativeArgumentAsync(arg)),
      };
    }));
  }

  function attachMojoListeners(router) {
    if (!router || router.__xenonBootstrapListenersAttached) {
      return;
    }
    router.__xenonBootstrapListenersAttached = true;

    router.nodeInvokeResult.addListener((reqId, success, result, cbResults, errorMsg) => {
      const pending = pendingMojoInvokes.get(reqId);
      if (!pending) return;
      pendingMojoInvokes.delete(reqId);
      if (!success) {
        pending.reject(new Error(errorMsg || 'Mojo node invoke failed'));
        return;
      }
      if (Array.isArray(cbResults)) {
        for (const cb of cbResults) {
          mojoCallbacks.get(cb.callbackId)?.(adoptNativeReturn(valueFromMojo(cb.value)));
        }
      }
      pending.resolve(adoptNativeReturn(valueFromMojo(result)));
    });

    router.nodeCallbackInvoked.addListener((callbackId, args, receiver) => {
      const cb = mojoCallbacks.get(callbackId);
      if (cb) {
        invokeNativeCallback(cb, args.map(valueFromMojo),
            receiver === undefined ? undefined : valueFromMojo(receiver));
      }
    });

    router.nodeConstructResult.addListener((reqId, success, instanceId, errorMsg) => {
      const pending = pendingMojoConstructs.get(reqId);
      if (!pending) return;
      pendingMojoConstructs.delete(reqId);
      if (!success) {
        pending.reject(new Error(errorMsg || 'Mojo node construct failed'));
        return;
      }
      pending.resolve(instanceId);
    });

    if (router.nodeServiceReset) {
      router.nodeServiceReset.addListener(() => {
        nativeServiceGeneration++;
        const resetError = new Error('Utility service restarted');
        for (const pending of pendingMojoInvokes.values()) {
          pending.reject(resetError);
        }
        pendingMojoInvokes.clear();
        for (const pending of pendingMojoConstructs.values()) {
          pending.reject(resetError);
        }
        pendingMojoConstructs.clear();
      });
    }
  }

  async function ensureMojoBridge() {
    const existingHandler = globalThis.__xenonPageHandler__;
    const existingRouter = globalThis.__xenonPageCallbackRouter__;
    if (existingHandler && existingRouter) {
      mojoHandler = existingHandler;
      mojoRouter = existingRouter;
      attachMojoListeners(mojoRouter);
      return mojoHandler;
    }
    if (mojoHandler) return mojoHandler;
    if (mojoBridgePromise) return mojoBridgePromise;

    mojoBridgePromise = (async () => {
      try {
        if (globalThis.__xenonPageHandler__ &&
            globalThis.__xenonPageCallbackRouter__) {
          mojoHandler = globalThis.__xenonPageHandler__;
          mojoRouter = globalThis.__xenonPageCallbackRouter__;
          attachMojoListeners(mojoRouter);
          return mojoHandler;
        }

        const { PageCallbackRouter, PageHandlerFactory, PageHandlerRemote } =
            await import('/xenon_node.mojom-webui.js');

        if (globalThis.__xenonPageHandler__ &&
            globalThis.__xenonPageCallbackRouter__) {
          mojoHandler = globalThis.__xenonPageHandler__;
          mojoRouter = globalThis.__xenonPageCallbackRouter__;
          attachMojoListeners(mojoRouter);
          return mojoHandler;
        }

        mojoRouter = new PageCallbackRouter();
        mojoHandler = new PageHandlerRemote();
        PageHandlerFactory.getRemote().createPageHandler(
            mojoRouter.$.bindNewPipeAndPassRemote(),
            mojoHandler.$.bindNewPipeAndPassReceiver());
        if (!injectedPaths.isGuest) {
          globalThis.__xenonPageHandler__ = mojoHandler;
          globalThis.__xenonPageCallbackRouter__ = mojoRouter;
          globalThis.__xenonPageHandlerBound__ = true;
        }
        attachMojoListeners(mojoRouter);
        return mojoHandler;
      } catch (err) {
        console.warn('[Xenon Renderer] Mojo Node bridge unavailable:', err);
        mojoBridgePromise = null;
        return null;
      }
    })();

    return mojoBridgePromise;
  }

  function argsHaveCallback(args) {
    const seen = new Set();
    const visit = (value) => {
      if (getNativeHandle(value)) {
        return false;
      }
      if (typeof value === 'function') {
        return true;
      }
      if (!value || typeof value !== 'object' || seen.has(value)) {
        return false;
      }
      seen.add(value);
      if (Array.isArray(value)) {
        return value.some(visit);
      }
      return Object.values(value).some(visit);
    };
    return args.some(visit);
  }

  // Packaged Vue calls
  //   client.callRemoteClientFunction(main, 'openElectronSelectFileDialog', opts)
  // which tunnels through Utility ipcMain. After a Utility crash that pipe is
  // dead (`__xenon:net:data ready=0`), so the picker never appears. Host the
  // dialog on the Browser UI thread via PageHandler instead.
  async function showNativeOpenDialog(options) {
    const opts = options && typeof options === 'object' ? options : {};
    const title = String(opts.title || '选择文件');
    const properties = Array.isArray(opts.properties) ? opts.properties : [];
    const multi = properties.includes('multiSelections');
    const directory = properties.includes('openDirectory');
    const exts = [];
    if (directory) {
      exts.push('__pick_directory__');
    }
    for (const filter of (opts.filters || [])) {
      for (const ext of (filter && filter.extensions) || []) {
        const trimmed = String(ext || '').replace(/^\./, '');
        if (trimmed) {
          exts.push(trimmed);
        }
      }
    }
    const handler = await ensureMojoBridge();
    if (!handler || typeof handler.openNativeFileDialog !== 'function') {
      return unsupportedElectronApi('dialog.showOpenDialog');
    }
    const result = await handler.openNativeFileDialog(title, exts, multi);
    const paths = Array.isArray(result) ? result : result && result.filePaths;
    if (!Array.isArray(paths)) {
      throw new Error('Native open dialog returned an invalid result');
    }
    return paths;
  }

  function wrapCallRemoteClientFunction(orig) {
    if (typeof orig !== 'function' || orig.__xenonSelectFileWrapped) {
      return orig;
    }
    const wrapped = function(...args) {
      // node-net-ipc: (context, fnName, ...fnArgs). context is 'main-process'.
      const fnName = typeof args[1] === 'string' ? args[1] :
          (typeof args[0] === 'string' ? args[0] : '');
      if (fnName === 'openElectronSelectFileDialog' ||
          fnName === 'showOpenDialog') {
        const options = typeof args[1] === 'string' ? args[2] : args[1];
        console.info(
            '[Xenon Renderer] openElectronSelectFileDialog -> native picker');
        return showNativeOpenDialog(options || {}).then(paths => [paths]);
      }
      return orig.apply(this, args);
    };
    wrapped.__xenonSelectFileWrapped = true;
    return wrapped;
  }

  function installSelectFileDialogHook() {
    if (installSelectFileDialogHook.installed) {
      return;
    }
    installSelectFileDialogHook.installed = true;
    try {
      Object.defineProperty(Object.prototype, 'callRemoteClientFunction', {
        configurable: true,
        enumerable: false,
        set(fn) {
          Object.defineProperty(this, 'callRemoteClientFunction', {
            configurable: true,
            enumerable: true,
            writable: true,
            value: wrapCallRemoteClientFunction(fn),
          });
        },
        get() {
          return undefined;
        },
      });
    } catch (error) {
      console.warn(
          '[Xenon Renderer] failed to hook callRemoteClientFunction setter:',
          error);
    }
    const nativeDefineProperty = Object.defineProperty;
    Object.defineProperty = function(obj, prop, desc) {
      if (String(prop) === 'callRemoteClientFunction' && desc) {
        if (typeof desc.value === 'function') {
          desc = Object.assign({}, desc, {
            value: wrapCallRemoteClientFunction(desc.value),
          });
        } else if (typeof desc.get === 'function') {
          const origGet = desc.get;
          desc = Object.assign({}, desc, {
            get() {
              return wrapCallRemoteClientFunction(origGet.call(this));
            },
          });
        }
      }
      return nativeDefineProperty.call(this, obj, prop, desc);
    };

  }
  installSelectFileDialogHook();

  function isNativeInstanceWire(value) {
    if (!value || typeof value !== 'object' ||
        value.__xenon_node_wire_type__ !== 'native_instance') {
      return false;
    }
    const instanceId = Number(value.instance_id);
    return Number.isInteger(instanceId);
  }

  function isNativeFunctionWire(value) {
    if (!value || typeof value !== 'object' ||
        value.__xenon_node_wire_type__ !== 'native_function') {
      return false;
    }
    return Number.isInteger(Number(value.instance_id));
  }

  function createNativeFunctionProxy(value, fallbackModulePath) {
    let handle = {
      modulePath: value.module_path || fallbackModulePath,
      className: '',
      instanceId: Number(value.instance_id),
      ownerToken: value.owner_token,
      generation: nativeServiceGeneration,
    };
    const cached = cachedNativeProxy(handle);
    const previousProxy = cached && cached.proxy.deref();
    if (previousProxy) return previousProxy;
    handle = cached && cached.handle.deref() || handle;
    const proxy = function(...args) {
      if (globalThis.__xenonNodeAddonTrace === true) {
        try {
          console.debug('[Xenon Node Remote Function]', JSON.stringify({
            modulePath: handle.modulePath,
            name: value.name || '',
            instanceId: handle.instanceId,
            arguments: args,
          }));
        } catch (error) {
          console.debug('[Xenon Node Remote Function]',
              handle.modulePath, value.name || '', error);
        }
      }
      return invokeInstanceMethod(handle, 'call', [undefined, ...args]);
    };
    Object.defineProperty(proxy, nativeHandleSymbol, {
      configurable: false,
      enumerable: false,
      writable: false,
      value: handle,
    });
    Object.defineProperty(proxy, '__instanceId', {
      configurable: true,
      enumerable: false,
      get() {
        return handle.instanceId;
      },
    });
    if (value.name) {
      try {
        Object.defineProperty(proxy, 'name', {
          configurable: true,
          value: String(value.name),
        });
      } catch (_error) {
      }
    }
    trackNativeHandle(handle);
    rememberNativeProxy(handle, proxy);
    return proxy;
  }

  function adoptNativeCallResult(value, modulePath, retainedHandles) {
    // The transport returns a Promise only when the operation already started
    // in Utility is still pending. Await that operation without invoking it
    // again; synchronous results keep their original return type.
    if (value && typeof value.then === 'function') {
      return retainNativeHandles(Promise.resolve(value).then(result =>
          adoptNativeReturn(result, modulePath)), retainedHandles);
    }
    return adoptNativeReturn(value, modulePath);
  }

  function adoptNativeReturn(value, fallbackModulePath) {
    if (nativeBinaryView(value)) return value;
    if (Array.isArray(value)) {
      return value.map(item => adoptNativeReturn(item, fallbackModulePath));
    }
    if (!value || typeof value !== 'object') {
      return value;
    }
    const wireType = value.__xenon_node_wire_type__;
    if (wireType === 'binary') return adoptNativeBinary(value);
    if (wireType === 'global') return globalThis;
    if (wireType === 'undefined') {
      return undefined;
    }
    if (wireType === 'bigint') {
      try {
        return BigInt(value.value || '0');
      } catch (_error) {
        return 0n;
      }
    }
    if (wireType === 'date') {
      return new Date(value.value);
    }
    if (wireType === 'number') {
      switch (value.value) {
        case 'nan':
          return NaN;
        case 'infinity':
          return Infinity;
        case '-infinity':
          return -Infinity;
        case '-0':
          return -0;
      }
    }
    if (isNativeFunctionWire(value)) {
      return createNativeFunctionProxy(value, fallbackModulePath);
    }
    if (isNativeInstanceWire(value)) {
      const prototypeMembers = Array.isArray(value.prototype) ?
          value.prototype : [];
      return createNativeInstanceProxy({
        modulePath: value.module_path || fallbackModulePath,
        className: value.class_name || '',
        instanceId: Number(value.instance_id),
        ownerToken: value.owner_token,
        generation: nativeServiceGeneration,
      }, prototypeMembers, null, value.fields);
    }
    const result = {};
    for (const [key, item] of Object.entries(value)) {
      result[key] = adoptNativeReturn(item, fallbackModulePath);
    }
    return result;
  }

  function assertNativeHandleLive(handle) {
    if (!handle || handle.released ||
        handle.generation !== nativeServiceGeneration) {
      const error = new Error('Native instance has been released or its service connection changed');
      error.code = 'ERR_NATIVE_INSTANCE_INVALIDATED';
      throw error;
    }
  }

  function resolvedHandleInstanceId(handle) {
    assertNativeHandleLive(handle);
    return Promise.resolve(handle.instanceId).then((instanceId) => {
      assertNativeHandleLive(handle);
      assignNativeInstanceResult(handle, instanceId);
      if (Number.isInteger(handle.instanceId)) {
        return handle.instanceId;
      }
      throw new TypeError('Native addon instance id is not resolved');
    });
  }

  function invokeInstanceMethod(handle, methodName, methodArgs) {
    assertNativeHandleLive(handle);
    const modulePath = handle.modulePath;
    const retainedHandles = new Set([handle]);
    const runSync = (instanceId) => adoptNativeCallResult(
        transport.invokeNodeInstanceSync(
            modulePath, nativeInstanceSelector(handle, instanceId), methodName,
            ...methodArgs.map(arg => wireNativeArgumentSync(arg, undefined, retainedHandles))),
        modulePath, retainedHandles);
    if (typeof handle.instanceId === 'number' &&
        typeof transport.invokeNodeInstanceSync === 'function') {
      return runSync(handle.instanceId);
    }
    const pendingArguments = Promise.all(methodArgs.map(arg =>
        wireNativeArgumentAsync(arg, undefined, retainedHandles)));
    return retainNativeHandles(Promise.all([
      resolvedHandleInstanceId(handle), pendingArguments,
    ]).then(([instanceId, wiredArgs]) => transport.invoke(
              '__xenon:node-addon:invoke-instance', {
                modulePath,
                instanceId,
                ...(handle.ownerToken ? {ownerToken: handle.ownerToken} : {}),
                methodName,
                arguments: wiredArgs,
              })).then(result => adoptNativeReturn(result, handle.modulePath)), retainedHandles);
  }

  function createNativeInstanceProxy(handle, prototypeMembers, proto, fields) {
    if (handle.instanceId && typeof handle.instanceId === 'object' &&
        typeof handle.instanceId.then !== 'function') {
      const result = handle.instanceId;
      handle.instanceId = result.instance_id;
      handle.ownerToken = result.owner_token;
    }
    const cached = cachedNativeProxy(handle);
    const previousProxy = cached && cached.proxy.deref();
    if (previousProxy) {
      if (fields && typeof fields === 'object') {
        for (const [key, fieldValue] of Object.entries(fields)) {
          Object.defineProperty(previousProxy, key, {
            configurable: true, enumerable: true, writable: true,
            value: adoptNativeReturn(fieldValue, handle.modulePath),
          });
        }
      }
      return previousProxy;
    }
    handle = cached && cached.handle.deref() || handle;
    const baseInstance = Object.create(proto || null);
    Object.defineProperty(baseInstance, nativeHandleSymbol, {
      configurable: false,
      enumerable: false,
      writable: false,
      value: handle,
    });
    Object.defineProperty(baseInstance, '__instanceId', {
      configurable: true,
      enumerable: false,
      get() {
        return handle.instanceId;
      },
    });

    for (const member of (prototypeMembers || [])) {
      if (member.kind === 'function' || member.kind === 'class') {
        if (proto && member.name in proto) continue;
        Object.defineProperty(baseInstance, member.name, {
          configurable: true,
          enumerable: false,
          writable: false,
          value: function(...methodArgs) {
            return invokeInstanceMethod(handle, member.name, methodArgs);
          },
        });
      }
    }

    if (fields && typeof fields === 'object') {
      for (const [key, fieldValue] of Object.entries(fields)) {
        Object.defineProperty(baseInstance, key, {
          configurable: true,
          enumerable: true,
          writable: true,
          value: adoptNativeReturn(fieldValue, handle.modulePath),
        });
      }
    }

    const boundNativeMethods = new Map();
    const proxy = new Proxy(baseInstance, {
      get(target, prop, receiver) {
        if (typeof prop === 'symbol' || prop in target) {
          const value = Reflect.get(target, prop, receiver);
          if (typeof value !== 'function' || !nativePrototypeMethods.has(value)) return value;
          if (!boundNativeMethods.has(value)) {
            boundNativeMethods.set(value, (...args) => invokeInstanceMethod(handle, prop, args));
          }
          return boundNativeMethods.get(value);
        }
        if (typeof prop !== 'string' || !Number.isInteger(handle.instanceId) ||
            typeof transport.inspectNodeInstanceMemberSync !== 'function') {
          return undefined;
        }
        assertNativeHandleLive(handle);
        const instanceId = handle.instanceId;
        let member = transport.inspectNodeInstanceMemberSync(
            handle.modulePath, nativeInstanceSelector(handle, instanceId), prop);
        if (!member || member.kind === 'undefined') {
          return undefined;
        }
        if (member.kind === 'function') {
          const method = function(...methodArgs) {
            return invokeInstanceMethod(handle, prop, methodArgs);
          };
          Object.defineProperty(target, prop, {
            configurable: true,
            enumerable: false,
            writable: false,
            value: method,
          });
          return method;
        }
        if (member.kind === 'value') {
          return adoptNativeReturn(member.value, handle.modulePath);
        }
        return undefined;
      },
    });
    if (handle.instanceId && typeof handle.instanceId.then === 'function') {
      const pendingId = handle.instanceId;
      handle.instanceId = pendingId.then(result => {
        assignNativeInstanceResult(handle, result);
        rememberNativeProxy(handle, proxy);
        return handle.instanceId;
      });
      const registration = handle.instanceId;
      pendingNativeConstructors.add(registration);
      registration.then(() => pendingNativeConstructors.delete(registration),
          () => pendingNativeConstructors.delete(registration));
      // A constructor with callbacks returns its wrapper immediately. Preserve
      // rejection for a later method call without creating an unhandled chain.
      handle.instanceId.catch(() => {});
    } else {
      assignNativeInstanceResult(handle, handle.instanceId);
      rememberNativeProxy(handle, proxy);
    }
    return proxy;
  }

  function createMojoExportFunction(modulePath, functionName) {
    return function(...args) {
      if (new.target) {
        const Constructor = createMojoClassExport(modulePath, functionName);
        // Native functions may be constructors even when their initial shape
        // has no instance members. Preserve the consumer's prototype before
        // entering native code so inherited JS methods are included as well.
        return Reflect.construct(Constructor, args, new.target);
      }
      // Native calls return synchronously or continue the original Promise.
      // Callback values use the same observer ids without changing the native
      // function's immediate return value into a Promise.
      const retainedHandles = new Set();
      if (typeof transport.invokeNodeExportSync === 'function') {
        const invokeSync = () => adoptNativeCallResult(
            transport.invokeNodeExportSync(
                modulePath, functionName,
                ...args.map(arg => wireNativeArgumentSync(arg, undefined, retainedHandles))),
            modulePath, retainedHandles);
        return invokeSync();
      }
      return retainNativeHandles(Promise.all(args.map(arg =>
          wireNativeArgumentAsync(arg, undefined, retainedHandles)))
          .then(wiredArgs => transport.invoke(
              '__xenon:node-addon:invoke-export', {
                modulePath,
                functionName,
                arguments: wiredArgs,
              }))
          .then(result => adoptNativeReturn(result, modulePath)), retainedHandles);
    };
  }

  function buildExportFromMojoInfo(modulePath, item, parentPath = '') {
    const fullPath = parentPath ? `${parentPath}.${item.name}` : item.name;
    if (item.kind === 'function' || item.kind === 'class') {
      return createMojoClassExport(modulePath, fullPath, item);
    }
    if (item.children && item.children.length > 0) {
      const obj = {};
      for (const child of item.children) {
        obj[child.name] = buildExportFromMojoInfo(modulePath, child, fullPath);
      }
      return obj;
    }
    return valueFromMojo(item.value);
  }

  function createMojoClassExport(modulePath, className, info = {}) {
    const nativeMethodNames = new Set((info.prototype || []).filter(member =>
        member.kind === 'function' || member.kind === 'class').map(member => member.name));
    function ConstructorProxy(...args) {
      if (!new.target) return createMojoExportFunction(modulePath, className)(...args);
      let instanceId;
      const retainedHandles = new Set();
      const prototypeProperties = Object.create(null);
      const seenNames = new Set();
      for (let prototype = new.target.prototype;
           prototype && prototype !== Object.prototype;
           prototype = Object.getPrototypeOf(prototype)) {
        for (const key of Object.getOwnPropertyNames(prototype)) {
          if (seenNames.has(key)) continue;
          seenNames.add(key);
          const property = Object.getOwnPropertyDescriptor(prototype, key);
          // A JS wrapper around an existing native method may call its saved
          // original. Keep the native implementation to avoid callback recursion.
          if (key !== 'constructor' && !nativeMethodNames.has(key) &&
              property && typeof property.value === 'function') {
            prototypeProperties[key] = wireNativeArgumentSync(property.value);
          }
        }
      }
      const hasPrototypeProperties = Object.keys(prototypeProperties).length > 0;
      const constructSync = hasPrototypeProperties ?
          transport.constructNodeExportWithPrototypeSync : transport.constructNodeExportSync;
      if (typeof constructSync === 'function') {
        const wiredArgs = args.map(arg => wireNativeArgumentSync(arg, undefined, retainedHandles));
        instanceId = constructSync.call(transport,
            modulePath, className,
            ...(hasPrototypeProperties ? [prototypeProperties, ...wiredArgs] : wiredArgs));
      } else {
        instanceId = retainNativeHandles(Promise.all(
            args.map(arg => wireNativeArgumentAsync(arg, undefined, retainedHandles)))
            .then(wiredArgs => transport.invoke(
                '__xenon:node-addon:construct-export', {
                  modulePath,
                  exportPath: className,
                  ...(hasPrototypeProperties ? {prototypeProperties} : {}),
                  arguments: wiredArgs,
                })), retainedHandles);
      }
      return createNativeInstanceProxy({
        modulePath,
        className,
        instanceId,
        generation: nativeServiceGeneration,
      }, info.prototype || [], new.target.prototype);
    }

    for (const name of nativeMethodNames) {
      if (name === 'constructor') continue;
      const method = function(...args) {
        return invokeInstanceMethod(getNativeHandle(this), name, args);
      };
      nativePrototypeMethods.add(method);
      Object.defineProperty(ConstructorProxy.prototype, name, {
        configurable: true, enumerable: false, writable: true, value: method,
      });
    }

    return ConstructorProxy;
  }

  function buildExactNativeExport(modulePath, info, exportPath = '') {
    if (!info || info.kind === 'undefined') return undefined;
    if (info.kind === 'null') return null;
    if (info.kind === 'symbol' || info.kind === 'external') {
      const error = new Error(`Native export kind ${info.kind} is not supported`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    }
    if (info.kind === 'bigint') return BigInt(valueFromMojo(info.value));
    const callable = info.kind === 'function' || info.kind === 'class';
    if (!callable && info.kind !== 'object' && info.kind !== 'array') {
      return valueFromMojo(info.value);
    }
    const target = callable ?
        createMojoClassExport(modulePath, exportPath, info) :
        (info.kind === 'array' ? [] : {});
    for (const child of (info.children || [])) {
      const key = child.name;
      const childPath = exportPath ? `${exportPath}.${key}` : key;
      const enumerable = child.enumerable !== false;
      const writable = child.writable !== false;
      const composite = ['object', 'array', 'function', 'class'].includes(child.kind);
      const read = (allowComposite = true) => {
        // The native protocol addresses exports with dot-separated paths.
        // Never silently redirect a literal dotted/empty property to another export.
        if (!key || key.includes('.')) {
          const error = new Error('Native export names containing dots or empty names are not supported');
          error.code = 'ERR_NOT_SUPPORTED';
          throw error;
        }
        const description = transport.inspectNodeExportSync(modulePath, childPath);
        if (!allowComposite && description &&
            ['object', 'array', 'function', 'class'].includes(description.kind)) {
          const error = new Error('Native accessors returning objects or functions require a retained value handle');
          error.code = 'ERR_NOT_SUPPORTED';
          throw error;
        }
        return buildExactNativeExport(modulePath, description, childPath);
      };
      const current = Object.getOwnPropertyDescriptor(target, key);
      if (current && !current.configurable) {
        // A constructor's local prototype must remain the prototype used by
        // its instance wrappers. Other immutable built-ins retain their shape.
        if (key !== 'prototype' && current.writable && !composite && child.kind !== 'property') {
          Object.defineProperty(target, key, {value: buildExactNativeExport(modulePath, child, childPath)});
        }
        continue;
      }
      if (child.kind === 'property') {
        Object.defineProperty(target, key, {
          configurable: true, enumerable, get() { return read(false); },
          set() {
            const error = new Error('Native export accessor assignment is not supported');
            error.code = 'ERR_NOT_SUPPORTED';
            throw error;
          },
        });
      } else if (composite) {
        Object.defineProperty(target, key, {
          configurable: true, enumerable,
          get() {
            const value = read();
            Object.defineProperty(target, key, {configurable: true, enumerable, writable, value});
            return value;
          },
          set: writable ? function(value) {
            Object.defineProperty(target, key, {configurable: true, enumerable, writable, value});
          } : undefined,
        });
      } else {
        Object.defineProperty(target, key, {
          configurable: true, enumerable, writable,
          value: buildExactNativeExport(modulePath, child, childPath),
        });
      }
    }
    return target;
  }

  // node-sqlite3 API backed by real SQLite on a document-owned worker sequence.
  // Only SQLite values cross IPC; SQL, transactions and bindings stay native.
  const sqliteErrorNames = [
    'OK', 'ERROR', 'INTERNAL', 'PERM', 'ABORT', 'BUSY', 'LOCKED', 'NOMEM',
    'READONLY', 'INTERRUPT', 'IOERR', 'CORRUPT', 'NOTFOUND', 'FULL', 'CANTOPEN',
    'PROTOCOL', 'EMPTY', 'SCHEMA', 'TOOBIG', 'CONSTRAINT', 'MISMATCH', 'MISUSE',
    'NOLFS', 'AUTH', 'FORMAT', 'RANGE', 'NOTADB',
  ];
  function sqliteError(error) {
    const result = error instanceof Error ? error : new Error(String(error));
    const match = result.message.match(/\bSQLITE_([A-Z]+):/);
    if (match) {
      result.code = 'SQLITE_' + match[1];
      result.errno = sqliteErrorNames.indexOf(match[1]);
    }
    return result;
  }
  function sqliteEncode(value) {
    if (value === undefined) return {__xenon_sqlite_undefined__: true};
    if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) {
      return {__xenon_sqlite_blob__: Buffer.from(value).toString('base64')};
    }
    if (value instanceof Date) return value.getTime();
    if (value instanceof RegExp) return String(value);
    if (value == null) return null;
    if (['string', 'number', 'boolean'].includes(typeof value)) return value;
    throw new TypeError('Unsupported SQLite parameter type');
  }
  function sqliteArguments(args) {
    const values = args.slice();
    const callback = typeof values[values.length - 1] === 'function' ?
        values.pop() : undefined;
    let params;
    if (values.length) {
      const first = values[0];
      if (Array.isArray(first)) {
        params = first.map(sqliteEncode);
      } else if (first && typeof first === 'object' &&
                 !ArrayBuffer.isView(first) && !(first instanceof ArrayBuffer) &&
                 !(first instanceof Date) && !(first instanceof RegExp)) {
        params = Object.fromEntries(
            Object.entries(first).map(([key, value]) => [key, sqliteEncode(value)]));
      } else {
        params = values.map(sqliteEncode);
      }
    }
    return {callback, params};
  }
  function sqliteDecodeRow(row) {
    if (row == null) return undefined;
    return Object.fromEntries(Object.entries(row).map(([key, value]) => [
      key, value && typeof value === 'object' &&
          typeof value.__xenon_sqlite_blob__ === 'string' ?
          Buffer.from(value.__xenon_sqlite_blob__, 'base64') : value,
    ]));
  }
  const sqlitePreparationFailed = Symbol('sqlitePreparationFailed');

  class SqliteStatement extends EventEmitter {
    constructor(db, sql, cb) {
      super();
      this.db = db;
      this.sql = String(sql);
      this._id = 0;
      this._prepareError = null;
      db._schedule(this, cb, async () => {
        try {
          this._id = await db._call('prepare', {sql: this.sql});
        } catch (error) {
          this._prepareError = error;
          throw error;
        }
      });
    }
    _operation(operation, args, complete) {
      const {callback, params} = sqliteArguments(args);
      this.db._schedule(this, callback, () => {
        // node-sqlite3 reports preparation failure once, then discards the
        // statement's queued operations without calling their callbacks.
        if (this._prepareError) return sqlitePreparationFailed;
        return this.db._call(operation, {statementId: this._id, params});
      }, complete);
      return this;
    }
    bind(...args) { return this._operation('bind', args); }
    reset(cb) { return this._operation('reset', typeof cb === 'function' ? [cb] : []); }
    finalize(cb) { return this._operation('finalize', typeof cb === 'function' ? [cb] : []); }
    run(...args) {
      return this._operation('run', args, (info, cb) => {
        this.lastID = info.lastID;
        this.changes = info.changes;
        if (cb) cb.call(this, null);
      });
    }
    get(...args) {
      return this._operation('get', args, (row, cb) => {
        if (cb) cb.call(this, null, sqliteDecodeRow(row));
      });
    }
    all(...args) {
      return this._operation('all', args, (rows, cb) => {
        if (cb) cb.call(this, null, rows.map(sqliteDecodeRow));
      });
    }
    each(...args) {
      const complete = args.length > 1 &&
          typeof args[args.length - 1] === 'function' &&
          typeof args[args.length - 2] === 'function' ? args.pop() : undefined;
      const callback = typeof args[args.length - 1] === 'function' ?
          args.pop() : undefined;
      return this.all(...args, function(error, rows) {
        if (error) {
          if (callback) callback.call(this, error);
          else this.emit('error', error);
          return;
        }
        for (const row of rows) {
          if (callback) callback.call(this, null, row);
        }
        if (complete) complete.call(this, null, rows.length);
      });
    }
  }

  class SqliteDatabase extends EventEmitter {
    constructor(filename, modeOrCb, cb) {
      super();
      this.filename = String(filename);
      this.open = false;
      this._id = 0;
      this._queue = Promise.resolve();
      this._openError = null;
      const callback = typeof modeOrCb === 'function' ? modeOrCb : cb;
      const mode = typeof modeOrCb === 'number' ? modeOrCb : 0x10006;
      this._schedule(this, callback, async () => {
        try {
          this._id = await transport.invoke('__xenon:sqlite', {
            operation: 'open',
            path: this.filename === ':memory:' || !this.filename ?
                this.filename : normalizeFsPath(this.filename),
            mode,
          });
          this.open = true;
          this.emit('open');
        } catch (error) {
          this._openError = error;
          throw error;
        }
      });
    }
    _call(operation, options = {}) {
      if (this._openError) throw this._openError;
      return transport.invoke('__xenon:sqlite', {
        operation, databaseId: this._id, ...options,
      });
    }
    _schedule(owner, cb, operation, complete) {
      const job = this._queue.then(operation);
      this._queue = job.then(value => {
        if (value === sqlitePreparationFailed) return;
        if (complete) complete(value, cb);
        else if (cb) cb.call(owner, null);
      }, error => {
        const failure = sqliteError(error);
        if (cb) cb.call(owner, failure);
        else owner.emit('error', failure);
      }).catch(error => {
        // A caller's callback exception must not stall later database jobs.
        queueMicrotask(() => { throw error; });
      });
      return this;
    }
    _statement(sql, args) {
      const cb = args[args.length - 1];
      return new SqliteStatement(this, sql, typeof cb === 'function' ?
          function(error) { if (error) cb.call(this, error); } : undefined);
    }
    prepare(sql, ...args) {
      const stmt = this._statement(sql, args);
      return args.length ? stmt.bind(...args) : stmt;
    }
    run(sql, ...args) {
      this._statement(sql, args).run(...args).finalize();
      return this;
    }
    get(sql, ...args) {
      this._statement(sql, args).get(...args).finalize();
      return this;
    }
    all(sql, ...args) {
      this._statement(sql, args).all(...args).finalize();
      return this;
    }
    each(sql, ...args) {
      this._statement(sql, args).each(...args).finalize();
      return this;
    }
    exec(sql, cb) {
      return this._schedule(this, cb, () => this._call('exec', {sql: String(sql)}));
    }
    close(cb) {
      return this._schedule(this, cb, async () => {
        await this._call('close');
        this.open = false;
        this.emit('close');
      });
    }
    wait(cb) { return this._schedule(this, cb, () => {}); }
    serialize(cb) { if (cb) cb(); return this; }
    parallelize(cb) { if (cb) cb(); return this; }
    configure(option, value) {
      if (option !== 'busyTimeout' || !Number.isInteger(value) || value < 0) {
        throw new TypeError('Unsupported SQLite configuration');
      }
      return this._schedule(this, undefined,
          () => this._call('configure', {busyTimeout: value}));
    }
  }

  // sqlite3's JS wrapper expects this export even when backup is unused.
  // Unsupported operations report errors, never synthetic successful writes.
  class SqliteBackup extends EventEmitter {
    constructor(...args) {
      super();
      this._fail(args[args.length - 1]);
    }
    _fail(cb) {
      queueMicrotask(() => {
        const error = sqliteError(new Error('SQLITE_MISUSE: Backup is not supported'));
        if (typeof cb === 'function') cb.call(this, error);
        else this.emit('error', error);
      });
      return this;
    }
    step(_pages, cb) { return this._fail(cb); }
    finish(cb) { return this._fail(cb); }
  }

  const sqlite3Module = {
    Database: SqliteDatabase,
    Statement: SqliteStatement,
    Backup: SqliteBackup,
    ...Object.fromEntries(sqliteErrorNames.map((name, index) => [name, index])),
    OPEN_READONLY: 1,
    OPEN_READWRITE: 2,
    OPEN_CREATE: 4,
    OPEN_FULLMUTEX: 0x00010000,
    OPEN_URI: 0x00000040,
    OPEN_SHAREDCACHE: 0x00020000,
    OPEN_PRIVATECACHE: 0x00040000,
    verbose: () => sqlite3Module,
  };

  function loadNativeNodeModule(normalizedPath) {
    const exportsList = transport.requireNodeModuleSync(normalizedPath);
    // Current hosts return the exact root in the load reply. Avoid building,
    // copying and discarding a legacy export tree plus a second sync IPC.
    if (exportsList && !Array.isArray(exportsList) &&
        typeof exportsList.kind === 'string') {
      return buildExactNativeExport(normalizedPath, exportsList);
    }
    if (typeof transport.inspectNodeExportSync === 'function') {
      return buildExactNativeExport(normalizedPath,
          transport.inspectNodeExportSync(normalizedPath, ''));
    }
    // Older transports expose only a shallow tree. Current hosts provide the
    // root descriptor above, including its actual type and lazy nested shape.
    if (!Array.isArray(exportsList)) {
      const error = new Error('Native module load returned an invalid export descriptor');
      error.code = 'ERR_INVALID_NATIVE_EXPORTS';
      throw error;
    }
    const target = {};
    for (const item of exportsList) {
      Object.defineProperty(target, item.name, {
        value: buildExportFromMojoInfo(normalizedPath, item),
        enumerable: true, configurable: true, writable: true,
      });
    }
    return target;
  }

  // Node `net` named-pipe / socket pairing. Same-isolate listen+connect is
  // paired locally; cross-process (Utility ipcMain ↔ renderer) is tunneled
  // over reserved Electron IPC channels `__xenon:net:*`.
  function normalizeNetPath(value) {
    let path = String(value || '');
    path = path.replace(/\//g, '\\');
    if (process.platform === 'win32') {
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

  function sendXenonNet(channel, payload) {
    try {
      transport.send(channel, payload);
    } catch (error) {
      console.warn('[xenon-net] send failed', channel, error);
    }
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
    socket._id = 'r-' + (nextNetSocketId++);
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
      sendXenonNet('__xenon:net:close', {toId: socket._peerId, fromId: socket._id});
    }
    emitNetEvent(socket, 'end');
    emitNetEvent(socket, 'close');
  }

  function deliverNetBytes(socket, data) {
    const buf = Buffer.isBuffer(data) || data instanceof Uint8Array ?
        Buffer.from(data) : Buffer.from(String(data));
    emitNetEvent(socket, 'data', buf);
  }

  const netModule = {
    Socket: class extends EventEmitter {
      constructor() {
        super();
        this._connected = false;
        this._closed = false;
        this._peer = null;
        this._peerId = null;
        this._id = null;
        this._connectCb = null;
        this._pendingWrites = [];
        this.connecting = false;
        this.destroyed = false;
        this.remoteAddress = '';
        this.localAddress = '';
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
        if (server) {
          const incoming = new netModule.Socket();
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
            // A server or client connection listener can synchronously destroy
            // either endpoint. Do not continue the queued handshake afterward.
            if (this._closed || incoming._closed) return;
            emitNetEvent(this, 'connect');
            if (this._closed || incoming._closed) return;
            const callback = this._connectCb;
            this._connectCb = null;
            if (callback) callNetCallback(this, callback);
          });
          return this;
        }
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
          });
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
        console.info('[xenon-net] listen', this._path);
        if (isNamedPipePath(this._path)) {
          this._nativeId = 'server-r-' + (nextNetServerId++);
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
          this._closeCb = typeof cb === 'function' ? cb : null;
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
      const server = new netModule.Server();
      if (typeof args[0] === 'function') {
        server.on('connection', args[0]);
      }
      return server;
    },
    connect: (...args) => {
      const socket = new netModule.Socket();
      socket.connect(...args);
      return socket;
    },
    createConnection: (...args) => netModule.connect(...args),
    isIP: (input) => (input && input.includes(':') ? 6 : (input && input.includes('.') ? 4 : 0)),
    isIPv4: (input) => /^(?:[0-9]{1,3}\.){3}[0-9]{1,3}$/.test(input),
    isIPv6: (input) => !!(input && input.includes(':')),
  };
  netModule.default = netModule;

  dispatchXenonNet = (channel, payload) => {
    const msg = payload || {};
    if (channel === '__xenon:net:listening') {
      const server = nativeNetServers.get(msg.serverId);
      if (server) {
        emitNetEvent(server, 'listening');
        if (server._listenCb) {
          callNetCallback(server, server._listenCb);
          server._listenCb = null;
        }
      }
      return true;
    }
    if (channel === '__xenon:net:connection') {
      const server = nativeNetServers.get(msg.serverId);
      if (!server || !msg.socketId) {
        return true;
      }
      const incoming = new netModule.Socket();
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
        if (server._closeCb) {
          callNetCallback(server, server._closeCb);
          server._closeCb = null;
        }
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
        });
        return true;
      }
      const incoming = new netModule.Socket();
      incoming._asyncContext = server._asyncContext;
      incoming._connected = true;
      incoming._peerId = msg.fromId;
      allocNetSocket(incoming);
      // Match net.Server ordering: install connection/data listeners before
      // the peer observes its connect event.
      emitNetEvent(server, 'connection', incoming);
      sendXenonNet('__xenon:net:connected', {
        toId: msg.fromId,
        peerId: incoming._id,
      });
      return true;
    }
    if (channel === '__xenon:net:connected') {
      const socket = netSockets.get(msg.toId);
      if (!socket) {
        return true;
      }
      socket._peerId = msg.peerId;
      socket._connected = true;
      socket.connecting = false;
      emitNetEvent(socket, 'connect');
      if (socket._connectCb) {
        callNetCallback(socket, socket._connectCb);
        socket._connectCb = null;
      }
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
          const err = new Error(msg.code || 'net error');
          err.code = msg.code;
          emitNetEvent(server, 'error', err);
        }
        return true;
      }
      const socket = netSockets.get(msg.toId);
      if (socket) {
        const err = new Error(msg.code || 'net error');
        err.code = msg.code;
        socket.connecting = false;
        socket._pendingWrites.length = 0;
        emitNetEvent(socket, 'error', err);
      }
      return true;
    }
    return false;
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
  Stream.default = Stream;
  const streamModule = Stream;

  function rotl32(v, n) {
    return (v << n) | (v >>> (32 - n));
  }

  function md5Bytes(bytes) {
    const originalLen = bytes.length;
    const bitLen = originalLen * 8;
    const paddedLen = (((originalLen + 8) >> 6) + 1) << 6;
    const buf = new Uint8Array(paddedLen);
    buf.set(bytes);
    buf[originalLen] = 0x80;
    const view = new DataView(buf.buffer);
    view.setUint32(paddedLen - 8, bitLen, true);
    view.setUint32(paddedLen - 4, Math.floor(bitLen / 0x100000000), true);

    let a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    const ff = (x, y, z) => (x & y) | (~x & z);
    const gg = (x, y, z) => (x & z) | (y & ~z);
    const hh = (x, y, z) => x ^ y ^ z;
    const ii = (x, y, z) => y ^ (x | ~z);
    const cmn = (q, a0, b0, x, s, t) =>
        (rotl32((a0 + q + x + t) | 0, s) + b0) | 0;

    for (let i = 0; i < paddedLen; i += 64) {
      const x = new Int32Array(16);
      for (let j = 0; j < 16; j++) x[j] = view.getInt32(i + j * 4, true);
      let aa = a, bb = b, cc = c, dd = d;
      const r = (f, k, s, t) => {
        const fval = f(b, c, d);
        const tmp = d;
        d = c;
        c = b;
        b = cmn(fval, a, b, x[k], s, t);
        a = tmp;
      };
      r(ff, 0, 7, 0xd76aa478); r(ff, 1, 12, 0xe8c7b756);
      r(ff, 2, 17, 0x242070db); r(ff, 3, 22, 0xc1bdceee);
      r(ff, 4, 7, 0xf57c0faf); r(ff, 5, 12, 0x4787c62a);
      r(ff, 6, 17, 0xa8304613); r(ff, 7, 22, 0xfd469501);
      r(ff, 8, 7, 0x698098d8); r(ff, 9, 12, 0x8b44f7af);
      r(ff, 10, 17, 0xffff5bb1); r(ff, 11, 22, 0x895cd7be);
      r(ff, 12, 7, 0x6b901122); r(ff, 13, 12, 0xfd987193);
      r(ff, 14, 17, 0xa679438e); r(ff, 15, 22, 0x49b40821);
      r(gg, 1, 5, 0xf61e2562); r(gg, 6, 9, 0xc040b340);
      r(gg, 11, 14, 0x265e5a51); r(gg, 0, 20, 0xe9b6c7aa);
      r(gg, 5, 5, 0xd62f105d); r(gg, 10, 9, 0x02441453);
      r(gg, 15, 14, 0xd8a1e681); r(gg, 4, 20, 0xe7d3fbc8);
      r(gg, 9, 5, 0x21e1cde6); r(gg, 14, 9, 0xc33707d6);
      r(gg, 3, 14, 0xf4d50d87); r(gg, 8, 20, 0x455a14ed);
      r(gg, 13, 5, 0xa9e3e905); r(gg, 2, 9, 0xfcefa3f8);
      r(gg, 7, 14, 0x676f02d9); r(gg, 12, 20, 0x8d2a4c8a);
      r(hh, 5, 4, 0xfffa3942); r(hh, 8, 11, 0x8771f681);
      r(hh, 11, 16, 0x6d9d6122); r(hh, 14, 23, 0xfde5380c);
      r(hh, 1, 4, 0xa4beea44); r(hh, 4, 11, 0x4bdecfa9);
      r(hh, 7, 16, 0xf6bb4b60); r(hh, 10, 23, 0xbebfbc70);
      r(hh, 13, 4, 0x289b7ec6); r(hh, 0, 11, 0xeaa127fa);
      r(hh, 3, 16, 0xd4ef3085); r(hh, 6, 23, 0x04881d05);
      r(hh, 9, 4, 0xd9d4d039); r(hh, 12, 11, 0xe6db99e5);
      r(hh, 15, 16, 0x1fa27cf8); r(hh, 2, 23, 0xc4ac5665);
      r(ii, 0, 6, 0xf4292244); r(ii, 7, 10, 0x432aff97);
      r(ii, 14, 15, 0xab9423a7); r(ii, 5, 21, 0xfc93a039);
      r(ii, 12, 6, 0x655b59c3); r(ii, 3, 10, 0x8f0ccc92);
      r(ii, 10, 15, 0xffeff47d); r(ii, 1, 21, 0x85845dd1);
      r(ii, 8, 6, 0x6fa87e4f); r(ii, 15, 10, 0xfe2ce6e0);
      r(ii, 6, 15, 0xa3014314); r(ii, 13, 21, 0x4e0811a1);
      r(ii, 4, 6, 0xf7537e82); r(ii, 11, 10, 0xbd3af235);
      r(ii, 2, 15, 0x2ad7d2bb); r(ii, 9, 21, 0xeb86d391);
      a = (a + aa) | 0; b = (b + bb) | 0; c = (c + cc) | 0; d = (d + dd) | 0;
    }

    const out = new Uint8Array(16);
    const outView = new DataView(out.buffer);
    outView.setInt32(0, a, true);
    outView.setInt32(4, b, true);
    outView.setInt32(8, c, true);
    outView.setInt32(12, d, true);
    return out;
  }

  function sha1Bytes(bytes) {
    const originalLen = bytes.length;
    const bitLenHi = Math.floor(originalLen / 0x20000000);
    const bitLenLo = (originalLen << 3) >>> 0;
    const paddedLen = (((originalLen + 8) >> 6) + 1) << 6;
    const buf = new Uint8Array(paddedLen);
    buf.set(bytes);
    buf[originalLen] = 0x80;
    const view = new DataView(buf.buffer);
    view.setUint32(paddedLen - 8, bitLenHi);
    view.setUint32(paddedLen - 4, bitLenLo);

    let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe;
    let h3 = 0x10325476, h4 = 0xc3d2e1f0;
    const w = new Uint32Array(80);
    for (let i = 0; i < paddedLen; i += 64) {
      for (let j = 0; j < 16; j++) w[j] = view.getUint32(i + j * 4);
      for (let j = 16; j < 80; j++) {
        w[j] = rotl32(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1) >>> 0;
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4;
      for (let j = 0; j < 80; j++) {
        let f, k;
        if (j < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (j < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        const temp = (rotl32(a, 5) + f + e + k + w[j]) >>> 0;
        e = d; d = c; c = rotl32(b, 30) >>> 0; b = a; a = temp;
      }
      h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0;
      h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0;
    }
    const out = new Uint8Array(20);
    const outView = new DataView(out.buffer);
    outView.setUint32(0, h0); outView.setUint32(4, h1); outView.setUint32(8, h2);
    outView.setUint32(12, h3); outView.setUint32(16, h4);
    return out;
  }

  const kSha256K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];

  function sha256Bytes(bytes) {
    const originalLen = bytes.length;
    const bitLenHi = Math.floor(originalLen / 0x20000000);
    const bitLenLo = (originalLen << 3) >>> 0;
    const paddedLen = (((originalLen + 8) >> 6) + 1) << 6;
    const buf = new Uint8Array(paddedLen);
    buf.set(bytes);
    buf[originalLen] = 0x80;
    const view = new DataView(buf.buffer);
    view.setUint32(paddedLen - 8, bitLenHi);
    view.setUint32(paddedLen - 4, bitLenLo);

    let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    const w = new Uint32Array(64);
    const rotr = (x, n) => (x >>> n) | (x << (32 - n));
    for (let i = 0; i < paddedLen; i += 64) {
      for (let j = 0; j < 16; j++) w[j] = view.getUint32(i + j * 4);
      for (let j = 16; j < 64; j++) {
        const s0 = rotr(w[j - 15], 7) ^ rotr(w[j - 15], 18) ^ (w[j - 15] >>> 3);
        const s1 = rotr(w[j - 2], 17) ^ rotr(w[j - 2], 19) ^ (w[j - 2] >>> 10);
        w[j] = (w[j - 16] + s0 + w[j - 7] + s1) >>> 0;
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, hh = h7;
      for (let j = 0; j < 64; j++) {
        const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const ch = (e & f) ^ (~e & g);
        const temp1 = (hh + S1 + ch + kSha256K[j] + w[j]) >>> 0;
        const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const temp2 = (S0 + maj) >>> 0;
        hh = g; g = f; f = e; e = (d + temp1) >>> 0;
        d = c; c = b; b = a; a = (temp1 + temp2) >>> 0;
      }
      h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0;
      h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0;
      h6 = (h6 + g) >>> 0; h7 = (h7 + hh) >>> 0;
    }
    const out = new Uint8Array(32);
    const outView = new DataView(out.buffer);
    outView.setUint32(0, h0); outView.setUint32(4, h1);
    outView.setUint32(8, h2); outView.setUint32(12, h3);
    outView.setUint32(16, h4); outView.setUint32(20, h5);
    outView.setUint32(24, h6); outView.setUint32(28, h7);
    return out;
  }

  function hashBytes(algorithm, bytes) {
    const algo = String(algorithm || '').toLowerCase().replace(/-/g, '');
    if (algo === 'md5') return md5Bytes(bytes);
    if (algo === 'sha1') return sha1Bytes(bytes);
    if (algo === 'sha256') return sha256Bytes(bytes);
    throw new Error('Unsupported digest algorithm: ' + algorithm);
  }

  function hashBlockSize(algorithm) {
    return 64;
  }

  function snapshotDigestInput(data, encoding) {
    if (typeof data === 'string') return Buffer.from(data, encoding || 'utf8');
    if (ArrayBuffer.isView(data)) {
      return Buffer.from(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
    }
    // Buffer.from(ArrayBuffer) shares its storage; digest inputs must be
    // captured when update/createHmac is called, before user mutation.
    if (Object.prototype.toString.call(data) === '[object ArrayBuffer]' ||
        Object.prototype.toString.call(data) === '[object SharedArrayBuffer]') {
      return Buffer.from(new Uint8Array(data));
    }
    return Buffer.from(data);
  }

  function createHashObject(algorithm, initialChunks) {
    const chunks = initialChunks ? initialChunks.slice() : [];
    return {
      update(data, encoding) {
        chunks.push(snapshotDigestInput(data, encoding));
        return this;
      },
      digest(encoding) {
        const digestBytes = hashBytes(algorithm, Buffer.concat(chunks));
        const buf = Buffer.from(digestBytes);
        return encoding ? buf.toString(encoding) : buf;
      },
    };
  }

  function hmacBytes(algorithm, key, data) {
    const blockSize = hashBlockSize(algorithm);
    let keyBytes = Buffer.from(key);
    if (keyBytes.length > blockSize) {
      keyBytes = Buffer.from(hashBytes(algorithm, keyBytes));
    }
    const ikey = Buffer.alloc(blockSize);
    const okey = Buffer.alloc(blockSize);
    ikey.set(keyBytes);
    okey.set(keyBytes);
    for (let i = 0; i < blockSize; i++) {
      ikey[i] ^= 0x36;
      okey[i] ^= 0x5c;
    }
    const inner = hashBytes(algorithm, Buffer.concat([ikey, data]));
    return hashBytes(algorithm, Buffer.concat([okey, Buffer.from(inner)]));
  }

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
        const encoded = transport.sendSync('__xenon:crypto', {
          operation: 'cipher',
          algorithm: normalizedAlgorithm,
          encrypt: Boolean(encrypt),
          keyBase64: keyBuffer.toString('base64'),
          ivBase64: ivBuffer.toString('base64'),
          dataBase64: Buffer.concat(chunks).toString('base64'),
          autoPadding,
        });
        const result = Buffer.from(encoded, 'base64');
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

  function randomArgumentError(name, value, range) {
    const error = typeof value === 'number' ?
        new RangeError(`${name} is outside ${range}`) :
        new TypeError(`${name} must be a number`);
    error.code = typeof value === 'number' ? 'ERR_OUT_OF_RANGE' : 'ERR_INVALID_ARG_TYPE';
    return error;
  }
  function randomByteCount(value, name, elementSize, maximum) {
    const bytes = typeof value === 'number' ? value * elementSize : NaN;
    if (typeof value !== 'number' || !Number.isFinite(bytes) || bytes < 0 ||
        bytes > Math.min(maximum, 0x7fffffff)) {
      throw randomArgumentError(name, value, `0..${Math.min(maximum, 0x7fffffff)}`);
    }
    return Math.floor(bytes);
  }
  function randomTargetBytes(buf, offset = 0, size) {
    const view = ArrayBuffer.isView(buf);
    const buffer = view ? buf.buffer : buf;
    if (!view && !(buffer instanceof ArrayBuffer) &&
        !(typeof SharedArrayBuffer === 'function' && buffer instanceof SharedArrayBuffer)) {
      const error = new TypeError('buf must be an ArrayBuffer or an ArrayBuffer view');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
    const elementSize = buf.BYTES_PER_ELEMENT || 1;
    const byteOffset = randomByteCount(offset, 'offset', elementSize, buf.byteLength);
    const byteLength = size === undefined ? buf.byteLength - byteOffset :
        randomByteCount(size, 'size', elementSize, buf.byteLength - byteOffset);
    return new Uint8Array(buffer, (view ? buf.byteOffset : 0) + byteOffset, byteLength);
  }
  function fillSecureRandom(bytes) {
    const source = globalThis.crypto;
    if (!source || typeof source.getRandomValues !== 'function') {
      const error = new Error('A secure random source is not available');
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    }
    // WebCrypto limits each call to 65536 bytes, independent of view type.
    for (let offset = 0; offset < bytes.length; offset += 65536) {
      source.getRandomValues(bytes.subarray(offset, offset + 65536));
    }
  }
  function randomCompleteAsync(bytes, buf, callback) {
    queueMicrotask(() => {
      try {
        fillSecureRandom(bytes);
      } catch (error) {
        callback(error);
        return;
      }
      callback(null, buf);
    });
  }

  const cryptoModule = {
    randomBytes(size, callback) {
      if (callback !== undefined) fsValidateCallback(callback);
      const length = randomByteCount(size, 'size', 1, 0x7fffffff);
      const buf = Buffer.alloc(length);
      if (callback !== undefined) {
        randomCompleteAsync(buf, buf, callback);
        return;
      }
      fillSecureRandom(buf);
      return buf;
    },
    randomFillSync(buf, offset = 0, size) {
      fillSecureRandom(randomTargetBytes(buf, offset, size));
      return buf;
    },
    randomFill(buf, offset, size, callback) {
      if (typeof offset === 'function') {
        callback = offset; offset = 0; size = undefined;
      } else if (typeof size === 'function') {
        callback = size; size = undefined;
      }
      fsValidateCallback(callback);
      randomCompleteAsync(randomTargetBytes(buf, offset, size), buf, callback);
    },
    createCipheriv: (algorithm, key, iv) =>
      createCipherObject(algorithm, key, iv, true),
    createDecipheriv: (algorithm, key, iv) =>
      createCipherObject(algorithm, key, iv, false),
    createHash: (algorithm) => createHashObject(algorithm),
    createHmac: (algorithm, key) => {
      const keySnapshot = snapshotDigestInput(key);
      const chunks = [];
      return {
        update(data, encoding) {
          chunks.push(snapshotDigestInput(data, encoding));
          return this;
        },
        digest(encoding) {
          const digestBytes = hmacBytes(
              algorithm, keySnapshot, Buffer.concat(chunks));
          const buf = Buffer.from(digestBytes);
          return encoding ? buf.toString(encoding) : buf;
        },
      };
    },
  };
  cryptoModule.default = cryptoModule;

  const urlModule = {
    parse: (urlString) => {
      try {
        return new URL(urlString, 'http://localhost');
      } catch {
        return {};
      }
    },
    format: (urlObj) => urlObj.toString(),
    resolve: (from, to) => {
      try {
        return new URL(to, from).toString();
      } catch {
        return to;
      }
    },
    fileURLToPath: (url) => {
      if (typeof url === 'string') {
        return decodeURIComponent(url.replace(/^file:\/\/\/?/, ''));
      }
      return decodeURIComponent(url.pathname.replace(/^\/([a-zA-Z]:)/, '$1'));
    },
    pathToFileURL: (path) => new URL('file:///' + path.replace(/\\/g, '/')),
    URL: globalThis.URL,
    URLSearchParams: globalThis.URLSearchParams,
  };
  urlModule.default = urlModule;

  // --- 9. Standard globalThis.require ---
  const hostedCjsCache = Object.create(null);
  const moduleResolutionCache = new Map();
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
  let canonicalModuleRoots;
  const builtinModuleCache = new Map();
  const builtinModuleNames = new Set([
    'assert', 'async_hooks', 'buffer', 'child_process', 'constants', 'crypto',
    'dns', 'events', 'fs', 'fs/promises', 'http', 'http2', 'https', 'net', 'os',
    'path', 'path/posix', 'path/win32', 'process', 'querystring', 'readline', 'stream',
    'string_decoder', 'timers', 'tls', 'tty', 'url', 'util', 'zlib',
  ]);
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
  let hostedCjsLoadingFile = '';

  function moduleError(code, message) {
    return Object.assign(new Error(message), {code});
  }

  function builtinModuleId(request) {
    if (typeof request !== 'string') {
      throw Object.assign(new TypeError('Module name must be a string'),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (!request || request.includes('\0')) {
      throw Object.assign(new TypeError('Module name must not be empty or contain null bytes'),
                          {code: 'ERR_INVALID_ARG_VALUE'});
    }
    if (request.startsWith('node:')) {
      const name = request.slice(5);
      if (!builtinModuleNames.has(name)) {
        throw moduleError('ERR_UNKNOWN_BUILTIN_MODULE',
                          `No such built-in module: ${request}`);
      }
      return name;
    }
    return builtinModuleNames.has(request) || request === 'electron' ||
        request === 'xenon:sqlite3' ? request : '';
  }

  function moduleFilenameFromSource(filename) {
    if (typeof filename !== 'string' || !filename) return filename;
    let sourceUrl;
    try { sourceUrl = new URL(filename); } catch (_error) { return filename; }
    const configuredMappings = injectedPaths.rendererUrlMappings;
    const mappings = Array.isArray(configuredMappings) ? configuredMappings : [];
    const candidates = mappings.map(mapping => {
      try {
        return {root: mapping.sourcePathPrefix, url: new URL(mapping.targetBaseUrl)};
      } catch (_error) { return null; }
    }).filter(mapping => mapping && typeof mapping.root === 'string' &&
        pathModule.isAbsolute(mapping.root));
    // Old hosts only expose the current document's path. This fallback is for
    // that one origin, and is disabled whenever the host supplies mappings.
    if (!mappings.length && injectedPaths.documentPath && globalThis.location) {
      try {
        const location = globalThis.location;
        const documentUrl = new URL(location.href ||
            `${location.protocol}//${location.hostname}${location.pathname || '/index.html'}`);
        const documentName = decodeURIComponent(documentUrl.pathname);
        const documentParts = documentName.split('/').filter(Boolean);
        let root = pathModule.normalize(injectedPaths.documentPath);
        // Preserve the corresponding relative filename instead of assigning a
        // script in another origin to this application's module directory.
        for (const part of documentParts.slice().reverse()) {
          if (pathModule.basename(root).toLowerCase() !== part.toLowerCase()) {
            root = '';
            break;
          }
          root = pathModule.dirname(root);
        }
        if (root) candidates.push({root, url: new URL('/', documentUrl)});
      } catch (_error) {}
    }
    candidates.sort((a, b) => b.url.pathname.length - a.url.pathname.length);
    for (const mapping of candidates) {
      const target = mapping.url;
      const prefix = target.pathname.endsWith('/') ? target.pathname : target.pathname + '/';
      if (sourceUrl.protocol !== target.protocol || sourceUrl.host !== target.host ||
          sourceUrl.username || sourceUrl.password || !sourceUrl.pathname.startsWith(prefix)) continue;
      const suffix = sourceUrl.pathname.slice(prefix.length);
      // Encoded separators must not turn a URL component into a filesystem
      // traversal or a different drive. Canonical root checks still run at load.
      if (/%(?:2f|5c)/i.test(suffix)) return filename;
      let relative;
      try { relative = decodeURIComponent(suffix); } catch (_error) { return filename; }
      if (relative.includes('\0') || relative.includes('\\') ||
          relative.split('/').some(part => part === '..' || part.includes(':'))) return filename;
      const root = pathModule.normalize(mapping.root);
      const resolved = pathModule.resolve(root, relative || '.');
      const lower = resolved.toLowerCase();
      if (lower === root.toLowerCase() || lower.startsWith(root.replace(/[\\/]+$/, '').toLowerCase() + '\\')) {
        return resolved;
      }
    }
    return filename;
  }

  function installBindingsFileNameShim() {
    if (globalThis.__xenonBindingsFileNameShim) {
      return;
    }
    globalThis.__xenonBindingsFileNameShim = true;
    let innerPST = Error.prepareStackTrace;
    const wrappedFormatters = new WeakSet();
    Object.defineProperty(Error, 'prepareStackTrace', {
      configurable: true,
      enumerable: false,
      get() {
        return innerPST;
      },
      set(fn) {
        // bindings temporarily installs a formatter and restores the previous
        // value (usually undefined). Preserve V8's default string stack then,
        // and don't wrap an already saved formatter again on restoration.
        if (typeof fn !== 'function' || wrappedFormatters.has(fn)) {
          innerPST = fn;
          return;
        }
        innerPST = function(err, stack) {
          const fallback = hostedCjsLoadingFile;
          const mapped = (stack || []).map(site => new Proxy(site, {
            get(target, property) {
              if (property === 'getFileName' || property === 'getScriptNameOrSourceURL') {
                return () => {
                  try { return moduleFilenameFromSource(target[property]()) || fallback; }
                  catch (_error) { return fallback; }
                };
              }
              // V8 CallSite methods require the original internal receiver.
              const value = Reflect.get(target, property, target);
              return typeof value === 'function' ? value.bind(target) : value;
            },
          }));
          return fn(err, mapped);
        };
        wrappedFormatters.add(innerPST);
      },
    });
  }

  // Browser-loaded bundles use bindings before any hosted CommonJS module is
  // required, so install the filename mapping for the document itself.
  installBindingsFileNameShim();

  function resolveHostedCjs(request, parentFile = globalThis.__filename) {
    parentFile = moduleFilenameFromSource(parentFile);
    const builtin = builtinModuleId(request);
    if (builtin) return request.startsWith('node:') ? request : builtin;
    if (request.startsWith('#')) {
      throw moduleError('ERR_NOT_SUPPORTED', 'Package imports are not supported');
    }
    const cacheKey = `${parentFile}\0${request}`;
    const cachedPath = moduleResolutionCache.get(cacheKey);
    if (cachedPath) {
      // A deleted or failed module must be resolved again, including realpath
      // and the permitted-root check. Successful cached modules need no IPC.
      if (hostedCjsCache[cachedPath]) return cachedPath;
      moduleResolutionCache.delete(cacheKey);
    }
    const canonicalPath = filename => {
      const normalized = pathModule.normalize(filename);
      // ASAR members retain their virtual identity in the Browser bridge;
      // they are not standalone paths that the OS can normalize.
      return fsUsesVirtualMount(normalized) ? normalized :
          pathModule.normalize(fsNativeSync('realpath', normalized));
    };
    if (!canonicalModuleRoots) {
      canonicalModuleRoots = [];
      for (const root of new Set([appPath, exeDir].filter(Boolean))) {
        const configured = pathModule.normalize(root);
        try {
          canonicalModuleRoots.push({configured, canonical: canonicalPath(configured)});
        } catch (error) {
          if (error.code !== 'ENOENT' && error.code !== 'ENOTDIR') throw error;
        }
      }
    }
    const within = (filename, roots) => {
      const lower = pathModule.normalize(filename).toLowerCase();
      return roots.some(root => {
        const normalized = root.replace(/[\\/]+$/, '').toLowerCase();
        return lower === normalized || lower.startsWith(normalized + '\\');
      });
    };
    const realRoots = canonicalModuleRoots.map(root => root.canonical);
    const permittedPaths = canonicalModuleRoots.flatMap(
        root => [root.configured, root.canonical]);
    const inRoot = filename => within(filename, permittedPaths);
    const entries = new Map();
    const inspect = filename => {
      filename = pathModule.normalize(filename);
      if (!inRoot(filename)) return null;
      if (entries.has(filename)) return entries.get(filename);
      const virtual = memFiles.get(normalizeFsPath(filename));
      if (virtual) {
        const entry = {type: virtual.type, filename};
        entries.set(filename, entry);
        return entry;
      }
      try {
        const stat = fsNativeSync('stat', filename);
        const canonical = canonicalPath(filename);
        if (!within(canonical, realRoots)) {
          throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${request}'`);
        }
        const entry = {
          type: stat.isFile ? 'file' : stat.isDirectory ? 'dir' : '',
          filename: canonical,
        };
        entries.set(filename, entry);
        return entry;
      } catch (error) {
        if (error.code === 'ENOENT' || error.code === 'ENOTDIR') {
          entries.set(filename, null);
          return null;
        }
        throw error;
      }
    };
    // exports and main resolution can inspect the same package. Reuse its
    // parsed config only for this resolution; a later retry or cache deletion
    // must observe the current file, including changed main/exports fields.
    let packageConfigs;
    const packageJson = directory => {
      const entry = inspect(pathModule.join(directory, 'package.json'));
      if (!entry || entry.type !== 'file') return null;
      const filename = entry.filename;
      packageConfigs ||= new Map();
      if (packageConfigs.has(filename)) return packageConfigs.get(filename);
      try {
        const config = JSON.parse(readHostedCjsSource(filename));
        packageConfigs.set(filename, config);
        return config;
      } catch (error) {
        if (error instanceof SyntaxError) {
          throw moduleError('ERR_INVALID_PACKAGE_CONFIG',
                            `Invalid package config ${filename}: ${error.message}`);
        }
        throw error;
      }
    };
    const asFile = filename => {
      const exact = inspect(filename);
      if (exact && exact.type === 'file') return exact.filename;
      for (const extension of ['.js', '.json', '.node']) {
        const entry = inspect(filename + extension);
        if (entry && entry.type === 'file') return entry.filename;
      }
      return '';
    };
    const asPath = (filename, depth = 0) => {
      if (depth > 32) {
        throw moduleError('ERR_INVALID_PACKAGE_CONFIG', 'Package main resolution is too deeply nested');
      }
      const file = asFile(filename);
      if (file) return file;
      const directory = inspect(filename);
      if (!directory || directory.type !== 'dir') return '';
      filename = directory.filename;
      const pkg = packageJson(filename);
      if (pkg && typeof pkg.main === 'string' && pkg.main) {
        const main = pathModule.resolve(filename, pkg.main);
        if (main !== filename) {
          const resolved = asPath(main, depth + 1);
          if (resolved) return resolved;
        }
      }
      for (const index of ['index.js', 'index.json', 'index.node']) {
        const entry = inspect(pathModule.join(filename, index));
        if (entry && entry.type === 'file') return entry.filename;
      }
      return '';
    };
    let resolved = '';
    if (pathModule.isAbsolute(request) || /^\.\.?([\\/]|$)/.test(request)) {
      resolved = asPath(pathModule.resolve(pathModule.dirname(parentFile), request));
    } else {
      const parts = request.replace(/\\/g, '/').split('/');
      const packageName = parts[0].startsWith('@') ? parts.slice(0, 2).join('/') : parts[0];
      let directory = pathModule.dirname(parentFile);
      while (inRoot(directory)) {
        if (pathModule.basename(directory).toLowerCase() !== 'node_modules') {
          const modules = pathModule.join(directory, 'node_modules');
          const pkg = packageJson(pathModule.join(modules, packageName));
          if (pkg && Object.prototype.hasOwnProperty.call(pkg, 'exports')) {
            throw moduleError('ERR_NOT_SUPPORTED',
                              `Package exports are not supported: ${packageName}`);
          }
          resolved = asPath(pathModule.join(modules, request));
          if (resolved) break;
        }
        const parent = pathModule.dirname(directory);
        if (parent === directory) break;
        directory = parent;
      }
    }
    if (!resolved) throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${request}'`);
    if (hostedCjsCache[resolved]) moduleResolutionCache.set(cacheKey, resolved);
    return resolved;
  }

  function loadHostedCjsModule(resolved) {
    if (hostedCjsCache[resolved]) return hostedCjsCache[resolved].exports;
    installBindingsFileNameShim();
    const moduleObject = {
      exports: {},
      filename: resolved,
      id: resolved,
      loaded: false,
      children: [],
      parent: null,
      paths: [],
    };
    hostedCjsCache[resolved] = moduleObject;
    const previousLoading = hostedCjsLoadingFile;
    hostedCjsLoadingFile = resolved;
    try {
      // Bind relative requires to this module, including requires called by
      // exported functions after the module's initial evaluation has finished.
      const moduleRequire = request => electronRequire(request, resolved);
      moduleRequire.resolve = request => resolveHostedCjs(request, resolved);
      moduleRequire.cache = hostedCjsCache;
      moduleObject.require = moduleRequire;
      if (/\.node$/i.test(resolved)) {
        moduleObject.exports = loadNativeNodeModule(resolved);
      } else {
        const code = readHostedCjsSource(resolved).replace(/^\uFEFF/, '');
        if (/\.json$/i.test(resolved)) {
          moduleObject.exports = JSON.parse(code);
        } else {
          const fn = new Function(
              'exports', 'require', 'module', '__filename', '__dirname', 'process', 'Buffer',
              code.replace(/^#![^\r\n]*/, '') + '\n//# sourceURL=' + resolved.replace(/\\/g, '/'));
          fn.call(moduleObject.exports, moduleObject.exports, moduleRequire, moduleObject, resolved,
              pathModule.dirname(resolved), process, Buffer);
        }
      }
      moduleObject.loaded = true;
    } catch (error) {
      delete hostedCjsCache[resolved];
      throw error;
    } finally {
      hostedCjsLoadingFile = previousLoading;
    }
    return moduleObject.exports;
  }

  function readHostedCjsSource(filename) {
    // Preloads and their dependencies are real application modules. Do not
    // hide on-disk scripts behind the renderer's synthetic filesystem mount.
    return memFiles.has(normalizeFsPath(filename)) ?
        String(fsModule.readFileSync(filename, 'utf8')) :
        fsReadResult(fsNativeSync('read_file', filename), 'utf8');
  }

  function loadBuiltinModule(request) {
    if (typeof request !== 'string') {
      throw new TypeError('Module name must be a string');
    }

    const norm = request;
    if (norm === 'http2') return http2Module;
    if (norm === 'path/win32') return pathModule.win32;
    if (norm === 'path/posix') return pathModule.posix;

    // Electron
    if (norm === 'electron' || norm === 'node:electron') {
      return electron;
    }

    if (norm === 'process') return process;

    // Path
    if (norm === 'path' || norm === 'node:path') {
      return pathModule;
    }

    // OS
    if (norm === 'os' || norm === 'node:os') {
      return osModule;
    }

    // Events
    if (norm === 'events' || norm === 'node:events') {
      return EventEmitter;
    }

    // Async hooks
    if (norm === 'async_hooks' || norm === 'node:async_hooks') {
      return asyncHooksModule;
    }

    // Buffer
    if (norm === 'buffer' || norm === 'node:buffer') {
      return { Buffer };
    }

    // Util
    if (norm === 'util' || norm === 'node:util') {
      return utilModule;
    }

    // Net
    if (norm === 'net' || norm === 'node:net') {
      return netModule;
    }

    // Stream
    if (norm === 'stream' || norm === 'node:stream') {
      return streamModule;
    }

    // Crypto
    if (norm === 'crypto' || norm === 'node:crypto') {
      return cryptoModule;
    }

    // URL
    if (norm === 'url' || norm === 'node:url') {
      return urlModule;
    }

    // TTY (readable-stream / debug / winston often require this)
    if (norm === 'tty' || norm === 'node:tty') {
      return {
        isatty: () => false,
        ReadStream: class extends EventEmitter {},
        WriteStream: class extends EventEmitter {
          constructor() {
            super();
            this.columns = 80;
            this.rows = 24;
            this.isTTY = false;
          }
          write() { return true; }
        },
      };
    }

    // DNS
    if (norm === 'dns' || norm === 'node:dns') {
      const unsupported = (hostname) => {
        const error = new Error(
            `DNS resolution is unavailable in the hosted runtime: ${hostname}`);
        error.code = 'ENOTSUP';
        error.syscall = 'getaddrinfo';
        error.hostname = String(hostname || '');
        return error;
      };
      return {
        lookup: (hostname, opts, cb) => {
          const callback = typeof opts === 'function' ? opts : cb;
          if (typeof callback === 'function') {
            queueMicrotask(() => callback(unsupported(hostname)));
          }
        },
        resolve: (hostname, cb) => {
          if (typeof cb === 'function') {
            queueMicrotask(() => cb(unsupported(hostname)));
          }
        },
        promises: {
          lookup: async hostname => { throw unsupported(hostname); },
          resolve: async hostname => { throw unsupported(hostname); },
        },
      };
    }

    // Timers
    if (norm === 'timers' || norm === 'node:timers') {
      return {
        setTimeout: globalThis.setTimeout.bind(globalThis),
        clearTimeout: globalThis.clearTimeout.bind(globalThis),
        setInterval: globalThis.setInterval.bind(globalThis),
        clearInterval: globalThis.clearInterval.bind(globalThis),
        setImmediate: (fn, ...args) =>
            globalThis.setTimeout(() => fn(...args), 0),
        clearImmediate: (id) => globalThis.clearTimeout(id),
      };
    }

    // Querystring
    if (norm === 'querystring' || norm === 'node:querystring') {
      return {
        parse: (str) => Object.fromEntries(new URLSearchParams(str)),
        stringify: (obj) => new URLSearchParams(obj || {}).toString(),
        escape: encodeURIComponent,
        unescape: decodeURIComponent,
      };
    }

    // StringDecoder
    if (norm === 'string_decoder' || norm === 'node:string_decoder') {
      const cls = class StringDecoder {
        constructor(enc = 'utf8') { this.encoding = enc; }
        write(buf) { return new TextDecoder(this.encoding).decode(buf); }
        end(buf) { return buf ? new TextDecoder(this.encoding).decode(buf) : ''; }
      };
      return { StringDecoder: cls, default: cls };
    }

    // TLS
    if (norm === 'tls' || norm === 'node:tls') {
      return tlsModule;
    }

    // HTTP / HTTPS — Node ClientRequest surface backed by the browser network
    // service.
    // A no-op stub never fires response/error callbacks, so any await on
    // https.request hangs forever.
    if (norm === 'http' || norm === 'node:http' || norm === 'https' ||
        norm === 'node:https') {
      const buildRequestUrl = (opt) => {
        if (typeof opt === 'string') {
          return opt;
        }
        if (opt && typeof opt === 'object' && opt.href) {
          return String(opt.href);
        }
        const o = opt && typeof opt === 'object' ? opt : {};
        const protocol =
            o.protocol || (norm.indexOf('https') >= 0 ? 'https:' : 'http:');
        const host = o.hostname || o.host || 'localhost';
        const port = o.port ? `:${o.port}` : '';
        const path = o.path || o.pathname || '/';
        return `${protocol}//${host}${port}${path}`;
      };

      const createClientRequest = (opt, cb) => {
        const req = new EventEmitter();
        if (typeof cb === 'function') req.once('response', cb);
        const bodyChunks = [];
        let finished = false;
        let timeoutId = null;
        let aborted = false;
        const options = typeof opt === 'string' ? {href: opt} : (opt || {});

        const cancelPendingRequest = () => {
          aborted = true;
          bodyChunks.length = 0;
          if (timeoutId !== null) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
        };
        req.write = (chunk, encoding) => {
          if (aborted) return false;
          if (chunk == null) {
            return true;
          }
          if (typeof chunk === 'string') {
            const format = typeof encoding === 'string' ? encoding.toLowerCase() : 'utf8';
            if (['latin1', 'binary', 'ascii'].includes(format)) {
              const bytes = Buffer.alloc(chunk.length);
              for (let i = 0; i < chunk.length; ++i) bytes[i] = chunk.charCodeAt(i) & 255;
              bodyChunks.push(bytes);
            } else if (['utf16le', 'utf-16le', 'ucs2', 'ucs-2'].includes(format)) {
              const bytes = Buffer.alloc(chunk.length * 2);
              for (let i = 0; i < chunk.length; ++i) {
                const code = chunk.charCodeAt(i);
                bytes[i * 2] = code & 255;
                bytes[i * 2 + 1] = code >>> 8;
              }
              bodyChunks.push(bytes);
            } else if (['utf8', 'utf-8', 'hex', 'base64', 'base64url'].includes(format)) {
              bodyChunks.push(Buffer.from(chunk, format));
            } else {
              const error = new TypeError(`Unknown encoding: ${encoding}`);
              error.code = 'ERR_UNKNOWN_ENCODING';
              throw error;
            }
          } else if (ArrayBuffer.isView(chunk)) {
            // Snapshot the visible raw bytes once at write time, including
            // non-byte typed arrays and DataView subviews.
            bodyChunks.push(Buffer.from(new Uint8Array(
                chunk.buffer, chunk.byteOffset, chunk.byteLength)));
          } else {
            bodyChunks.push(String(chunk));
          }
          return true;
        };
        req.setTimeout = (ms, onTimeout) => {
          if (aborted) return req;
          if (timeoutId) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
          const delay = Number(ms);
          if (!(delay > 0)) {
            return req;
          }
          timeoutId = setTimeout(() => {
            if (aborted) return;
            cancelPendingRequest();
            const err = new Error('Request timeout');
            err.code = 'ETIMEDOUT';
            req.emit('timeout');
            if (typeof onTimeout === 'function') {
              onTimeout();
            }
            req.emit('error', err);
          }, delay);
          return req;
        };
        req.abort = () => {
          cancelPendingRequest();
          return req;
        };
        req.destroy = (err) => {
          cancelPendingRequest();
          if (err) {
            req.emit('error', err);
          }
          return req;
        };
        req.end = (chunk, encoding) => {
          if (finished || aborted) {
            return req;
          }
          finished = true;
          if (chunk != null) {
            req.write(chunk, encoding);
          }
          const timeoutMs = Number(options.timeout);
          if (timeoutMs > 0 && !timeoutId) {
            req.setTimeout(timeoutMs);
          }
          queueMicrotask(async () => {
            try {
              if (aborted) {
                return;
              }
              const method = String(options.method || 'GET').toUpperCase();
              const headers = Object.assign({}, options.headers || {});
              let bodyBuffer = Buffer.alloc(0);
              if (method !== 'GET' && method !== 'HEAD' && bodyChunks.length) {
                bodyBuffer = Buffer.concat(bodyChunks.map(part =>
                    typeof part === 'string' ? Buffer.from(part) : part));
              }
              // The encoded request owns its payload after submission. Do not
              // keep the caller's queued upload snapshots on the request.
              bodyChunks.length = 0;
              // Electron/Node requests use the browser network service rather
              // than renderer fetch. This preserves caller headers and avoids
              // applying renderer CORS policy to a Node networking API.
              const response = await transport.invoke('__xenon:net-request', {
                url: buildRequestUrl(options),
                method,
                headers,
                bodyBase64: bodyBuffer.toString('base64'),
                useSessionCookies: Boolean(options.useSessionCookies),
              });
              if (timeoutId) {
                clearTimeout(timeoutId);
                timeoutId = null;
              }
              if (aborted) {
                return;
              }
              const responseBody = Buffer.from(
                  String(response && response.bodyBase64 || ''), 'base64');
              const incoming = new EventEmitter();
              incoming.statusCode = Number(response && response.statusCode) || 0;
              incoming.statusMessage =
                  String(response && response.statusMessage || '');
              incoming.headers = Object.assign({}, response && response.headers);
              incoming.url = String(response && response.finalUrl || '');
              let responseEncoding = null;
              incoming.setEncoding = (encoding) => {
                responseEncoding = String(encoding || 'utf8');
                return incoming;
              };
              req.emit('response', incoming);
              queueMicrotask(() => {
                if (aborted) return;
                if (responseBody.length) {
                  incoming.emit('data', responseEncoding ?
                      responseBody.toString(responseEncoding) : responseBody);
                }
                if (aborted) return;
                incoming.emit('end');
              });
            } catch (error) {
              if (timeoutId) {
                clearTimeout(timeoutId);
                timeoutId = null;
              }
              if (!aborted) {
                req.emit('error', error instanceof Error ? error
                                                         : new Error(String(error)));
              }
            }
          });
          return req;
        };
        return req;
      };

      return {
        request: (opt, cb) => createClientRequest(opt, cb),
        get: (url, cb) => {
          const req = createClientRequest(url, cb);
          req.end();
          return req;
        },
        createServer: () => {
          throw moduleError('ERR_NOT_SUPPORTED',
              `${norm.replace(/^node:/, '')}.createServer is not supported by this runtime`);
        },
        Agent: class {},
      };
    }

    // Child Process
    if (norm === 'child_process' || norm === 'node:child_process') {
      return childProcessModule;
    }

    // Zlib
    if (norm === 'zlib' || norm === 'node:zlib') {
      return createZlibModule((operation, data, options, asynchronous) => {
        const request = {operation, dataBase64: data.toString('base64'), options};
        if (asynchronous) {
          return transport.invoke('__xenon:zlib', request)
              .then(value => Buffer.from(value, 'base64'));
        }
        return Buffer.from(transport.sendSync('__xenon:zlib', request), 'base64');
      });
    }

    // Assert
    if (norm === 'assert' || norm === 'node:assert') {
      const assertFn = (val, msg) => { if (!val) throw new Error(msg || 'Assertion failed'); };
      assertFn.ok = assertFn;
      assertFn.strictEqual = (a, b) => { if (a !== b) throw new Error('Assertion failed'); };
      return assertFn;
    }

    // Constants
    if (norm === 'constants' || norm === 'node:constants') {
      return {};
    }

    // Readline
    if (norm === 'readline' || norm === 'node:readline') {
      return readlineModule;
    }

    // FS
    if (norm === 'fs' || norm === 'node:fs' || norm === 'fs/promises' || norm === 'node:fs/promises') {
      return norm.includes('promises') ? fsModule.promises : fsModule;
    }

    if (norm === 'xenon:sqlite3') {
      return sqlite3Module;
    }
    throw moduleError('ERR_NOT_SUPPORTED', `Module '${request}' is not supported by this runtime`);
  }

  const electronRequire = globalThis.require = function(request, parentFile = globalThis.__filename) {
    const builtin = builtinModuleId(request);
    if (builtin) {
      if (!builtinModuleCache.has(builtin)) {
        builtinModuleCache.set(builtin, loadBuiltinModule(builtin));
      }
      return builtinModuleCache.get(builtin);
    }
    const resolved = resolveHostedCjs(request, parentFile);
    const exports = loadHostedCjsModule(resolved);
    moduleResolutionCache.set(`${parentFile}\0${request}`, resolved);
    return exports;
  };

  globalThis.Buffer = Buffer;
  globalThis.require.resolve = request => resolveHostedCjs(request);
  globalThis.require.cache = hostedCjsCache;
  globalThis.require.main = undefined;
  globalThis.require.extensions = { '.js': () => {}, '.json': () => {}, '.node': () => {} };
  globalThis.__xenonElectronRequire = globalThis.require;

  // Locale identifiers used by application i18n commonly contain underscores
  // (for example zh_CN), while HTTP Accept-Language requires BCP 47 language
  // tags (zh-CN). An underscore makes this otherwise CORS-safelisted header
  // require preflight. Normalize only this standard header at the transport
  // boundary; request bodies and application response state remain untouched.
  const normalizeAcceptLanguage = (value) =>
      String(value == null ? '' : value).replace(/_/g, '-');

  if (typeof globalThis.fetch === 'function' &&
      typeof Headers === 'function') {
    const originalFetch = globalThis.fetch.bind(globalThis);
    const normalizeHeaders = (headers) => {
      const normalized = new Headers(headers || {});
      if (normalized.has('accept-language')) {
        normalized.set(
            'accept-language',
            normalizeAcceptLanguage(normalized.get('accept-language')));
      }
      return normalized;
    };
    globalThis.fetch = (input, init) => {
      const nextInit = init ? Object.assign({}, init) : {};
      if (nextInit.headers) {
        nextInit.headers = normalizeHeaders(nextInit.headers);
      } else if (typeof Request === 'function' && input instanceof Request) {
        input = new Request(input, {headers: normalizeHeaders(input.headers)});
      }
      return originalFetch(input, nextInit);
    };
  }

  if (typeof XMLHttpRequest === 'function' && XMLHttpRequest.prototype &&
      typeof XMLHttpRequest.prototype.setRequestHeader === 'function') {
    const originalSetRequestHeader =
        XMLHttpRequest.prototype.setRequestHeader;
    XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
      if (String(name).toLowerCase() === 'accept-language') {
        value = normalizeAcceptLanguage(value);
      }
      return originalSetRequestHeader.call(this, name, value);
    };
  }

  if (typeof window !== 'undefined') {
    window.Buffer = Buffer;
    window.process = globalThis.process;
    window.require = globalThis.require;
    window.__xenonElectronRequire = globalThis.require;
    const evaluate = globalThis.eval;
    ipcRenderer.on('__xenon:execute-javascript', async (_event, id, code) => {
      try {
        const value = await evaluate(code);
        transport.send('__xenon:execute-javascript-result', id, true, value);
      } catch (error) {
        transport.send('__xenon:execute-javascript-result', id, false, String(error.message || error));
      }
    });

    // A webview owns a Browser-managed inner WebContents. The blank iframe is
    // only Chromium's attachment point; it never navigates to the guest URL.
    const guestElements = new Map();
    ipcRenderer.on('__xenon:guest-event', (_event, id, name, details = {}) => {
      const el = guestElements.get(id);
      if (!el) return;
      if (details.url) el._guestUrl = details.url;
      if (details.title) el._guestTitle = details.title;
      if ('canGoBack' in details) el._canGoBack = details.canGoBack;
      if ('canGoForward' in details) el._canGoForward = details.canGoForward;
      if (name === 'did-start-loading') el._loading = true;
      if (name === 'did-stop-loading') el._loading = false;
      el._emit(name, details);
      if (name === 'destroyed') {
        guestElements.delete(id);
        el._guestId = 0;
        el._attaching = null;
        el._guestFrame.remove();
        el._guestFrame = null;
      }
    });

    function upgradeWebView(el) {
      if (!el || el.__xenon_webview_upgraded__) {
        el?._ensureGuestAttached?.();
        return;
      }
      el.__xenon_webview_upgraded__ = true;
      Object.assign(el.style, {
        display: el.style.display || 'block',
        width: el.style.width || '100%', height: el.style.height || '100%',
      });
      el._guestId = 0;
      el._guestUrl = '';
      el._guestTitle = '';
      el._loading = false;
      el._emit = (name, details = {}) => {
        const event = new CustomEvent(name, {detail: details});
        Object.assign(event, details);
        el.dispatchEvent(event);
      };
      const preferences = () => {
        const prefs = {contextIsolation: true, sandbox: true};
        for (const item of (el.getAttribute('webpreferences') || '').split(',')) {
          const [key, raw] = item.trim().split('=');
          if (!key) continue;
          prefs[key] = raw === undefined || /^(yes|true|1)$/i.test(raw) ? true :
              /^(no|false|0)$/i.test(raw) ? false : raw;
        }
        prefs.preload = el.getAttribute('preload') || '';
        prefs.userAgent = el.getAttribute('useragent') || '';
        prefs.nodeIntegration = el.hasAttribute('nodeintegration');
        prefs.nodeIntegrationInSubFrames = el.hasAttribute('nodeintegrationinsubframes');
        prefs.allowPopups = el.hasAttribute('allowpopups');
        if (el.hasAttribute('partition')) prefs.partition = el.getAttribute('partition');
        return prefs;
      };
      el._ensureGuestAttached = () => {
        if (!el.isConnected || el._guestId || el._attaching) return;
        // Vue may set attributes after createElement(). Wait until the element
        // is connected and all initial attributes have been assigned.
        el._attaching = Promise.resolve().then(async () => {
          if (!el.isConnected) { el._attaching = null; return; }
          const frame = document.createElement('iframe');
          frame.src = 'about:blank';
          Object.assign(frame.style, {width: '100%', height: '100%', border: '0', display: 'block'});
          el._guestFrame = frame;
          el.appendChild(frame);
          const id = await transport.attachGuest(frame, preferences());
          el._guestId = id;
          guestElements.set(id, el);
          const url = el.getAttribute('src');
          if (url) await ipcRenderer.invoke('__xenon:guest-call', id, 'loadURL', [url]);
        }).catch(error => {
          console.error('[xenon-ipc] Unable to attach webview:', error);
          el._emit('did-fail-load', {errorCode: -2, errorDescription: error.message,
            validatedURL: el.getAttribute('src') || '', isMainFrame: true});
        });
      };
      const call = async (method, ...args) => {
        el._ensureGuestAttached();
        await el._attaching;
        if (!el._guestId) throw new Error('Guest WebContents is not attached');
        return ipcRenderer.invoke('__xenon:guest-call', el._guestId, method, args);
      };
      Object.defineProperty(el, 'src', {
        get: () => el.getAttribute('src') || '',
        set: value => el.setAttribute('src', String(value || '')),
        configurable: true,
      });
      const observer = new MutationObserver(mutations => {
        for (const mutation of mutations) {
          if (el._guestId && mutation.attributeName === 'src') {
            call('loadURL', el.src).catch(error => console.error(error));
          } else if (el._guestId && mutation.attributeName === 'useragent') {
            call('setUserAgent', el.getAttribute('useragent')).catch(error => console.error(error));
          }
        }
        el._ensureGuestAttached();
      });
      observer.observe(el, {attributes: true, attributeFilter: ['src', 'useragent']});
      el.loadURL = url => call('loadURL', String(url));
      el.getURL = () => el._guestUrl || el.src;
      el.getTitle = () => el._guestTitle;
      el.isLoading = () => el._loading;
      el.isWaitingForResponse = () => el._loading;
      el.canGoBack = () => Boolean(el._canGoBack);
      el.canGoForward = () => Boolean(el._canGoForward);
      for (const method of ['reload', 'stop', 'goBack', 'goForward',
                            'send', 'sendToFrame', 'executeJavaScript']) {
        el[method] = (...args) => call(method, ...args);
      }
      el.setUserAgent = ua => el.setAttribute('useragent', String(ua));
      el.getUserAgent = () => el.getAttribute('useragent') || navigator.userAgent;
      el.getWebContentsId = () => {
        if (!el._guestId) throw new Error('Guest WebContents is not attached');
        return el._guestId;
      };
      el.focus = () => el._guestFrame?.focus();
      el._ensureGuestAttached();
    }

    if (typeof Document !== 'undefined' && Document.prototype) {
      for (const method of ['createElement', 'createElementNS']) {
        const original = Document.prototype[method];
        Document.prototype[method] = function(...args) {
          const el = original.apply(this, args);
          if (el.nodeName === 'WEBVIEW') upgradeWebView(el);
          return el;
        };
      }
    }
    const startWebviewObserver = () => {
      if (!document.documentElement) {
        document.addEventListener('DOMContentLoaded', startWebviewObserver, {once: true});
        return;
      }
      new MutationObserver(mutations => {
        for (const mutation of mutations) {
          for (const node of mutation.addedNodes) {
            if (node.nodeType !== 1) continue;
            if (node.nodeName === 'WEBVIEW') upgradeWebView(node);
            node.querySelectorAll?.('webview').forEach(upgradeWebView);
          }
        }
      }).observe(document.documentElement, {childList: true, subtree: true});
      document.querySelectorAll('webview').forEach(upgradeWebView);
    };
    startWebviewObserver();

    // This bootstrap runs at document creation, before application scripts.
    // Resolve preferences through the document's registered WebContents, not
    // a process-global window ID or an application-specific preload alias.
    let preload = '';
    let preferences = null;
    try {
      preferences =
          transport.sendSync('__xenon:renderer-web-preferences');
      preload = preferences && preferences.preload;
      if (preload) {
        if (preferences.contextIsolation !== false) {
          throw new Error('Isolated-world preloads are not supported yet');
        }
        electronRequire(preload);
      }
    } catch (error) {
      console.error('[xenon-ipc] Unable to load preload script:', preload, error);
      ipcRenderer.send('__xenon:preload-error', preload, String(error.message || error));
    }
    // A guest preload can keep Node APIs in its CommonJS closure without
    // giving the remote page require(), process, or the privileged transport.
    if (injectedPaths.isGuest && preferences?.nodeIntegration !== true) {
      for (const key of ['require', 'process', 'Buffer', 'global', '__filename',
                        '__dirname', 'xenonIpcRenderer', '__xenonElectronRequire',
                        '__xenonElectronIpc', '__xenonPaths']) {
        delete globalThis[key];
      }
    } else if (globalThis.electron === undefined) {
      globalThis.electron = electron;
    }
  }
})();
