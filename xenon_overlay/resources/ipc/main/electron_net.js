  // Electron networking uses the selected Chromium Session. Node's HTTP
  // implementation has different cookie/proxy/partition semantics.
  function createElectronNet({getSession, isSession, isReady = () => true}) {
    const active = new Map();
    const maxBodyBytes = 32 * 1024 * 1024;
    const cacheModes = new Set(['default', 'no-store', 'reload', 'no-cache', 'force-cache', 'only-if-cached']);
    const fail = (code, message, name = 'Error') => Object.assign(new Error(message), {code, name});
    const unsupported = method => { throw fail('ERR_NOT_SUPPORTED', method + ' is not supported'); };
    const argumentError = message => Object.assign(new TypeError(message), {code: 'ERR_INVALID_ARG_TYPE'});
    const abortError = () => fail('ABORT_ERR', 'The operation was aborted', 'AbortError');
    const nativeError = value => value?.code ? value : fail(
        /\b(?:ERR_[A-Z_]+|ABORT_ERR)\b/.exec(String(value?.message || value))?.[0] || 'ERR_FAILED',
        String(value?.message || value));
    function ready() {
      if (!isReady()) throw fail('ERR_NOT_READY', 'electron.net is available after app is ready');
      if (typeof globalThis.__xenonElectronNetStart !== 'function' ||
          typeof globalThis.__xenonElectronNetControl !== 'function') unsupported('electron.net transport');
    }
    function control(id, command) {
      return id !== null && globalThis.__xenonElectronNetControl(id, command);
    }
    function bytes(value, encoding) {
      if (typeof value === 'string') return Buffer.from(value, encoding);
      if (ArrayBuffer.isView(value)) return Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      if (value instanceof ArrayBuffer) return Buffer.from(new Uint8Array(value));
      throw argumentError('Request data must be a string, Buffer or ArrayBuffer');
    }
    function headerName(name) {
      if (typeof name !== 'string' || !/^[!#$%&'*+.^_`|~0-9a-z-]+$/i.test(name))
        throw argumentError('Invalid HTTP header name');
      return name.toLowerCase();
    }
    function headerValue(value) {
      const values = Array.isArray(value) ? value : [value];
      if (!values.length) throw argumentError('HTTP header array must not be empty');
      const checked = values.map(item => {
        if (typeof item !== 'string' && typeof item !== 'number') throw argumentError('Invalid HTTP header value');
        const text = String(item);
        if (/[\r\n\0]/.test(text)) throw argumentError('Invalid HTTP header value');
        return text;
      });
      return Array.isArray(value) ? checked : checked[0];
    }
    function headersFrom(init) {
      const result = Object.create(null);
      if (init === undefined) return result;
      if (!init || typeof init !== 'object' || Array.isArray(init)) throw argumentError('headers must be an object');
      for (const [name, value] of Object.entries(init)) result[headerName(name)] = headerValue(value);
      return result;
    }
    function incomingHeaders(headers) {
      const result = Object.create(null);
      for (const [key, value] of Object.entries(headers || {})) {
        const name = key.toLowerCase();
        const values = Array.isArray(value) ? value.map(String) : [String(value)];
        result[name] = name === 'set-cookie' ? values : values.join(name === 'cookie' ? '; ' : ', ');
      }
      return result;
    }
    function sessionOf(options, forced) {
      if (forced) {
        if (!isSession(forced)) throw argumentError('Invalid Session');
        return forced;
      }
      if (options.session !== undefined) {
        if (!isSession(options.session)) throw argumentError('session must be an Electron Session');
        return options.session;
      }
      if (options.partition !== undefined && typeof options.partition !== 'string')
        throw argumentError('partition must be a string');
      return getSession(options.partition || '');
    }
    function requestOptions(input) {
      const options = typeof input === 'string' || input instanceof URL ? {url: String(input)} : input;
      if (!options || typeof options !== 'object' || Array.isArray(options)) throw argumentError('Invalid request options');
      let url;
      if (options.url !== undefined) url = new URL(String(options.url));
      else {
        const protocol = options.protocol || 'http:';
        const hostname = options.hostname;
        const host = hostname !== undefined ?
          (hostname.includes(':') && !hostname.startsWith('[') ? '[' + hostname + ']' : hostname) :
          (options.host || 'localhost');
        const pathname = options.path === undefined ? '/' : String(options.path);
        if (!pathname.startsWith('/')) throw argumentError('Request path must start with /');
        url = new URL(protocol + '//' + host + pathname);
        if (options.port !== undefined) {
          if (!Number.isInteger(Number(options.port)) || Number(options.port) < 0 || Number(options.port) > 65535)
            throw argumentError('Invalid request port');
          url.port = String(options.port);
        }
      }
      if (!['http:', 'https:'].includes(url.protocol)) unsupported('electron.net URL scheme ' + url.protocol);
      if (url.username || url.password) unsupported('Credentials embedded in request URLs');
      const method = String(options.method || 'GET').toUpperCase();
      if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/i.test(method)) throw argumentError('Invalid HTTP method');
      const redirect = options.redirect || 'follow';
      if (!['follow', 'manual', 'error'].includes(redirect)) throw argumentError('Invalid redirect mode');
      const cache = options.cache || 'default';
      if (!cacheModes.has(cache)) throw argumentError('Invalid cache mode');
      for (const name of ['useSessionCookies', 'bypassCustomProtocolHandlers'])
        if (options[name] !== undefined && typeof options[name] !== 'boolean') throw argumentError(name + ' must be boolean');
      if (options.credentials !== undefined && !['include', 'omit'].includes(options.credentials))
        unsupported('electron.net credentials ' + options.credentials);
      for (const name of ['origin', 'referrerPolicy', 'priority', 'priorityIncremental'])
        if (options[name] !== undefined) unsupported('electron.net option ' + name);
      // These Node-only controls cannot override a Chromium Session's network
      // policy. Ordinary Node defaults are accepted for Electron callers that
      // share their request options with Node's HTTP API.
      if (options.rejectUnauthorized === false || options.ca || options.cert || options.key || options.socketPath)
        unsupported('Per-request TLS or socket options');
      const headers = headersFrom(options.headers);
      if (options.auth !== undefined && !Object.hasOwn(headers, 'authorization'))
        headers.authorization = 'Basic ' + Buffer.from(String(options.auth)).toString('base64');
      return {url: url.href, method, headers, redirect, cache,
        partition: sessionOf(options).partition,
        ...(options.credentials === undefined ? {} : {credentials: options.credentials}),
        useSessionCookies: options.useSessionCookies === true,
        bypassCustomProtocolHandlers: options.bypassCustomProtocolHandlers === true};
    }

    class IncomingMessage extends streamModule.Readable {
      constructor(owner, event) {
        // Each native read credit delivers at most one bounded network chunk.
        super({highWaterMark: 1, autoDestroy: true});
        this._owner = owner;
        this.statusCode = event.statusCode;
        this.statusMessage = event.statusMessage || '';
        this.httpVersion = event.httpVersion || '';
        const [major, minor] = this.httpVersion.split('.').map(Number);
        this.httpVersionMajor = major || 0; this.httpVersionMinor = minor || 0;
        this.headers = incomingHeaders(event.headers);
        this.rawHeaders = (event.rawHeaders || []).slice();
        this.url = event.url || '';
        this.trailers = Object.create(null); this.rawTrailers = [];
        this.complete = false; this.aborted = false;
        this._credit = false;
        this.once('end', () => owner._close());
      }
      _read() { this._requestNext(true); }
      _requestNext(readRequested = false) {
        if (this._credit || this.complete || this.destroyed || (!readRequested && this.readableLength) ||
            (this.readableFlowing === false && !this.listenerCount('readable'))) return;
        this._credit = true;
        try { control(this._owner._id, 'resume'); }
        catch (error) { this._owner.destroy(nativeError(error)); }
      }
      resume() {
        super.resume();
        this._requestNext();
        return this;
      }
      _data(value) {
        if (this.destroyed || this.complete) return;
        this._credit = false;
        this.push(Buffer.from(value || '', 'base64'));
      }
      _end() {
        if (this.destroyed || this.complete) return;
        this._credit = false; this.complete = true;
        this.push(null);
      }
      _fail(error) { this.destroy(error); }
      _destroy(error, callback) {
        this._credit = false;
        if (!this.complete && !this.aborted) {
          this.aborted = true;
          this.emit('aborted');
        }
        if (!this._owner.destroyed) this._owner.destroy(error);
        callback(error);
      }
    }

    class ClientRequest extends streamModule.Writable {
      constructor(input, callback) {
        super({autoDestroy: false}); ready();
        if (callback !== undefined && typeof callback !== 'function') throw argumentError('response callback must be a function');
        this._options = requestOptions(input);
        this._id = null; this._chunks = []; this._length = 0;
        this._started = false; this._redirectPending = false;
        this._authChallenges = new Map();
        this.aborted = false;
        if (callback) this.once('response', callback);
      }
      on(name, listener) {
        if (name === 'login' && typeof globalThis.__xenonElectronNetAuth !== 'function')
          unsupported('ClientRequest authentication transport');
        return super.on(name, listener);
      }
      addListener(name, listener) { return this.on(name, listener); }
      once(name, listener) {
        if (name === 'login' && typeof globalThis.__xenonElectronNetAuth !== 'function')
          unsupported('ClientRequest authentication transport');
        return super.once(name, listener);
      }
      get chunkedEncoding() { return false; }
      set chunkedEncoding(value) { if (value) unsupported('Streaming request uploads'); }
      getHeader(name) {
        const value = this._options.headers[headerName(name)];
        return Array.isArray(value) ? value.slice() : value;
      }
      getHeaders() { return Object.fromEntries(Object.keys(this._options.headers).map(name => [name, this.getHeader(name)])); }
      getHeaderNames() { return Object.keys(this._options.headers); }
      hasHeader(name) { return Object.hasOwn(this._options.headers, headerName(name)); }
      setHeader(name, value) {
        if (this._started) throw fail('ERR_HTTP_HEADERS_SENT', 'Request headers have already been sent');
        this._options.headers[headerName(name)] = headerValue(value); return this;
      }
      removeHeader(name) {
        if (this._started) throw fail('ERR_HTTP_HEADERS_SENT', 'Request headers have already been sent');
        delete this._options.headers[headerName(name)];
      }
      write(chunk, encoding, callback) {
        if (this._length + (typeof chunk === 'string' ? Buffer.byteLength(chunk,
            typeof encoding === 'string' ? encoding : undefined) : chunk?.byteLength || 0) > maxBodyBytes)
          throw fail('ERR_BUFFER_TOO_LARGE', 'Electron request upload exceeds 32 MiB');
        return super.write(chunk, encoding, callback);
      }
      _write(chunk, encoding, callback) {
        try {
          const data = bytes(chunk, encoding);
          if (this._length + data.length > maxBodyBytes)
            throw fail('ERR_BUFFER_TOO_LARGE', 'Electron request upload exceeds 32 MiB');
          this._chunks.push(data); this._length += data.length;
        } catch (error) { callback(error); return; }
        callback();
      }
      _final(callback) {
        this._started = true;
        const bodyBase64 = Buffer.concat(this._chunks, this._length).toString('base64');
        this._chunks.length = 0; this._length = 0;
        try {
          this._id = globalThis.__xenonElectronNetStart({...this._options, bodyBase64});
          active.set(this._id, this);
        } catch (error) { callback(nativeError(error)); return; }
        callback();
      }
      _event(event) {
        if (this.destroyed) return;
        if (event.type === 'response') {
          this.res = new IncomingMessage(this, event);
          this.emit('response', this.res);
        } else if (event.type === 'login') {
          if (typeof globalThis.__xenonElectronNetAuth !== 'function') {
            this.destroy(fail('ERR_NOT_SUPPORTED', 'ClientRequest authentication transport is unavailable')); return;
          }
          const challengeId = event.challengeId;
          if (this._authChallenges.has(challengeId)) return;
          const challenge = {};
          this._authChallenges.set(challengeId, challenge);
          const reply = (username, password) => {
            if (this.destroyed || this._id === null || this._authChallenges.get(challengeId) !== challenge) return;
            this._authChallenges.delete(challengeId);
            if ((username !== undefined || password !== undefined) &&
                (typeof username !== 'string' || typeof password !== 'string')) {
              globalThis.__xenonElectronNetAuth(this._id, challengeId);
              throw argumentError('Authentication credentials must be strings');
            }
            if (username === undefined && password === undefined)
              globalThis.__xenonElectronNetAuth(this._id, challengeId);
            else
              globalThis.__xenonElectronNetAuth(this._id, challengeId, username, password);
          };
          try { if (!this.emit('login', {...event.authInfo}, reply)) reply(); }
          catch (error) { reply(); throw error; }
        } else if (event.type === 'data') this.res?._data(event.bodyBase64);
        else if (event.type === 'end') { active.delete(this._id); this._id = null; this.res?._end(); }
        else if (event.type === 'error') this.destroy(fail(event.code || 'ERR_FAILED', event.message || 'Network request failed'));
        else if (event.type === 'redirect') {
          if (this._options.redirect === 'error') { this.destroy(fail('ERR_FAILED', 'Redirect was disallowed')); return; }
          this._redirectPending = true;
          this._followRequested = false;
          try { this.emit('redirect', event.statusCode, event.method, event.url, incomingHeaders(event.headers)); }
          finally {
            this._redirectPending = false;
            if (!this.destroyed) {
              if (this._options.redirect === 'follow' || this._followRequested) control(this._id, 'follow-redirect');
              else this.destroy(fail('ERR_FAILED', 'Redirect was cancelled'));
            }
          }
        }
      }
      followRedirect() {
        if (!this._redirectPending || this.destroyed) throw fail('ERR_INVALID_STATE', 'No redirect is pending');
        this._followRequested = true;
      }
      _close() { this.destroy(); }
      _destroy(error, callback) {
        this._chunks.length = 0; this._length = 0;
        this._authChallenges.clear();
        if (this._id !== null) { active.delete(this._id); control(this._id, 'abort'); this._id = null; }
        if (this.res && !this.res.destroyed) {
          queueMicrotask(() => { this.res.destroy(error); callback(error); });
        } else callback(error);
      }
      abort() {
        if (this.aborted || this.closed) return;
        this.aborted = true;
        queueMicrotask(() => this.emit('abort'));
        this.destroy();
      }
      getUploadProgress() { return unsupported('ClientRequest.getUploadProgress'); }
      setTimeout() { return unsupported('ClientRequest.setTimeout'); }
      flushHeaders() { return unsupported('Streaming request headers'); }
    }

    async function fetchSession(sess, input, init = {}) {
      ready();
      if (!isSession(sess)) throw argumentError('Invalid Session');
      if (!init || typeof init !== 'object' || Array.isArray(init)) throw argumentError('Invalid fetch options');
      if (init.redirect === 'manual') unsupported('electron.net.fetch manual redirects');
      let body = init.body;
      let headers = init.headers;
      if (body instanceof globalThis.Blob) {
        headers = new globalThis.Headers(headers === undefined && input instanceof globalThis.Request ? input.headers : headers);
        if (body.type && !headers.has('content-type')) headers.set('content-type', body.type);
        body = new Uint8Array(await body.arrayBuffer());
      }
      const request = new globalThis.Request(input, {...init, ...(headers === undefined ? {} : {headers}),
        ...(body === undefined ? {} : {body})});
      const signal = request.signal;
      if (signal?.aborted) throw signal.reason || abortError();
      if (input instanceof globalThis.Request) input.bodyUsed = true;
      const options = {url: request.url, method: request.method,
        headers: Object.fromEntries(request.headers), session: sess,
        credentials: request.credentials === 'include' ? 'include' : 'omit',
        redirect: request.redirect, cache: init.cache || 'default',
        bypassCustomProtocolHandlers: init.bypassCustomProtocolHandlers};
      return new Promise((resolve, reject) => {
        const operation = new ClientRequest(options);
        let response, responseError, consuming = false, settled = false;
        const abort = () => {
          responseError = signal.reason || abortError();
          operation.destroy(responseError);
        };
        const cleanup = () => signal?.removeEventListener('abort', abort);
        operation.on('error', error => { responseError ||= error; if (!settled) { settled = true; reject(error); } });
        operation.once('close', cleanup);
        operation.once('response', incoming => {
          // Keep errors observable even before a caller consumes the body.
          incoming.on('error', error => { responseError ||= error; });
          try {
            response = new globalThis.Response(null, {status: incoming.statusCode,
              statusText: incoming.statusMessage, headers: incoming.headers,
              url: incoming.url || request.url, redirected: incoming.url !== request.url});
            const consume = async () => {
              if (consuming) throw new TypeError('Body has already been consumed');
              consuming = true; response.bodyUsed = true;
              if (responseError) throw responseError;
              const chunks = []; let length = 0;
              for await (const chunk of incoming) {
                length += chunk.length;
                if (length > maxBodyBytes) throw fail('ERR_BUFFER_TOO_LARGE', 'Buffered Electron fetch body exceeds 32 MiB; use net.request for streaming');
                chunks.push(chunk);
              }
              if (responseError) throw responseError;
              return Buffer.concat(chunks, length);
            };
            Object.defineProperties(response, {
              body: {get: () => unsupported('Web Streams response bodies')},
              clone: {value: () => unsupported('Cloning a streaming Electron fetch response')},
              arrayBuffer: {value: async () => { const data = await consume(); return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength); }},
              bytes: {value: async () => new Uint8Array(await consume())},
              text: {value: async () => new globalThis.Blob([await consume()]).text()},
              json: {value: async () => JSON.parse(await response.text())},
              blob: {value: async () => new globalThis.Blob([await consume()], {type: response.headers.get('content-type') || ''})},
            });
            settled = true; resolve(response);
          } catch (error) {
            if (!settled) { settled = true; reject(error); }
            operation.destroy(error);
          }
        });
        if (signal) signal.addEventListener('abort', abort, {once: true});
        if (signal?.aborted) { abort(); return; }
        operation.end(request._bytes || undefined);
      });
    }
    globalThis.__xenonDispatchElectronNetEvent = event => {
      if (!event || !Number.isSafeInteger(event.id)) return;
      active.get(event.id)?._event(event);
    };
    globalThis.__xenonShutdownElectronNet = () => {
      for (const operation of [...active.values()]) operation.destroy(abortError());
      active.clear();
    };
    const net = {
      request: (options, callback) => new ClientRequest(options, callback),
      fetch: (input, init) => fetchSession(getSession(''), input, init),
      isOnline() {
        if (typeof globalThis.__xenonElectronNetOnline !== 'function') unsupported('net.isOnline');
        return globalThis.__xenonElectronNetOnline();
      },
      resolveHost: async () => unsupported('net.resolveHost'),
    };
    Object.defineProperty(net, 'online', {enumerable: true, get: () => net.isOnline()});
    return {net, fetchSession};
  }
