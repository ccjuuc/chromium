  class Session extends EventEmitter {
    constructor(partition = '') {
      super();
      this.partition = String(partition || '');
      this._userAgent = '';
      this._downloadPath = '';
      this.webRequest = Object.fromEntries([
        'onBeforeRequest', 'onBeforeSendHeaders', 'onSendHeaders',
        'onHeadersReceived', 'onResponseStarted', 'onBeforeRedirect',
        'onCompleted', 'onErrorOccurred',
      ].map(method => [method, () => electronUnsupported(`webRequest.${method}`)]));
      this.cookies = Object.fromEntries(['get', 'set', 'remove', 'flushStore']
          .map(method => [method, async () => electronUnsupported(`cookies.${method}`)]));
      this.protocol = Object.fromEntries([
        'registerFileProtocol', 'registerBufferProtocol', 'registerStringProtocol',
        'registerHttpProtocol', 'registerStreamProtocol', 'unregisterProtocol',
        'isProtocolRegistered', 'interceptFileProtocol', 'interceptStringProtocol',
        'interceptBufferProtocol', 'interceptHttpProtocol', 'interceptStreamProtocol',
        'uninterceptProtocol', 'isProtocolIntercepted',
      ].map(method => [method, () => electronUnsupported(`protocol.${method}`)]));
    }
    getUserAgent() {
      return this._userAgent || String(__xenonUserAgent || '');
    }
    setUserAgent(userAgent) {
      this._userAgent = String(userAgent || '');
    }
    async setProxy(config) { return electronUnsupported('session.setProxy'); }
    async resolveProxy(url) { return electronUnsupported('session.resolveProxy'); }
    async clearCache() { return electronUnsupported('session.clearCache'); }
    async clearStorageData(options) { return electronUnsupported('session.clearStorageData'); }
    async clearAuthCache() { return electronUnsupported('session.clearAuthCache'); }
    async clearHostResolverCache() { return electronUnsupported('session.clearHostResolverCache'); }
    setDownloadPath(p) { return electronUnsupported('session.setDownloadPath'); }
    enableNetworkEmulation(options) { return electronUnsupported('session.enableNetworkEmulation'); }
    disableNetworkEmulation() { return electronUnsupported('session.disableNetworkEmulation'); }
    setCertificateVerifyProc(proc) { return electronUnsupported('session.setCertificateVerifyProc'); }
    setPermissionRequestHandler(handler) { return electronUnsupported('session.setPermissionRequestHandler'); }
    setPermissionCheckHandler(handler) { return electronUnsupported('session.setPermissionCheckHandler'); }
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
  const guestWebContents = new Map();
  const pendingGuestScripts = new Map();
  let nextGuestScriptId = 1;
  let nextWebContentsId = 1;

  function getSenderWebContents(sender) {
    let contents = rendererWebContents.get(sender.endpointId);
    if (!contents) {
      const owner = BrowserWindow.fromId(Number(sender.windowId));
      contents = guestWebContents.get(Number(sender.windowId)) ||
          (owner ? owner.webContents : new WebContents(null));
      rendererWebContents.set(sender.endpointId, contents);
      webContentsState.get(contents).endpoints.set(sender.endpointId, sender);
    }
    return contents;
  }

  function getMainEndpoint(contents) {
    // The document host only admits primary main frames. During navigation,
    // prefer the newly registered document over an endpoint being torn down.
    return [...webContentsState.get(contents).endpoints.values()]
        .filter(endpoint => endpoint.isMainFrame !== false).at(-1);
  }

  function destroyWebContents(contents) {
    const state = webContentsState.get(contents);
    if (state.destroyed) return;
    state.destroyed = true;
    for (const [id, pending] of pendingGuestScripts) {
      if (pending.contents !== contents) continue;
      clearTimeout(pending.timer);
      pendingGuestScripts.delete(id);
      pending.reject(new Error('WebContents was destroyed'));
    }
    const processIds = new Set();
    for (const endpoint of state.endpoints.values()) {
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
    if (attached) {
      getSenderWebContents(sender);
      return;
    }
    const contents = rendererWebContents.get(sender.endpointId);
    if (!contents) return;
    rendererWebContents.delete(sender.endpointId);
    const state = webContentsState.get(contents);
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
      this.session = defaultSession;
    }
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
    get frameId() { return getMainEndpoint(this)?.frameId || 0; }
    getProcessId() { return this.processId; }
    getOSProcessId() {
      if (this.isDestroyed()) throw new Error('Object has been destroyed');
      const id = this._guest?.nativeId ?? this._owner?.id;
      if (id === undefined) throw new Error('WebContents has no native owner');
      return callBrowserWindow({id}, 'get-os-process-id');
    }
    getOwnerBrowserWindow() { return this._owner; }
    getLastWebPreferences() {
      return this._guest ? {...this._guest.preferences} :
          this._owner ? {...this._owner._webPreferences} : null;
    }
    isDestroyed() { return webContentsState.get(this).destroyed; }
    getURL() { return this._url || ''; }
    getTitle() { return this._pageTitle || ''; }
    loadURL(url, options) {
      if (options && typeof options === 'object' && options.userAgent) {
        this.setUserAgent(options.userAgent);
      }
      this._url = mapRendererUrl(String(url || ''));
      const nativeId = this._guest?.nativeId || this._owner?.id;
      if (nativeId && typeof __xenonLoadBrowserWindowURL === 'function') {
        __xenonLoadBrowserWindowURL(nativeId, this._url);
      }
      return Promise.resolve();
    }
    executeJavaScript(code) {
      if (this._guest) {
        return new Promise((resolve, reject) => {
          if (this.isDestroyed() || !getMainEndpoint(this)) {
            reject(new Error('Guest renderer is unavailable'));
            return;
          }
          const id = nextGuestScriptId++;
          const timer = setTimeout(() => {
            pendingGuestScripts.delete(id);
            reject(new Error('Guest script evaluation timed out'));
          }, 30000);
          pendingGuestScripts.set(id, {contents: this, resolve, reject, timer});
          this.send('__xenon:execute-javascript', id, String(code));
        });
      }
      if (this._owner) {
        callBrowserWindow(
            this._owner, 'execute-javascript', {code: String(code || '')});
      }
      return Promise.resolve();
    }
    openDevTools() {}
    closeDevTools() {}
    isDevToolsOpened() { return false; }
    setWindowOpenHandler(handler) {
      if (typeof handler !== 'function') throw new TypeError('Expected a function');
      webContentsState.get(this).windowOpenHandler = handler;
    }
    setUserAgent(userAgent) {
      this._userAgent = String(userAgent || '');
      if (!this._guest && this.session && typeof this.session.setUserAgent === 'function') {
        this.session.setUserAgent(this._userAgent);
      }
      if (this._owner || this._guest) {
        callBrowserWindow(
            this._owner || {id: this._guest.nativeId}, 'set-user-agent', {value: this._userAgent});
      }
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
  }
  ipcMain.handle('__xenon:register-guest', (event, nativeId, preferences) => {
    if (!Number.isInteger(nativeId) || nativeId >= 0 || guestWebContents.has(nativeId)) {
      throw new Error('Invalid guest WebContents identity');
    }
    const contents = new WebContents(null);
    contents._guest = {nativeId, host: event.sender, preferences: {...preferences}};
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
    const pending = pendingGuestScripts.get(id);
    if (!pending || pending.contents !== event.sender) return;
    clearTimeout(pending.timer);
    pendingGuestScripts.delete(id);
    if (success) pending.resolve(value);
    else pending.reject(new Error(String(value)));
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
      this._webPreferences = Object.freeze({
        contextIsolation: true,
        nodeIntegration: false,
        ...options.webPreferences,
      });
      if (this._webPreferences.preload &&
          (typeof this._webPreferences.preload !== 'string' ||
           !pathModule.isAbsolute(this._webPreferences.preload))) {
        throw new TypeError('BrowserWindow preload must be an absolute path');
      }
      const title = options.title === undefined ? 'Electron' : String(options.title);
      if (typeof __xenonCreateBrowserWindow !== 'function') {
        return electronUnsupported('BrowserWindow creation');
      }
      const created =
          __xenonCreateBrowserWindow({
                  width: options.width || 800,
                  height: options.height || 600,
                  show: options.show !== false,
                  frame: options.frame !== false,
                  transparent: Boolean(options.transparent),
                  parentId: options.parent && options.parent.id
                      ? options.parent.id
                      : 0,
                  title,
                });
      this.id = created.id;
      try {
        callBrowserWindow(this, 'set-web-preferences', this._webPreferences);
        this._hwnd = created.hwnd || '0';
        this._parent = options.parent || null;
        this.webContents = new WebContents(this);
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
        this._hasShadow = options.hasShadow !== false;
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
    loadFile(file) { return this.webContents.loadURL(file); }
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
    setHasShadow(value) { this._hasShadow = Boolean(value); }
    hasShadow() { return this._hasShadow; }
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
    setWindowButtonVisibility() {}
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
    const guest = guestWebContents.get(Number(windowId));
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
      if (!guest._guest.host.isDestroyed())
        guest._guest.host.send('__xenon:guest-event', guest.id, name, details || {});
      if (name === 'destroyed') {
        guestWebContents.delete(Number(windowId));
        destroyWebContents(guest);
      } else {
        guest.emit(name, {sender: guest}, details);
      }
      return;
    }
    const win = BrowserWindow.fromId(Number(windowId));
    // Native notifications already queued before destroy can arrive after the
    // backing Widget is gone. No application listener may reuse that window.
    if (!win || win.isDestroyed()) return;
    if (eventName === 'close-requested') {
      win.close();
      return;
    }
    if (eventName === 'native-menu-command') {
      const click = nativeMenuClicks.get(Number(details && details.commandId));
      if (typeof click === 'function') {
        try {
          click();
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
      win.webContents.emit(
          eventName.slice(webContentsEventPrefix.length),
          createBrowserWindowEvent(win.webContents), details);
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
    for (const item of items || []) {
      if (!item) continue;
      const type = item.type || (item.submenu ? 'submenu' : 'normal');
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
      if (entry.type === 'radio' && item.groupId != null) {
        entry.groupId = Number(item.groupId) || 0;
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
    static buildFromTemplate(template) { return new Menu(template); }
    static setApplicationMenu() {}
    static getApplicationMenu() { return null; }
    constructor(template = []) {
      super();
      this.items = template;
    }
    popup(options = {}) {
      const window = (options && options.window) ||
          BrowserWindow.getFocusedWindow();
      if (!window || window._destroyed) return;
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
  const nativeTheme = new EventEmitter();
  nativeTheme.themeSource = 'system';
  Object.defineProperty(nativeTheme, 'shouldUseDarkColors', {get: () => false});
  const powerMonitor = new EventEmitter();
  const autoUpdater = new EventEmitter();
  autoUpdater.autoDownload = true;
  autoUpdater.autoInstallOnAppQuit = true;
  autoUpdater.setFeedURL = (urlOrOptions) => {
    const url = typeof urlOrOptions === 'string' ? urlOrOptions : (urlOrOptions && urlOrOptions.url ? urlOrOptions.url : '');
    return callElectronApi('autoUpdater.setFeedURL', {url});
  };
  autoUpdater.getFeedURL = async () => {
    const res = await callElectronApi('autoUpdater.getFeedURL', {});
    return (res && res.url) || '';
  };
  autoUpdater.checkForUpdates = async () => {
    autoUpdater.emit('checking-for-update');
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
  const electronModule = {
    app,
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
    },
    dialog: {
      showOpenDialog: async (winOrOpts, maybeOpts) => {
        const opts = (maybeOpts && typeof maybeOpts === 'object') ? maybeOpts : (winOrOpts && typeof winOrOpts === 'object' ? winOrOpts : {});
        if (typeof __xenonShowOpenDialog !== 'function') return electronUnsupported('dialog.showOpenDialog');
        const filePaths = __xenonShowOpenDialog(opts);
        return { canceled: filePaths.length === 0, filePaths };
      },
      showOpenDialogSync: (winOrOpts, maybeOpts) => {
        const opts = (maybeOpts && typeof maybeOpts === 'object') ? maybeOpts : (winOrOpts && typeof winOrOpts === 'object' ? winOrOpts : {});
        if (typeof __xenonShowOpenDialog !== 'function') return electronUnsupported('dialog.showOpenDialogSync');
        const filePaths = __xenonShowOpenDialog(opts);
        return filePaths.length > 0 ? filePaths : undefined;
      },
      showSaveDialog: async () => electronUnsupported('dialog.showSaveDialog'),
      showSaveDialogSync: () => electronUnsupported('dialog.showSaveDialogSync'),
      showMessageBox: async () => electronUnsupported('dialog.showMessageBox'),
      showMessageBoxSync: () => electronUnsupported('dialog.showMessageBoxSync'),
    },
    Tray,
    Menu,
    nativeTheme,
    systemPreferences: {isAeroGlassEnabled: () => false},
    powerMonitor,
    session,
    globalShortcut: {
      register: () => electronUnsupported('globalShortcut.register'),
      registerAll: () => electronUnsupported('globalShortcut.registerAll'),
      // No registration can succeed in this runtime, so cleanup is a valid no-op.
      unregister() {}, unregisterAll() {}, isRegistered: () => false,
    },
    nativeImage: {createEmpty: () => ({}), createFromPath: path => ({path})},
    clipboard: {
      readText: type => callElectronApi('clipboard.readText', {type}),
      writeText: (text, type) => { callElectronApi('clipboard.writeText', {text, type}); },
      readHTML: type => callElectronApi('clipboard.readHTML', {type}),
      writeHTML: (markup, type) => { callElectronApi('clipboard.writeHTML', {markup, type}); },
      clear: type => { callElectronApi('clipboard.clear', {type}); },
    },
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
