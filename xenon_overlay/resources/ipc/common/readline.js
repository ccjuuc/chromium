  const readlineModule = (() => {
    const unsupported = operation => {
      const error = new Error(`readline ${operation} is not supported by this runtime`);
      error.code = 'ERR_NOT_SUPPORTED';
      throw error;
    };
    const invalid = name => {
      const error = new TypeError(`Invalid readline ${name}`);
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    };
    // The main isolate's TextDecoder fallback is not incremental, so keep the
    // UTF-8 decoder state here instead of losing split multibyte characters.
    function decodeChunk(state, chunk) {
      const bytes = ArrayBuffer.isView(chunk) ?
          new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength) :
          new Uint8Array(chunk);
      let result = '';
      for (let i = 0; i < bytes.length; ++i) {
        const byte = bytes[i];
        if (!state.needed) {
          if (byte <= 0x7f) { result += String.fromCharCode(byte); continue; }
          state.seen = 0;
          state.lower = 0x80;
          state.upper = 0xbf;
          if (byte >= 0xc2 && byte <= 0xdf) {
            state.needed = 1; state.point = byte & 0x1f;
          } else if (byte >= 0xe0 && byte <= 0xef) {
            state.needed = 2; state.point = byte & 0x0f;
            if (byte === 0xe0) state.lower = 0xa0;
            if (byte === 0xed) state.upper = 0x9f;
          } else if (byte >= 0xf0 && byte <= 0xf4) {
            state.needed = 3; state.point = byte & 7;
            if (byte === 0xf0) state.lower = 0x90;
            if (byte === 0xf4) state.upper = 0x8f;
          } else {
            result += '\ufffd';
          }
        } else if (byte < state.lower || byte > state.upper) {
          state.needed = 0;
          result += '\ufffd';
          --i;
        } else {
          state.lower = 0x80;
          state.upper = 0xbf;
          state.point = (state.point << 6) | (byte & 0x3f);
          if (++state.seen === state.needed) {
            result += String.fromCodePoint(state.point);
            state.needed = 0;
          }
        }
      }
      return result;
    }
    class Interface extends EventEmitter {
      constructor(options, output, completer, terminal) {
        super();
        if (options && typeof options.on === 'function') {
          options = {input: options, output, completer, terminal};
        }
        options = options || {};
        const input = options.input;
        if (!input || typeof input.on !== 'function' ||
            typeof input.removeListener !== 'function') invalid('input');
        this.input = input;
        this.output = options.output;
        this.terminal = options.terminal === undefined ?
            !!(this.output && this.output.isTTY) : !!options.terminal;
        if (this.terminal) unsupported('terminal editing');
        this.closed = false;
        this.paused = false;
        this.line = '';
        this._prompt = options.prompt === undefined ? '> ' : String(options.prompt);
        this._question = null;
        this._decoder = {needed: 0};
        this._lastCR = null;
        this._crlfDelay = Math.max(100, Number(options.crlfDelay) || 100);
        this._onData = chunk => {
          if (this.closed) return;
          const text = typeof chunk === 'string' ? chunk :
              decodeChunk(this._decoder, chunk);
          this._consume(text);
        };
        this._onEnd = () => {
          if (this.closed) return;
          // Node readline emits its buffered text without flushing an
          // incomplete UTF-8 byte sequence when the input ends.
          this._decoder.needed = 0;
          if (this.line) {
            const line = this.line;
            this.line = '';
            this._emitLine(line);
          }
          this.close();
        };
        this._onError = error => this.emit('error', error);
        input.on('data', this._onData);
        input.on('end', this._onEnd);
        input.on('error', this._onError);
        this._signal = options.signal;
        this._onAbort = () => this.close();
        if (this._signal) {
          if (this._signal.aborted) queueMicrotask(this._onAbort);
          else this._signal.addEventListener('abort', this._onAbort, {once: true});
        }
        if (typeof input.resume === 'function') input.resume();
      }
      _emitLine(line) {
        if (this.closed) return;
        if (this._question) {
          const callback = this._question;
          this._question = null;
          callback(line);
        } else {
          this.emit('line', line);
        }
      }
      _consume(text) {
        for (const char of text) {
          if (this.closed) break;
          if (char === '\n' && this._lastCR !== null &&
              Date.now() - this._lastCR <= this._crlfDelay) {
            this._lastCR = null;
            continue;
          }
          this._lastCR = null;
          if (char === '\r' || char === '\n') {
            const line = this.line;
            this.line = '';
            if (char === '\r') this._lastCR = Date.now();
            this._emitLine(line);
          } else {
            this.line += char;
          }
        }
      }
      close() {
        if (this.closed) return;
        this.closed = true;
        this._question = null;
        this.input.removeListener('data', this._onData);
        this.input.removeListener('end', this._onEnd);
        this.input.removeListener('error', this._onError);
        if (this._signal) this._signal.removeEventListener('abort', this._onAbort);
        if (typeof this.input.pause === 'function') this.input.pause();
        this.emit('close');
      }
      pause() {
        if (!this.paused) {
          this.paused = true;
          if (typeof this.input.pause === 'function') this.input.pause();
          this.emit('pause');
        }
        return this;
      }
      resume() {
        if (this.paused && !this.closed) {
          this.paused = false;
          if (typeof this.input.resume === 'function') this.input.resume();
          this.emit('resume');
        }
        return this;
      }
      setPrompt(prompt) { this._prompt = String(prompt); }
      getPrompt() { return this._prompt; }
      prompt() {
        if (this.closed) return;
        this.resume();
        if (this.output) this.output.write(this._prompt);
      }
      question(query, options, callback) {
        if (typeof options === 'function') callback = options;
        else if (options && options.signal) unsupported('question AbortSignal');
        if (typeof callback !== 'function') invalid('question callback');
        if (this.closed) {
          const error = new Error('readline was closed');
          error.code = 'ERR_USE_AFTER_CLOSE';
          throw error;
        }
        if (this._question) return;
        this._question = callback;
        this.resume();
        if (this.output) this.output.write(String(query));
      }
      write(data, key) {
        if (key !== undefined) unsupported('keypress editing');
        this.resume();
        this._onData(data);
      }
    }
    return {
      Interface,
      createInterface: (...args) => new Interface(...args),
      emitKeypressEvents: () => unsupported('emitKeypressEvents'),
      clearLine: () => unsupported('clearLine'),
      clearScreenDown: () => unsupported('clearScreenDown'),
      cursorTo: () => unsupported('cursorTo'),
      moveCursor: () => unsupported('moveCursor'),
    };
  })();
