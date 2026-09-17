  function createMainNetwork(nativeRequest, nativeAbort) {
    const serverModule = createHttpServerModule(netModule);
    function error(code, message, name = 'Error') {
      const result = new Error(message); result.code = code; result.name = name; return result;
    }
    const unsupported = method => { throw error('ERR_NOT_SUPPORTED', method + ' is not supported'); };
    function networkError(value) {
      if (value && value.code) return value;
      const message = String(value && value.message || value);
      const code = /(?:ABORT_ERR|ERR_[A-Z_]+|EINVAL)/.exec(message);
      return error(code ? code[0] : 'ERR_NETWORK', message);
    }
    function binary(value) {
      if (value == null) return null;
      if (typeof value === 'string') return Buffer.from(value);
      if (ArrayBuffer.isView(value)) return Buffer.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      if (value instanceof ArrayBuffer) return Buffer.from(new Uint8Array(value));
      if (value instanceof URLSearchParams) return Buffer.from(value.toString());
      return unsupported('Streaming, Blob and FormData request bodies');
    }
    class Headers {
      constructor(init) {
        this._values = new Map();
        if (init == null) return;
        if (typeof init[Symbol.iterator] === 'function') {
          for (const pair of init) {
            if (!pair || pair.length !== 2) throw new TypeError('Invalid header entry');
            this.append(pair[0], pair[1]);
          }
        } else {
          for (const [key, value] of Object.entries(init)) this.append(key, value);
        }
      }
      _name(name) {
        name = String(name).toLowerCase();
        if (!/^[!#$%&'*+.^_\x60|~0-9a-z-]+$/.test(name)) throw new TypeError('Invalid header name');
        return name;
      }
      _value(value) {
        value = String(value).trim();
        if (/[\r\n\0]/.test(value)) throw new TypeError('Invalid header value');
        return value;
      }
      append(name, value) {
        name = this._name(name); value = this._value(value);
        this._values.set(name, this._values.has(name) ? this._values.get(name) + ', ' + value : value);
      }
      set(name, value) { this._values.set(this._name(name), this._value(value)); }
      get(name) { return this._values.get(this._name(name)) ?? null; }
      has(name) { return this._values.has(this._name(name)); }
      delete(name) { this._values.delete(this._name(name)); }
      *entries() { yield* [...this._values].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0); }
      *keys() { for (const [key] of this.entries()) yield key; }
      *values() { for (const [, value] of this.entries()) yield value; }
      forEach(callback, self) { for (const [key, value] of this.entries()) callback.call(self, value, key, this); }
      [Symbol.iterator]() { return this.entries(); }
      get [Symbol.toStringTag]() { return 'Headers'; }
    }
    class AbortSignal extends EventEmitter {
      constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
      addEventListener(name, callback, options) {
        if (options && options.once) this.once(name, callback); else this.on(name, callback);
      }
      removeEventListener(name, callback) { this.removeListener(name, callback); }
      throwIfAborted() { if (this.aborted) throw this.reason; }
      static abort(reason) { const controller = new AbortController(); controller.abort(reason); return controller.signal; }
      static timeout(delay) {
        if (!Number.isInteger(delay) || delay < 0) throw new RangeError('Invalid timeout');
        const controller = new AbortController();
        setTimeout(() => controller.abort(error('ETIMEDOUT', 'The operation timed out', 'TimeoutError')), delay);
        return controller.signal;
      }
      get [Symbol.toStringTag]() { return 'AbortSignal'; }
    }
    class AbortController {
      constructor() { this.signal = new AbortSignal(); }
      abort(reason = error('ABORT_ERR', 'The operation was aborted', 'AbortError')) {
        const signal = this.signal;
        if (signal.aborted) return;
        signal.aborted = true; signal.reason = reason;
        const event = {type: 'abort', target: signal};
        signal.emit('abort', event);
        if (typeof signal.onabort === 'function') signal.onabort.call(signal, event);
      }
      get [Symbol.toStringTag]() { return 'AbortController'; }
    }
    class Body {
      _setBody(value) {
        this._bytes = binary(value);
        if (this._bytes && this._bytes.length > 32 * 1024 * 1024) throw error('ERR_BUFFER_TOO_LARGE', 'HTTP upload exceeds 32 MiB');
        this.bodyUsed = false;
      }
      _consume() {
        if (this.bodyUsed) throw new TypeError('Body has already been consumed');
        this.bodyUsed = true;
        return this._bytes || Buffer.alloc(0);
      }
      async text() {
        // Fetch uses the WHATWG UTF-8 decoder, including BOM removal and
        // replacement of malformed sequences. Buffer.toString keeps the BOM.
        const bytes = this._consume();
        const text = [];
        let i = bytes.length >= 3 && bytes[0] === 239 && bytes[1] === 187 && bytes[2] === 191 ? 3 : 0;
        while (i < bytes.length) {
          const first = bytes[i++];
          if (first < 128) { text.push(String.fromCharCode(first)); continue; }
          let needed = first >= 194 && first <= 223 ? 1 : first >= 224 && first <= 239 ? 2 : first >= 240 && first <= 244 ? 3 : 0;
          if (!needed) { text.push('\uFFFD'); continue; }
          let code = first & (needed === 1 ? 31 : needed === 2 ? 15 : 7);
          let valid = true;
          for (let part = 0; part < needed; ++part) {
            const next = bytes[i];
            const low = part === 0 && first === 224 ? 160 : part === 0 && first === 240 ? 144 : 128;
            const high = part === 0 && first === 237 ? 159 : part === 0 && first === 244 ? 143 : 191;
            if (next === undefined || next < low || next > high) { valid = false; break; }
            ++i; code = (code << 6) | (next & 63);
          }
          text.push(valid ? String.fromCodePoint(code) : '\uFFFD');
        }
        return text.join('');
      }
      async json() { return JSON.parse(await this.text()); }
      async arrayBuffer() {
        const value = this._consume();
        return value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength);
      }
      get body() { return unsupported('Streaming response bodies'); }
    }
    class Request extends Body {
      constructor(input, init = {}) {
        super();
        const source = input instanceof Request ? input : null;
        if (source && source.bodyUsed) throw new TypeError('Request body has already been consumed');
        this.url = String(source ? source.url : input);
        const parsed = new URL(this.url);
        if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') throw new TypeError('fetch requires an HTTP(S) URL');
        this.url = parsed.href;
        this.method = String(init.method || (source && source.method) || 'GET').toUpperCase();
        if (!/^[!#$%&'*+.^_\x60|~0-9a-z-]+$/i.test(this.method)) throw new TypeError('Invalid HTTP method');
        this.headers = new Headers(init.headers === undefined && source ? source.headers : init.headers);
        const body = init.body === undefined && source ? source._bytes : init.body;
        if (body != null && (this.method === 'GET' || this.method === 'HEAD')) throw new TypeError('GET and HEAD requests cannot have a body');
        this._setBody(body);
        if (!this.headers.has('content-type') && typeof body === 'string') this.headers.set('content-type', 'text/plain;charset=UTF-8');
        if (!this.headers.has('content-type') && body instanceof URLSearchParams) this.headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');
        this.signal = init.signal === undefined && source ? source.signal : init.signal;
        if (this.signal != null && typeof this.signal.addEventListener !== 'function') throw new TypeError('Invalid AbortSignal');
        this.redirect = init.redirect || (source && source.redirect) || 'follow';
        if (!['follow', 'error'].includes(this.redirect)) unsupported('fetch redirect mode ' + this.redirect);
        this._credentials = init.credentials || (source && source.credentials) || 'same-origin';
        if (!['omit', 'include', 'same-origin'].includes(this._credentials)) throw new TypeError('Invalid credentials mode');
        if (init.integrity) unsupported('fetch integrity');
      }
      clone() { return new Request(this); }
      get credentials() { return this._credentials; }
      get [Symbol.toStringTag]() { return 'Request'; }
    }
    class Response extends Body {
      constructor(body = null, init = {}) {
        super();
        this.status = init.status === undefined ? 200 : Number(init.status);
        if (!Number.isInteger(this.status) || this.status < 200 || this.status > 599) throw new RangeError('Invalid response status');
        this.statusText = String(init.statusText || '');
        this.headers = new Headers(init.headers);
        this.url = init.url || '';
        this.redirected = Boolean(init.redirected);
        this.type = 'basic';
        this._setBody(body);
      }
      get ok() { return this.status >= 200 && this.status < 300; }
      clone() {
        if (this.bodyUsed) throw new TypeError('Response body has already been consumed');
        return new Response(this._bytes, this);
      }
      get [Symbol.toStringTag]() { return 'Response'; }
    }
    function fetch(input, init) {
      let request;
      try { request = new Request(input, init); }
      catch (failure) { return Promise.reject(failure); }
      const signal = request.signal;
      if (signal && signal.aborted) return Promise.reject(signal.reason || error('ABORT_ERR', 'The operation was aborted', 'AbortError'));
      if (input instanceof Request) input.bodyUsed = true;
      return new Promise((resolve, reject) => {
        let operation;
        let finished = false;
        const cleanup = () => { if (signal) signal.removeEventListener('abort', abort); };
        const fail = reason => { if (finished) return; finished = true; cleanup(); reject(reason); };
        const abort = () => {
          fail(signal.reason || error('ABORT_ERR', 'The operation was aborted', 'AbortError'));
          if (operation) nativeAbort(operation.id);
        };
        if (signal) signal.addEventListener('abort', abort, {once: true});
        try {
          operation = nativeRequest({url: request.url, method: request.method,
            headers: Object.fromEntries(request.headers),
            bodyBase64: request._bytes ? request._bytes.toString('base64') : '',
            useSessionCookies: request.credentials === 'include', redirect: request.redirect,
            timeoutMs: 300000});
          Promise.resolve(operation.promise).then(value => {
            if (finished) return;
            let response;
            try {
              response = new Response(Buffer.from(value.bodyBase64 || '', 'base64'), {
                status: value.statusCode, statusText: value.statusMessage,
                headers: value.headers, url: value.finalUrl || request.url,
                redirected: !!value.finalUrl && value.finalUrl !== request.url});
            } catch (failure) { fail(failure); return; }
            finished = true; cleanup(); resolve(response);
          }, failure => fail(networkError(failure)));
        } catch (failure) { fail(networkError(failure)); }
      });
    }
    class IncomingMessage extends serverModule.IncomingMessage {
      constructor(response) {
        super();
        this.statusCode = response.statusCode;
        this.statusMessage = response.statusMessage || '';
        this.headers = Object.fromEntries(new Headers(response.headers));
        // Chromium has already decoded compressed response bodies. Expose
        // headers matching the delivered bytes so Node consumers do not unzip
        // them twice. Fetch retains the original response headers separately.
        if (this.headers['content-encoding']) {
          delete this.headers['content-encoding'];
          delete this.headers['content-length'];
        }
        this.rawHeaders = Object.entries(this.headers).flat();
        this.url = response.finalUrl || '';
        this.httpVersion = response.httpVersion || '';
        this.complete = false; this.readableEnded = false; this.destroyed = false;
        this._data = Buffer.from(response.bodyBase64 || '', 'base64');
        this._flowing = false; this._scheduled = false; this._delivered = false;
      }
      on(name, listener) {
        super.on(name, listener);
        if (name === 'data' && !this._paused) { this._flowing = true; this._pump(); }
        return this;
      }
      addListener(name, listener) { return this.on(name, listener); }
      once(name, listener) { return EventEmitter.prototype.once.call(this, name, listener); }
      setEncoding(encoding) { this._encoding = encoding || 'utf8'; return this; }
      pause() { this._paused = true; this._flowing = false; return this; }
      resume() { this._paused = false; this._flowing = true; this._pump(); return this; }
      _pump() {
        if (this._scheduled || this.destroyed || this.readableEnded) return;
        this._scheduled = true;
        queueMicrotask(() => {
          this._scheduled = false;
          if (!this._flowing || this.destroyed || this.readableEnded) return;
          if (!this._delivered) {
            this._delivered = true;
            if (this._data.length) this.emit('data', this._encoding ? this._data.toString(this._encoding) : this._data);
            this._data = Buffer.alloc(0);
            this._pump();
            return;
          }
          this.complete = true; this.readableEnded = true;
          this.emit('end'); this._close();
        });
      }
      pipe(destination, options = {}) {
        this.on('data', chunk => {
          if (destination.write(chunk) === false) {
            this.pause(); destination.once('drain', () => this.resume());
          }
        });
        if (options.end !== false) this.once('end', () => destination.end());
        destination.emit('pipe', this);
        return destination;
      }
      destroy(reason) {
        if (this.destroyed) return this;
        this.destroyed = true; this._data = Buffer.alloc(0);
        queueMicrotask(() => { if (reason) this.emit('error', reason); this._close(); });
        return this;
      }
      _close() { if (!this._closed) { this._closed = true; this.emit('close'); } }
    }
    function httpModuleFor(protocol) {
      class ClientRequest extends EventEmitter {
        constructor(input, options, callback) {
          super();
          if (typeof options === 'function') { callback = options; options = undefined; }
          let settings = {};
          let url;
          if (typeof input === 'string' || input instanceof URL) url = new URL(String(input));
          else settings = {...input};
          settings = {...settings, ...(options || {})};
          if (!url) {
            const host = settings.hostname || settings.host || 'localhost';
            const port = settings.port ? ':' + settings.port : '';
            url = new URL((settings.protocol || protocol) + '//' + host + port + (settings.path || '/'));
          } else if (settings.path) url = new URL(settings.path, url.href);
          if (url.protocol !== protocol) throw error('ERR_INVALID_PROTOCOL', 'Protocol ' + url.protocol + ' is not supported by this module');
          this._url = url.href;
          this.method = String(settings.method || 'GET').toUpperCase();
          this._headers = new Headers(settings.headers);
          if (settings.auth && !this._headers.has('authorization')) this._headers.set('authorization', 'Basic ' + Buffer.from(String(settings.auth)).toString('base64'));
          this._chunks = []; this._length = 0; this._sent = false;
          this.destroyed = false; this.aborted = false; this.writableEnded = false;
          this._closed = false; this._timeout = Number(settings.timeout) || 0;
          if (callback) this.once('response', callback);
          if (settings.signal) {
            this._signal = settings.signal;
            this._onAbort = () => this.destroy(settings.signal.reason || error('ABORT_ERR', 'The operation was aborted', 'AbortError'));
            if (settings.signal.aborted) queueMicrotask(this._onAbort);
            else settings.signal.addEventListener('abort', this._onAbort, {once: true});
          }
        }
        setHeader(name, value) { if (this._sent) throw error('ERR_HTTP_HEADERS_SENT', 'Headers already sent'); this._headers.set(name, value); return this; }
        getHeader(name) { return this._headers.get(name) ?? undefined; }
        getHeaders() { return Object.fromEntries(this._headers); }
        hasHeader(name) { return this._headers.has(name); }
        removeHeader(name) { if (this._sent) throw error('ERR_HTTP_HEADERS_SENT', 'Headers already sent'); this._headers.delete(name); }
        write(chunk, encoding, callback) {
          if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
          if (this.writableEnded || this.destroyed) throw error('ERR_STREAM_WRITE_AFTER_END', 'write after end');
          const data = typeof chunk === 'string' ? Buffer.from(chunk, encoding) : binary(chunk);
          if (!data) throw new TypeError('Invalid HTTP body chunk');
          if (this._length + data.length > 32 * 1024 * 1024) throw error('ERR_BUFFER_TOO_LARGE', 'HTTP upload exceeds 32 MiB');
          this._chunks.push(data); this._length += data.length;
          if (callback) queueMicrotask(callback);
          return true;
        }
        end(chunk, encoding, callback) {
          if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
          else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
          if (this.writableEnded || this.destroyed) return this;
          if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
          this.writableEnded = true; this._sent = true;
          if (callback) this.once('finish', callback);
          const request = {url: this._url, method: this.method, headers: this.getHeaders(),
            bodyBase64: Buffer.concat(this._chunks).toString('base64'), timeoutMs: 300000,
            redirect: 'error'};
          this._chunks.length = 0;
          try {
            this._operation = nativeRequest(request);
            this._armTimeout();
            queueMicrotask(() => { if (!this.destroyed) this.emit('finish'); });
            Promise.resolve(this._operation.promise).then(value => {
              if (this.destroyed) return;
              clearTimeout(this._timer);
              this._operation = null;
              const incoming = new IncomingMessage(value);
              this.res = incoming;
              incoming.once('end', () => this._close());
              this.emit('response', incoming);
              if (!incoming._data.length) incoming.resume();
            }, reason => { if (!this.destroyed) this.destroy(networkError(reason)); });
          } catch (reason) { this.destroy(networkError(reason)); }
          return this;
        }
        _armTimeout() {
          clearTimeout(this._timer);
          if (this._timeout > 0 && this._sent && !this.destroyed) this._timer = setTimeout(() => this.emit('timeout'), this._timeout);
        }
        setTimeout(delay, callback) {
          if (!Number.isFinite(delay) || delay < 0) throw new RangeError('Invalid timeout');
          this._timeout = delay;
          if (callback) this.once('timeout', callback);
          this._armTimeout(); return this;
        }
        _close() {
          if (this._closed) return;
          this._closed = true; clearTimeout(this._timer);
          if (this._signal) this._signal.removeEventListener('abort', this._onAbort);
          this.emit('close');
        }
        destroy(reason) {
          if (this.destroyed) return this;
          this.destroyed = true; this._chunks.length = 0; clearTimeout(this._timer);
          if (this._operation) nativeAbort(this._operation.id);
          if (this.res) this.res.destroy();
          queueMicrotask(() => { if (reason) this.emit('error', reason); this._close(); });
          return this;
        }
        abort() { if (this.aborted) return; this.aborted = true; this.emit('abort'); this.destroy(); }
        setNoDelay() { return unsupported('Per-request TCP no-delay settings'); }
        setSocketKeepAlive() { return unsupported('Per-request TCP keepalive settings'); }
        flushHeaders() { return unsupported('Streaming request headers'); }
      }
      class Agent {
        constructor(options = {}) {
          if (options.rejectUnauthorized === false || options.ca || options.cert || options.key || options.proxy) unsupported('Custom HTTP Agent TLS/proxy settings');
          this.options = {...options}; this.protocol = protocol;
        }
        destroy() {}
      }
      const result = {ClientRequest, IncomingMessage: serverModule.IncomingMessage,
        Agent, globalAgent: new Agent(),
        request: (input, options, callback) => new ClientRequest(input, options, callback),
        get(input, options, callback) { const request = new ClientRequest(input, options, callback); request.end(); return request; },
        createServer: () => unsupported('HTTP createServer')};
      if (protocol === 'http:') Object.assign(result, serverModule);
      return result;
    }
    return {http: httpModuleFor('http:'), https: httpModuleFor('https:'),
      globals: {fetch, Headers, Request, Response, AbortController, AbortSignal}};
  }

  const mainNetwork = createMainNetwork(
      request => __xenonHttpRequest(request), id => __xenonHttpAbort(id));
  const httpModule = mainNetwork.http;
  const httpsModule = mainNetwork.https;
  Object.assign(globalThis, mainNetwork.globals);
