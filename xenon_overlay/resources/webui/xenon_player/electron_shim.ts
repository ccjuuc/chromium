// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* Electron / Node polyfill for chrome://xenon-player — intentionally loose. */
/* eslint-disable @typescript-eslint/require-await */
/* eslint-disable @typescript-eslint/no-unnecessary-type-conversion */
/* eslint-disable @typescript-eslint/no-unnecessary-type-assertion */
/* eslint-disable @typescript-eslint/no-explicit-any */

/**
 * Electron / Node compatibility surface for the xmp_xdas_2 renderer bundle.
 * Native `.node` modules are preloaded through Mojo before the synchronous
 * Electron require function is exposed.
 */

import {bindPlayerVideoWindow, controlPlayerWindow, preparePlayerHost, showPlayerVideoHost, whenRequired} from './require.js';
import {AnyFn, createSmartIpcObject, openNativeFileDialog, registeredRpcFunctions} from './services/net_ipc.js';
import {
  activePlayList,
  activePlayListManager,
  registerPlaylistRpcFallbacks,
} from './services/playlist_service.js';
import {
  NativeAplayerStack,
  fileNameByBlobUrl,
  realPcAddon,
  registerPlayerRpcFunctions,
  setCurrentPlayingUrl,
  setRealPcAddon,
} from './services/player_service.js';

const realNativeAddons = new Map<string, any>();

type DataUrlPayload = {
  mimeType: string;
  bytes: Uint8Array;
  text: string;
};

function decodeDataUrl(url: string): DataUrlPayload {
  const comma = url.indexOf(',');
  if (!url.startsWith('data:') || comma < 0) {
    throw new TypeError('Invalid data URL');
  }

  const metadata = url.slice(5, comma);
  const encoded = url.slice(comma + 1);
  const mimeType = metadata.split(';')[0] || 'text/plain';
  let bytes: Uint8Array;
  if (metadata.split(';').includes('base64')) {
    const binary = atob(encoded);
    bytes = Uint8Array.from(binary, character => character.charCodeAt(0));
  } else {
    bytes = new TextEncoder().encode(decodeURIComponent(encoded));
  }
  return {
    mimeType,
    bytes,
    text: new TextDecoder().decode(bytes),
  };
}

function installDataUrlTransport() {
  const nativeFetch = window.fetch.bind(window);
  window.fetch = ((input: RequestInfo|URL, init?: RequestInit) => {
    const url = input instanceof Request ? input.url : String(input);
    if (!url.startsWith('data:')) {
      return nativeFetch(input, init);
    }
    try {
      const payload = decodeDataUrl(url);
      const body = payload.bytes.slice().buffer as ArrayBuffer;
      return Promise.resolve(new Response(body, {
        status: 200,
        headers: {'content-type': payload.mimeType},
      }));
    } catch (error) {
      return Promise.reject(error);
    }
  }) as typeof window.fetch;

  const NativeXMLHttpRequest = window.XMLHttpRequest;
  window.XMLHttpRequest = new Proxy(NativeXMLHttpRequest, {
    construct(target, args) {
      const xhr = Reflect.construct(target, args) as XMLHttpRequest;
      let dataUrl: string|null = null;
      let readyState: number = XMLHttpRequest.UNSENT;
      let status = 0;
      let responseType: XMLHttpRequestResponseType = '';
      let responseText = '';
      let response: unknown = null;
      let timeout = 0;
      let withCredentials = false;

      const emit = (type: string) => {
        xhr.dispatchEvent(new ProgressEvent(type));
      };
      const proxy = new Proxy(xhr, {
        get(native, property) {
          if (dataUrl) {
            switch (property) {
              case 'readyState':
                return readyState;
              case 'status':
                return status;
              case 'statusText':
                return status === 200 ? 'OK' : '';
              case 'responseURL':
                return dataUrl;
              case 'responseType':
                return responseType;
              case 'responseText':
                return responseText;
              case 'response':
                return response;
              case 'timeout':
                return timeout;
              case 'withCredentials':
                return withCredentials;
            }
          }

          if (property === 'open') {
            return (method: string, url: string|URL, ...openArgs: unknown[]) => {
              const requestedUrl = String(url);
              if (requestedUrl.startsWith('data:')) {
                dataUrl = requestedUrl;
                readyState = XMLHttpRequest.OPENED;
                emit('readystatechange');
                return;
              }
              dataUrl = null;
              return (native.open as AnyFn)(method, requestedUrl, ...openArgs);
            };
          }
          if (property === 'send') {
            return (body?: Document|XMLHttpRequestBodyInit|null) => {
              if (!dataUrl) {
                return native.send(body);
              }
              queueMicrotask(() => {
                try {
                  const payload = decodeDataUrl(dataUrl!);
                  status = 200;
                  responseText = payload.text;
                  switch (responseType) {
                    case 'arraybuffer':
                      response = payload.bytes.slice().buffer as ArrayBuffer;
                      break;
                    case 'blob': {
                      const body =
                          payload.bytes.slice().buffer as ArrayBuffer;
                      response = new Blob([body], {
                        type: payload.mimeType,
                      });
                      break;
                    }
                    case 'json':
                      response = JSON.parse(payload.text);
                      break;
                    default:
                      response = payload.text;
                      break;
                  }
                  readyState = XMLHttpRequest.DONE;
                  emit('readystatechange');
                  emit('load');
                } catch {
                  status = 0;
                  readyState = XMLHttpRequest.DONE;
                  emit('readystatechange');
                  emit('error');
                }
                emit('loadend');
              });
            };
          }
          if (property === 'abort') {
            return () => {
              if (!dataUrl) {
                return native.abort();
              }
              dataUrl = null;
              readyState = XMLHttpRequest.UNSENT;
              emit('abort');
              emit('loadend');
            };
          }
          if (property === 'setRequestHeader') {
            return (name: string, value: string) => {
              if (!dataUrl) {
                native.setRequestHeader(name, value);
              }
            };
          }
          if (property === 'getResponseHeader') {
            return (name: string) => dataUrl &&
                    name.toLowerCase() === 'content-type' ?
                decodeDataUrl(dataUrl).mimeType :
                native.getResponseHeader(name);
          }
          if (property === 'getAllResponseHeaders') {
            return () => dataUrl ?
                `content-type: ${decodeDataUrl(dataUrl).mimeType}\r\n` :
                native.getAllResponseHeaders();
          }

          const value = Reflect.get(native, property, native);
          return typeof value === 'function' ? value.bind(native) : value;
        },
        set(native, property, value) {
          if (dataUrl) {
            if (property === 'responseType') {
              responseType = value as XMLHttpRequestResponseType;
              return true;
            }
            if (property === 'timeout') {
              timeout = Number(value);
              return true;
            }
            if (property === 'withCredentials') {
              withCredentials = Boolean(value);
              return true;
            }
          }
          return Reflect.set(native, property, value, native);
        },
      });
      return proxy;
    },
  }) as typeof XMLHttpRequest;
}

function nativeAddonKey(moduleId: string): string {
  return basenamePath(moduleId.replace(/\\/g, '/')).toLowerCase();
}

function createXmpHelperAdapter(addon: any) {
  const synchronousFallback = createSyncNativeAddon(
      'xmp_helper.node', false);
  return new Proxy(addon, {
    get(target, property, receiver) {
      if (Object.prototype.hasOwnProperty.call(
              synchronousFallback, property)) {
        return Reflect.get(synchronousFallback, property);
      }
      return Reflect.get(target, property, receiver);
    },
  });
}

function createDkAddonAdapter(addon: any) {
  const synchronousFallback = createSyncNativeAddon('dk_addon.node', false);
  return new Proxy(addon, {
    get(target, property, receiver) {
      if (Object.prototype.hasOwnProperty.call(
              synchronousFallback, property)) {
        return Reflect.get(synchronousFallback, property);
      }
      return Reflect.get(target, property, receiver);
    },
  });
}

async function registerRealNativeAddon(moduleId: string, addon: any) {
  const key = nativeAddonKey(moduleId);
  let exposedAddon = addon;
  if (key === 'pc_addon.node') {
    setRealPcAddon(addon);
    exposedAddon = createSyncNativeAddon('pc_addon.node', false);
  } else if (key === 'xmp_helper.node') {
    exposedAddon = createXmpHelperAdapter(addon);
  } else if (key === 'dk_addon.node') {
    exposedAddon = createDkAddonAdapter(addon);
  }
  realNativeAddons.set(key, exposedAddon);
  if (key === 'pc_addon.node') {
    realNativeAddons.set('playercontrol.node', exposedAddon);
  }
}

async function preloadRealNativeAddons(): Promise<void> {
  const requiredAddons = [
    'pc_addon.node',
    'xmp_helper.node',
    'player_helper.node',
    'dk_addon.node',
  ];
  for (const moduleId of requiredAddons) {
    const addon = await whenRequired(moduleId);
    await registerRealNativeAddon(moduleId, addon);
    console.info('[xenon-player] native addon ready:', moduleId);
  }
}

class SimpleEventEmitter {
  private listeners = new Map<string, Set<AnyFn>>();

  on(event: string, listener: AnyFn): this {
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(listener);
    return this;
  }

  once(event: string, listener: AnyFn): this {
    const wrap: AnyFn = (...args) => {
      this.off(event, wrap);
      listener(...args);
    };
    return this.on(event, wrap);
  }

  off(event: string, listener: AnyFn): this {
    this.listeners.get(event)?.delete(listener);
    return this;
  }

  removeListener(event: string, listener: AnyFn): this {
    return this.off(event, listener);
  }

  addListener(event: string, listener: AnyFn): this {
    return this.on(event, listener);
  }

  emit(event: string, ...args: unknown[]): boolean {
    const set = this.listeners.get(event);
    if (!set || set.size === 0) {
      return false;
    }
    for (const listener of [...set]) {
      listener(...args);
    }
    return true;
  }
}

function normalizeSlashes(p: string): string {
  return String(p ?? '').replace(/\\/g, '/').replace(/\/+/g, '/');
}

function joinPath(...parts: string[]): string {
  return normalizeSlashes(parts.filter(part => part && part.length > 0).join('/'));
}

/**
 * Win32-ish dirname that always terminates DirEntryManager.getPathTreeByDirPath's
 * `for (; path !== rootDir; ) path = dirname(path)` loop. Returning '' for drive
 * roots / bare names prevents infinite spins when rootDir is empty or separators
 * disagree.
 */
function dirnamePath(p: string): string {
  let normalized = normalizeSlashes(p);
  if (normalized.length > 3 && normalized.endsWith('/')) {
    normalized = normalized.slice(0, -1);
  }
  if (!normalized || normalized === '.') {
    return '';
  }
  // C: or C:/ → stop
  if (/^[a-zA-Z]:\/?$/.test(normalized)) {
    return '';
  }
  const idx = normalized.lastIndexOf('/');
  if (idx < 0) {
    return '';
  }
  if (idx === 0) {
    return '/';
  }
  // C:/foo → C:/
  if (/^[a-zA-Z]:\//.test(normalized) && idx === 2) {
    return normalized.slice(0, 3);
  }
  return normalized.slice(0, idx);
}

function basenamePath(p: string): string {
  if (p && fileNameByBlobUrl.has(p)) {
    return fileNameByBlobUrl.get(p)!;
  }
  const normalized = normalizeSlashes(p);
  if (normalized.endsWith('/') && normalized.length > 1) {
    return basenamePath(normalized.slice(0, -1));
  }
  const idx = normalized.lastIndexOf('/');
  return idx < 0 ? normalized : normalized.slice(idx + 1);
}

function extnamePath(p: string): string {
  const base = basenamePath(p);
  const idx = base.lastIndexOf('.');
  return idx <= 0 ? '' : base.slice(idx);
}

const pathApi = {
  join: joinPath,
  dirname: dirnamePath,
  basename: basenamePath,
  extname: extnamePath,
  sep: '/',
  delimiter: ';',
  resolve: (...parts: string[]) => joinPath(...parts),
  normalize: (p: string) => normalizeSlashes(p),
  isAbsolute: (p: string) => /^([a-zA-Z]:)?[\\/]/.test(p) || p.startsWith('/'),
  win32: null as unknown,
  posix: null as unknown,
};
pathApi.win32 = pathApi;
pathApi.posix = pathApi;

const dummyStats = {
  isFile: () => true,
  isDirectory: () => false,
  isBlockDevice: () => false,
  isCharacterDevice: () => false,
  isSymbolicLink: () => false,
  isFIFO: () => false,
  isSocket: () => false,
  size: 0,
  mtime: new Date(),
  ctime: new Date(),
  birthtime: new Date(),
};

const fsApi = {
  existsSync: (_path: string) => true,
  mkdirSync: (_path: string, _opts?: unknown) => undefined,
  readFileSync: (_path: string, _opts?: unknown) => '',
  writeFileSync: (_path: string, _data: unknown) => undefined,
  unlinkSync: (_path: string) => undefined,
  rmdirSync: (_path: string) => undefined,
  statSync: (_path: string) => dummyStats,
  lstatSync: (_path: string) => dummyStats,
  accessSync: (_path: string) => undefined,
  readdirSync: (_path: string) => [] as string[],
  realpathSync: (p: string) => p,
  readdir: (_path: string, cb?: AnyFn) => {
    queueMicrotask(() => cb?.(null, []));
  },
  readFile: (_path: string, _optsOrCb?: unknown, cb?: AnyFn) => {
    const callback = typeof _optsOrCb === 'function' ? _optsOrCb : cb;
    queueMicrotask(() => callback?.(null, ''));
  },
  writeFile: (_path: string, _data: unknown, _optsOrCb?: unknown, cb?: AnyFn) => {
    const callback = typeof _optsOrCb === 'function' ? _optsOrCb : cb;
    queueMicrotask(() => callback?.(null));
  },
  stat: (_path: string, cb?: AnyFn) => {
    queueMicrotask(() => cb?.(null, dummyStats));
  },
  lstat: (_path: string, cb?: AnyFn) => {
    queueMicrotask(() => cb?.(null, dummyStats));
  },
  access: (_path: string, _modeOrCb?: unknown, cb?: AnyFn) => {
    const callback = typeof _modeOrCb === 'function' ? _modeOrCb : cb;
    queueMicrotask(() => callback?.(null));
  },
  watch: (_path: string, _optsOrCb?: unknown, _cb?: AnyFn) => ({
    close: () => undefined,
    on: () => undefined,
  }),
  watchFile: (_path: string, _optsOrCb?: unknown, _cb?: AnyFn) => undefined,
  unwatchFile: (_path: string) => undefined,
  promises: {
    readFile: async () => '',
    writeFile: async () => undefined,
    mkdir: async () => undefined,
    access: async () => undefined,
    stat: async () => dummyStats,
    lstat: async () => dummyStats,
    readdir: async () => [] as string[],
    unlink: async () => undefined,
    rm: async () => undefined,
  },
  constants: {
    F_OK: 0,
    R_OK: 4,
    W_OK: 2,
    X_OK: 1,
  },
};

const cryptoApi = {
  createHash: (_algo: string) => {
    let data = '';
    const hasher = {
      update: (chunk: string|Uint8Array) => {
        if (typeof chunk === 'string') {
          data += chunk;
        } else if (chunk && chunk.length) {
          for (let i = 0; i < chunk.length; i++) {
            data += String.fromCharCode(chunk[i] ?? 0);
          }
        }
        return hasher;
      },
      digest: (_enc?: string) => {
        // Not cryptographically correct; only prevents boot crashes.
        let hash = 0;
        for (let i = 0; i < data.length; i++) {
          hash = ((hash << 5) - hash) + data.charCodeAt(i);
          hash |= 0;
        }
        return (hash >>> 0).toString(16).padStart(8, '0');
      },
    };
    return hasher;
  },
  randomBytes: (size: number) => {
    const buf = new Uint8Array(size);
    if (typeof window !== 'undefined' && window.crypto?.getRandomValues) {
      window.crypto.getRandomValues(buf as unknown as Uint8Array<ArrayBuffer>);
    }
    return decorateBuffer(buf);
  },
  randomFillSync: (buf: Uint8Array) => {
    if (typeof window !== 'undefined' && window.crypto?.getRandomValues) {
      window.crypto.getRandomValues(buf as unknown as Uint8Array<ArrayBuffer>);
    }
    return buf;
  },
};

/** Minimal Buffer stand-in so Electron bundles can call Buffer.from / alloc. */
type XenonBuffer = Uint8Array&{
  toString: (encoding?: string) => string;
  copy: (target: Uint8Array, targetStart?: number, sourceStart?: number,
         sourceEnd?: number) => number;
};

function bufferToString(buf: Uint8Array, encoding?: string): string {
  if (encoding === 'hex') {
    return Array.from(buf).map(b => b.toString(16).padStart(2, '0')).join('');
  }
  if (encoding === 'base64') {
    let binary = '';
    buf.forEach(b => binary += String.fromCharCode(b));
    return btoa(binary);
  }
  return new TextDecoder().decode(buf);
}

function decorateBuffer(bytes: Uint8Array): XenonBuffer {
  const buf = bytes as XenonBuffer;
  buf.toString = (encoding?: string) => bufferToString(buf, encoding);
  buf.copy = (target: Uint8Array, targetStart = 0, sourceStart = 0,
              sourceEnd = buf.length) => {
    const slice = buf.subarray(sourceStart, sourceEnd);
    target.set(slice, targetStart);
    return slice.length;
  };
  return buf;
}

const XenonBufferCtor = {
  from(value: ArrayBuffer|ArrayLike<number>|string|Uint8Array,
       encodingOrOffset?: string|number, length?: number): XenonBuffer {
    if (typeof value === 'string') {
      return decorateBuffer(new TextEncoder().encode(value));
    }
    if (value instanceof ArrayBuffer) {
      const offset =
          typeof encodingOrOffset === 'number' ? encodingOrOffset : 0;
      const len =
          typeof length === 'number' ? length : value.byteLength - offset;
      return decorateBuffer(new Uint8Array(value, offset, len));
    }
    if (ArrayBuffer.isView(value)) {
      return decorateBuffer(new Uint8Array(
          value.buffer, value.byteOffset, value.byteLength));
    }
    return decorateBuffer(Uint8Array.from(value as ArrayLike<number>));
  },
  alloc(size: number, fill?: number): XenonBuffer {
    const buf = decorateBuffer(new Uint8Array(size));
    if (fill !== undefined) {
      buf.fill(fill);
    }
    return buf;
  },
  allocUnsafe(size: number): XenonBuffer {
    return decorateBuffer(new Uint8Array(size));
  },
  isBuffer(value: unknown): value is XenonBuffer {
    return value instanceof Uint8Array;
  },
  concat(list: ArrayLike<Uint8Array>, totalLength?: number): XenonBuffer {
    const arr = Array.from(list);
    const len = totalLength ?? arr.reduce((n, b) => n + b.byteLength, 0);
    const out = new Uint8Array(len);
    let offset = 0;
    for (const part of arr) {
      out.set(part, offset);
      offset += part.byteLength;
    }
    return decorateBuffer(out);
  },
  byteLength(value: string, encoding?: string): number {
    if (encoding === 'hex') {
      return Math.ceil(String(value).length / 2);
    }
    if (encoding === 'base64') {
      return Math.floor(String(value).length * 0.75);
    }
    return new TextEncoder().encode(String(value)).length;
  },
  isEncoding(_enc?: string): boolean {
    return true;
  },
  equals(a: Uint8Array, b: Uint8Array): boolean {
    if (a.length !== b.length) {
      return false;
    }
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) {
        return false;
      }
    }
    return true;
  },
};

const bufferModule = {Buffer: XenonBufferCtor, kMaxLength: 0x7fffffff};

class StringDecoder {
  encoding: string;
  constructor(encoding = 'utf8') {
    this.encoding = encoding;
  }
  write(buffer: Uint8Array): string {
    return new TextDecoder().decode(buffer);
  }
  end(buffer?: Uint8Array): string {
    return buffer ? this.write(buffer) : '';
  }
}

const stringDecoderModule = {StringDecoder};

class NetSocket extends SimpleEventEmitter {
  connecting = false;
  destroyed = false;
  remoteAddress = '127.0.0.1';
  remotePort = 0;

  connect(_pathOrPort?: unknown, _hostOrListener?: unknown, listener?: AnyFn) {
    const cb = typeof _hostOrListener === 'function' ? _hostOrListener :
                                                         listener;
    this.connecting = true;
    queueMicrotask(() => {
      this.connecting = false;
      this.emit('connect');
      if (typeof cb === 'function') {
        cb();
      }
    });
    return this;
  }
  write(_chunk?: unknown, _encoding?: unknown, cb?: AnyFn) {
    if (typeof cb === 'function') {
      queueMicrotask(() => cb());
    }
    return true;
  }
  end(cb?: AnyFn) {
    queueMicrotask(() => {
      this.emit('end');
      this.emit('close');
      if (typeof cb === 'function') {
        cb();
      }
    });
    return this;
  }
  destroy() {
    this.destroyed = true;
    this.emit('close');
    return this;
  }
  setNoDelay() {
    return this;
  }
  setTimeout() {
    return this;
  }
  setKeepAlive() {
    return this;
  }
  ref() {
    return this;
  }
  unref() {
    return this;
  }
}

const netModule = {
  Socket: NetSocket,
  connect: (...args: unknown[]) => {
    const socket = new NetSocket();
    socket.connect(...args);
    return socket;
  },
  createConnection: (...args: unknown[]) => {
    const socket = new NetSocket();
    socket.connect(...args);
    return socket;
  },
  createServer: (connectionListener?: AnyFn) => {
    const server = new SimpleEventEmitter() as SimpleEventEmitter&{
      listen: (...args: unknown[]) => unknown;
      close: (cb?: AnyFn) => unknown;
      address: () => {port: number; address: string};
    };
    server.listen = (...listenArgs: unknown[]) => {
      const cb = listenArgs.find(a => typeof a === 'function') as AnyFn |
          undefined;
      queueMicrotask(() => {
        server.emit('listening');
        cb?.();
        if (connectionListener) {
          const client = new NetSocket();
          queueMicrotask(() => connectionListener(client));
        }
      });
      return server;
    };
    server.close = (cb?: AnyFn) => {
      queueMicrotask(() => {
        server.emit('close');
        cb?.();
      });
      return server;
    };
    server.address = () => ({port: 0, address: '127.0.0.1'});
    return server;
  },
  isIP: () => 0,
  isIPv4: () => false,
  isIPv6: () => false,
};

// --- Standard Electron Modules Implementation ---

const ipcHandlers: Record<string, AnyFn> = {};

const PLAYER_TRANSPARENT_STYLE_ID = 'xenon-player-transparent-style';

function ensureTransparentPlayerSurface() {
  if (document.getElementById(PLAYER_TRANSPARENT_STYLE_ID)) {
    return;
  }
  const style = document.createElement('style');
  style.id = PLAYER_TRANSPARENT_STYLE_ID;
  style.textContent = `
    html, body, #root, .app-container, .xmp-player-container,
    .xmp-player, .player-screen-container {
      background: transparent !important;
      background-color: transparent !important;
    }
    .xmp-player.is-audio, .xmp-player.is-media-loading,
    .xmp-player.is-media-delivering {
      background: transparent !important;
    }
  `;
  document.head.appendChild(style);
}

ensureTransparentPlayerSurface();

const ipcMain = Object.assign(new SimpleEventEmitter(), {
  handle: (channel: string, listener: AnyFn) => {
    ipcHandlers[channel] = listener;
  },
  handleOnce: (channel: string, listener: AnyFn) => {
    ipcHandlers[channel] = async (event: unknown, ...args: unknown[]) => {
      delete ipcHandlers[channel];
      return listener(event, ...args);
    };
  },
  removeHandler: (channel: string) => {
    delete ipcHandlers[channel];
  },
});

function emitRendererWindowEvent(channel: string, ...args: unknown[]): void {
  ipcRenderer.emit(channel, {sender: ipcRenderer}, ...args);
}

function handlePlayerWindowCommand(
    channel: string, args: unknown[]): boolean {
  let action = '';
  let flag = false;
  switch (channel) {
    case 'window:minizeWindow':
      action = 'minimize';
      break;
    case 'window:maxWindow':
      action = 'toggle-maximize';
      break;
    case 'window:closeWindow':
      action = 'close';
      break;
    case 'window:hideWindow':
      action = 'hide';
      break;
    case 'window:showWindow':
      action = 'show';
      break;
    case 'window:focusWindow':
    case 'window:bringCurrentWinToTop':
      action = 'focus';
      break;
    case 'window:fullscreen':
      action = 'fullscreen';
      flag = true;
      break;
    case 'window:fullscreenOff':
      action = 'fullscreen';
      break;
    case 'window:pin':
      action = 'pin';
      flag = Boolean(args[0]);
      break;
    default:
      return false;
  }

  controlPlayerWindow(action, flag)
      .then(state => {
        switch (action) {
          case 'minimize':
            emitRendererWindowEvent('window:onMinimize');
            break;
          case 'toggle-maximize':
            emitRendererWindowEvent(
                state ? 'window:onMaximize' : 'window:onRestore', state);
            break;
          case 'hide':
            emitRendererWindowEvent('window:onHide');
            break;
          case 'show':
            emitRendererWindowEvent('window:onShow');
            break;
          case 'focus':
            emitRendererWindowEvent('window:onFocus');
            break;
          case 'fullscreen':
            emitRendererWindowEvent('window:onFullscreen', state);
            break;
        }
      })
      .catch(error => console.warn(
          `[xenon-player] ${channel} native window command failed:`, error));
  return true;
}

const ipcRenderer = Object.assign(new SimpleEventEmitter(), {
  send: (channel: string, ...args: unknown[]) => {
    console.info('[xenon-player ipcRenderer.send]', channel, ...args);
    if (channel === 'AplayerWndShow') {
      window.dispatchEvent(new CustomEvent('xenon-player-wnd-show'));
      showPlayerVideoHost(true).catch(e => console.warn('[xenon-player] showPlayerVideoHost(true) error:', e));
      const home = document.querySelector<HTMLElement>('.home-container');
      if (home) home.style.display = 'none';
      const player = document.querySelector<HTMLElement>('.xmp-player-container');
      if (player) player.style.display = 'block';
      ensureTransparentPlayerSurface();
    } else if (channel === 'AplayerWndBind') {
      const playerWindow = Number(args[0]);
      const handles = window.__xenonPlayerHandles__;
      if (!handles || !Number.isSafeInteger(playerWindow) ||
          playerWindow <= 0) {
        console.warn('[xenon-player] cannot bind native player window', {
          handles,
          playerWindow,
        });
      } else {
        bindPlayerVideoWindow(playerWindow).catch(error => console.warn(
            '[xenon-player] bindPlayerVideoWindow error:', error));
      }
    } else if (channel === 'AplayerWndHide') {
      showPlayerVideoHost(false).catch(e => console.warn('[xenon-player] showPlayerVideoHost(false) error:', e));
      const home = document.querySelector<HTMLElement>('.home-container');
      if (home) home.style.display = 'flex';
    } else {
      handlePlayerWindowCommand(channel, args);
    }
    const fakeEvent = { sender: ipcRenderer };
    ipcMain.emit(channel, fakeEvent, ...args);
    if (channel === 'ipc-renderer-ready') {
      // The original Electron main process acknowledges this once its IPC
      // client is ready. Cache-backed settings (including currVolume) wait on
      // that signal before every get/set, so omitting it leaves the player at
      // its intentional bootstrap volume of zero and also blocks slider input.
      queueMicrotask(() => ipcRenderer.emit(
          'ipc-process-client-ready', {sender: ipcRenderer}));
    }
  },
  sendSync: (channel: string, ...args: unknown[]) => {
    console.info('[xenon-player ipcRenderer.sendSync]', channel, ...args);
    const fakeEvent = { sender: ipcRenderer, returnValue: null as unknown };
    ipcMain.emit(channel, fakeEvent, ...args);
    return fakeEvent.returnValue;
  },
  invoke: async (channel: string, ...args: unknown[]) => {
    console.info('[xenon-player ipcRenderer.invoke]', channel, ...args);
    if (typeof ipcHandlers[channel] === 'function') {
      const fakeEvent = { sender: ipcRenderer };
      return ipcHandlers[channel](fakeEvent, ...args);
    }
    return null;
  },
  postMessage: (channel: string, message: unknown, _transfer?: unknown[]) => {
    ipcRenderer.emit(channel, { sender: ipcRenderer, data: message }, message);
  },
});

class WebContents extends SimpleEventEmitter {
  id = 1;
  getUserAgent = () => navigator.userAgent;
  setUserAgent = (_ua: string) => {};
  openDevTools = (_options?: unknown) => {};
  closeDevTools = () => {};
  isDevToolsOpened = () => false;
  send = (channel: string, ...args: unknown[]) => {
    ipcRenderer.emit(channel, { sender: this }, ...args);
  };
  reload = () => window.location.reload();
  getURL = () => window.location.href;
  loadURL = async (url: string) => { window.location.href = url; };
  setWindowOpenHandler = () => {};
}

class BrowserWindow extends SimpleEventEmitter {
  id = 1;
  webContents = new WebContents();
  private _visible = true;
  private _isMinimized = false;
  private _isMaximized = false;
  private _alwaysOnTop = false;
  private _title = document.title || 'Xenon Player';

  constructor(_options?: unknown) {
    super();
  }

  static getAllWindows = () => [browserWindowInstance];
  static getFocusedWindow = () => browserWindowInstance;
  static fromId = (_id: number) => browserWindowInstance;
  static fromWebContents = (_wc: unknown) => browserWindowInstance;

  loadURL = async (url: string) => { window.location.href = url; };
  getURL = () => window.location.href;
  show = () => { this._visible = true; this.emit('show'); };
  hide = () => { this._visible = false; this.emit('hide'); };
  close = () => { this.emit('close'); };
  destroy = () => { this.emit('closed'); };
  isDestroyed = () => false;
  isVisible = () => this._visible;
  focus = () => { window.focus(); this.emit('focus'); };
  blur = () => { window.blur(); this.emit('blur'); };
  isFocused = () => document.hasFocus();
  minimize = () => { this._isMinimized = true; this.emit('minimize'); };
  restore = () => { this._isMinimized = false; this.emit('restore'); };
  isMinimized = () => this._isMinimized;
  maximize = () => { this._isMaximized = true; this.emit('maximize'); };
  unmaximize = () => { this._isMaximized = false; this.emit('unmaximize'); };
  isMaximized = () => this._isMaximized;
  setFullScreen = (flag: boolean) => {
    if (flag) {
      document.documentElement.requestFullscreen?.().catch(() => {});
      this.emit('enter-full-screen');
    } else {
      document.exitFullscreen?.().catch(() => {});
      this.emit('leave-full-screen');
    }
  };
  isFullScreen = () => !!document.fullscreenElement;
  setAlwaysOnTop = (flag: boolean) => { this._alwaysOnTop = flag; };
  isAlwaysOnTop = () => this._alwaysOnTop;
  getBounds = () => ({ x: window.screenX || 0, y: window.screenY || 0, width: window.innerWidth, height: window.innerHeight });
  setBounds = (_bounds: Partial<{x: number; y: number; width: number; height: number}>) => {};
  getSize = () => [window.innerWidth, window.innerHeight];
  setSize = (_w: number, _h: number) => {};
  getPosition = () => [window.screenX || 0, window.screenY || 0];
  setPosition = (_x: number, _y: number) => {};
  setMinimumSize = (_w: number, _h: number) => {};
  setMaximumSize = (_w: number, _h: number) => {};
  setResizable = (_flag: boolean) => {};
  setTitle = (title: string) => { this._title = title; document.title = title; };
  getTitle = () => document.title || this._title;
  getParentWindow = () => null;
  getChildWindows = () => [];
  setParentWindow = () => {};
  getNativeWindowHandle = () => {
    const handle = window.__xenonPlayerHandles__?.parentWindow || 0;
    const buf = new Uint8Array(8);
    new DataView(buf.buffer).setUint32(0, handle, true);
    return {
      readUIntLE: (_offset: number, _length: number) => handle,
      readUInt32LE: (_offset: number) => handle,
      buffer: buf.buffer,
    };
  };
  bringToTop = () => { window.focus(); };
  flashFrame = (_flag: boolean) => {};
  setSkipTaskbar = (_flag: boolean) => {};
  setProgressBar = (_progress: number) => {};
}

const browserWindowInstance = new BrowserWindow();

const appModule = Object.assign(new SimpleEventEmitter(), {
  getAppPath: () => dirnamePath(String(window.__xenonExecPath__ || '')),
  getPath: (name: string) => {
    const exeDir = dirnamePath(String(window.__xenonExecPath__ || ''));
    if (name === 'userData' || name === 'appData') {
      return joinPath(exeDir, 'xenon_player_userdata');
    }
    if (name === 'logs') {
      return joinPath(exeDir, 'logs');
    }
    if (name === 'temp') {
      return joinPath(exeDir, 'temp');
    }
    if (name === 'desktop') {
      return joinPath(exeDir, 'desktop');
    }
    if (name === 'documents') {
      return joinPath(exeDir, 'documents');
    }
    if (name === 'downloads') {
      return joinPath(exeDir, 'downloads');
    }
    if (name === 'exe') {
      return String(window.__xenonExecPath__ || '');
    }
    return exeDir;
  },
  setPath: (_name: string, _path: string) => {},
  getName: () => 'xenon-player',
  setName: (_name: string) => {},
  getVersion: () => '7.0.0.1',
  getLocale: () => navigator.language || 'zh-CN',
  getLocaleCountryCode: () => 'CN',
  isReady: () => true,
  whenReady: () => Promise.resolve(),
  quit: () => window.close(),
  exit: (_code = 0) => window.close(),
  relaunch: () => window.location.reload(),
  focus: () => window.focus(),
  hide: () => {},
  show: () => {},
  requestSingleInstanceLock: () => true,
  hasSingleInstanceLock: () => true,
  releaseSingleInstanceLock: () => {},
});

const dialogModule = {
  showOpenDialog: async (optsOrWin?: unknown, maybeOpts?: unknown) => {
    const options = ((maybeOpts || optsOrWin || {}) as {
      title?: string;
      properties?: string[];
      filters?: Array<{name: string; extensions: string[]}>;
    });
    const title = options.title || '选择文件';
    const multi = (options.properties || []).includes('multiSelections');
    const paths = await openNativeFileDialog(title, [], multi);
    return { canceled: !paths || paths.length === 0, filePaths: paths || [] };
  },
  showOpenDialogSync: (_optsOrWin?: unknown, _maybeOpts?: unknown) => [] as string[],
  showSaveDialog: async (optsOrWin?: unknown, maybeOpts?: unknown) => {
    const options = ((maybeOpts || optsOrWin || {}) as { title?: string; defaultPath?: string });
    return { canceled: false, filePath: options.defaultPath || 'video.mp4' };
  },
  showSaveDialogSync: (_optsOrWin?: unknown, maybeOpts?: unknown) => {
    const options = ((maybeOpts || {}) as { defaultPath?: string });
    return options.defaultPath || '';
  },
  showMessageBox: async (optsOrWin?: unknown, maybeOpts?: unknown) => {
    const options = ((maybeOpts || optsOrWin || {}) as { message?: string; buttons?: string[] });
    if (options.buttons && options.buttons.length > 1) {
      const ok = window.confirm(options.message || '确认操作？');
      return { response: ok ? 0 : 1, checkboxChecked: false };
    }
    if (options.message) {
      window.alert(options.message);
    }
    return { response: 0, checkboxChecked: false };
  },
  showMessageBoxSync: (optsOrWin?: unknown, maybeOpts?: unknown) => {
    const options = ((maybeOpts || optsOrWin || {}) as { message?: string; buttons?: string[] });
    if (options.buttons && options.buttons.length > 1) {
      const ok = window.confirm(options.message || '确认操作？');
      return ok ? 0 : 1;
    }
    if (options.message) {
      window.alert(options.message);
    }
    return 0;
  },
  showErrorBox: (title: string, content: string) => {
    console.error(`[xenon-player dialog.showErrorBox] ${title}: ${content}`);
  },
};

const shellModule = {
  openExternal: async (url: string) => {
    window.open(url, '_blank', 'noopener,noreferrer');
  },
  openPath: async (filePath: string) => {
    if (filePath) {
      window.open('file:///' + filePath.replace(/\\/g, '/'), '_blank');
    }
    return '';
  },
  showItemInFolder: (fullPath: string) => {
    if (fullPath) {
      window.open('file:///' + fullPath.replace(/\\/g, '/'), '_blank');
    }
  },
  trashItem: async (_path: string) => true,
  beep: () => {
    try {
      const ctx = new (window.AudioContext || (window as unknown as {webkitAudioContext: typeof AudioContext}).webkitAudioContext)();
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.type = 'sine';
      osc.frequency.setValueAtTime(440, ctx.currentTime);
      gain.gain.setValueAtTime(0.1, ctx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + 0.2);
      osc.start();
      osc.stop(ctx.currentTime + 0.2);
    } catch {}
  },
  readShortcutLink: (_shortcutPath: string) => ({ target: '' }),
  writeShortcutLink: (_shortcutPath: string, _operation: unknown, _options: unknown) => true,
};

let inMemoryClipboardText = '';
let inMemoryClipboardHtml = '';

window.addEventListener('copy', () => {
  navigator.clipboard?.readText?.().then(text => { inMemoryClipboardText = text; }).catch(() => {});
}, { passive: true });
window.addEventListener('paste', () => {
  navigator.clipboard?.readText?.().then(text => { inMemoryClipboardText = text; }).catch(() => {});
}, { passive: true });

const clipboardModule = {
  readText: (_type?: string) => inMemoryClipboardText,
  readTextAsync: async () => {
    try {
      const text = await navigator.clipboard.readText();
      inMemoryClipboardText = text;
      return text;
    } catch {
      return inMemoryClipboardText;
    }
  },
  writeText: (text: string, _type?: string) => {
    inMemoryClipboardText = String(text || '');
    try {
      navigator.clipboard?.writeText?.(inMemoryClipboardText).catch(() => {});
    } catch {}
  },
  readHTML: (_type?: string) => inMemoryClipboardHtml,
  writeHTML: (markup: string, _type?: string) => { inMemoryClipboardHtml = markup; },
  readImage: (_type?: string) => nativeImageModule.createEmpty(),
  writeImage: (_image: unknown, _type?: string) => {},
  readRTF: (_type?: string) => '',
  writeRTF: (_text: string, _type?: string) => {},
  clear: (_type?: string) => {
    inMemoryClipboardText = '';
    inMemoryClipboardHtml = '';
    try { navigator.clipboard?.writeText?.('').catch(() => {}); } catch {}
  },
  availableFormats: (_type?: string) => inMemoryClipboardHtml ? ['text/plain', 'text/html'] : ['text/plain'],
  has: (format: string, _type?: string) => format === 'text/plain' || (format === 'text/html' && !!inMemoryClipboardHtml),
};

class NativeImageImpl {
  private _dataUrl = '';
  constructor(dataUrl = '') { this._dataUrl = dataUrl; }
  toPNG = () => new Uint8Array(0);
  toJPEG = (_quality = 100) => new Uint8Array(0);
  toBitmap = () => new Uint8Array(0);
  toDataURL = () => this._dataUrl;
  getBitmap = () => new Uint8Array(0);
  getNativeHandle = () => new Uint8Array(0);
  isEmpty = () => !this._dataUrl;
  getSize = () => ({ width: 0, height: 0 });
  setAspectRatio = (_ratio: number) => {};
  resize = (_options: unknown) => this;
  crop = (_rect: unknown) => this;
  getAspectRatio = () => 1;
}

const nativeImageModule = {
  createEmpty: () => new NativeImageImpl(),
  createFromPath: (path: string) => new NativeImageImpl(path),
  createFromBuffer: (_buf: unknown) => new NativeImageImpl(),
  createFromDataURL: (dataURL: string) => new NativeImageImpl(dataURL),
  createFromNamedImage: (name: string) => new NativeImageImpl(name),
};

const darkMediaQuery = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
let currentThemeSource: 'system' | 'light' | 'dark' = 'system';

const nativeThemeModule = Object.assign(new SimpleEventEmitter(), {
  get shouldUseDarkColors() {
    if (currentThemeSource === 'dark') return true;
    if (currentThemeSource === 'light') return false;
    return !!darkMediaQuery?.matches;
  },
  get themeSource() {
    return currentThemeSource;
  },
  set themeSource(val: 'system' | 'light' | 'dark') {
    if (val !== currentThemeSource) {
      currentThemeSource = val;
      nativeThemeModule.emit('updated');
    }
  },
  get shouldUseHighContrastColors() {
    return false;
  },
  get shouldUseInvertedColorScheme() {
    return false;
  },
});

darkMediaQuery?.addEventListener?.('change', () => {
  if (currentThemeSource === 'system') {
    nativeThemeModule.emit('updated');
  }
});

let lastCursorPos = { x: 0, y: 0 };
window.addEventListener('mousemove', (e) => {
  lastCursorPos = {
    x: (window.screenX || 0) + e.clientX,
    y: (window.screenY || 0) + e.clientY,
  };
}, { passive: true });

const getRealDisplay = () => {
  const width = window.screen?.width || window.innerWidth || 1920;
  const height = window.screen?.height || window.innerHeight || 1080;
  const availWidth = window.screen?.availWidth || width;
  const availHeight = window.screen?.availHeight || height;
  const scaleFactor = window.devicePixelRatio || 1;
  return {
    id: 1,
    bounds: { x: 0, y: 0, width, height },
    workArea: { x: 0, y: 0, width: availWidth, height: availHeight },
    scaleFactor,
    rotation: 0,
    touchSupport: 'unknown',
  };
};

const screenModule = Object.assign(new SimpleEventEmitter(), {
  getCursorScreenPoint: () => ({ ...lastCursorPos }),
  getPrimaryDisplay: () => getRealDisplay(),
  getAllDisplays: () => [getRealDisplay()],
  getDisplayMatching: () => getRealDisplay(),
  getDisplayNearestPoint: () => getRealDisplay(),
});

const defaultSession = {
  webRequest: {
    onBeforeRequest: (_filter: unknown, listener?: AnyFn) => { listener?.({ url: '' }, () => {}); },
    onBeforeSendHeaders: (_filter: unknown, listener?: AnyFn) => { listener?.({ url: '', requestHeaders: {} }, () => {}); },
    onSendHeaders: (_filter: unknown, listener?: AnyFn) => { listener?.({ url: '', requestHeaders: {} }); },
    onHeadersReceived: (_filter: unknown, listener?: AnyFn) => { listener?.({ url: '', responseHeaders: {} }, () => {}); },
    onResponseStarted: (_filter: unknown, _listener?: AnyFn) => {},
    onBeforeRedirect: (_filter: unknown, _listener?: AnyFn) => {},
    onCompleted: (_filter: unknown, _listener?: AnyFn) => {},
    onErrorOccurred: (_filter: unknown, _listener?: AnyFn) => {},
  },
  cookies: {
    get: async (filter?: {name?: string; url?: string}) => {
      const result: Array<{name: string; value: string; domain: string; path: string}> = [];
      const cookieStr = document.cookie || '';
      for (const pair of cookieStr.split(';')) {
        const [k, v] = pair.trim().split('=');
        if (k && (!filter?.name || filter.name === k)) {
          result.push({
            name: k,
            value: decodeURIComponent(v || ''),
            domain: window.location.hostname || 'localhost',
            path: '/',
          });
        }
      }
      return result;
    },
    set: async (details: {name: string; value: string; domain?: string; path?: string; expirationDate?: number}) => {
      let cookie = `${encodeURIComponent(details.name)}=${encodeURIComponent(details.value)}; path=${details.path || '/'}`;
      if (details.expirationDate) {
        cookie += `; expires=${new Date(details.expirationDate * 1000).toUTCString()}`;
      }
      document.cookie = cookie;
    },
    remove: async (_url: string, name: string) => {
      document.cookie = `${encodeURIComponent(name)}=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/`;
    },
    flushStore: async () => {},
  },
  protocol: {
    registerFileProtocol: () => true,
    registerHttpProtocol: () => true,
  },
  setProxy: async () => {},
  getCacheSize: async () => 0,
  clearCache: async () => {},
  clearStorageData: async () => {},
};

const sessionModule = {
  defaultSession,
  fromPartition: () => defaultSession,
};

const protocolModule = {
  registerSchemesAsPrivileged: () => {},
  registerFileProtocol: () => true,
  registerHttpProtocol: () => true,
  registerStringProtocol: () => true,
  registerBufferProtocol: () => true,
  registerStreamProtocol: () => true,
  unregisterProtocol: () => true,
  isProtocolRegistered: () => true,
  interceptFileProtocol: () => true,
};

class MenuItem {
  id = '';
  label = '';
  enabled = true;
  visible = true;
  checked = false;
  constructor(options: Partial<MenuItem> = {}) { Object.assign(this, options); }
}

const MenuModule = Object.assign(class Menu {
  items: MenuItem[] = [];
  append = (item: MenuItem) => { this.items.push(item); };
  insert = (pos: number, item: MenuItem) => { this.items.splice(pos, 0, item); };
  popup = () => {};
  closePopup = () => {};
}, {
  buildFromTemplate: (template: Array<Partial<MenuItem>>) => {
    const menu = new MenuModule();
    menu.items = template.map(t => new MenuItem(t));
    return menu;
  },
  setApplicationMenu: (_menu: unknown) => {},
  getApplicationMenu: () => null,
});

const powerMonitorModule = Object.assign(new SimpleEventEmitter(), {
  getSystemIdleState: () => 'active',
  getSystemIdleTime: () => 0,
  isOnBatteryPower: () => false,
});

let updateFeedUrl = '';
let currentCheckingActive = false;

const autoUpdaterModule = Object.assign(new SimpleEventEmitter(), {
  autoDownload: false,
  autoRunAppAfterInstall: true,
  autoInstallOnAppQuit: true,
  channel: 'latest',
  currentVersion: { version: '7.0.0.1' },
  isUpdaterActive: () => currentCheckingActive,
  setFeedURL: (urlOrOptions: unknown) => {
    if (typeof urlOrOptions === 'string') {
      updateFeedUrl = urlOrOptions;
    } else if (urlOrOptions && typeof urlOrOptions === 'object' && 'url' in urlOrOptions) {
      updateFeedUrl = String((urlOrOptions as {url: unknown}).url || '');
    }
  },
  getFeedURL: () => updateFeedUrl,
  checkForUpdates: async () => {
    currentCheckingActive = true;
    autoUpdaterModule.emit('checking-for-update');
    if (!updateFeedUrl) {
      currentCheckingActive = false;
      queueMicrotask(() => {
        autoUpdaterModule.emit('update-not-available', { version: '7.0.0.1' });
      });
      return { updateInfo: { version: '7.0.0.1' } };
    }
    try {
      const resp = await fetch(updateFeedUrl);
      const data = await resp.json() as {version?: string; releaseNotes?: string};
      currentCheckingActive = false;
      if (data && data.version && data.version !== '7.0.0.1') {
        autoUpdaterModule.emit('update-available', data);
        return { updateInfo: data };
      }
      autoUpdaterModule.emit('update-not-available', { version: '7.0.0.1' });
      return { updateInfo: { version: '7.0.0.1' } };
    } catch (err) {
      currentCheckingActive = false;
      autoUpdaterModule.emit('error', err);
      return null;
    }
  },
  checkForUpdatesAndNotify: async () => {
    return autoUpdaterModule.checkForUpdates();
  },
  downloadUpdate: async () => {
    autoUpdaterModule.emit('download-progress', { percent: 100 });
    autoUpdaterModule.emit('update-downloaded', { version: '7.0.0.1' });
    return [];
  },
  quitAndInstall: () => {
    window.location.reload();
  },
});

const electronApi = {
  ipcRenderer,
  ipcMain,
  BrowserWindow,
  app: appModule,
  dialog: dialogModule,
  shell: shellModule,
  clipboard: clipboardModule,
  screen: screenModule,
  nativeImage: nativeImageModule,
  nativeTheme: nativeThemeModule,
  session: sessionModule,
  protocol: protocolModule,
  Menu: MenuModule,
  MenuItem,
  powerMonitor: powerMonitorModule,
  autoUpdater: autoUpdaterModule,
  contextBridge: {
    exposeInMainWorld: () => undefined,
  },
};

declare global {
  interface Window {
    __xenonExecPath__?: string;
    __xenonFrontendDir__?: string;
    __xenonPlayerHandles__?: {floatWindow: number; parentWindow: number};
    require?: (id: string) => unknown;
    __non_webpack_require__?: (id: string) => unknown;
    process?: object;
    module?: {exports: unknown};
    exports?: unknown;
    global?: typeof globalThis;
    electron?: typeof electronApi;
  }
}

function installProcess(execPath: string) {
  const exeDir = dirnamePath(execPath);
  const proc = {
    execPath,
    cwd: () => exeDir,
    platform: 'win32',
    arch: 'x64',
    env: {
      HOME: exeDir,
      USERPROFILE: exeDir,
      APP_BASE_DIR: exeDir,
    } as Record<string, string>,
    versions: {node: '16.17.1', electron: '22.0.0'},
    nextTick: (cb: AnyFn) => queueMicrotask(() => cb()),
    browser: true,
  };
  window.process = proc;
  (globalThis as {process?: unknown}).process = proc;
}

class SqliteStatement {
  bind(...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null));
    return this;
  }
  reset(cb?: AnyFn) {
    queueMicrotask(() => cb?.(null));
    return this;
  }
  finalize(cb?: AnyFn) {
    queueMicrotask(() => cb?.(null));
    return this;
  }
  run(...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null));
    return this;
  }
  get(...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null, {}));
    return this;
  }
  all(...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null, []));
    return this;
  }
  each(...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null, {}));
    return this;
  }
}

class SqliteDatabase {
  constructor(_path?: string, _modeOrCb?: unknown, cb?: AnyFn) {
    const callback = typeof _modeOrCb === 'function' ? _modeOrCb : cb;
    queueMicrotask(() => callback?.(null));
  }
  run(_sql: string, ...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null));
    return this;
  }
  get(_sql: string, ...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null, {}));
    return this;
  }
  all(_sql: string, ...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null, []));
    return this;
  }
  each(_sql: string, ...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null, {}));
    return this;
  }
  exec(_sql: string, cb?: AnyFn) {
    queueMicrotask(() => cb?.(null));
    return this;
  }
  prepare(_sql: string, ...args: unknown[]) {
    const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
    queueMicrotask(() => cb?.(null));
    return new SqliteStatement();
  }
  close(cb?: AnyFn) {
    queueMicrotask(() => cb?.(null));
  }
  serialize(cb?: AnyFn) {
    cb?.();
  }
  parallelize(cb?: AnyFn) {
    cb?.();
  }
}

const sqlite3Module = {
  Database: SqliteDatabase,
  Statement: SqliteStatement,
  OPEN_READONLY: 1,
  OPEN_READWRITE: 2,
  OPEN_CREATE: 4,
  OPEN_FULLMUTEX: 0x00010000,
  OPEN_URI: 0x00000040,
  OPEN_SHAREDCACHE: 0x00020000,
  OPEN_PRIVATECACHE: 0x00040000,
  verbose: () => sqlite3Module,
};

function createSyncNativeAddon(
    moduleId: string, useRealAddon = true): Record<string, unknown> {
  const handles = () => window.__xenonPlayerHandles__ ||
      {floatWindow: 0, parentWindow: 0};
  const noop = () => undefined;
  const base = nativeAddonKey(moduleId);
  const realAddon = useRealAddon ? realNativeAddons.get(base) : undefined;
  if (realAddon) {
    return realAddon as Record<string, unknown>;
  }

  if (base.includes('sqlite') || moduleId.includes('sqlite')) {
    return sqlite3Module as unknown as Record<string, unknown>;
  }

  if (base.includes('xmp_helper') || moduleId.includes('xmp_helper')) {
    class NativeScrapeSdk {
      registerCallback(_cb?: AnyFn) { return Promise.resolve(0); }
      unregisterCallback() { return Promise.resolve(0); }
      init() { return Promise.resolve(0); }
      uninit() { return Promise.resolve(0); }
      listEvents() { return Promise.resolve(JSON.stringify({ events: [], next_page_token: '' })); }
      deleteEvents() { return Promise.resolve(0); }
      cleanEvents() { return Promise.resolve({ task_id: 1 }); }
      listCleanEvents() { return Promise.resolve({ task_id: 1 }); }
    }
    class SimpleObjectRefAddon {
      constructor(_id?: string, _cb?: AnyFn) {
        void _id;
        void _cb;
      }
    }
    class NativeSqliteStorage {
      constructor(_p?: string) {}
      exec() { return 0; }
      query() { return []; }
      close() {}
    }
    class NativeFileMonitor {
      watch() {}
      unwatch() {}
      start() {}
      stop() {}
    }
    const xmpTarget: Record<string, unknown> = {
      getFileVersion: () => '7.0.0.1',
      getPeerId: () => '00000000000000000000000000000000',
      getDmideCode: () => '0000000000000000',
      getInstallChannel: () => 'xenon',
      getOSName: () => 'Windows',
      getAppBuildID: () => '1',
      getPublicUserDataPath: () => dirnamePath(String(window.__xenonExecPath__ || '')),
      readINI: () => '',
      writeINI: () => true,
      readRegString: () => ({ succ: false, v: '' }),
      readRegDword: () => ({ succ: false, v: 0 }),
      writeRegString: () => true,
      writeRegDword: () => true,
      isAutoRun: () => false,
      setAutoRun: () => true,
      openDir: () => true,
      getProcessCreationTime: () => Date.now(),
      setCommandLineCallback: () => 0,
      getCommandLine: () => '',
      invalidPlayerWnd: () => 0,
      setPlayerWndVisible: () => 0,
      hideWndCursor: () => 0,
      setProgramWndStatus: () => 0,
      NativeScrapeSdk,
      SimpleObjectRefAddon,
      NativeSqliteStorage,
      NativeFileMonitor,
    };
    return new Proxy(xmpTarget, {
      get: (t, p) => {
        if (typeof p === 'string' && p in t) return t[p];
        if (p === 'then') return undefined;
        return (..._args: unknown[]) => 0;
      },
    }) as unknown as Record<string, unknown>;
  }

  if (base.includes('player_helper') || moduleId.includes('player_helper')) {
    return new Proxy(function() {}, {
      get: (_t, prop) => {
        if (prop === 'then') {
          return undefined;
        }
        return (..._args: unknown[]) => 0;
      },
      apply: () => 0,
    }) as unknown as Record<string, unknown>;
  }

  if (base.includes('pc_addon') || moduleId.includes('pc_addon') ||
      moduleId.includes('playercontrol') ||
      moduleId.includes('PlayerControl')) {
    let mediaStateChangeListeners: Array<(state: number) => void> = [];

    const createDummyManager = (mediaName = 'video.mp4', mediaUrl = '', videoEl?: HTMLVideoElement | null): object => {
      const listeners: Record<string, AnyFn[]> = {};
      const mediaId = 'media_' + Date.now() + '_' + Math.random().toString(36).slice(2, 6);

      if (videoEl) {
        const onTimeUpdate = () => {
          const posMs = Math.floor((videoEl.currentTime || 0) * 1000);
          const progressListeners = listeners['attachProgressChangedEvent'] || [];
          for (const listener of progressListeners) {
            try { listener(posMs); } catch (_e) {}
          }
        };

        const onPlay = () => {
          const playStateListeners = listeners['attachPlayStateChangeEvent'] || [];
          for (const listener of playStateListeners) {
            try { listener(4); } catch (_e) {} // MediaState.MsPlay = 4
          }
          for (const listener of mediaStateChangeListeners) {
            try { listener(4); } catch (_e) {} // MediaState.MsPlay = 4
          }
        };

        const onPause = () => {
          const playStateListeners = listeners['attachPlayStateChangeEvent'] || [];
          for (const listener of playStateListeners) {
            try { listener(3); } catch (_e) {} // MediaState.MsPause = 3
          }
          for (const listener of mediaStateChangeListeners) {
            try { listener(3); } catch (_e) {} // MediaState.MsPause = 3
          }
        };

        const onEnded = () => {
          const playStateListeners = listeners['attachPlayStateChangeEvent'] || [];
          for (const listener of playStateListeners) {
            try { listener(6); } catch (_e) {} // MediaState.MsStop = 6
          }
          for (const listener of mediaStateChangeListeners) {
            try { listener(6); } catch (_e) {} // MediaState.MsStop = 6
          }
          activePlayListManager.playNext();
        };

        const onLoaded = () => {
          const firstRenderListeners = listeners['attachFirstRenderEvent'] || [];
          for (const listener of firstRenderListeners) {
            try { listener(0); } catch (_e) {}
          }
          const ratioListeners = listeners['attachRatioPreparedEvent'] || [];
          for (const listener of ratioListeners) {
            try { listener(); } catch (_e) {}
          }
          const playStateListeners = listeners['attachPlayStateChangeEvent'] || [];
          for (const listener of playStateListeners) {
            try { listener(4); } catch (_e) {} // MediaState.MsPlay = 4
          }
          for (const listener of mediaStateChangeListeners) {
            try { listener(4); } catch (_e) {} // MediaState.MsPlay = 4
          }
        };

        videoEl.addEventListener('timeupdate', onTimeUpdate);
        videoEl.addEventListener('play', onPlay);
        videoEl.addEventListener('playing', onPlay);
        videoEl.addEventListener('pause', onPause);
        videoEl.addEventListener('ended', onEnded);
        videoEl.addEventListener('loadedmetadata', onLoaded);
        videoEl.addEventListener('canplay', onLoaded);
      }

      return new Proxy(function() {}, {
        get: (_t, prop) => {
          if (prop === 'then') return undefined;
          if (prop === 'id') return mediaId;
          if (prop === 'getCount') return () => activePlayList.length;
          if (prop === 'getItem') return (idx: number) => activePlayList[idx] || null;
          if (prop === 'getList' || prop === 'getPlayList') return () => activePlayListManager.getPlayList();
          if (prop === 'getHistoryList' || prop === 'search') return () => [];
          if (prop === 'isPrepared') return () => true;
          if (prop === 'getName') return () => mediaName;
          if (prop === 'getType') return () => 5; // MediaType.MtNewXmpLocal
          if (prop === 'getPlayId') return () => mediaId;
          if (prop === 'getAttribute') return () => ({
            name: mediaName,
            playUrl: mediaUrl || 'd:\\test.mp4',
            mediaType: 5,
          });
          if (prop === 'getExtraAttribute') return () => '';
          if (prop === 'getMediaWidth') return () => (videoEl?.videoWidth || 1920);
          if (prop === 'getMediaHeight') return () => (videoEl?.videoHeight || 1080);
          if (prop === 'getDuration') return () => (videoEl && Number.isFinite(videoEl.duration) ? Math.floor(videoEl.duration * 1000) : 60000);
          if (prop === 'getPosition') return () => (videoEl ? Math.floor(videoEl.currentTime * 1000) : 0);
          if (prop === 'getPlayProgress') return () => (videoEl ? Math.floor(videoEl.currentTime * 1000) : 0);
          if (prop === 'getMediaState') return () => (videoEl && !videoEl.paused ? 4 : 3); // 4 = MsPlay, 3 = MsPause
          if (prop === 'getMediaErrorInfo') return () => ({ errCode: 0, errMsg: '' });
          if (prop === 'getEndStatEventParam') return (cb: AnyFn) => queueMicrotask(() => cb?.({}));
          if (prop === 'progressMoveTo' || prop === 'setPosition') {
            return (pos: number) => {
              if (videoEl && Number.isFinite(pos)) {
                videoEl.currentTime = pos / 1000;
              }
            };
          }
          if (prop === 'play') {
            return () => { videoEl?.play().catch(() => {}); };
          }
          if (prop === 'pause') {
            return () => { videoEl?.pause(); };
          }
          if (typeof prop === 'string' && prop.startsWith('attach')) {
            return (cb: AnyFn) => {
              if (typeof cb === 'function') {
                listeners[prop] = listeners[prop] || [];
                listeners[prop].push(cb);
                if (prop === 'attachFirstRenderEvent') {
                  queueMicrotask(() => cb(0));
                } else if (prop === 'attachRatioPreparedEvent') {
                  queueMicrotask(() => cb());
                } else if (prop === 'attachPlayStateChangeEvent') {
                  queueMicrotask(() => {
                    try { cb(2); } catch {} // MsSucc = 2 (fulfills waitPlayerShowGetMediaInfo for playlist)
                    if (videoEl && !videoEl.paused) {
                      setTimeout(() => {
                        try { cb(4); } catch {} // MsPlay = 4 (shows pause button)
                      }, 20);
                    }
                  });
                } else if (prop === 'attachProgressChangedEvent') {
                  if (videoEl) {
                    const posMs = Math.floor(videoEl.currentTime * 1000);
                    queueMicrotask(() => cb(posMs));
                  }
                }
              }
              return listeners[prop]?.length || 1;
            };
          }
          if (typeof prop === 'string' && prop.startsWith('detach')) {
            return (cookie: unknown) => {
              const attachProp = prop.replace('detach', 'attach');
              if (listeners[attachProp]) {
                if (typeof cookie === 'function') {
                  listeners[attachProp] = listeners[attachProp].filter(c => c !== cookie);
                } else if (typeof cookie === 'number' && cookie > 0) {
                  listeners[attachProp].splice(cookie - 1, 1);
                }
              }
            };
          }
          if (prop === 'getPlayList') {
            return () => activePlayListManager;
          }
          if (typeof prop === 'string' && (prop.includes('Manager') || prop.startsWith('get'))) {
            return () => createDummyManager(mediaName, mediaUrl, videoEl);
          }
          return (...args: unknown[]) => {
            const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
            if (cb) {
              queueMicrotask(() => cb(createDummyManager(mediaName, mediaUrl, videoEl)));
            }
            return 0;
          };
        },
        apply: (_t, _this, args) => {
          const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
          if (cb) {
            queueMicrotask(() => cb(createDummyManager(mediaName, mediaUrl, videoEl)));
          }
          return 0;
        },
        construct: () => createDummyManager(mediaName, mediaUrl, videoEl),
      });
    };


    const pcExport = {
      initAddon: (param?: any) => {
        if (realPcAddon && typeof realPcAddon.initAddon === 'function') {
          return realPcAddon.initAddon(param);
        }
        return true;
      },
      setWndEx: (floatWnd?: any, parentWnd?: any) => {
        const fw = Number(floatWnd || handles().floatWindow || 0);
        const pw = Number(parentWnd || handles().parentWindow || 0);
        if (realPcAddon && typeof realPcAddon.setWndEx === 'function') {
          return realPcAddon.setWndEx(fw, pw);
        }
        return true;
      },
      setSnapshotWnd: noop,
      setPlayableExt: (exts?: any) => {
        if (realPcAddon && typeof realPcAddon.setPlayableExt === 'function') {
          return realPcAddon.setPlayableExt(exts);
        }
      },
      setScreenSaveActive: noop,
      setGlobalHttpRequest: noop,
      updateUserInfo: noop,
      NativeAplayerStack,
      getAplayerWnd: (cb?: (hwnd: number) => void) => {
        if (realPcAddon && typeof realPcAddon.getAplayerWnd === 'function') {
          let settled = false;
          const finish = (hwnd: number) => {
            if (settled) {
              return;
            }
            settled = true;
            cb?.(hwnd);
          };
          try {
            realPcAddon.getAplayerWnd((hwnd: number) => finish(Number(hwnd) || 0));
          } catch (error) {
            console.warn('[xenon-player] getAplayerWnd threw:', error);
          }
          // InitPlayer awaits this callback; never leave it hanging (NaN HWND /
          // native stall would block createApp and leave a black empty shell).
          window.setTimeout(() => {
            const h = handles();
            finish(Number(h.parentWindow) || Number(h.floatWindow) || 1);
          }, 2000);
          return;
        }
        const hwnd = handles().parentWindow || handles().floatWindow || 1;
        queueMicrotask(() => cb?.(hwnd));
      },
    };
    return new Proxy(pcExport, {
      get: (t, prop) => {
        if (typeof prop === 'string' && prop in t) {
          return (t as Record<string, unknown>)[prop];
        }
        if (prop === 'then') return undefined;
        return (..._args: unknown[]) => 0;
      },
    }) as unknown as Record<string, unknown>;
  }

  if (base.includes('dk_addon') || moduleId.includes('dk_addon')) {
    const createProxyObject = (): object => new Proxy(function() {}, {
      get: (_target, prop) => {
        if (prop === 'then') return undefined;
        if (prop === 'getId') return () => 1;
        if (prop === 'isSupportPlay') return () => 1;
        if (prop === 'getTaskInfo') return () => ({ fileName: 'video.mp4', fileSize: 10000000 });
        if (prop === 'findRepeatTask') return () => [];
        if (prop === 'createTask') return () => createProxyObject();
        if (typeof prop === 'string' && prop.startsWith('attach')) {
          return () => 1;
        }
        if (typeof prop === 'string' && prop.startsWith('detach')) {
          return () => undefined;
        }
        if (typeof prop === 'string' && (prop.includes('Manager') || prop.startsWith('get'))) {
          return () => createProxyObject();
        }
        return () => 0;
      },
      apply: () => 0,
      construct: () => createProxyObject(),
    });

    const createProxyClass = () => class {
      constructor() {
        return new Proxy(this, {
          get: (target, prop) => {
            if (typeof prop === 'string' && prop in target) {
              return (target as Record<string, unknown>)[prop];
            }
            if (prop === 'getId') return () => 1;
            if (prop === 'isSupportPlay') return () => 1;
            if (prop === 'findRepeatTask') return () => [];
            if (prop === 'createTask') return () => createProxyObject();
            if (typeof prop === 'string' && prop.startsWith('attach')) {
              return () => 1;
            }
            if (typeof prop === 'string' && prop.startsWith('detach')) {
              return () => undefined;
            }
            if (typeof prop === 'string' && (prop.includes('Manager') || prop.startsWith('get'))) {
              return () => createProxyObject();
            }
            return () => undefined;
          },
        });
      }
      createTask(_taskSet: unknown) {
        return createProxyObject();
      }
      findRepeatTask(_taskSet: unknown) {
        return [];
      }
    };

    const NativeDkHelper = {
      isThunderPrivateUrl: (url: string) => (String(url).toLowerCase().startsWith('thunder://') ? 1 : 0),
      parseThunderPrivateUrl: (url: string) => {
        try {
          if (url.startsWith('thunder://')) {
            const raw = atob(url.slice(10));
            if (raw.startsWith('AA') && raw.endsWith('ZZ')) {
              return raw.slice(2, -2);
            }
            return raw;
          }
        } catch (_e) {}
        return url;
      },
      parserEd2kLink: (url: string) => ({ isEd2k: url.startsWith('ed2k://') ? 1 : 0, name: '', size: 0, hash: '' }),
      parseMagnetUrl: (url: string) => ({ isMagnet: url.startsWith('magnet:?') ? 1 : 0, name: '', hash: '' }),
      parseP2spUrl: (url: string) => {
        let name = 'video.mp4';
        try {
          const u = new URL(url);
          name = u.pathname.split('/').filter(Boolean).pop() || 'video.mp4';
        } catch (_e) {
          name = url.split('/').filter(Boolean).pop() || 'video.mp4';
        }
        return { isP2sp: 1, name, isThunderUrl: 0, realUrl: url };
      },
      parseFileNameFromP2spUrlPath: (urlPath: string) => {
        try {
          const u = new URL(urlPath);
          return u.pathname.split('/').filter(Boolean).pop() || 'video.mp4';
        } catch (_e) {
          return urlPath.split('/').filter(Boolean).pop() || 'video.mp4';
        }
      },
      getTaskTypeFromUrl: (url: string) => {
        const lower = String(url).toLowerCase();
        if (lower.startsWith('http://') || lower.startsWith('https://') || lower.startsWith('ftp://')) return 1;
        if (lower.startsWith('magnet:?')) return 2;
        if (lower.startsWith('ed2k://')) return 3;
        return 1;
      },
      parseBtTaskInfo: (_filePath: string) => ({ name: '', hash: '', files: [] }),
      proxyVerify: () => ({ success: true }),
    };

    const NativeTaskManager = createProxyClass();
    const NativeCategoryManager = createProxyClass();
    const NativeTaskInterface = createProxyObject();
    const NativeCategoryInterface = createProxyObject();

    return {
      initAddon: () => true,
      NativeDkHelper,
      NativeTaskManager,
      NativeCategoryManager,
      NativeTaskInterface,
      NativeCategoryInterface,
    };
  }

  return new Proxy(function() {}, {
    get: (_t, prop) => {
      if (prop === 'then') return undefined;
      if (prop === 'length') return 4;
      if (prop === 'split') return () => ['7', '0', '0', '1'];
      if (prop === 'toString' || prop === 'valueOf') return () => '7.0.0.1';
      if (prop === Symbol.toPrimitive) return () => '7.0.0.1';
      return (..._args: unknown[]) => 0;
    },
    apply: () => 0,
    construct: () => ({}),
  }) as unknown as Record<string, unknown>;
}

function installRequire() {
  (globalThis as {Buffer?: typeof XenonBufferCtor}).Buffer = XenonBufferCtor;
  (window as {Buffer?: typeof XenonBufferCtor}).Buffer = XenonBufferCtor;

  const eventsModule = Object.assign(SimpleEventEmitter, {
    EventEmitter: SimpleEventEmitter,
    default: SimpleEventEmitter,
  });

  class Stream extends SimpleEventEmitter {
    pipe<T>(dest: T): T { return dest; }
    destroy() { return this; }
  }
  class ReadableStream extends Stream {
    read() { return null; }
    setEncoding() { return this; }
    pause() { return this; }
    resume() { return this; }
    isPaused() { return false; }
    unpipe() { return this; }
    unshift() {}
    wrap() { return this; }
    push() { return true; }
  }
  class WritableStream extends Stream {
    write(_chunk: unknown, _encodingOrCb?: unknown, cb?: AnyFn) {
      const callback = typeof _encodingOrCb === 'function' ? _encodingOrCb : cb;
      queueMicrotask(() => callback?.(null));
      return true;
    }
    end(_chunkOrCb?: unknown, _encodingOrCb?: unknown, cb?: AnyFn) {
      const callback = typeof _chunkOrCb === 'function' ? _chunkOrCb : (typeof _encodingOrCb === 'function' ? _encodingOrCb : cb);
      queueMicrotask(() => callback?.(null));
      return this;
    }
    setDefaultEncoding() { return this; }
    cork() {}
    uncork() {}
  }
  class DuplexStream extends ReadableStream {
    write(_chunk: unknown, _encodingOrCb?: unknown, cb?: AnyFn) {
      const callback = typeof _encodingOrCb === 'function' ? _encodingOrCb : cb;
      queueMicrotask(() => callback?.(null));
      return true;
    }
    end(_chunkOrCb?: unknown, _encodingOrCb?: unknown, cb?: AnyFn) {
      const callback = typeof _chunkOrCb === 'function' ? _chunkOrCb : (typeof _encodingOrCb === 'function' ? _encodingOrCb : cb);
      queueMicrotask(() => callback?.(null));
      return this;
    }
  }
  class TransformStream extends DuplexStream {}
  class PassThroughStream extends TransformStream {}

  const streamModule = {
    EventEmitter: SimpleEventEmitter,
    default: SimpleEventEmitter,
    Stream,
    Readable: ReadableStream,
    Writable: WritableStream,
    Duplex: DuplexStream,
    Transform: TransformStream,
    PassThrough: PassThroughStream,
    pipeline: (...args: unknown[]) => {
      const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
      queueMicrotask(() => cb?.(null));
    },
    finished: (_stream: unknown, cb?: AnyFn) => {
      queueMicrotask(() => cb?.(null));
    },
    promises: {
      pipeline: async (..._args: unknown[]) => undefined,
      finished: async (_stream: unknown) => undefined,
    },
  };

  const osModule = {
    platform: () => 'win32',
    release: () => '10.0.0',
    arch: () => 'x64',
    homedir: () => dirnamePath(String(window.__xenonExecPath__ || '')),
    tmpdir: () =>
        joinPath(dirnamePath(String(window.__xenonExecPath__ || '')), 'tmp'),
    hostname: () => 'xenon-player',
    EOL: '\n',
  };

  (window as unknown as {
    playLocalFile?: (p: string) => Promise<unknown>;
    __xmpRpcCall?: (fn: string, ...args: unknown[]) => Promise<unknown>;
    __registeredRpcFunctions?: Record<string, AnyFn>;
  }).__registeredRpcFunctions = registeredRpcFunctions;
  (window as unknown as {
    playLocalFile?: (p: string, directUrl?: string) => Promise<unknown>;
    __xmpRpcCall?: (fn: string, ...args: unknown[]) => Promise<unknown>;
  }).playLocalFile = async (filePath: string, directUrl?: string) => {
    const playTarget = directUrl || filePath;
    setCurrentPlayingUrl(playTarget);
    const stack = new NativeAplayerStack();
    const name = playTarget.split(/[\\/]/).pop()?.split('?')[0] || 'video.mp4';
    const isWeb = playTarget.startsWith('http://') || playTarget.startsWith('https://');
    await stack.openMedia({
      name,
      playUrl: playTarget,
      mediaType: isWeb ? 3 : 5,
    });
    return true;
  };
  (window as unknown as {
    __xmpRpcCall?: (fn: string, ...args: unknown[]) => Promise<unknown>;
  }).__xmpRpcCall = async (fn: string, ...args: unknown[]) => {
    if (registeredRpcFunctions[fn]) {
      return await registeredRpcFunctions[fn]({}, ...args);
    }
    return null;
  };



  const ipcClientModule = { client: createSmartIpcObject() };
  const ipcServerModule = { server: createSmartIpcObject() };

  const ipcBaseModule = {
    mainProcessContext: {},
    mainRendererContext: {},
  };

  const builtinModules: Record<string, unknown> = {
    electron: electronApi,
    '@xunlei/node-net-ipc/dist/ipc-client': ipcClientModule,
    '@xunlei/node-net-ipc/dist/ipc-server': ipcServerModule,
    '@xunlei/node-net-ipc/dist/ipc-base': ipcBaseModule,
    events: eventsModule,
    'node:events': eventsModule,
    path: pathApi,
    'node:path': pathApi,
    fs: fsApi,
    'node:fs': fsApi,
    'fs/promises': fsApi.promises,
    'node:fs/promises': fsApi.promises,
    crypto: cryptoApi,
    'node:crypto': cryptoApi,
    buffer: bufferModule,
    'node:buffer': bufferModule,
    string_decoder: stringDecoderModule,
    'node:string_decoder': stringDecoderModule,
    net: netModule,
    'node:net': netModule,
    readline: {
      createInterface: () => ({
        on() {
          return this;
        },
        close() {},
        question(_q: string, cb?: (answer: string) => void) {
          cb?.('');
        },
      }),
    },
    'node:readline': {
      createInterface: () => ({
        on() {
          return this;
        },
        close() {},
        question(_q: string, cb?: (answer: string) => void) {
          cb?.('');
        },
      }),
    },
    'https-proxy-agent': {
      HttpsProxyAgent: class HttpsProxyAgent {},
    },
    http: {
      request: (...args: unknown[]) => {
        const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
        const req = new SimpleEventEmitter() as SimpleEventEmitter&{
          end: () => unknown;
          write: () => boolean;
          destroy: () => void;
        };
        req.end = () => {
          queueMicrotask(() => {
            if (cb) {
              const res = new SimpleEventEmitter() as SimpleEventEmitter&{
                statusCode: number;
                headers: Record<string, string>;
                setEncoding: () => void;
              };
              res.statusCode = 200;
              res.headers = {'content-type': 'application/json'};
              res.setEncoding = () => undefined;
              cb(res);
              queueMicrotask(() => {
                res.emit('data', '{}');
                res.emit('end');
              });
            }
          });
          return req;
        };
        req.write = () => true;
        req.destroy = () => undefined;
        return req;
      },
      get(...args: unknown[]) {
        const req = (this as {request: (...a: unknown[]) => unknown}).request(...args);
        (req as {end?: () => void}).end?.();
        return req;
      },
      createServer: () => {
        const server = new SimpleEventEmitter() as SimpleEventEmitter&{
          listen: () => unknown;
          close: () => unknown;
        };
        server.listen = () => server;
        server.close = () => server;
        return server;
      },
    },
    https: {
      request: (...args: unknown[]) => {
        const cb = args.find(a => typeof a === 'function') as AnyFn | undefined;
        const req = new SimpleEventEmitter() as SimpleEventEmitter&{
          end: () => unknown;
          write: () => boolean;
          destroy: () => void;
        };
        req.end = () => {
          queueMicrotask(() => {
            if (cb) {
              const res = new SimpleEventEmitter() as SimpleEventEmitter&{
                statusCode: number;
                headers: Record<string, string>;
                setEncoding: () => void;
              };
              res.statusCode = 200;
              res.headers = {'content-type': 'application/json'};
              res.setEncoding = () => undefined;
              cb(res);
              queueMicrotask(() => {
                res.emit('data', '{}');
                res.emit('end');
              });
            }
          });
          return req;
        };
        req.write = () => true;
        req.destroy = () => undefined;
        return req;
      },
      get(...args: unknown[]) {
        const req = (this as {request: (...a: unknown[]) => unknown}).request(...args);
        (req as {end?: () => void}).end?.();
        return req;
      },
    },
    url: {
      URL,
      URLSearchParams,
      parse: (u: string) => {
        try {
          return new URL(u);
        } catch {
          return {};
        }
      },
    },
    'node:url': {
      URL,
      URLSearchParams,
      parse: (u: string) => {
        try {
          return new URL(u);
        } catch {
          return {};
        }
      },
    },
    querystring: {
      parse: () => ({}),
      stringify: () => '',
    },
    'node:querystring': {
      parse: () => ({}),
      stringify: () => '',
    },
    stream: streamModule,
    'node:stream': streamModule,
    'stream/promises': streamModule.promises,
    'node:stream/promises': streamModule.promises,
    tls: {},
    'node:tls': {},
    zlib: {
      gzipSync: (x: unknown) => x,
      gunzipSync: (x: unknown) => x,
      deflateSync: (x: unknown) => x,
      inflateSync: (x: unknown) => x,
    },
    'node:zlib': {
      gzipSync: (x: unknown) => x,
      gunzipSync: (x: unknown) => x,
      deflateSync: (x: unknown) => x,
      inflateSync: (x: unknown) => x,
    },
    child_process: {
      exec: () => undefined,
      spawn: () => ({
        on() {
          return this;
        },
        stdout: new SimpleEventEmitter(),
        stderr: new SimpleEventEmitter(),
        stdin: {write: () => true},
      }),
    },
    'node:child_process': {
      exec: () => undefined,
      spawn: () => ({
        on() {
          return this;
        },
        stdout: new SimpleEventEmitter(),
        stderr: new SimpleEventEmitter(),
        stdin: {write: () => true},
      }),
    },
    util: {
      inherits: (ctor: AnyFn, superCtor: AnyFn) => {
        Object.setPrototypeOf(
            ctor.prototype,
            (superCtor as AnyFn & {prototype: object}).prototype);
      },
      promisify: (fn: AnyFn) => (...args: unknown[]) =>
          new Promise((resolve, reject) => {
            try {
              fn(...args, (err: unknown, res: unknown) => {
                if (err) {
                  reject(err);
                } else {
                  resolve(res !== undefined ? res : true);
                }
              });
            } catch (e) {
              reject(e);
            }
          }),
    },
    'node:util': {
      inherits: (ctor: AnyFn, superCtor: AnyFn) => {
        Object.setPrototypeOf(
            ctor.prototype,
            (superCtor as AnyFn & {prototype: object}).prototype);
      },
      promisify: (fn: AnyFn) => (...args: unknown[]) =>
          new Promise((resolve, reject) => {
            try {
              fn(...args, (err: unknown, res: unknown) => {
                if (err) {
                  reject(err);
                } else {
                  resolve(res !== undefined ? res : true);
                }
              });
            } catch (e) {
              reject(e);
            }
          }),
    },
    os: osModule,
    'node:os': osModule,
    assert: {
      ok: () => undefined,
      equal: () => undefined,
      strictEqual: () => undefined,
    },
    'node:assert': {
      ok: () => undefined,
      equal: () => undefined,
      strictEqual: () => undefined,
    },
    sqlite3: sqlite3Module,
    'node_sqlite3.node': sqlite3Module,
    'electron-updater': { autoUpdater: autoUpdaterModule },
  };

  const requireImpl = (id: string): unknown => {
    if (Object.prototype.hasOwnProperty.call(builtinModules, id)) {
      return builtinModules[id];
    }

    const normalized = id.replace(/\\/g, '/');
    const base = basenamePath(normalized);
    if (base.endsWith('.node') || normalized.includes('pc_addon') ||
        normalized.includes('player_helper') ||
        normalized.includes('dk_addon') || normalized.includes('xmp_helper') ||
        normalized.includes('sqlite')) {
      return createSyncNativeAddon(normalized);
    }

    console.warn('[xenon-player] unresolved require (fallback to proxy):', id);
    return createSyncNativeAddon(normalized);
  };

  window.require = requireImpl;
  window.__non_webpack_require__ = requireImpl;
  (globalThis as {require?: typeof requireImpl}).require = requireImpl;
  (globalThis as {__non_webpack_require__?: typeof requireImpl}).__non_webpack_require__ = requireImpl;
  (globalThis as {global?: typeof globalThis}).global = globalThis;
  (window as {global?: typeof globalThis}).global = globalThis;
  window.electron = electronApi;
  window.module = {exports: {}};
  window.exports = window.module.exports;
  (globalThis as {__filename?: string}).__filename =
      'chrome://xenon-player/host.js';
  (globalThis as {__dirname?: string}).__dirname = 'chrome://xenon-player';
}

/**
 * Electron main-renderer reads ph/ch via GetUrlArgs(location.href).
 * Location.prototype.href is not patchable in chrome:// WebUI, so write the
 * query into the real URL (can be cleared after Vue mounts if desired).
 */
function installPhChInLocation(handles: {floatWindow: number; parentWindow: number}) {
  try {
    const url = new URL(window.location.href);
    // Electron: ph = video parent, ch = UI float.
    url.searchParams.set('ph', String(handles.parentWindow));
    url.searchParams.set('ch', String(handles.floatWindow));
    history.replaceState(null, '', url.toString());
  } catch (error) {
    console.warn('[xenon-player] failed to write ph/ch into location:', error);
  }
}

export async function installElectronShim(execPath: string): Promise<{
  floatWindow: number;
  parentWindow: number;
}> {
  window.__xenonExecPath__ = execPath;
  installDataUrlTransport();
  installProcess(execPath);

  const handles = await preparePlayerHost();
  window.__xenonPlayerHandles__ = handles;
  console.info('[xenon-player] player host ready', window.__xenonPlayerHandles__);
  installPhChInLocation(handles);
  await preloadRealNativeAddons();
  installRequire();
  registerPlaylistRpcFallbacks();
  registerPlayerRpcFunctions();

  return handles;
}

export async function cleanupElectronShim(): Promise<void> {
  await showPlayerVideoHost(false).catch(() => {});
}
