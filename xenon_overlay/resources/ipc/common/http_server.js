  // HTTP/1.x server framing over the native net transport. Keep this shared
  // between main and renderer so their public module contracts stay identical.
  function createHttpServerModule(net) {
    const requestHighWaterMark = 64 * 1024;
    const token = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
    const reasons = {100: 'Continue', 200: 'OK', 201: 'Created', 204: 'No Content',
      301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified', 400: 'Bad Request',
      401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed',
      408: 'Request Timeout', 413: 'Payload Too Large', 417: 'Expectation Failed',
      431: 'Request Header Fields Too Large', 500: 'Internal Server Error',
      501: 'Not Implemented', 503: 'Service Unavailable'};
    function fail(code, message) { const error = new Error(message); error.code = code; return error; }
    function header(name, value) {
      if (!token.test(name)) throw fail('ERR_INVALID_HTTP_TOKEN', 'Invalid HTTP header name');
      if (/[\x00-\x08\x0a-\x1f\x7f-\uffff]/.test(String(value))) {
        throw fail('ERR_INVALID_CHAR', 'Invalid HTTP header value');
      }
    }
    function bytes(value, encoding) {
      if (typeof value === 'string') return Buffer.from(value, encoding);
      if (value instanceof Uint8Array) return Buffer.from(value);
      throw fail('ERR_INVALID_ARG_TYPE', 'HTTP body must be a string or Uint8Array');
    }
    function delimiter(data, start = 0) {
      for (let i = start; i + 1 < data.length; ++i) if (data[i] === 13 && data[i + 1] === 10) return i;
      return -1;
    }
    function decodedBytes(data, encoding) {
      if (encoding === 'ascii' || encoding === 'latin1') {
        // Main and renderer Buffer implementations need not provide the same
        // legacy text decoders. These encodings map each byte independently.
        return Array.from(data, byte => String.fromCharCode(
            byte & (encoding === 'ascii' ? 0x7f : 0xff))).join('');
      }
      return data.toString(encoding);
    }
    class IncomingMessage extends EventEmitter {
      constructor(socket, flowChanged = () => {}) {
        super(); this.socket = this.connection = socket;
        this.headers = Object.create(null); this.rawHeaders = [];
        this.trailers = Object.create(null); this.rawTrailers = [];
        this.complete = false; this.aborted = false; this.destroyed = false;
        this.readable = true; this.readableEnded = false;
        this._queue = []; this._flow = null; this._encoding = null;
        this.readableLength = 0; this.readableHighWaterMark = requestHighWaterMark;
        this._flowChanged = flowChanged; this._drainQueued = false;
        this._tail = Buffer.alloc(0); this._closed = false;
      }
      on(name, callback) {
        super.on(name, callback);
        if (name === 'data' && this._flow !== false) this.resume();
        return this;
      }
      addListener(name, callback) { return this.on(name, callback); }
      once(name, callback) {
        super.once(name, callback);
        if (name === 'data' && this._flow !== false) this.resume();
        return this;
      }
      setEncoding(encoding) {
        let name = String(encoding || 'utf8').toLowerCase();
        if (name === 'utf-8') name = 'utf8';
        if (name === 'binary') name = 'latin1';
        if (['utf16le', 'utf-16le', 'ucs2', 'ucs-2', 'base64', 'base64url'].includes(name)) {
          throw fail('ERR_NOT_SUPPORTED', `HTTP request stream decoding for ${name} is not supported`);
        }
        if (!['utf8', 'ascii', 'latin1', 'hex'].includes(name)) {
          throw fail('ERR_UNKNOWN_ENCODING', `Unknown encoding: ${encoding}`);
        }
        this._encoding = name; return this;
      }
      get readableFlowing() { return this._flow; }
      isPaused() { return this._flow === false; }
      pause() {
        this._flow = false; this._flowChanged(); return this;
      }
      resume() {
        this._flow = true;
        if (!this._drainQueued) {
          this._drainQueued = true;
          queueMicrotask(() => { this._drainQueued = false; this._drain(); });
        }
        return this;
      }
      _capacity() {
        return this._flow === false ? 0 : Math.max(0, this.readableHighWaterMark - this.readableLength);
      }
      _drain() {
        while (this._flow && this._queue.length && !this.destroyed) {
          let chunk = this._queue.shift();
          this.readableLength -= chunk.length;
          if (this._encoding) {
            chunk = Buffer.concat([this._tail, chunk]); this._tail = Buffer.alloc(0);
            if (/^utf-?8$/i.test(this._encoding)) {
              let lead = chunk.length - 1;
              while (lead >= 0 && (chunk[lead] & 0xc0) === 0x80) --lead;
              const first = chunk[lead];
              const size = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : first >= 0xc2 ? 2 : 1;
              if (lead >= 0 && chunk.length - lead < size) {
                this._tail = chunk.subarray(lead); chunk = chunk.subarray(0, lead);
              }
            }
            chunk = decodedBytes(chunk, this._encoding);
          }
          if (chunk.length) this.emit('data', chunk);
        }
        if (this._flow && this.complete && !this._queue.length && !this.readableEnded && !this.destroyed) {
          if (this._tail.length) { this.emit('data', decodedBytes(this._tail, this._encoding)); this._tail = Buffer.alloc(0); }
          this.readable = false; this.readableEnded = true;
          this.emit('end'); this._close();
        }
        this._flowChanged();
      }
      _push(chunk) {
        if (chunk.length) { this._queue.push(chunk); this.readableLength += chunk.length; }
        this._drain();
      }
      _finish() { this.complete = true; this._drain(); }
      _close() {
        if (this._closed) return;
        this._closed = true; this.destroyed = true; this.readable = false;
        this._queue.length = 0; this.readableLength = 0; this._tail = Buffer.alloc(0);
        this.emit('close');
      }
      _abort() {
        if (this._closed) return;
        this.destroyed = true;
        if ((!this.readableEnded || !this.complete) && !this.aborted) {
          this.aborted = true; this.emit('aborted');
        }
        this._close();
      }
      setTimeout(delay, callback) { this.socket.setTimeout(delay, callback); return this; }
      destroy(error) {
        if (this.destroyed) return this;
        this.destroyed = true; this._queue.length = 0; this.readableLength = 0;
        if (this.socket) this.socket.destroy(error);
        else queueMicrotask(() => { if (error) this.emit('error', error); this._abort(); });
        return this;
      }
      pipe(destination) {
        this.on('data', chunk => { if (destination.write(chunk) === false) this.pause(); });
        destination.on('drain', () => this.resume()); this.once('end', () => destination.end());
        return destination;
      }
    }
    class ServerResponse extends EventEmitter {
      constructor(request, done) {
        super(); this.req = request; this.socket = this.connection = request.socket;
        this.statusCode = 200; this.statusMessage = undefined; this.sendDate = true;
        this.headersSent = false; this.finished = this.writableEnded = this.writableFinished = false;
        this._headers = new Map(); this._done = done; this._chunked = false;
        this._close = request.httpVersion === '1.0' || /(?:^|,)\s*close\s*(?:,|$)/i.test(request.headers.connection || '');
      }
      setHeader(name, value) {
        if (this.headersSent) throw fail('ERR_HTTP_HEADERS_SENT', 'HTTP headers already sent');
        name = String(name); for (const item of Array.isArray(value) ? value : [value]) header(name, item);
        this._headers.set(name.toLowerCase(), {name, value}); return this;
      }
      getHeader(name) { return this._headers.get(String(name).toLowerCase())?.value; }
      getHeaderNames() { return [...this._headers.keys()]; }
      getHeaders() { return Object.assign(Object.create(null), Object.fromEntries([...this._headers].map(([k, v]) => [k, v.value]))); }
      hasHeader(name) { return this._headers.has(String(name).toLowerCase()); }
      removeHeader(name) {
        if (this.headersSent) throw fail('ERR_HTTP_HEADERS_SENT', 'HTTP headers already sent');
        this._headers.delete(String(name).toLowerCase());
      }
      writeHead(code, message, headers) {
        if (typeof message === 'object') { headers = message; message = undefined; }
        if (!Number.isInteger(code) || code < 100 || code > 999) throw fail('ERR_HTTP_INVALID_STATUS_CODE', 'Invalid HTTP status code');
        this.statusCode = code; this.statusMessage = message;
        if (Array.isArray(headers)) {
          for (let i = 0; i < headers.length; i += 2) this.setHeader(headers[i], headers[i + 1]);
        } else if (headers) for (const [name, value] of Object.entries(headers)) this.setHeader(name, value);
        this._sendHeaders(); return this;
      }
      _sendHeaders(length) {
        if (this.headersSent) return;
        if (!Number.isInteger(this.statusCode) || this.statusCode < 100 || this.statusCode > 999) throw fail('ERR_HTTP_INVALID_STATUS_CODE', 'Invalid HTTP status code');
        this._noBody = this.req.method === 'HEAD' || this.statusCode < 200 || this.statusCode === 204 || this.statusCode === 304;
        if (this.sendDate && !this.hasHeader('date')) this.setHeader('Date', new Date().toUTCString());
        if (this.hasHeader('connection')) this._close = /\bclose\b/i.test(String(this.getHeader('connection')));
        if (!this._noBody && !this.hasHeader('content-length') && !this.hasHeader('transfer-encoding')) {
          if (length !== undefined) this.setHeader('Content-Length', length);
          else if (this.req.httpVersion === '1.1') this.setHeader('Transfer-Encoding', 'chunked');
          else this._close = true;
        }
        this._chunked = !this._noBody && /\bchunked\b/i.test(String(this.getHeader('transfer-encoding') || ''));
        if (!this.hasHeader('connection')) this.setHeader('Connection', this._close ? 'close' : 'keep-alive');
        const message = this.statusMessage === undefined ? reasons[this.statusCode] || 'unknown' : String(this.statusMessage);
        header('status', message);
        let output = `HTTP/1.1 ${this.statusCode} ${message}\r\n`;
        for (const {name, value} of this._headers.values()) {
          for (const item of Array.isArray(value) ? value : [value]) output += `${name}: ${item}\r\n`;
        }
        this.headersSent = true; this.socket.write(output + '\r\n');
      }
      flushHeaders() { this._sendHeaders(); }
      writeContinue(callback) { this.socket.write('HTTP/1.1 100 Continue\r\n\r\n', callback); }
      write(chunk, encoding, callback) {
        if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
        if (this.writableEnded) {
          const error = fail('ERR_STREAM_WRITE_AFTER_END', 'write after end');
          queueMicrotask(() => { if (callback) callback(error); this.emit('error', error); }); return false;
        }
        const data = bytes(chunk, encoding); this._sendHeaders();
        if (!data.length || this._noBody) { if (callback) queueMicrotask(callback); return true; }
        return this.socket.write(this._chunked ? Buffer.concat([Buffer.from(data.length.toString(16) + '\r\n'), data, Buffer.from('\r\n')]) : data, callback);
      }
      end(chunk, encoding, callback) {
        if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
        else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
        if (this.writableEnded) return this;
        const data = chunk == null ? null : bytes(chunk, encoding);
        this._sendHeaders(data ? data.length : 0);
        if (data) this.write(data);
        this.finished = this.writableEnded = true;
        if (callback) this.once('finish', callback);
        const finish = error => {
          if (error) { this.emit('error', error); return; }
          this.writableFinished = true; this.emit('finish'); this._done(this._close); this.emit('close');
        };
        const last = this._chunked ? '0\r\n\r\n' : '';
        if (this._close) this.socket.end(last, finish); else this.socket.write(last, finish);
        return this;
      }
      setTimeout(delay, callback) { this.socket.setTimeout(delay, callback); return this; }
      destroy(error) { this.socket.destroy(error); return this; }
    }
    class Server extends EventEmitter {
      constructor(options, listener) {
        super();
        if (typeof options === 'function') { listener = options; options = {}; }
        options ||= {};
        for (const name of Object.keys(options)) {
          if (!['maxHeaderSize', 'requestTimeout', 'headersTimeout', 'keepAliveTimeout'].includes(name)) throw fail('ERR_NOT_SUPPORTED', `HTTP server option ${name} is not supported`);
          if (!Number.isInteger(options[name]) || options[name] < 0) throw fail('ERR_OUT_OF_RANGE', `HTTP server option ${name} must be a non-negative integer`);
        }
        this.maxHeaderSize = options.maxHeaderSize ?? 16384;
        this.requestTimeout = options.requestTimeout ?? 300000;
        this.headersTimeout = options.headersTimeout ?? Math.min(60000, this.requestTimeout || 60000);
        this.keepAliveTimeout = options.keepAliveTimeout ?? 5000;
        this.timeout = 0; this.maxHeadersCount = 2000; this._connections = new Map(); this._closing = false;
        this._server = net.createServer(socket => this._accept(socket));
        for (const name of ['listening', 'close', 'error']) this._server.on(name, (...args) => this.emit(name, ...args));
        if (listener) this.on('request', listener);
      }
      get listening() { return this._server.listening; }
      listen(...args) { this._closing = false; this._server.listen(...args); return this; }
      address() { return this._server.address(); }
      ref() { this._server.ref(); return this; }
      unref() { this._server.unref(); return this; }
      close(callback) { this._closing = true; this._server.close(callback); this.closeIdleConnections(); return this; }
      closeAllConnections() { for (const socket of this._connections.keys()) socket.destroy(); }
      closeIdleConnections() { for (const [socket, state] of this._connections) if (!state.req) socket.end(); }
      setTimeout(delay, callback) { this.timeout = delay; if (callback) this.on('timeout', callback); return this; }
      _accept(socket) {
        const state = {data: Buffer.alloc(0), req: null, res: null, mode: 'headers', left: 0,
          trailerBytes: 0, failed: false, timer: null, parsing: false, readQueued: false, readPaused: false};
        this._connections.set(socket, state);
        const syncSocketFlow = () => {
          if (socket.destroyed) return;
          const bodyBlocked = state.req && !state.req.complete && !state.req._capacity();
          // A response may be asynchronous while the peer pipelines its next
          // requests. Pause that raw queue as well as the current body queue.
          const paused = !state.failed && (bodyBlocked ||
              (state.mode === 'wait' && state.data.length >= requestHighWaterMark));
          if (paused === state.readPaused) return;
          state.readPaused = paused;
          if (paused) socket.pause(); else socket.resume();
        };
        const flowChanged = () => {
          // Parse already received bytes before requesting more from the OS.
          // User data listeners can pause/resume inside parse(), so defer the
          // continuation instead of recursively entering the HTTP parser.
          if (state.parsing || state.readQueued || state.failed || socket.destroyed) return;
          state.readQueued = true;
          queueMicrotask(() => { state.readQueued = false; parse(); });
          syncSocketFlow();
        };
        const arm = (delay, idle = false) => {
          clearTimeout(state.timer);
          state.timer = delay > 0 ? setTimeout(() => {
            if (idle) socket.end();
            else bad(fail('ERR_HTTP_REQUEST_TIMEOUT', 'HTTP request timed out'), 408);
          }, delay) : null;
        };
        const bad = (error, status = 400) => {
          if (state.failed) return; state.failed = true; clearTimeout(state.timer);
          syncSocketFlow();
          if (!this.emit('clientError', error, socket)) socket.end(`HTTP/1.1 ${status} ${reasons[status]}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
        };
        const next = () => {
          if (!state.req?.complete || !state.res?.writableFinished) return;
          state.req.resume(); state.req = state.res = null; state.mode = 'headers';
          if (this._closing) { clearTimeout(state.timer); socket.end(); return; }
          state.trailerBytes = 0; arm(this.keepAliveTimeout, true); parse();
        };
        const addHeader = (line, headers, raw) => {
          const index = line.indexOf(':');
          if (index <= 0 || !token.test(line.slice(0, index))) throw fail('HPE_INVALID_HEADER_TOKEN', 'Invalid HTTP request header');
          const name = line.slice(0, index), key = name.toLowerCase(), rawValue = line.slice(index + 1);
          // HTTP optional whitespace is only SP/HTAB. String.trim() also
          // removes prohibited controls and would hide malformed framing.
          header(name, rawValue);
          const value = rawValue.replace(/^[ \t]+|[ \t]+$/g, '');
          raw.push(name, value);
          if ((key === 'content-length' || key === 'transfer-encoding' || key === 'host') && headers[key] !== undefined) throw fail('HPE_UNEXPECTED_CONTENT_LENGTH', 'Duplicate framing header');
          headers[key] = headers[key] === undefined ? value : headers[key] + (key === 'cookie' ? '; ' : ', ') + value;
        };
        const complete = () => { clearTimeout(state.timer); state.mode = 'wait'; state.req._finish(); next(); };
        const parseAvailable = () => {
          while (!state.failed && !socket.destroyed) {
            if (state.mode === 'wait') return;
            if (state.mode === 'headers') {
              let boundary = -1;
              for (let i = 0; i + 3 < state.data.length; ++i) if (state.data[i] === 13 && state.data[i + 1] === 10 && state.data[i + 2] === 13 && state.data[i + 3] === 10) { boundary = i; break; }
              if (boundary < 0) { if (state.data.length > this.maxHeaderSize) bad(fail('HPE_HEADER_OVERFLOW', 'HTTP headers too large'), 431); return; }
              if (boundary + 4 > this.maxHeaderSize) { bad(fail('HPE_HEADER_OVERFLOW', 'HTTP headers too large'), 431); return; }
              const lines = state.data.subarray(0, boundary).toString('latin1').split('\r\n');
              state.data = state.data.subarray(boundary + 4);
              const request = new IncomingMessage(socket, flowChanged);
              try {
                const start = /^(\S+) ([^\x00-\x20]+) HTTP\/(1\.[01])$/.exec(lines.shift());
                if (!start || !token.test(start[1])) throw fail('HPE_INVALID_CONSTANT', 'Invalid HTTP request line');
                request.method = start[1]; request.url = start[2]; request.httpVersion = start[3];
                request.httpVersionMajor = 1; request.httpVersionMinor = Number(start[3][2]);
                if (this.maxHeadersCount && lines.length > this.maxHeadersCount) throw fail('HPE_HEADER_OVERFLOW', 'Too many HTTP headers');
                for (const line of lines) addHeader(line, request.headers, request.rawHeaders);
                const length = request.headers['content-length'], transfer = request.headers['transfer-encoding'];
                if (transfer !== undefined && (length !== undefined || transfer.toLowerCase() !== 'chunked')) throw fail('HPE_UNEXPECTED_CONTENT_LENGTH', 'Invalid HTTP message framing');
                if (length !== undefined && (!/^\d+$/.test(length) || !Number.isSafeInteger(Number(length)))) throw fail('HPE_INVALID_CONTENT_LENGTH', 'Invalid HTTP content length');
                state.mode = transfer ? 'chunk-size' : 'body'; state.left = Number(length || 0);
              } catch (error) { bad(error, error.code === 'HPE_HEADER_OVERFLOW' ? 431 : 400); return; }
              state.req = request;
              state.res = new ServerResponse(request, close => {
                request.resume();
                if (!close) next();
              });
              arm(this.requestTimeout);
              const expect = request.headers.expect;
              if (expect && expect.toLowerCase() === '100-continue') {
                if (!this.emit('checkContinue', request, state.res)) { state.res.writeContinue(); this.emit('request', request, state.res); }
              } else if (expect) {
                if (!this.emit('checkExpectation', request, state.res)) { state.res.setHeader('Connection', 'close'); state.res.writeHead(417); state.res.end(); }
              } else this.emit('request', request, state.res);
              continue;
            }
            if (state.mode === 'body' || state.mode === 'chunk-data') {
              if (state.left) {
                if (!state.data.length) return;
                const capacity = state.req._capacity();
                if (!capacity) return;
                const count = Math.min(state.left, state.data.length, capacity), data = state.data.subarray(0, count);
                state.data = state.data.subarray(count); state.left -= count; state.req._push(data);
                if (state.left) continue;
              }
              if (state.mode === 'body') complete(); else state.mode = 'chunk-crlf';
              continue;
            }
            if (state.mode === 'chunk-crlf') {
              if (state.data.length < 2) return;
              if (state.data[0] !== 13 || state.data[1] !== 10) { bad(fail('HPE_INVALID_CHUNK_SIZE', 'Invalid HTTP chunk ending')); return; }
              state.data = state.data.subarray(2); state.mode = 'chunk-size'; continue;
            }
            const lineEnd = delimiter(state.data);
            if (lineEnd < 0) { if (state.data.length > this.maxHeaderSize) bad(fail('HPE_HEADER_OVERFLOW', 'HTTP chunk header too large'), 431); return; }
            if (lineEnd + 2 > this.maxHeaderSize) { bad(fail('HPE_HEADER_OVERFLOW', 'HTTP chunk header too large'), 431); return; }
            const line = state.data.subarray(0, lineEnd).toString('latin1'); state.data = state.data.subarray(lineEnd + 2);
            if (state.mode === 'chunk-size') {
              const size = line.split(';', 1)[0];
              if (!/^[0-9a-f]+$/i.test(size) || !Number.isSafeInteger(parseInt(size, 16))) { bad(fail('HPE_INVALID_CHUNK_SIZE', 'Invalid HTTP chunk size')); return; }
              state.left = parseInt(size, 16); state.mode = state.left ? 'chunk-data' : 'trailers';
            } else if (!line) complete();
            else {
              try {
                state.trailerBytes += lineEnd + 2;
                if (state.trailerBytes > this.maxHeaderSize ||
                    (this.maxHeadersCount && state.req.rawTrailers.length / 2 >= this.maxHeadersCount)) throw fail('HPE_HEADER_OVERFLOW', 'Too many HTTP trailers');
                addHeader(line, state.req.trailers, state.req.rawTrailers);
              } catch (error) { bad(error, error.code === 'HPE_HEADER_OVERFLOW' ? 431 : 400); return; }
            }
          }
        };
        const parse = () => {
          if (state.parsing || state.failed || socket.destroyed) return;
          state.parsing = true;
          try { parseAvailable(); }
          finally { state.parsing = false; syncSocketFlow(); }
        };
        arm(this.headersTimeout);
        socket.on('data', data => {
          if (state.failed) return;
          // Bound queued pipelined data while application handlers are pending.
          if (state.data.length + data.length > 32 * 1024 * 1024) { bad(fail('HPE_HEADER_OVERFLOW', 'HTTP pending data limit exceeded'), 413); return; }
          if (state.mode === 'headers' && !state.data.length) arm(this.headersTimeout);
          state.data = Buffer.concat([state.data, Buffer.from(data)]); parse();
        });
        socket.on('error', error => bad(error));
        socket.on('end', () => { if (state.req && !state.req.complete) bad(fail('HPE_INVALID_EOF_STATE', 'Incomplete HTTP request')); });
        socket.on('close', () => {
          clearTimeout(state.timer); this._connections.delete(socket);
          if (state.req) state.req._abort();
          if (state.res && !state.res.writableFinished) state.res.emit('close');
        });
        if (this.timeout) socket.setTimeout(this.timeout, () => { if (!this.emit('timeout', socket)) socket.destroy(); });
        this.emit('connection', socket);
      }
    }
    return {Server, IncomingMessage, ServerResponse, createServer: (options, listener) => new Server(options, listener)};
  }
