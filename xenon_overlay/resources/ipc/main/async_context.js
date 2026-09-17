  // Promise hooks are scoped to this V8 context. Blink owns the isolate's
  // continuation-preserved embedder data, so never overwrite that slot.
  let currentAsyncContext;
  let asyncContextHooksInstalled = false;
  const promiseAsyncContexts = new WeakMap();
  const asyncContextStack = [];
  const installAsyncContextHooks = globalThis.__xenonInstallAsyncContextHooks;
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
