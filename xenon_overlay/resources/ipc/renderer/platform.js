  // --- 3. Path Module (Win32 & POSIX) ---
  function validatePath(path, name = 'path') {
    if (typeof path !== 'string') {
      throw Object.assign(new TypeError(`${name} must be a string`),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    return path;
  }

  function pathRoot(path, windows) {
    if (!windows) return {end: path.startsWith('/') ? 1 : 0, device: '',
      absolute: path.startsWith('/')};
    const drive = /^[a-zA-Z]:/.exec(path);
    if (drive) {
      const absolute = /^[\\/]/.test(path.slice(2));
      return {end: absolute ? 3 : 2, device: drive[0], absolute};
    }
    const unc = /^[\\/]{2}([^\\/]+)[\\/]+([^\\/]+)/.exec(path);
    if (unc) return {end: unc[0].length, device: `\\\\${unc[1]}\\${unc[2]}`,
      absolute: true};
    const absolute = /^[\\/]/.test(path);
    return {end: absolute ? 1 : 0, device: '', absolute};
  }

  function normalizedPathTail(path, absolute, windows) {
    const parts = [];
    for (const part of path.split(windows ? /[\\/]/ : '/')) {
      if (!part || part === '.') continue;
      if (part === '..' && parts.length && parts[parts.length - 1] !== '..') {
        parts.pop();
      } else if (part !== '..' || !absolute) {
        parts.push(part);
      }
    }
    return parts.join(windows ? '\\' : '/');
  }

  function createPathModule(windows) {
    const sep = windows ? '\\' : '/';
    const isSep = value => value === '/' || (windows && value === '\\');
    const module = {
      sep,
      delimiter: windows ? ';' : ':',
      isAbsolute(path) {
        return pathRoot(validatePath(path), windows).absolute;
      },
      normalize(path) {
        validatePath(path);
        if (!path) return '.';
        const root = pathRoot(path, windows);
        if (windows && /^\\\\[?.]\\/.test(root.device)) {
          root.device = root.device.slice(0, 3);
          root.end = 4;
        }
        let tail = normalizedPathTail(path.slice(root.end), root.absolute, windows);
        if (!tail && !root.absolute) tail = '.';
        if (tail && isSep(path[path.length - 1])) tail += sep;
        // Normalization must not turn a relative component into a drive path.
        if (windows && !root.device && !root.absolute && path.includes(':') &&
            (/^[a-zA-Z]:/.test(tail) || /:(?:[\\/]|$)/.test(path))) return '.\\' + tail;
        return root.device + (root.absolute ? sep : '') + tail;
      },
      join(...paths) {
        for (const path of paths) validatePath(path);
        const nonempty = paths.filter(Boolean);
        if (!nonempty.length) return '.';
        let joined = nonempty.join(sep);
        // Only an intentional server component in the first argument starts
        // a UNC path. Joining separators must not manufacture a server name.
        if (windows && !/^[\\/]{2}[^\\/]/.test(nonempty[0])) {
          joined = joined.replace(/^[\\/]{2,}/, '\\');
        }
        return module.normalize(joined);
      },
      resolve(...paths) {
        let device = '', tail = '', absolute = false;
        for (let i = paths.length - 1; i >= -1; --i) {
          let path;
          if (i >= 0) {
            path = validatePath(paths[i], `paths[${i}]`);
            if (!path) continue;
          } else {
            path = process.cwd();
            if (windows && device) {
              path = process.env[`=${device}`] || path;
              const cwdRoot = pathRoot(path, true);
              if (cwdRoot.device && cwdRoot.device.toLowerCase() !== device.toLowerCase()) {
                path = device + '\\';
              }
            } else if (!windows && runtimePlatform === 'win32') {
              path = path.replace(/\\/g, '/');
              path = path.slice(path.indexOf('/'));
            }
          }
          const root = pathRoot(path, windows);
          if (windows && /^\\\\[?.]\\/.test(root.device)) {
            root.device = root.device.slice(0, 3);
            root.end = 4;
          }
          if (root.device) {
            if (device && root.device.toLowerCase() !== device.toLowerCase()) continue;
            device = root.device;
          }
          if (!absolute) {
            tail = path.slice(root.end) + sep + tail;
            absolute = root.absolute;
          }
          if (absolute && (!windows || device)) break;
        }
        tail = normalizedPathTail(tail, absolute, windows);
        return device + (absolute ? sep : '') + tail || '.';
      },
      dirname(path) {
        validatePath(path);
        if (!path) return '.';
        const root = pathRoot(path, windows);
        let rootEnd = root.end;
        if (windows && root.device.startsWith('\\\\') && isSep(path[rootEnd])) ++rootEnd;
        let end = path.length - 1;
        while (end >= rootEnd && isSep(path[end])) --end;
        while (end >= rootEnd && !isSep(path[end])) --end;
        if (end < rootEnd) return path.slice(0, rootEnd) || '.';
        if (!windows && end === 1 && path.startsWith('/')) return '//';
        return path.slice(0, end);
      },
      basename(path, suffix) {
        validatePath(path);
        if (suffix !== undefined) validatePath(suffix, 'suffix');
        const start = windows && /^[a-zA-Z]:/.test(path) ? 2 : 0;
        let end = path.length;
        while (end > start && isSep(path[end - 1])) --end;
        let begin = end;
        while (begin > start && !isSep(path[begin - 1])) --begin;
        let base = path.slice(begin, end);
        if (suffix && base.endsWith(suffix) && (base !== suffix || path === suffix)) {
          base = base.slice(0, -suffix.length);
        }
        return base;
      },
      extname(path) {
        const base = module.basename(path);
        const dot = base.lastIndexOf('.');
        return dot <= 0 || base === '..' ? '' : base.slice(dot);
      },
      parse(path) {
        validatePath(path);
        const parsedRoot = pathRoot(path, windows);
        let rootEnd = parsedRoot.end;
        if (windows && parsedRoot.device.startsWith('\\\\') && isSep(path[rootEnd])) ++rootEnd;
        const root = path.slice(0, rootEnd);
        let end = path.length;
        while (end > rootEnd && isSep(path[end - 1])) --end;
        let begin = end;
        while (begin > rootEnd && !isSep(path[begin - 1])) --begin;
        const base = path.slice(begin, end);
        const dot = base.lastIndexOf('.');
        const ext = dot <= 0 || (base === '..' && (windows || begin !== 1)) ? '' : base.slice(dot);
        return {root, dir: begin > rootEnd ? path.slice(0, begin - 1) : root,
          base, ext, name: ext ? base.slice(0, -ext.length) : base};
      },
      format(obj) {
        if (obj === null || typeof obj !== 'object') {
          throw Object.assign(new TypeError('pathObject must be an object'),
                              {code: 'ERR_INVALID_ARG_TYPE'});
        }
        const dir = obj.dir || obj.root;
        const ext = obj.ext && !obj.ext.startsWith('.') ? '.' + obj.ext : (obj.ext || '');
        const base = obj.base || (obj.name || '') + ext;
        return !dir ? base : dir === obj.root ? dir + base : dir + sep + base;
      },
      relative(from, to) {
        validatePath(from, 'from');
        validatePath(to, 'to');
        if (from === to) return '';
        from = module.resolve(from);
        to = module.resolve(to);
        const compare = value => windows ? value.toLowerCase() : value;
        const fromRoot = pathRoot(from, windows), toRoot = pathRoot(to, windows);
        if (compare(fromRoot.device) !== compare(toRoot.device)) return to;
        const source = from.slice(fromRoot.end).split(sep).filter(Boolean);
        const target = to.slice(toRoot.end).split(sep).filter(Boolean);
        let common = 0;
        while (common < source.length && common < target.length &&
               compare(source[common]) === compare(target[common])) ++common;
        return [...source.slice(common).map(() => '..'), ...target.slice(common)].join(sep);
      },
      toNamespacedPath(path) {
        if (!windows || typeof path !== 'string' || !path) return path;
        const resolved = module.resolve(path);
        if (resolved.startsWith('\\\\') && !/^[?.]$/.test(resolved[2])) {
          return '\\\\?\\UNC\\' + resolved.slice(2);
        }
        return /^[a-zA-Z]:\\/.test(resolved) ? '\\\\?\\' + resolved : path;
      },
    };
    module._makeLong = module.toNamespacedPath;
    return module;
  }

  const win32 = createPathModule(true);
  const posix = createPathModule(false);
  win32.win32 = posix.win32 = win32;
  win32.posix = posix.posix = posix;
  const pathModule = runtimePlatform === 'win32' ? win32 : posix;

  // --- 4. OS Module ---
  const osPlatform = runtimePlatform;
  const osArch = runtimeArch;
  const osEndianness = injectedPaths.endianness;
  const osNativeCall = request => transport.sendSync('__xenon:os', request);
  function osQuery(method) {
    try {
      return osNativeCall({method});
    } catch (error) {
      // Private IPC preserves the native message; restore its Node error code.
      const match = /^(ERR_[A-Z_]+|E[A-Z0-9_]+):/.exec(String(error?.message || error));
      if (match && !error.code) error.code = match[1];
      throw error;
    }
  }

  function osUserInfo(options) {
    // Node treats absent/unrecognized encodings as UTF-8.
    const requested = options?.encoding;
    const encoding = typeof requested === 'string' ? requested.toLowerCase() : 'utf8';
    const result = osQuery('userInfo');
    if (!['buffer', 'hex', 'base64', 'base64url', 'ascii', 'latin1', 'binary',
          'utf16le', 'utf-16le', 'ucs2', 'ucs-2'].includes(encoding)) {
      return result;
    }
    for (const key of ['username', 'homedir', 'shell']) {
      if (result[key] === null) continue;
      const bytes = Buffer.from(result[key], 'utf8');
      if (encoding === 'buffer') {
        result[key] = bytes;
      } else if (['hex', 'base64', 'base64url'].includes(encoding)) {
        result[key] = bytes.toString(encoding);
      } else if (['ascii', 'latin1', 'binary'].includes(encoding)) {
        result[key] = Array.from(bytes, byte =>
            String.fromCharCode(encoding === 'ascii' ? byte & 0x7f : byte)).join('');
      } else if (['utf16le', 'utf-16le', 'ucs2', 'ucs-2'].includes(encoding)) {
        // Decode pairs directly to preserve lone UTF-16 code units, as Buffer does.
        let value = '';
        for (let i = 0; i + 1 < bytes.length; i += 2) {
          value += String.fromCharCode(bytes[i] | (bytes[i + 1] << 8));
        }
        result[key] = value;
      }
    }
    return result;
  }

  const osModule = {
    platform: () => osPlatform,
    arch: () => osArch,
    endianness() {
      if (osEndianness !== 'LE' && osEndianness !== 'BE') {
        throw Object.assign(new Error('OS byte order metadata is unavailable'),
                            {code: 'ERR_NOT_SUPPORTED'});
      }
      return osEndianness;
    },
    homedir() {
      const value = globalThis.process.env[osPlatform === 'win32' ? 'USERPROFILE' : 'HOME'];
      return value === undefined ? osQuery('homedir') : String(value);
    },
    tmpdir() {
      const env = globalThis.process.env;
      const value = osPlatform === 'win32' ? env.TEMP || env.TMP :
          env.TMPDIR || env.TMP || env.TEMP;
      const directory = value ? String(value) : osQuery('tmpdir');
      if (osPlatform === 'win32') {
        return directory.length > 1 && directory.endsWith('\\') &&
            !directory.endsWith(':\\') ? directory.slice(0, -1) : directory;
      }
      return directory.length > 1 && directory.endsWith('/') ?
          directory.slice(0, -1) : directory;
    },
    userInfo: osUserInfo,
    EOL: osPlatform === 'win32' ? '\r\n' : '\n',
    devNull: osPlatform === 'win32' ? '\\\\.\\nul' : '/dev/null',
  };
  // Query mutable system state on every call. Requiring os never enumerates
  // CPUs, users or interfaces, and never adds a synchronous startup round trip.
  for (const method of ['type', 'release', 'version', 'machine', 'hostname',
                        'cpus', 'totalmem', 'freemem', 'uptime', 'loadavg',
                        'networkInterfaces', 'availableParallelism',
                        'getPriority', 'setPriority']) {
    osModule[method] = () => osQuery(method);
  }
