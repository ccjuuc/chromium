  // --- 7. Process Object ---
  const processEmitter = new EventEmitter();
  const process = globalThis.process = {
    isMainFrame: injectedPaths.isMainFrame !== false,
    platform: runtimePlatform,
    arch: runtimeArch,
    type: 'renderer',
    versions: Object.assign({}, injectedPaths.versions),
    version: injectedPaths.versions?.node ? 'v' + injectedPaths.versions.node : undefined,
    env: {
      NODE_ENV: 'production',
      APPDATA: appData,
      LOCALAPPDATA: localAppData,
      TEMP: tempDir,
      APP_BASE_DIR: exeDir,
      ...(homeDir ? {USERPROFILE: homeDir, HOME: homeDir} : {}),
    },
    pid: injectedPaths.pid,
    execPath,
    argv: [execPath],
    cwd() {
      if (typeof injectedPaths.cwd === 'string' && injectedPaths.cwd) return injectedPaths.cwd;
      throw Object.assign(new Error('Host working directory is unavailable'), {code: 'ERR_NOT_SUPPORTED'});
    },
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
    contextId: injectedPaths.contextId,
    contextIsolated: injectedPaths.contextIsolated,
    sandboxed: injectedPaths.sandboxed,
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

  // The standard renderer dialog API delegates to the Browser's picker.
  // Application RPC methods continue through their original IPC handlers.
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
