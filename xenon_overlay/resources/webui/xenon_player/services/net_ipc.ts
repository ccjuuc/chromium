// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable @typescript-eslint/require-await */
/* eslint-disable @typescript-eslint/no-explicit-any */

import {openNativeFileDialog} from '../require.js';
export {openNativeFileDialog};

export type AnyFn = (...args: unknown[]) => unknown;

// Use a single global event bus shared across all createSmartIpcObject()
// instances.  The Vue frontend may require() either the ipc-client or
// ipc-server module — both are backed by createSmartIpcObject() — while
// NativeAplayerStack broadcasts via the module-level ipcBroadcast().  If each
// call site kept its own closure-scoped bus, state-change events from the
// player service would never reach the Vue listeners (or vice-versa), which
// made the play/pause button appear stuck.
const g = globalThis as unknown as {
  __xenonNetIpcEventBus__?: Record<string, AnyFn[]>;
  __xenonNetIpcCookieSeq__?: number;
};
if (!g.__xenonNetIpcEventBus__) {
  g.__xenonNetIpcEventBus__ = {};
}
if (!g.__xenonNetIpcCookieSeq__) {
  g.__xenonNetIpcCookieSeq__ = 1;
}
export const ipcEventBus: Record<string, AnyFn[]> = g.__xenonNetIpcEventBus__;
export let ipcCookieSeq: number = g.__xenonNetIpcCookieSeq__;

// Same idea for RPC functions: the exported map and the one exposed on
// window.__registeredRpcFunctions must be the same object so that handlers
// registered from any require() context are visible everywhere.
const gRpc = globalThis as unknown as {
  __xenonRegisteredRpcFunctions__?: Record<string, AnyFn>;
};
if (!gRpc.__xenonRegisteredRpcFunctions__) {
  gRpc.__xenonRegisteredRpcFunctions__ = {};
}
export const registeredRpcFunctions: Record<string, AnyFn> =
    gRpc.__xenonRegisteredRpcFunctions__;

const IPC_STORE_LOCAL_STORAGE_KEY = 'xenon-player.net-ipc-store.v1';

function loadIpcStore(): Record<string, unknown> {
  try {
    const value = window.localStorage.getItem(IPC_STORE_LOCAL_STORAGE_KEY);
    if (value) {
      const parsed = JSON.parse(value) as unknown;
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        return parsed as Record<string, unknown>;
      }
    }
  } catch (error) {
    console.warn('[xenon-player net-ipc] failed to load cache store:', error);
  }
  return {};
}

function saveIpcStore(store: Record<string, unknown>): void {
  try {
    window.localStorage.setItem(
        IPC_STORE_LOCAL_STORAGE_KEY, JSON.stringify(store));
  } catch (error) {
    console.warn('[xenon-player net-ipc] failed to save cache store:', error);
  }
}

export function ipcBroadcast(eventName: string, ...args: unknown[]) {
  const list = ipcEventBus[eventName] || [];
  if (eventName === 'AplayerMeidaPlayStateChange' || eventName === 'AplayerStackMediaChangeEvent') {
    console.log('[xenon-player net-ipc] ipcBroadcast:', eventName, 'args:', JSON.stringify(args), 'listeners:', list.length);
  }
  for (const listener of list) {
    try {
      listener(null, ...args);
    } catch (e) {
      console.error('[xenon-player net-ipc] Event listener error:', e);
    }
  }
}

export function createSmartIpcObject(): object {
  const inMemoryStore = loadIpcStore();
  const target: Record<string, unknown> = {
    start: (_opts: unknown, _appName: unknown, flag: unknown, cb?: AnyFn) => {
      const callback = typeof flag === 'function' ? flag : cb;
      if (typeof callback === 'function') {
        window.setTimeout(() => callback('connect'), 10);
      }
    },
    registerFunctions: (fns: Record<string, AnyFn>) => {
      if (fns && typeof fns === 'object') {
        Object.assign(registeredRpcFunctions, fns);
      }
    },
    broadcastEvent: (eventName: string, ...args: unknown[]) => {
      ipcBroadcast(eventName, ...args);
    },
    AttachServerEvent: (eventName: string, cb: AnyFn) => {
      // ipcBroadcast calls listeners with (null, ...args) where null is the
      // IPC context placeholder.  The original Electron net-ipc library strips
      // this context before invoking attachServerEvent callbacks, so we wrap
      // the callback to drop the leading null and forward the real args.
      if (eventName === 'AplayerMeidaPlayStateChange' || eventName === 'AplayerStackMediaChangeEvent') {
        console.log('[xenon-player net-ipc] AttachServerEvent registered:', eventName);
      }
      const wrapped: AnyFn = (_ctx: unknown, ...realArgs: unknown[]) => {
        try { cb(...realArgs); } catch (e) { console.error('[xenon-player net-ipc] AttachServerEvent listener error:', e); }
      };
      ipcEventBus[eventName] = ipcEventBus[eventName] || [];
      ipcEventBus[eventName].push(wrapped);
      return ++ipcCookieSeq;
    },
    attachServerEvent: (eventName: string, cb: AnyFn) => {
      const wrapped: AnyFn = (_ctx: unknown, ...realArgs: unknown[]) => {
        try { cb(...realArgs); } catch (e) { console.error('[xenon-player net-ipc] attachServerEvent listener error:', e); }
      };
      ipcEventBus[eventName] = ipcEventBus[eventName] || [];
      ipcEventBus[eventName].push(wrapped);
      return ++ipcCookieSeq;
    },
    DetachServerEvent: (eventName: string, cookieOrCb: unknown) => {
      if (ipcEventBus[eventName]) {
        if (typeof cookieOrCb === 'function') {
          ipcEventBus[eventName] = ipcEventBus[eventName].filter(c => c !== cookieOrCb);
        }
      }
    },
    callRemoteServerFunction: async (_ctx: unknown, fnName: unknown, ...args: unknown[]) => {
      if (typeof fnName === 'string' && registeredRpcFunctions[fnName]) {
        const res = await registeredRpcFunctions[fnName](_ctx, ...args);
        return [res];
      }
      return Promise.resolve(['']);
    },
    callServerFunction: async (fnName: unknown, ...args: unknown[]) => {
      if (typeof fnName === 'string' && registeredRpcFunctions[fnName]) {
        const res = await registeredRpcFunctions[fnName]({}, ...args);
        return [res];
      }
      return Promise.resolve(['']);
    },
    callRemoteClientFunction: async (_ctx: unknown, fnName: unknown, ...args: unknown[]) => {
      if (fnName === 'openElectronSelectFileDialog' || fnName === 'showOpenDialog') {
        const opts = (args[0] || {}) as {title?: string; properties?: string[]};
        const title = opts.title || '选择文件';
        const multi = (opts.properties || []).includes('multiSelections');
        console.log('[xenon-player net-ipc] openElectronSelectFileDialog -> openNativeFileDialog');
        return openNativeFileDialog(title, [], multi).then((paths: string[]) => {
          if (paths && paths.length > 0) {
            return [paths];
          }
          return [[]];
        }).catch((err: unknown) => {
          console.error('[xenon-player net-ipc] openNativeFileDialog error:', err);
          return [[]];
        });
      }
      if (typeof fnName === 'string' && registeredRpcFunctions[fnName]) {
        if (fnName.startsWith('AplayerStack') || fnName.startsWith('AplayerMedia')) {
          console.log('[xenon-player net-ipc] callRemoteClientFunction:', fnName);
        }
        const res = await registeredRpcFunctions[fnName](_ctx, ...args);
        return [res];
      }
      if (typeof fnName === 'string' && fnName.startsWith('cache')) {
        const separator = fnName.lastIndexOf(':');
        const operation = separator >= 0 ? fnName.slice(separator + 1) : '';
        const namespace = separator >= 0 ? fnName.slice(0, separator) : '';
        const prefix = `${namespace}\u0000`;
        const key = `${prefix}${String(args[0])}`;
        if (operation === 'get') {
          const defaultValue = args[1];
          return [Object.prototype.hasOwnProperty.call(inMemoryStore, key) ?
              inMemoryStore[key] : defaultValue];
        }
        if (operation === 'set') {
          inMemoryStore[key] = args[1];
          saveIpcStore(inMemoryStore);
          return [true];
        }
        if (operation === 'delete') {
          if (args[0] === undefined || args[0] === null) {
            for (const storedKey of Object.keys(inMemoryStore)) {
              if (storedKey.startsWith(prefix)) {
                delete inMemoryStore[storedKey];
              }
            }
          } else {
            delete inMemoryStore[key];
          }
          saveIpcStore(inMemoryStore);
          return [true];
        }
        if (operation === 'getAllValue') {
          const values: Record<string, unknown> = {};
          for (const [storedKey, value] of Object.entries(inMemoryStore)) {
            if (storedKey.startsWith(prefix)) {
              values[storedKey.slice(prefix.length)] = value;
            }
          }
          return [values];
        }
      }
      if (typeof fnName === 'string' && fnName.endsWith('_get')) {
        const key = String(args[0]);
        const defaultVal = args[1];
        const val = inMemoryStore[key] !== undefined ? inMemoryStore[key] : (defaultVal !== undefined ? defaultVal : null);
        return Promise.resolve([val]);
      }
      if (typeof fnName === 'string' && fnName.endsWith('_set')) {
        const key = String(args[0]);
        const val = args[1];
        inMemoryStore[key] = val;
        saveIpcStore(inMemoryStore);
        return Promise.resolve([true]);
      }
      if (typeof fnName === 'string' && fnName.endsWith('_delete')) {
        const key = String(args[0]);
        delete inMemoryStore[key];
        saveIpcStore(inMemoryStore);
        return Promise.resolve([true]);
      }
      if (typeof fnName === 'string' && fnName.endsWith('_getAllValue')) {
        return Promise.resolve([{...inMemoryStore}]);
      }
      if (fnName === 'GetAppVersion') {
        return Promise.resolve(['7.0.0.1']);
      }
      if (fnName === 'GetStartupChannel') {
        return Promise.resolve(['xenon']);
      }
      return Promise.resolve(['']);
    },
    client: {
      call: async (methodName: string, ...args: unknown[]) => {
        if (registeredRpcFunctions[methodName]) {
          return await registeredRpcFunctions[methodName]({}, ...args);
        }
        return null;
      },
      emit: (eventName: string, ...args: unknown[]) => {
        ipcBroadcast(eventName, ...args);
      },
      on: (eventName: string, cb: AnyFn) => {
        ipcEventBus[eventName] = ipcEventBus[eventName] || [];
        ipcEventBus[eventName].push(cb);
      },
      off: (eventName: string, cb: AnyFn) => {
        if (ipcEventBus[eventName]) {
          ipcEventBus[eventName] = ipcEventBus[eventName].filter(c => c !== cb);
        }
      },
      callRemoteClientFunction: async (_ctx: unknown, fnName: unknown, ...args: unknown[]) => {
        if (typeof target['callRemoteClientFunction'] === 'function') {
          return (target['callRemoteClientFunction'] as AnyFn)(_ctx, fnName, ...args);
        }
        return Promise.resolve(['']);
      },
      callRemoteServerFunction: async (_ctx: unknown, fnName: unknown, ...args: unknown[]) => {
        if (typeof target['callRemoteServerFunction'] === 'function') {
          return (target['callRemoteServerFunction'] as AnyFn)(_ctx, fnName, ...args);
        }
        return Promise.resolve(['']);
      },
      registerFunctions: (fns: Record<string, AnyFn>) => {
        if (fns && typeof fns === 'object') {
          Object.assign(registeredRpcFunctions, fns);
        }
      },
    },
    server: {
      registerFunctions: (fns: Record<string, AnyFn>) => {
        if (fns && typeof fns === 'object') {
          Object.assign(registeredRpcFunctions, fns);
        }
      },
      broadcastEvent: (eventName: string, ...args: unknown[]) => {
        ipcBroadcast(eventName, ...args);
      },
      emit: (eventName: string, ...args: unknown[]) => {
        ipcBroadcast(eventName, ...args);
      },
      on: (eventName: string, cb: AnyFn) => {
        ipcEventBus[eventName] = ipcEventBus[eventName] || [];
        ipcEventBus[eventName].push(cb);
      },
      off: (eventName: string, cb: AnyFn) => {
        if (ipcEventBus[eventName]) {
          ipcEventBus[eventName] = ipcEventBus[eventName].filter(c => c !== cb);
        }
      },
    },
    NetIpc: class {
      start(_opts: unknown, _name: unknown, _flag: unknown, cb?: AnyFn) {
        if (typeof cb === 'function') cb('connect');
      }
      call(methodName: string, ...args: unknown[]) {
        if (registeredRpcFunctions[methodName]) {
          return registeredRpcFunctions[methodName]({}, ...args);
        }
        return Promise.resolve(null);
      }
      registerFunctions(fns: Record<string, AnyFn>) {
        if (fns && typeof fns === 'object') {
          Object.assign(registeredRpcFunctions, fns);
        }
      }
      broadcastEvent(eventName: string, ...args: unknown[]) {
        ipcBroadcast(eventName, ...args);
      }
    },
  };

  return new Proxy(target, {
    get(t, prop: string | symbol) {
      if (typeof prop === 'string') {
        if (prop in t) {
          return t[prop];
        }
        if (prop === 'default' || prop === '__esModule') {
          return t;
        }
        if (prop.startsWith('get') || prop.startsWith('is') || prop.startsWith('has')) {
          return () => inMemoryStore[prop] ?? null;
        }
        if (prop.startsWith('set')) {
          return (val: unknown) => { inMemoryStore[prop] = val; };
        }
        return (...args: unknown[]) => {
          const lastArg = args[args.length - 1];
          if (typeof lastArg === 'function') {
            window.setTimeout(() => (lastArg as AnyFn)(null, null), 0);
          }
          return Promise.resolve(null);
        };
      }
      return Reflect.get(t, prop);
    },
  });
}
