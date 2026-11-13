  // Worker execution and structured clone belong to the native thread engine.
  // This layer owns Node's objects/events, without serializing application data.
  function createWorkerThreads({EventEmitter, getProcess, pathModule, urlModule}) {
    const workers = new Map();
    const state = new WeakMap();
    const SHARE_ENV = Symbol('nodejs.worker_threads.SHARE_ENV');
    const threadId = globalThis.__xenonWorkerThreadId || 0;
    const isMainThread = threadId === 0;
    const unsupported = feature => { throw Object.assign(new Error(feature + ' is not supported'), {code: 'ERR_NOT_SUPPORTED'}); };
    const invalid = message => Object.assign(new TypeError(message), {code: 'ERR_INVALID_ARG_TYPE'});
    const nativeFailure = error => {
      if (error && !error.code) {
        const code = /\b(ERR_[A-Z_]+)\b/.exec(String(error.message));
        if (code) error.code = code[0];
      }
      return error;
    };
    function callNative(name, ...args) {
      if (typeof globalThis[name] !== 'function') unsupported('worker_threads transport');
      try { return globalThis[name](...args); }
      catch (error) { throw nativeFailure(error); }
    }
    function transferList(value) {
      if (value === undefined) return [];
      if (!Array.isArray(value)) throw invalid('transferList must be an array');
      // The native serializer validates the entire list before detaching any
      // buffer. JS must not detach, JSON-copy, or partially transfer it here.
      return value;
    }
    function optionsFor(filename, options) {
      if (options === undefined) options = {};
      if (!options || typeof options !== 'object' || Array.isArray(options)) throw invalid('Worker options must be an object');
      const supported = new Set(['eval', 'argv', 'env', 'execArgv', 'stdin', 'stdout', 'stderr',
        'name', 'resourceLimits', 'workerData', 'transferList', 'trackUnmanagedFds']);
      for (const key of Object.keys(options)) if (!supported.has(key)) unsupported('Worker option ' + key);
      if (options.eval !== undefined && typeof options.eval !== 'boolean') throw invalid('eval must be boolean');
      const evaluate = options.eval === true;
      if (filename instanceof URL) {
        if (evaluate || filename.protocol !== 'file:') unsupported('Worker URL scheme or eval URL');
        filename = urlModule.fileURLToPath(filename);
      }
      if (typeof filename !== 'string') throw invalid('Worker filename or source must be a string or file URL');
      if (!evaluate) {
        if (filename.includes('\0'))
          throw Object.assign(new TypeError('Worker filename must not contain null bytes'), {code: 'ERR_INVALID_ARG_VALUE'});
        if (!pathModule.isAbsolute(filename) && !/^\.\.?[\\/]/.test(filename))
          throw Object.assign(new TypeError('Worker filename must be absolute or start with ./ or ../'), {code: 'ERR_WORKER_PATH'});
        filename = pathModule.resolve(filename);
      }
      for (const name of ['stdin', 'stdout', 'stderr']) {
        if (options[name] !== undefined && typeof options[name] !== 'boolean') throw invalid(name + ' must be boolean');
        if (options[name]) unsupported('Worker ' + name + ' streams');
      }
      if (options.execArgv !== undefined && !Array.isArray(options.execArgv)) throw invalid('execArgv must be an array');
      if (options.execArgv?.length) unsupported('Worker execArgv');
      if (options.argv !== undefined && !Array.isArray(options.argv)) throw invalid('argv must be an array');
      if (options.name !== undefined && typeof options.name !== 'string') throw invalid('Worker name must be a string');
      if (options.trackUnmanagedFds !== undefined && typeof options.trackUnmanagedFds !== 'boolean')
        throw invalid('trackUnmanagedFds must be boolean');
      if (options.env === SHARE_ENV) unsupported('worker_threads.SHARE_ENV');
      const environment = options.env === undefined ? getProcess().env : options.env;
      if (!environment || typeof environment !== 'object' || Array.isArray(environment)) throw invalid('env must be an object');
      const env = Object.fromEntries(Object.entries(environment).map(([key, value]) => [key, String(value)]));
      const resourceLimits = {};
      if (options.resourceLimits !== undefined) {
        if (!options.resourceLimits || typeof options.resourceLimits !== 'object' || Array.isArray(options.resourceLimits))
          throw invalid('resourceLimits must be an object');
        for (const [key, value] of Object.entries(options.resourceLimits)) {
          if (!['maxOldGenerationSizeMb', 'maxYoungGenerationSizeMb', 'stackSizeMb'].includes(key))
            unsupported('Worker resource limit ' + key);
          if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0)
            throw Object.assign(new RangeError('Invalid Worker resource limit ' + key), {code: 'ERR_OUT_OF_RANGE'});
          resourceLimits[key] = value;
        }
      }
      return {filename, native: {eval: evaluate, argv: (options.argv || []).map(String), env,
        execArgv: [], name: options.name || '', resourceLimits, trackUnmanagedFds: options.trackUnmanagedFds !== false},
        workerData: options.workerData, transferList: transferList(options.transferList)};
    }

    class Worker extends EventEmitter {
      constructor(filename, options) {
        super();
        const request = optionsFor(filename, options);
        const result = callNative('__xenonWorkerCreate', request.filename, request.native, request.workerData, request.transferList);
        if (!result || !Number.isSafeInteger(result.id) || result.id <= 0)
          throw Object.assign(new Error('Invalid native Worker identity'), {code: 'ERR_INVALID_STATE'});
        if (typeof result.threadName !== 'string' || !result.resourceLimits ||
            typeof result.resourceLimits !== 'object' || Array.isArray(result.resourceLimits)) {
          callNative('__xenonWorkerControl', result.id, 'terminate');
          throw Object.assign(new Error('Invalid native Worker metadata'), {code: 'ERR_INVALID_STATE'});
        }
        state.set(this, {id: result.id, threadName: result.threadName,
          resourceLimits: {...result.resourceLimits}, exited: false, online: false,
          termination: null, resolveTermination: null});
        workers.set(result.id, this);
      }
      get threadId() { const current = state.get(this); return current.exited ? -1 : current.id; }
      get threadName() { const current = state.get(this); return current.exited ? null : current.threadName; }
      get resourceLimits() { const current = state.get(this); return current.exited ? {} : {...current.resourceLimits}; }
      get stdin() { return null; }
      get stdout() { return unsupported('Worker stdout stream'); }
      get stderr() { return unsupported('Worker stderr stream'); }
      postMessage(value, transfers) {
        const current = state.get(this);
        if (current.exited) return;
        callNative('__xenonWorkerPost', current.id, value, transferList(transfers));
      }
      terminate() {
        const current = state.get(this);
        if (current.exited) return Promise.resolve();
        if (current.termination) return current.termination;
        current.termination = new Promise(resolve => { current.resolveTermination = resolve; });
        try { callNative('__xenonWorkerControl', current.id, 'terminate'); }
        catch (error) { current.termination = null; current.resolveTermination = null; return Promise.reject(error); }
        return current.termination;
      }
      ref() { const current = state.get(this); if (!current.exited) callNative('__xenonWorkerControl', current.id, 'ref'); return this; }
      unref() { const current = state.get(this); if (!current.exited) callNative('__xenonWorkerControl', current.id, 'unref'); return this; }
      getHeapSnapshot() { return Promise.reject(Object.assign(new Error('Worker heap snapshots are not supported'), {code: 'ERR_NOT_SUPPORTED'})); }
      getHeapStatistics() { return Promise.reject(Object.assign(new Error('Worker heap statistics are not supported'), {code: 'ERR_NOT_SUPPORTED'})); }
      cpuUsage() { return Promise.reject(Object.assign(new Error('Worker CPU usage is not supported'), {code: 'ERR_NOT_SUPPORTED'})); }
      get performance() { return {eventLoopUtilization: () => unsupported('Worker performance.eventLoopUtilization')}; }
    }

    const portToken = {};
    class MessagePort extends EventEmitter {
      constructor(token) {
        super();
        if (token !== portToken) throw Object.assign(new TypeError('Illegal constructor'), {code: 'ERR_ILLEGAL_CONSTRUCTOR'});
        this._started = false; this._refed = false; this._closing = false; this._closed = false;
        this._queue = []; this._scheduled = false;
      }
      postMessage(value, transfers) {
        if (this._closing || this._closed) return;
        callNative('__xenonWorkerPost', 0, value, transferList(transfers));
      }
      on(name, listener) {
        super.on(name, listener);
        if (name === 'message') { this.ref(); this.start(); }
        return this;
      }
      addListener(name, listener) { return this.on(name, listener); }
      once(name, listener) {
        super.once(name, listener);
        if (name === 'message') { this.ref(); this.start(); }
        return this;
      }
      prependListener(name, listener) {
        super.prependListener(name, listener);
        if (name === 'message') { this.ref(); this.start(); }
        return this;
      }
      prependOnceListener(name, listener) {
        super.prependOnceListener(name, listener);
        if (name === 'message') { this.ref(); this.start(); }
        return this;
      }
      removeListener(name, listener) {
        super.removeListener(name, listener);
        if (name === 'message' && !this.listenerCount('message')) {
          this._started = false;
          this.unref();
        }
        return this;
      }
      off(name, listener) { return this.removeListener(name, listener); }
      removeAllListeners(name) {
        super.removeAllListeners(name);
        if ((name === undefined || name === 'message') && !this.listenerCount('message')) {
          this._started = false;
          this.unref();
        }
        return this;
      }
      start() { if (!this._closed && !this._closing) { this._started = true; this._flush(); } }
      _flush() {
        if (!this._started || this._scheduled || this._closed || this._closing) return;
        this._scheduled = true;
        queueMicrotask(() => {
          this._scheduled = false;
          while (this._queue.length && this._started && !this._closed && !this._closing)
            this.emit('message', this._queue.shift());
        });
      }
      _message(value) { if (!this._closed && !this._closing) { this._queue.push(value); this._flush(); } }
      close(callback) {
        if (typeof callback === 'function' && !this._closed) this.once('close', callback);
        if (this._closed || this._closing) return;
        this._closing = true;
        try { callNative('__xenonWorkerControl', 0, 'close'); }
        catch (error) { this._closing = false; throw error; }
      }
      _close() {
        if (this._closed) return;
        this._closed = true; this._closing = false; this._refed = false; this._queue.length = 0;
        this.emit('close');
      }
      ref() {
        if (!this._closed && !this._closing && !this._refed) {
          if (callNative('__xenonWorkerControl', 0, 'ref') !== false) this._refed = true;
        }
        return this;
      }
      unref() {
        if (!this._closed && !this._closing && this._refed) {
          callNative('__xenonWorkerControl', 0, 'unref'); this._refed = false;
        }
        return this;
      }
      hasRef() { return this._refed && !this._closed && !this._closing; }
      addEventListener() { return unsupported('MessagePort EventTarget listeners'); }
      removeEventListener() { return unsupported('MessagePort EventTarget listeners'); }
      get onmessage() { return null; }
      set onmessage(_listener) { unsupported('MessagePort EventTarget listeners'); }
      get onmessageerror() { return null; }
      set onmessageerror(_listener) { unsupported('MessagePort EventTarget listeners'); }
    }
    const parentPort = isMainThread ? null : new MessagePort(portToken);
    globalThis.__xenonDispatchWorkerEvent = (id, type, payload) => {
      if (id === 0 && parentPort) {
        if (type === 'message') parentPort._message(payload);
        else if (type === 'close') parentPort._close();
        else if (type === 'messageerror') parentPort.emit('messageerror', payload);
        return;
      }
      const worker = workers.get(id);
      if (!worker) return;
      const current = state.get(worker);
      if (type === 'online') {
        if (!current.online) { current.online = true; worker.emit('online'); }
      } else if (type === 'message') worker.emit('message', payload);
      else if (type === 'messageerror') worker.emit('messageerror', payload);
      else if (type === 'error') {
        const error = new Error(payload && Object.hasOwn(payload, 'message') ? String(payload.message) : 'Worker failed');
        if (payload?.name) error.name = String(payload.name);
        if (payload?.stack) error.stack = String(payload.stack);
        if (payload?.code) error.code = String(payload.code);
        worker.emit('error', error);
      } else if (type === 'exit') {
        current.exited = true; workers.delete(id);
        try { worker.emit('exit', payload); }
        finally { current.resolveTermination?.(payload); }
      }
    };
    globalThis.__xenonShutdownWorkers = () => {
      for (const [id] of workers) callNative('__xenonWorkerControl', id, 'terminate');
      parentPort?.close();
    };
    class MessageChannel { constructor() { unsupported('worker_threads.MessageChannel'); } }
    return {Worker, MessagePort, MessageChannel, parentPort, isMainThread, isInternalThread: false,
      threadId, threadName: String(globalThis.__xenonWorkerThreadName || ''),
      workerData: isMainThread ? null : globalThis.__xenonWorkerData,
      resourceLimits: {...globalThis.__xenonWorkerResourceLimits}, SHARE_ENV,
      markAsUntransferable: () => unsupported('worker_threads.markAsUntransferable'),
      isMarkedAsUntransferable: () => unsupported('worker_threads.isMarkedAsUntransferable'),
      markAsUncloneable: value => {
        if ((typeof value !== 'object' && typeof value !== 'function') || value === null) return;
        callNative('__xenonMarkAsUncloneable', value);
      },
      moveMessagePortToContext: () => unsupported('worker_threads.moveMessagePortToContext'),
      receiveMessageOnPort: () => unsupported('worker_threads.receiveMessageOnPort'),
      setEnvironmentData: () => unsupported('worker_threads.setEnvironmentData'),
      getEnvironmentData: () => unsupported('worker_threads.getEnvironmentData'),
      postMessageToThread: async () => unsupported('worker_threads.postMessageToThread'),
    };
  }
