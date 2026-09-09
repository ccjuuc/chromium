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
  }
  EventEmitter.prototype.on = function(name, listener) {
    if (typeof listener !== 'function') {
      throw new TypeError('IPC listener must be a function');
    }
    const listeners = this.events_.get(name) || [];
    listeners.push(listener);
    this.events_.set(name, listeners);
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
    const listeners = this.events_.get(name);
    if (!listeners || !listeners.length) {
      return false;
    }
    for (const listener of [...listeners]) {
      listener.apply(this, args);
    }
    return true;
  };
  EventEmitter.prototype.removeListener = function(name, listener) {
    const listeners = this.events_.get(name);
    if (!listeners) {
      return this;
    }
    const remaining = listeners.filter(
        candidate => candidate !== listener && candidate.listener !== listener);
    if (remaining.length) {
      this.events_.set(name, remaining);
    } else {
      this.events_.delete(name);
    }
    return this;
  };
  EventEmitter.prototype.off = function(name, listener) {
    return this.removeListener(name, listener);
  };
  EventEmitter.prototype.removeAllListeners = function(name) {
    if (name === undefined) {
      this.events_.clear();
    } else {
      this.events_.delete(name);
    }
    return this;
  };
  EventEmitter.prototype.listeners = function(name) {
    return [...(this.events_.get(name) || [])];
  };
  EventEmitter.prototype.listenerCount = function(name) {
    return (this.events_.get(name) || []).length;
  };
  EventEmitter.prototype.setMaxListeners = function(_n) {
    return this;
  };
  EventEmitter.prototype.getMaxListeners = function() {
    return 100;
  };
  EventEmitter.EventEmitter = EventEmitter;
  EventEmitter.default = EventEmitter;
  EventEmitter.defaultMaxListeners = 100;

  // Node's async_hooks implementation is embedder-backed. Hosted renderers
  // still need its public context API for libraries such as OpenTelemetry.
  // Preserve the synchronous scope semantics; AsyncResource.bind() lets those
  // libraries carry the captured scope into callbacks they own.
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

  // --- 2. ipcRenderer ---
  const ipcRenderer = new EventEmitter();
  let dispatchXenonNet = null;
  let dispatchNodeAddon = null;

  ipcRenderer.send = (channel, ...args) => transport.send(channel, ...args);
  ipcRenderer.sendSync = (channel, ...args) =>
      transport.sendSync(channel, ...args);
  ipcRenderer.invoke = (channel, ...args) => transport.invoke(channel, ...args);
  ipcRenderer.postMessage = (channel, message, transfer) => {
    if (transfer && transfer.length) {
      throw new TypeError('MessagePort transfer is not supported by this transport');
    }
    transport.postMessage(channel, message);
  };
  ipcRenderer.sendToHost = (channel, ...args) => {
    return transport.send('__xenon:send-to-host', channel, args);
  };

  transport.setDispatchHandler((channel, args) => {
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
    ipcRenderer.emit(channel, {sender: ipcRenderer}, ...values);
  });

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

  const electron = {
    ipcRenderer,
    app: appApi,
    dialog: {
      showOpenDialog: async (options) => {
        const filePaths = await showNativeOpenDialog(options);
        return {canceled: !filePaths.length, filePaths};
      },
      showOpenDialogSync: (options) => {
        console.warn(
            '[Xenon Renderer] dialog.showOpenDialogSync is async-only; use showOpenDialog');
        return [];
      },
      showSaveDialog: async () => ({ canceled: true, filePath: '' }),
      showMessageBox: async () => ({ response: 0 }),
    },
    remote: {
      dialog: {
        showOpenDialog: async (options) => {
          const filePaths = await showNativeOpenDialog(options);
          return {canceled: !filePaths.length, filePaths};
        },
        showOpenDialogSync: (options) => [],
      },
      app: appApi,
      getCurrentWindow: () => ({
        isMaximized: () => false,
        isMinimized: () => false,
        isFullScreen: () => false,
        maximize: () => {},
        unmaximize: () => {},
        minimize: () => {},
        close: () => {},
        setFullScreen: () => {},
        webContents: { id: 1, send: () => {} },
      }),
    },
    clipboard: {
      readText: () => '',
      writeText: (_text) => {},
    },
    shell: {
      openExternal: (url) => window.open(url, '_blank'),
      openPath: (_path) => Promise.resolve(''),
      showItemInFolder: (_path) => {},
    },
    webFrame: {
      setZoomFactor: (_factor) => {},
      getZoomFactor: () => 1,
      setZoomLevel: (_level) => {},
      getZoomLevel: () => 0,
    },
  };
  globalThis.__xenonElectronIpc = electron;
  if (globalThis.electron === undefined) {
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

  const pathModule = Object.assign({}, win32, { win32, posix });

  // --- 4. OS Module ---
  const osModule = {
    networkInterfaces: () => transport.sendSync('__xenon:os-network-interfaces'),
    platform: () => 'win32',
    arch: () => 'x64',
    type: () => 'Windows_NT',
    release: () => '10.0.19045',
    homedir: () => homeDir,
    tmpdir: () => tempDir,
    hostname: () => 'XenonHost',
    userInfo: () => ({
      username: 'Administrator',
      uid: -1,
      gid: -1,
      homedir: homeDir,
      shell: null,
    }),
    cpus: () => [{ model: 'Intel(R) Core(TM)', speed: 2800, times: { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 } }],
    totalmem: () => 17179869184,
    freemem: () => 8589934592,
    endianness: () => 'LE',
    EOL: '\r\n',
  };

  // --- 5. Buffer & Util ---
  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder();

  class Buffer extends Uint8Array {
    static from(value, encoding) {
      if (typeof value === 'string') {
        if (encoding === 'hex') {
          const match = value.match(/.{1,2}/g) || [];
          return new Buffer(match.map(byte => parseInt(byte, 16)));
        }
        if (encoding === 'base64') {
          const binary = atob(value);
          const bytes = new Uint8Array(binary.length);
          for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
          return new Buffer(bytes.buffer);
        }
        return new Buffer(textEncoder.encode(value).buffer);
      }
      if (ArrayBuffer.isView(value)) {
        return new Buffer(value.buffer, value.byteOffset, value.byteLength);
      }
      if (value instanceof ArrayBuffer) {
        return new Buffer(value);
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
      return obj instanceof Uint8Array;
    }

    static byteLength(string, encoding) {
      return Buffer.from(string, encoding).length;
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
      if (encoding === 'base64') {
        let binary = '';
        for (let i = 0; i < view.byteLength; i++) {
          binary += String.fromCharCode(view[i]);
        }
        return btoa(binary);
      }
      return textDecoder.decode(view);
    }

    slice(start, end) {
      return Buffer.from(Uint8Array.prototype.slice.call(this, start, end));
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
    return Buffer.isBuffer(data) || ArrayBuffer.isView(data) ?
        Buffer.from(data) : Buffer.from(String(data), 'utf8');
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

  function fsCallback(promise, callback, includeValue = false) {
    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }
    promise.then(
        value => includeValue ? callback(null, value) : callback(null),
        error => callback(error));
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
      const bytes = Buffer.isBuffer(data) || ArrayBuffer.isView(data) ?
          Buffer.from(data) :
          Buffer.from(String(data), 'utf8');
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
      const extra = Buffer.isBuffer(data) || ArrayBuffer.isView(data) ?
          Buffer.from(data) :
          Buffer.from(String(data), 'utf8');
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
    chmodSync: (path, _mode) => fsUsesVirtualMount(path) ?
        undefined : fsNativeSync('chmod', path),
    createReadStream: () => new streamModule.Readable(),
    createWriteStream: () => new streamModule.Writable(),
    promises: {},
  };

  fsModule.readFile = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    fsCallback(fsAsyncOperation(
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
    fsCallback(fsAsyncOperation(
        path, 'write_file', extra,
        () => fsModule.writeFileSync(path, data, options)), callback);
  };
  fsModule.stat = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(fsAsyncOperation(
        path, 'stat', {}, () => fsModule.statSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
        callback, true);
  };
  fsModule.lstat = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(fsAsyncOperation(
        path, 'lstat', {}, () => fsModule.lstatSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
        callback, true);
  };
  fsModule.readdir = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(fsAsyncOperation(
        path, 'readdir', {}, () => fsModule.readdirSync(path)), callback, true);
  };
  fsModule.mkdir = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {recursive: Boolean(options && options.recursive)};
    fsCallback(fsAsyncOperation(
        path, 'mkdir', extra, () => fsModule.mkdirSync(path, options)), callback);
  };
  fsModule.unlink = (path, callback) => fsCallback(fsAsyncOperation(
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
    fsCallback(fsAsyncOperation(
        path, 'rm', extra, () => fsModule.rmSync(path, options)), callback);
  };
  fsModule.rmdir = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {recursive: Boolean(options && options.recursive)};
    fsCallback(fsAsyncOperation(
        path, 'rmdir', extra, () => fsModule.rmdirSync(path, options)), callback);
  };
  fsModule.access = (path, mode, callback) => {
    if (typeof mode === 'function') callback = mode;
    fsCallback(fsAsyncOperation(
        path, 'access', {}, () => fsModule.accessSync(path, mode)), callback);
  };
  fsModule.appendFile = (path, data, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {dataBase64: fsBytes(data).toString('base64')};
    fsCallback(fsAsyncOperation(
        path, 'append_file', extra,
        () => fsModule.appendFileSync(path, data, options)), callback);
  };
  fsModule.rename = (oldPath, newPath, callback) => fsCallback(
      fsUsesVirtualMount(oldPath) ?
          Promise.resolve().then(() => fsModule.renameSync(oldPath, newPath)) :
          fsNativeAsync('rename', oldPath, {destination: normalizeFsPath(newPath)}),
      callback);
  fsModule.copyFile = (src, dest, flags, callback) => {
    if (typeof flags === 'function') callback = flags;
    fsCallback(fsUsesVirtualMount(src) ?
        Promise.resolve().then(() => fsModule.copyFileSync(src, dest)) :
        fsNativeAsync('copy_file', src, {destination: normalizeFsPath(dest)}),
        callback);
  };
  fsModule.chmod = (path, mode, callback) => fsCallback(fsAsyncOperation(
      path, 'chmod', {}, () => fsModule.chmodSync(path, mode)), callback);
  fsModule.exists = (path, callback) => {
    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }
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
    fsCallback(fsAsyncOperation(
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
    chmod: (path, mode) => fsAsyncOperation(
        path, 'chmod', {}, () => fsModule.chmodSync(path, mode)),
    realpath: (path) => fsAsyncOperation(
        path, 'realpath', {}, () => String(path)),
  };
  fsModule.default = fsModule;

  // --- 7. Process Object ---
  const processEmitter = new EventEmitter();
  const process = globalThis.process = {
    isMainFrame: injectedPaths.isMainFrame !== false,
    platform: 'win32',
    arch: 'x64',
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
      USERPROFILE: homeDir,
      HOME: homeDir,
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
    isMainFrame: true,
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
  const nativeModuleCache = new Map();
  const nativeHandleSymbol = Symbol('xenon.nativeHandle');
  const asyncNativeInstanceMethods = new Set();
  const asyncNativeExportMethods = new Set();
  let nextMojoRequestId = 1;
  let nextMojoCallbackId = 1;
  let nativeServiceGeneration = 0;

  function isPendingPromiseError(error) {
    const message = String(error && error.message ? error.message : error || '');
    return message.includes('pending Promise') ||
        message.includes('only settled Promise');
  }

  function callbackWire(callback) {
    const callbackId = nextMojoCallbackId++;
    mojoCallbacks.set(callbackId, callback);
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
    try {
      callback(...args.map(arg => adoptNativeReturn(arg)));
    } catch (error) {
      console.error('[Xenon Node Callback Error]', error);
    }
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
    };
  }

  function wireNativeArgumentSync(value, seen = new Map()) {
    const handle = getNativeHandle(value);
    if (handle) {
      let instanceId = handle.instanceId;
      if (!Number.isInteger(instanceId) ||
          handle.generation !== nativeServiceGeneration) {
        instanceId = reviveNativeHandle(handle);
      }
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
    if (seen.has(value)) {
      return seen.get(value);
    }
    if (Array.isArray(value)) {
      const result = [];
      seen.set(value, result);
      for (const item of value) {
        result.push(wireNativeArgumentSync(item, seen));
      }
      return result;
    }
    const result = {};
    seen.set(value, result);
    for (const [key, item] of Object.entries(value)) {
      result[key] = wireNativeArgumentSync(item, seen);
    }
    return result;
  }

  async function wireNativeArgumentAsync(value, seen = new Map()) {
    const handle = getNativeHandle(value);
    if (handle) {
      const instanceId = await resolvedHandleInstanceId(handle, false);
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
    if (seen.has(value)) {
      return seen.get(value);
    }
    if (Array.isArray(value)) {
      const result = [];
      seen.set(value, result);
      for (const item of value) {
        result.push(await wireNativeArgumentAsync(item, seen));
      }
      return result;
    }
    const result = {};
    seen.set(value, result);
    for (const [key, item] of Object.entries(value)) {
      result[key] = await wireNativeArgumentAsync(item, seen);
    }
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
        mojoCallbacks.set(callbackId, arg);
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
    const wired = [];
    for (const arg of args) {
      if (typeof arg === 'function') {
        const callbackId = nextMojoCallbackId++;
        mojoCallbacks.set(callbackId, arg);
        wired.push({
          isCallback: true,
          callbackId,
          value: {nullValue: 0},
        });
        continue;
      }
      wired.push({
        isCallback: false,
        callbackId: 0,
        value: valueToMojo(await wireNativeArgumentAsync(arg)),
      });
    }
    return wired;
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
          mojoCallbacks.get(cb.callbackId)?.(valueFromMojo(cb.value));
        }
      }
      pending.resolve(adoptNativeReturn(valueFromMojo(result)));
    });

    router.nodeCallbackInvoked.addListener((callbackId, args) => {
      const cb = mojoCallbacks.get(callbackId);
      if (cb) {
        try {
          cb(...args.map(arg => adoptNativeReturn(valueFromMojo(arg))));
        } catch (err) {
          console.error('[Xenon Node Callback Error]', err);
        }
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
        globalThis.__xenonPageHandler__ = mojoHandler;
        globalThis.__xenonPageCallbackRouter__ = mojoRouter;
        globalThis.__xenonPageHandlerBound__ = true;
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
    try {
      const handler = await ensureMojoBridge();
      if (!handler || typeof handler.openNativeFileDialog !== 'function') {
        console.warn('[Xenon Renderer] openNativeFileDialog is not bound');
        return [];
      }
      const result = await handler.openNativeFileDialog(title, exts, multi);
      if (Array.isArray(result)) {
        return result;
      }
      return (result && result.filePaths) || [];
    } catch (error) {
      console.error('[Xenon Renderer] openNativeFileDialog failed:', error);
      return [];
    }
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
    const handle = {
      modulePath: value.module_path || fallbackModulePath,
      className: '',
      ctorArgs: [],
      instanceId: Number(value.instance_id),
      generation: nativeServiceGeneration,
    };
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
    handle.proxy = proxy;
    return proxy;
  }

  function adoptNativeReturn(value, fallbackModulePath) {
    if (Array.isArray(value)) {
      return value.map(item => adoptNativeReturn(item, fallbackModulePath));
    }
    if (!value || typeof value !== 'object') {
      return value;
    }
    const wireType = value.__xenon_node_wire_type__;
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
        ctorArgs: [],
        instanceId: Number(value.instance_id),
        generation: nativeServiceGeneration,
      }, prototypeMembers, null, value.fields);
    }
    const result = {};
    for (const [key, item] of Object.entries(value)) {
      result[key] = adoptNativeReturn(item, fallbackModulePath);
    }
    return result;
  }

  function reviveNativeHandle(handle) {
    if (!handle || !handle.className) {
      throw new Error('Unknown instance id');
    }
    try {
      reloadNativeNodeModule(handle.modulePath);
    } catch (error) {
      if (!isDeadNativeServiceError(error)) {
        throw error;
      }
    }
    const args = (handle.ctorArgs || []).filter(arg => typeof arg !== 'function');
    handle.instanceId = transport.constructNodeExportSync(
        handle.modulePath, handle.className,
        ...args.map(arg => wireNativeArgumentSync(arg)));
    handle.generation = nativeServiceGeneration;
    return handle.instanceId;
  }

  function resolvedHandleInstanceId(handle, forceRevive) {
    return Promise.resolve(handle.instanceId).then((instanceId) => {
      if (typeof instanceId === 'number') {
        handle.instanceId = instanceId;
      }
      if (!forceRevive && typeof handle.instanceId === 'number' &&
          handle.generation === nativeServiceGeneration) {
        return handle.instanceId;
      }
      return reviveNativeHandle(handle);
    });
  }

  function invokeInstanceMethod(
      handle, methodName, methodArgs, didRevive) {
    const modulePath = handle.modulePath;
    const userCallbacks = methodArgs.filter(arg => typeof arg === 'function');
    const fireUserCallbacksOnce = (...cbArgs) => {
      if (fireUserCallbacksOnce.settled) {
        return;
      }
      fireUserCallbacksOnce.settled = true;
      for (const cb of userCallbacks) {
        try {
          cb(...cbArgs);
        } catch (error) {
          console.error('[Xenon Node Callback Error]', error);
        }
      }
    };
    if (methodName === 'waitLoadFinish' && userCallbacks.length) {
      globalThis.setTimeout(() => {
        if (!fireUserCallbacksOnce.settled) {
          console.warn(
              '[Xenon Renderer] waitLoadFinish timed out; continuing');
          fireUserCallbacksOnce();
        }
      }, 8000);
    }
    const wrappedArgs = userCallbacks.length ? methodArgs.map(arg => {
      if (typeof arg !== 'function') {
        return arg;
      }
      return (...cbArgs) => {
        fireUserCallbacksOnce.settled = true;
        return arg(...cbArgs);
      };
    }) : methodArgs;
    const instanceKey = `${modulePath}::${methodName}`;
    const runSync = (instanceId) => adoptNativeReturn(
        transport.invokeNodeInstanceSync(
            modulePath, instanceId, methodName,
            ...wrappedArgs.map(arg => typeof arg === 'function' ?
                                        arg :
                                        wireNativeArgumentSync(arg))),
        modulePath);
    if (typeof handle.instanceId === 'number' &&
        handle.generation === nativeServiceGeneration &&
        !argsHaveCallback(wrappedArgs) &&
        !asyncNativeInstanceMethods.has(instanceKey) &&
        typeof transport.invokeNodeInstanceSync === 'function') {
      try {
        return runSync(handle.instanceId);
      } catch (error) {
        if (isPendingPromiseError(error)) {
          asyncNativeInstanceMethods.add(instanceKey);
        } else if (!didRevive && isDeadNativeServiceError(error)) {
          try {
            return runSync(reviveNativeHandle(handle));
          } catch (retryError) {
            if (isPendingPromiseError(retryError)) {
              asyncNativeInstanceMethods.add(instanceKey);
            } else {
              console.warn(
                  '[Xenon Renderer] native instance method failed:', methodName,
                  retryError);
              return undefined;
            }
          }
        } else {
          console.warn(
              '[Xenon Renderer] native instance method failed:', methodName,
              error);
          return undefined;
        }
      }
    }
    return resolvedHandleInstanceId(handle, false).then((instanceId) => {
      return Promise.all(wrappedArgs.map(arg => wireNativeArgumentAsync(arg)))
          .then(wiredArgs => transport.invoke(
              '__xenon:node-addon:invoke-instance', {
                modulePath,
                instanceId,
                methodName,
                arguments: wiredArgs,
              }));
    }).then(result => adoptNativeReturn(result, modulePath)).catch((error) => {
      if (!didRevive && isDeadNativeServiceError(error)) {
        reviveNativeHandle(handle);
        return invokeInstanceMethod(handle, methodName, methodArgs, true);
      }
      console.warn(
          '[Xenon Renderer] native instance method failed:', methodName,
          error);
      throw error;
    });
  }

  function createNativeInstanceProxy(handle, prototypeMembers, proto, fields) {
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

    const proxy = new Proxy(baseInstance, {
      get(target, prop, receiver) {
        if (typeof prop === 'symbol' || prop in target) {
          return Reflect.get(target, prop, receiver);
        }
        if (typeof prop !== 'string' || !Number.isInteger(handle.instanceId) ||
            typeof transport.inspectNodeInstanceMemberSync !== 'function') {
          return undefined;
        }
        let instanceId = handle.instanceId;
        if (handle.generation !== nativeServiceGeneration) {
          try {
            instanceId = reviveNativeHandle(handle);
          } catch (error) {
            if (!isDeadNativeServiceError(error)) {
              throw error;
            }
            return undefined;
          }
        }
        let member = transport.inspectNodeInstanceMemberSync(
            handle.modulePath, instanceId, prop);
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
    handle.proxy = proxy;
    if (handle.instanceId && typeof handle.instanceId.then === 'function') {
      const pendingId = handle.instanceId;
      pendingId.then(instanceId => {
        if (handle.instanceId === pendingId && Number.isInteger(instanceId)) {
          handle.instanceId = instanceId;
        }
      }, () => {});
    }
    return proxy;
  }

  function createMojoExportFunction(modulePath, functionName) {
    return function(...args) {
      // Node addon calls without JavaScript callbacks are synchronous. Keep
      // the asynchronous observer path only for callback-bearing invocations.
      if (!argsHaveCallback(args)) {
        const exportKey = `${modulePath}::${functionName}`;
        const invokeSync = () => adoptNativeReturn(
            transport.invokeNodeExportSync(
                modulePath, functionName,
                ...args.map(arg => typeof arg === 'function' ?
                                       arg :
                                       wireNativeArgumentSync(arg))),
            modulePath);
        if (!asyncNativeExportMethods.has(exportKey)) {
          try {
            return invokeSync();
          } catch (error) {
            if (isPendingPromiseError(error)) {
              asyncNativeExportMethods.add(exportKey);
            } else if (isDeadNativeServiceError(error)) {
              try {
                reloadNativeNodeModule(modulePath);
                return invokeSync();
              } catch (retryError) {
                if (isPendingPromiseError(retryError)) {
                  asyncNativeExportMethods.add(exportKey);
                } else {
                  console.warn(
                      '[Xenon Renderer] native export failed after reload:',
                      functionName, retryError);
                  throw retryError;
                }
              }
            } else {
              console.warn(
                  '[Xenon Renderer] native export failed:', functionName, error);
              throw error;
            }
          }
        }
      }
      return Promise.all(args.map(arg => wireNativeArgumentAsync(arg)))
          .then(wiredArgs => transport.invoke(
              '__xenon:node-addon:invoke-export', {
                modulePath,
                functionName,
                arguments: wiredArgs,
              }))
          .then(result => adoptNativeReturn(result, modulePath));
    };
  }

  function buildExportFromMojoInfo(modulePath, item, parentPath = '') {
    const fullPath = parentPath ? `${parentPath}.${item.name}` : item.name;
    if (item.kind === 'function') {
      return createMojoExportFunction(modulePath, fullPath);
    }
    if (item.kind === 'class') {
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
    function ConstructorProxy(...args) {
      let instanceId;
      if (!argsHaveCallback(args) &&
          typeof transport.constructNodeExportSync === 'function') {
        instanceId = transport.constructNodeExportSync(
            modulePath, className,
            ...args.map(arg => typeof arg === 'function' ?
                                   arg :
                                   wireNativeArgumentSync(arg)));
      } else {
        instanceId = Promise.all(
            args.map(arg => wireNativeArgumentAsync(arg)))
            .then(wiredArgs => transport.invoke(
                '__xenon:node-addon:construct-export', {
                  modulePath,
                  exportPath: className,
                  arguments: wiredArgs,
                }));
      }
      return createNativeInstanceProxy({
        modulePath,
        className,
        ctorArgs: args.filter(arg => typeof arg !== 'function'),
        instanceId,
        generation: nativeServiceGeneration,
      }, info.prototype || [], ConstructorProxy.prototype);
    }

    return ConstructorProxy;
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

  function isDeadNativeServiceError(error) {
    const message = String(error && error.message ? error.message : error || '');
    return message.includes('Utility native module service is unavailable') ||
        message.includes('No Node addon has been loaded') ||
        message.includes('Synchronous native') ||
        message.includes('Native module load failed') ||
        message.includes('Unknown instance id') ||
        message.includes('Utility service restarted');
  }

  function reloadNativeNodeModule(normalizedPath) {
    nativeModuleCache.delete(normalizedPath);
    return loadNativeNodeModule(normalizedPath);
  }

  function loadNativeNodeModule(normalizedPath) {
    const baseName = String(normalizedPath).split(/[/\\]/).pop().toLowerCase();
    if (baseName === 'node_sqlite3.node') {
      nativeModuleCache.set(normalizedPath, {
        path: normalizedPath,
        target: sqlite3Module,
        proxy: sqlite3Module,
        loadPromise: Promise.resolve(sqlite3Module),
      });
      return sqlite3Module;
    }

    let cached = nativeModuleCache.get(normalizedPath);
    if (cached) return cached.proxy;

    const target = {};
    let loadResolve, loadReject;
    const loadPromise = new Promise((resolve, reject) => {
      loadResolve = resolve;
      loadReject = reject;
    });

    const record = { path: normalizedPath, target, proxy: null, loadPromise };

    record.proxy = new Proxy(target, {
      get(obj, prop, receiver) {
        if (prop === 'then') return undefined;
        if (prop === '__xenonReady') return loadPromise;
        if (prop === '__esModule') return true;
        if (prop === 'default') return receiver;
        // The synchronous load below supplies the native export list. Missing
        // properties must stay undefined so optional feature detection works.
        return Reflect.get(obj, prop, receiver);
      }
    });

    nativeModuleCache.set(normalizedPath, record);

    try {
      const exportsList = transport.requireNodeModuleSync(normalizedPath);
      for (const item of (exportsList || [])) {
        target[item.name] = buildExportFromMojoInfo(normalizedPath, item);
      }
      loadResolve(record.proxy);
    } catch (error) {
      nativeModuleCache.delete(normalizedPath);
      loadReject(error);
      // require() reports the synchronous exception; mark the readiness
      // promise handled so it does not also produce an unhandled rejection.
      loadPromise.catch(() => {});
      throw error;
    }

    return record.proxy;
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
    if (wire.t === 'b64') {
      const raw = atob(String(wire.d || ''));
      const u8 = new Uint8Array(raw.length);
      for (let i = 0; i < raw.length; i++) {
        u8[i] = raw.charCodeAt(i);
      }
      return Buffer.from(u8);
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
    socket.emit('end');
    socket.emit('close');
  }

  function deliverNetBytes(socket, data) {
    const buf = Buffer.isBuffer(data) || data instanceof Uint8Array ?
        Buffer.from(data) : Buffer.from(String(data));
    socket.emit('data', buf);
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
        const cb = typeof args[args.length - 1] === 'function' ?
            args[args.length - 1] : null;
        this._connectCb = cb;
        this.connecting = true;
        const path = normalizeNetPath(netPathFromListenOrConnect(args));
        allocNetSocket(this);
        const server = netServers.get(path);
        if (server) {
          const incoming = new netModule.Socket();
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
        if (this._nativeId) {
          this._closeCb = typeof cb === 'function' ? cb : null;
          sendXenonNet('__xenon:net:unlisten', {serverId: this._nativeId});
          return this;
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
        server.emit('listening');
        if (server._listenCb) {
          server._listenCb();
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
      incoming._id = msg.socketId;
      incoming._peerId = msg.socketId;
      incoming._connected = true;
      netSockets.set(incoming._id, incoming);
      server.emit('connection', incoming);
      return true;
    }
    if (channel === '__xenon:net:server-closed') {
      const server = nativeNetServers.get(msg.serverId);
      if (server) {
        nativeNetServers.delete(msg.serverId);
        server._nativeId = null;
        if (server._closeCb) {
          server._closeCb();
          server._closeCb = null;
        }
        server.emit('close');
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
      incoming._connected = true;
      incoming._peerId = msg.fromId;
      allocNetSocket(incoming);
      // Match net.Server ordering: install connection/data listeners before
      // the peer observes its connect event.
      server.emit('connection', incoming);
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
      socket.emit('connect');
      if (socket._connectCb) {
        socket._connectCb();
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
          server.emit('error', err);
        }
        return true;
      }
      const socket = netSockets.get(msg.toId);
      if (socket) {
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

  function Stream() {
    EventEmitter.call(this);
  }
  Object.setPrototypeOf(Stream.prototype, EventEmitter.prototype);
  Object.setPrototypeOf(Stream, EventEmitter);

  function Readable(options) {
    Stream.call(this);
    if (options) {
      if (typeof options.read === 'function') this._read = options.read;
      if (typeof options.destroy === 'function') this._destroy = options.destroy;
    }
  }
  Object.setPrototypeOf(Readable.prototype, Stream.prototype);
  Object.setPrototypeOf(Readable, Stream);
  Readable.prototype.pipe = function(dest) { return dest; };
  Readable.prototype.read = function() { return null; };

  function Writable(options) {
    Stream.call(this);
    if (options && typeof options.write === 'function') {
      this._write = options.write;
    }
  }
  Object.setPrototypeOf(Writable.prototype, Stream.prototype);
  Object.setPrototypeOf(Writable, Stream);
  Writable.prototype.write = function() { return true; };
  Writable.prototype.end = function() {};

  function Duplex(options) {
    Readable.call(this, options);
  }
  Object.setPrototypeOf(Duplex.prototype, Readable.prototype);
  Object.setPrototypeOf(Duplex, Readable);
  Duplex.prototype.write = function() { return true; };
  Duplex.prototype.end = function() {};

  function Transform(options) {
    Duplex.call(this, options);
  }
  Object.setPrototypeOf(Transform.prototype, Duplex.prototype);
  Object.setPrototypeOf(Transform, Duplex);

  function PassThrough(options) {
    Transform.call(this, options);
  }
  Object.setPrototypeOf(PassThrough.prototype, Transform.prototype);
  Object.setPrototypeOf(PassThrough, Transform);

  // Node's `require('stream')` is the Stream constructor with Readable/etc.
  // attached as properties. Hosted winston/readable-stream call
  // `inherits(X, require('stream'))` / read `.prototype` on that export.
  Stream.EventEmitter = EventEmitter;
  Stream.Stream = Stream;
  Stream.Readable = Readable;
  Stream.Writable = Writable;
  Stream.Duplex = Duplex;
  Stream.Transform = Transform;
  Stream.PassThrough = PassThrough;
  Stream.pipeline = (...args) => {
    const cb = typeof args[args.length - 1] === 'function' ? args[args.length - 1] : null;
    if (cb) cb(null);
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

  function createHashObject(algorithm, initialChunks) {
    const chunks = initialChunks ? initialChunks.slice() : [];
    return {
      update(data, encoding) {
        chunks.push(typeof data === 'string' ?
                    Buffer.from(data, encoding || 'utf8') :
                    Buffer.from(data));
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

  const cryptoModule = {
    randomBytes: (size) => {
      const buf = Buffer.alloc(size || 0);
      if (globalThis.crypto && globalThis.crypto.getRandomValues) {
        globalThis.crypto.getRandomValues(buf);
      }
      return buf;
    },
    randomFillSync: (buf) => {
      if (globalThis.crypto && globalThis.crypto.getRandomValues && buf) {
        globalThis.crypto.getRandomValues(buf);
      }
      return buf;
    },
    randomFill: (buf, cb) => {
      if (globalThis.crypto && globalThis.crypto.getRandomValues && buf) {
        globalThis.crypto.getRandomValues(buf);
      }
      if (typeof cb === 'function') cb(null, buf);
    },
    createCipheriv: (algorithm, key, iv) =>
      createCipherObject(algorithm, key, iv, true),
    createDecipheriv: (algorithm, key, iv) =>
      createCipherObject(algorithm, key, iv, false),
    createHash: (algorithm) => createHashObject(algorithm),
    createHmac: (algorithm, key) => {
      const chunks = [];
      return {
        update(data, encoding) {
          chunks.push(typeof data === 'string' ?
                      Buffer.from(data, encoding || 'utf8') :
                      Buffer.from(data));
          return this;
        },
        digest(encoding) {
          const digestBytes = hmacBytes(
              algorithm, key, Buffer.concat(chunks));
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
  const previousRequire =
      typeof globalThis.require === 'function' ? globalThis.require : null;

  const hostedCjsCache = new Map();
  let hostedCjsLoadingFile = '';

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
          const mapped = (stack || []).map((site) => ({
            getFileName: () => {
              try {
                return site.getFileName() || fallback;
              } catch (_e) {
                return fallback;
              }
            },
            getLineNumber: () => {
              try {
                return site.getLineNumber();
              } catch (_e) {
                return 1;
              }
            },
            getColumnNumber: () => {
              try {
                return site.getColumnNumber();
              } catch (_e) {
                return 1;
              }
            },
            getFunctionName: () => {
              try {
                return site.getFunctionName();
              } catch (_e) {
                return '';
              }
            },
            getEvalOrigin: () => fallback,
            isNative: () => false,
            toString: () => String(fallback || ''),
          }));
          return fn(err, mapped);
        };
        wrappedFormatters.add(innerPST);
      },
    });
  }

  function resolveHostedCjs(request) {
    if (typeof request !== 'string') {
      return '';
    }
    if (request.indexOf('.') < 0 && request.indexOf('/') < 0 &&
        request.indexOf('\\') < 0) {
      return '';
    }
    let resolved = pathModule.normalize(normalizeFsPath(request));
    if (!pathModule.isAbsolute(resolved)) {
      return '';
    }
    const lower = resolved.toLowerCase();
    const roots = [exeDir, appPath].filter(Boolean).map(
        root => pathModule.normalize(normalizeFsPath(root)).toLowerCase());
    if (!roots.some(root => lower === root || lower.startsWith(root + '\\'))) {
      return '';
    }
    const tryPaths = [];
    if (/\.(js|json|cjs)$/i.test(resolved)) {
      tryPaths.push(resolved);
    } else {
      tryPaths.push(
          resolved + '.js', pathModule.join(resolved, 'index.js'),
          resolved + '.json');
    }
    for (const candidate of tryPaths) {
      try {
        if (fsModule.existsSync(candidate) ||
            fsNativeSync('exists', candidate)) {
          return candidate;
        }
      } catch (_e) {
      }
    }
    return '';
  }

  function loadHostedCjsModule(request) {
    const resolved = resolveHostedCjs(request);
    if (!resolved) {
      return undefined;
    }
    if (hostedCjsCache.has(resolved)) {
      return hostedCjsCache.get(resolved).exports;
    }
    if (/\.json$/i.test(resolved)) {
      const parsed = JSON.parse(readHostedCjsSource(resolved));
      hostedCjsCache.set(resolved, {exports: parsed});
      return parsed;
    }
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
    hostedCjsCache.set(resolved, moduleObject);
    const previousLoading = hostedCjsLoadingFile;
    hostedCjsLoadingFile = resolved;
    try {
      const code = readHostedCjsSource(resolved);
      // Bind relative requires to this module, including requires called by
      // exported functions after the module's initial evaluation has finished.
      const moduleRequire = request => electronRequire(
          typeof request === 'string' && /^\.\.?([\\/]|$)/.test(request) ?
              pathModule.resolve(pathModule.dirname(resolved), request) : request);
      moduleRequire.resolve = request => resolveHostedCjs(
          pathModule.resolve(pathModule.dirname(resolved), request)) || request;
      moduleObject.require = moduleRequire;
      const fn = new Function(
          'exports', 'require', 'module', '__filename', '__dirname', 'process', 'Buffer',
          code + '\n//# sourceURL=' + resolved.replace(/\\/g, '/'));
      fn.call(moduleObject.exports, moduleObject.exports, moduleRequire, moduleObject, resolved,
          pathModule.dirname(resolved), process, Buffer);
      moduleObject.loaded = true;
    } catch (error) {
      hostedCjsCache.delete(resolved);
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

  const electronRequire = globalThis.require = function(request) {
    if (typeof request !== 'string') {
      throw new TypeError('Module name must be a string');
    }

    const norm = request.replace(/\\/g, '/').toLowerCase();

    // Electron
    if (norm === 'electron' || norm === 'node:electron') {
      return electron;
    }

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
      return {
        connect: () => new netModule.Socket(),
        createServer: () => new EventEmitter(),
      };
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
        const bodyChunks = [];
        let finished = false;
        let timeoutId = null;
        let aborted = false;
        const options = typeof opt === 'string' ? {href: opt} : (opt || {});

        req.write = (chunk) => {
          if (chunk == null) {
            return true;
          }
          if (typeof chunk === 'string') {
            bodyChunks.push(chunk);
          } else if (chunk instanceof Uint8Array || Buffer.isBuffer(chunk)) {
            bodyChunks.push(Buffer.from(chunk));
          } else {
            bodyChunks.push(String(chunk));
          }
          return true;
        };
        req.setTimeout = (ms, onTimeout) => {
          if (timeoutId) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
          const delay = Number(ms);
          if (!(delay > 0)) {
            return req;
          }
          timeoutId = setTimeout(() => {
            aborted = true;
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
          aborted = true;
          if (timeoutId) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
          return req;
        };
        req.destroy = (err) => {
          aborted = true;
          if (timeoutId) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
          if (err) {
            req.emit('error', err);
          }
          return req;
        };
        req.end = (chunk) => {
          if (finished) {
            return req;
          }
          finished = true;
          if (chunk != null) {
            req.write(chunk);
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
                    typeof part === 'string' ? Buffer.from(part) :
                    Buffer.from(part)));
              }
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
              if (typeof cb === 'function') {
                cb(incoming);
              }
              queueMicrotask(() => {
                if (responseBody.length) {
                  incoming.emit('data', responseEncoding ?
                      responseBody.toString(responseEncoding) : responseBody);
                }
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
        createServer: () => new EventEmitter(),
        Agent: class {},
      };
    }

    // Child Process
    if (norm === 'child_process' || norm === 'node:child_process') {
      return {
        exec: (_cmd, _opt, cb) => { if (typeof cb === 'function') cb(null, '', ''); },
        execSync: () => '',
        spawn: () => new EventEmitter(),
        fork: () => new EventEmitter(),
      };
    }

    // Zlib
    if (norm === 'zlib' || norm === 'node:zlib') {
      return {
        createGzip: () => new EventEmitter(),
        createGunzip: () => new EventEmitter(),
        gzip: (_buf, cb) => { if (typeof cb === 'function') cb(null, Buffer.alloc(0)); },
        gunzip: (_buf, cb) => { if (typeof cb === 'function') cb(null, Buffer.alloc(0)); },
      };
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
      return {
        createInterface: () => ({
          on: () => {},
          close: () => {},
          question: (_q, cb) => { if (typeof cb === 'function') cb(''); },
        }),
      };
    }

    // FS
    if (norm === 'fs' || norm === 'node:fs' || norm === 'fs/promises' || norm === 'node:fs/promises') {
      return norm.includes('promises') ? fsModule.promises : fsModule;
    }

    if (norm === 'sqlite3' || norm.endsWith('/sqlite3')) {
      return sqlite3Module;
    }

    // Native Node-API Addon (*.node)
    if (norm.endsWith('.node')) {
      const baseName = String(request).split(/[/\\]/).pop();
      if (baseName.toLowerCase() === 'node_sqlite3.node') {
        return sqlite3Module;
      }
      // Keep qualified native module paths qualified. Different Electron apps
      // may intentionally ship same-named addons with different ABIs or SDK
      // versions, so the canonical path (not the basename) is module identity.
      const requestedPath = normalizeFsPath(request);
      const isQualifiedPath = /[/\\]/.test(request);
      let targetPath = isQualifiedPath ? requestedPath : baseName;
      // Application roots are intentionally virtualized for normal renderer
      // fs access. Native loading is different: addons must be unpacked real
      // files. Honor an existing qualified path exactly. For a missing or
      // unqualified path, resolve within the current application before using
      // the host executable directory.
      let found = isQualifiedPath &&
          Boolean(fsNativeSync('exists', requestedPath));
      const appCandidates = [
        appPath + '\\' + baseName,
        appPath + '\\build\\Release\\' + baseName,
        appPath + '\\Release\\' + baseName,
      ];
      if (!found) {
        for (const cand of appCandidates) {
          if (Boolean(fsNativeSync('exists', cand))) {
            targetPath = cand;
            found = true;
            break;
          }
        }
      }
      if (!found && Boolean(fsNativeSync('exists', exeDir + '\\' + baseName))) {
        targetPath = exeDir + '\\' + baseName;
        found = true;
      }
      if (!found) {
        const error = new Error(`Cannot find module '${request}'`);
        error.code = 'MODULE_NOT_FOUND';
        throw error;
      }
      return loadNativeNodeModule(targetPath);
    }

    const hostedCjsPath = resolveHostedCjs(request);
    if (hostedCjsPath) {
      return loadHostedCjsModule(hostedCjsPath);
    }

    if (previousRequire) {
      return previousRequire.apply(this, arguments);
    }

    throw new Error(`Cannot find module '${String(request)}'`);
  };

  globalThis.Buffer = Buffer;
  globalThis.require.resolve = path => path;
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
    }
  }
})();
