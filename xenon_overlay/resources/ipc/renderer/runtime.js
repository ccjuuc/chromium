  // Node's async_hooks implementation is embedder-backed. Hosted renderers
  // still need its public context API for libraries such as OpenTelemetry.
  // Preserve the synchronous scope semantics; AsyncResource.bind() lets those
  // libraries carry the captured scope into callbacks they own.
  // Promise hooks are scoped to this V8 context. Blink owns the isolate's
  // continuation-preserved embedder data, so never overwrite that slot.
  let currentAsyncContext;
  let asyncContextHooksInstalled = false;
  const promiseAsyncContexts = new WeakMap();
  const asyncContextStack = [];
  const installAsyncContextHooks = transport.installAsyncContextHooks?.bind(transport);
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

  // --- 2. ipcRenderer ---
  const ipcRenderer = new EventEmitter();
  let dispatchXenonNet = null;
  let dispatchNodeAddon = null;

  ipcRenderer.send = (channel, ...args) => transport.send(channel, ...args);
  ipcRenderer.sendSync = (channel, ...args) =>
      transport.sendSync(channel, ...args);
  ipcRenderer.invoke = (channel, ...args) => transport.invoke(channel, ...args);
  ipcRenderer.postMessage = (channel, message, transfer) => {
    if (transfer !== undefined && !Array.isArray(transfer)) {
      throw new TypeError('The "transfer" argument must be an array');
    }
    if (transfer && transfer.length) {
      throw new TypeError('MessagePort transfer is not supported by this transport');
    }
    transport.postMessage(channel, message);
  };
  ipcRenderer.sendToHost = (channel, ...args) => {
    return transport.send('__xenon:send-to-host', channel, args);
  };

  const dispatchRendererMessage = (channel, args) => {
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
    ipcRenderer.emit(channel, {sender: ipcRenderer, ports: []}, ...values);
  };
  transport.setDispatchHandler((...args) =>
      runWithAsyncContext(undefined, dispatchRendererMessage, undefined, args));

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
    isPackaged: injectedPaths.isPackaged,
    whenReady: () => Promise.resolve(appApi),
    isReady: () => true,
  };

  function unsupportedElectronApi(name) {
    const error = new Error(`electron.${name} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }

  function throwHostApiError(error) {
    // IpcResult carries an error string. Recover the native code without
    // replacing an existing exception or treating a failure as an API result.
    if (error && !error.code && typeof error.message === 'string') {
      const code = /^([A-Z][A-Z_]+):/.exec(error.message);
      if (code) error.code = code[1];
    }
    throw error;
  }

  function callHostElectronApi(operation, args = {}) {
    if (typeof transport.sendSync !== 'function') {
      return unsupportedElectronApi(operation);
    }
    try {
      return transport.sendSync('__xenon:electron-api', {operation, ...args});
    } catch (error) {
      return throwHostApiError(error);
    }
  }

  async function invokeHostElectronApi(operation, args = {}) {
    if (typeof transport.invoke !== 'function') {
      return unsupportedElectronApi(operation);
    }
    try {
      return await transport.invoke('__xenon:electron-api', {operation, ...args});
    } catch (error) {
      return throwHostApiError(error);
    }
  }

  const dialogApi = {
    showOpenDialog: async (options) => {
      const filePaths = await showNativeOpenDialog(options);
      return {canceled: !filePaths.length, filePaths};
    },
    showOpenDialogSync: () => unsupportedElectronApi('dialog.showOpenDialogSync'),
    showSaveDialog: async () => unsupportedElectronApi('dialog.showSaveDialog'),
    showMessageBox: async () => unsupportedElectronApi('dialog.showMessageBox'),
  };

  const electron = {
    ipcRenderer,
    app: appApi,
    dialog: dialogApi,
    remote: {
      dialog: dialogApi,
      app: appApi,
      getCurrentWindow: () => unsupportedElectronApi('remote.getCurrentWindow'),
    },
    clipboard: {
      readText: (type = 'clipboard') => callHostElectronApi('clipboard.readText', {type}),
      writeText(text, type = 'clipboard') {
        callHostElectronApi('clipboard.writeText', {text, type});
      },
      readHTML: (type = 'clipboard') => callHostElectronApi('clipboard.readHTML', {type}),
      writeHTML(markup, type = 'clipboard') {
        callHostElectronApi('clipboard.writeHTML', {markup, type});
      },
      clear(type = 'clipboard') { callHostElectronApi('clipboard.clear', {type}); },
    },
    shell: {
      openExternal: (url, options = {}) =>
          invokeHostElectronApi('shell.openExternal', {url, options}),
      openPath: path => invokeHostElectronApi('shell.openPath', {path}),
      showItemInFolder(path) {
        callHostElectronApi('shell.showItemInFolder', {path});
      },
    },
    webFrame: {
      setZoomFactor: () => unsupportedElectronApi('webFrame.setZoomFactor'),
      getZoomFactor: () => unsupportedElectronApi('webFrame.getZoomFactor'),
      setZoomLevel: () => unsupportedElectronApi('webFrame.setZoomLevel'),
      getZoomLevel: () => unsupportedElectronApi('webFrame.getZoomLevel'),
    },
    autoUpdater: (() => {
      const updater = new EventEmitter();
      updater.autoDownload = true;
      updater.autoInstallOnAppQuit = true;
      updater.setFeedURL = (urlOrOptions) => {
        const url = typeof urlOrOptions === 'string' ? urlOrOptions : (urlOrOptions && urlOrOptions.url ? urlOrOptions.url : '');
        return invokeHostElectronApi('autoUpdater.setFeedURL', {url});
      };
      updater.getFeedURL = async () => {
        const res = await invokeHostElectronApi('autoUpdater.getFeedURL', {});
        return (res && res.url) || '';
      };
      updater.checkForUpdates = async () => {
        updater.emit('checking-for-update');
        return invokeHostElectronApi('autoUpdater.checkForUpdates', {});
      };
      updater.checkForUpdatesAndNotify = async () => updater.checkForUpdates();
      updater.downloadUpdate = async () => invokeHostElectronApi('autoUpdater.downloadUpdate', {});
      updater.quitAndInstall = () => {
        invokeHostElectronApi('autoUpdater.quitAndInstall', {});
      };
      ipcRenderer.on('__xenon:auto-updater-event', (_event, eventName, ...args) => {
        updater.emit(eventName, ...args);
      });
      return updater;
    })(),
  };
  globalThis.__xenonElectronIpc = electron;
  // Guest preloads can explicitly expose their own API, but a page without
  // Node integration must never inherit this unrestricted convenience alias.
  if (!injectedPaths.isGuest && globalThis.electron === undefined) {
    globalThis.electron = electron;
  }
