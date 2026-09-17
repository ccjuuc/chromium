  let appReady = false;
  let resolveAppReady;
  const appReadyPromise = new Promise(resolve => { resolveAppReady = resolve; });
  const app = new EventEmitter();
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
  app.isPackaged = Boolean(__xenonRendererBaseUrl) ||
      (Array.isArray(globalThis.__xenonRendererUrlMappings) &&
       globalThis.__xenonRendererUrlMappings.length > 0);
  app.getAppPath = () => __xenonAppPath;
  app.getPath = name => __xenonGetPath(String(name));
  app.getVersion = () => __xenonAppVersion;
  app.getName = () => __xenonAppName;
  app.whenReady = () => appReadyPromise;
  app.isReady = () => appReady;
  app.disableHardwareAcceleration = () => {};
  app.requestSingleInstanceLock = () => electronUnsupported('app.requestSingleInstanceLock');
  app.releaseSingleInstanceLock = () => electronUnsupported('app.releaseSingleInstanceLock');
  app.setAppUserModelId = () => {};
  app.setAsDefaultProtocolClient = () => electronUnsupported('app.setAsDefaultProtocolClient');
  app.removeAsDefaultProtocolClient = () => electronUnsupported('app.removeAsDefaultProtocolClient');
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
  app.commandLine = {appendSwitch() {}, appendArgument() {}, hasSwitch() { return false; }};

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
