  let appReady = false;
  let resolveAppReady;
  const appReadyPromise = new Promise(resolve => { resolveAppReady = resolve; });
  const app = new EventEmitter();
  let appUserAgentFallback = String(globalThis.__xenonUserAgent || '');
  Object.defineProperty(app, 'userAgentFallback', {
    enumerable: true,
    configurable: true,
    get: () => appUserAgentFallback,
    set(value) {
      if (typeof value !== 'string') throw new TypeError('Expected a user agent string');
      appUserAgentFallback = value;
    },
  });
  let appExitState = 'running';
  let appQuitPhase = '';
  let appQuitAdvancing = false;
  let beforeQuitEmitted = false;
  let willQuitEmitted = false;
  let quitEmitted = false;

  function requireAppExitHost() {
    if (typeof globalThis.__xenonExitApp !== 'function' ||
        (typeof globalThis.__xenonCanExitApp === 'function' &&
         !globalThis.__xenonCanExitApp())) {
      return electronUnsupported('app exit');
    }
    return globalThis.__xenonExitApp;
  }

  function stopAppEvents() {
    appEventsStopped = true;
    pendingAppEvents.length = 0;
  }

  function cancelAppQuit() {
    if (appExitState !== 'quitting') return;
    appExitState = 'running';
    appQuitPhase = '';
    beforeQuitEmitted = false;
    willQuitEmitted = false;
    appEventsStopped = false;
  }

  function finishAppExit(code, exitHost) {
    if (quitEmitted) return;
    quitEmitted = true;
    appExitState = 'stopped';
    stopAppEvents();
    globalThis.__xenonShutdownNotifications?.();
    try {
      app.emit('quit', createBrowserWindowEvent(app), code);
    } finally {
      try {
        processEmitter.emit('exit', code);
      } finally {
        if (exitHost) exitHost(code);
      }
    }
  }

  function destroyAllAppWindows() {
    let firstError;
    for (const window of BrowserWindow.getAllWindows()) {
      try {
        window.destroy();
      } catch (error) {
        firstError ||= error;
      }
    }
    if (firstError) throw firstError;
  }

  function forceAppExit(code, exitHost, notifyShutdown) {
    if (appExitState === 'stopped' || appExitState === 'exiting') return;
    appExitState = 'exiting';
    stopAppEvents();
    try {
      if (notifyShutdown && !beforeQuitEmitted) {
        beforeQuitEmitted = true;
        app.emit('before-quit', createBrowserWindowEvent(app, true));
      }
    } finally {
      try {
        destroyAllAppWindows();
      } finally {
        try {
          if (notifyShutdown && !willQuitEmitted) {
            willQuitEmitted = true;
            app.emit('will-quit', createBrowserWindowEvent(app, true));
          }
        } finally {
          finishAppExit(code, exitHost);
        }
      }
    }
  }

  function continueAppQuit() {
    if (appExitState !== 'quitting' || appQuitPhase !== 'closing' ||
        appQuitAdvancing) return;
    appQuitAdvancing = true;
    try {
      for (const window of BrowserWindow.getAllWindows()) {
        if (appExitState !== 'quitting') return;
        // app.quit() can be called inside this window's close listener. Let
        // that original request finish before inspecting whether it was vetoed.
        if (window._closeRequested) return;
        window.close();
        if (appExitState !== 'quitting') return;
        if (!window.isDestroyed()) {
          cancelAppQuit();
          return;
        }
      }
      appQuitPhase = 'will-quit';
      willQuitEmitted = true;
      const event = createBrowserWindowEvent(app, true);
      app.emit('will-quit', event);
      if (appExitState !== 'quitting') return;
      if (event.defaultPrevented) {
        cancelAppQuit();
        return;
      }
      finishAppExit(0, requireAppExitHost());
    } catch (error) {
      cancelAppQuit();
      throw error;
    } finally {
      appQuitAdvancing = false;
    }
  }
  app.isPackaged = Boolean(globalThis.__xenonIsPackaged);
  app.getAppPath = () => __xenonAppPath;
  // GPU feature status is currently unknown; do not invent enabled features.
  app.getGPUFeatureStatus = () => ({});
  app.getLocale = () => {
    if (typeof globalThis.__xenonBrowserWindowCall !== 'function')
      return electronUnsupported('app.getLocale');
    const locale = __xenonBrowserWindowCall(0, 'app-get-locale', {});
    if (typeof locale !== 'string' || !locale)
      throw Object.assign(new Error('Host application locale is unavailable'),
          {code: 'ERR_IPC_INVALID_RESPONSE'});
    return locale;
  };
  app.getPreferredSystemLanguages = () => {
    if (typeof globalThis.__xenonBrowserWindowCall !== 'function')
      return electronUnsupported('app.getPreferredSystemLanguages');
    const languages = __xenonBrowserWindowCall(0, 'app-get-preferred-system-languages', {});
    const invalid = () => Object.assign(new Error('Host system language preferences are unavailable'),
        {code: 'ERR_IPC_INVALID_RESPONSE'});
    if (!Array.isArray(languages)) throw invalid();
    const result = [];
    for (const language of languages) {
      if (typeof language !== 'string' || !language || language.includes('\0')) throw invalid();
      result.push(language);
    }
    return result;
  };
  function requireAppPathString(value, argument) {
    if (typeof value !== 'string') {
      throw Object.assign(new TypeError(`${argument} must be a string`),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (value.includes('\0')) {
      throw Object.assign(new TypeError(`${argument} must not contain NUL`),
                          {code: 'ERR_INVALID_ARG_VALUE'});
    }
    return value;
  }
  app.getPath = name => __xenonGetPath(requireAppPathString(name, 'name'));
  app.setPath = (name, path) => {
    requireAppPathString(name, 'name');
    requireAppPathString(path, 'path');
    if (typeof globalThis.__xenonSetPath !== 'function')
      return electronUnsupported('app.setPath');
    __xenonSetPath(name, path);
  };
  app.setAppLogsPath = path => {
    if (path !== undefined) requireAppPathString(path, 'path');
    if (typeof globalThis.__xenonSetAppLogsPath !== 'function')
      return electronUnsupported('app.setAppLogsPath');
    if (path === undefined) __xenonSetAppLogsPath();
    else __xenonSetAppLogsPath(path);
  };
  app.getVersion = () => __xenonAppVersion;
  app.getName = () => __xenonAppName;
  function callLoginItemSettings(method, options) {
    if (options === undefined && method === 'get') options = {};
    if (!options || typeof options !== 'object' || Array.isArray(options)) {
      throw Object.assign(new TypeError('Login item options must be an object'),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (typeof globalThis.__xenonBrowserWindowCall !== 'function') {
      return electronUnsupported('app.' + method + 'LoginItemSettings');
    }
    try {
      return __xenonBrowserWindowCall(0, 'app-login-item-settings', {method, options});
    } catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  }
  app.getLoginItemSettings = options => globalThis.__xenonPlatform === 'darwin'
      ? callElectronApi('app.getLoginItemSettings', {options})
      : callLoginItemSettings('get', options);
  app.setLoginItemSettings = options => {
    callLoginItemSettings('set', options);
  };
  app.whenReady = () => appReadyPromise;
  app.isReady = () => appReady;
  app.disableHardwareAcceleration = () => {};
  function singleInstanceError(message, code = 'ERR_INVALID_ARG_VALUE') {
    return Object.assign(new TypeError(message), {code});
  }
  function cloneSingleInstanceData(value) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
      throw singleInstanceError('additionalData must be a JSON object', 'ERR_INVALID_ARG_TYPE');
    }
    const ancestors = new Set();
    let budget = 0;
    function clone(current, depth) {
      if (depth > 60) throw singleInstanceError('additionalData exceeds the depth limit', 'ERR_OUT_OF_RANGE');
      if (current === null || typeof current === 'boolean') return current;
      if (typeof current === 'number') {
        if (!Number.isFinite(current)) throw singleInstanceError('additionalData requires finite JSON numbers');
        return current;
      }
      if (typeof current === 'string') {
        budget += Buffer.byteLength(current, 'utf8');
        if (budget > 1024 * 1024) throw singleInstanceError('additionalData exceeds the size limit', 'ERR_OUT_OF_RANGE');
        return current;
      }
      if (typeof current !== 'object' ||
          (!Array.isArray(current) && Object.prototype.toString.call(current) !== '[object Object]')) {
        throw singleInstanceError('additionalData only supports JSON values', 'ERR_NOT_SUPPORTED');
      }
      if (ancestors.has(current)) throw singleInstanceError('additionalData must not contain cycles');
      ancestors.add(current);
      const result = Array.isArray(current) ? [] : {};
      if (Array.isArray(current)) {
        if (current.length > 1024 * 1024) throw singleInstanceError('additionalData exceeds the size limit', 'ERR_OUT_OF_RANGE');
        for (let index = 0; index < current.length; ++index) {
          result.push(clone(current[index], depth + 1));
          budget += 2;
        }
      } else {
        for (const key of Reflect.ownKeys(current)) {
          const descriptor = Object.getOwnPropertyDescriptor(current, key);
          if (!descriptor.enumerable) continue;
          if (typeof key !== 'string' || !Object.hasOwn(descriptor, 'value')) {
            throw singleInstanceError('additionalData requires enumerable string data properties', 'ERR_NOT_SUPPORTED');
          }
          budget += Buffer.byteLength(key, 'utf8') + 4;
          Object.defineProperty(result, key, {value: clone(descriptor.value, depth + 1),
            enumerable: true, configurable: true, writable: true});
        }
      }
      ancestors.delete(current);
      if (budget > 1024 * 1024) throw singleInstanceError('additionalData exceeds the size limit', 'ERR_OUT_OF_RANGE');
      return result;
    }
    return clone(value, 0);
  }
  function callSingleInstance(method, details = {}) {
    if (typeof globalThis.__xenonBrowserWindowCall !== 'function') {
      return electronUnsupported('app single-instance locking');
    }
    try {
      const result = __xenonBrowserWindowCall(0, 'app-single-instance', {method, ...details});
      if (method !== 'release' && typeof result !== 'boolean') {
        throw Object.assign(new Error('Host returned an invalid single-instance result'),
                            {code: 'ERR_IPC_INVALID_RESPONSE'});
      }
      return result;
    } catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  }
  app.requestSingleInstanceLock = (additionalData = {}) => {
    const data = cloneSingleInstanceData(additionalData);
    // Electron reads launch arguments and cwd natively. process.argv is a
    // mutable application array and is not the second-instance command line.
    const details = {additionalData: data};
    if (Buffer.byteLength(JSON.stringify(details), 'utf8') > 1024 * 1024 - 256) {
      throw singleInstanceError('Single-instance arguments exceed the size limit', 'ERR_OUT_OF_RANGE');
    }
    return callSingleInstance('request', details);
  };
  app.hasSingleInstanceLock = () => callSingleInstance('has');
  app.releaseSingleInstanceLock = () => { callSingleInstance('release'); };
  app.setAppUserModelId = id => {
    requireAppPathString(id, 'appUserModelId');
    if (!id || id.length > 128)
      throw Object.assign(new TypeError('appUserModelId must contain 1 to 128 characters'),
                          {code: 'ERR_INVALID_ARG_VALUE'});
    __xenonBrowserWindowCall(0, 'app-set-user-model-id', {id});
  };
  app.setJumpList = categories => {
    if (categories !== null && !Array.isArray(categories))
      throw Object.assign(new TypeError('categories must be an array or null'),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    return __xenonBrowserWindowCall(0, 'app-set-jump-list', {categories});
  };
  // OS protocol association is optional. Report that no association was
  // changed, allowing applications to continue without deep-link integration.
  app.setAsDefaultProtocolClient = () => false;
  app.removeAsDefaultProtocolClient = () => false;
  // Keep the asynchronous API contract. A rejected operation does not claim
  // that the process-wide proxy configuration was applied.
  app.setProxy = async () => electronUnsupported('app.setProxy');
  app.addRecentDocument = () => {};
  app.clearRecentDocuments = () => {};
  app.focus = () => {};
  app.quit = () => {
    if (appExitState !== 'running') return;
    // Check capability before any cleanup, so an unavailable host cannot leave
    // a live container whose SDK and windows have already been shut down.
    requireAppExitHost();
    appExitState = 'quitting';
    appQuitPhase = 'before-quit';
    stopAppEvents();
    beforeQuitEmitted = true;
    try {
      const event = createBrowserWindowEvent(app, true);
      app.emit('before-quit', event);
      if (appExitState !== 'quitting') return;
      if (event.defaultPrevented) {
        cancelAppQuit();
        return;
      }
      appQuitPhase = 'closing';
      continueAppQuit();
    } catch (error) {
      cancelAppQuit();
      throw error;
    }
  };
  app.exit = (code = 0) => {
    if (appExitState === 'stopped' || appExitState === 'exiting') return;
    if (typeof code !== 'number' || !Number.isInteger(code)) {
      throw Object.assign(new TypeError('Exit code must be an integer'),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (code < -2147483648 || code > 2147483647) {
      throw Object.assign(new RangeError('Exit code must fit a signed 32-bit integer'),
                          {code: 'ERR_OUT_OF_RANGE'});
    }
    forceAppExit(code, requireAppExitHost(), false);
  };
  app.commandLine = createAppCommandLine(request => {
    if (typeof globalThis.__xenonBrowserWindowCall !== 'function')
      return electronUnsupported('app.commandLine');
    return __xenonBrowserWindowCall(0, 'app-command-line', request);
  });

  class DownloadItem extends EventEmitter {
    constructor(url = '', filename = '') {
      super();
      this._url = String(url || '');
      this._filename = String(filename || '');
      this._state = 'progressing';
      this._totalBytes = 0;
      this._receivedBytes = 0;
      this._paused = false;
      this._savePath = '';
    }
    getURL() { return this._url; }
    getFilename() { return this._filename; }
    getState() { return this._state; }
    isPaused() { return this._paused; }
    getTotalBytes() { return this._totalBytes; }
    getReceivedBytes() { return this._receivedBytes; }
    getContentDisposition() { return ''; }
    getMimeType() { return ''; }
    hasUserGesture() { return true; }
    getSavePath() { return this._savePath; }
    setSavePath(path) { this._savePath = String(path || ''); }
    pause() {
      this._paused = true;
      this.emit('updated', { sender: this }, 'interrupted');
    }
    resume() {
      this._paused = false;
      this.emit('updated', { sender: this }, 'progressing');
    }
    cancel() {
      this._state = 'cancelled';
      this.emit('updated', { sender: this }, 'cancelled');
      this.emit('done', { sender: this }, 'cancelled');
    }
  }

  function electronUnsupported(method) {
    const error = new Error(`Electron ${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }

  function callElectronApi(operation, details = {}) {
    if (typeof __xenonBrowserWindowCall !== 'function') {
      return electronUnsupported(operation);
    }
    try {
      const request = {operation};
      for (const [key, value] of Object.entries(details)) {
        if (value !== undefined) request[key] = value;
      }
      return __xenonBrowserWindowCall(0, 'electron-api', request);
    } catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  }
