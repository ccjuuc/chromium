  // Protocol callbacks stay in their owning main isolate and Session. Native
  // registration publishes only the scheme/session identity to the browser.
  const declaredProtocolSchemes = new Set();
  const sessionProtocolHandlers = new Map();
  const pendingProtocolRequests = new Map();
  const protocolBodyLimit = 32 * 1024 * 1024;

  function declareProtocolSchemes(entries) {
    for (const entry of entries) declaredProtocolSchemes.add(entry.scheme.toLowerCase());
  }

  function createSessionProtocol(partition) {
    const handlers = new Map();
    sessionProtocolHandlers.set(partition, handlers);
    function schemeName(value) {
      if (typeof value !== 'string' || !/^[a-z][a-z0-9+.-]*$/i.test(value)) {
        throw new TypeError('Expected a protocol scheme without a colon');
      }
      return value.toLowerCase();
    }
    const result = Object.fromEntries([
      'registerFileProtocol', 'registerBufferProtocol', 'registerStringProtocol',
      'registerHttpProtocol', 'registerStreamProtocol', 'unregisterProtocol',
      'isProtocolRegistered', 'interceptFileProtocol', 'interceptStringProtocol',
      'interceptBufferProtocol', 'interceptHttpProtocol', 'interceptStreamProtocol',
      'uninterceptProtocol', 'isProtocolIntercepted',
    ].map(method => [method, () => electronUnsupported(`protocol.${method}`)]));
    return Object.assign(result, {
      handle(value, handler) {
        const scheme = schemeName(value);
        if (typeof handler !== 'function') throw new TypeError('Protocol handler must be a function');
        if (!declaredProtocolSchemes.has(scheme)) {
          throw new Error('Protocol scheme was not declared by this application: ' + scheme);
        }
        if (handlers.has(scheme)) throw new Error('Protocol is already handled: ' + scheme);
        if (typeof __xenonBrowserWindowCall !== 'function') return electronUnsupported('protocol.handle');
        __xenonBrowserWindowCall(0, 'protocol-handler', {partition, scheme, handled: true});
        handlers.set(scheme, handler);
      },
      unhandle(value) {
        const scheme = schemeName(value);
        if (!handlers.has(scheme)) return;
        __xenonBrowserWindowCall(0, 'protocol-handler', {partition, scheme, handled: false});
        handlers.delete(scheme);
      },
      isProtocolHandled(value) { return handlers.has(schemeName(value)); },
    });
  }

  globalThis.__xenonDispatchProtocol = async (partition, id, details) => {
    if (typeof id !== 'string' || pendingProtocolRequests.has(id)) throw new Error('Invalid protocol request identity');
    if (pendingProtocolRequests.size >= 128) throw new Error('Too many pending protocol requests');
    const parsed = new URL(details.url);
    const handler = sessionProtocolHandlers.get(partition)?.get(parsed.protocol.slice(0, -1));
    if (!handler) throw new Error('Protocol has no handler in this session');
    const controller = new AbortController();
    pendingProtocolRequests.set(id, controller);
    try {
      const request = new Request(parsed.href, {
        method: details.method, headers: details.headers,
        body: details.body?.byteLength ? details.body : undefined,
        signal: controller.signal,
      });
      const aborted = new Promise((_resolve, reject) => {
        controller.signal.addEventListener('abort', () => reject(controller.signal.reason), {once: true});
      });
      const response = await Promise.race([Promise.resolve().then(() => handler(request)), aborted]);
      if (!(response instanceof Response)) throw new TypeError('Protocol handler must return a Response');
      const status = response.status;
      const statusText = response.statusText;
      const headers = [...response.headers];
      if (!Number.isInteger(status) || status < 200 || status > 599 ||
          typeof statusText !== 'string' || statusText.length > 1024 || /[\r\n\0]/.test(statusText)) {
        throw new TypeError('Invalid protocol response status');
      }
      if (headers.reduce((size, [name, value]) => size + Buffer.byteLength(name) + Buffer.byteLength(value), 0) > 65536) {
        throw new RangeError('Protocol response headers exceed 64 KiB');
      }
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
