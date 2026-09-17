  // Shared OS-backed child process objects. Native spawn returns the actual
  // PID or error; pipe and lifecycle notifications are always asynchronous.
  function createChildProcessModule(nativeCall, subscribe) {
    const children = new Map();
    let nextId = 0;
    const internalConstructor = Symbol('ChildProcess');
    const fail = (code, message, Type = Error) => Object.assign(new Type(message), {code});
    const unsupported = operation => {
      throw fail('ERR_NOT_SUPPORTED', `child_process.${operation} is not supported by this runtime`);
    };
    function checkedString(value, name, empty = true) {
      if (typeof value !== 'string')
        throw fail('ERR_INVALID_ARG_TYPE', `The "${name}" argument must be a string`, TypeError);
      if ((!empty && !value) || value.includes('\0'))
        throw fail('ERR_INVALID_ARG_VALUE', `Invalid "${name}" argument`, TypeError);
      return value;
    }
    function operation(child, op, fields = {}) {
      return nativeCall({id: child._id, op, ...fields});
    }
    function resultError(result, syscall) {
      const error = fail(result?.code || 'EIO',
          `${syscall} ${result?.code || 'EIO'}: ${result?.message || 'Native operation failed'}`);
      error.syscall = syscall;
      if (result?.errno !== undefined) error.errno = result.errno;
      return error;
    }
    function emit(resource, name, ...args) {
      return runWithAsyncContext(resource._context, resource.emit, resource, [name, ...args]);
    }

    class OutputPipe extends streamModule.Readable {
      constructor(child, fd) {
        super();
        Object.assign(this, {_child: child, _fd: fd, _context: child._context,
          _queue: [], _flowing: null, _eof: false, _tail: Buffer.alloc(0),
          readable: true, readableLength: 0, readableEnded: false,
          destroyed: false, closed: false});
      }
      on(name, listener) {
        super.on(name, listener);
        if (name === 'data' && this._flowing !== false) this.resume();
        else if (name === 'readable') { this._flowing = false; this._read(); }
        return this;
      }
      addListener(name, listener) { return this.on(name, listener); }
      once(name, listener) {
        super.once(name, listener);
        if (name === 'data' && this._flowing !== false) this.resume();
        else if (name === 'readable') { this._flowing = false; this._read(); }
        return this;
      }
      _read() {
        if (!this.destroyed && !this._eof && this._child._active)
          operation(this._child, 'resume', {fd: this._fd});
      }
      pause() {
        this._flowing = false;
        if (!this.destroyed && !this._eof && this._child._active)
          operation(this._child, 'pause', {fd: this._fd});
        return this;
      }
      resume() {
        // A readable listener owns consumption through read(). In particular,
        // exit-time draining must not discard its buffered short final chunk.
        this._flowing = this.listenerCount('readable') === 0;
        this._read();
        queueMicrotask(() => this._drain());
        return this;
      }
      isPaused() { return this._flowing === false; }
      setEncoding(encoding) {
        if (!['utf8', 'utf-8'].includes(String(encoding).toLowerCase()))
          return unsupported('stdout/stderr.setEncoding(' + encoding + ')');
        if (this._encoding) return this;
        this._encoding = 'utf8';
        const text = this._decode(Buffer.concat(this._queue, this.readableLength), this._eof);
        this._queue = text.length ? [text] : [];
        this.readableLength = text.length;
        return this;
      }
      _decode(bytes, final = false) {
        if (!this._encoding) return bytes;
        const data = this._tail.length ? Buffer.concat([this._tail, bytes]) : bytes;
        let end = data.length;
        if (!final && end) {
          let start = end - 1;
          while (start > 0 && (data[start] & 0xc0) === 0x80 && end - start < 4) --start;
          const lead = data[start];
          const count = lead >= 0xc2 && lead <= 0xdf ? 2 :
              lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
          if (end - start < count) end = start;
        }
        this._tail = Buffer.from(data.subarray(end));
        return data.subarray(0, end).toString('utf8');
      }
      _accept(bytes) {
        if (this.destroyed) return;
        const chunk = this._decode(bytes);
        if (!chunk.length) return;
        this._queue.push(chunk);
        this.readableLength += chunk.length;
        if (this._flowing) this._drain();
        else {
          if (this.readableLength >= 65536 && this._child._active)
            operation(this._child, 'pause', {fd: this._fd});
          emit(this, 'readable');
        }
      }
      _drain() {
        while (!this.destroyed && this._flowing && this._queue.length) {
          const chunk = this._queue.shift();
          this.readableLength -= chunk.length;
          emit(this, 'data', chunk);
        }
        this._finish();
      }
      _end() {
        if (this.destroyed || this._eof) return;
        this._eof = true;
        const tail = this._decode(Buffer.alloc(0), true);
        if (tail.length) {
          this._queue.push(tail);
          this.readableLength += tail.length;
        }
        if (this._flowing) this._drain();
        else {
          // read(size) can now return a short chunk. Include any decoder tail
          // before waking readers, so it cannot bypass read() as a data event.
          emit(this, 'readable');
          this._finish();
        }
      }
      read(size) {
        if (size !== undefined && (!Number.isInteger(size) || size < 0))
          throw fail('ERR_OUT_OF_RANGE', 'Read size must be a non-negative integer', RangeError);
        if (!this.readableLength || size === 0 ||
            (size > this.readableLength && !this._eof)) {
          this._read();
          this._finish();
          return null;
        }
        // Node counts UTF-16 code units after setEncoding(), bytes otherwise.
        const data = this._encoding ? this._queue.join('') :
            Buffer.concat(this._queue, this.readableLength);
        const count = size === undefined ? data.length : Math.min(size, data.length);
        this._queue = count < data.length ? [data.slice(count)] : [];
        this.readableLength -= count;
        this._read();
        queueMicrotask(() => this._finish());
        return data.slice(0, count);
      }
      _finish() {
        if (!this._eof || this._queue.length || this.readableEnded || this.destroyed) return;
        this.readable = false;
        this.readableEnded = true;
        emit(this, 'end');
        this.destroy();
      }
      destroy(error) {
        if (this.destroyed) return this;
        this.destroyed = true;
        this.readable = false;
        this._queue = [];
        this.readableLength = 0;
        if (this._child._active) operation(this._child, 'destroy', {fd: this._fd});
        queueMicrotask(() => {
          try { if (error) emit(this, 'error', error); }
          finally {
            this.closed = true;
            emit(this, 'close');
            this._child._maybeClose();
          }
        });
        return this;
      }
      pipe(destination, options) {
        const onData = data => { if (destination.write(data) === false) this.pause(); };
        const onDrain = () => this.resume();
        this.on('data', onData);
        destination.on('drain', onDrain);
        this.once('end', () => {
          destination.removeListener('drain', onDrain);
          if (options?.end !== false) destination.end();
        });
        destination.emit('pipe', this);
        return destination;
      }
    }

    class InputPipe extends streamModule.Writable {
      constructor(child) {
        super();
        Object.assign(this, {_child: child, _context: child._context,
          _pending: new Map(), _nextToken: 0, writable: true, writableLength: 0,
          writableEnded: false, writableFinished: false, destroyed: false, closed: false});
      }
      write(chunk, encoding, callback) {
        if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
        if (callback !== undefined && typeof callback !== 'function')
          throw fail('ERR_INVALID_ARG_TYPE', 'The callback must be a function', TypeError);
        if (typeof chunk !== 'string' && !(chunk instanceof Uint8Array))
          throw fail('ERR_INVALID_ARG_TYPE', 'The chunk must be a string or Uint8Array', TypeError);
        const bytes = Buffer.from(chunk, encoding);
        const token = ++this._nextToken;
        this._pending.set(token, {callback, size: bytes.length});
        this.writableLength += bytes.length;
        const result = this.writableEnded || this.destroyed ?
            {ok: false, code: 'ERR_STREAM_WRITE_AFTER_END'} :
            operation(this._child, 'write', {fd: 0, token, data: bytes.toString('base64')});
        if (!result?.ok) queueMicrotask(() => this._complete(token, resultError(result, 'write')));
        return result?.ok === true && this.writableLength < 65536;
      }
      _complete(token, error) {
        const pending = this._pending.get(token);
        if (!pending) return;
        this._pending.delete(token);
        const blocked = this.writableLength >= 65536;
        this.writableLength -= pending.size;
        if (pending.callback)
          runWithAsyncContext(this._context, pending.callback, undefined, [error || null]);
        if (error) this.destroy(error);
        else if (blocked && this.writableLength < 65536) emit(this, 'drain');
        this._maybeClose();
      }
      end(chunk, encoding, callback) {
        if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
        else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
        if (callback !== undefined && typeof callback !== 'function')
          throw fail('ERR_INVALID_ARG_TYPE', 'The callback must be a function', TypeError);
        if (callback) this.once('finish', callback);
        if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
        if (!this.writableEnded) {
          this.writableEnded = true;
          this.writable = false;
          const result = operation(this._child, 'end', {fd: 0});
          if (!result?.ok) this.destroy(resultError(result, 'shutdown'));
        }
        return this;
      }
      destroy(error) {
        if (this.destroyed) return this;
        this.destroyed = true;
        this.writable = false;
        if (this._child._active) operation(this._child, 'destroy', {fd: 0});
        queueMicrotask(() => {
          try { if (error) emit(this, 'error', error); }
          finally { this._destroyDispatched = true; this._maybeClose(); }
        });
        return this;
      }
      _maybeClose() {
        if (!this.destroyed || !this._destroyDispatched || this.closed || this._pending.size) return;
        this.closed = true;
        emit(this, 'close');
        this._child._maybeClose();
      }
    }

    class ChildProcess extends EventEmitter {
      constructor(token) {
        super();
        if (token !== internalConstructor) unsupported('ChildProcess');
        Object.assign(this, {_id: String(++nextId), _context: currentAsyncContext,
          _active: false, pid: undefined, connected: false, killed: false,
          exitCode: null, signalCode: null});
      }
      spawn() { return unsupported('ChildProcess.spawn'); }
      send() { return unsupported('ChildProcess.send'); }
      disconnect() { return unsupported('ChildProcess.disconnect'); }
      kill(signal = 'SIGTERM') {
        const nativeSignal = typeof signal === 'string' ? signal.toUpperCase() : signal;
        if (typeof nativeSignal === 'string' ? !/^SIG[A-Z0-9]+$/.test(nativeSignal) :
            !Number.isInteger(nativeSignal) || nativeSignal < 0 || nativeSignal > 0x7fffffff)
          throw fail('ERR_UNKNOWN_SIGNAL', 'Unknown signal: ' + signal, TypeError);
        if (!this._active || this.exitCode !== null || this.signalCode !== null) return false;
        const result = operation(this, 'kill', {signal: nativeSignal});
        if (result?.ok) { this.killed = true; return true; }
        if (result?.code === 'EINVAL')
          throw fail('ERR_UNKNOWN_SIGNAL', 'Unknown signal: ' + signal, TypeError);
        if (result?.code !== 'ESRCH') emit(this, 'error', resultError(result, 'kill'));
        return false;
      }
      ref() { if (this._active) operation(this, 'ref'); return this; }
      unref() { if (this._active) operation(this, 'unref'); return this; }
      _maybeClose() {
        if (!this._closeReceived || this._closeEmitted ||
            this.stdio.some(stream => stream && !stream.closed)) return;
        this._closeEmitted = true;
        children.delete(this._id);
        emit(this, 'close', this.exitCode, this.signalCode);
      }
    }

    function spawn(file, args, options) {
      checkedString(file, 'file', false);
      if (args === undefined) args = [];
      else if (!Array.isArray(args)) { options = args; args = []; }
      if (options === undefined || options === null) options = {};
      if (typeof options !== 'object' || Array.isArray(options))
        throw fail('ERR_INVALID_ARG_TYPE', 'The options argument must be an object', TypeError);
      args = args.map(value => checkedString(String(value), 'args'));
      const supported = new Set(['cwd', 'env', 'stdio', 'windowsHide', 'argv0']);
      for (const key of Object.keys(options)) {
        if (!supported.has(key) && options[key] !== undefined) {
          if (['shell', 'detached', 'windowsVerbatimArguments'].includes(key) && options[key] === false)
            continue;
          unsupported('spawn option ' + key);
        }
      }
      const request = {file, args};
      if (options.cwd !== undefined) request.cwd = checkedString(options.cwd, 'cwd');
      if (options.argv0 !== undefined) request.argv0 = checkedString(options.argv0, 'argv0');
      if (options.windowsHide !== undefined) {
        if (typeof options.windowsHide !== 'boolean')
          throw fail('ERR_INVALID_ARG_TYPE', 'windowsHide must be a boolean', TypeError);
        request.windowsHide = options.windowsHide;
      }
      const env = options.env == null ? globalThis.process?.env : options.env;
      if (env !== undefined && env !== null) {
        if (typeof env !== 'object' || Array.isArray(env))
          throw fail('ERR_INVALID_ARG_TYPE', 'env must be an object', TypeError);
        request.env = Object.create(null);
        const names = new Set();
        for (const key of Object.keys(env).sort()) {
          checkedString(key, 'env key', false);
          if (key.includes('=')) throw fail('ERR_INVALID_ARG_VALUE', 'Invalid environment key', TypeError);
          const normalized = globalThis.process?.platform === 'win32' ? key.toUpperCase() : key;
          if (env[key] === undefined || names.has(normalized)) continue;
          names.add(normalized);
          request.env[key] = checkedString(String(env[key]), 'env value');
        }
      }
      let stdio = options.stdio;
      if (stdio === undefined || stdio === null) stdio = 'pipe';
      if (typeof stdio === 'string') stdio = [stdio, stdio, stdio];
      if (!Array.isArray(stdio)) throw fail('ERR_INVALID_ARG_TYPE', 'stdio must be a string or array', TypeError);
      if (stdio.length > 3) unsupported('spawn extra stdio descriptors');
      request.stdio = Array.from({length: 3}, (_, fd) => {
        const mode = stdio[fd] == null ? 'pipe' : stdio[fd];
        if (!['pipe', 'ignore', 'inherit'].includes(mode)) unsupported('spawn stdio ' + String(mode));
        return mode;
      });
      const child = new ChildProcess(internalConstructor);
      child.spawnfile = file;
      child.spawnargs = [options.argv0 === undefined ? file : options.argv0, ...args];
      const result = operation(child, 'spawn', request);
      child.stdio = request.stdio.map((mode, fd) => mode !== 'pipe' ? null :
          fd === 0 ? new InputPipe(child) : new OutputPipe(child, fd));
      [child.stdin, child.stdout, child.stderr] = child.stdio;
      if (!result?.ok) {
        const error = resultError(result, 'spawn ' + file);
        error.path = file;
        error.spawnargs = args;
        child.exitCode = result?.errno ?? -1;
        queueMicrotask(() => {
          for (const stream of child.stdio) stream?.destroy();
          try { emit(child, 'error', error); }
          finally { queueMicrotask(() => emit(child, 'close', child.exitCode, null)); }
        });
        return child;
      }
      child.pid = result.pid;
      child._active = true;
      children.set(child._id, child);
      queueMicrotask(() => emit(child, 'spawn'));
      return child;
    }

    subscribe(event => {
      const child = children.get(event?.id);
      if (!child) return;
      const stream = child.stdio[event.fd];
      switch (event.event) {
        case 'data': stream?._accept(Buffer.from(event.data, 'base64')); break;
        case 'end':
          if (stream) {
            if (event.code) stream.destroy(fail(event.code, 'Child process pipe read failed'));
            else stream._end();
          }
          break;
        case 'write':
          child.stdin?._complete(event.token,
              event.code ? fail(event.code, 'Child process pipe write failed') : null);
          break;
        case 'finish':
          if (child.stdin && !child.stdin.destroyed) {
            if (event.code) child.stdin.destroy(fail(event.code, 'Child process pipe shutdown failed'));
            else { child.stdin.writableFinished = true; emit(child.stdin, 'finish'); child.stdin.destroy(); }
          }
          break;
        case 'exit':
          child.signalCode = event.signal ? event.signalName : null;
          child.exitCode = event.signal ? null : event.exitCode;
          emit(child, 'exit', child.exitCode, child.signalCode);
          child.stdin?.destroy();
          // Drain unused pipes too: close follows exit after the OS reaches EOF.
          child.stdout?.resume();
          child.stderr?.resume();
          break;
        case 'close':
          child._active = false;
          child._closeReceived = true;
          child._maybeClose();
          break;
      }
    });
    return {ChildProcess, spawn,
      _forkChild: () => unsupported('_forkChild'), spawnSync: () => unsupported('spawnSync'),
      exec: () => unsupported('exec'), execSync: () => unsupported('execSync'),
      execFile: () => unsupported('execFile'), execFileSync: () => unsupported('execFileSync'),
      fork: () => unsupported('fork')};
  }
