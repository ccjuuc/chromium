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
    resourcesPath: String(globalThis.__xenonResourcesPath || ''),
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
    cwd: () => {
      if (typeof globalThis.__xenonWorkingDirectory !== 'string') {
        throw Object.assign(new Error('Process working directory is unavailable'),
                            {code: 'ERR_NOT_SUPPORTED'});
      }
      return globalThis.__xenonWorkingDirectory;
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
