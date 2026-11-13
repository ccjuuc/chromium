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
        super({highWaterMark: 65536, autoDestroy: true});
        Object.assign(this, {_child: child, _fd: fd, _context: child._context, _eof: false});
        this.once('close', () => {
          this._closeObserved = true;
          queueMicrotask(() => child._maybeClose());
        });
      }
      emit(name, ...args) {
        return runWithAsyncContext(this._context, super.emit, this, [name, ...args]);
      }
      _read() {
        if (!this.destroyed && !this._eof && this._child._active) {
          const result = operation(this._child, 'resume', {fd: this._fd});
          if (!result?.ok) this.destroy(resultError(result, 'read'));
        }
      }
      _accept(bytes) {
        if (this.destroyed || this._eof) return;
        // Readable owns buffering, decoders, flow, read(n), async iteration and
        // pipeline. Only native backpressure crosses this adapter boundary.
        if (!this.push(bytes) && this._child._active) {
          const result = operation(this._child, 'pause', {fd: this._fd});
          if (!result?.ok) this.destroy(resultError(result, 'read'));
        }
      }
      _end() {
        if (this.destroyed || this._eof) return;
        this._eof = true;
        this.push(null);
      }
      _destroy(error, callback) {
        if (this._child._active) operation(this._child, 'destroy', {fd: this._fd});
        callback(error);
      }
    }

    class InputPipe extends streamModule.Writable {
      constructor(child) {
        super({highWaterMark: 65536, autoDestroy: true});
        Object.assign(this, {_child: child, _context: child._context,
          _pending: new Map(), _nextToken: 0, _finalCallback: null, _destroyCallback: null});
        this.once('close', () => {
          this._closeObserved = true;
          queueMicrotask(() => child._maybeClose());
        });
      }
      emit(name, ...args) {
        return runWithAsyncContext(this._context, super.emit, this, [name, ...args]);
      }
      _write(chunk, encoding, callback) {
        const token = ++this._nextToken;
        this._pending.set(token, callback);
        const result = operation(this._child, 'write',
          {fd: 0, token, data: Buffer.from(chunk, encoding).toString('base64')});
        if (!result?.ok) queueMicrotask(() => this._complete(token, resultError(result, 'write')));
      }
      _complete(token, error) {
        const callback = this._pending.get(token);
        if (!callback) return;
        this._pending.delete(token);
        runWithAsyncContext(this._context, callback, undefined, [error || null]);
        this._completeDestroy();
      }
      _final(callback) {
        this._finalCallback = callback;
        const result = operation(this._child, 'end', {fd: 0});
        if (!result?.ok) queueMicrotask(() => this._finishNative(resultError(result, 'shutdown')));
      }
      _finishNative(error) {
        const callback = this._finalCallback;
        if (!callback) return;
        this._finalCallback = null;
        runWithAsyncContext(this._context, callback, undefined, [error || null]);
        this._completeDestroy();
      }
      _destroy(error, callback) {
        this._destroyCallback = () => callback(error);
        if (this._child._active) operation(this._child, 'destroy', {fd: 0});
        // libuv retains write/shutdown requests through cancellation. Their
        // callbacks must finish before Writable emits close or child closes.
        this._completeDestroy();
      }
      _completeDestroy() {
        if (!this._destroyCallback || this._pending.size || this._finalCallback) return;
        const callback = this._destroyCallback;
        this._destroyCallback = null;
        callback();
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
            this.stdio.some(stream => stream && !stream._closeObserved)) return;
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
          finally { child._closeReceived = true; child._maybeClose(); }
        });
        return child;
      }
      child.pid = result.pid;
      child._active = true;
      children.set(child._id, child);
      queueMicrotask(() => emit(child, 'spawn'));
      return child;
    }

    function execFile(file, args, options, callback) {
      if (Array.isArray(args)) args = args.slice();
      else if (typeof args === 'function') {
        callback = args; args = []; options = {};
      } else if (args != null && typeof args === 'object') {
        callback = options; options = args; args = [];
      } else if (args == null) args = [];
      else throw fail('ERR_INVALID_ARG_TYPE', 'The args argument must be an array', TypeError);
      if (typeof options === 'function') { callback = options; options = {}; }
      if (options == null) options = {};
      if (typeof options !== 'object' || Array.isArray(options))
        throw fail('ERR_INVALID_ARG_TYPE', 'The options argument must be an object', TypeError);
      if (callback != null && typeof callback !== 'function')
        throw fail('ERR_INVALID_ARG_TYPE', 'The callback argument must be a function', TypeError);
      const settings = {encoding: 'utf8', timeout: 0, maxBuffer: 1024 * 1024,
        killSignal: 'SIGTERM', ...options};
      if (settings.timeout != null && (!Number.isInteger(settings.timeout) || settings.timeout < 0))
        throw fail('ERR_OUT_OF_RANGE', 'options.timeout must be a non-negative integer', RangeError);
      if (settings.maxBuffer != null &&
          (typeof settings.maxBuffer !== 'number' || !(settings.maxBuffer >= 0)))
        throw fail('ERR_OUT_OF_RANGE', 'options.maxBuffer must be a non-negative number', RangeError);
      const signal = settings.signal;
      if (signal != null && (typeof signal !== 'object' || typeof signal.aborted !== 'boolean' ||
          typeof signal.addEventListener !== 'function' || typeof signal.removeEventListener !== 'function'))
        throw fail('ERR_INVALID_ARG_TYPE', 'options.signal must be an AbortSignal', TypeError);
      let killSignal = settings.killSignal;
      if (killSignal != null) {
        if (typeof killSignal !== 'string' && typeof killSignal !== 'number')
          throw fail('ERR_INVALID_ARG_TYPE', 'options.killSignal must be a string or number', TypeError);
        if (typeof killSignal === 'string') killSignal = killSignal.toUpperCase();
        const result = nativeCall({op: 'validate-signal', signal: killSignal});
        if (!result?.ok) throw fail('ERR_UNKNOWN_SIGNAL', 'Unknown signal: ' + settings.killSignal, TypeError);
      }
      // execFile collects output from actual pipes; it never constructs a shell
      // command. Spawn remains authoritative for platform and launch options.
      const spawnOptions = {};
      const collected = new Set(['encoding', 'timeout', 'maxBuffer', 'killSignal', 'signal']);
      for (const key of Object.keys(options)) {
        if (!collected.has(key) && !(key === 'cwd' && options[key] == null))
          spawnOptions[key] = options[key];
      }
      if (spawnOptions.stdio !== undefined) unsupported('execFile option stdio');
      const child = spawn(file, args, spawnOptions);
      const encoding = settings.encoding !== 'buffer' && Buffer.isEncoding(settings.encoding)
          ? settings.encoding : null;
      const output = {stdout: [], stderr: []};
      const lengths = {stdout: 0, stderr: 0};
      let failure = null, finished = false, killed = false, timeout;
      const cmd = [file, ...args].join(' ');
      function cleanup() {
        if (timeout !== undefined) { clearTimeout(timeout); timeout = undefined; }
        signal?.removeEventListener('abort', abort);
      }
      function finish(code, exitSignal) {
        if (finished) return;
        finished = true;
        cleanup();
        if (!callback) return;
        const collect = name => encoding || child[name]?.readableEncoding
            ? output[name].join('') : Buffer.concat(output[name]);
        const stdout = collect('stdout'), stderr = collect('stderr');
        if (!failure && code === 0 && exitSignal === null) callback(null, stdout, stderr);
        else {
          failure ||= Object.assign(new Error(`Command failed: ${cmd}\n${stderr}`),
              {code, killed: child.killed || killed, signal: exitSignal});
          failure.cmd = cmd;
          callback(failure, stdout, stderr);
        }
      }
      function onError(error) {
        if (finished) return;
        failure = error;
        child.stdout?.destroy(); child.stderr?.destroy();
        finish();
      }
      function kill() {
        child.stdout?.destroy(); child.stderr?.destroy();
        killed = true;
        try { child.kill(killSignal == null ? undefined : killSignal); }
        catch (error) { onError(error); }
      }
      function abort() {
        if (finished) return;
        try {
          if (child.kill(killSignal == null ? undefined : killSignal)) {
            emit(child, 'error', Object.assign(new Error('The operation was aborted'),
                {name: 'AbortError', code: 'ABORT_ERR', cause: signal.reason}));
          }
        } catch (error) { emit(child, 'error', error); }
      }
      for (const name of ['stdout', 'stderr']) {
        const stream = child[name];
        if (!stream) continue;
        if (encoding) stream.setEncoding(encoding);
        stream.on('data', chunk => {
          if (finished) return;
          const length = stream.readableEncoding ? Buffer.byteLength(chunk, stream.readableEncoding) : chunk.length;
          const previous = lengths[name];
          lengths[name] += length;
          if (lengths[name] > settings.maxBuffer) {
            // Like Node, the limit counts encoded bytes while string output
            // keeps the Readable decoder's complete characters across chunks.
            output[name].push(chunk.slice(0, settings.maxBuffer - previous));
            failure = fail('ERR_CHILD_PROCESS_STDIO_MAXBUFFER', `${name} maxBuffer length exceeded`, RangeError);
            kill();
          } else output[name].push(chunk);
        });
      }
      child.on('close', finish);
      child.on('error', onError);
      child.once('exit', cleanup);
      if (settings.timeout > 0) timeout = setTimeout(() => { timeout = undefined; kill(); }, settings.timeout);
      if (signal) {
        if (signal.aborted) queueMicrotask(abort);
        else signal.addEventListener('abort', abort, {once: true});
      }
      return child;
    }
    Object.defineProperty(execFile, Symbol.for('nodejs.util.promisify.custom'), {
      value: function(...args) {
        let resolve, reject;
        const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
        promise.child = execFile(...args, (error, stdout, stderr) => {
          if (error) { error.stdout = stdout; error.stderr = stderr; reject(error); }
          else resolve({stdout, stderr});
        });
        return promise;
      },
    });
    Object.defineProperty(execFile[Symbol.for('nodejs.util.promisify.custom')],
        'name', {value: execFile.name, configurable: true});

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
          child.stdin?._finishNative(
              event.code ? fail(event.code, 'Child process pipe shutdown failed') : null);
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
    return {ChildProcess, spawn, execFile,
      _forkChild: () => unsupported('_forkChild'), spawnSync: () => unsupported('spawnSync'),
      exec: () => unsupported('exec'), execSync: () => unsupported('execSync'),
      execFileSync: () => unsupported('execFileSync'),
      fork: () => unsupported('fork')};
  }
