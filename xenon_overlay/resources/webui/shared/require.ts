// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* Dynamic Node N-API / Mojo wire bridge; values are intentionally untyped. */
/* eslint-disable @typescript-eslint/no-explicit-any */

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from './xenon_node.mojom-webui.js';
import type {NodeExportInfo, NodeInvokeArg, PageHandlerInterface} from './xenon_node.mojom-webui.js';
import type {Value} from 'chrome://resources/mojo/mojo/public/mojom/base/values.mojom-webui.js';

export type {NodeExportInfo};

type PendingLoad = {
  resolve: (addon: any) => void;
  reject: (error: Error) => void;
};

type PendingResult = {
  resolve: (value: any) => void; reject: (error: Error) => void;
};

type PendingInspect = {
  modulePath: string; exportPath: string;
  resolve: (value: NodeExportInfo|null) => void;
  reject: (error: Error) => void;
};

type PendingConstruct = {
  resolve: (instanceId: number) => void; reject: (error: Error) => void;
};

type PendingMany = {
  resolve: (value: any[]) => void;
  reject: (error: Error) => void;
};

type ModuleLoadedListener = (
    path: string, success: boolean, errorMsg: string,
    exportsList: NodeExportInfo[]) => void;

type ModuleRecord = {
  path: string; target: Record<string, any>; proxy: any;
  loadPromise: Promise<any>;
  loadState: 'loading' | 'loaded' | 'failed';
  exports: string[];
  exportTree: NodeExportInfo[];
  pendingLoad: PendingLoad;
};

type InstanceState = {
  modulePath: string; ready: Promise<number>; serviceGeneration: number;
  released: boolean;
};

type FinalizedInstance = {
  modulePath: string; instanceId: number; serviceGeneration: number;
};

const WIRE_TYPE_KEY = '__xenon_node_wire_type__';
const WIRE_VALUE_KEY = 'value';

const moduleCache = new Map<string, ModuleRecord>();
const pendingInvokes = new Map<number, PendingResult>();
const pendingInspects = new Map<number, PendingInspect>();
const pendingConstructs = new Map<number, PendingConstruct>();
const pendingManys = new Map<number, PendingMany>();
const pendingPropertyReads = new Map<number, PendingResult>();
const pendingPropertyWrites = new Map<number, PendingResult>();
const moduleLoadedListeners = new Set<ModuleLoadedListener>();
const callbackIds = new WeakMap<Function, number>();
const callbacks = new Map<number, Function>();
const instanceStates = new WeakMap<object, InstanceState>();
let nextRequestId = 1;
let nextCallbackId = 1;
let serviceGeneration = 0;

const callbackRouter = new PageCallbackRouter();
const pageHandler: PageHandlerInterface = new PageHandlerRemote();

PageHandlerFactory.getRemote().createPageHandler(
    callbackRouter.$.bindNewPipeAndPassRemote(),
    (pageHandler as PageHandlerRemote).$.bindNewPipeAndPassReceiver());

export interface PlayerHostHandles {
  floatWindow: number;
  parentWindow: number;
}

export async function preparePlayerHost(): Promise<PlayerHostHandles> {
  const {floatWindow, parentWindow, errorMsg} =
      await pageHandler.preparePlayerHost();
  if (errorMsg) {
    throw new Error(errorMsg);
  }

  const handles = {
    floatWindow: Number(floatWindow),
    parentWindow: Number(parentWindow),
  };
  if (!Number.isSafeInteger(handles.floatWindow) ||
      !Number.isSafeInteger(handles.parentWindow) || handles.floatWindow <= 0 ||
      handles.parentWindow <= 0) {
    throw new Error('Browser returned invalid native player window handles');
  }
  return handles;
}

export async function bindPlayerVideoWindow(playerWindow: number):
    Promise<void> {
  if (!Number.isSafeInteger(playerWindow) || playerWindow <= 0) {
    throw new Error('Invalid native player window handle');
  }
  const {errorMsg} =
      await pageHandler.bindPlayerVideoWindow(String(playerWindow));
  if (errorMsg) {
    throw new Error(errorMsg);
  }
}

export async function showPlayerVideoHost(show: boolean): Promise<void> {
  await pageHandler.showPlayerVideoHost(show);
}

export async function controlPlayerWindow(
    action: string, flag = false): Promise<boolean> {
  const {state, errorMsg} =
      await pageHandler.controlPlayerWindow(action, flag);
  if (errorMsg) {
    throw new Error(errorMsg);
  }
  return state;
}

export async function openNativeFileDialog(
    title: string, filterExtensions: string[] = [],
    allowMulti = false): Promise<string[]> {
  const {filePaths} = await pageHandler.openNativeFileDialog(
      title, filterExtensions, allowMulti);
  return filePaths;
}

export async function scanDirectoryVideos(dirPath: string): Promise<string[]> {
  const {videoPaths} = await pageHandler.scanDirectoryVideos(dirPath);
  return videoPaths;
}

const instanceFinalizer = typeof FinalizationRegistry === 'undefined' ?
    null :
    new FinalizationRegistry<FinalizedInstance>(
        ({modulePath, instanceId, serviceGeneration: instanceGeneration}) => {
          if (instanceGeneration === serviceGeneration) {
            pageHandler.releaseNodeInstance(modulePath, instanceId);
          }
        });

function normalizeNodePath(path: string): string {
  const trimmedPath = path.trim();
  if (navigator.platform.startsWith('Win')) {
    return trimmedPath.replace(/\//g, '\\').toLowerCase();
  }
  return trimmedPath;
}

function wireDictionary(storage: {[key: string]: Value}): Value {
  return {dictionaryValue: {storage}};
}

function taggedWireValue(type: string, value?: Value): Value {
  const storage: {[key: string]: Value} = {
    [WIRE_TYPE_KEY]: {stringValue: type},
  };
  if (value !== undefined) {
    storage[WIRE_VALUE_KEY] = value;
  }
  return wireDictionary(storage);
}

function withCycleCheck<T>(
    value: object, seen: Set<object>, convert: () => T): T {
  if (seen.has(value)) {
    throw new TypeError('Cyclic values cannot cross the Xenon Node wire');
  }
  seen.add(value);
  try {
    return convert();
  } finally {
    seen.delete(value);
  }
}

function valueToWire(value: any, seen = new Set<object>()): Value {
  if (value === undefined) {
    return taggedWireValue('undefined');
  }
  if (value === null) {
    return {nullValue: 0};
  }
  if (typeof value === 'boolean') {
    return {boolValue: value};
  }
  if (typeof value === 'number') {
    if (Number.isNaN(value)) {
      return taggedWireValue('number', {stringValue: 'nan'});
    }
    if (value === Infinity || value === -Infinity) {
      return taggedWireValue(
          'number', {stringValue: value > 0 ? 'infinity' : '-infinity'});
    }
    if (Object.is(value, -0)) {
      return taggedWireValue('number', {stringValue: '-0'});
    }
    if (Number.isInteger(value) && value >= -2147483648 &&
        value <= 2147483647) {
      return {intValue: value};
    }
    return {doubleValue: value};
  }
  if (typeof value === 'bigint') {
    return taggedWireValue('bigint', {stringValue: value.toString()});
  }
  if (typeof value === 'string') {
    return {stringValue: value};
  }
  if (typeof value === 'function' || typeof value === 'symbol') {
    throw new TypeError(`Unsupported native argument type: ${typeof value}`);
  }
  if (value instanceof ArrayBuffer) {
    return {binaryValue: Array.from(new Uint8Array(value))};
  }
  if (ArrayBuffer.isView(value)) {
    return {
      binaryValue: Array.from(
          new Uint8Array(value.buffer, value.byteOffset, value.byteLength)),
    };
  }
  if (value instanceof Date) {
    return taggedWireValue('date', valueToWire(value.getTime(), seen));
  }
  if (value instanceof Map) {
    return withCycleCheck(
        value, seen,
        () => taggedWireValue('map', {
          listValue: {
            storage: Array.from(value, ([key, item]) => ({
                                         listValue: {
                                           storage: [
                                             valueToWire(key, seen),
                                             valueToWire(item, seen),
                                           ],
                                         },
                                       })),
          },
        }));
  }
  if (value instanceof Set) {
    return withCycleCheck(
        value, seen,
        () => taggedWireValue('set', {
          listValue: {
            storage: Array.from(value, item => valueToWire(item, seen)),
          },
        }));
  }
  if (Array.isArray(value)) {
    return withCycleCheck(
        value, seen,
        () => ({
          listValue: {storage: value.map(item => valueToWire(item, seen))},
        }));
  }
  if (typeof value === 'object') {
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
      throw new TypeError(`Unsupported native argument object: ${
          value.constructor?.name ?? 'unknown'}`);
    }
    return withCycleCheck(value, seen, () => {
      // Mojo's JS map encoder reads `value.constructor.name`, so the wire map
      // must retain Object.prototype. Define keys explicitly to avoid invoking
      // the legacy __proto__ setter.
      const storage: {[key: string]: Value} = {};
      for (const [key, item] of Object.entries(value)) {
        Object.defineProperty(storage, key, {
          configurable: true,
          enumerable: true,
          value: valueToWire(item, seen),
          writable: true,
        });
      }
      return wireDictionary(storage);
    });
  }

  throw new TypeError(`Unsupported native argument type: ${typeof value}`);
}

function wireString(value: Value|undefined): string|undefined {
  if (!value || value.stringValue === null || value.stringValue === undefined) {
    return undefined;
  }
  return value.stringValue;
}

function valueFromWire(value: Value): any {
  if (value.nullValue !== null && value.nullValue !== undefined) {
    return null;
  }
  if (value.boolValue !== null && value.boolValue !== undefined) {
    return value.boolValue;
  }
  if (value.intValue !== null && value.intValue !== undefined) {
    return value.intValue;
  }
  if (value.doubleValue !== null && value.doubleValue !== undefined) {
    return value.doubleValue;
  }
  if (value.stringValue !== null && value.stringValue !== undefined) {
    return value.stringValue;
  }
  if (value.binaryValue !== null && value.binaryValue !== undefined) {
    return new Uint8Array(value.binaryValue);
  }
  if (value.listValue !== null && value.listValue !== undefined) {
    return value.listValue.storage.map(item => valueFromWire(item));
  }
  if (value.dictionaryValue !== null && value.dictionaryValue !== undefined) {
    const storage = value.dictionaryValue.storage;
    const wireType = wireString(storage[WIRE_TYPE_KEY]);
    if (wireType === 'undefined') {
      return undefined;
    }
    if (wireType === 'bigint') {
      return BigInt(wireString(storage[WIRE_VALUE_KEY]) ?? '0');
    }
    if (wireType === 'date') {
      return new Date(valueFromWire(storage[WIRE_VALUE_KEY]!));
    }
    if (wireType === 'map') {
      return new Map(valueFromWire(storage[WIRE_VALUE_KEY]!));
    }
    if (wireType === 'set') {
      return new Set(valueFromWire(storage[WIRE_VALUE_KEY]!));
    }
    if (wireType === 'number') {
      switch (wireString(storage[WIRE_VALUE_KEY])) {
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

    const result: Record<string, any> = Object.create(null);
    for (const [key, item] of Object.entries(storage)) {
      result[key] = valueFromWire(item);
    }
    return result;
  }
  return undefined;
}

function callbackArg(callback: Function): NodeInvokeArg {
  let callbackId = callbackIds.get(callback);
  if (callbackId === undefined) {
    callbackId = nextCallbackId++;
    callbackIds.set(callback, callbackId);
  }
  callbacks.set(callbackId, callback);
  return {
    isCallback: true,
    callbackId,
    value: {nullValue: 0},
  };
}

function toInvokeArgs(args: any[]): NodeInvokeArg[] {
  return args.map(arg => {
    if (typeof arg === 'function') {
      return callbackArg(arg);
    }
    return {
      isCallback: false,
      callbackId: 0,
      value: valueToWire(arg),
    };
  });
}

function nextRequest(): number {
  return nextRequestId++;
}

function invokeExport(
    modulePath: string, functionName: string, args: any[]): Promise<any> {
  const record = moduleCache.get(modulePath);
  if (!record) {
    return Promise.reject(
        new Error(`Module has not been required: ${modulePath}`));
  }

  return record.loadPromise.then(() => {
    const requestId = nextRequest();
    return new Promise((resolve, reject) => {
      pendingInvokes.set(requestId, {resolve, reject});
      pageHandler.invokeNodeExport(
          requestId, modulePath, functionName, toInvokeArgs(args));
    });
  });
}

function constructExportId(
    modulePath: string, exportPath: string, args: any[]): Promise<number> {
  const requestId = nextRequest();
  return new Promise((resolve, reject) => {
    pendingConstructs.set(requestId, {resolve, reject});
    pageHandler.constructNodeExport(
        requestId, modulePath, exportPath, toInvokeArgs(args));
  });
}

function invokeInstance(
    state: InstanceState, methodName: string, args: any[]): Promise<any> {
  if (state.released || state.serviceGeneration !== serviceGeneration) {
    return Promise.reject(new Error('Native instance has been released'));
  }
  return state.ready.then(instanceId => {
    if (state.released || state.serviceGeneration !== serviceGeneration) {
      throw new Error('Native instance has been released');
    }
    const requestId = nextRequest();
    return new Promise((resolve, reject) => {
      pendingInvokes.set(requestId, {resolve, reject});
      pageHandler.invokeNodeInstance(
          requestId, state.modulePath, instanceId, methodName,
          toInvokeArgs(args));
    });
  });
}

function getInstanceProperty(
    state: InstanceState, propertyName: string): Promise<any> {
  if (state.released || state.serviceGeneration !== serviceGeneration) {
    return Promise.reject(new Error('Native instance has been released'));
  }
  return state.ready.then(instanceId => {
    const requestId = nextRequest();
    return new Promise((resolve, reject) => {
      pendingPropertyReads.set(requestId, {resolve, reject});
      pageHandler.getNodeInstanceProperty(
          requestId, state.modulePath, instanceId, propertyName);
    });
  });
}

function setInstanceProperty(
    state: InstanceState, propertyName: string, value: any): Promise<void> {
  if (state.released || state.serviceGeneration !== serviceGeneration) {
    return Promise.reject(new Error('Native instance has been released'));
  }
  return state.ready.then(instanceId => {
    const requestId = nextRequest();
    return new Promise((resolve, reject) => {
      pendingPropertyWrites.set(requestId, {resolve, reject});
      pageHandler.setNodeInstanceProperty(
          requestId, state.modulePath, instanceId, propertyName,
          valueToWire(value));
    });
  });
}

function getExportProperty(
    modulePath: string, objectPath: string,
    propertyName: string): Promise<any> {
  const requestId = nextRequest();
  return new Promise((resolve, reject) => {
    pendingPropertyReads.set(requestId, {resolve, reject});
    pageHandler.getNodeExportProperty(
        requestId, modulePath, objectPath, propertyName);
  });
}

function setExportProperty(
    modulePath: string, objectPath: string, propertyName: string,
    value: any): Promise<void> {
  const requestId = nextRequest();
  return new Promise((resolve, reject) => {
    pendingPropertyWrites.set(requestId, {resolve, reject});
    pageHandler.setNodeExportProperty(
        requestId, modulePath, objectPath, propertyName, valueToWire(value));
  });
}

function reportAsyncError(promise: Promise<unknown>) {
  promise.catch(error => {
    queueMicrotask(() => {
      throw error;
    });
  });
}

function requireInstanceState(instance: object): InstanceState {
  const state = instanceStates.get(instance);
  if (!state) {
    throw new TypeError('Invalid receiver for native class member');
  }
  if (state.serviceGeneration !== serviceGeneration) {
    state.released = true;
    throw new Error('Native instance was invalidated when Utility restarted');
  }
  return state;
}

function releaseInstance(
    state: InstanceState, unregisterToken: object): Promise<void> {
  if (state.released) {
    return Promise.resolve();
  }
  state.released = true;
  instanceFinalizer?.unregister(unregisterToken);
  return state.ready.then(instanceId => {
    if (state.serviceGeneration === serviceGeneration) {
      pageHandler.releaseNodeInstance(state.modulePath, instanceId);
    }
  });
}

function defineInstanceMember(prototype: object, info: NodeExportInfo): void {
  if (info.kind === 'function') {
    Object.defineProperty(prototype, info.name, {
      configurable: true,
      enumerable: info.enumerable,
      writable: false,
      value: function(this: object, ...args: any[]) {
        return invokeInstance(requireInstanceState(this), info.name, args);
      },
    });
    return;
  }

  Object.defineProperty(prototype, info.name, {
    configurable: true,
    enumerable: info.enumerable,
    get(this: object) {
      return getInstanceProperty(requireInstanceState(this), info.name);
    },
    set: info.writable ?
        function(this: object, value: any) {
          reportAsyncError(setInstanceProperty(
              requireInstanceState(this), info.name, value));
        } :
        undefined,
  });
}

function createClassExport(
    modulePath: string, classPath: string, info: NodeExportInfo): any {
  const target = function XenonNativeClass() {};
  Object.defineProperty(target, 'name', {
    configurable: true,
    value: classPath.split('.').at(-1) ?? classPath,
  });

  for (const member of info.prototype) {
    defineInstanceMember(target.prototype, member);
  }
  Object.defineProperties(target.prototype, {
    __xenonReady: {
      configurable: true,
      get(this: object) {
        return requireInstanceState(this).ready;
      },
    },
    __xenonInstanceId: {
      configurable: true,
      get(this: object) {
        return requireInstanceState(this).ready;
      },
    },
    $get: {
      configurable: true,
      value: function(this: object, propertyName: string) {
        return getInstanceProperty(requireInstanceState(this), propertyName);
      },
    },
    $set: {
      configurable: true,
      value: function(this: object, propertyName: string, value: any) {
        return setInstanceProperty(
            requireInstanceState(this), propertyName, value);
      },
    },
    $invokePath: {
      configurable: true,
      value: function(this: object, methodPath: string, ...args: any[]) {
        if (typeof methodPath !== 'string' ||
            methodPath.split('.').some(part => !part)) {
          return Promise.reject(new TypeError('Invalid native method path'));
        }
        return invokeInstance(
            requireInstanceState(this),
            `$xenonInvokePath:${methodPath}`, args);
      },
    },
    $dispose: {
      configurable: true,
      value: function(this: object) {
        return releaseInstance(requireInstanceState(this), this);
      },
    },
  });

  const classProxy = new Proxy(target, {
    construct(_target, args, newTarget) {
      const instanceTarget = Object.create(newTarget.prototype);
      const state: InstanceState = {
        modulePath,
        ready: constructExportId(modulePath, classPath, [...args]),
        serviceGeneration,
        released: false,
      };
      state.ready.catch(() => {});

      const instanceProxy = new Proxy(instanceTarget, {
        get(obj, property, receiver) {
          if (property === 'then') {
            return undefined;
          }
          if (Reflect.has(obj, property)) {
            return Reflect.get(obj, property, receiver);
          }
          if (typeof property === 'symbol') {
            return Reflect.get(obj, property, receiver);
          }
          return getInstanceProperty(state, property);
        },
        set(obj, property, value, receiver) {
          if (typeof property === 'symbol' || Reflect.has(obj, property)) {
            return Reflect.set(obj, property, value, receiver);
          }
          reportAsyncError(setInstanceProperty(state, property, value));
          return true;
        },
      });
      instanceStates.set(instanceTarget, state);
      instanceStates.set(instanceProxy, state);
      state.ready.then(instanceId => {
        if (state.released) {
          pageHandler.releaseNodeInstance(modulePath, instanceId);
          return;
        }
        instanceFinalizer?.register(
            instanceProxy, {
              modulePath,
              instanceId,
              serviceGeneration: state.serviceGeneration
            },
            instanceProxy);
      }, () => {});
      return instanceProxy;
    },
    apply() {
      throw new TypeError(
          `Class constructor ${classPath} cannot be invoked without 'new'`);
    },
  });

  defineChildExports(classProxy, modulePath, classPath, info.children);
  return classProxy;
}

function createExportFunction(modulePath: string, functionName: string) {
  return (...args: any[]) => invokeExport(modulePath, functionName, args);
}

function primitiveExportValue(info: NodeExportInfo): any {
  if (info.kind === 'undefined') {
    return undefined;
  }
  if (info.kind === 'bigint') {
    return BigInt(valueFromWire(info.value));
  }
  return valueFromWire(info.value);
}

function createExportObject(
    modulePath: string, objectPath: string, info: NodeExportInfo): any {
  const target: Record<string, any> = {};
  defineChildExports(target, modulePath, objectPath, [
    ...info.children,
    ...info.prototype.filter(
        member => !info.children.some(child => child.name === member.name)),
  ]);
  return new Proxy(target, {
    get(obj, property, receiver) {
      if (property === 'then') {
        return undefined;
      }
      if (Reflect.has(obj, property)) {
        return Reflect.get(obj, property, receiver);
      }
      if (typeof property === 'symbol') {
        return Reflect.get(obj, property, receiver);
      }
      return getExportProperty(modulePath, objectPath, property);
    },
    set(obj, property, value, receiver) {
      if (typeof property === 'symbol' || Reflect.has(obj, property)) {
        return Reflect.set(obj, property, value, receiver);
      }
      reportAsyncError(
          setExportProperty(modulePath, objectPath, property, value));
      return true;
    },
  });
}

function createExportValue(
    modulePath: string, qualifiedName: string, info: NodeExportInfo): any {
  if (info.kind === 'class') {
    return createClassExport(modulePath, qualifiedName, info);
  }
  if (info.kind === 'function') {
    const fn = createExportFunction(modulePath, qualifiedName);
    defineChildExports(fn, modulePath, qualifiedName, info.children);
    return fn;
  }
  if (info.kind === 'object') {
    return createExportObject(modulePath, qualifiedName, info);
  }
  return primitiveExportValue(info);
}

function defineChildExports(
    target: object, modulePath: string, objectPath: string,
    children: NodeExportInfo[]): void {
  for (const child of children) {
    if (child.kind === 'property') {
      Object.defineProperty(target, child.name, {
        configurable: true,
        enumerable: child.enumerable,
        get() {
          return getExportProperty(modulePath, objectPath, child.name);
        },
        set: child.writable ?
            (value: any) => {
              reportAsyncError(
                  setExportProperty(modulePath, objectPath, child.name, value));
            } :
            undefined,
      });
      continue;
    }
    const childPath = objectPath ? `${objectPath}.${child.name}` : child.name;
    let currentValue = createExportValue(modulePath, childPath, child);
    if (child.writable) {
      Object.defineProperty(target, child.name, {
        configurable: true,
        enumerable: child.enumerable,
        get() {
          return currentValue;
        },
        set(value: any) {
          reportAsyncError(
              setExportProperty(modulePath, objectPath, child.name, value)
                  .then(() => {
                    currentValue = value;
                  }));
        },
      });
      continue;
    }
    Object.defineProperty(target, child.name, {
      configurable: true,
      enumerable: child.enumerable,
      writable: false,
      value: currentValue,
    });
  }
}

function defineExports(record: ModuleRecord, exportsList: NodeExportInfo[]) {
  const nextNames = new Set(exportsList.map(info => info.name));
  for (const name of record.exports) {
    if (!nextNames.has(name)) {
      Reflect.deleteProperty(record.target, name);
    }
  }

  record.exportTree = exportsList.slice();
  record.exports = [...nextNames];
  defineChildExports(record.target, record.path, '', exportsList);
}

function notifyModuleLoaded(
    path: string, success: boolean, errorMsg: string,
    exportsList: NodeExportInfo[]) {
  for (const listener of moduleLoadedListeners) {
    listener(path, success, errorMsg, exportsList);
  }
}

function createModuleRecord(modulePath: string): ModuleRecord {
  let pendingLoad!: PendingLoad;
  const target: Record<string, any> = {};
  const loadPromise = new Promise<any>((resolve, reject) => {
    pendingLoad = {resolve, reject};
  });

  const record: ModuleRecord = {
    path: modulePath,
    target,
    proxy: null,
    loadPromise,
    loadState: 'loading',
    exports: [],
    exportTree: [],
    pendingLoad,
  };

  record.proxy = new Proxy(target, {
    get(obj, property, receiver) {
      if (property === 'then') {
        return undefined;
      }
      if (property === '__xenonReady') {
        return record.loadPromise;
      }
      return Reflect.get(obj, property, receiver);
    },
    ownKeys() {
      return record.exports;
    },
    getOwnPropertyDescriptor(obj, property) {
      return Reflect.getOwnPropertyDescriptor(obj, property);
    },
  });

  return record;
}

callbackRouter.nodeModuleLoaded.addListener(
    (path: string, success: boolean, errorMsg: string,
     exportsList: NodeExportInfo[]) => {
      notifyModuleLoaded(path, success, errorMsg, exportsList);

      const modulePath = normalizeNodePath(path);
      const record = moduleCache.get(modulePath);
      if (!record) {
        return;
      }

      if (!success) {
        record.loadState = 'failed';
        moduleCache.delete(modulePath);
        record.pendingLoad.reject(
            new Error(errorMsg || `Failed to require native module: ${path}`));
        return;
      }

      record.loadState = 'loaded';
      defineExports(record, exportsList);
      record.pendingLoad.resolve(record.proxy);
    });

callbackRouter.nodeInvokeResult.addListener(
    (requestId: number, success: boolean, result: Value,
     callbackResults: Array<{callbackId: number, value: Value}>,
     errorMsg: string) => {
      const pending = pendingInvokes.get(requestId);
      if (!pending) {
        return;
      }
      pendingInvokes.delete(requestId);

      if (!success) {
        pending.reject(new Error(errorMsg || 'Native export invocation failed'));
        return;
      }

      // Compatibility for a Utility process from before callbacks moved to
      // NodeAddonObserver.
      for (const callbackResult of callbackResults) {
        callbacks.get(callbackResult.callbackId)
            ?.(valueFromWire(callbackResult.value));
      }
      pending.resolve(valueFromWire(result));
    });

callbackRouter.nodeCallbackInvoked.addListener(
    (callbackId: number, args: Value[]) => {
      const callback = callbacks.get(callbackId);
      if (!callback) {
        return;
      }
      try {
        callback(...args.map(valueFromWire));
      } catch (error) {
        queueMicrotask(() => {
          throw error;
        });
      }
    });

callbackRouter.nodeCallbackReleased.addListener((callbackId: number) => {
  callbacks.delete(callbackId);
});

function rejectPendingRequests(error: Error): void {
  const pendingMaps: Array<Map<number, PendingResult>> = [
    pendingInvokes,
    pendingPropertyReads,
    pendingPropertyWrites,
  ];
  for (const pendingMap of pendingMaps) {
    for (const pending of pendingMap.values()) {
      pending.reject(error);
    }
    pendingMap.clear();
  }
  for (const pending of pendingInspects.values()) {
    pending.reject(error);
  }
  pendingInspects.clear();
  for (const pending of pendingConstructs.values()) {
    pending.reject(error);
  }
  pendingConstructs.clear();
  for (const pending of pendingManys.values()) {
    pending.reject(error);
  }
  pendingManys.clear();
}

callbackRouter.nodeServiceReset.addListener(() => {
  ++serviceGeneration;
  callbacks.clear();
  rejectPendingRequests(new Error('Utility service restarted'));
});

callbackRouter.nodeInspectResult.addListener(
    (requestId: number, success: boolean, errorMsg: string,
     info: NodeExportInfo|null) => {
      const pending = pendingInspects.get(requestId);
      if (!pending) {
        return;
      }
      pendingInspects.delete(requestId);
      if (!success) {
        pending.reject(new Error(errorMsg || 'Inspect export failed'));
        return;
      }

      if (info && !pending.exportPath.includes('.')) {
        const record = moduleCache.get(pending.modulePath);
        if (record) {
          const exportsList = record.exportTree.map(
              item => item.name === info.name ? info : item);
          defineExports(record, exportsList);
        }
      }
      pending.resolve(info);
    });

callbackRouter.nodeConstructResult.addListener(
    (requestId: number, success: boolean, instanceId: number,
     errorMsg: string) => {
      const pending = pendingConstructs.get(requestId);
      if (!pending) {
        return;
      }
      pendingConstructs.delete(requestId);
      if (!success) {
        pending.reject(new Error(errorMsg || 'Construct export failed'));
        return;
      }
      pending.resolve(instanceId);
    });

callbackRouter.nodeInvokeManyResult.addListener(
    (requestId: number,
     results: Array<{success: boolean, result: Value, errorMsg: string}>) => {
      const pending = pendingManys.get(requestId);
      if (!pending) {
        return;
      }
      pendingManys.delete(requestId);
      try {
        pending.resolve(results.map(item => {
          if (!item.success) {
            throw new Error(item.errorMsg || 'Batched invoke failed');
          }
          return valueFromWire(item.result);
        }));
      } catch (error) {
        pending.reject(error as Error);
      }
    });

callbackRouter.nodePropertyResult.addListener(
    (requestId: number, success: boolean, result: Value, errorMsg: string) => {
      const pending = pendingPropertyReads.get(requestId);
      if (!pending) {
        return;
      }
      pendingPropertyReads.delete(requestId);
      if (!success) {
        pending.reject(new Error(errorMsg || 'Native property read failed'));
        return;
      }
      pending.resolve(valueFromWire(result));
    });

callbackRouter.nodeSetPropertyResult.addListener(
    (requestId: number, success: boolean, errorMsg: string) => {
      const pending = pendingPropertyWrites.get(requestId);
      if (!pending) {
        return;
      }
      pendingPropertyWrites.delete(requestId);
      if (!success) {
        pending.reject(new Error(errorMsg || 'Native property write failed'));
        return;
      }
      pending.resolve(undefined);
    });

export function require(path: string): any {
  const modulePath = normalizeNodePath(path);
  let record = moduleCache.get(modulePath);
  if (!record) {
    record = createModuleRecord(modulePath);
    moduleCache.set(modulePath, record);
    pageHandler.requireNodeModule(modulePath);
  }
  return record.proxy;
}

export function whenRequired(path: string): Promise<any> {
  const modulePath = normalizeNodePath(path);
  return require(modulePath).__xenonReady;
}

/** Export tree for a loaded module, or null if missing/failed. */
export function getModuleExportTree(path: string): NodeExportInfo[]|null {
  const record = moduleCache.get(normalizeNodePath(path));
  if (!record || record.loadState !== 'loaded') {
    return null;
  }
  return record.exportTree.slice();
}

/**
 * True if paths refer to the same module (basename match, case-insensitive).
 */
export function isSameModulePath(a: string, b: string): boolean {
  const na = normalizeNodePath(a);
  const nb = normalizeNodePath(b);
  if (na === nb) {
    return true;
  }
  const baseA = na.replace(/^.*\\/, '');
  const baseB = nb.replace(/^.*\\/, '');
  return !!baseA && baseA === baseB;
}

export function inspectExport(
    modulePath: string, exportPath: string): Promise<NodeExportInfo|null> {
  const normalizedPath = normalizeNodePath(modulePath);
  const requestId = nextRequest();
  return new Promise((resolve, reject) => {
    pendingInspects.set(requestId, {
      modulePath: normalizedPath,
      exportPath,
      resolve,
      reject,
    });
    pageHandler.inspectNodeExport(requestId, normalizedPath, exportPath);
  });
}

export function invokeMany(
    modulePath: string,
    calls: Array<{functionName: string, args?: any[]}>): Promise<any[]> {
  const requestId = nextRequest();
  const wireCalls =
      calls.map(call => ({
                  functionName: call.functionName,
                  args: (call.args ?? []).map(arg => {
                    if (typeof arg === 'function') {
                      throw new TypeError(
                          'invokeMany does not support callback arguments');
                    }
                    return {
                      isCallback: false,
                      callbackId: 0,
                      value: valueToWire(arg),
                    };
                  }),
                }));

  return new Promise((resolve, reject) => {
    pendingManys.set(requestId, {resolve, reject});
    pageHandler.invokeNodeExports(
        requestId, normalizeNodePath(modulePath), wireCalls);
  });
}

export function addNodeModuleLoadedListener(listener: ModuleLoadedListener) {
  moduleLoadedListeners.add(listener);
}

export function removeNodeModuleLoadedListener(listener: ModuleLoadedListener) {
  moduleLoadedListeners.delete(listener);
}

if (typeof window !== 'undefined') {
  (window as any).require = require;
}
