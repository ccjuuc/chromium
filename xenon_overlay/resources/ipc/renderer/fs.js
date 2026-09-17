  // --- 6. In-memory FS (do not claim every path exists) ---
  function normalizeFsPath(p) {
    if (p instanceof URL) p = urlModule.fileURLToPath(p);
    let s = String(p || '');
    // chrome:// resources have a separate synthetic namespace. A native
    // POSIX filename may legally contain backslashes, including at its end.
    if (/^chrome:[\\/]+/i.test(s)) {
      s = s.replace(/^chrome:[\\/]+/i, 'chrome:\\').replace(/\//g, '\\');
      return s.replace(/\\+$/, '');
    }
    return runtimePlatform === 'win32' ? s.replace(/\//g, '\\') : s;
  }

  function fsParentPath(p) {
    if (!fsUsesVirtualMount(p)) {
      const parent = pathModule.dirname(p);
      return parent === '.' || parent === p ? '' : parent;
    }
    const i = p.lastIndexOf('\\');
    if (i <= 0) {
      return '';
    }
    if (p.length >= 2 && p[1] === ':' && i === 2) {
      return p.slice(0, 3);
    }
    return p.slice(0, i);
  }

  function fsErrno(code, op, p) {
    const err = new Error(`${code}: ${op} '${p}'`);
    err.code = code;
    return err;
  }

  const memFiles = new Map();

  function fsEnsureDir(p) {
    p = normalizeFsPath(p);
    if (!p) {
      return;
    }
    const parent = fsParentPath(p);
    if (parent && parent !== p && !memFiles.has(parent)) {
      fsEnsureDir(parent);
    }
    const existing = memFiles.get(p);
    if (existing && existing.type === 'file') {
      throw fsErrno('ENOTDIR', 'mkdir', p);
    }
    memFiles.set(p, {type: 'dir', data: null, mtimeMs: Date.now()});
  }

  function fsMakeStats(entry) {
    return {
      isFile: () => entry.type === 'file',
      isDirectory: () => entry.type === 'dir',
      isSymbolicLink: () => false,
      size: entry.data ? entry.data.length : 0,
      mtimeMs: entry.mtimeMs,
      mtime: new Date(entry.mtimeMs),
    };
  }

  function fsEncodingOf(encoding) {
    if (!encoding) {
      return null;
    }
    if (typeof encoding === 'string') {
      return encoding;
    }
    return encoding.encoding || null;
  }

  const fsConstants = {F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1};

  // Electron's node-integrated renderer exposes Node's real `fs` module. This
  // renderer remains Chromium-sandboxed, so native paths are brokered to the
  // Browser process (whose filesystem bridge also understands ASAR archives).
  // Only chrome:// resources use the synthetic mount. In particular, an
  // extracted application's appPath is a real filesystem root: treating it as
  // virtual hides its package metadata, configuration, and plugins.
  function fsUsesVirtualMount(path) {
    const normalized = normalizeFsPath(path).toLowerCase();
    return normalized.startsWith('chrome:\\');
  }

  const fsVirtualMountRoot = (() => {
    const pageLocation = globalThis.location;
    if (!pageLocation || String(pageLocation.protocol).toLowerCase() !== 'chrome:' ||
        !pageLocation.hostname) {
      return '';
    }
    return `chrome:\\${pageLocation.hostname}`;
  })();
  const fsVirtualPackagePath = fsVirtualMountRoot ?
      fsVirtualMountRoot + '\\package.json' : '';

  function fsIsVirtualRoot(normalizedPath) {
    return Boolean(fsVirtualMountRoot) &&
        normalizeFsPath(normalizedPath).toLowerCase() ===
            fsVirtualMountRoot.toLowerCase();
  }

  function fsIsVirtualPackageJson(normalizedPath) {
    return Boolean(fsVirtualPackagePath) &&
        normalizeFsPath(normalizedPath).toLowerCase() ===
            fsVirtualPackagePath.toLowerCase();
  }

  function fsGetVirtualEntry(normalizedPath) {
    const entry = memFiles.get(normalizedPath);
    if (entry) {
      return entry;
    }
    if (fsIsVirtualPackageJson(normalizedPath)) {
      const pkg = JSON.stringify({
        name: hostedAppName || 'app',
        version: hostedAppVersion,
      });
      return {
        type: 'file',
        data: Buffer.from(pkg, 'utf8'),
        mtimeMs: 0,
      };
    }
    if (fsIsVirtualRoot(normalizedPath)) {
      return {type: 'dir', data: null, mtimeMs: 0};
    }
    return null;
  }

  function fsNodeError(error, syscall, path) {
    const message = String(error && error.message || error || 'EIO: fs error');
    const match = /^([A-Z][A-Z0-9_]+):/.exec(message);
    const result = new Error(message);
    result.code = match ? match[1] : 'EIO';
    result.errno = result.code;
    result.syscall = syscall;
    result.path = String(path);
    return result;
  }

  function fsNativeSync(operation, path, extra = {}) {
    try {
      return transport.sendSync('__xenon:fs', {
        operation,
        path: normalizeFsPath(path),
        ...extra,
      });
    } catch (error) {
      throw fsNodeError(error, operation, path);
    }
  }

  function fsNativeAsync(operation, path, extra = {}) {
    return transport.invoke('__xenon:fs', {
      operation,
      path: normalizeFsPath(path),
      ...extra,
    }).catch(error => {
      throw fsNodeError(error, operation, path);
    });
  }

  function fsBytes(data) {
    return ArrayBuffer.isView(data) ?
        Buffer.from(new Uint8Array(data.buffer, data.byteOffset, data.byteLength)) :
        Buffer.from(String(data), 'utf8');
  }

  function fsNativeStats(stat) {
    return {
      isFile: () => Boolean(stat.isFile),
      isDirectory: () => Boolean(stat.isDirectory),
      isSymbolicLink: () => Boolean(stat.isSymbolicLink),
      size: Number(stat.size) || 0,
      mtimeMs: Number(stat.mtimeMs) || 0,
      mtime: new Date(Number(stat.mtimeMs) || 0),
      birthtimeMs: Number(stat.birthtimeMs) || 0,
      birthtime: new Date(Number(stat.birthtimeMs) || 0),
      mode: stat.isDirectory ? 0o777 : 0o666,
    };
  }

  function fsReadResult(bytes, options) {
    // Internal IPC converts native BLOB results to ArrayBuffer. Keep accepting
    // the existing encoded result for older transports and test fixtures.
    const buffer = typeof bytes === 'string' ? Buffer.from(bytes, 'base64') :
        Buffer.from(new Uint8Array(bytes));
    const encoding = fsEncodingOf(options);
    return encoding ? buffer.toString(encoding) : buffer;
  }

  function fsAsyncOperation(path, operation, extra, virtualCall, transform) {
    const promise = fsUsesVirtualMount(path) ?
        Promise.resolve().then(virtualCall) :
        fsNativeAsync(operation, path, extra);
    return transform ? promise.then(transform) : promise;
  }

  function fsValidateCallback(callback) {
    if (typeof callback !== 'function') {
      const error = new TypeError('The callback argument must be a function');
      error.code = 'ERR_INVALID_ARG_TYPE';
      throw error;
    }
  }

  function fsCallback(operation, callback, includeValue = false) {
    fsValidateCallback(callback);
    let promise;
    try {
      promise = operation();
    } catch (error) {
      promise = Promise.reject(error);
    }
    promise.then(
        value => includeValue ? callback(null, value) : callback(null),
        error => callback(error));
  }

  function fsUnsupported(method) {
    const error = new Error(`fs.${method} is not supported by this runtime`);
    error.code = 'ERR_NOT_SUPPORTED';
    throw error;
  }

  const fsModule = {
    constants: fsConstants,
    existsSync: (path) => {
      if (fsUsesVirtualMount(path)) {
        return Boolean(fsGetVirtualEntry(normalizeFsPath(path)));
      }
      return Boolean(fsNativeSync('exists', path));
    },
    statSync: (path) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeStats(fsNativeSync('stat', path));
      }
      const entry = fsGetVirtualEntry(normalizeFsPath(path));
      if (!entry) {
        throw fsErrno('ENOENT', 'stat', path);
      }
      return fsMakeStats(entry);
    },
    lstatSync: (path) => fsModule.statSync(path),
    readFileSync: (path, encoding) => {
      if (!fsUsesVirtualMount(path)) {
        return fsReadResult(fsNativeSync('read_file', path, {returnBytes: true}), encoding);
      }
      const entry = fsGetVirtualEntry(normalizeFsPath(path));
      if (!entry) {
        throw fsErrno('ENOENT', 'open', path);
      }
      if (entry.type !== 'file') {
        throw fsErrno('EISDIR', 'read', path);
      }
      const enc = fsEncodingOf(encoding);
      const buf = Buffer.from(entry.data || new Uint8Array(0));
      return enc ? buf.toString(enc) : buf;
    },
    writeFileSync: (path, data, _options) => {
      if (!fsUsesVirtualMount(path)) {
        fsNativeSync('write_file', path, {data: fsBytes(data)});
        return;
      }
      const p = normalizeFsPath(path);
      if (fsIsVirtualPackageJson(p)) {
        throw fsErrno('EROFS', 'open', path);
      }
      const parent = fsParentPath(p);
      if (parent) {
        fsEnsureDir(parent);
      }
      const bytes = fsBytes(data);
      memFiles.set(p, {type: 'file', data: bytes, mtimeMs: Date.now()});
    },
    mkdirSync: (path, opts) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('mkdir', path, {
          recursive: Boolean(opts && typeof opts === 'object' && opts.recursive),
        });
      }
      fsEnsureDir(normalizeFsPath(path));
    },
    readdirSync: (path) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('readdir', path);
      }
      const dir = normalizeFsPath(path);
      const entry = fsGetVirtualEntry(dir);
      if (!entry || entry.type !== 'dir') {
        throw fsErrno('ENOENT', 'scandir', path);
      }
      const prefix = dir.endsWith('\\') ? dir : dir + '\\';
      const names = new Set();
      if (fsIsVirtualRoot(dir)) {
        names.add('package.json');
      }
      for (const key of memFiles.keys()) {
        if (!key.startsWith(prefix)) {
          continue;
        }
        const rest = key.slice(prefix.length);
        if (rest && !rest.includes('\\')) {
          names.add(rest);
        }
      }
      return [...names];
    },
    unlinkSync: (path) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('unlink', path);
      }
      const p = normalizeFsPath(path);
      const entry = memFiles.get(p);
      if (!entry && fsIsVirtualPackageJson(p)) {
        throw fsErrno('EROFS', 'unlink', path);
      }
      if (!entry || entry.type !== 'file') {
        throw fsErrno('ENOENT', 'unlink', path);
      }
      memFiles.delete(p);
    },
    rmSync: (path, opts) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('rm', path, {
          recursive: Boolean(opts && opts.recursive),
          force: Boolean(opts && opts.force),
        });
      }
      const p = normalizeFsPath(path);
      if (!memFiles.has(p)) {
        if (fsGetVirtualEntry(p)) {
          throw fsErrno('EROFS', 'rm', path);
        }
        if (opts && opts.force) return;
        throw fsErrno('ENOENT', 'rm', path);
      }
      memFiles.delete(p);
    },
    rmdirSync: (path, opts) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('rmdir', path, {
          recursive: Boolean(opts && opts.recursive),
        });
      }
      return fsModule.rmSync(path, opts);
    },
    accessSync: (path, _mode) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('access', path);
      }
      const p = normalizeFsPath(path);
      if (!fsGetVirtualEntry(p)) {
        throw fsErrno('ENOENT', 'access', path);
      }
    },
    appendFileSync: (path, data) => {
      if (!fsUsesVirtualMount(path)) {
        return fsNativeSync('append_file', path, {
          data: fsBytes(data),
        });
      }
      let prev = Buffer.alloc(0);
      try {
        prev = fsModule.readFileSync(path);
      } catch (err) {
        if (!err || err.code !== 'ENOENT') {
          throw err;
        }
      }
      const extra = fsBytes(data);
      fsModule.writeFileSync(path, Buffer.concat([prev, extra]));
    },
    renameSync: (oldPath, newPath) => {
      if (!fsUsesVirtualMount(oldPath) || !fsUsesVirtualMount(newPath)) {
        if (fsUsesVirtualMount(oldPath) !== fsUsesVirtualMount(newPath)) {
          throw fsErrno('EXDEV', 'rename', oldPath);
        }
        return fsNativeSync('rename', oldPath, {
          destination: normalizeFsPath(newPath),
        });
      }
      const from = normalizeFsPath(oldPath);
      const entry = memFiles.get(from);
      if (!entry && fsGetVirtualEntry(from)) {
        throw fsErrno('EROFS', 'rename', oldPath);
      }
      if (!entry) {
        throw fsErrno('ENOENT', 'rename', oldPath);
      }
      memFiles.set(normalizeFsPath(newPath), entry);
      memFiles.delete(from);
    },
    copyFileSync: (src, dest) => {
      if (!fsUsesVirtualMount(src) || !fsUsesVirtualMount(dest)) {
        if (fsUsesVirtualMount(src) !== fsUsesVirtualMount(dest)) {
          throw fsErrno('EXDEV', 'copyfile', src);
        }
        return fsNativeSync('copy_file', src, {
          destination: normalizeFsPath(dest),
        });
      }
      fsModule.writeFileSync(dest, fsModule.readFileSync(src));
    },
    promises: {},
  };

  fsModule.readFile = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    fsCallback(() => fsAsyncOperation(
        path, 'read_file', {returnBytes: true}, () => fsModule.readFileSync(path, options),
        encoded => fsUsesVirtualMount(path) ? encoded : fsReadResult(encoded, options)),
        callback, true);
  };
  fsModule.writeFile = (path, data, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {data: fsBytes(data)};
    fsCallback(() => fsAsyncOperation(
        path, 'write_file', extra,
        () => fsModule.writeFileSync(path, data, options)), callback);
  };
  fsModule.stat = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'stat', {}, () => fsModule.statSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
        callback, true);
  };
  fsModule.lstat = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'lstat', {}, () => fsModule.lstatSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
        callback, true);
  };
  fsModule.readdir = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'readdir', {}, () => fsModule.readdirSync(path)), callback, true);
  };
  fsModule.mkdir = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {recursive: Boolean(options && options.recursive)};
    fsCallback(() => fsAsyncOperation(
        path, 'mkdir', extra, () => fsModule.mkdirSync(path, options)), callback);
  };
  fsModule.unlink = (path, callback) => fsCallback(() => fsAsyncOperation(
      path, 'unlink', {}, () => fsModule.unlinkSync(path)), callback);
  fsModule.rm = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {
      recursive: Boolean(options && options.recursive),
      force: Boolean(options && options.force),
    };
    fsCallback(() => fsAsyncOperation(
        path, 'rm', extra, () => fsModule.rmSync(path, options)), callback);
  };
  fsModule.rmdir = (path, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {recursive: Boolean(options && options.recursive)};
    fsCallback(() => fsAsyncOperation(
        path, 'rmdir', extra, () => fsModule.rmdirSync(path, options)), callback);
  };
  fsModule.access = (path, mode, callback) => {
    if (typeof mode === 'function') callback = mode;
    fsCallback(() => fsAsyncOperation(
        path, 'access', {}, () => fsModule.accessSync(path, mode)), callback);
  };
  fsModule.appendFile = (path, data, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    const extra = {data: fsBytes(data)};
    fsCallback(() => fsAsyncOperation(
        path, 'append_file', extra,
        () => fsModule.appendFileSync(path, data, options)), callback);
  };
  fsModule.rename = (oldPath, newPath, callback) => fsCallback(
      () => fsUsesVirtualMount(oldPath) ?
          Promise.resolve().then(() => fsModule.renameSync(oldPath, newPath)) :
          fsNativeAsync('rename', oldPath, {destination: normalizeFsPath(newPath)}),
      callback);
  fsModule.copyFile = (src, dest, flags, callback) => {
    if (typeof flags === 'function') callback = flags;
    fsCallback(() => fsUsesVirtualMount(src) ?
        Promise.resolve().then(() => fsModule.copyFileSync(src, dest)) :
        fsNativeAsync('copy_file', src, {destination: normalizeFsPath(dest)}),
        callback);
  };
  fsModule.exists = (path, callback) => {
    fsValidateCallback(callback);
    const promise = fsUsesVirtualMount(path) ?
        Promise.resolve(fsModule.existsSync(path)) :
        fsNativeAsync('exists', path);
    promise.then(value => callback(Boolean(value)), () => callback(false));
  };

  const realpathSync = (path) => fsUsesVirtualMount(path) ?
      String(path) : fsNativeSync('realpath', path);
  realpathSync.native = realpathSync;
  fsModule.realpathSync = realpathSync;
  const realpath = (path, options, callback) => {
    if (typeof options === 'function') callback = options;
    fsCallback(() => fsAsyncOperation(
        path, 'realpath', {}, () => String(path)), callback, true);
  };
  realpath.native = realpath;
  fsModule.realpath = realpath;

  fsModule.promises = {
    stat: (path) => fsAsyncOperation(
        path, 'stat', {}, () => fsModule.statSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
    lstat: (path) => fsAsyncOperation(
        path, 'lstat', {}, () => fsModule.lstatSync(path),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat)),
    readFile: (path, options) => fsAsyncOperation(
        path, 'read_file', {returnBytes: true}, () => fsModule.readFileSync(path, options),
        encoded => fsUsesVirtualMount(path) ? encoded : fsReadResult(encoded, options)),
    writeFile: (path, data, options) => fsAsyncOperation(
        path, 'write_file', {data: fsBytes(data)},
        () => fsModule.writeFileSync(path, data, options)),
    mkdir: (path, options) => fsAsyncOperation(
        path, 'mkdir', {recursive: Boolean(options && options.recursive)},
        () => fsModule.mkdirSync(path, options)),
    readdir: (path) => fsAsyncOperation(
        path, 'readdir', {}, () => fsModule.readdirSync(path)),
    unlink: (path) => fsAsyncOperation(
        path, 'unlink', {}, () => fsModule.unlinkSync(path)),
    rm: (path, options) => fsAsyncOperation(
        path, 'rm', {
          recursive: Boolean(options && options.recursive),
          force: Boolean(options && options.force),
        }, () => fsModule.rmSync(path, options)),
    rmdir: (path, options) => fsAsyncOperation(
        path, 'rmdir', {recursive: Boolean(options && options.recursive)},
        () => fsModule.rmdirSync(path, options)),
    access: (path, mode) => fsAsyncOperation(
        path, 'access', {}, () => fsModule.accessSync(path, mode)),
    appendFile: (path, data, options) => fsAsyncOperation(
        path, 'append_file', {data: fsBytes(data)},
        () => fsModule.appendFileSync(path, data, options)),
    rename: (oldPath, newPath) => fsUsesVirtualMount(oldPath) ?
        Promise.resolve().then(() => fsModule.renameSync(oldPath, newPath)) :
        fsNativeAsync('rename', oldPath, {destination: normalizeFsPath(newPath)}),
    copyFile: (src, dest) => fsUsesVirtualMount(src) ?
        Promise.resolve().then(() => fsModule.copyFileSync(src, dest)) :
        fsNativeAsync('copy_file', src, {destination: normalizeFsPath(dest)}),
    realpath: (path) => fsAsyncOperation(
        path, 'realpath', {}, () => String(path)),
  };
  // These APIs need descriptors, streaming, watchers, or permission support.
  // Do not return empty streams or report success without doing the operation.
  for (const method of [
    'watch', 'watchFile', 'unwatchFile',
    'chmodSync', 'chownSync', 'openSync', 'closeSync', 'readSync', 'writeSync',
  ]) {
    fsModule[method] = () => fsUnsupported(method);
  }
  for (const method of ['chmod', 'chown', 'open', 'close', 'read', 'write']) {
    fsModule[method] = (...args) => fsCallback(
        async () => fsUnsupported(method), args[args.length - 1]);
  }
  for (const method of ['chmod', 'chown', 'open']) {
    fsModule.promises[method] = async () => fsUnsupported('promises.' + method);
  }
  // ReadStream currently reads through the existing whole-file worker bridge,
  // then delivers bounded chunks. No OS descriptor or fake open event is exposed.
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
      Promise.resolve().then(() => this.destroyed ? null : fsModule.promises.readFile(path))
          .then(bytes => {
            if (this.destroyed) return;
            this.pending = false;
            this._bytes = (Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes)).subarray(start,
                end === Infinity ? bytes.length : Math.min(bytes.length, end + 1));
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


  fsModule.default = fsModule;
