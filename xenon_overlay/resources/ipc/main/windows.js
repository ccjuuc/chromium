  function callElectronSession(partition, method, args = {}) {
    if (typeof __xenonBrowserWindowCall !== 'function') {
      return electronUnsupported('session.' + method);
    }
    return __xenonBrowserWindowCall(0, 'electron-session', {partition, method, ...args});
  }

  class SessionServiceWorker extends EventEmitter {
    constructor(session, info) {
      super();
      this.session = session;
      this.versionId = Number(info.versionId);
      this.scope = info.scope;
      this.scriptURL = info.scriptUrl;
      this._endpointId = '';
      this.ipc = new EventEmitter();
      this.ipc._invokeHandlers = new Map();
      this.ipc.handle = (channel, handler) => {
        if (typeof channel !== 'string' || typeof handler !== 'function')
          throw new TypeError('ipc.handle requires a channel and handler');
        if (this.ipc._invokeHandlers.has(channel))
          throw new Error('Attempted to register a second handler for ' + channel);
        this.ipc._invokeHandlers.set(channel, handler);
      };
      this.ipc.handleOnce = (channel, handler) => this.ipc.handle(channel, (...args) => {
        this.ipc.removeHandler(channel);
        return handler(...args);
      });
      this.ipc.removeHandler = channel => this.ipc._invokeHandlers.delete(channel);
    }
    send(channel, ...args) {
      if (!this._endpointId) throw new Error('Service worker IPC endpoint is unavailable');
      __xenonSendToRenderer(this._endpointId, channel, args);
    }
  }

  class SessionServiceWorkers extends EventEmitter {
    constructor(session) {
      super();
      this._session = session;
      this._workers = new Map();
      super.on('newListener', name => {
        if (name === 'running-status-changed')
          callElectronSession(session.partition, 'observe-workers');
      });
    }
    getAllRunning() {
      return callElectronSession(this._session.partition, 'get-running-workers');
    }
    getWorkerFromVersionID(versionId) {
      const id = Number(versionId);
      if (!Number.isSafeInteger(id)) throw new TypeError('Invalid service worker version ID');
      const known = this._workers.get(id);
      if (known && known._endpointId) return known;
      const info = this.getAllRunning()[String(id)];
      if (!info) return undefined;
      const worker = known || new SessionServiceWorker(this._session, info);
      this._workers.set(id, worker);
      return worker;
    }
  }

  // Only browser-supplied metadata reaches this entry. Endpoint IDs change on
  // every worker start so an old object's handlers cannot follow a restart.
  function getWorkerForIpc(sender, attached = true) {
    const workers = getSession(sender.partition).serviceWorkers;
    const id = Number(sender.versionId);
    let worker = workers._workers.get(id);
    if (!attached) {
      if (worker && worker._endpointId === sender.endpointId) {
        worker._endpointId = '';
        workers._workers.delete(id);
      }
      return worker;
    }
    if (!worker || (worker._endpointId && worker._endpointId !== sender.endpointId)) {
      if (worker) worker._endpointId = '';
      worker = new SessionServiceWorker(workers._session, sender);
      workers._workers.set(id, worker);
    }
    worker._endpointId = sender.endpointId;
    worker.workerToken = sender.workerToken || sender.endpointId;
    return worker;
  }

  function handleElectronSessionEvent(payload) {
    if (!payload || payload.event !== 'running-status-changed') return;
    const serviceWorkers = getSession(payload.partition).serviceWorkers;
    const details = payload.details;
    if (details.runningStatus === 'stopped') {
      const worker = serviceWorkers._workers.get(Number(details.versionId));
      if (worker) worker._endpointId = '';
      serviceWorkers._workers.delete(Number(details.versionId));
    }
    serviceWorkers.emit('running-status-changed', {...details});
  }

  class Session extends EventEmitter {
    constructor(partition = '') {
      super();
      // Session identity is shared by native windows, preloads and protocol
      // handlers; changing it would route one object to different partitions.
      Object.defineProperty(this, 'partition', {
        value: String(partition || ''), enumerable: true,
      });
      this._userAgent = '';
      this._downloadPath = '';
      this.webRequest = createSessionWebRequest(this.partition);
      this.cookies = Object.fromEntries(['get', 'set', 'remove', 'flushStore']
          .map(method => [method, async () => electronUnsupported(`cookies.${method}`)]));
      this.protocol = createSessionProtocol(this.partition);
      this.serviceWorkers = new SessionServiceWorkers(this);
    }
    getPreloadScripts() { return callElectronSession(this.partition, 'get-preloads'); }
    registerPreloadScript(script) {
      if (!script || typeof script !== 'object' ||
          !['frame', 'service-worker'].includes(script.type) ||
          typeof script.filePath !== 'string' || !pathModule.isAbsolute(script.filePath) ||
          (script.id !== undefined && (typeof script.id !== 'string' || !script.id)))
        throw new TypeError('Invalid preload script descriptor');
      return callElectronSession(this.partition, 'register-preload', {script});
    }
    unregisterPreloadScript(id) {
      if (typeof id !== 'string' || !id) throw new TypeError('Invalid preload script ID');
      callElectronSession(this.partition, 'unregister-preload', {id});
    }
    getUserAgent() {
      return this._userAgent || app.userAgentFallback;
    }
    setUserAgent(userAgent) {
      this._userAgent = String(userAgent || '');
    }
    async setProxy(config) { return electronUnsupported('session.setProxy'); }
    async resolveProxy(url) { return electronUnsupported('session.resolveProxy'); }
    fetch(input, init) { return electronNet.fetchSession(this, input, init); }
    async clearCache() { return electronUnsupported('session.clearCache'); }
    async clearStorageData(options) { return electronUnsupported('session.clearStorageData'); }
    async clearAuthCache() { return electronUnsupported('session.clearAuthCache'); }
    async clearHostResolverCache() { return electronUnsupported('session.clearHostResolverCache'); }
    setDownloadPath(p) { return electronUnsupported('session.setDownloadPath'); }
    enableNetworkEmulation(options) { return electronUnsupported('session.enableNetworkEmulation'); }
    disableNetworkEmulation() { return electronUnsupported('session.disableNetworkEmulation'); }
    setCertificateVerifyProc(proc) { return electronUnsupported('session.setCertificateVerifyProc'); }
    setPermissionRequestHandler(handler) {
      setSessionPermissionHandler(this.partition, 'request', handler);
    }
    setPermissionCheckHandler(handler) {
      setSessionPermissionHandler(this.partition, 'check', handler);
    }
    setDisplayMediaRequestHandler(handler, options) {
      setSessionPermissionHandler(this.partition, 'display', handler, options);
    }
    async getBlobData(identifier) { return electronUnsupported('session.getBlobData'); }
    createInterruptedDownload(options) { return electronUnsupported('session.createInterruptedDownload'); }
  }

  const sessionMap = new Map();
  function getSession(partition = '') {
    const key = String(partition || '');
    let sess = sessionMap.get(key);
    if (!sess) {
      sess = new Session(key);
      sessionMap.set(key, sess);
    }
    return sess;
  }
  const defaultSession = getSession('');
  const session = {
    defaultSession,
    fromPartition: (partition) => getSession(partition),
  };

  const reportedDeferredSchemePrivileges = new Set();
  const protocol = {
    registerSchemesAsPrivileged(customSchemes) {
      if (appReady) {
        throw new Error(
            'protocol.registerSchemesAsPrivileged must be called before app is ready');
      }
      if (!Array.isArray(customSchemes)) {
        throw new TypeError('Expected an array of custom schemes');
      }
      const privilegeNames = new Set([
        'standard', 'secure', 'bypassCSP', 'allowServiceWorkers',
        'supportFetchAPI', 'corsEnabled', 'stream', 'codeCache',
      ]);
      const schemes = new Set();
      const deferredPrivileges = new Set();
      for (const entry of customSchemes) {
        if (!entry || typeof entry !== 'object' || Array.isArray(entry) ||
            typeof entry.scheme !== 'string' ||
            !/^[a-z][a-z0-9+.-]*$/i.test(entry.scheme)) {
          throw new TypeError('Each custom scheme requires a valid scheme name');
        }
        const scheme = entry.scheme.toLowerCase();
        if (schemes.has(scheme)) throw new TypeError('Duplicate custom scheme: ' + scheme);
        schemes.add(scheme);
        if (entry.privileges !== undefined) {
          if (!entry.privileges || typeof entry.privileges !== 'object' ||
              Array.isArray(entry.privileges)) {
            throw new TypeError('Custom scheme privileges must be an object');
          }
          for (const [name, value] of Object.entries(entry.privileges)) {
            if (!privilegeNames.has(name) || typeof value !== 'boolean') {
              throw new TypeError('Invalid custom scheme privilege: ' + name);
            }
            if (value && (name === 'stream' || name === 'allowServiceWorkers')) {
              return electronUnsupported('Custom scheme privilege ' + name);
            }
            if (value && name === 'codeCache') {
              deferredPrivileges.add(scheme + ': ' + name);
            }
          }
        }
      }
      if (typeof globalThis.__xenonRegisterSchemesAsPrivileged !== 'function') {
        return electronUnsupported('protocol.registerSchemesAsPrivileged');
      }
      // 各模块可在 ready 前分次声明；每次都由宿主核对启动时冻结的权限，
      // 不在运行期增加或覆盖 Chromium 的协议权限。
      globalThis.__xenonRegisterSchemesAsPrivileged(customSchemes);
      // Code caching is an optional optimization. Its declaration does not
      // change the startup privileges or imply that a cache was enabled.
      for (const privilege of deferredPrivileges) {
        if (reportedDeferredSchemePrivileges.has(privilege)) continue;
        reportedDeferredSchemePrivileges.add(privilege);
        if (typeof __xenonLog === 'function') {
          __xenonLog('Custom scheme ' + privilege +
              ' requested but disabled in the compatibility runtime');
        }
      }
    },
  };
  // Electron keeps global registration separate from session request handlers.
  // Its module exposes the default session's methods only after app readiness.
  Object.setPrototypeOf(protocol, new Proxy(Object.create(null), {
    get(_target, name) {
      const handlers = appReady ? defaultSession.protocol : null;
      if (!handlers || !Object.prototype.hasOwnProperty.call(handlers, name)) return;
      return (...args) => Reflect.apply(handlers[name], handlers, args);
    },
    has: (_target, name) => appReady && name in defaultSession.protocol,
    ownKeys: () => appReady ? Reflect.ownKeys(defaultSession.protocol) : [],
    getOwnPropertyDescriptor(_target, name) {
      if (appReady && Object.prototype.hasOwnProperty.call(defaultSession.protocol, name)) {
        return {configurable: true, enumerable: true};
      }
    },
  }));

  globalThis.__xenonDispatchWillDownload = (url, filename, windowId) => {
    const owner = BrowserWindow.fromId(Number(windowId));
    const contents = owner ? owner.webContents : null;
    const sess = contents?.session || defaultSession;
    const item = new DownloadItem(url, filename);
    const event = {
      defaultPrevented: false,
      preventDefault() { this.defaultPrevented = true; }
    };
    sess.emit('will-download', event, item, contents);
    return item;
  };

  // Keep transport state out of the public object inspected by remote bridges.
  const webContentsState = new WeakMap();
  const allWebContents = new Map();
  const rendererWebContents = new Map();
  const webFrameState = new WeakMap();
  const guestWebContents = new Map();
  let electronViews;
  const pendingScriptExecutions = new Map();
  let nextScriptExecutionId = 1;
  let nextWebContentsId = 1;

  class WebFrameMain {
    constructor(sender) {
      webFrameState.set(this, {
        processId: sender.processId,
        routingId: sender.frameId,
        url: typeof sender.url === 'string' ? sender.url : '',
        detached: false,
      });
    }
    get processId() { return webFrameState.get(this).processId; }
    get routingId() { return webFrameState.get(this).routingId; }
    get url() { return webFrameState.get(this).url; }
    get detached() { return webFrameState.get(this).detached; }
  }

  function getSenderWebContents(sender) {
    let contents = rendererWebContents.get(sender.endpointId);
    if (!contents) {
      const owner = BrowserWindow.fromId(Number(sender.windowId));
      contents = guestWebContents.get(Number(sender.windowId)) ||
          (owner ? owner.webContents : new WebContents(null));
      rendererWebContents.set(sender.endpointId, contents);
    }
    const state = webContentsState.get(contents);
    const endpoints = state.endpoints;
    let endpoint = endpoints.get(sender.endpointId);
    if (!endpoint) {
      if (sender.isMainFrame === true) {
        cancelScriptExecutions(pending => pending.contents === contents &&
            pending.endpointId !== sender.endpointId,
            'Script execution document was replaced');
      }
      endpoint = {...sender, frame: new WebFrameMain(sender)};
      endpoints.set(sender.endpointId, endpoint);
    } else {
      // URL updates originate in the browser's document host, including
      // same-document navigations. Preserve the identity of this document.
      webFrameState.get(endpoint.frame).url =
          typeof sender.url === 'string' ? sender.url : '';
    }
    // Only the browser's current main document defines the committed URL.
    // Subframes and late updates from a replaced document cannot overwrite it.
    if (sender.isMainFrame === true && getMainEndpoint(contents) === endpoint) {
      state.committedUrl = endpoint.frame.url;
    }
    return contents;
  }

  function getMainEndpoint(contents) {
    // Guests may admit subframes. Only browser-authored main-frame metadata
    // can select mainFrame; a newer navigation supersedes the old document.
    return [...webContentsState.get(contents).endpoints.values()]
        .filter(endpoint => endpoint.isMainFrame !== false).at(-1);
  }

  function cancelScriptExecution(id, pending, reason) {
    clearTimeout(pending.timer);
    pendingScriptExecutions.delete(id);
    try {
      __xenonSendToRenderer(pending.endpointId,
          '__xenon:cancel-execute-javascript', [id]);
    } catch (_) {
      // A revoked endpoint already destroys its native pending evaluations.
    }
    pending.reject(new Error(reason));
  }

  function cancelScriptExecutions(matches, reason) {
    for (const [id, pending] of pendingScriptExecutions) {
      if (matches(pending)) cancelScriptExecution(id, pending, reason);
    }
  }

  function destroyWebContents(contents) {
    const state = webContentsState.get(contents);
    if (state.destroyed) return;
    state.destroyed = true;
    cancelScriptExecutions(pending => pending.contents === contents,
        'WebContents was destroyed');
    const processIds = new Set();
    for (const endpoint of state.endpoints.values()) {
      webFrameState.get(endpoint.frame).detached = true;
      rendererWebContents.delete(endpoint.endpointId);
      processIds.add(endpoint.processId);
    }
    state.endpoints.clear();
    allWebContents.delete(contents.id);
    for (const processId of processIds) {
      contents.emit('render-view-deleted', {sender: contents}, processId);
    }
    contents.emit('destroyed');
  }

  globalThis.__xenonDispatchRendererEvent = (sender, attached) => {
    if (sender.type === 'service-worker') {
      getWorkerForIpc(sender, attached);
      return;
    }
    if (attached) {
      getSenderWebContents(sender);
      return;
    }
    const contents = rendererWebContents.get(sender.endpointId);
    if (!contents) return;
    cancelScriptExecutions(pending => pending.endpointId === sender.endpointId,
        'Script execution document was detached');
    rendererWebContents.delete(sender.endpointId);
    const state = webContentsState.get(contents);
    const endpoint = state.endpoints.get(sender.endpointId);
    if (endpoint) webFrameState.get(endpoint.frame).detached = true;
    state.endpoints.delete(sender.endpointId);
    if (![...state.endpoints.values()].some(
        endpoint => endpoint.processId === sender.processId)) {
      contents.emit('render-view-deleted', {sender: contents}, sender.processId);
    }
    // A document disconnect is not destruction of its owning BrowserWindow.
    if (!contents._owner && !contents._guest && state.endpoints.size === 0) {
      destroyWebContents(contents);
    }
  };

  class WebContents extends EventEmitter {
    constructor(owner) {
      super();
      this.id = nextWebContentsId++;
      this._owner = owner;
      webContentsState.set(this, {endpoints: new Map(), destroyed: false});
      allWebContents.set(this.id, this);
      this.session = getSession(owner?._webPreferences?.partition || '');
    }
    _updateInputObservation() {
      const state = webContentsState.get(this);
      const enabled = this.listenerCount('before-input-event') > 0;
      if (state.destroyed || enabled === !!state.observesBeforeInput) return;
      const nativeId = this._guest?.nativeId ?? this._owner?.id;
      if (nativeId === undefined) return electronUnsupported('before-input-event on unowned WebContents');
      callBrowserWindow({id: nativeId}, 'observe-before-input', {enabled});
      state.observesBeforeInput = enabled;
    }
    // Wrappers survive removeAllListeners(), including removal of the meta
    // events; only subscription transitions cross the native boundary.
    on(name, listener) { super.on(name, listener); this._updateInputObservation(); return this; }
    addListener(name, listener) { return this.on(name, listener); }
    once(name, listener) { super.once(name, listener); this._updateInputObservation(); return this; }
    prependListener(name, listener) { super.prependListener(name, listener); this._updateInputObservation(); return this; }
    prependOnceListener(name, listener) { super.prependOnceListener(name, listener); this._updateInputObservation(); return this; }
    removeListener(name, listener) { super.removeListener(name, listener); this._updateInputObservation(); return this; }
    off(name, listener) { return this.removeListener(name, listener); }
    removeAllListeners(name) { super.removeAllListeners(name); this._updateInputObservation(); return this; }
    send(channel, ...args) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const endpoint = getMainEndpoint(this);
      if (endpoint) __xenonSendToRenderer(endpoint.endpointId, channel, args);
    }
    sendToFrame(frameId, channel, ...args) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const [processId, routingId] = Array.isArray(frameId) ? frameId :
          [this.processId, frameId];
      const endpoint = [...webContentsState.get(this).endpoints.values()].find(
          item => item.processId === processId && item.frameId === routingId);
      if (!endpoint) return false;
      __xenonSendToRenderer(endpoint.endpointId, channel, args);
      return true;
    }
    get processId() { return getMainEndpoint(this)?.processId || 0; }
    get mainFrame() { return getMainEndpoint(this)?.frame || null; }
    get frameId() { return getMainEndpoint(this)?.frameId || 0; }
    getProcessId() { return this.processId; }
    getOSProcessId() {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      return callBrowserWindow({id}, 'get-os-process-id');
    }
    getOwnerBrowserWindow() {
      if (!this._guest?.standalone) return this._owner;
      const id = callBrowserWindow({id: 0}, 'electron-view',
          {action: 'contents-owner', webContentsId: this._guest.nativeId});
      return id ? BrowserWindow.fromId(id) || null : null;
    }
    setBackgroundThrottling(allowed) {
      if (typeof allowed !== 'boolean') throw new TypeError('Expected a boolean');
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      callBrowserWindow({id}, 'set-background-throttling', {allowed});
      if (this._owner) {
        this._owner._webPreferences = Object.freeze({
          ...this._owner._webPreferences, backgroundThrottling: allowed,
        });
      }
    }
    getBackgroundThrottling() {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      return callBrowserWindow({id}, 'get-background-throttling');
    }
    getLastWebPreferences() {
      return this._guest ? {...this._guest.preferences} :
          this._owner ? {...this._owner._webPreferences} : null;
    }
    isDestroyed() { return webContentsState.get(this).destroyed; }
    isOffscreen() {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      // All current hosts render into real windows; offscreen rendering is
      // not implemented by this runtime.
      return false;
    }
    getType() {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      // Unowned wrappers represent an existing browser-managed document,
      // rather than a BrowserWindow created by this Electron container.
      return this._guest?.standalone ? 'window' : this._guest ? 'webview' : this._owner ? 'window' : 'remote';
    }
    get hostWebContents() { return this._guest?.host; }
    close(options = {}) {
      if (this.isDestroyed()) return;
      if (!this._guest?.standalone) return electronUnsupported('webContents.close on this owner');
      if (!options || typeof options !== 'object') throw new TypeError('options must be an object');
      if (options.waitForBeforeUnload !== undefined && typeof options.waitForBeforeUnload !== 'boolean')
        throw new TypeError('waitForBeforeUnload must be a boolean');
      callBrowserWindow({id: 0}, 'electron-view', {action: 'close-contents',
        webContentsId: this._guest.nativeId, waitForBeforeUnload: options.waitForBeforeUnload ?? false});
    }
    getURL() { return webContentsState.get(this).committedUrl ?? this._url ?? ''; }
    getTitle() { return this._pageTitle || ''; }
    loadFile(file, options = {}) {
      if (typeof file !== 'string') throw new TypeError('File path must be a string');
      if (!options || typeof options !== 'object' || Array.isArray(options))
        throw new TypeError('loadFile options must be an object');
      const target = urlModule.pathToFileURL(pathModule.resolve(__xenonAppPath, file));
      target.search = urlModule.format({query: options.query, search: options.search});
      target.hash = options.hash || '';
      return this.loadURL(target.href);
    }
    async loadURL(url, options) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      if (typeof url !== 'string') throw new TypeError('URL must be a string');
      if (options && typeof options === 'object' && options.userAgent) {
        this.setUserAgent(options.userAgent);
      }
      this._url = mapRendererUrl(url);
      const nativeId = this._guest?.nativeId || this._owner?.id;
      if (!nativeId || typeof __xenonBrowserWindowCallAsync !== 'function')
        throw new Error('WebContents has no native navigation endpoint');
      const result = await __xenonBrowserWindowCallAsync(
          nativeId, 'load-url', {url: this._url});
      if (!result || typeof result.ok !== 'boolean')
        throw Object.assign(new Error('Invalid native navigation response'),
                            {code: 'ERR_IPC_INVALID_RESPONSE'});
      if (!result.ok) {
        const error = new Error(`${result.errorDescription} loading '${result.url}'`);
        Object.defineProperties(error, {
          code: {value: result.errorDescription, enumerable: true, configurable: true, writable: true},
          errno: {value: result.errorCode, enumerable: true, configurable: true, writable: true},
          url: {value: result.url, enumerable: true, configurable: true, writable: true},
        });
        throw error;
      }
    }
    async executeJavaScript(code, userGesture = false) {
      if (typeof code !== 'string' || typeof userGesture !== 'boolean') {
        throw Object.assign(new TypeError('Expected a script string and boolean userGesture'),
                            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      if (userGesture) {
        throw Object.assign(new Error('executeJavaScript userGesture is not supported'),
                            {code: 'ERR_NOT_SUPPORTED'});
      }
      if (this.isDestroyed()) throw new Error('WebContents was destroyed');
      const nativeId = this._guest?.nativeId ?? this._owner?.id;
      let confirmedEndpoint;
      if (nativeId !== undefined) {
        if (typeof __xenonBrowserWindowCallAsync !== 'function')
          throw new Error('WebContents runtime host is unavailable');
        confirmedEndpoint = await __xenonBrowserWindowCallAsync(
            nativeId, 'wait-for-main-frame-runtime', {});
        if (typeof confirmedEndpoint !== 'string' || !confirmedEndpoint)
          throw new Error('WebContents main-frame document is unavailable');
      }
      const endpoint = getMainEndpoint(this);
      if (this.isDestroyed() || !endpoint || endpoint.isMainFrame !== true ||
          (confirmedEndpoint !== undefined && endpoint.endpointId !== confirmedEndpoint) ||
          endpoint.frame.detached) {
        throw new Error('WebContents main-frame document is unavailable');
      }
      if (!Number.isSafeInteger(nextScriptExecutionId)) {
        throw Object.assign(new Error('Script execution request IDs exhausted'),
                            {code: 'ERR_OUT_OF_RANGE'});
      }
      return new Promise((resolve, reject) => {
        const id = nextScriptExecutionId++;
        const pending = {contents: this, endpointId: endpoint.endpointId,
          frame: endpoint.frame, resolve, reject, timer: undefined};
        pending.timer = setTimeout(() => {
          cancelScriptExecution(id, pending, 'Script evaluation timed out');
        }, 30000);
        pendingScriptExecutions.set(id, pending);
        try {
          __xenonSendToRenderer(endpoint.endpointId,
              '__xenon:execute-javascript', [id, code]);
        } catch (error) {
          clearTimeout(pending.timer);
          pendingScriptExecutions.delete(id);
          reject(error);
        }
      });
    }
    async setVisualZoomLevelLimits(minimum, maximum) {
      if (typeof minimum !== 'number' || typeof maximum !== 'number')
        throw Object.assign(new TypeError('Zoom limits must be numbers'), {code: 'ERR_INVALID_ARG_TYPE'});
      if (!Number.isFinite(minimum) || !Number.isFinite(maximum) || minimum <= 0 || maximum < minimum)
        throw Object.assign(new RangeError('Invalid visual zoom limits'), {code: 'ERR_INVALID_ARG_VALUE'});
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      callBrowserWindow({id}, 'set-visual-zoom-limits', {minimum, maximum});
    }
    getZoomLevel() {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      const level = callBrowserWindow({id}, 'get-zoom-level');
      if (typeof level !== 'number' || !Number.isFinite(level))
        throw Object.assign(new Error('Invalid native zoom level'), {code: 'ERR_IPC_INVALID_RESPONSE'});
      return level;
    }
    setZoomLevel(level) {
      if (typeof level !== 'number' || !Number.isFinite(level))
        throw Object.assign(new TypeError('Zoom level must be a finite number'), {code: 'ERR_INVALID_ARG_VALUE'});
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      callBrowserWindow({id}, 'set-zoom-level', {level});
    }
    getZoomFactor() { return Math.pow(1.2, this.getZoomLevel()); }
    setZoomFactor(factor) {
      if (typeof factor !== 'number' || !Number.isFinite(factor) || factor <= 0)
        throw Object.assign(new TypeError('Zoom factor must be positive and finite'), {code: 'ERR_INVALID_ARG_VALUE'});
      this.setZoomLevel(Math.log(factor) / Math.log(1.2));
    }
    get zoomLevel() { return this.getZoomLevel(); }
    set zoomLevel(value) { this.setZoomLevel(value); }
    get zoomFactor() { return this.getZoomFactor(); }
    set zoomFactor(value) { this.setZoomFactor(value); }
    openDevTools() {}
    closeDevTools() {}
    isDevToolsOpened() { return false; }
    setWindowOpenHandler(handler) {
      if (typeof handler !== 'function') throw new TypeError('Expected a function');
      webContentsState.get(this).windowOpenHandler = handler;
    }
    setUserAgent(userAgent) {
      const value = String(userAgent || '');
      if (this._owner || this._guest) {
        callBrowserWindow(
            this._owner || {id: this._guest.nativeId}, 'set-user-agent', {value});
      }
      // A WebContents override must not change its shared Session.
      this._userAgent = value;
    }
    getUserAgent() {
      if (this._owner || this._guest) {
        const value = callBrowserWindow(this._owner || {id: this._guest.nativeId}, 'get-user-agent');
        if (typeof value === 'string') {
          this._userAgent = value;
        }
      }
      if (!this._userAgent && this.session &&
          typeof this.session.getUserAgent === 'function') {
        this._userAgent = this.session.getUserAgent();
      }
      return this._userAgent || '';
    }
    toggleDevTools() {}
    setIgnoreMenuShortcuts(ignore) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      if (typeof ignore !== 'boolean') throw new TypeError('ignore must be a boolean');
      webContentsState.get(this).ignoreMenuShortcuts = ignore;
    }
  }
  ipcMain.handle('__xenon:register-guest', (event, nativeId, preferences) => {
    if (!Number.isInteger(nativeId) || nativeId >= 0 || guestWebContents.has(nativeId)) {
      throw new Error('Invalid guest WebContents identity');
    }
    const contents = new WebContents(null);
    contents._guest = {nativeId, host: event.sender, preferences: {...preferences}};
    contents.session = getSession(preferences.partition || '');
    guestWebContents.set(nativeId, contents);
    event.sender.emit('did-attach-webview', {sender: event.sender}, contents);
    return contents.id;
  });
  ipcMain.handle('__xenon:guest-call', (event, id, method, args = []) => {
    const contents = allWebContents.get(id);
    if (!contents?._guest || contents._guest.host !== event.sender) {
      throw new Error('Guest WebContents does not belong to this document');
    }
    if (['loadURL', 'send', 'sendToFrame', 'setUserAgent', 'executeJavaScript'].includes(method)) {
      return contents[method](...args);
    }
    const commands = {reload: 'reload', stop: 'stop', goBack: 'go-back', goForward: 'go-forward'};
    if (commands[method]) return callBrowserWindow({id: contents._guest.nativeId}, commands[method]);
    throw new Error('Unsupported guest method: ' + method);
  });
  ipcMain.on('__xenon:send-to-host', (event, channel, args) => {
    const contents = event.sender;
    if (!contents._guest) throw new Error('sendToHost requires a guest WebContents');
    contents._guest.host.send('__xenon:guest-event', contents.id, 'ipc-message',
        {channel, args, frameId: event.frameId});
  });
  ipcMain.on('__xenon:execute-javascript-result', (event, id, success, value) => {
    const pending = pendingScriptExecutions.get(id);
    if (!pending || pending.contents !== event.sender ||
        pending.frame !== event.senderFrame || pending.frame.detached ||
        getMainEndpoint(pending.contents)?.endpointId !== pending.endpointId) return;
    clearTimeout(pending.timer);
    pendingScriptExecutions.delete(id);
    if (success === true) pending.resolve(value);
    else pending.reject(value);
  });
  function mapRendererUrl(url) {
    const mappings = Array.isArray(__xenonRendererUrlMappings) ?
        __xenonRendererUrlMappings : [];
    const raw = String(url || '');
    const normalized = raw.replace(/\\/g, '/');
    let protocol = '';
    let search = '';
    let hash = '';
    try {
      const parsed = new URL(normalized);
      protocol = parsed.protocol;
      search = parsed.search;
      hash = parsed.hash;
    } catch (e) {
      const hashIdx = normalized.indexOf('#');
      const withoutHash = hashIdx >= 0 ? normalized.slice(0, hashIdx) : normalized;
      const searchIdx = withoutHash.indexOf('?');
      if (searchIdx >= 0) {
        search = withoutHash.slice(searchIdx);
      }
      if (hashIdx >= 0) {
        hash = normalized.slice(hashIdx);
      }
    }
    if (normalized.startsWith('file:') || protocol === 'file:') {
      let sourcePath = normalized;
      try {
        sourcePath = decodeURIComponent(new URL(normalized).pathname)
            .replace(/^\/([A-Za-z]:)/, '$1');
      } catch (e) {
        sourcePath = decodeURIComponent(
            normalized.replace(/^file:\/\/\/?/i, ''));
      }
      sourcePath = sourcePath.replace(/\\/g, '/');
      const fold = value => __xenonPlatform === 'win32' ?
          value.toLowerCase() : value;
      const foldedSource = fold(sourcePath);
      for (const mapping of mappings) {
        const sourcePrefix = String(mapping?.sourcePathPrefix || '')
            .replace(/\\/g, '/').replace(/\/$/, '');
        const targetBase = String(mapping?.targetBaseUrl || '');
        const foldedPrefix = fold(sourcePrefix);
        if (!sourcePrefix || !targetBase ||
            (foldedSource !== foldedPrefix &&
             !foldedSource.startsWith(foldedPrefix + '/'))) {
          continue;
        }
        const relativePath = sourcePath.slice(sourcePrefix.length)
            .replace(/^\/+/, '') || 'index.html';
        const mapped = new URL(
            relativePath,
            targetBase.endsWith('/') ? targetBase : targetBase + '/');
        mapped.search = search;
        mapped.hash = hash;
        return mapped.toString();
      }
    }
    // URLs outside declared renderer roots retain their original destination.
    return url;
  }
  const browserWindows = [];
  let browserWindowCloseDepth = 0;
  let allBrowserWindowsClosed = true;

  function finishBrowserWindowClose(window, closeNative) {
    if (window._destroyed) return;
    window._destroyed = true;
    window._visible = false;
    ++browserWindowCloseDepth;
    try {
      try {
        destroyWebContents(window.webContents);
      } finally {
        if (closeNative && typeof __xenonCloseBrowserWindow === 'function') {
          __xenonCloseBrowserWindow(window.id);
        }
      }
    } finally {
      try {
        window.emit('closed');
      } finally {
        --browserWindowCloseDepth;
        // A closed listener can destroy a paired window or create its
        // replacement. Inspect the final live set after that cascade finishes.
        if (browserWindowCloseDepth === 0 && !allBrowserWindowsClosed &&
            !browserWindows.some(item => !item._destroyed)) {
          allBrowserWindowsClosed = true;
          if (appExitState === 'running') app.emit('window-all-closed');
        }
      }
    }
  }

  function callBrowserWindow(window, command, details = {}, commitState) {
    if (typeof __xenonBrowserWindowCall !== 'function') {
      return electronUnsupported(`BrowserWindow.${command}`);
    }
    try {
      const reply = __xenonBrowserWindowCall(window.id, command, details);
      const transactional = reply?.__xenonWindowCall === true;
      const value = transactional ? reply.value : reply;
      // Successful command state must be visible to its geometry listeners.
      // Commit before callbacks so reentrant setters cannot be overwritten.
      if (commitState) commitState(value);
      if ((command === 'get-bounds' || command === 'set-bounds') &&
          value && typeof value === 'object') {
        cacheWindowBounds(window, value,
            transactional ? reply.boundsRevision : undefined);
      }
      if (transactional) {
        // Commit every affected window before emitting: a listener can query a
        // paired window, resize it again, or destroy it during this callback.
        const changes = (reply.boundsChanges || []).map(change => ({
          ...change,
          window: change.windowId === window.id ? window :
              browserWindows.find(item => item.id === change.windowId),
        })).filter(change => change.window && !change.window._destroyed);
        for (const change of changes)
          cacheWindowBounds(change.window, change.bounds, change.revision, true);
        for (const change of changes) {
          if (browserWindows.includes(change.window))
            emitWindowBoundsChange(change.window, change.moved, change.resized);
        }
      }
      return value;
    } catch (error) {
      const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
      if (code && !error.code) error.code = code[1];
      throw error;
    }
  }
  function cacheWindowBounds(window, bounds, revision, notified = false) {
    if (Number.isSafeInteger(revision)) {
      if (revision < (window._boundsRevision || 0)) return;
      window._boundsRevision = revision;
    }
    window._bounds = {x: bounds.x, y: bounds.y,
      width: bounds.width, height: bounds.height};
    if (notified) window._lastNotifiedBounds = {...window._bounds};
  }
  function emitWindowBoundsChange(window, moved, resized) {
    if (window._destroyed) return;
    if (moved) window.emit('move', createBrowserWindowEvent(window));
    if (resized && !window._destroyed)
      window.emit('resize', createBrowserWindowEvent(window));
  }
  function emitWebContentsEvent(contents, name, details) {
    const event = createBrowserWindowEvent(contents);
    if (name === 'did-fail-load' || name === 'did-fail-provisional-load') {
      contents.emit(name, event, details.errorCode, details.errorDescription,
          details.validatedURL, details.isMainFrame,
          details.frameProcessId ?? -1, details.frameRoutingId ?? -1);
    } else {
      contents.emit(name, event, details);
    }
  }

  function createBrowserWindowEvent(window, cancellable = false) {
    return {
      sender: window,
      returnValue: undefined,
      defaultPrevented: false,
      preventDefault() {
        if (cancellable) {
          this.defaultPrevented = true;
          this.returnValue = false;
        }
      },
    };
  }
  class BrowserWindow extends EventEmitter {
    constructor(options = {}) {
      super();
      if (appExitState !== 'running') {
        throw Object.assign(new Error('Cannot create a BrowserWindow while the application exits'),
                            {code: 'ERR_APP_QUITTING'});
      }
      const webPreferences = {
        contextIsolation: true,
        nodeIntegration: false,
        ...options.webPreferences,
      };
      if (webPreferences.session !== undefined) {
        if (!(webPreferences.session instanceof Session))
          throw new TypeError('webPreferences.session must be a Session');
        webPreferences.partition = webPreferences.session.partition;
        delete webPreferences.session;
      }
      if (webPreferences.partition !== undefined &&
          typeof webPreferences.partition !== 'string')
        throw new TypeError('webPreferences.partition must be a string');
      this._webPreferences = Object.freeze(electronSandbox.normalizePreferences(webPreferences));
      if (this._webPreferences.preload &&
          (typeof this._webPreferences.preload !== 'string' ||
           !pathModule.isAbsolute(this._webPreferences.preload))) {
        throw new TypeError('BrowserWindow preload must be an absolute path');
      }
      const title = options.title === undefined ? 'Electron' : String(options.title);
      const titleBarStyle = options.titleBarStyle ?? 'default';
      const paintWhenInitiallyHidden = options.paintWhenInitiallyHidden === undefined
          ? true : options.paintWhenInitiallyHidden;
      if (typeof paintWhenInitiallyHidden !== 'boolean')
        throw new TypeError('paintWhenInitiallyHidden must be a boolean');
      if (__xenonPlatform === 'darwin' &&
          !['default', 'hidden', 'hiddenInset'].includes(titleBarStyle)) {
        return electronUnsupported('BrowserWindow titleBarStyle ' + titleBarStyle);
      }
      if (typeof __xenonCreateBrowserWindow !== 'function') {
        return electronUnsupported('BrowserWindow creation');
      }
      const created =
          callElectronSession(this._webPreferences.partition || '', 'create-window', {options: {
                  width: options.width || 800,
                  height: options.height || 600,
                  show: options.show !== false,
                  paintWhenInitiallyHidden,
                  frame: options.frame !== false,
                  transparent: Boolean(options.transparent),
                  hasShadow: options.hasShadow !== false,
                  titleBarStyle,
                  parentId: options.parent && options.parent.id
                      ? options.parent.id
                      : 0,
                  title,
                }});
      this.id = created.id;
      try {
        callBrowserWindow(this, 'set-web-preferences', this._webPreferences);
        this._hwnd = created.hwnd || '0';
        this._parent = options.parent || null;
        this.webContents = new WebContents(this);
        // Apply the selected Session's explicit UA or the app fallback through
        // the native bridge before the caller can start its first navigation.
        const userAgent = this.webContents.session.getUserAgent();
        if (userAgent) this.webContents.setUserAgent(userAgent);
        this._visible = options.show !== false;
        this._minimized = false;
        this._maximized = false;
        this._fullscreen = false;
        this._destroyed = false;
        this._resizable = options.resizable !== false;
        this._movable = options.movable !== false;
        this._maximizable = options.maximizable !== false;
        this._minimizable = options.minimizable !== false;
        this._closable = options.closable !== false;
        this._focusable = options.focusable !== false;
        this._enabled = true;
        this._opacity = 1;
        if (__xenonPlatform !== 'darwin') this._hasShadow = options.hasShadow !== false;
        this._bounds = {
          x: options.x != null ? Number(options.x) : 0,
          y: options.y != null ? Number(options.y) : 0,
          width: Number(options.width) || 800,
          height: Number(options.height) || 600,
        };
        this._minSize = {width: 0, height: 0};
        this._maxSize = {width: 0, height: 0};
        // Electron's constructor ignores values that cannot be converted to int.
        const minimumDimension = value => Number.isInteger(value) &&
            value >= -2147483648 && value <= 2147483647 ? Math.max(0, value) : 0;
        const minWidth = minimumDimension(options.minWidth);
        const minHeight = minimumDimension(options.minHeight);
        if (minWidth || minHeight) this.setMinimumSize(minWidth, minHeight);
        this._title = title;
        this._backgroundColor = '#000000';
        if (options.backgroundColor !== undefined) {
          this.setBackgroundColor(options.backgroundColor);
        }
        this._windowMessageHooks = new Map();
        if (options.alwaysOnTop) {
          this.setAlwaysOnTop(true);
        }
        // Electron: omit x/y → center; `center: true` also centers. Child windows
        // with a parent keep the host-synced parent bounds unless positioned.
        if (options.x != null || options.y != null) {
          const actual = callBrowserWindow(this, 'set-bounds', this._bounds);
          if (actual && typeof actual === 'object') this._bounds = actual;
        } else if (options.center === true || !this._parent) {
          callBrowserWindow(this, 'center');
          const actual = callBrowserWindow(this, 'get-bounds');
          if (actual && typeof actual === 'object') this._bounds = actual;
        } else {
          const actual = callBrowserWindow(this, 'get-bounds');
          if (actual && typeof actual === 'object') this._bounds = actual;
        }
        this._lastNotifiedBounds = {...this._bounds};
      } catch (error) {
        // A failed constructor never enters browserWindows, so ordinary app
        // lifecycle cleanup cannot find this partially initialized window.
        this._destroyed = true;
        try {
          try {
            if (this.webContents) destroyWebContents(this.webContents);
          } finally {
            if (typeof __xenonCloseBrowserWindow === 'function')
              __xenonCloseBrowserWindow(this.id);
          }
        } catch (cleanupError) {
          console.error('[xenon-ipc] Failed to release an uninitialized BrowserWindow:', cleanupError);
        }
        throw error;
      }
      browserWindows.push(this);
      allBrowserWindowsClosed = false;
    }
    static getAllWindows() { return browserWindows.filter(item => !item._destroyed); }
    static getFocusedWindow() { return BrowserWindow.getAllWindows().find(window => window.isFocused()) || null; }
    static fromId(id) { return browserWindows.find(item => item.id === id) || null; }
    static fromWebContents(contents) {
      if (!contents) return null;
      if (contents instanceof WebContents && contents._guest?.standalone)
        return contents.getOwnerBrowserWindow();
      if (contents._owner instanceof BrowserWindow) return contents._owner;
      if (contents instanceof WebContents && contents._owner) {
        return contents._owner;
      }
      const windowId = Number(
          contents.__xenonWindowId ?? contents.windowId ?? 0);
      const matched = browserWindows.find(item => item.webContents === contents ||
          (windowId > 0 && item.id === windowId)) || null;
      if (!matched && typeof __xenonLog === 'function') {
        __xenonLog('BrowserWindow.fromWebContents miss windowId=' + windowId +
                   ' windows=' + browserWindows.map(item => item.id).join(','));
      }
      return matched;
    }
    getNativeWindowHandle() {
      const buf = Buffer.alloc(8);
      try {
        buf.writeBigUInt64LE(BigInt(this._hwnd), 0);
      } catch (e) {
        buf.writeUInt32LE(Number(this._hwnd) >>> 0, 0);
      }
      return buf;
    }
    getParentWindow() { return this._parent; }
    setParentWindow(parent) {
      if (parent !== null && !(parent instanceof BrowserWindow)) {
        throw new TypeError('parent must be a BrowserWindow or null');
      }
      for (let ancestor = parent; ancestor; ancestor = ancestor._parent) {
        if (ancestor === this) throw new Error('BrowserWindow parent cycle');
      }
      if (this._destroyed || parent?._destroyed) throw new Error('Object has been destroyed');
      callBrowserWindow(this, 'set-parent-window', {parentId: parent?.id || 0},
          () => { this._parent = parent; });
    }
    isModal() { return false; }
    moveTop() { callBrowserWindow(this, 'move-top'); }
    setProgressBar(progress, options = {}) {
      if (typeof progress !== 'number' || !Number.isFinite(progress)) {
        throw new TypeError('progress must be a finite number');
      }
      callBrowserWindow(this, 'set-progress-bar', {progress, mode: options.mode});
    }
    getChildWindows() {
      return BrowserWindow.getAllWindows().filter(item => item._parent === this);
    }
    get resizable() { return this._resizable; }
    set resizable(value) { this.setResizable(value); }
    get movable() { return this._movable; }
    set movable(value) { this.setMovable(value); }
    get maximizable() { return this._maximizable; }
    set maximizable(value) { this.setMaximizable(value); }
    get minimizable() { return this._minimizable; }
    set minimizable(value) { this.setMinimizable(value); }
    get closable() { return this._closable; }
    set closable(value) { this.setClosable(value); }
    get focusable() { return this._focusable; }
    set focusable(value) { this.setFocusable(value); }
    loadURL(url, options) { return this.webContents.loadURL(url, options); }
    loadFile(file, options) { return this.webContents.loadFile(file, options); }
    show() {
      if (typeof __xenonBrowserWindowCall === 'function') {
        callBrowserWindow(this, 'show');
      } else {
        this._visible = true;
        this.emit('show');
      }
    }
    showInactive() {
      if (typeof __xenonBrowserWindowCall === 'function') {
        callBrowserWindow(this, 'show-inactive');
      } else {
        this.show();
      }
    }
    hide() {
      if (typeof __xenonBrowserWindowCall === 'function') {
        callBrowserWindow(this, 'hide');
      } else {
        this._visible = false;
        this.emit('hide');
      }
    }
    isVisible() {
      const value = callBrowserWindow(this, 'is-visible');
      return typeof value === 'boolean' ? value : this._visible;
    }
    focus() { callBrowserWindow(this, 'focus'); }
    blur() { callBrowserWindow(this, 'blur'); }
    isFocused() {
      const value = callBrowserWindow(this, 'is-focused');
      return typeof value === 'boolean' ? value : false;
    }
    minimize() { callBrowserWindow(this, 'minimize'); }
    isMinimized() {
      const value = callBrowserWindow(this, 'is-minimized');
      return typeof value === 'boolean' ? value : this._minimized;
    }
    maximize() { callBrowserWindow(this, 'maximize'); }
    unmaximize() { callBrowserWindow(this, 'unmaximize'); }
    isMaximized() {
      const value = callBrowserWindow(this, 'is-maximized');
      return typeof value === 'boolean' ? value : this._maximized;
    }
    restore() { callBrowserWindow(this, 'restore'); }
    setFullScreen(value) {
      callBrowserWindow(this, 'set-fullscreen', {value: Boolean(value)});
    }
    isFullScreen() {
      const value = callBrowserWindow(this, 'is-fullscreen');
      return typeof value === 'boolean' ? value : this._fullscreen;
    }
    setAlwaysOnTop(value) {
      callBrowserWindow(this, 'set-always-on-top', {value: Boolean(value)});
    }
    isAlwaysOnTop() { return callBrowserWindow(this, 'is-always-on-top'); }
    setSkipTaskbar() {}
    setTitleBarOverlay(options) {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      if (!options || typeof options !== 'object' || Array.isArray(options))
        throw new TypeError('Title bar overlay options must be an object');
      for (const name of ['color', 'symbolColor']) {
        if (options[name] !== undefined && typeof options[name] !== 'string')
          throw new TypeError('Title bar overlay ' + name + ' must be a string');
      }
      if (options.height !== undefined &&
          (!Number.isInteger(options.height) || options.height < 0))
        throw new TypeError('Title bar overlay height must be a non-negative integer');
      // Optional native caption styling is deferred. Keep the host's caption
      // and geometry instead of claiming custom overlay controls were created.
    }
    setTitle(title) {
      const value = String(title ?? '');
      callBrowserWindow(this, 'set-title', {title: value});
      this._title = value;
    }
    getTitle() { return this._title; }
    setBounds(bounds) {
      if (!bounds || typeof bounds !== 'object') return;
      const actual = callBrowserWindow(this, 'set-bounds', bounds);
      // The call commits the returned geometry before notifying listeners.
      // Do not overwrite a newer value set by a reentrant resize listener.
      if (!actual || typeof actual !== 'object') this._bounds = {
          x: bounds.x == null ? this._bounds.x : Number(bounds.x),
          y: bounds.y == null ? this._bounds.y : Number(bounds.y),
          width: bounds.width == null ? this._bounds.width : Number(bounds.width),
          height: bounds.height == null ? this._bounds.height : Number(bounds.height),
        };
    }
    getBounds() {
      callBrowserWindow(this, 'get-bounds');
      return {...this._bounds};
    }
    getContentBounds() { return this.getBounds(); }
    setContentBounds(bounds) { this.setBounds(bounds); }
    getNormalBounds() {
      const actual = callBrowserWindow(this, 'get-normal-bounds');
      return actual && typeof actual === 'object' ? {...actual} : this.getBounds();
    }
    setSize(width, height) { this.setBounds({width, height}); }
    getSize() { const bounds = this.getBounds(); return [bounds.width, bounds.height]; }
    setContentSize(width, height) { this.setSize(width, height); }
    getContentSize() { return this.getSize(); }
    setPosition(x, y) { this.setBounds({x, y}); }
    getPosition() { const bounds = this.getBounds(); return [bounds.x, bounds.y]; }
    center() { callBrowserWindow(this, 'center'); }
    setMinimumSize(width, height) {
      for (const value of [width, height]) {
        if (!Number.isInteger(value) || value < -2147483648 || value > 2147483647)
          throw new TypeError('Minimum size dimensions must be 32-bit integers');
      }
      callBrowserWindow(this, 'set-minimum-size', {
        width: Math.max(0, width), height: Math.max(0, height),
      }, actual => {
        if (actual && typeof actual === 'object') this._minSize = actual;
        else if (width > 0 || height > 0)
          this._minSize = {width: Math.max(0, width), height: Math.max(0, height)};
      });
    }
    getMinimumSize() {
      const actual = callBrowserWindow(this, 'get-minimum-size');
      if (actual && typeof actual === 'object') this._minSize = actual;
      return [this._minSize.width, this._minSize.height];
    }
    setMaximumSize(width, height) {
      this._maxSize = {width: Number(width) || 0, height: Number(height) || 0};
    }
    getMaximumSize() { return [this._maxSize.width, this._maxSize.height]; }
    setResizable(value) { this._resizable = Boolean(value); }
    isResizable() { return this._resizable; }
    setMovable(value) { this._movable = Boolean(value); }
    isMovable() { return this._movable; }
    setMaximizable(value) { this._maximizable = Boolean(value); }
    isMaximizable() { return this._maximizable; }
    setMinimizable(value) { this._minimizable = Boolean(value); }
    isMinimizable() { return this._minimizable; }
    setClosable(value) { this._closable = Boolean(value); }
    isClosable() { return this._closable; }
    setFocusable(value) { this._focusable = Boolean(value); }
    isFocusable() { return this._focusable; }
    setEnabled(value) {
      this._enabled = Boolean(value);
      callBrowserWindow(this, 'set-enabled', {value: this._enabled});
    }
    isEnabled() {
      const value = callBrowserWindow(this, 'is-enabled');
      return typeof value === 'boolean' ? value : this._enabled;
    }
    setOpacity(value) {
      this._opacity = Number(value);
      callBrowserWindow(this, 'set-opacity', {value: this._opacity});
    }
    getOpacity() { return this._opacity; }
    setHasShadow(value) {
      if (__xenonPlatform !== 'darwin') {
        this._hasShadow = Boolean(value);
        return;
      }
      if (this._destroyed) throw new Error('Object has been destroyed');
      if (typeof value !== 'boolean') throw new TypeError('hasShadow must be a boolean');
      callBrowserWindow(this, 'set-has-shadow', {value});
    }
    hasShadow() {
      if (__xenonPlatform !== 'darwin') return this._hasShadow;
      if (this._destroyed) throw new Error('Object has been destroyed');
      const value = callBrowserWindow(this, 'has-shadow');
      if (typeof value !== 'boolean') return electronUnsupported('BrowserWindow.hasShadow');
      return value;
    }
    setBackgroundColor(color) {
      if (typeof __xenonBrowserWindowCall !== 'function') {
        const error = new Error('BrowserWindow background color host is unavailable');
        error.code = 'ERR_NOT_SUPPORTED';
        throw error;
      }
      callBrowserWindow(this, 'set-background-color', {color});
      this._backgroundColor = color;
    }
    getBackgroundColor() { return this._backgroundColor; }
    setAspectRatio() {}
    setKiosk() {}
    isKiosk() { return false; }
    isNormal() { return !this._maximized && !this._minimized && !this._fullscreen; }
    setSimpleFullScreen(value) { this.setFullScreen(value); }
    isSimpleFullScreen() { return this.isFullScreen(); }
    isFullScreenable() { return true; }
    setFullScreenable() {}
    setVisibleOnAllWorkspaces() {}
    isVisibleOnAllWorkspaces() { return false; }
    setWindowButtonVisibility(visible) {
      if (__xenonPlatform !== 'darwin') return;
      if (this._destroyed) throw new Error('Object has been destroyed');
      if (typeof visible !== 'boolean') throw new TypeError('visible must be a boolean');
      callBrowserWindow(this, 'set-window-button-visibility', {visible});
    }
    setSheetOffset(offsetY, offsetX = 0) {
      if (this._destroyed) throw new Error('Object has been destroyed');
      if (!Number.isFinite(offsetY) || !Number.isFinite(offsetX))
        throw new TypeError('Sheet offsets must be finite numbers');
      this._sheetOffsetY = offsetY;
      this._sheetOffsetX = offsetX;
    }
    setDocumentEdited() {}
    isDocumentEdited() { return false; }
    setRepresentedFilename() {}
    getRepresentedFilename() { return ''; }
    setMenu() {}
    removeMenu() {}
    setMenuBarVisibility() {}
    setAutoHideMenuBar() {}
    isMenuBarVisible() { return false; }
    setIcon() {}
    setOverlayIcon() {}
    setShape(rects) {
      callBrowserWindow(this, 'set-shape', {
        rects: Array.isArray(rects) ? rects : [],
      });
    }
    flashFrame() {}
    setContentProtection() {}
    setAppDetails() {}
    setThumbnailClip() {}
    setThumbnailToolTip() {}
    close() {
      if (this._destroyed || this._closeRequested) return;
      this._closeRequested = true;
      const event = createBrowserWindowEvent(this, true);
      try {
        this.emit('close', event);
        // Renderer beforeunload/unload is not yet part of this host close path.
        if (!event.defaultPrevented) this.destroy();
      } catch (error) {
        cancelAppQuit();
        throw error;
      } finally {
        this._closeRequested = false;
        if (!this._destroyed && event.defaultPrevented) cancelAppQuit();
        continueAppQuit();
      }
    }
    destroy() {
      finishBrowserWindowClose(this, true);
    }
    isDestroyed() { return this._destroyed; }
    isWindowMessageHooked(message) {
      return this._windowMessageHooks.has(Number(message));
    }
    hookWindowMessage(message, callback) {
      const key = Number(message);
      if (!Number.isInteger(key) || typeof callback !== 'function') return;
      this._windowMessageHooks.set(key, callback);
      callBrowserWindow(this, 'hook-window-message', {message: key});
    }
    unhookWindowMessage(message) {
      const key = Number(message);
      this._windowMessageHooks.delete(key);
      callBrowserWindow(this, 'unhook-window-message', {message: key});
    }
    setIgnoreMouseEvents(value) {
      callBrowserWindow(this, 'set-ignore-mouse-events', {
        value: Boolean(value),
      });
    }
  }

  const nativeMenuClicks = new Map();
  let activeNativeMenu = null;
  globalThis.__xenonDispatchBrowserWindowEvent =
      (windowId, eventName, details = null) => {
    if (windowId === 0 && eventName === 'notification') {
      globalThis.__xenonDispatchNotificationEvent?.(details);
      return;
    }
    if (windowId === 0 && eventName === '__xenon-system-theme') {
      if (!details || !details.state) return;
      if (details.accentChanged && typeof details.state.accentColor === 'string') {
        systemPreferences.emit('accent-color-changed', {}, details.state.accentColor);
      }
      if (details.nativeThemeChanged) nativeTheme.emit('updated');
      return;
    }
    if (windowId === 0 && eventName === '__xenon-second-instance') {
      if (!details || !Array.isArray(details.argv) ||
          !details.argv.every(arg => typeof arg === 'string') ||
          typeof details.workingDirectory !== 'string') return;
      globalThis.__xenonDispatchAppEvent('second-instance',
          [details.argv, details.workingDirectory, details.additionalData]);
      return;
    }
    if (windowId === 0 && eventName === '__xenon-session-event') {
      handleElectronSessionEvent(details);
      return;
    }
    if (windowId === 0 && eventName === '__xenon-permission-request') {
      dispatchElectronPermissionRequest(details);
      return;
    }
    if (windowId === 0 && eventName === '__xenon-mac-player-event') {
      globalThis.__xenonDispatchMacPlayerEvent?.(details);
      return;
    }
    if (windowId === 0 && eventName === 'auto-updater') {
      if (!details || typeof details.event !== 'string' ||
          !Array.isArray(details.args)) return;
      // This is a host-wide updater event, so it also works before the app has
      // created a BrowserWindow. Use the existing document endpoint transport.
      for (const endpointId of rendererWebContents.keys()) {
        __xenonSendToRenderer(endpointId, '__xenon:auto-updater-event',
            [details.event, ...details.args]);
      }
      const args = details.event === 'error' ?
          [new Error(String(details.args[0]))] : details.args;
      // A container that does not use the host updater must not acquire an
      // unhandled EventEmitter error merely because another app requested it.
      if (details.event !== 'error' || autoUpdater.listenerCount('error') > 0) {
        autoUpdater.emit(details.event, ...args);
      }
      return;
    }
    const guest = guestWebContents.get(Number(windowId));
    if (eventName === 'electron-view') {
      electronViews?.dispatch(details);
      return;
    }
    if (guest && eventName === 'web-contents-before-input-event') {
      const event = createBrowserWindowEvent(guest, true);
      guest.emit('before-input-event', event, details);
      return event.defaultPrevented;
    }
    if (guest && eventName.startsWith('guest-')) {
      const name = eventName.slice('guest-'.length);
      if (details?.url) guest._url = details.url;
      if (name === 'new-window') {
        const handler = webContentsState.get(guest).windowOpenHandler;
        const decision = handler ? handler(details) :
            {action: guest._guest.preferences.allowPopups ? 'allow' : 'deny'};
        if (decision?.action === 'allow') {
          const popup = new BrowserWindow(decision.overrideBrowserWindowOptions || {});
          popup.loadURL(details.url);
        }
      }
      if (guest._guest.host && !guest._guest.host.isDestroyed())
        guest._guest.host.send('__xenon:guest-event', guest.id, name, details || {});
      if (name === 'destroyed') {
        guestWebContents.delete(Number(windowId));
        destroyWebContents(guest);
      } else if (name === 'will-prevent-unload' && guest._guest.standalone) {
        const event = createBrowserWindowEvent(guest, true);
        guest.emit(name, event);
        if (event.defaultPrevented && !guest.isDestroyed()) guest.close();
      } else {
        emitWebContentsEvent(guest, name, details);
      }
      return;
    }
    const win = BrowserWindow.fromId(Number(windowId));
    // Native notifications already queued before destroy can arrive after the
    // backing Widget is gone. No application listener may reuse that window.
    if (!win || win.isDestroyed())
      return eventName === 'web-contents-before-input-event' ? true : undefined;
    if (eventName === 'web-contents-before-input-event') {
      const event = createBrowserWindowEvent(win.webContents, true);
      win.webContents.emit('before-input-event', event, details);
      return event.defaultPrevented;
    }
    if (eventName === 'close-requested') {
      win.close();
      return;
    }
    if (eventName === 'native-menu-command') {
      // This flag controls application menu accelerators only. Normal popup
      // selection and shortcuts implemented by the page continue to work.
      if (details?.triggeredByAccelerator === true &&
          webContentsState.get(win.webContents).ignoreMenuShortcuts) return;
      const click = nativeMenuClicks.get(Number(details && details.commandId));
      if (typeof click === 'function') {
        try {
          click(details || {}, win, win.webContents);
        } catch (error) {
          console.error(error);
        }
      }
      return;
    }
    if (eventName === 'native-menu-closed') {
      const menu = activeNativeMenu;
      activeNativeMenu = null;
      nativeMenuClicks.clear();
      if (menu) {
        menu.emit('menu-will-close');
      }
      return;
    }
    if (eventName === 'web-contents-page-title-updated' && details) {
      const title = String(details.title ?? '');
      const explicitSet = Boolean(details.explicitSet);
      const contents = win.webContents;
      contents._pageTitle = title;
      const event = createBrowserWindowEvent(win, true);
      win.emit('page-title-updated', event, title, explicitSet);
      if (!event.defaultPrevented && !win.isDestroyed()) {
        win.setTitle(title);
      }
      if (!contents.isDestroyed()) {
        contents.emit('page-title-updated',
            createBrowserWindowEvent(contents), title, explicitSet);
      }
      return;
    }
    const webContentsEventPrefix = 'web-contents-';
    if (eventName.startsWith(webContentsEventPrefix)) {
      emitWebContentsEvent(win.webContents,
          eventName.slice(webContentsEventPrefix.length), details);
      return;
    }
    if (eventName === 'bounds-changed' && details) {
      if (Number.isSafeInteger(details.revision)) {
        if (details.revision <= (win._lastAsyncBoundsRevision || 0)) return;
        win._lastAsyncBoundsRevision = details.revision;
        // An earlier native drag may arrive after a synchronous setter. Keep
        // its notification, but never roll the current geometry back to it.
        cacheWindowBounds(win, details, details.revision, true);
        emitWindowBoundsChange(win, details.moved, details.resized);
        return;
      }
      // A synchronous setBounds/getBounds can refresh the value cache before
      // the native event arrives. Compare notifications independently so that
      // querying geometry cannot suppress the corresponding move/resize event.
      const old = win._lastNotifiedBounds;
      const moved = old.x !== details.x || old.y !== details.y;
      const resized = old.width !== details.width ||
          old.height !== details.height;
      win._bounds = {...details};
      win._lastNotifiedBounds = {...details};
      emitWindowBoundsChange(win, moved, resized);
      return;
    }
    if (eventName === 'state-changed' && details) {
      const wasMinimized = win._minimized;
      const wasMaximized = win._maximized;
      const wasFullscreen = win._fullscreen;
      win._minimized = Boolean(details.minimized);
      win._maximized = Boolean(details.maximized);
      win._fullscreen = Boolean(details.fullscreen);
      if (!wasMinimized && win._minimized) {
        win.emit('minimize', createBrowserWindowEvent(win));
      }
      if (wasMinimized && !win._minimized) {
        win.emit('restore', createBrowserWindowEvent(win));
      }
      if (!wasMaximized && win._maximized) {
        win.emit('maximize', createBrowserWindowEvent(win));
      }
      if (wasMaximized && !win._maximized) {
        win.emit('unmaximize', createBrowserWindowEvent(win));
      }
      if (!wasFullscreen && win._fullscreen) {
        win.emit('enter-full-screen', createBrowserWindowEvent(win));
      }
      if (wasFullscreen && !win._fullscreen) {
        win.emit('leave-full-screen', createBrowserWindowEvent(win));
      }
      return;
    }
    if (eventName === 'window-message' && details) {
      const callback = win._windowMessageHooks.get(Number(details.message));
      if (!callback) return;
      const wParam = Buffer.alloc(8);
      const lParam = Buffer.alloc(8);
      wParam.writeBigUInt64LE(BigInt.asUintN(64, BigInt(details.wParam)), 0);
      lParam.writeBigUInt64LE(BigInt.asUintN(64, BigInt(details.lParam)), 0);
      callback(wParam, lParam);
      return;
    }
    if (eventName === 'show' || eventName === 'hide') {
      const visible = eventName === 'show';
      if (win._visible !== visible) {
        win._visible = visible;
        win.emit(eventName, createBrowserWindowEvent(win));
      }
      return;
    }
    if (eventName === 'closed') {
      finishBrowserWindowClose(win, false);
      return;
    }
    win.emit(eventName, createBrowserWindowEvent(win), details);
  };

  function serializeNativeImage(icon) {
    if (!icon) return undefined;
    if (typeof icon === 'string' && icon) return icon;
    if (typeof icon.path === 'string' && icon.path) return icon.path;
    return undefined;
  }
  function serializeMenuTemplate(items, state) {
    const serialized = [];
    let radioGroup = 0;
    for (const item of items || []) {
      if (!item) continue;
      const type = item.type || (item.submenu ? 'submenu' : 'normal');
      if (type === 'separator') radioGroup = 0;
      if (type === 'separator') {
        serialized.push({type: 'separator'});
        continue;
      }
      const id = ++state.nextId;
      if (typeof item.click === 'function') {
        state.clicks.set(id, item.click.bind(item));
      }
      const entry = {
        id,
        type: (type === 'submenu' || item.submenu) ? 'submenu' : type,
        label: String(item.label || ''),
        enabled: item.enabled !== false,
        visible: item.visible !== false,
        checked: Boolean(item.checked),
      };
      const icon = serializeNativeImage(item.icon);
      if (icon) entry.icon = icon;
      if (entry.type === 'radio') {
        if (!radioGroup) radioGroup = id;
        entry.groupId = radioGroup;
      }
      if (entry.type === 'submenu') {
        const submenu = Array.isArray(item.submenu)
            ? item.submenu
            : (item.submenu && Array.isArray(item.submenu.items)
                   ? item.submenu.items : []);
        entry.submenu = serializeMenuTemplate(submenu, state);
      }
      serialized.push(entry);
    }
    return serialized;
  }
  class Tray extends EventEmitter {
    setImage() {}
    setToolTip() {}
    setContextMenu() {}
    destroy() {}
  }
  class Menu extends EventEmitter {
    static buildFromTemplate(template) {
      if (!Array.isArray(template)) throw new TypeError('Menu template must be an array');
      const menu = new Menu();
      for (const item of template) menu.append(item instanceof MenuItem ? item : new MenuItem(item));
      return menu;
    }
    static setApplicationMenu() {}
    static getApplicationMenu() { return null; }
    constructor() {
      super();
      this.items = [];
    }
    append(item) { this.insert(this.items.length, item); }
    insert(position, item) {
      if (!Number.isInteger(position) || position < 0 || position > this.items.length)
        throw new RangeError('Menu insertion position is out of range');
      if (!(item instanceof MenuItem)) throw new TypeError('Expected a MenuItem');
      if (item.menu) throw new Error('MenuItem already belongs to a menu');
      Object.defineProperty(item, 'menu', {value: this, enumerable: true});
      this.items.splice(position, 0, item);
      if (item.type === 'radio' && item.checked) item.checked = true;
    }
    getMenuItemById(id) {
      if (typeof id !== 'string') throw new TypeError('Menu item id must be a string');
      const visited = new Set();
      function find(items) {
        if (!Array.isArray(items) || visited.has(items)) return null;
        visited.add(items);
        for (const item of items) {
          if (!item || typeof item !== 'object') continue;
          if (item.id === id) return item;
          const submenu = Array.isArray(item.submenu) ? item.submenu : item.submenu?.items;
          const found = find(submenu);
          if (found) return found;
        }
        return null;
      }
      return find(this.items);
    }
    _preparePopup(seen = new Set()) {
      if (seen.has(this)) throw new TypeError('Menu contains a submenu cycle');
      seen.add(this);
      let radios = [];
      const finish = () => {
        if (radios.length && !radios.some(item => item.checked)) radios[0].checked = true;
        radios = [];
      };
      for (const item of this.items) {
        if (item.type === 'separator') finish();
        else if (item.type === 'radio') radios.push(item);
        if (item.submenu instanceof Menu) item.submenu._preparePopup(seen);
      }
      finish();
      seen.delete(this);
    }
    popup(options = {}) {
      const window = (options && options.window) ||
          BrowserWindow.getFocusedWindow();
      if (!window || window._destroyed) return;
      this.emit('menu-will-show');
      this._preparePopup();
      nativeMenuClicks.clear();
      activeNativeMenu = this;
      const payload = {
        items: serializeMenuTemplate(this.items, {
          nextId: 0,
          clicks: nativeMenuClicks,
        }),
      };
      if (options && Number.isFinite(options.x) && Number.isFinite(options.y)) {
        payload.x = Math.round(options.x);
        payload.y = Math.round(options.y);
      }
      callBrowserWindow(window, 'popup-menu', payload);
    }
    closePopup() {}
  }
  const MenuItem = createMenuItemClass({Menu, unsupported: electronUnsupported, platform: __xenonPlatform,
    runRole(role, window, contents) {
      if (role === 'quit') { app.quit(); return true; }
      if (['filemenu', 'editmenu', 'viewmenu', 'windowmenu'].includes(role)) return true;
      if (!window) return false;
      if (role === 'close') { window.close(); return true; }
      if (role === 'minimize') { window.minimize(); return true; }
      if (role === 'zoom') { window.isMaximized() ? window.unmaximize() : window.maximize(); return true; }
      if (role === 'togglefullscreen') { window.setFullScreen(!window.isFullScreen()); return true; }
      if (!contents) return false;
      if (role === 'reload' || role === 'forcereload') {
        callBrowserWindow(window, 'reload', {ignoreCache: role === 'forcereload'});
        return true;
      }
      if (['resetzoom', 'zoomin', 'zoomout'].includes(role)) {
        contents.setZoomLevel(role === 'resetzoom' ? 0 : contents.getZoomLevel() + (role === 'zoomin' ? 0.5 : -0.5));
        return true;
      }
      if (['undo', 'redo', 'cut', 'copy', 'paste', 'pasteandmatchstyle', 'delete', 'selectall'].includes(role)) {
        callBrowserWindow(window, 'edit-command', {action: role});
        return true;
      }
      if (role === 'toggledevtools') return electronUnsupported('MenuItem.role.' + role);
      return false;
    },
  });
  const nativeTheme = new EventEmitter();
  Object.defineProperty(nativeTheme, 'themeSource', {
    enumerable: true,
    get: () => 'system',
    set: value => {
      if (!['system', 'light', 'dark'].includes(value)) {
        throw Object.assign(new TypeError('themeSource must be system, light, or dark'),
            {code: 'ERR_INVALID_ARG_VALUE'});
      }
      // A container must not change the global browser theme for other apps.
      if (value !== 'system') electronUnsupported('nativeTheme.themeSource override');
    },
  });
  for (const property of ['shouldUseDarkColors', 'shouldUseHighContrastColors',
    'shouldUseInvertedColorScheme', 'prefersReducedTransparency']) {
    Object.defineProperty(nativeTheme, property, {
      enumerable: true,
      get: () => {
        try {
          return Boolean(callElectronApi('nativeTheme.getState')?.[property]);
        } catch (_) {
          return false;
        }
      },
    });
  }
  const systemPreferences = new EventEmitter();
  const userDefaults = new Map();
  systemPreferences.getAccentColor = () => callElectronApi('systemPreferences.getAccentColor');
  systemPreferences.isAeroGlassEnabled = () => callElectronApi('systemPreferences.isAeroGlassEnabled');
  systemPreferences.setUserDefault = (key, value, type) => {
    userDefaults.set(String(key), value);
  };
  systemPreferences.getUserDefault = (key, type) => {
    if (key === 'AppleInterfaceStyle') {
      return nativeTheme.shouldUseDarkColors ? 'Dark' : null;
    }
    return userDefaults.get(String(key));
  };
  systemPreferences.removeUserDefault = key => {
    userDefaults.delete(String(key));
  };
  systemPreferences.isTrustedAccessibilityClient = prompt => true;
  systemPreferences.getMediaAccessStatus = mediaType => 'granted';
  systemPreferences.askForMediaAccess = async mediaType => true;
  systemPreferences.getEffectiveAppearance = () => (nativeTheme.shouldUseDarkColors ? 'dark' : 'light');
  systemPreferences.getColor = color => '#000000';
  systemPreferences.getSystemColor = color => '#000000';
  systemPreferences.canPromptTouchID = () => false;

  let nextNotificationId = 1;
  const notificationSubscriptions = new Map();
  systemPreferences.subscribeLocalNotification = (event, callback) => {
    const id = nextNotificationId++;
    if (typeof callback === 'function') {
      notificationSubscriptions.set(id, {event, callback});
    }
    return id;
  };
  systemPreferences.unsubscribeLocalNotification = id => {
    notificationSubscriptions.delete(Number(id));
  };
  systemPreferences.subscribeNotification = (event, callback) => {
    const id = nextNotificationId++;
    if (typeof callback === 'function') {
      notificationSubscriptions.set(id, {event, callback});
    }
    return id;
  };
  systemPreferences.unsubscribeNotification = id => {
    notificationSubscriptions.delete(Number(id));
  };
  systemPreferences.subscribeWorkspaceNotification = (event, callback) => {
    const id = nextNotificationId++;
    if (typeof callback === 'function') {
      notificationSubscriptions.set(id, {event, callback});
    }
    return id;
  };
  systemPreferences.unsubscribeWorkspaceNotification = id => {
    notificationSubscriptions.delete(Number(id));
  };
  systemPreferences.postLocalNotification = (event, userInfo) => {};
  systemPreferences.postNotification = (event, userInfo) => {};
  systemPreferences.postWorkspaceNotification = (event, userInfo) => {};

  nativeTheme.on('updated', () => {
    for (const sub of notificationSubscriptions.values()) {
      if (sub.event === 'AppleInterfaceThemeChangedNotification') {
        try {
          sub.callback('AppleInterfaceThemeChangedNotification', {});
        } catch (_) {}
      }
    }
  });
  const powerMonitor = new EventEmitter();
  const autoUpdater = new EventEmitter();
  for (const property of ['autoDownload', 'autoInstallOnAppQuit']) {
    Object.defineProperty(autoUpdater, property, {
      enumerable: true,
      get: () => callElectronApi('autoUpdater.getConfiguration')[property],
      set: value => {
        callElectronApi('autoUpdater.configure', {[property]: value});
      },
    });
  }
  autoUpdater.setFeedURL = (urlOrOptions) => {
    const url = typeof urlOrOptions === 'string' ? urlOrOptions : (urlOrOptions && urlOrOptions.url ? urlOrOptions.url : '');
    return callElectronApi('autoUpdater.setFeedURL', {url});
  };
  autoUpdater.getFeedURL = async () => {
    const res = await callElectronApi('autoUpdater.getFeedURL', {});
    return (res && res.url) || '';
  };
  autoUpdater.checkForUpdates = async () => {
    return callElectronApi('autoUpdater.checkForUpdates', {});
  };
  autoUpdater.checkForUpdatesAndNotify = async () => {
    return autoUpdater.checkForUpdates();
  };
  autoUpdater.downloadUpdate = async () => {
    return callElectronApi('autoUpdater.downloadUpdate', {});
  };
  autoUpdater.quitAndInstall = () => {
    callElectronApi('autoUpdater.quitAndInstall', {});
  };
  const clipboardApi = createClipboardApi({
    callSync: callElectronApi,
    callAsync: async (operation, details) => callElectronApi(operation, details),
    Blob: globalThis.Blob,
  });
  const electronModule = {
    app,
    crashReporter: createCrashReporter({
      main: true, app, callHost: callElectronApi,
      callLocal: typeof globalThis.__xenonCrashReporterCall === 'function'
        ? (operation, details) => globalThis.__xenonCrashReporterCall({operation, ...details})
        : undefined,
    }),
    ipcMain,
    BrowserWindow,
    webContents: {
      fromId: id => allWebContents.get(Number(id)),
      getAllWebContents: () => [...allWebContents.values()],
    },
    shell: {
      openExternal: async (url, options = {}) => {
        callElectronApi('shell.openExternal', {url, options});
      },
      openPath: async path => callElectronApi('shell.openPath', {path}),
      showItemInFolder: path => { callElectronApi('shell.showItemInFolder', {path}); },
      trashItem: async targetPath => {
        if (typeof targetPath !== 'string' || !targetPath) {
          throw Object.assign(new TypeError('trashItem requires a non-empty path string'),
                              {code: 'ERR_INVALID_ARG_TYPE'});
        }
        const resolved = pathModule.resolve(targetPath);
        if (__xenonPlatform === 'darwin') {
          const home = typeof osModule?.homedir === 'function' ? osModule.homedir() : '';
          if (home) {
            const trashDir = pathModule.join(home, '.Trash');
            const base = pathModule.basename(resolved);
            const ext = pathModule.extname(base);
            const stem = ext ? base.slice(0, -ext.length) : base;
            let dest = pathModule.join(trashDir, base);
            if (fsModule.existsSync(dest)) {
              dest = pathModule.join(trashDir, `${stem} ${Date.now()}${ext}`);
            }
            try {
              await fsPromises.rename(resolved, dest);
              return;
            } catch (_) {}
          }
        }
        await fsPromises.rm(resolved, {recursive: true, force: false});
      },
      beep: () => {},
    },
    dialog: {
      showErrorBox: (title, content) => {
        if (typeof title !== 'string' || typeof content !== 'string') {
          throw Object.assign(new TypeError('Error dialog title and content must be strings'),
                              {code: 'ERR_INVALID_ARG_TYPE'});
        }
        if (typeof __xenonBrowserWindowCall !== 'function')
          return electronUnsupported('dialog.showErrorBox');
        try { __xenonBrowserWindowCall(0, 'electron-error-box', {title, content}); }
        catch (error) {
          const code = /^([A-Z][A-Z_]+):/.exec(String(error.message));
          if (code && !error.code) error.code = code[1];
          throw error;
        }
      },
      ...createElectronDialogs({BrowserWindow, isReady: () => appReady, platform: __xenonPlatform,
        callSync: typeof __xenonBrowserWindowCall === 'function'
          ? (...args) => __xenonBrowserWindowCall(...args) : undefined,
        callAsync: typeof __xenonBrowserWindowCallAsync === 'function'
          ? (...args) => __xenonBrowserWindowCallAsync(...args) : undefined,
      }),
    },
    Tray,
    Menu,
    MenuItem,
    nativeTheme,
    systemPreferences,
    powerMonitor,
    session,
    protocol,
    globalShortcut: {
      register: () => electronUnsupported('globalShortcut.register'),
      registerAll: () => electronUnsupported('globalShortcut.registerAll'),
      // No registration can succeed in this runtime, so cleanup is a valid no-op.
      unregister() {}, unregisterAll() {}, isRegistered: () => false,
    },
    nativeImage: createNativeImageApi({
      callSync: callElectronApi,
      readFileSync: path => fsModule.readFileSync(path),
    }),
    clipboard: clipboardApi.clipboard,
    ClipboardItem: clipboardApi.ClipboardItem,
    screen: (() => {
      const call = (method, options = {}) => {
        if (typeof __xenonBrowserWindowCall !== 'function') {
          const error = new Error('Electron screen host is unavailable');
          error.code = 'ERR_NOT_SUPPORTED';
          throw error;
        }
        // Screen queries have no BrowserWindow. The host reserves id 0 for
        // these application-wide reads on its UI thread.
        return callBrowserWindow({id: 0}, 'screen', {method, ...options});
      };
      return Object.assign(new EventEmitter(), {
        getPrimaryDisplay: () => call('getPrimaryDisplay'),
        getAllDisplays: () => call('getAllDisplays'),
        getDisplayMatching: rect => call('getDisplayMatching', {rect}),
        getDisplayNearestPoint: point => call('getDisplayNearestPoint', {point}),
        getCursorScreenPoint: () => call('getCursorScreenPoint'),
      });
    })(),
    autoUpdater,
  };
  electronModule.default = electronModule;
