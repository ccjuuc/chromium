  // Protocol callbacks stay in their owning main isolate and Session. Native
  // registration publishes only the scheme/session identity to the browser.
  const sessionProtocolHandlers = new Map();
  const pendingProtocolRequests = new Map();
  const protocolBodyLimit = 32 * 1024 * 1024;

  function createSessionProtocol(partition) {
    const handlers = new Map();
    sessionProtocolHandlers.set(partition, handlers);
    function schemeName(value) {
      if (typeof value !== 'string' || value.length > 64 || !/^[a-z][a-z0-9+.-]*$/i.test(value)) {
        throw new TypeError('Expected a protocol scheme without a colon');
      }
      return value.toLowerCase();
    }
    function registration(value, handler, kind, intercepted, callback) {
      const scheme = schemeName(value);
      if (typeof handler !== 'function') throw new TypeError('Protocol handler must be a function');
      if (callback !== undefined && typeof callback !== 'function')
        throw new TypeError('Protocol completion callback must be a function');
      if (intercepted ? scheme !== 'file' : /^(?:about|android-app|blob|content|data|devtools|dom-distiller|externalfile|file|filesystem|ftp|https?|isolated-app|javascript|mailto|tel|urn|view-source|ws|wss|chrome.*)$/.test(scheme))
        return electronUnsupported(`protocol handler for ${scheme}`);
      if (handlers.has(scheme)) {
        if (kind === 'response') throw new Error('Protocol is already handled: ' + scheme);
        callback?.(new Error('The scheme has been registered'));
        return false;
      }
      if (typeof __xenonBrowserWindowCall !== 'function') return electronUnsupported('protocol handler registration');
      __xenonBrowserWindowCall(0, 'protocol-handler', {partition, scheme, handled: true});
      handlers.set(scheme, {kind, intercepted, handler});
      callback?.(null);
      return true;
    }
    function unregister(value, intercepted, callback) {
      const scheme = schemeName(value);
      if (callback !== undefined && typeof callback !== 'function')
        throw new TypeError('Protocol completion callback must be a function');
      const entry = handlers.get(scheme);
      if (!entry || entry.intercepted !== intercepted) {
        callback?.(new Error('The scheme has not been registered'));
        return false;
      }
      __xenonBrowserWindowCall(0, 'protocol-handler', {partition, scheme, handled: false});
      handlers.delete(scheme);
      callback?.(null);
      return true;
    }
    const result = Object.fromEntries([
      'registerBufferProtocol', 'registerStringProtocol',
      'registerHttpProtocol', 'registerStreamProtocol', 'interceptStringProtocol',
      'interceptBufferProtocol', 'interceptHttpProtocol', 'interceptStreamProtocol',
    ].map(method => [method, () => electronUnsupported(`protocol.${method}`)]));
    return Object.assign(result, {
      handle(value, handler) {
        const scheme = schemeName(value);
        registration(scheme, handler, 'response', scheme === 'file');
      },
      unhandle(value) {
        const scheme = schemeName(value);
        if (!unregister(scheme, scheme === 'file'))
          throw new Error('Failed to unhandle protocol: ' + scheme);
      },
      isProtocolHandled(value) { return handlers.has(schemeName(value)); },
      registerFileProtocol(value, handler, callback) {
        return registration(value, handler, 'file', false, callback);
      },
      interceptFileProtocol(value, handler, callback) {
        return registration(value, handler, 'file', true, callback);
      },
      unregisterProtocol(value, callback) { return unregister(value, false, callback); },
      uninterceptProtocol(value, callback) { return unregister(value, true, callback); },
      isProtocolRegistered(value) { return handlers.get(schemeName(value))?.intercepted === false; },
      isProtocolIntercepted(value) { return handlers.get(schemeName(value))?.intercepted === true; },
    });
  }

  function checkProtocolHeaders(headers) {
    if (headers.reduce((size, [name, value]) => size + Buffer.byteLength(name) + Buffer.byteLength(value), 0) > 65536)
      throw new RangeError('Protocol response headers exceed 64 KiB');
    return headers;
  }

  function fileProtocolResponse(value) {
    const response = typeof value === 'string' ? {path: value} : value;
    if (!response || typeof response !== 'object' || Array.isArray(response))
      throw new TypeError('File protocol callback must provide a path or response object');
    if (Object.hasOwn(response, 'error')) {
      if (!Number.isInteger(response.error) || response.error >= 0 || response.error < -2147483648)
        throw new TypeError('Protocol error must be a negative network error code');
      return {error: response.error};
    }
    if (typeof response.path !== 'string' || !response.path || response.path.includes('\0'))
      throw new TypeError('File protocol response requires a nonempty path without NUL');
    const headers = [];
    if (response.headers !== undefined) {
      if (!response.headers || typeof response.headers !== 'object' || Array.isArray(response.headers))
        throw new TypeError('File protocol headers must be an object');
      for (const [name, values] of Object.entries(response.headers)) {
        for (const value of Array.isArray(values) ? values : [values]) {
          if (typeof value !== 'string') throw new TypeError('Protocol header values must be strings');
          headers.push(...new Headers({[name]: value}));
        }
      }
    }
    if (response.mimeType !== undefined && typeof response.mimeType !== 'string')
      throw new TypeError('Protocol mimeType must be a string');
    if (response.charset !== undefined && typeof response.charset !== 'string')
      throw new TypeError('Protocol charset must be a string');
    if (response.mimeType && !headers.some(([name]) => name === 'content-type')) {
      headers.push(...new Headers({'content-type': response.mimeType +
          (response.charset ? '; charset=' + response.charset : '')}));
    }
    // Electron's file response authorizes the returned bytes for this custom
    // protocol; the native loader still enforces the original request's CORS.
    if (!headers.some(([name]) => name === 'access-control-allow-origin'))
      headers.push(['access-control-allow-origin', '*']);
    const status = response.statusCode === undefined ? 200 : response.statusCode;
    if (!Number.isInteger(status) || status < 200 || status > 599)
      throw new TypeError('Invalid protocol response status');
    const result = {kind: 'file', path: response.path, status, statusText: '',
      headers: checkProtocolHeaders(headers)};
    for (const name of ['method', 'referrer']) {
      if (response[name] === undefined) continue;
      if (typeof response[name] !== 'string' || /[\r\n\0]/.test(response[name]))
        throw new TypeError('Invalid protocol response ' + name);
      result[name] = response[name];
    }
    return result;
  }

  globalThis.__xenonDispatchProtocol = async (partition, id, details) => {
    if (typeof id !== 'string' || pendingProtocolRequests.has(id)) throw new Error('Invalid protocol request identity');
    if (pendingProtocolRequests.size >= 128) throw new Error('Too many pending protocol requests');
    const parsed = new URL(details.url);
    const registration = sessionProtocolHandlers.get(partition)?.get(parsed.protocol.slice(0, -1));
    if (!registration) throw new Error('Protocol has no handler in this session');
    const controller = new AbortController();
    pendingProtocolRequests.set(id, controller);
    try {
      const aborted = new Promise((_resolve, reject) => {
        controller.signal.addEventListener('abort', () => reject(controller.signal.reason), {once: true});
      });
      if (registration.kind === 'file') {
        const request = {url: parsed.href, method: details.method,
          referrer: details.referrer || '', headers: Object.fromEntries(details.headers || [])};
        if (details.body?.byteLength) request.uploadData = [{bytes: Buffer.from(details.body)}];
        const response = await Promise.race([new Promise((resolve, reject) => {
          try { registration.handler(request, resolve); } catch (error) { reject(error); }
        }), aborted]);
        return fileProtocolResponse(response);
      }
      const request = new Request(parsed.href, {
        method: details.method, headers: details.headers,
        body: details.body?.byteLength ? details.body : undefined,
        signal: controller.signal,
      });
      const response = await Promise.race([Promise.resolve().then(() => registration.handler(request)), aborted]);
      if (!(response instanceof Response)) throw new TypeError('Protocol handler must return a Response');
      if (response.type === 'error') return {error: -2};
      const status = response.status;
      const statusText = response.statusText;
      const headers = [...response.headers];
      if (!Number.isInteger(status) || status < 200 || status > 599 ||
          typeof statusText !== 'string' || statusText.length > 1024 || /[\r\n\0]/.test(statusText)) {
        throw new TypeError('Invalid protocol response status');
      }
      checkProtocolHeaders(headers);
      const body = new Uint8Array(await Promise.race([response.arrayBuffer(), aborted]));
      if (body.byteLength > protocolBodyLimit) throw new RangeError('Protocol response exceeds 32 MiB');
      if (controller.signal.aborted) throw controller.signal.reason;
      return {status, statusText, headers, body};
    } finally {
      pendingProtocolRequests.delete(id);
    }
  };
  globalThis.__xenonCancelProtocol = id => {
    pendingProtocolRequests.get(id)?.abort(Object.assign(new Error('Protocol request aborted'), {code: 'ABORT_ERR'}));
  };
