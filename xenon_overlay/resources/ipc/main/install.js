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
    setImmediate,
    clearImmediate,
  };
  const timersPromisesModule = createTimersPromises(timersModule);
  timersModule.promises = timersPromisesModule;
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
  const dataViewByteLength = Object.getOwnPropertyDescriptor(
      DataView.prototype, 'byteLength').get;
  const v8Module = {
    getHeapCodeStatistics() { return __xenonGetHeapCodeStatistics(); },
    serialize(value) {
      return Buffer.from(__xenonV8Serialize(value, Buffer, dataViewByteLength));
    },
  };

  globalThis.structuredClone = function structuredClone(value, options = undefined) {
    const invalid = message => Object.assign(new TypeError(message), {code: 'ERR_INVALID_ARG_TYPE'});
    if (arguments.length === 0)
      throw Object.assign(new TypeError('structuredClone requires a value'), {code: 'ERR_MISSING_ARGS'});
    if (options != null && typeof options !== 'object' && typeof options !== 'function')
      throw invalid('structuredClone options must be an object');
    let transfers;
    const supplied = options?.transfer;
    if (supplied !== undefined) {
      if (supplied === null || (typeof supplied !== 'object' && typeof supplied !== 'function'))
        throw invalid('structuredClone transfer must be an iterable');
      const iterator = supplied[Symbol.iterator];
      if (typeof iterator !== 'function') throw invalid('structuredClone transfer must be an iterable');
      transfers = [];
      for (const entry of {[Symbol.iterator]: () => iterator.call(supplied)}) transfers.push(entry);
    }
    try {
      return __xenonStructuredClone(value, transfers);
    } catch (error) {
      // Native failures carry an inaccessible V8 private brand. A user getter
      // may throw an arbitrary object, including a same-named Error; preserve it.
      if (__xenonIsDataCloneError(error)) throw new MainDOMException(error.message, 'DataCloneError');
      throw error;
    }
  };

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
  globalThis.__xenonDiagnosticsChannel = diagnosticsChannelModule;
  globalThis.__xenonPath = pathModule;
  globalThis.__xenonPathWin32 = win32;
  globalThis.__xenonPathPosix = posix;
  globalThis.__xenonOs = osModule;
  if (!isNodeWorkerRole) {
    // Utility processes have their own Electron API list. Publish only the
    // supported utility subset; main window/app/session APIs stay inaccessible.
    globalThis.__xenonElectron = isUtilityProcessRole ? {net: electronNet.net} : electronModule;
  }
  globalThis.__xenonWorkerThreads = workerThreadsModule;
  // Node exposes the same MessagePort class globally and from worker_threads.
  // Its supported instances are currently real Worker parent ports; do not
  // alias Electron's distinct MessagePortMain event contract here.
  globalThis.MessagePort = workerThreadsModule.MessagePort;
  globalThis.__xenonFs = fsModule;
  globalThis.__xenonFsPromises = fsPromises;
  Object.defineProperty(globalThis, '__xenonOriginalFs', {get: getOriginalFsModule});
  Object.defineProperty(globalThis, '__xenonOriginalFsPromises', {
    get: () => getOriginalFsModule().promises,
  });
  globalThis.__xenonBuffer = createBufferModule(Buffer, globalThis.__xenonBufferLimits);
  globalThis.__xenonCrypto = cryptoModule;
  Object.defineProperty(globalThis, 'crypto', {enumerable: true, configurable: true,
    get: () => mainWebCrypto.crypto});
  globalThis.Crypto = mainWebCrypto.Crypto;
  globalThis.SubtleCrypto = mainWebCrypto.SubtleCrypto;
  globalThis.__xenonUtil = utilModule;
  globalThis.__xenonUtilTypes = utilModule.types;
  globalThis.__xenonUrl = urlModule;
  globalThis.__xenonModule = moduleModule;
  globalThis.__xenonStream = streamModule;
  globalThis.__xenonStreamPromises = streamModule.promises;
  globalThis.__xenonStreamWeb = webStreamsModule;
  globalThis.__xenonStringDecoder = stringDecoderModule;
  globalThis.__xenonChildProcess = childProcessModule;
  globalThis.__xenonAssert = assert;
  globalThis.__xenonConstants = constantsModule;
  globalThis.__xenonZlib = zlibModule;
  globalThis.__xenonNet = netModule;
  globalThis.__xenonDgram = dgramModule;
  globalThis.__xenonTls = tlsModule;
  globalThis.__xenonHttp = httpModule;
  globalThis.__xenonHttp2 = http2Module;
  globalThis.__xenonHttps = httpsModule;
  globalThis.__xenonTty = ttyModule;
  globalThis.__xenonReadline = readlineModule;
  globalThis.__xenonDns = dnsModule;
  globalThis.__xenonQuerystring = querystringModule;
  globalThis.__xenonTimers = timersModule;
  globalThis.__xenonTimersPromises = timersPromisesModule;
  globalThis.__xenonPerfHooks = perfHooksModule;
  globalThis.__xenonV8 = v8Module;
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
    if (isNodeWorkerRole || isUtilityProcessRole || appEventsStopped) return;
    if (typeof eventName !== 'string' || !Array.isArray(args)) {
      throw new TypeError('Application events require a name and argument array');
    }
    pendingAppEvents.push([eventName, args.slice()]);
    dispatchPendingAppEvents();
  };
  globalThis.__xenonMarkAppReady = () => {
    if (isNodeWorkerRole || isUtilityProcessRole || appReady || appExitState !== 'running') return;
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
    if (isNodeWorkerRole) return;
    if (isUtilityProcessRole) return globalThis.__xenonUtilityShutdown?.();
    // Native teardown already owns container termination. Notify and clean up
    // once without recursively requesting another native exit.
    forceAppExit(0, null, true);
  };
  const hostedExecPath = String(__xenonExecPath || '');
  const processEmitter = new EventEmitter();
  globalThis.process = {
    // Electron main-process apps expect process.type === 'browser'.
    type: 'browser',
    argv: Array.isArray(globalThis.__xenonArgv) ? globalThis.__xenonArgv.slice() : [hostedExecPath],
    execPath: hostedExecPath,
    resourcesPath: String(globalThis.__xenonResourcesPath || ''),
    pid: __xenonPid,
    kill(pid, signal = 'SIGTERM') {
      if (typeof pid !== 'number') {
        throw Object.assign(new TypeError('The "pid" argument must be a number'),
                            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      if (!Number.isInteger(pid) || pid < -2147483648 || pid > 2147483647) {
        throw Object.assign(new RangeError('The "pid" argument must be a 32-bit integer'),
                            {code: 'ERR_OUT_OF_RANGE'});
      }
      if (signal !== 0) {
        throw Object.assign(new Error('Only process.kill(pid, 0) is supported'),
                            {code: 'ERR_NOT_SUPPORTED', syscall: 'kill'});
      }
      if (typeof __xenonChildProcessCall !== 'function') {
        throw Object.assign(new Error('Native process status transport is unavailable'),
                            {code: 'ERR_NOT_SUPPORTED', syscall: 'kill'});
      }
      const result = __xenonChildProcessCall({
        id: '@process', op: 'processProbe', pid, signal: 0,
      });
      if (!result || result.ok !== true) {
        const code = result && result.code || 'ERR_SYSTEM_ERROR';
        throw Object.assign(new Error(`kill ${code}: ${result && result.message || 'Process status check failed'}`),
                            {code, errno: result && result.errno, syscall: 'kill'});
      }
      return true;
    },
    platform: __xenonPlatform,
    arch: __xenonArch,
    // Node's Windows main/Utility environment is case-insensitive. A Worker
    // receives a case-sensitive snapshot even on Windows.
    env: processEnvironmentSnapshot(__xenonEnv, __xenonPlatform === 'win32' && !isNodeWorkerRole),
    stdin: { fd: 0, isTTY: false, read() { return null; }, on() { return this; } },
    versions: {
      electron: '0.0.0-compat',
      chrome: __xenonChromeVersion,
      node: '0.0.0-compat',
      v8: __xenonV8Version,
    },
    version: 'v0.0.0-compat',
    cwd: () => {
      if (typeof globalThis.__xenonProcessDirectory === 'function') {
        const result = globalThis.__xenonProcessDirectory();
        if (!result || result.ok !== true || typeof result.directory !== 'string') {
          throw Object.assign(new Error(`uv_cwd ${result?.message || 'failed'}`),
              {code: result?.code || 'ERR_SYSTEM_ERROR', errno: result?.errno,
                syscall: 'uv_cwd'});
        }
        return result.directory;
      }
      if (typeof globalThis.__xenonWorkingDirectory !== 'string') {
        throw Object.assign(new Error('Process working directory is unavailable'),
                            {code: 'ERR_NOT_SUPPORTED'});
      }
      return globalThis.__xenonWorkingDirectory;
    },
    chdir(directory) {
      if (isNodeWorkerRole) {
        throw Object.assign(new TypeError('process.chdir() is not supported in workers'),
            {code: 'ERR_WORKER_UNSUPPORTED_OPERATION'});
      }
      if (typeof directory !== 'string') {
        throw Object.assign(new TypeError('The "directory" argument must be a string'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      if (directory.includes('\0')) {
        throw Object.assign(new TypeError('The "directory" argument must not contain null bytes'),
            {code: 'ERR_INVALID_ARG_VALUE'});
      }
      if (typeof globalThis.__xenonProcessDirectory !== 'function') {
        throw Object.assign(new Error('Native process directory transport is unavailable'),
            {code: 'ERR_NOT_SUPPORTED'});
      }
      const previous = process.cwd();
      const result = globalThis.__xenonProcessDirectory(directory);
      if (!result || result.ok !== true) {
        throw Object.assign(new Error(`chdir ${result?.code || 'ERR_SYSTEM_ERROR'}: ` +
            `${result?.message || 'failed'}, '${previous}' -> '${directory}'`),
            {code: result?.code === 'ENOTSUP' ? 'ERR_NOT_SUPPORTED' :
                result?.code || 'ERR_SYSTEM_ERROR', errno: result?.errno,
              syscall: 'chdir', path: previous, dest: directory});
      }
    },
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
    exit: code => {
      if (!isNodeWorkerRole) return app.exit(code === undefined ? 0 : code);
      if (typeof globalThis.__xenonWorkerExit !== 'function')
        throw Object.assign(new Error('Worker exit transport is unavailable'), {code: 'ERR_NOT_SUPPORTED'});
      if (code !== undefined) process.exitCode = code;
      // Node lets an exit listener's exception propagate (and even be caught
      // by the caller). Terminating in finally would discard that exception.
      globalThis.__xenonWorkerShutdown(process.exitCode || 0);
      return globalThis.__xenonWorkerExit(process.exitCode || 0);
    },
  };
  const originalConsole = globalThis.console;
  installNodeDebuglog(utilModule, process, EventEmitter, Buffer, text => {
    if (typeof __xenonLog === 'function') __xenonLog(text);
    else originalConsole.error(text.endsWith('\n') ? text.slice(0, -1) : text);
  });
  globalThis.__xenonConsole = installNodeConsole({util: utilModule, process, EventEmitter, Buffer,
    writeHost: text => {
      if (typeof __xenonLog === 'function') __xenonLog(text);
      else originalConsole.log(text.endsWith('\n') ? text.slice(0, -1) : text);
    },
    now: () => { const time = process.hrtime(); return time[0] * 1000 + time[1] / 1e6; },
    publish: (name, args) => diagnosticsChannelModule.channel('console.' + name).publish(args),
  });
  globalThis.console = globalThis.__xenonConsole;
  if (isNodeWorkerRole) {
    let exitCode;
    let exiting = false;
    Object.defineProperty(process, 'exitCode', {
      enumerable: true, configurable: true,
      get: () => exitCode,
      set(value) {
        if (value == null) { exitCode = undefined; return; }
        if (typeof value === 'string' && value.trim() !== '') value = Number(value);
        if (typeof value !== 'number')
          throw Object.assign(new TypeError('exitCode must be a number or numeric string'), {code: 'ERR_INVALID_ARG_TYPE'});
        if (!Number.isInteger(value))
          throw Object.assign(new RangeError('exitCode must be an integer'), {code: 'ERR_OUT_OF_RANGE'});
        exitCode = value | 0;
      },
    });
    globalThis.__xenonWorkerBeforeExit = () => {
      if (!exiting) process.emit('beforeExit', process.exitCode || 0);
      return process.exitCode || 0;
    };
    globalThis.__xenonWorkerShutdown = code => {
      if (exiting) return process.exitCode || 0;
      process.exitCode = code;
      exiting = true;
      process._exiting = true;
      process.emit('exit', process.exitCode || 0);
      return process.exitCode || 0;
    };
    delete process.type;
    delete process.resourcesPath;
    delete process.getSystemVersion;
    delete process.electronBinding;
    delete process.versions.electron;
    delete process.versions.chrome;
    process.execArgv = [];
    process._linkedBinding = () => {
      throw Object.assign(new Error('Linked bindings are not supported in Worker threads'), {code: 'ERR_NOT_SUPPORTED'});
    };
  } else if (!isUtilityProcessRole) {
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
  }
  if (isUtilityProcessRole) {
    const runtime = installUtilityRuntime({process, Buffer, stream: streamModule,
      call: globalThis.__xenonUtilityProcessCall,
      exitHost: globalThis.__xenonUtilityExit,
      execArgv: globalThis.__xenonExecArgv || []});
    globalThis.__xenonUtilityShutdown = runtime.shutdown;
    globalThis.__xenonInstallUtilityParentPort = port => installUtilityParentPort({
      process, EventEmitter, port, setPaused: globalThis.__xenonSetMessagePortPaused,
    });
  }
  globalThis.global = globalThis;
  globalThis.GLOBAL = globalThis;
  globalThis.root = globalThis;
  globalThis.TextEncoder = utilModule.TextEncoder;
  globalThis.TextDecoder = utilModule.TextDecoder;
  globalThis.setImmediate = typeof setImmediate === 'function' ? setImmediate : (fn, ...args) => setTimeout(fn, 0, ...args);
  globalThis.clearImmediate = typeof clearImmediate === 'function' ? clearImmediate : id => clearTimeout(id);

  // Incoming IPC/window events begin a new task. enterWith() from one message
  // must not leak into the next; Promise/timer continuations keep their frame.
  for (const name of ['__xenonDispatchSend', '__xenonDispatchInvoke',
    '__xenonDispatchSync', '__xenonDispatchBrowserWindowEvent',
    '__xenonDispatchRendererEvent', '__xenonDispatchWillDownload',
    '__xenonDispatchProtocol', '__xenonCancelProtocol',
    '__xenonDispatchWorkerEvent', '__xenonDispatchUtilityProcessEvent']) {
    const dispatch = globalThis[name];
    globalThis[name] = (...args) =>
        runWithAsyncContext(undefined, dispatch, undefined, args);
  }
