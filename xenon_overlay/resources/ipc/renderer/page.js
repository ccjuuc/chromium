  // Locale identifiers used by application i18n commonly contain underscores
  // (for example zh_CN), while HTTP Accept-Language requires BCP 47 language
  // tags (zh-CN). An underscore makes this otherwise CORS-safelisted header
  // require preflight. Normalize only this standard header at the transport
  // boundary; request bodies and application response state remain untouched.
  const normalizeAcceptLanguage = (value) =>
      String(value == null ? '' : value).replace(/_/g, '-');

  if (typeof globalThis.fetch === 'function' &&
      typeof Headers === 'function') {
    const originalFetch = globalThis.fetch.bind(globalThis);
    const normalizeHeaders = (headers) => {
      const normalized = new Headers(headers || {});
      if (normalized.has('accept-language')) {
        normalized.set(
            'accept-language',
            normalizeAcceptLanguage(normalized.get('accept-language')));
      }
      return normalized;
    };
    globalThis.fetch = (input, init) => {
      const nextInit = init ? Object.assign({}, init) : {};
      if (nextInit.headers) {
        nextInit.headers = normalizeHeaders(nextInit.headers);
      } else if (typeof Request === 'function' && input instanceof Request) {
        input = new Request(input, {headers: normalizeHeaders(input.headers)});
      }
      return originalFetch(input, nextInit);
    };
  }

  if (typeof XMLHttpRequest === 'function' && XMLHttpRequest.prototype &&
      typeof XMLHttpRequest.prototype.setRequestHeader === 'function') {
    const originalSetRequestHeader =
        XMLHttpRequest.prototype.setRequestHeader;
    XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
      if (String(name).toLowerCase() === 'accept-language') {
        value = normalizeAcceptLanguage(value);
      }
      return originalSetRequestHeader.call(this, name, value);
    };
  }

  if (typeof window !== 'undefined') {
    window.Buffer = Buffer;
    window.process = globalThis.process;
    window.require = globalThis.require;
    window.__xenonElectronRequire = globalThis.require;
    const evaluate = globalThis.eval;
    ipcRenderer.on('__xenon:execute-javascript', async (_event, id, code) => {
      try {
        const value = await evaluate(code);
        transport.send('__xenon:execute-javascript-result', id, true, value);
      } catch (error) {
        transport.send('__xenon:execute-javascript-result', id, false, String(error.message || error));
      }
    });

    // A webview owns a Browser-managed inner WebContents. The blank iframe is
    // only Chromium's attachment point; it never navigates to the guest URL.
    const guestElements = new Map();
    ipcRenderer.on('__xenon:guest-event', (_event, id, name, details = {}) => {
      const el = guestElements.get(id);
      if (!el) return;
      if (details.url) el._guestUrl = details.url;
      if (details.title) el._guestTitle = details.title;
      if ('canGoBack' in details) el._canGoBack = details.canGoBack;
      if ('canGoForward' in details) el._canGoForward = details.canGoForward;
      if (name === 'did-start-loading') el._loading = true;
      if (name === 'did-stop-loading') el._loading = false;
      el._emit(name, details);
      if (name === 'destroyed') {
        guestElements.delete(id);
        el._guestId = 0;
        el._attaching = null;
        el._guestFrame.remove();
        el._guestFrame = null;
      }
    });

    function upgradeWebView(el) {
      if (!el || el.__xenon_webview_upgraded__) {
        el?._ensureGuestAttached?.();
        return;
      }
      el.__xenon_webview_upgraded__ = true;
      Object.assign(el.style, {
        display: el.style.display || 'block',
        width: el.style.width || '100%', height: el.style.height || '100%',
      });
      el._guestId = 0;
      el._guestUrl = '';
      el._guestTitle = '';
      el._loading = false;
      el._emit = (name, details = {}) => {
        const event = new CustomEvent(name, {detail: details});
        Object.assign(event, details);
        el.dispatchEvent(event);
      };
      const preferences = () => {
        const prefs = {contextIsolation: true, sandbox: true};
        for (const item of (el.getAttribute('webpreferences') || '').split(',')) {
          const [key, raw] = item.trim().split('=');
          if (!key) continue;
          prefs[key] = raw === undefined || /^(yes|true|1)$/i.test(raw) ? true :
              /^(no|false|0)$/i.test(raw) ? false : raw;
        }
        prefs.preload = el.getAttribute('preload') || '';
        prefs.userAgent = el.getAttribute('useragent') || '';
        prefs.nodeIntegration = el.hasAttribute('nodeintegration');
        prefs.nodeIntegrationInSubFrames = el.hasAttribute('nodeintegrationinsubframes');
        prefs.allowPopups = el.hasAttribute('allowpopups');
        if (el.hasAttribute('partition')) prefs.partition = el.getAttribute('partition');
        return prefs;
      };
      el._ensureGuestAttached = () => {
        if (!el.isConnected || el._guestId || el._attaching) return;
        // Vue may set attributes after createElement(). Wait until the element
        // is connected and all initial attributes have been assigned.
        el._attaching = Promise.resolve().then(async () => {
          if (!el.isConnected) { el._attaching = null; return; }
          const frame = document.createElement('iframe');
          frame.src = 'about:blank';
          Object.assign(frame.style, {width: '100%', height: '100%', border: '0', display: 'block'});
          el._guestFrame = frame;
          el.appendChild(frame);
          const id = await transport.attachGuest(frame, preferences());
          el._guestId = id;
          guestElements.set(id, el);
          const url = el.getAttribute('src');
          if (url) await ipcRenderer.invoke('__xenon:guest-call', id, 'loadURL', [url]);
        }).catch(error => {
          console.error('[xenon-ipc] Unable to attach webview:', error);
          el._emit('did-fail-load', {errorCode: -2, errorDescription: error.message,
            validatedURL: el.getAttribute('src') || '', isMainFrame: true});
        });
      };
      const call = async (method, ...args) => {
        el._ensureGuestAttached();
        await el._attaching;
        if (!el._guestId) throw new Error('Guest WebContents is not attached');
        return ipcRenderer.invoke('__xenon:guest-call', el._guestId, method, args);
      };
      Object.defineProperty(el, 'src', {
        get: () => el.getAttribute('src') || '',
        set: value => el.setAttribute('src', String(value || '')),
        configurable: true,
      });
      const observer = new MutationObserver(mutations => {
        for (const mutation of mutations) {
          if (el._guestId && mutation.attributeName === 'src') {
            call('loadURL', el.src).catch(error => console.error(error));
          } else if (el._guestId && mutation.attributeName === 'useragent') {
            call('setUserAgent', el.getAttribute('useragent')).catch(error => console.error(error));
          }
        }
        el._ensureGuestAttached();
      });
      observer.observe(el, {attributes: true, attributeFilter: ['src', 'useragent']});
      el.loadURL = url => call('loadURL', String(url));
      el.getURL = () => el._guestUrl || el.src;
      el.getTitle = () => el._guestTitle;
      el.isLoading = () => el._loading;
      el.isWaitingForResponse = () => el._loading;
      el.canGoBack = () => Boolean(el._canGoBack);
      el.canGoForward = () => Boolean(el._canGoForward);
      for (const method of ['reload', 'stop', 'goBack', 'goForward',
                            'send', 'sendToFrame', 'executeJavaScript']) {
        el[method] = (...args) => call(method, ...args);
      }
      el.setUserAgent = ua => el.setAttribute('useragent', String(ua));
      el.getUserAgent = () => el.getAttribute('useragent') || navigator.userAgent;
      el.getWebContentsId = () => {
        if (!el._guestId) throw new Error('Guest WebContents is not attached');
        return el._guestId;
      };
      el.focus = () => el._guestFrame?.focus();
      el._ensureGuestAttached();
    }

    if (typeof Document !== 'undefined' && Document.prototype) {
      for (const method of ['createElement', 'createElementNS']) {
        const original = Document.prototype[method];
        Document.prototype[method] = function(...args) {
          const el = original.apply(this, args);
          if (el.nodeName === 'WEBVIEW') upgradeWebView(el);
          return el;
        };
      }
    }
    const startWebviewObserver = () => {
      if (!document.documentElement) {
        document.addEventListener('DOMContentLoaded', startWebviewObserver, {once: true});
        return;
      }
      new MutationObserver(mutations => {
        for (const mutation of mutations) {
          for (const node of mutation.addedNodes) {
            if (node.nodeType !== 1) continue;
            if (node.nodeName === 'WEBVIEW') upgradeWebView(node);
            node.querySelectorAll?.('webview').forEach(upgradeWebView);
          }
        }
      }).observe(document.documentElement, {childList: true, subtree: true});
      document.querySelectorAll('webview').forEach(upgradeWebView);
    };
    startWebviewObserver();

    // This bootstrap runs at document creation, before application scripts.
    // Resolve preferences through the document's registered WebContents, not
    // a process-global window ID or an application-specific preload alias.
    let preload = '';
    let preferences = null;
    try {
      preferences =
          transport.sendSync('__xenon:renderer-web-preferences');
      preload = preferences && preferences.preload;
      if (preload) {
        if (preferences.contextIsolation !== false) {
          throw new Error('Isolated-world preloads are not supported yet');
        }
        electronRequire(preload);
      }
    } catch (error) {
      console.error('[xenon-ipc] Unable to load preload script:', preload, error);
      ipcRenderer.send('__xenon:preload-error', preload, String(error.message || error));
    }
    // A guest preload can keep Node APIs in its CommonJS closure without
    // giving the remote page require(), process, or the privileged transport.
    if (injectedPaths.isGuest && preferences?.nodeIntegration !== true) {
      for (const key of ['require', 'process', 'Buffer', 'global', '__filename',
                        '__dirname', 'xenonIpcRenderer', '__xenonElectronRequire',
                        '__xenonElectronIpc', '__xenonPaths']) {
        delete globalThis[key];
      }
    } else if (globalThis.electron === undefined) {
      globalThis.electron = electron;
    }
  }
