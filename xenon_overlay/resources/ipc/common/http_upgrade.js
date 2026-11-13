  // Node HTTP upgrades require a live byte stream. URLLoader is a buffered
  // browser request transport and cannot transfer ownership of that stream.
  // This path is shared by both Node module facades; it never uses browser
  // cookies, the browser cache, CORS permissions, or a WebSocket implementation.
  function isHttpUpgradeRequest(headers) {
    let upgrade = '', connection = '';
    for (const [name, value] of Object.entries(headers)) {
      if (name.toLowerCase() === 'upgrade') upgrade = String(value);
      else if (name.toLowerCase() === 'connection') connection = String(value);
    }
    return !!upgrade && connection.split(',').some(value =>
      value.trim().toLowerCase() === 'upgrade');
  }

  function startHttpUpgradeRequest(net, server, options, events) {
    const fail = (code, message) => Object.assign(new Error(message), {code});
    const url = new URL(options.url);
    if (url.protocol !== 'http:')
      throw fail('ERR_NOT_SUPPORTED', 'HTTPS protocol upgrades require a TLS socket transport');
    const settings = options.settings || {};
    if (settings.agent && settings.agent !== options.globalAgent)
      throw fail('ERR_NOT_SUPPORTED', 'Custom HTTP agents for protocol upgrades are not supported');
    for (const name of ['insecureHTTPParser', 'joinDuplicateHeaders']) {
      if (settings[name]) throw fail('ERR_NOT_SUPPORTED', 'HTTP upgrade option ' + name + ' is not supported');
    }
    const maxHeaderSize = settings.maxHeaderSize || 16 * 1024;
    if (settings.maxHeaderSize !== undefined) {
      if (typeof settings.maxHeaderSize !== 'number')
        throw fail('ERR_INVALID_ARG_TYPE', 'maxHeaderSize must be a number');
      if (!Number.isSafeInteger(settings.maxHeaderSize) || settings.maxHeaderSize < 0)
        throw fail('ERR_OUT_OF_RANGE', 'maxHeaderSize must be a non-negative integer');
    }
    const headers = {...options.headers};
    const names = new Set(Object.keys(headers).map(name => name.toLowerCase()));
    if (!names.has('host') && settings.setHost !== false) headers.Host = url.host;
    const auth = settings.auth !== undefined ? settings.auth :
      url.username || url.password ? decodeURIComponent(url.username) + ':' + decodeURIComponent(url.password) : undefined;
    if (auth && !names.has('authorization')) headers.Authorization = 'Basic ' + Buffer.from(auth).toString('base64');
    const body = options.body || Buffer.alloc(0);
    if (names.has('transfer-encoding'))
      throw fail('ERR_NOT_SUPPORTED', 'Chunked upload for protocol upgrades is not supported');
    if (!names.has('content-length') && body.length) headers['Content-Length'] = String(body.length);
    const method = options.method || 'GET';
    server.validateHeaderName(method, 'HTTP method');
    const path = settings.path || url.pathname + url.search;
    if (/[^\x21-\xff]/.test(path)) throw fail('ERR_UNESCAPED_CHARACTERS', 'Request path contains unescaped characters');
    const lines = [method + ' ' + path + ' HTTP/1.1'];
    for (const [name, value] of Object.entries(headers)) {
      server.validateHeaderName(name); server.validateHeaderValue(name, value);
      for (const item of Array.isArray(value) ? value : [value]) lines.push(name + ': ' + item);
    }
    const headerBytes = Buffer.from(lines.join('\r\n') + '\r\n\r\n', 'latin1');
    let socket, incoming, stopped = false, detached = false, sent = false;
    let data = Buffer.alloc(0), mode = 'headers', left = 0, parsing = false;
    let eof = false, timeout = options.timeout || 0, trailerBytes = 0;

    function removeListeners() {
      if (!socket) return;
      for (const [name, listener] of [['data', onData], ['end', onEnd],
        ['error', onError], ['close', onClose], ['timeout', onTimeout], ['connect', send]])
        socket.removeListener(name, listener);
    }
    function error(reason) {
      if (stopped || detached) return;
      stopped = true; removeListeners();
      if (incoming && !incoming.complete) incoming._abort();
      socket?.destroy(); events.error(reason);
    }
    function onError(reason) { error(reason); }
    function onTimeout() { if (!stopped && !detached) events.timeout(); }
    function onClose() {
      if (stopped || detached) return;
      if (!incoming) { error(fail('ECONNRESET', 'socket hang up')); return; }
      if (!incoming.complete) { error(fail('ECONNRESET', 'aborted')); return; }
      stopped = true; removeListeners(); events.close();
    }
    function onEnd() {
      eof = true;
      if (mode === 'eof') { parse(); return; }
      if (!incoming || !incoming.complete) error(fail('ECONNRESET', 'socket hang up'));
    }
    function onData(bytes) {
      if (stopped || detached) return;
      data = Buffer.concat([data, Buffer.from(bytes)]); parse();
    }
    function boundary() {
      for (let i = 0; i + 3 < data.length; ++i)
        if (data[i] === 13 && data[i + 1] === 10 && data[i + 2] === 13 && data[i + 3] === 10) return i;
      return -1;
    }
    function lineEnd() {
      for (let i = 0; i + 1 < data.length; ++i) if (data[i] === 13 && data[i + 1] === 10) return i;
      return -1;
    }
    function addHeader(line, result, raw) {
      const colon = line.indexOf(':');
      if (colon <= 0 || !/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(line.slice(0, colon)))
        throw fail('HPE_INVALID_HEADER_TOKEN', 'Invalid HTTP response header');
      const name = line.slice(0, colon), key = name.toLowerCase(), value = line.slice(colon + 1).replace(/^[ \t]+|[ \t]+$/g, '');
      if (/[^\t\x20-\x7e\x80-\xff]/.test(value)) throw fail('HPE_INVALID_HEADER_TOKEN', 'Invalid HTTP response header value');
      raw.push(name, value);
      if (key === 'content-length' && result[key] !== undefined)
        throw fail('HPE_UNEXPECTED_CONTENT_LENGTH', 'Duplicate Content-Length');
      if (key === 'set-cookie') (result[key] ||= []).push(value);
      else result[key] = result[key] === undefined ? value : result[key] + (key === 'cookie' ? '; ' : ', ') + value;
    }
    function parseHead(bytes) {
      const lines = bytes.toString('latin1').split('\r\n');
      const status = /^HTTP\/(1\.[01]) ([0-9]{3})(?: ([\t\x20-\x7e\x80-\xff]*))?$/.exec(lines.shift());
      if (!status) throw fail('HPE_INVALID_CONSTANT', 'Invalid HTTP response status line');
      const result = new server.IncomingMessage(socket, () => { if (!parsing) parse(); });
      result.statusCode = Number(status[2]); result.statusMessage = status[3] || '';
      result.httpVersion = status[1]; result.httpVersionMajor = 1; result.httpVersionMinor = Number(status[1][2]);
      for (const line of lines) addHeader(line, result.headers, result.rawHeaders);
      const fullHeaders = result.headers;
      const count = options.getMaxHeadersCount?.();
      // Node's _http_common initializes maxHeaderPairs to 2000 scalar entries.
      const pairs = typeof count === 'number' ? count << 1 : 2000;
      if (pairs > 0 && result.rawHeaders.length > pairs) {
        result.headers = Object.create(null);
        for (const line of lines.slice(0, pairs / 2)) addHeader(line, result.headers, []);
        // Node v24.19.0 src/node_http_parser.cc uses kMaxHeaderFieldsCount=32;
        // _http_common.js applies maxHeaderPairs when accumulating flushed
        // headers. Below that threshold IncomingMessage keeps the raw array.
        if (lines.length >= 32) result.rawHeaders.length = pairs;
      }
      const length = fullHeaders['content-length'], transfer = fullHeaders['transfer-encoding'];
      if (length !== undefined && transfer !== undefined) throw fail('HPE_UNEXPECTED_CONTENT_LENGTH', 'Conflicting HTTP response framing');
      if (length !== undefined && (!/^\d+$/.test(length) || !Number.isSafeInteger(Number(length))))
        throw fail('HPE_INVALID_CONTENT_LENGTH', 'Invalid HTTP response Content-Length');
      if (transfer !== undefined && transfer.toLowerCase() !== 'chunked')
        throw fail('HPE_INVALID_TRANSFER_ENCODING', 'Unsupported HTTP transfer encoding');
      return {response: result, headers: fullHeaders};
    }
    function complete() {
      mode = 'done'; socket.setTimeout(0); incoming._finish();
      // This transport has no connection pool. Release a non-upgraded socket
      // after its complete response, including servers that keep it alive.
      socket.destroy();
    }
    function available() {
      while (!stopped && !detached && mode !== 'done') {
        if (mode === 'headers') {
          const end = boundary();
          if (end < 0) {
            if (data.length > maxHeaderSize) error(fail('HPE_HEADER_OVERFLOW', 'HTTP response headers too large'));
            return;
          }
          if (end + 4 > maxHeaderSize) { error(fail('HPE_HEADER_OVERFLOW', 'HTTP response headers too large')); return; }
          let parsed;
          try { parsed = parseHead(data.subarray(0, end)); } catch (reason) { error(reason); return; }
          const {response, headers: responseHeaders} = parsed;
          data = data.subarray(end + 4);
          if (response.statusCode < 200 && response.statusCode !== 101) {
            events.information(response); continue;
          }
          incoming = response;
          if (response.statusCode === 101 && isHttpUpgradeRequest(responseHeaders)) {
            response.complete = true; detached = true; removeListeners(); socket.setTimeout(0);
            const head = data; data = Buffer.alloc(0);
            events.upgrade(response, socket, head); events.close(); return;
          }
          const length = responseHeaders['content-length'], transfer = responseHeaders['transfer-encoding'];
          const noBody = method === 'HEAD' || response.statusCode < 200 || response.statusCode === 204 || response.statusCode === 304;
          mode = noBody || length !== undefined ? 'body' : transfer ? 'chunk-size' : 'eof';
          left = noBody ? 0 : Number(length || 0);
          events.response(response);
          if (stopped) return;
          continue;
        }
        if (mode === 'body' || mode === 'chunk-data' || mode === 'eof') {
          if ((mode === 'eof' || left) && data.length) {
            const capacity = incoming._capacity();
            if (!capacity) { socket.pause(); return; }
            const size = Math.min(data.length, capacity, mode === 'eof' ? Infinity : left);
            const part = data.subarray(0, size); data = data.subarray(size);
            if (mode !== 'eof') left -= size;
            incoming._push(part); continue;
          }
          if (mode === 'eof') { if (eof) complete(); return; }
          if (left) return;
          if (mode === 'body') { complete(); return; }
          mode = 'chunk-crlf'; continue;
        }
        if (mode === 'chunk-crlf') {
          if (data.length < 2) return;
          if (data[0] !== 13 || data[1] !== 10) { error(fail('HPE_INVALID_CHUNK_SIZE', 'Invalid HTTP chunk ending')); return; }
          data = data.subarray(2); mode = 'chunk-size'; continue;
        }
        const end = lineEnd();
        if (end < 0) {
          if (data.length > maxHeaderSize) error(fail('HPE_HEADER_OVERFLOW', 'HTTP chunk header too large'));
          return;
        }
        if (end + 2 > maxHeaderSize) { error(fail('HPE_HEADER_OVERFLOW', 'HTTP chunk header too large')); return; }
        const line = data.subarray(0, end).toString('latin1'); data = data.subarray(end + 2);
        if (mode === 'chunk-size') {
          const size = line.split(';', 1)[0];
          if (!/^[0-9a-f]+$/i.test(size) || !Number.isSafeInteger(parseInt(size, 16))) {
            error(fail('HPE_INVALID_CHUNK_SIZE', 'Invalid HTTP chunk size')); return;
          }
          left = parseInt(size, 16); mode = left ? 'chunk-data' : 'trailers';
        } else if (!line) { complete(); return; }
        else {
          trailerBytes += end + 2;
          if (trailerBytes > maxHeaderSize) { error(fail('HPE_HEADER_OVERFLOW', 'HTTP trailers too large')); return; }
          try { addHeader(line, incoming.trailers, incoming.rawTrailers); } catch (reason) { error(reason); return; }
        }
      }
    }
    function parse() {
      if (parsing || stopped || detached) return;
      parsing = true;
      try { available(); }
      finally {
        parsing = false;
        if (!stopped && !detached && mode !== 'done' && (!incoming || incoming._capacity())) socket.resume();
      }
    }
    function send() {
      if (sent || stopped || detached) return;
      sent = true;
      socket.write(body.length ? Buffer.concat([headerBytes, body]) : headerBytes, reason => {
        if (reason) error(reason); else if (!stopped) events.finish();
      });
    }
    function attach(connected) {
      if (socket || stopped) { if (stopped) connected.destroy(); return; }
      if (!connected || typeof connected.on !== 'function' ||
          typeof connected.write !== 'function' || typeof connected.destroy !== 'function')
        throw fail('ERR_INVALID_ARG_TYPE', 'createConnection must provide a socket');
      socket = connected;
      socket.on('data', onData).on('end', onEnd).on('error', onError).on('close', onClose).on('timeout', onTimeout);
      socket.setTimeout(timeout);
      events.socket(socket);
      if (stopped) return;
      if (socket.connecting) socket.once('connect', send); else send();
    }
    queueMicrotask(() => {
      if (stopped) return;
      const connectOptions = {...settings, host: url.hostname.replace(/^\[|\]$/g, ''),
        port: Number(url.port || 80), path: settings.socketPath};
      // HTTP's handshake timeout is installed on the assigned socket below.
      delete connectOptions.timeout;
      // A createConnection function is part of Node's public HTTP API (and is
      // used by ordinary proxy/socket libraries). It must receive caller options.
      try {
        if (settings.createConnection) {
          const result = settings.createConnection(connectOptions, (reason, connected) => {
            if (reason) error(reason); else if (connected) attach(connected);
          });
          if (result) attach(result);
        } else attach(net.connect(connectOptions));
      } catch (reason) { error(reason); }
    });
    return {
      get detached() { return detached; },
      destroy() {
        if (stopped || detached) return;
        stopped = true; removeListeners(); data = Buffer.alloc(0);
        if (incoming && !incoming.complete) incoming._abort();
        socket?.destroy();
      },
      setTimeout(delay) { timeout = delay; if (socket && !detached && !stopped) socket.setTimeout(delay); },
    };
  }
