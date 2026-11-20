  function nativeFsPathArgument(value, name) {
    if (Buffer.isBuffer(value)) {
      const decoded = value.toString();
      const encoded = Buffer.from(decoded);
      // The transport carries strings; never silently replace non-UTF-8 bytes.
      if (encoded.length !== value.length || encoded.some((byte, index) => byte !== value[index])) {
        throw Object.assign(new Error('fs.a non-UTF-8 ' + name + ' is not supported by this runtime'),
            {code: 'ERR_NOT_SUPPORTED'});
      }
      value = decoded;
    } else if (value instanceof URL) value = urlModule.fileURLToPath(value);
    if (typeof value !== 'string') {
      throw Object.assign(new TypeError(`${name} must be a string, Buffer, or file URL`),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (value.includes('\0')) {
      throw Object.assign(new TypeError(`${name} must not contain null bytes`),
          {code: 'ERR_INVALID_ARG_VALUE'});
    }
    return value;
  }

  function nativeFsAccessMode(mode) {
    if (mode == null) return 0;
    if (typeof mode !== 'number') {
      throw Object.assign(new TypeError('mode must be a number'), {code: 'ERR_INVALID_ARG_TYPE'});
    }
    // Node truncates finite numeric modes before checking the F/R/W/X mask.
    const value = Math.trunc(mode);
    if (!Number.isFinite(value) || value < 0 || value > 7) {
      throw Object.assign(new RangeError('mode must be between 0 and 7'), {code: 'ERR_OUT_OF_RANGE'});
    }
    return value || 0;
  }

  function nativeFsChmodMode(mode) {
    if (typeof mode === 'string') {
      if (!/^[0-7]+$/.test(mode)) {
        throw Object.assign(new TypeError('mode must be an octal string'),
            {code: 'ERR_INVALID_ARG_VALUE'});
      }
      mode = Number.parseInt(mode, 8);
    }
    if (typeof mode !== 'number') {
      throw Object.assign(new TypeError('mode must be a number or octal string'),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (!Number.isInteger(mode) || mode < 0 || mode > 0xffffffff) {
      throw Object.assign(new RangeError('mode must be an unsigned 32-bit integer'),
          {code: 'ERR_OUT_OF_RANGE'});
    }
    return mode;
  }

  function nativeFsWriteOperation(options, defaultFlag = 'w') {
    if (options != null && typeof options !== 'string' && typeof options !== 'object') {
      throw Object.assign(new TypeError('options must be a string or object'),
          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (typeof options === 'object' && options !== null) {
      if (options.flush != null && typeof options.flush !== 'boolean') {
        throw Object.assign(new TypeError('flush must be a boolean'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      if (options.flush === true || (options.mode != null && options.mode !== 0o666)) {
        throw Object.assign(new Error('fs.writeFile custom mode or flush is not supported by this runtime'),
            {code: 'ERR_NOT_SUPPORTED'});
      }
    }
    const flag = typeof options === 'object' ? options?.flag ?? defaultFlag : defaultFlag;
    if (flag !== 'w' && flag !== 'a') {
      throw Object.assign(new Error('fs.writeFile with flag ' + flag + ' is not supported by this runtime'),
          {code: 'ERR_NOT_SUPPORTED'});
    }
    return flag === 'a' ? 'append_file' : 'write_file';
  }

  function nativeFsWriteData(data, options) {
    if (typeof data === 'string') {
      return Buffer.from(data, typeof options === 'string' ? options : options?.encoding);
    }
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    throw Object.assign(new TypeError('File data must be a string, Buffer, or an ArrayBuffer view'),
        {code: 'ERR_INVALID_ARG_TYPE'});
  }

  // Native filesystem facade shared by fs and Electron original-fs. Each
  // instance captures its transport; concurrent async calls never toggle
  // a process-wide ASAR flag or borrow another module's filesystem view.
  function createNativeFs(callSync, callAsync) {
    const {Stats, createFsStats} = createFsStatsApi();
    const fsConstants = {
      ...fsStatTypeConstants,
      ...globalThis.__xenonFsConstants,
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
      ({...options, operation, path: fsPathArgument(path, 'path')});
    const fsCallSync = (request, data) => {
      try {
        const value = callSync(request, data);
        return value === null ? undefined : value;
      } catch (error) {
        throw fsError(error, request);
      }
    };
    const fsCallAsync = async (request, data) => {
      try {
        const value = await callAsync(request, data);
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
    const fsPathArgument = nativeFsPathArgument;
    const fsAccessRequest = (path, mode) => {
      path = fsPathArgument(path, 'path');
      return fsRequest('access', path, {mode: nativeFsAccessMode(mode)});
    };
    const fsChmodRequest = (path, mode) => {
      path = fsPathArgument(path, 'path');
      return fsRequest('chmod', path, {mode: nativeFsChmodMode(mode)});
    };
    const fsSymlinkRequest = (target, path, type) => {
      target = fsPathArgument(target, 'target');
      path = fsPathArgument(path, 'path');
      if (type != null && !['file', 'dir', 'junction'].includes(type)) {
        throw Object.assign(new TypeError('type must be file, dir, or junction'),
            {code: 'ERR_INVALID_ARG_VALUE'});
      }
      // A lock's target can be literal metadata or a missing relative pathname.
      // Only the link pathname is resolved by the native filesystem bridge.
      return fsRequest('symlink', path, {target, ...(type == null ? {} : {type})});
    };
    const fsReadlinkEncoding = options => {
      if (options != null && typeof options !== 'string' && typeof options !== 'object') {
        throw Object.assign(new TypeError('options must be a string or object'),
            {code: 'ERR_INVALID_ARG_TYPE'});
      }
      const selected = fsEncoding(options);
      const encoding = selected == null || selected === '' ? 'utf8' : selected;
      if (encoding === 'buffer') return encoding;
      if (typeof encoding !== 'string' || ![
        'utf8', 'utf-8', 'hex', 'base64', 'base64url', 'ascii', 'latin1', 'binary',
        'ucs2', 'ucs-2', 'utf16le', 'utf-16le',
      ].includes(encoding.toLowerCase())) {
        throw Object.assign(new TypeError('Invalid readlink encoding'),
            {code: 'ERR_INVALID_ARG_VALUE'});
      }
      return encoding.toLowerCase();
    };
    const fsReadlinkRequest = path =>
      fsRequest('readlink', fsPathArgument(path, 'path'), {returnBytes: true});
    const fsReadlinkResult = (bytes, encoding) => {
      const buffer = fsReadResult(bytes);
      if (encoding === 'buffer') return buffer;
      // The main-process Buffer shim only decodes UTF-8/base64/hex itself.
      if (['ascii', 'latin1', 'binary'].includes(encoding)) {
        const mask = encoding === 'ascii' ? 0x7f : 0xff;
        let result = '';
        for (let offset = 0; offset < buffer.length; offset += 8192) {
          result += String.fromCharCode(...buffer.subarray(offset, offset + 8192)
              .map(byte => byte & mask));
        }
        return result;
      }
      if (['ucs2', 'ucs-2', 'utf16le', 'utf-16le'].includes(encoding)) {
        let result = '';
        for (let offset = 0; offset + 1 < buffer.length; offset += 2) {
          result += String.fromCharCode(buffer[offset] | (buffer[offset + 1] << 8));
        }
        return result;
      }
      return fsReadResult(buffer, {encoding});
    };
    const fsWriteData = nativeFsWriteData;
    const fsWriteRequest = (path, options, defaultFlag) =>
      fsRequest(nativeFsWriteOperation(options, defaultFlag), path);
    const fsStatResult = createFsStats;

    class Dirent {
      constructor(name, parentPath, stats) {
        this.name = name;
        this.parentPath = parentPath;
        this.path = parentPath;
        this._isFile = Boolean(stats?.isFile?.());
        this._isDirectory = Boolean(stats?.isDirectory?.());
        this._isSymbolicLink = Boolean(stats?.isSymbolicLink?.());
        this._isBlockDevice = Boolean(stats?.isBlockDevice?.());
        this._isCharacterDevice = Boolean(stats?.isCharacterDevice?.());
        this._isFIFO = Boolean(stats?.isFIFO?.());
        this._isSocket = Boolean(stats?.isSocket?.());
      }
      isFile() { return this._isFile; }
      isDirectory() { return this._isDirectory; }
      isSymbolicLink() { return this._isSymbolicLink; }
      isBlockDevice() { return this._isBlockDevice; }
      isCharacterDevice() { return this._isCharacterDevice; }
      isFIFO() { return this._isFIFO; }
      isSocket() { return this._isSocket; }
    }
    const fsJoinChildPath = (dirPath, name) => {
      if (!dirPath) return name;
      if (dirPath.endsWith('/') || dirPath.endsWith('\\')) return dirPath + name;
      return dirPath + (dirPath.includes('\\') && !dirPath.includes('/') ? '\\' : '/') + name;
    };
    const fsReaddirWithTypesSync = (dirPath, entries) => {
      const parent = fsPathArgument(dirPath, 'path');
      return entries.map(name => {
        let stats = null;
        try {
          stats = fsStatResult(
              fsCallSync(fsRequest('lstat', fsJoinChildPath(parent, name), {bigint: false})),
              false);
        } catch {}
        return new Dirent(name, parent, stats);
      });
    };
    const fsReaddirWithTypesAsync = async (dirPath, entries) => {
      const parent = fsPathArgument(dirPath, 'path');
      return Promise.all(entries.map(async name => {
        let stats = null;
        try {
          stats = fsStatResult(
              await fsCallAsync(fsRequest('lstat', fsJoinChildPath(parent, name), {bigint: false})),
              false);
        } catch {}
        return new Dirent(name, parent, stats);
      }));
    };

    const fsPromises = {
      async readFile(path, options) {
        return fsReadResult(await fsCallAsync(
          fsRequest('read_file', path, {returnBytes: true})), options);
      },
      async writeFile(path, data, options) {
        return fsCallAsync(fsWriteRequest(path, options), fsWriteData(data, options));
      },
      async appendFile(path, data, options) {
        return fsCallAsync(fsWriteRequest(path, options, 'a'), fsWriteData(data, options));
      },
      async stat(path, options) {
        const bigint = options?.bigint === true;
        return fsStatResult(await fsCallAsync(fsRequest('stat', path, {bigint})), bigint);
      },
      async lstat(path, options) {
        const bigint = options?.bigint === true;
        return fsStatResult(await fsCallAsync(fsRequest('lstat', path, {bigint})), bigint);
      },
      async readdir(path, options) {
        const entries = await fsCallAsync(fsRequest('readdir', path));
        if (options && typeof options === 'object' && options.withFileTypes) {
          return fsReaddirWithTypesAsync(path, entries);
        }
        return entries;
      },
      async mkdir(path, options) {
        return fsCallAsync(fsRequest('mkdir', path, {recursive: Boolean(options?.recursive)}));
      },
      async unlink(path) { return fsCallAsync(fsRequest('unlink', path)); },
      async symlink(target, path, type) {
        return fsCallAsync(fsSymlinkRequest(target, path, type));
      },
      async readlink(path, options) {
        const encoding = fsReadlinkEncoding(options);
        return fsReadlinkResult(await fsCallAsync(fsReadlinkRequest(path)), encoding);
      },
      async rm(path, options) {
        return fsCallAsync(fsRequest('rm', path, {
          recursive: Boolean(options?.recursive), force: Boolean(options?.force),
        }));
      },
      async access(path, mode = 0) {
        return fsCallAsync(fsAccessRequest(path, mode));
      },
      async realpath(path) { return fsCallAsync(fsRequest('realpath', path)); },
      async copyFile(src, dest, flags = 0) {
        if (flags !== 0) return fsUnsupported('copyFile with flags');
        return fsCallAsync(fsRequest('copy_file', src, {destination: fsPathArgument(dest, 'destination')}));
      },
      async rename(oldPath, newPath) {
        return fsCallAsync(fsRequest('rename', oldPath, {destination: fsPathArgument(newPath, 'destination')}));
      },
      async open() { return fsUnsupported('promises.open'); },
      async chmod(path, mode) { await fsCallAsync(fsChmodRequest(path, mode)); },
      async chown() { return fsUnsupported('promises.chown'); },
    };

    const fsModule = {
      Stats,
      Dirent,
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
      appendFileSync(path, data, options) {
        return fsCallSync(fsWriteRequest(path, options, 'a'), fsWriteData(data, options));
      },
      statSync(path, options) {
        const bigint = options?.bigint === true;
        return fsStatResult(fsCallSync(fsRequest('stat', path, {bigint})), bigint);
      },
      lstatSync(path, options) {
        const bigint = options?.bigint === true;
        return fsStatResult(fsCallSync(fsRequest('lstat', path, {bigint})), bigint);
      },
      readdirSync(path, options) {
        const entries = fsCallSync(fsRequest('readdir', path));
        if (options && typeof options === 'object' && options.withFileTypes) {
          return fsReaddirWithTypesSync(path, entries);
        }
        return entries;
      },
      mkdirSync(path, options) {
        return fsCallSync(fsRequest('mkdir', path, {recursive: Boolean(options?.recursive)}));
      },
      unlinkSync(path) { return fsCallSync(fsRequest('unlink', path)); },
      symlinkSync(target, path, type) {
        return fsCallSync(fsSymlinkRequest(target, path, type));
      },
      readlinkSync(path, options) {
        const encoding = fsReadlinkEncoding(options);
        return fsReadlinkResult(fsCallSync(fsReadlinkRequest(path)), encoding);
      },
      rmdirSync(path, options) {
        return fsCallSync(fsRequest('rmdir', path, {recursive: Boolean(options?.recursive)}));
      },
      rmSync(path, options) {
        return fsCallSync(fsRequest('rm', path, {
          recursive: Boolean(options?.recursive), force: Boolean(options?.force),
        }));
      },
      accessSync(path, mode = 0) {
        return fsCallSync(fsAccessRequest(path, mode));
      },
      readFile(path, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.readFile(path, options));
      },
      writeFile(path, data, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.writeFile(path, data, options));
      },
      appendFile(path, data, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.appendFile(path, data, options));
      },
      stat(path, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.stat(path, options));
      },
      lstat(path, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.lstat(path, options));
      },
      readdir(path, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.readdir(path, options));
      },
      mkdir(path, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.mkdir(path, options));
      },
      unlink(path, callback) { fsCallback(callback, () => fsPromises.unlink(path)); },
      symlink(target, path, type, callback) {
        if (typeof type === 'function') { callback = type; type = undefined; }
        fsCallback(callback, () => fsPromises.symlink(target, path, type));
      },
      readlink(path, options, callback) {
        if (typeof options === 'function') { callback = options; options = undefined; }
        fsCallback(callback, () => fsPromises.readlink(path, options));
      },
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
        fsValidateCallback(callback);
        const request = fsAccessRequest(path, mode);
        fsCallAsync(request).then(() => callback(null), error => callback(error));
      },
      chmodSync(path, mode) { fsCallSync(fsChmodRequest(path, mode)); },
      chmod(path, mode, callback) {
        const request = fsChmodRequest(path, mode);
        fsValidateCallback(callback);
        fsCallAsync(request).then(() => callback(null), error => callback(error));
      },
      exists(path, callback) {
        fsValidateCallback(callback);
        fsCallback((error, value) => callback(!error && Boolean(value)),
          async () => fsCallAsync(fsRequest('exists', path)));
      },
      copyFile(src, dest, flags, callback) {
        if (typeof flags === 'function') { callback = flags; flags = undefined; }
        fsCallback(callback, () => fsPromises.copyFile(src, dest, flags));
      },
      copyFileSync(src, dest, flags = 0) {
        if (flags !== 0) return fsUnsupported('copyFileSync with flags');
        return fsCallSync(fsRequest('copy_file', src, {destination: fsPathArgument(dest, 'destination')}));
      },
      rename(oldPath, newPath, callback) {
        fsCallback(callback, () => fsPromises.rename(oldPath, newPath));
      },
      renameSync(oldPath, newPath) {
        return fsCallSync(fsRequest('rename', oldPath, {destination: fsPathArgument(newPath, 'destination')}));
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
      'chownSync', 'openSync', 'closeSync', 'readSync', 'writeSync',
    ]) {
      fsModule[method] = () => fsUnsupported(method);
    }
    for (const method of ['chown', 'open', 'close', 'read', 'write']) {
      fsModule[method] = (...args) => fsCallback(args[args.length - 1],
        async () => fsUnsupported(method));
    }

    // Read the requested range through the worker bridge, then deliver bounded
    // chunks. No OS descriptor or fake open event is exposed.
    installFileStreams(fsModule, (path, start, end) => {
      const range = {returnBytes: true, start};
      if (end !== Infinity) range.end = end;
      return fsCallAsync(fsRequest('read_file', path, range));
    }, fsUnsupported);

    if (typeof installNativeFileDescriptors === 'function') {
      installNativeFileDescriptors(fsModule, {sync: fsCallSync,
        asynchronous: fsCallAsync, path: fsPathArgument, stats: fsStatResult});
    }
    installMkdtemp(fsModule, {Buffer, path: fsPathArgument,
      sync: fsCallSync, asynchronous: fsCallAsync});
    return fsModule;
  }
