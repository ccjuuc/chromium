  const fsConstants = {
    F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1,
  };
  const fsError = (error, request) => {
    const match = /^([A-Z][A-Z0-9_]+):/.exec(error.message || '');
    if (match) error.code = match[1];
    error.path = request.path;
    const syscall = /, ([a-z_]+) '/.exec(error.message || '');
    if (syscall) error.syscall = syscall[1];
    return error;
  };
  const fsRequest = (operation, path, options = {}) =>
    ({...options, operation, path: String(path)});
  const fsCallSync = (request, data) => {
    try {
      const value = __xenonFsCall(request, data);
      return value === null ? undefined : value;
    } catch (error) {
      throw fsError(error, request);
    }
  };
  const fsCallAsync = async (request, data) => {
    try {
      const value = await __xenonFsCallAsync(request, data);
      return value === null ? undefined : value;
    } catch (error) {
      throw fsError(error, request);
    }
  };
  const fsUnsupported = method => {
    const error = new Error(`fs.${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  };
  const fsValidateCallback = callback => {
    if (typeof callback !== 'function') {
      const error = new TypeError('The callback argument must be a function');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
  };
  const fsCallback = (callback, operation) => {
    fsValidateCallback(callback);
    // Separate fulfillment/rejection callbacks ensure a user callback throwing
    // does not cause the same callback to be invoked for a second time.
    operation().then(value => callback(null, value), error => callback(error));
  };
  const fsEncoding = options =>
    typeof options === 'string' ? options : options?.encoding;
  const fsReadResult = (bytes, options) => {
    const buffer = Buffer.from(bytes);
    const encoding = fsEncoding(options);
    return encoding ? buffer.toString(encoding) : buffer;
  };
  const fsWriteData = (data, options) => {
    if (typeof data === 'string') return Buffer.from(data, fsEncoding(options));
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    const error = new TypeError('File data must be a string, Buffer, or an ArrayBuffer view');
    error.code = 'ERR_INVALID_ARG_TYPE';
    throw error;
  };
  const fsWriteRequest = (path, options) => {
    const flag = typeof options === 'object' ? options?.flag : undefined;
    if (flag !== undefined && flag !== 'w' && flag !== 'a') {
      return fsUnsupported('writeFile with flag ' + flag);
    }
    return fsRequest(flag === 'a' ? 'append_file' : 'write_file', path);
  };
  const fsStatResult = stat => ({
    isFile: () => stat.isFile,
    isDirectory: () => stat.isDirectory,
    isSymbolicLink: () => stat.isSymbolicLink,
    size: stat.size,
    mtime: new Date(stat.mtimeMs),
    mtimeMs: stat.mtimeMs,
    birthtime: new Date(stat.birthtimeMs),
    birthtimeMs: stat.birthtimeMs,
    mode: stat.isDirectory ? 0o777 : 0o666,
  });

  const fsPromises = {
    async readFile(path, options) {
      return fsReadResult(await fsCallAsync(
        fsRequest('read_file', path, {returnBytes: true})), options);
    },
    async writeFile(path, data, options) {
      return fsCallAsync(fsWriteRequest(path, options), fsWriteData(data, options));
    },
    async appendFile(path, data, options) {
      const appendOptions = typeof options === 'string' ? {encoding: options, flag: 'a'} :
          {flag: 'a', ...options};
      return fsCallAsync(fsWriteRequest(path, appendOptions), fsWriteData(data, appendOptions));
    },
    async stat(path) {
      return fsStatResult(await fsCallAsync(fsRequest('stat', path)));
    },
    async lstat(path) {
      return fsStatResult(await fsCallAsync(fsRequest('lstat', path)));
    },
    async readdir(path) { return fsCallAsync(fsRequest('readdir', path)); },
    async mkdir(path, options) {
      return fsCallAsync(fsRequest('mkdir', path, {recursive: Boolean(options?.recursive)}));
    },
    async unlink(path) { return fsCallAsync(fsRequest('unlink', path)); },
    async rm(path, options) {
      return fsCallAsync(fsRequest('rm', path, {
        recursive: Boolean(options?.recursive), force: Boolean(options?.force),
      }));
    },
    async access(path, mode = 0) {
      if (mode !== 0) return fsUnsupported('access with permission mode');
      return fsCallAsync(fsRequest('access', path));
    },
    async realpath(path) { return fsCallAsync(fsRequest('realpath', path)); },
    async copyFile(src, dest, flags = 0) {
      if (flags !== 0) return fsUnsupported('copyFile with flags');
      return fsCallAsync(fsRequest('copy_file', src, {destination: String(dest)}));
    },
    async rename(oldPath, newPath) {
      return fsCallAsync(fsRequest('rename', oldPath, {destination: String(newPath)}));
    },
    async open() { return fsUnsupported('promises.open'); },
    async chmod() { return fsUnsupported('promises.chmod'); },
    async chown() { return fsUnsupported('promises.chown'); },
  };

  const fsModule = {
    constants: fsConstants,
    promises: fsPromises,
    existsSync(path) {
      try { return Boolean(fsCallSync(fsRequest('exists', path))); }
      catch { return false; }
    },
    readFileSync(path, options) {
      return fsReadResult(fsCallSync(
        fsRequest('read_file', path, {returnBytes: true})), options);
    },
    writeFileSync(path, data, options) {
      return fsCallSync(fsWriteRequest(path, options), fsWriteData(data, options));
    },
    statSync(path) { return fsStatResult(fsCallSync(fsRequest('stat', path))); },
    lstatSync(path) { return fsStatResult(fsCallSync(fsRequest('lstat', path))); },
    readdirSync(path) { return fsCallSync(fsRequest('readdir', path)); },
    mkdirSync(path, options) {
      return fsCallSync(fsRequest('mkdir', path, {recursive: Boolean(options?.recursive)}));
    },
    unlinkSync(path) { return fsCallSync(fsRequest('unlink', path)); },
    rmdirSync(path, options) {
      return fsCallSync(fsRequest('rmdir', path, {recursive: Boolean(options?.recursive)}));
    },
    rmSync(path, options) {
      return fsCallSync(fsRequest('rm', path, {
        recursive: Boolean(options?.recursive), force: Boolean(options?.force),
      }));
    },
    accessSync(path, mode = 0) {
      if (mode !== 0) return fsUnsupported('accessSync with permission mode');
      return fsCallSync(fsRequest('access', path));
    },
    readFile(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.readFile(path, options));
    },
    writeFile(path, data, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.writeFile(path, data, options));
    },
    stat(path, callback) { fsCallback(callback, () => fsPromises.stat(path)); },
    lstat(path, callback) { fsCallback(callback, () => fsPromises.lstat(path)); },
    readdir(path, callback) { fsCallback(callback, () => fsPromises.readdir(path)); },
    mkdir(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.mkdir(path, options));
    },
    unlink(path, callback) { fsCallback(callback, () => fsPromises.unlink(path)); },
    rm(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsPromises.rm(path, options));
    },
    rmdir(path, options, callback) {
      if (typeof options === 'function') { callback = options; options = undefined; }
      fsCallback(callback, () => fsCallAsync(
        fsRequest('rmdir', path, {recursive: Boolean(options?.recursive)})));
    },
    access(path, mode, callback) {
      if (typeof mode === 'function') { callback = mode; mode = undefined; }
      fsCallback(callback, () => fsPromises.access(path, mode));
    },
    exists(path, callback) {
      fsValidateCallback(callback);
      fsCallback((error, value) => callback(!error && Boolean(value)),
        () => fsCallAsync(fsRequest('exists', path)));
    },
    copyFile(src, dest, flags, callback) {
      if (typeof flags === 'function') { callback = flags; flags = undefined; }
      fsCallback(callback, () => fsPromises.copyFile(src, dest, flags));
    },
    copyFileSync(src, dest, flags = 0) {
      if (flags !== 0) return fsUnsupported('copyFileSync with flags');
      return fsCallSync(fsRequest('copy_file', src, {destination: String(dest)}));
    },
    rename(oldPath, newPath, callback) {
      fsCallback(callback, () => fsPromises.rename(oldPath, newPath));
    },
    renameSync(oldPath, newPath) {
      return fsCallSync(fsRequest('rename', oldPath, {destination: String(newPath)}));
    },
  };

  const realpathFn = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(callback, () => fsPromises.realpath(path, options));
  };
  realpathFn.native = realpathFn;
  fsModule.realpath = realpathFn;
  const realpathSyncFn = path => fsCallSync(fsRequest('realpath', path));
  realpathSyncFn.native = realpathSyncFn;
  fsModule.realpathSync = realpathSyncFn;

  // Do not fabricate descriptors, streams, watchers, or successful I/O.
  for (const method of [
    'watch', 'watchFile', 'unwatchFile',
    'chmodSync', 'chownSync', 'openSync', 'closeSync', 'readSync', 'writeSync',
  ]) {
    fsModule[method] = () => fsUnsupported(method);
  }
  for (const method of ['chmod', 'chown', 'open', 'close', 'read', 'write']) {
    fsModule[method] = (...args) => fsCallback(args[args.length - 1],
      async () => fsUnsupported(method));
  }

  // Read the requested range through the worker bridge, then deliver bounded
  // chunks. No OS descriptor or fake open event is exposed.
  class FileReadStream extends Readable {
    constructor(path, options = {}) {
      super();
      if (typeof options === 'string') options = {encoding: options};
      if (!options || typeof options !== 'object') throw new TypeError('Invalid ReadStream options');
      if (options.fd != null || options.fs || (options.flags && options.flags !== 'r') ||
          options.autoClose === false) return fsUnsupported('ReadStream options');
      this.path = path;
      this.pending = true;
      this.readable = true;
      this.readableEnded = false;
      this.destroyed = false;
      this.closed = false;
      this.bytesRead = 0;
      this._flowing = null;
      this._queued = false;
      this._resumeQueued = false;
      this._bytes = null;
      this._offset = 0;
      this._encoding = null;
      this._decoder = {needed: 0};
      this._emitClose = options.emitClose !== false;
      this._chunkSize = options.highWaterMark === undefined ? 65536 : options.highWaterMark;
      const start = options.start === undefined ? 0 : options.start;
      const end = options.end === undefined ? Infinity : options.end;
      if (!Number.isSafeInteger(start) || start < 0 ||
          (end !== Infinity && (!Number.isSafeInteger(end) || end < start)) ||
          !Number.isSafeInteger(this._chunkSize) || this._chunkSize <= 0) {
        const error = new RangeError('Invalid ReadStream range or highWaterMark');
        error.code = 'ERR_OUT_OF_RANGE';
        throw error;
      }
      if (options.encoding) this.setEncoding(options.encoding);
      this._signal = options.signal;
      this._abort = () => {
        const error = new Error('The operation was aborted');
        error.name = 'AbortError';
        error.code = 'ABORT_ERR';
        this.destroy(error);
      };
      if (this._signal) {
        if (this._signal.aborted) queueMicrotask(this._abort);
        else this._signal.addEventListener('abort', this._abort, {once: true});
      }
      const range = {returnBytes: true, start};
      if (end !== Infinity) range.end = end;
      Promise.resolve().then(() => this.destroyed ? null :
          fsCallAsync(fsRequest('read_file', path, range)))
          .then(bytes => {
            if (this.destroyed) return;
            this.pending = false;
            this._bytes = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
            this.emit('ready');
            this._schedule();
          }, error => this.destroy(error));
    }
    on(name, listener) {
      super.on(name, listener);
      if (name === 'data' && this._flowing !== false) this.resume();
      return this;
    }
    once(name, listener) {
      super.once(name, listener);
      if (name === 'data' && this._flowing !== false) this.resume();
      return this;
    }
    setEncoding(encoding) {
      if (!['utf8', 'utf-8'].includes(String(encoding).toLowerCase()))
        return fsUnsupported('ReadStream encoding ' + encoding);
      this._encoding = 'utf8';
      return this;
    }
    pause() {
      if (this._flowing !== false) {
        this._flowing = false;
        this.emit('pause');
      }
      return this;
    }
    resume() {
      if (this.destroyed || this.readableEnded) return this;
      if (!this._flowing && !this._resumeQueued) {
        this._resumeQueued = true;
        queueMicrotask(() => {
          this._resumeQueued = false;
          if (this._flowing && !this.destroyed) this.emit('resume');
        });
      }
      this._flowing = true;
      this._schedule();
      return this;
    }
    isPaused() { return this._flowing === false; }
    _decodeUtf8(bytes, final = false) {
      const state = this._decoder;
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
      if (final && state.needed) {
        state.needed = 0;
        result += '\ufffd';
      }
      return result;
    }
    _schedule() {
      if (this._queued || !this._flowing || !this._bytes || this.destroyed) return;
      this._queued = true;
      queueMicrotask(() => {
        this._queued = false;
        if (!this._flowing || !this._bytes || this.destroyed) return;
        if (this._offset >= this._bytes.length) {
          if (this._encoding) {
            const tail = this._decodeUtf8(new Uint8Array(0), true);
            if (tail) this.emit('data', tail);
            if (!this._flowing || this.destroyed) return;
          }
          this.readableEnded = true;
          this.readable = false;
          this._bytes = null;
          this.emit('end');
          this.destroy();
          return;
        }
        const end = Math.min(this._offset + this._chunkSize, this._bytes.length);
        const bytes = this._bytes.subarray(this._offset, end);
        this._offset = end;
        this.bytesRead += bytes.length;
        // Decode incrementally: the main bootstrap's TextDecoder fallback is
        // not streaming, and per-chunk TextDecoder would also strip BOMs.
        const chunk = this._encoding ? this._decodeUtf8(bytes) : bytes;
        if (chunk.length) this.emit('data', chunk);
        this._schedule();
      });
    }
    destroy(error) {
      if (this.destroyed) return this;
      this.destroyed = true;
      this.pending = false;
      this.readable = false;
      this._bytes = null;
      if (this._signal) this._signal.removeEventListener('abort', this._abort);
      queueMicrotask(() => {
        try { if (error) this.emit('error', error); }
        finally {
          this.closed = true;
          if (this._emitClose) this.emit('close');
        }
      });
      return this;
    }
    close(callback) {
      if (callback) {
        if (this.closed) queueMicrotask(callback);
        else this.once('close', callback);
      }
      return this.destroy();
    }
  }
  fsModule.ReadStream = FileReadStream;
  fsModule.createReadStream = (path, options) => new FileReadStream(path, options);

  // Path-based writer over the real asynchronous filesystem bridge. Each
  // operation opens/closes its file; no persistent descriptor or open event is
  // invented. Descriptor-based writes and positioned writes remain unsupported.
  class FileWriteStream extends Writable {
    constructor(path, options = {}) {
      super();
      if (typeof options === 'string') options = {encoding: options};
      if (!options || typeof options !== 'object') throw new TypeError('Invalid WriteStream options');
      if (options.fd != null || options.fs || options.start !== undefined ||
          options.autoClose === false || options.flush || options.objectMode ||
          (options.mode !== undefined && options.mode !== 0o666)) {
        return fsUnsupported('WriteStream options');
      }
      const flags = options.flags === undefined ? 'w' : options.flags;
      if (flags !== 'w' && flags !== 'a') return fsUnsupported('WriteStream flags ' + flags);
      const highWaterMark = options.highWaterMark === undefined ? 16384 : options.highWaterMark;
      if (!Number.isSafeInteger(highWaterMark) || highWaterMark < 0) {
        throw Object.assign(new RangeError('Invalid WriteStream highWaterMark'), {code: 'ERR_OUT_OF_RANGE'});
      }
      this.path = path;
      this.fd = null;
      this.pending = true;
      this.writable = true;
      this.writableEnded = false;
      this.writableFinished = false;
      this.destroyed = false;
      this.closed = false;
      this.errored = null;
      this.bytesWritten = 0;
      this.writableLength = 0;
      this.writableHighWaterMark = highWaterMark;
      this.writableNeedDrain = false;
      this.writableCorked = 0;
      this._writes = [];
      this._endCallbacks = [];
      this._closeCallbacks = [];
      this._busy = true;
      this._closeScheduled = false;
      this._finishScheduled = false;
      this._emitClose = options.emitClose !== false;
      this.setDefaultEncoding(options.encoding === undefined ? 'utf8' : options.encoding);
      this._signal = options.signal;
      this._abort = () => this.destroy(Object.assign(new Error('The operation was aborted'),
          {name: 'AbortError', code: 'ABORT_ERR'}));
      if (this._signal) {
        if (typeof this._signal.addEventListener !== 'function' ||
            typeof this._signal.removeEventListener !== 'function') {
          throw Object.assign(new TypeError('signal must be an AbortSignal'), {code: 'ERR_INVALID_ARG_TYPE'});
        }
        if (this._signal.aborted) queueMicrotask(this._abort);
        else this._signal.addEventListener('abort', this._abort, {once: true});
      }
      Promise.resolve().then(() => {
        if (this.destroyed) return;
        return flags === 'a' ? fsModule.promises.appendFile(path, Buffer.alloc(0)) :
                               fsModule.promises.writeFile(path, Buffer.alloc(0));
      }).then(() => {
        this._busy = false;
        this.pending = false;
        if (this.destroyed) return this._completeClose();
        // A completed create/truncate is observable, without fabricating an fd.
        this.emit('ready');
        this._pump();
      }, error => {
        this._busy = false;
        this.pending = false;
        this.destroy(error);
      });
    }
    _callback(callback, error) {
      if (callback) queueMicrotask(() => callback(error));
    }
    _validateCallback(callback) {
      if (callback !== undefined) fsValidateCallback(callback);
    }
    _error(code, message) {
      return Object.assign(new Error(message), {code});
    }
    setDefaultEncoding(encoding) {
      if (typeof encoding !== 'string' ||
          !['utf8', 'utf-8', 'hex', 'base64', 'base64url', 'ascii', 'latin1',
            'binary', 'ucs2', 'ucs-2', 'utf16le', 'utf-16le'].includes(encoding.toLowerCase())) {
        throw this._error('ERR_UNKNOWN_ENCODING', 'Unknown encoding: ' + encoding);
      }
      this._defaultEncoding = encoding;
      return this;
    }
    write(chunk, encoding, callback) {
      if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
      this._validateCallback(callback);
      if (this.destroyed) {
        this._callback(callback, this.errored || this._error('ERR_STREAM_DESTROYED', 'Cannot write after destroy'));
        return false;
      }
      if (this.writableEnded) {
        const error = this._error('ERR_STREAM_WRITE_AFTER_END', 'write after end');
        this._callback(callback, error);
        this.destroy(error);
        return false;
      }
      let bytes;
      if (typeof chunk === 'string') {
        const selected = encoding || this._defaultEncoding;
        const previous = this._defaultEncoding;
        this.setDefaultEncoding(selected);
        this._defaultEncoding = previous;
        bytes = Buffer.from(chunk, selected);
      } else if (ArrayBuffer.isView(chunk)) {
        bytes = Buffer.from(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
      } else {
        throw Object.assign(new TypeError('WriteStream data must be a string or ArrayBuffer view'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      this._writes.push({bytes, callback});
      this.writableLength += bytes.length;
      const accepted = this.writableLength < this.writableHighWaterMark;
      if (!accepted) this.writableNeedDrain = true;
      this._pump();
      return accepted;
    }
    _pump() {
      if (this._busy || this.pending || this.destroyed || this.writableCorked) return;
      const entry = this._writes.shift();
      if (!entry) {
        if (this.writableEnded) this._finish();
        return;
      }
      this._busy = true;
      // Byte storage is snapped at write(), before user code can mutate it.
      Promise.resolve().then(() => fsModule.promises.appendFile(this.path, entry.bytes)).then(() => {
        this._busy = false;
        this.bytesWritten += entry.bytes.length;
        this.writableLength -= entry.bytes.length;
        this._callback(entry.callback, this.destroyed ?
            (this.errored || this._error('ERR_STREAM_DESTROYED', 'Stream was destroyed')) : undefined);
        if (this.destroyed) return this._completeClose();
        if (this.writableNeedDrain && this.writableLength === 0 && !this.writableEnded) {
          this.writableNeedDrain = false;
          queueMicrotask(() => {
            if (!this.destroyed && !this.writableEnded && this.writableLength === 0) this.emit('drain');
          });
        }
        this._pump();
      }, error => {
        this._busy = false;
        this.writableLength -= entry.bytes.length;
        this._callback(entry.callback, error);
        this.destroy(error);
      });
    }
    end(chunk, encoding, callback) {
      if (typeof chunk === 'function') { callback = chunk; chunk = undefined; encoding = undefined; }
      else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
      this._validateCallback(callback);
      if (this.destroyed) {
        this._callback(callback, this.errored ||
            (this.writableFinished ? undefined : this._error('ERR_STREAM_DESTROYED', 'Stream was destroyed')));
        return this;
      }
      if (this.writableFinished && (chunk === undefined || chunk === null)) {
        this._callback(callback);
        return this;
      }
      if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
      if (callback) this._endCallbacks.push(callback);
      this.writableEnded = true;
      this.writable = false;
      this.writableCorked = 0;
      this._pump();
      return this;
    }
    _finish() {
      if (this._finishScheduled || this.writableFinished) return;
      this._finishScheduled = true;
      queueMicrotask(() => {
        if (this.destroyed) return;
        this.emit('prefinish');
        if (this.destroyed) return;
        this.writableFinished = true;
        this.writableNeedDrain = false;
        for (const callback of this._endCallbacks.splice(0)) this._callback(callback);
        queueMicrotask(() => {
          if (this.destroyed) return;
          try { this.emit('finish'); }
          finally { this.destroy(); }
        });
      });
    }
    cork() { ++this.writableCorked; }
    uncork() {
      if (this.writableCorked) --this.writableCorked;
      this._pump();
    }
    destroy(error) {
      if (this.destroyed) {
        if (error && !this.errored) this.errored = error;
        this._completeClose();
        return this;
      }
      this.destroyed = true;
      this.writable = false;
      this.errored = error || null;
      if (this._signal) this._signal.removeEventListener('abort', this._abort);
      this._completeClose();
      return this;
    }
    _completeClose() {
      // The native bridge cannot cancel an already submitted file operation.
      // close waits for it, so no write can complete after the close event.
      if (this._busy || this._closeScheduled) return;
      this._closeScheduled = true;
      this.pending = false;
      const error = this.errored || this._error('ERR_STREAM_DESTROYED', 'Stream was destroyed');
      for (const entry of this._writes.splice(0)) this._callback(entry.callback, error);
      for (const callback of this._endCallbacks.splice(0)) this._callback(callback, error);
      this.writableLength = 0;
      this.writableNeedDrain = false;
      queueMicrotask(() => {
        try {
          if (this.errored) this.emit('error', this.errored);
        } finally {
          this.closed = true;
          for (const callback of this._closeCallbacks.splice(0)) this._callback(callback);
          if (this._emitClose) this.emit('close');
        }
      });
    }
    close(callback) {
      this._validateCallback(callback);
      if (callback) {
        if (this.closed) this._callback(callback);
        else this._closeCallbacks.push(callback);
      }
      if (!this.destroyed) this.end();
      return this;
    }
  }
  fsModule.WriteStream = FileWriteStream;
  fsModule.createWriteStream = (path, options) => new FileWriteStream(path, options);
