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

  function fsMakeStats(entry, bigint = false) {
    return createFsStats({
      isFile: entry.type === 'file',
      isDirectory: entry.type === 'dir',
      isSymbolicLink: false,
      size: bigint ? String(entry.data?.length || 0) : entry.data?.length || 0,
      ...(bigint ? {mtimeNs: String(BigInt(entry.mtimeMs) * 1000000n)} :
        {mtimeMs: entry.mtimeMs}),
    }, bigint);
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

  const fsConstants = {...fsStatTypeConstants, F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1};

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

  function fsVirtualFileBytes(path) {
    const entry = fsGetVirtualEntry(normalizeFsPath(path));
    if (!entry) throw fsErrno('ENOENT', 'open', path);
    if (entry.type !== 'file') throw fsErrno('EISDIR', 'read', path);
    return entry.data || new Uint8Array(0);
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

  function fsBytes(data, options) {
    return Buffer.from(nativeFsWriteData(data, options));
  }

  function fsWriteSync(path, data, options, defaultFlag = 'w') {
    const operation = nativeFsWriteOperation(options, defaultFlag);
    let bytes = fsBytes(data, options);
    if (!fsUsesVirtualMount(path)) {
      fsNativeSync(operation, path, {data: bytes});
      return;
    }
    const p = normalizeFsPath(path);
    if (fsIsVirtualPackageJson(p)) throw fsErrno('EROFS', 'open', path);
    if (operation === 'append_file') {
      let previous = Buffer.alloc(0);
      try { previous = fsVirtualFileBytes(path); }
      catch (error) { if (error?.code !== 'ENOENT') throw error; }
      bytes = Buffer.concat([previous, bytes]);
    }
    const parent = fsParentPath(p);
    if (parent) fsEnsureDir(parent);
    memFiles.set(p, {type: 'file', data: bytes, mtimeMs: Date.now()});
  }

  async function fsWriteAsync(path, data, options, defaultFlag = 'w') {
    const operation = nativeFsWriteOperation(options, defaultFlag);
    const bytes = fsBytes(data, options);
    return fsAsyncOperation(path, operation, {data: bytes},
        () => fsWriteSync(path, bytes, options, defaultFlag));
  }

  function fsNativeStats(stat, bigint = false) {
    return createFsStats(stat, bigint);
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
    Stats,
    constants: fsConstants,
    existsSync: (path) => {
      if (fsUsesVirtualMount(path)) {
        return Boolean(fsGetVirtualEntry(normalizeFsPath(path)));
      }
      return Boolean(fsNativeSync('exists', path));
    },
    statSync: (path, options) => {
      const bigint = options?.bigint === true;
      if (!fsUsesVirtualMount(path)) {
        return fsNativeStats(fsNativeSync('stat', path, {bigint}), bigint);
      }
      const entry = fsGetVirtualEntry(normalizeFsPath(path));
      if (!entry) {
        throw fsErrno('ENOENT', 'stat', path);
      }
      return fsMakeStats(entry, bigint);
    },
    lstatSync: (path, options) => {
      const bigint = options?.bigint === true;
      return fsUsesVirtualMount(path) ? fsModule.statSync(path, {bigint}) :
        fsNativeStats(fsNativeSync('lstat', path, {bigint}), bigint);
    },
    readFileSync: (path, encoding) => {
      if (!fsUsesVirtualMount(path)) {
        return fsReadResult(fsNativeSync('read_file', path, {returnBytes: true}), encoding);
      }
      const bytes = fsVirtualFileBytes(path);
      const enc = fsEncodingOf(encoding);
      const buf = Buffer.from(bytes);
      return enc ? buf.toString(enc) : buf;
    },
    writeFileSync: (path, data, options) => fsWriteSync(path, data, options),
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
    accessSync: (path, mode) => {
      path = nativeFsPathArgument(path, 'path');
      mode = nativeFsAccessMode(mode);
      if (!fsUsesVirtualMount(path)) {
        fsNativeSync('access', path, {mode});
        return;
      }
      const p = normalizeFsPath(path);
      if (!fsGetVirtualEntry(p)) {
        throw fsErrno('ENOENT', 'access', path);
      }
      if ((mode & fsConstants.W_OK) && !memFiles.has(p)) {
        throw fsErrno('EACCES', 'access', path);
      }
    },
    appendFileSync: (path, data, options) => fsWriteSync(path, data, options, 'a'),
    chmodSync: (path, mode) => {
      path = nativeFsPathArgument(path, 'path');
      mode = nativeFsChmodMode(mode);
      if (fsUsesVirtualMount(path)) fsUnsupported('chmod on a virtual mount');
      fsNativeSync('chmod', path, {mode});
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
    fsCallback(() => fsWriteAsync(path, data, options), callback);
  };
  fsModule.stat = (path, options, callback) => {
    if (typeof options === 'function') { callback = options; options = undefined; }
    const bigint = options?.bigint === true;
    fsCallback(() => fsAsyncOperation(
        path, 'stat', {bigint}, () => fsModule.statSync(path, {bigint}),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat, bigint)),
        callback, true);
  };
  fsModule.lstat = (path, options, callback) => {
    if (typeof options === 'function') { callback = options; options = undefined; }
    const bigint = options?.bigint === true;
    fsCallback(() => fsAsyncOperation(
        path, 'lstat', {bigint}, () => fsModule.lstatSync(path, {bigint}),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat, bigint)),
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
    if (typeof mode === 'function') { callback = mode; mode = undefined; }
    fsValidateCallback(callback);
    path = nativeFsPathArgument(path, 'path');
    mode = nativeFsAccessMode(mode);
    fsCallback(() => fsAsyncOperation(
        path, 'access', {mode}, () => fsModule.accessSync(path, mode)), callback);
  };
  fsModule.appendFile = (path, data, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    fsCallback(() => fsWriteAsync(path, data, options, 'a'), callback);
  };
  fsModule.chmod = (path, mode, callback) => {
    path = nativeFsPathArgument(path, 'path');
    mode = nativeFsChmodMode(mode);
    fsValidateCallback(callback);
    fsCallback(() => fsAsyncOperation(path, 'chmod', {mode},
        () => fsModule.chmodSync(path, mode)), callback);
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
    stat: (path, options) => {
      const bigint = options?.bigint === true;
      return fsAsyncOperation(path, 'stat', {bigint}, () => fsModule.statSync(path, {bigint}),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat, bigint));
    },
    lstat: (path, options) => {
      const bigint = options?.bigint === true;
      return fsAsyncOperation(path, 'lstat', {bigint}, () => fsModule.lstatSync(path, {bigint}),
        stat => fsUsesVirtualMount(path) ? stat : fsNativeStats(stat, bigint));
    },
    readFile: (path, options) => fsAsyncOperation(
        path, 'read_file', {returnBytes: true}, () => fsModule.readFileSync(path, options),
        encoded => fsUsesVirtualMount(path) ? encoded : fsReadResult(encoded, options)),
    writeFile: (path, data, options) => fsWriteAsync(path, data, options),
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
    access: async (path, mode) => {
      path = nativeFsPathArgument(path, 'path');
      mode = nativeFsAccessMode(mode);
      await fsAsyncOperation(path, 'access', {mode}, () => fsModule.accessSync(path, mode));
    },
    chmod: async (path, mode) => {
      path = nativeFsPathArgument(path, 'path');
      mode = nativeFsChmodMode(mode);
      await fsAsyncOperation(path, 'chmod', {mode}, () => fsModule.chmodSync(path, mode));
    },
    appendFile: (path, data, options) => fsWriteAsync(path, data, options, 'a'),
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
    'chownSync', 'openSync', 'closeSync', 'readSync', 'writeSync',
  ]) {
    fsModule[method] = () => fsUnsupported(method);
  }
  for (const method of ['chown', 'open', 'close', 'read', 'write']) {
    fsModule[method] = (...args) => fsCallback(
        async () => fsUnsupported(method), args[args.length - 1]);
  }
  for (const method of ['chown', 'open']) {
    fsModule.promises[method] = async () => fsUnsupported('promises.' + method);
  }
  function fsReadStreamRange(path, start, end) {
    const range = {returnBytes: true, start};
    if (end !== Infinity) range.end = end;
    return fsAsyncOperation(path, 'read_file', range, () => {
      // Synthetic chrome:// files keep their existing filesystem validation.
      // Copy the selected bytes so a short stream does not retain the full file.
      const bytes = fsVirtualFileBytes(path);
      return Buffer.from(bytes.subarray(start,
          end === Infinity ? bytes.length : Math.min(bytes.length, end + 1)));
    }, bytes => fsUsesVirtualMount(path) ? bytes : fsReadResult(bytes));
  }

  // Read the requested range through the worker bridge, then deliver bounded
  // chunks. No OS descriptor or fake open event is exposed.
  installFileStreams(fsModule, fsReadStreamRange, fsUnsupported);

  const mkdtempRequest = request => {
    if (fsUsesVirtualMount(request.path))
      return fsUnsupported('mkdtemp on virtual resources');
    // Keep Node's original prefix spelling and trailing separator. Unlike a
    // filename lookup, changing the prefix changes the returned directory name.
    return request;
  };
  installMkdtemp(fsModule, {Buffer, path: nativeFsPathArgument,
    sync: request => {
      try { return transport.sendSync('__xenon:fs', mkdtempRequest(request)); }
      catch (error) { throw fsNodeError(error, 'mkdtemp', request.path); }
    },
    asynchronous: async request => {
      try { return await transport.invoke('__xenon:fs', mkdtempRequest(request)); }
      catch (error) { throw fsNodeError(error, 'mkdtemp', request.path); }
    },
  });

  fsModule.default = fsModule;
  let originalFsModule;
  function getOriginalFsModule() {
    const requestFor = (request, data) => ({...request,
      path: request.operation === 'mkdtemp' ? request.path : normalizeFsPath(request.path), noAsar: true,
      ...(data === undefined ? {} : {data})});
    return originalFsModule ||= createNativeFs(
        (request, data) => transport.sendSync('__xenon:fs', requestFor(request, data)),
        (request, data) => transport.invoke('__xenon:fs', requestFor(request, data)));
  }
