  // These type bits are the portable Node/POSIX stat contract. Permissions and
  // other metadata must come from the filesystem, never inferred from a type.
  const fsStatTypes = {
    S_IFMT: 0o170000, S_IFREG: 0o100000, S_IFDIR: 0o040000,
    S_IFCHR: 0o020000, S_IFBLK: 0o060000, S_IFIFO: 0o010000,
    S_IFLNK: 0o120000, S_IFSOCK: 0o140000,
  };
  const fsStatTypeConstants = {...fsStatTypes};
  if (osPlatform === 'win32') {
    delete fsStatTypeConstants.S_IFBLK;
    delete fsStatTypeConstants.S_IFSOCK;
  }
  function createFsStatsApi() {
    const fsStatsTypeHints = new WeakMap();

    function Stats(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks,
                   atimeMs, mtimeMs, ctimeMs, birthtimeMs) {
      Object.assign(this, {dev, mode, nlink, uid, gid, rdev, blksize, ino, size,
        blocks, atimeMs, mtimeMs, ctimeMs, birthtimeMs});
    }

    function exactStatInteger(value, field) {
      if (value === undefined) return undefined;
      if (typeof value !== 'string' || !/^-?\d+$/.test(value)) {
        throw Object.assign(new Error('Filesystem did not provide an exact integer for ' + field),
                            {code: 'ERR_IPC_INVALID_RESPONSE'});
      }
      return BigInt(value);
    }

    function BigIntStats(stat) {
      for (const field of ['dev', 'mode', 'nlink', 'uid', 'gid', 'rdev',
        'blksize', 'ino', 'size', 'blocks']) {
        this[field] = exactStatInteger(stat[field], field);
      }
      const times = ['atime', 'mtime', 'ctime', 'birthtime'];
      const nanos = times.map(field => {
        if (stat[field + 'Ns'] === undefined && stat[field + 'Ms'] !== undefined) {
          throw Object.assign(new Error('Filesystem did not provide exact nanoseconds for ' + field),
                              {code: 'ERR_IPC_INVALID_RESPONSE'});
        }
        return exactStatInteger(stat[field + 'Ns'], field + 'Ns');
      });
      times.forEach((field, index) => {
        this[field + 'Ms'] = nanos[index] === undefined ? undefined : nanos[index] / 1000000n;
      });
      times.forEach((field, index) => { this[field + 'Ns'] = nanos[index]; });
    }

    Stats.prototype._checkModeProperty = function(type) {
      if (osPlatform === 'win32' && (type === fsStatTypes.S_IFBLK ||
          type === fsStatTypes.S_IFIFO || type === fsStatTypes.S_IFSOCK)) return false;
      if (typeof this.mode === 'bigint')
        return (this.mode & BigInt(fsStatTypes.S_IFMT)) === BigInt(type);
      if (this.mode !== undefined) return (this.mode & fsStatTypes.S_IFMT) === type;
      // Older transports and virtual mounts can provide a known type without
      // exposing permissions or inode metadata. Do not invent a numeric mode.
      return fsStatsTypeHints.get(this)?.[type] === true;
    };
    BigIntStats.prototype._checkModeProperty = Stats.prototype._checkModeProperty;
    for (const [method, type] of Object.entries({
      isFile: fsStatTypes.S_IFREG,
      isDirectory: fsStatTypes.S_IFDIR,
      isCharacterDevice: fsStatTypes.S_IFCHR,
      isBlockDevice: fsStatTypes.S_IFBLK,
      isFIFO: fsStatTypes.S_IFIFO,
      isSymbolicLink: fsStatTypes.S_IFLNK,
      isSocket: fsStatTypes.S_IFSOCK,
    })) {
      Stats.prototype[method] = function() { return this._checkModeProperty(type); };
      BigIntStats.prototype[method] = Stats.prototype[method];
    }
    for (const constructor of [Stats, BigIntStats]) {
      for (const field of ['atime', 'mtime', 'ctime', 'birthtime']) {
        Object.defineProperty(constructor.prototype, field, {
          enumerable: true,
          configurable: true,
          get() {
            // Node rounds sub-millisecond stat times for the legacy Date fields.
            // An unavailable timestamp remains Invalid Date, rather than epoch 0.
            const value = new Date(Math.round(Number(this[field + 'Ms'])));
            Object.defineProperty(this, field, {
              value, enumerable: true, configurable: true, writable: true,
            });
            return value;
          },
          set(value) {
            Object.defineProperty(this, field, {
              value, enumerable: true, configurable: true, writable: true,
            });
          },
        });
      }
    }

    function createFsStats(stat, bigint = false) {
      const result = bigint ? new BigIntStats(stat) : new Stats(
          stat.dev, stat.mode, stat.nlink, stat.uid, stat.gid,
          stat.rdev, stat.blksize, stat.ino, stat.size, stat.blocks,
          stat.atimeMs, stat.mtimeMs, stat.ctimeMs, stat.birthtimeMs);
      fsStatsTypeHints.set(result, {
        [fsStatTypes.S_IFREG]: stat.isFile,
        [fsStatTypes.S_IFDIR]: stat.isDirectory,
        [fsStatTypes.S_IFCHR]: stat.isCharacterDevice,
        [fsStatTypes.S_IFBLK]: stat.isBlockDevice,
        [fsStatTypes.S_IFIFO]: stat.isFIFO,
        [fsStatTypes.S_IFLNK]: stat.isSymbolicLink,
        [fsStatTypes.S_IFSOCK]: stat.isSocket,
      });
      return result;
    }
    return {Stats, createFsStats};
  }
  const {Stats, createFsStats} = createFsStatsApi();
