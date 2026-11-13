// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(() => {
  'use strict';
  const isNodeWorkerRole = Number(globalThis.__xenonWorkerThreadId || 0) > 0;
  const isUtilityProcessRole = globalThis.__xenonUtilityProcess === true;
  if (isNodeWorkerRole && isUtilityProcessRole)
    throw new Error('An independent utility process cannot use the Worker role');
  // @include "main/utility_runtime.js"

  // @include "common/timers.js"
  const nativeNodeTimers = createNodeTimers({
    setTimeout: globalThis.setTimeout,
    clearTimeout: globalThis.clearTimeout,
    setInterval: globalThis.setInterval,
    clearInterval: globalThis.clearInterval,
    setImmediate: globalThis.setImmediate,
    clearImmediate: globalThis.clearImmediate,
    setReferenced: globalThis.__xenonSetTimerReferenced,
    warn: (message, type) => globalThis.process?.emitWarning?.(message, type),
  });
  Object.assign(globalThis, nativeNodeTimers);
  delete globalThis.__xenonSetTimerReferenced;

  // Node's lexer discovers CommonJS named exports without executing modules
  // while the ESM graph is being linked. Keep its upstream source unchanged.
  const commonJsExportLexer = (() => {
    const module = {exports: {}};
    // @include "third_party/cjs_module_lexer/lexer.js"
    return module.exports;
  })();
  globalThis.__xenonParseCommonJsExports = commonJsExportLexer.parse;

  // @include "main/events.js"
  // @include "common/events_helpers.js"

  // @include "common/web_streams.js"
  let getWebStreamState;
  const webStreamsModule = createWebStreamsModule(globalThis, () => {
    const module = {exports: {}};
    const exports = module.exports;
    // @include "third_party/web_streams_polyfill/dist/ponyfill.js"
    return module.exports;
  }, reader => { getWebStreamState = reader; });

  // @include "main/event_target.js"
  // @include "main/dom_exception.js"
  const MainDOMException = createMainDOMExceptionClass();
  globalThis.DOMException = MainDOMException;
  Object.assign(globalThis, createMainEventClasses(error => {
    queueMicrotask(() => {
      if (globalThis.process?.emit('uncaughtException', error, 'uncaughtException')) return;
      throw error;
    });
  }, () => globalThis.performance?.now() ?? Date.now()));

  // @include "main/async_context.js"
  // @include "main/diagnostics_channel.js"
  const diagnosticsChannelModule = createDiagnosticsChannel(error => {
    process.nextTick(() => {
      if (process.emit('uncaughtException', error, 'uncaughtException')) return;
      throw error;
    });
  });

  // @include "main/platform.js"

  // @include "main/app_command_line.js"
  // @include "main/app.js"
  // @include "main/sandbox.js"
  const electronSandbox = createElectronSandboxApi({isReady: () => appReady,
    callHost: request => callBrowserWindow({id: 0}, 'electron-sandbox', request)});
  app.enableSandbox = electronSandbox.enableSandbox;

  // @include "main/protocol.js"
  // @include "main/web_request.js"

  // @include "common/blob.js"
  const blobStreamConstructor = Object.getOwnPropertyDescriptor(webStreamsModule, 'ReadableStream');
  globalThis.Blob = createBlobConstructor({platform: __xenonPlatform,
    getReadableStream: blobStreamConstructor.get || (() => blobStreamConstructor.value)});
  globalThis.File = createFileConstructor({Blob: globalThis.Blob});
  // @include "common/clipboard.js"
  // @include "common/native_image.js"
  // @include "common/crash_reporter.js"

  // @include "main/menu.js"
  // @include "main/dialog.js"
  // @include "main/windows.js"
  // @include "main/permissions.js"
  // @include "main/views.js"
  if (!isNodeWorkerRole && !isUtilityProcessRole) {
    electronViews = createElectronViews({EventEmitter,
      isReady: () => appReady,
      callNative: request => {
        if (request.action === 'create' && appExitState !== 'running')
          throw Object.assign(new Error('Cannot create a View while the application exits'), {code: 'ERR_APP_QUITTING'});
        return callBrowserWindow({id: 0}, 'electron-view', request);
      },
      preparePreferences: provided => {
        const preferences = {contextIsolation: true, nodeIntegration: false, ...provided};
        if (preferences.session !== undefined) {
          if (!(preferences.session instanceof Session)) throw new TypeError('webPreferences.session must be a Session');
          preferences.partition = preferences.session.partition;
          delete preferences.session;
        }
        if (preferences.partition !== undefined && typeof preferences.partition !== 'string')
          throw new TypeError('webPreferences.partition must be a string');
        if (preferences.preload && (typeof preferences.preload !== 'string' || !pathModule.isAbsolute(preferences.preload)))
          throw new TypeError('WebContentsView preload must be an absolute path');
        return electronSandbox.normalizePreferences(preferences);
      },
      createContents: (nativeId, preferences) => {
        if (!Number.isInteger(nativeId) || nativeId >= 0 || guestWebContents.has(nativeId))
          throw new Error('Invalid WebContentsView WebContents identity');
        const contents = new WebContents(null);
        contents._guest = {nativeId, host: null, standalone: true, preferences: {...preferences}};
        contents.session = getSession(preferences.partition || '');
        guestWebContents.set(nativeId, contents);
        const userAgent = contents.session.getUserAgent();
        if (userAgent) contents.setUserAgent(userAgent);
        return contents;
      },
      getContentsNativeId: contents => {
        if (!(contents instanceof WebContents) || contents.isDestroyed() || !contents._guest?.standalone)
          throw new TypeError('options.webContents must be an unattached WebContentsView WebContents');
        return contents._guest.nativeId;
      },
      getWindowId: window => window.id,
    });
    electronModule.View = electronViews.View;
    electronModule.WebContentsView = electronViews.WebContentsView;
    Object.defineProperty(BrowserWindow.prototype, 'contentView', {
      get() { return electronViews.contentView(this); },
    });
  }
  // @include "main/notification.js"
  if (!isNodeWorkerRole && !isUtilityProcessRole) {
    electronModule.Notification = createNotificationClass({EventEmitter,
      nativeImage: electronModule.nativeImage, isReady: () => appReady,
      callHost: request => callBrowserWindow({id: 0}, 'notification', request),
      createEvent: sender => createBrowserWindowEvent(sender),
    });
  }

  // @include "main/message_ports.js"
  if (!isNodeWorkerRole) installMessagePortMainModule();

  // @include "common/buffer_numeric.js"
  // @include "common/buffer.js"
  // @include "main/buffer.js"

  // @include "main/power_save_blocker.js"
  // @include "main/safe_storage.js"
  if (!isNodeWorkerRole && !isUtilityProcessRole) {
    electronModule.powerSaveBlocker = createPowerSaveBlockerApi({
      callHost: request => callBrowserWindow({id: 0}, 'powerSaveBlocker', request),
    });
    electronModule.safeStorage = createSafeStorageApi({Buffer,
      platform: __xenonPlatform, isReady: () => appReady,
      callHost: request => callBrowserWindow({id: 0}, 'safeStorage', request),
      callHostAsync: async request => {
        if (typeof globalThis.__xenonBrowserWindowCallAsync !== 'function')
          return electronUnsupported('safeStorage');
        try {
          return await __xenonBrowserWindowCallAsync(0, 'safeStorage', request);
        } catch (error) {
          const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
          if (code && !error.code) error.code = code[1];
          throw error;
        }
      },
    });
  }

  // @include "third_party/readable_stream/bundle.js"
  // @include "common/stream_web_adapters.js"
  // @include "common/node_streams.js"
  const nodeStreams = createNodeStreams({Buffer, EventEmitter,
    modules: createReadableStreamModules(), webStreams: webStreamsModule, getWebStreamState});
  const streamModule = nodeStreams.stream;
  const {Stream, Readable, Writable, Duplex, Transform, PassThrough} = streamModule;
  const stringDecoderModule = nodeStreams.stringDecoder;
  // @include "common/file_streams.js"

  // @include "common/fs_stats.js"
  // @include "main/fs_descriptors.js"
  // @include "common/fs_mkdtemp.js"
  // @include "common/native_fs.js"
  // @include "main/fs.js"


  // @include "common/promisify.js"
  // @include "common/crypto_random.js"
  // @include "main/builtins.js"

  // @include "main/content_tracing.js"
  // @include "main/desktop_capturer.js"
  if (!isNodeWorkerRole && !isUtilityProcessRole) {
    electronModule.contentTracing = createContentTracing({
      callHost: request => __xenonBrowserWindowCallAsync(0, 'content-tracing', request),
      resolvePath: value => pathModule.resolve(value),
    });
    electronModule.desktopCapturer = createDesktopCapturer({
      callHost: request => __xenonBrowserWindowCallAsync(0, 'desktop-capturer', request),
      nativeImage: electronModule.nativeImage, isReady: () => appReady,
    });
  }

  // @include "common/x509_certificate.js"
  cryptoModule.X509Certificate = createX509CertificateClass({Buffer,
    call: (...args) => __xenonX509CertificateCall(...args),
    markAsUncloneable: globalThis.__xenonMarkAsUncloneable,
  });

  // @include "main/web_crypto.js"
  const mainWebCrypto = createMainWebCrypto({
    randomBytes: size => cryptoModule.randomBytes(size),
    randomUUID: () => cryptoModule.randomUUID(),
    digest: (algorithm, bytes) => __xenonCryptoDigest(algorithm, bytes, null),
    DOMException: MainDOMException,
  });
  Object.defineProperty(cryptoModule, 'webcrypto', {enumerable: true,
    get: () => mainWebCrypto.crypto});
  Object.defineProperty(cryptoModule, 'subtle', {enumerable: true,
    get: () => mainWebCrypto.crypto.subtle});
  cryptoModule.getRandomValues = array => mainWebCrypto.crypto.getRandomValues(array);

  // @include "common/deep_equal.js"
  const nativeUtilTypes = {
    isPromise: globalThis.__xenonIsPromise,
    isNativeError: globalThis.__xenonIsNativeError,
  };
  for (const name of Object.keys(nativeUtilTypes)) {
    if (typeof nativeUtilTypes[name] === 'function')
      utilModule.types[name] = nativeUtilTypes[name];
  }
  if (typeof globalThis.__xenonUtilType === 'function') {
    for (const name of ['AnyArrayBuffer', 'ArgumentsObject', 'ArrayBuffer',
      'ArrayBufferView', 'AsyncFunction', 'BigInt64Array', 'BigIntObject',
      'BigUint64Array', 'BooleanObject', 'BoxedPrimitive', 'DataView', 'Date',
      'External', 'Float16Array', 'Float32Array', 'Float64Array', 'GeneratorFunction',
      'GeneratorObject', 'Int16Array', 'Int32Array', 'Int8Array', 'Map', 'MapIterator',
      'ModuleNamespaceObject', 'NativeError', 'NumberObject', 'Promise', 'Proxy',
      'RegExp', 'Set', 'SetIterator', 'SharedArrayBuffer', 'StringObject',
      'SymbolObject', 'TypedArray', 'Uint16Array', 'Uint32Array', 'Uint8Array',
      'Uint8ClampedArray', 'WeakMap', 'WeakSet']) {
      utilModule.types['is' + name] = value => __xenonUtilType('is' + name, value);
    }
  }
  utilModule.isDeepStrictEqual = createIsDeepStrictEqual(nativeUtilTypes);

  // @include "common/util_text.js"
  utilModule.stripVTControlCharacters = stripVTControlCharacters;
  // @include "common/util_debuglog.js"
  // @include "common/console.js"

  // @include "common/module.js"
  // @include "common/package_maps.js"
  globalThis.__xenonResolvePackageMap = resolvePackageMap;
  const moduleModule = createModuleModule(pathModule, urlModule,
      filename => __xenonCreateRequire(filename),
      name => __xenonIsBuiltinModule(name));
  globalThis.__xenonDefaultModuleLookupPaths = moduleModule._resolveLookupPaths;

  // @include "main/module_hooks.js"
  const synchronousModuleHooks = createSynchronousModuleHooks({
    changed: () => globalThis.__xenonModuleHooksChanged?.(
        synchronousModuleHooks.hasResolveHooks(), synchronousModuleHooks.hasLoadHooks()),
    decodeSource: value => {
      const bytes = ArrayBuffer.isView(value) ?
        new Uint8Array(value.buffer, value.byteOffset, value.byteLength) : new Uint8Array(value);
      return Buffer.from(bytes).toString('utf8').replace(/^\uFEFF/, '');
    },
  });
  moduleModule.registerHooks = synchronousModuleHooks.registerHooks;
  globalThis.__xenonResolveWithHooks = (specifier, context, mode) =>
    synchronousModuleHooks.resolve(specifier, context,
        (request, options) => __xenonDefaultModuleResolve(request, options, mode));
  globalThis.__xenonLoadWithHooks = (url, context, requireMode) =>
    synchronousModuleHooks.load(url, context,
        (request, options) => __xenonDefaultModuleLoad(request, options, requireMode));

  // @include "common/child_process.js"
  // @include "main/child_process.js"

  // @include "main/utility_process.js"
  if (!isNodeWorkerRole && !isUtilityProcessRole) {
    electronModule.utilityProcess = createUtilityProcessModule({EventEmitter,
      Readable: streamModule.Readable, Buffer,
      MessageChannelMain: electronModule.MessageChannelMain, path: pathModule});
  }

  function assert(condition, message) {
    if (!condition) throw new Error(message || 'Assertion failed');
  }
  assert.ok = assert;
  assert.strictEqual = (a, b, m) => { if (a !== b) throw new Error(m || `Expected ${a} === ${b}`); };
  assert.deepStrictEqual = (a, b, m) => assert.strictEqual(JSON.stringify(a), JSON.stringify(b), m);
  assert.equal = assert.strictEqual;
  assert.notEqual = (a, b, m) => { if (a === b) throw new Error(m || `Expected ${a} !== ${b}`); };

  // @include "common/zlib.js"
  const zlibModule = createZlibModule((operation, data, options, asynchronous) =>
      __xenonZlibCall(operation, data, options, asynchronous), {
        Transform: streamModule.Transform,
        streamCall: (request, asynchronous) =>
          __xenonZlibStreamCall(request, asynchronous, ''),
      });
  const constantsModule = Object.assign({}, fsConstants, zlibModule.constants);
  function rendererZlibOptions(options) {
    if (!options) return {};
    return options.dictionary === undefined ? options :
      {...options, dictionary: Buffer.from(options.dictionary, 'base64')};
  }
  ipcMain.on('__xenon:zlib', (event, request) => {
    event.returnValue = Buffer.from(__xenonZlibCall(request.operation,
        Buffer.from(request.dataBase64, 'base64'), rendererZlibOptions(request.options), false))
        .toString('base64');
  });
  ipcMain.handle('__xenon:zlib', async (_event, request) => {
    const result = await __xenonZlibCall(request.operation,
        Buffer.from(request.dataBase64, 'base64'), rendererZlibOptions(request.options), true);
    return Buffer.from(result).toString('base64');
  });
  function rendererZlibStreamRequest(event, request, asynchronous) {
    // Ownership comes from the document/worker dispatch identity. A numeric
    // codec id sent by another document never grants access to that codec.
    let owner = event.serviceWorker && event.serviceWorker._endpointId;
    if (!owner) {
      const state = webContentsState.get(event.sender);
      if (state) {
        for (const [endpointId, endpoint] of state.endpoints) {
          if (endpoint.frame === event.senderFrame) { owner = endpointId; break; }
        }
      }
    }
    if (!owner) throw new Error('ERR_STREAM_DESTROYED: Compression document is unavailable');
    const nativeRequest = {...request};
    if (request.action === 'process') {
      if (typeof request.data !== 'string' || request.data.length > 87384) {
        throw new Error('ERR_OUT_OF_RANGE: Compression input exceeds 64 KiB');
      }
      nativeRequest.data = Buffer.from(request.data, 'base64');
    }
    if (request.options && request.options.dictionary !== undefined) {
      nativeRequest.options = {...request.options,
        dictionary: Buffer.from(request.options.dictionary, 'base64')};
    }
    const result = __xenonZlibStreamCall(nativeRequest, asynchronous, owner);
    const encode = value => request.action === 'process'
      ? {...value, output: Buffer.from(value.output).toString('base64')} : value;
    return asynchronous ? Promise.resolve(result).then(encode) : encode(result);
  }
  ipcMain.on('__xenon:zlib-stream', (event, request) => {
    event.returnValue = rendererZlibStreamRequest(event, request, false);
  });
  ipcMain.handle('__xenon:zlib-stream', (event, request) =>
    rendererZlibStreamRequest(event, request, true));
  // @include "common/net.js"
  // @include "main/net.js"
  // @include "main/dgram.js"
  const dgramModule = createDgramModule({EventEmitter, Buffer, isIP: netModule.isIP,
    captureContext: () => currentAsyncContext, run: runWithAsyncContext,
    dispatch: callback => { globalThis.__xenonDgramEvent = callback; },
    callNative: request => {
      if (typeof __xenonDgramCall !== 'function')
        throw Object.assign(new Error('Native UDP transport is unavailable'), {code: 'ERR_NOT_SUPPORTED'});
      // Binary data is a separate native argument: the metadata codec keeps
      // its legacy JSON semantics and must not stringify nested Buffers.
      const {bytes, ...options} = request;
      return __xenonDgramCall(options, bytes);
    }});
  // @include "common/http_server.js"
  // @include "common/http_upgrade.js"
  // @include "main/http.js"
  // @include "main/electron_net.js"
  const utilityNetSessions = new Map();
  const electronNet = createElectronNet({
    getSession: isUtilityProcessRole ? partition => {
      const key = String(partition || '');
      if (!utilityNetSessions.has(key)) utilityNetSessions.set(key, Object.freeze({partition: key}));
      return utilityNetSessions.get(key);
    } : getSession,
    isSession: value => isUtilityProcessRole ?
      !!value && utilityNetSessions.get(value.partition) === value : value instanceof Session,
    isReady: () => isUtilityProcessRole || appReady,
  });
  electronModule.net = electronNet.net;
  // @include "main/http2.js"
  const ttyModule = { isatty: () => false };
  // @include "main/tls.js"

  // Non-terminal readline consumes real stream data and preserves UTF-8 and
  // CRLF boundaries across chunks. Interactive terminal editing is separate.
  // @include "common/readline.js"
  // @include "main/mac_player.js"
  // @include "common/timers_promises.js"
  // @include "main/worker_threads.js"
  const workerThreadsModule = createWorkerThreads({EventEmitter,
    getProcess: () => globalThis.process, pathModule, urlModule});
  // @include "main/process_env.js"
  // @include "main/install.js"
})();
