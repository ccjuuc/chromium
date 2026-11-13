  // Main-only descriptor operations. Handles are owned by the native runtime,
  // including when an asynchronous operation outlives the JavaScript caller.
  const nativeFileHandles = new WeakSet();
  function installNativeFileDescriptors(fs, {sync, asynchronous, path, stats}) {
    const invalid = (message, code = 'ERR_INVALID_ARG_TYPE') =>
      Object.assign(new TypeError(message), {code});
    const integer = (value, name, maximum = Number.MAX_SAFE_INTEGER) => {
      if (!Number.isSafeInteger(value) || value < 0 || value > maximum)
        throw invalid(`${name} is out of range`, 'ERR_OUT_OF_RANGE');
      return value;
    };
    const request = (operation, fd, extra = {}) =>
      ({operation: 'fd_' + operation, fd: integer(fd, 'fd', 0x7fffffff), ...extra});
    const mode = value => {
      if (typeof value === 'string' && /^[0-7]+$/.test(value)) value = parseInt(value, 8);
      return integer(value, 'mode', 0xffffffff);
    };
    const position = value => value == null ? null : value === -1 ? -1 : integer(value, 'position');
    const callback = value => {
      if (typeof value !== 'function') throw invalid('callback must be a function');
      return value;
    };
    const openRequest = (name, flags = 'r', permissions = 0o666) => {
      if (typeof flags !== 'string' && !Number.isInteger(flags)) throw invalid('flags must be a string or integer');
      if (typeof flags === 'string' && !new Set([
        'r', 'r+', 'rs', 'sr', 'rs+', 'sr+', 'w', 'w+', 'wx', 'xw', 'wx+', 'xw+',
        'a', 'a+', 'ax', 'xa', 'ax+', 'xa+', 'as', 'sa', 'as+', 'sa+',
      ]).has(flags)) throw invalid('Unknown file open flag', 'ERR_INVALID_ARG_VALUE');
      return {operation: 'fd_open', path: path(name, 'path'), flags, mode: mode(permissions)};
    };
    const bufferView = value => {
      if (!ArrayBuffer.isView(value)) throw invalid('buffer must be an ArrayBuffer view');
      return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    };
    function readRequest(fd, buffer, offset, length, at) {
      if (buffer && !ArrayBuffer.isView(buffer) && typeof buffer === 'object') {
        ({buffer = Buffer.alloc(16384), offset = 0, length, position: at = null} = buffer);
      } else if (offset && typeof offset === 'object') {
        ({offset = 0, length, position: at = null} = offset);
      }
      if (buffer === undefined) buffer = Buffer.alloc(16384);
      const bytes = bufferView(buffer);
      offset = integer(offset ?? 0, 'offset', bytes.byteLength);
      length = integer(length ?? bytes.byteLength - offset, 'length', bytes.byteLength - offset);
      return {buffer, bytes: bytes.subarray(offset, offset + length),
        request: request('read', fd, {length, position: position(at)})};
    }
    function writeRequest(fd, data, offset, length, at) {
      let bytes;
      if (typeof data === 'string') {
        bytes = Buffer.from(data, typeof length === 'string' ? length : 'utf8');
        at = typeof offset === 'number' ? offset : null;
      } else {
        bytes = bufferView(data);
        if (offset && typeof offset === 'object')
          ({offset = 0, length, position: at = null} = offset);
        offset = integer(offset ?? 0, 'offset', bytes.byteLength);
        length = integer(length ?? bytes.byteLength - offset, 'length', bytes.byteLength - offset);
        bytes = bytes.subarray(offset, offset + length);
      }
      return {data, bytes, request: request('write', fd, {position: position(at)})};
    }
    function copyRead(read, value) {
      const bytes = Buffer.from(value);
      if (bytes.length > read.bytes.length)
        throw Object.assign(new Error('Native read exceeded the requested range'), {code: 'ERR_IPC_INVALID_RESPONSE'});
      read.bytes.set(bytes);
      return bytes.length;
    }
    fs.openSync = (name, flags, permissions) => sync(openRequest(name, flags, permissions));
    fs.open = (name, flags, permissions, done) => {
      if (typeof permissions === 'function') { done = permissions; permissions = undefined; }
      if (typeof flags === 'function') { done = flags; flags = undefined; }
      callback(done);
      const options = openRequest(name, flags, permissions);
      asynchronous(options).then(value => done(null, value), done);
    };
    fs.readSync = (fd, buffer, offset, length, at) => {
      bufferView(buffer);
      const read = readRequest(fd, buffer, offset, length, at);
      if (!read.bytes.length) return 0;
      return copyRead(read, sync(read.request));
    };
    fs.read = (fd, ...args) => {
      const done = callback(args.pop());
      const read = readRequest(fd, ...args);
      (read.bytes.length ? asynchronous(read.request) : Promise.resolve([]))
        .then(value => done(null, copyRead(read, value), read.buffer), done);
    };
    fs.writeSync = (fd, data, offset, length, at) => {
      const write = writeRequest(fd, data, offset, length, at);
      return sync(write.request, write.bytes);
    };
    fs.write = (fd, ...args) => {
      const done = callback(args.pop());
      const write = writeRequest(fd, ...args);
      asynchronous(write.request, Buffer.from(write.bytes)).then(value => done(null, value, write.data), done);
    };
    for (const name of ['close', 'fsync', 'fdatasync']) {
      fs[name + 'Sync'] = fd => sync(request(name, fd));
      fs[name] = (fd, done) => {
        callback(done);
        asynchronous(request(name, fd)).then(() => done(null), done);
      };
    }
    fs.fstatSync = (fd, options) => {
      const bigint = options?.bigint === true;
      return stats(sync(request('fstat', fd, {bigint})), bigint);
    };
    fs.fstat = (fd, options, done) => {
      if (typeof options === 'function') { done = options; options = undefined; }
      callback(done);
      const bigint = options?.bigint === true;
      asynchronous(request('fstat', fd, {bigint})).then(value => done(null, stats(value, bigint)), done);
    };
    for (const [name, field, validate, defaultValue] of [
      ['ftruncate', 'length', value => {
        if (!Number.isSafeInteger(value)) throw invalid('length is out of range', 'ERR_OUT_OF_RANGE');
        return Math.max(0, value);
      }, 0],
      ['fchmod', 'mode', mode, undefined],
    ]) {
      fs[name + 'Sync'] = (fd, value = defaultValue) => sync(request(name, fd, {[field]: validate(value)}));
      fs[name] = (fd, value, done) => {
        if (typeof value === 'function') { done = value; value = defaultValue; }
        callback(done);
        asynchronous(request(name, fd, {[field]: validate(value)})).then(() => done(null), done);
      };
    }
    function decode(bytes, options) {
      const encoding = typeof options === 'string' ? options : options?.encoding;
      const result = Buffer.concat(bytes);
      return encoding ? result.toString(encoding) : result;
    }
    const readFileSync = fs.readFileSync;
    fs.readFileSync = (name, options) => {
      if (typeof name !== 'number') return readFileSync(name, options);
      const pieces = [];
      for (;;) {
        const piece = Buffer.from(sync(request('read', name, {length: 65536, position: null})));
        if (!piece.length) return decode(pieces, options);
        pieces.push(piece);
      }
    };
    async function readFileDescriptor(fd, options) {
      const pieces = [];
      for (;;) {
        const piece = Buffer.from(await asynchronous(request('read', fd, {length: 65536, position: null})));
        if (!piece.length) return decode(pieces, options);
        pieces.push(piece);
      }
    }
    const readFile = fs.readFile;
    fs.readFile = (name, options, done) => {
      if (typeof name !== 'number') return readFile(name, options, done);
      if (typeof options === 'function') { done = options; options = undefined; }
      callback(done);
      readFileDescriptor(name, options).then(value => done(null, value), done);
    };
    function writeFileBytes(data, options) {
      return typeof data === 'string' ? Buffer.from(data,
        typeof options === 'string' ? options : options?.encoding) : Buffer.from(bufferView(data));
    }
    const writeFileSync = fs.writeFileSync;
    fs.writeFileSync = (name, data, options) => {
      if (typeof name !== 'number') return writeFileSync(name, data, options);
      const bytes = writeFileBytes(data, options);
      for (let offset = 0; offset < bytes.length;) {
        const count = sync(request('write', name, {position: null}), bytes.subarray(offset, offset + 1024 * 1024));
        if (!Number.isInteger(count) || count <= 0) throw Object.assign(new Error('File write made no progress'), {code: 'EIO'});
        offset += count;
      }
    };
    async function writeFileDescriptor(fd, data, options) {
      const bytes = writeFileBytes(data, options);
      for (let offset = 0; offset < bytes.length;) {
        const count = await asynchronous(request('write', fd, {position: null}), bytes.subarray(offset, offset + 1024 * 1024));
        if (!Number.isInteger(count) || count <= 0) throw Object.assign(new Error('File write made no progress'), {code: 'EIO'});
        offset += count;
      }
    }
    const writeFile = fs.writeFile;
    fs.writeFile = (name, data, options, done) => {
      if (typeof name !== 'number') return writeFile(name, data, options, done);
      if (typeof options === 'function') { done = options; options = undefined; }
      callback(done);
      writeFileDescriptor(name, data, options).then(() => done(null), done);
    };
    class FileHandle extends EventEmitter {
      #fd;
      #pending = new Set();
      #closing;
      constructor(fd) { super(); this.#fd = fd; nativeFileHandles.add(this); }
      get fd() { return this.#fd; }
      #run(operation) {
        if (this.#closing || this.#fd < 0)
          return Promise.reject(Object.assign(new Error('File is closed'), {code: 'EBADF'}));
        const fd = this.#fd;
        const task = Promise.resolve().then(() => operation(fd));
        this.#pending.add(task);
        task.then(() => this.#pending.delete(task), () => this.#pending.delete(task));
        return task;
      }
      read(...args) {
        return this.#run(async fd => {
          const read = readRequest(fd, ...args);
          return {bytesRead: copyRead(read, read.bytes.length ?
            await asynchronous(read.request) : []), buffer: read.buffer};
        });
      }
      write(...args) {
        // Copy upload bytes before callers can change their input view.
        let write;
        try { write = writeRequest(this.#fd, ...args); write.bytes = Buffer.from(write.bytes); }
        catch (error) { return Promise.reject(error); }
        return this.#run(async () => ({bytesWritten: await asynchronous(write.request, write.bytes), buffer: write.data}));
      }
      stat(options) {
        const bigint = options?.bigint === true;
        return this.#run(async fd => {
          return stats(await asynchronous(request('fstat', fd, {bigint})), bigint);
        });
      }
      sync() { return this.#run(fd => asynchronous(request('fsync', fd))); }
      datasync() { return this.#run(fd => asynchronous(request('fdatasync', fd))); }
      truncate(length = 0) {
        return this.#run(fd => {
          if (!Number.isSafeInteger(length)) throw invalid('length is out of range', 'ERR_OUT_OF_RANGE');
          return asynchronous(request('ftruncate', fd, {length: Math.max(0, length)}));
        });
      }
      chmod(value) { return this.#run(fd => asynchronous(request('fchmod', fd, {mode: mode(value)}))); }
      readFile(options) { return this.#run(fd => readFileDescriptor(fd, options)); }
      writeFile(data, options) { return this.#run(fd => writeFileDescriptor(fd, data, options)); }
      close() {
        if (!this.#closing) {
          const fd = this.#fd;
          this.#fd = -1;
          this.#closing = (async () => {
            await Promise.allSettled([...this.#pending]);
            await asynchronous(request('close', fd));
            this.emit('close');
          })();
        }
        return this.#closing;
      }
      [Symbol.asyncDispose]() { return this.close(); }
    }
    fs.promises.open = async (name, flags, permissions) =>
      new FileHandle(await asynchronous(openRequest(name, flags, permissions)));
    // Promise readFile/writeFile accept FileHandle, not numeric descriptors.
    const promiseReadFile = fs.promises.readFile;
    const promiseWriteFile = fs.promises.writeFile;
    fs.promises.readFile = (name, options) => nativeFileHandles.has(name) ?
      name.readFile(options) : promiseReadFile(name, options);
    fs.promises.writeFile = (name, data, options) => nativeFileHandles.has(name) ?
      name.writeFile(data, options) : promiseWriteFile(name, data, options);
  }
