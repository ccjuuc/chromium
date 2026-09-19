  function decodeFormComponent(value) {
    try {
      return decodeURIComponent(String(value).replace(/\+/g, ' '));
    } catch {
      return String(value).replace(/\+/g, ' ');
    }
  }
  function encodeFormComponent(value) {
    return encodeURIComponent(String(value))
        .replace(/%20/g, '+')
        .replace(/!/g, '%21')
        .replace(/'/g, '%27')
        .replace(/\(/g, '%28')
        .replace(/\)/g, '%29')
        .replace(/~/g, '%7E');
  }
  class URLSearchParams {
    constructor(init = '', onUpdate = null) {
      this._pairs = [];
      this._onUpdate = typeof onUpdate === 'function' ? onUpdate : null;
      this._replace(init);
    }
    _replace(init) {
      this._pairs = [];
      if (init instanceof URLSearchParams) {
        this._pairs = init._pairs.map(pair => [...pair]);
      } else if (typeof init === 'string') {
        const value = init.startsWith('?') ? init.slice(1) : init;
        for (const field of value.split('&')) {
          if (!field) continue;
          const separator = field.indexOf('=');
          const name = separator < 0 ? field : field.slice(0, separator);
          const entryValue = separator < 0 ? '' : field.slice(separator + 1);
          this._pairs.push([
            decodeFormComponent(name), decodeFormComponent(entryValue)]);
        }
      } else if (init && typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) {
          if (!pair || typeof pair[Symbol.iterator] !== 'function') {
            throw new TypeError('Each query pair must be iterable');
          }
          const values = [...pair];
          if (values.length !== 2) {
            throw new TypeError('Each query pair must contain two items');
          }
          this._pairs.push([String(values[0]), String(values[1])]);
        }
      } else if (init && typeof init === 'object') {
        for (const [name, value] of Object.entries(init)) {
          this._pairs.push([String(name), String(value)]);
        }
      }
    }
    _changed() {
      if (this._onUpdate) this._onUpdate(this.toString());
    }
    append(name, value) {
      this._pairs.push([String(name), String(value)]);
      this._changed();
    }
    delete(name, value) {
      const key = String(name);
      const hasValue = arguments.length > 1;
      const expected = String(value);
      this._pairs = this._pairs.filter(
          pair => pair[0] !== key || (hasValue && pair[1] !== expected));
      this._changed();
    }
    get(name) {
      const key = String(name);
      const pair = this._pairs.find(item => item[0] === key);
      return pair ? pair[1] : null;
    }
    getAll(name) {
      const key = String(name);
      return this._pairs.filter(item => item[0] === key).map(item => item[1]);
    }
    has(name, value) {
      const key = String(name);
      const hasValue = arguments.length > 1;
      const expected = String(value);
      return this._pairs.some(
          pair => pair[0] === key && (!hasValue || pair[1] === expected));
    }
    set(name, value) {
      const key = String(name);
      const entryValue = String(value);
      const first = this._pairs.findIndex(item => item[0] === key);
      if (first < 0) {
        this._pairs.push([key, entryValue]);
      } else {
        this._pairs[first][1] = entryValue;
        this._pairs = this._pairs.filter(
            (item, index) => item[0] !== key || index === first);
      }
      this._changed();
    }
    sort() {
      this._pairs.sort((left, right) => left[0].localeCompare(right[0]));
      this._changed();
    }
    entries() { return this._pairs.map(pair => [...pair])[Symbol.iterator](); }
    keys() { return this._pairs.map(pair => pair[0])[Symbol.iterator](); }
    values() { return this._pairs.map(pair => pair[1])[Symbol.iterator](); }
    forEach(callback, thisArg) {
      for (const [name, value] of this._pairs) {
        callback.call(thisArg, value, name, this);
      }
    }
    get size() { return this._pairs.length; }
    toString() {
      return this._pairs.map(pair =>
        `${encodeFormComponent(pair[0])}=${encodeFormComponent(pair[1])}`
      ).join('&');
    }
    [Symbol.iterator]() { return this.entries(); }
  }
  globalThis.URLSearchParams = URLSearchParams;

  class URL {
    constructor(input, base) {
      let href = String(input ?? '');
      if (base && !/^[A-Za-z][A-Za-z0-9+.-]*:/.test(href)) {
        const resolved = new URL(base);
        const dir = resolved.pathname.replace(/\/[^/]*$/, '/') || '/';
        href = resolved.protocol + '//' + resolved.host +
            (href.startsWith('/') ? href : dir + href);
      }
      href = href.replace(/\\/g, '/');
      if (/^file:\/\/[A-Za-z]:/.test(href)) {
        href = 'file:///' + href.slice('file://'.length);
      }
      this.username = '';
      this.password = '';
      this.hostname = '';
      this.port = '';
      this.pathname = '/';
      this._search = '';
      this._hash = '';
      this.searchParams = new URLSearchParams('', value => {
        this._search = value ? `?${value}` : '';
        this._recompose();
      });
      this._assign(href);
    }
    _assign(href) {
      const hashIdx = href.indexOf('#');
      if (hashIdx >= 0) {
        this._hash = href.slice(hashIdx);
        href = href.slice(0, hashIdx);
      } else {
        this._hash = '';
      }
      const searchIdx = href.indexOf('?');
      if (searchIdx >= 0) {
        this._search = href.slice(searchIdx);
        href = href.slice(0, searchIdx);
      } else {
        this._search = '';
      }
      this.searchParams._replace(this._search);
      const protoIdx = href.indexOf(':');
      this.protocol = protoIdx >= 0 ? href.slice(0, protoIdx + 1) : '';
      let rest = protoIdx >= 0 ? href.slice(protoIdx + 1) : href;
      this.hostname = '';
      this.port = '';
      if (rest.startsWith('//')) {
        rest = rest.slice(2);
        const slash = rest.indexOf('/');
        const authority = slash >= 0 ? rest.slice(0, slash) : rest;
        this.pathname = slash >= 0 ? rest.slice(slash) : '/';
        const at = authority.lastIndexOf('@');
        const hostport = at >= 0 ? authority.slice(at + 1) : authority;
        const colon = hostport.lastIndexOf(':');
        const bracket = hostport.indexOf(']');
        if (hostport.startsWith('[') && bracket >= 0) {
          this.hostname = hostport.slice(0, bracket + 1);
          this.port = hostport.slice(bracket + 2);
        } else if (colon >= 0) {
          this.hostname = hostport.slice(0, colon);
          this.port = hostport.slice(colon + 1);
        } else {
          this.hostname = hostport;
        }
      } else {
        this.pathname = rest || '/';
      }
      this._recompose();
    }
    get host() {
      return this.port ? `${this.hostname}:${this.port}` : this.hostname;
    }
    set host(value) {
      const text = String(value || '');
      const colon = text.lastIndexOf(':');
      if (colon >= 0 && !text.startsWith('[')) {
        this.hostname = text.slice(0, colon);
        this.port = text.slice(colon + 1);
      } else {
        this.hostname = text;
        this.port = '';
      }
      this._recompose();
    }
    get search() { return this._search; }
    set search(value) {
      const text = String(value || '');
      this._search = !text || text.startsWith('?') ? text : `?${text}`;
      this.searchParams._replace(this._search);
      this._recompose();
    }
    get hash() { return this._hash; }
    set hash(value) {
      const text = String(value || '');
      this._hash = !text || text.startsWith('#') ? text : `#${text}`;
      this._recompose();
    }
    get origin() { return `${this.protocol}//${this.host}`; }
    _recompose() {
      const hasAuthority = this.hostname !== '' || this.protocol === 'file:' ||
          this.protocol === 'chrome:' || this.protocol === 'http:' ||
          this.protocol === 'https:';
      let path = this.pathname || '';
      if (hasAuthority && path && !path.startsWith('/')) {
        path = `/${path}`;
      }
      this.href = `${this.protocol}${hasAuthority ? '//' + this.host : ''}${path}${this._search}${this._hash}`;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
  }
  globalThis.URL = URL;

  const invokeHandlers = new Map();
  const ipcMain = new EventEmitter();
  ipcMain.handle = (channel, handler) => {
    validateListener(handler);
    if (invokeHandlers.has(channel)) {
      throw new Error(`Attempted to register a second handler for '${channel}'`);
    }
    invokeHandlers.set(channel, handler);
  };
  ipcMain.handleOnce = (channel, handler) => {
    ipcMain.handle(channel, (event, ...args) => {
      ipcMain.removeHandler(channel);
      return handler(event, ...args);
    });
  };
  ipcMain.removeHandler = channel => invokeHandlers.delete(channel);

  function createEvent(sender) {
    const event = {
      processId: sender.processId,
      frameId: sender.frameId,
      sender: getSenderWebContents(sender),
      senderFrame: {
        processId: sender.processId,
        routingId: sender.frameId,
      },
      ports: [],
      reply(channel, ...args) {
        __xenonSendToRenderer(sender.endpointId, channel, args);
      },
      returnValue: undefined,
    };
    return event;
  }

  let dispatchXenonNet = null;
  globalThis.__xenonDispatchSend = (sender, channel, args) => {
    const list = Array.isArray(args) ? args : (args === undefined ? [] : [args]);
    if (channel === '__xenon:preload-error') {
      const event = createEvent(sender);
      const error = new Error(String(list[1] || 'Preload script failed'));
      event.sender.emit('preload-error', {sender: event.sender}, list[0], error);
      return;
    }
    if (typeof channel === 'string' && channel.startsWith('__xenon:net:') &&
        dispatchXenonNet && dispatchXenonNet(channel, list[0], sender)) {
      return;
    }
    ipcMain.emit(channel, createEvent(sender), ...list);
  };
  globalThis.__xenonDispatchInvoke = (sender, channel, args) => {
    const handler = invokeHandlers.get(channel);
    if (!handler) {
      return Promise.reject(new Error(`No handler registered for '${channel}'`));
    }
    try {
      return Promise.resolve(handler(createEvent(sender), ...args));
    } catch (error) {
      return Promise.reject(error);
    }
  };
  globalThis.__xenonDispatchSync = (sender, channel, args) => {
    const event = createEvent(sender);
    if (channel === '__xenon:renderer-web-preferences') {
      const endpoint = webContentsState.get(event.sender).endpoints.get(sender.endpointId);
      if (endpoint && typeof args[0] === 'boolean') endpoint.isMainFrame = args[0];
      return event.sender.getLastWebPreferences();
    }
    ipcMain.emit(channel, event, ...args);
    if (event.returnValue && typeof event.returnValue.then === 'function') {
      throw new Error(`Synchronous handler for '${channel}' returned a Promise`);
    }
    return event.returnValue;
  };

  const posix = {
    sep: '/',
    delimiter: ':',
    isAbsolute(path) {
      return typeof path === 'string' && path.startsWith('/');
    },
    normalize(path) {
      path = String(path || '.');
      if (!path) return '.';
      const isAbs = path.startsWith('/');
      const trailingSlash = path.endsWith('/') && path.length > 1;
      const parts = [];
      for (const segment of path.split('/')) {
        if (!segment || segment === '.') continue;
        if (segment === '..') {
          if (parts.length && parts[parts.length - 1] !== '..') {
            parts.pop();
          } else if (!isAbs) {
            parts.push('..');
          }
        } else {
          parts.push(segment);
        }
      }
      let result = (isAbs ? '/' : '') + parts.join('/');
      if (!result) return isAbs ? '/' : '.';
      if (trailingSlash && !result.endsWith('/')) result += '/';
      return result;
    },
    join(...paths) {
      const joined = paths.filter(p => typeof p === 'string' && p.length > 0).join('/');
      return posix.normalize(joined || '.');
    },
    resolve(...paths) {
      let resolved = '';
      for (let i = paths.length - 1; i >= 0; --i) {
        const p = String(paths[i] || '');
        if (!p) continue;
        resolved = resolved ? p + '/' + resolved : p;
        if (posix.isAbsolute(p)) break;
      }
      if (!posix.isAbsolute(resolved)) {
        resolved = '/' + resolved;
      }
      return posix.normalize(resolved);
    },
    dirname(path) {
      path = posix.normalize(path);
      if (path === '/') return '/';
      const idx = path.lastIndexOf('/');
      if (idx < 0) return '.';
      if (idx === 0) return '/';
      return path.slice(0, idx);
    },
    basename(path, ext) {
      path = String(path || '');
      const idx = path.lastIndexOf('/');
      let base = idx < 0 ? path : path.slice(idx + 1);
      if (ext && base.endsWith(String(ext))) {
        base = base.slice(0, -String(ext).length);
      }
      return base;
    },
    extname(path) {
      const base = posix.basename(path);
      const idx = base.lastIndexOf('.');
      return idx <= 0 ? '' : base.slice(idx);
    },
    parse(path) {
      path = String(path || '');
      const isAbs = posix.isAbsolute(path);
      const root = isAbs ? '/' : '';
      const base = posix.basename(path);
      const ext = posix.extname(path);
      const dir = posix.dirname(path);
      const name = ext ? base.slice(0, -ext.length) : base;
      return { root, dir, base, ext, name };
    },
    format(obj) {
      if (!obj || typeof obj !== 'object') return '';
      const dir = obj.dir || obj.root || '';
      const base = obj.base || `${obj.name || ''}${obj.ext || ''}`;
      if (!dir) return base;
      return dir.endsWith('/') ? dir + base : dir + '/' + base;
    },
    relative(from, to) {
      const fromParts = posix.resolve(from).split('/').filter(Boolean);
      const toParts = posix.resolve(to).split('/').filter(Boolean);
      let common = 0;
      while (common < fromParts.length && common < toParts.length &&
             fromParts[common] === toParts[common]) {
        ++common;
      }
      const up = new Array(fromParts.length - common).fill('..');
      return [...up, ...toParts.slice(common)].join('/') || '.';
    },
    toNamespacedPath: p => String(p),
  };

  const win32 = {
    sep: '\\',
    delimiter: ';',
    isAbsolute(path) {
      path = String(path || '');
      if (!path) return false;
      return /^[A-Za-z]:[\\/]|^[\\/]{2}[^\\/]+[\\/][^\\/]+|^[\\/]/.test(path);
    },
    normalize(path) {
      path = String(path || '.').replace(/\//g, '\\');
      if (!path) return '.';
      const isUnc = /^\\\\/.test(path);
      const driveMatch = path.match(/^[A-Za-z]:/);
      let device = '';
      if (driveMatch) {
        device = driveMatch[0];
        path = path.slice(2);
      } else if (isUnc) {
        const match = path.match(/^(\\\\[^\\]+\\[^\\]+)/);
        if (match) {
          device = match[0];
          path = path.slice(device.length);
        }
      }
      const isAbs = path.startsWith('\\');
      const parts = [];
      for (const segment of path.split('\\')) {
        if (!segment || segment === '.') continue;
        if (segment === '..') {
          if (parts.length && parts[parts.length - 1] !== '..') {
            parts.pop();
          } else if (!isAbs && !device) {
            parts.push('..');
          }
        } else {
          parts.push(segment);
        }
      }
      let result = device;
      if (isAbs) result += '\\';
      result += parts.join('\\');
      if (!result) return isAbs ? '\\' : (device ? device + (isAbs ? '\\' : '.') : '.');
      return result;
    },
    join(...paths) {
      const joined = paths.filter(p => typeof p === 'string' && p.length > 0)
                          .map(p => String(p).replace(/\//g, '\\'))
                          .join('\\');
      return win32.normalize(joined || '.');
    },
    resolve(...paths) {
      let resolvedDevice = '';
      let resolvedTail = '';
      let resolvedAbsolute = false;
      for (let i = paths.length - 1; i >= 0; --i) {
        let p = String(paths[i] || '').replace(/\//g, '\\');
        if (!p) continue;
        let device = '';
        const driveMatch = p.match(/^[A-Za-z]:/);
        if (driveMatch) {
          device = driveMatch[0];
          p = p.slice(2);
        } else if (/^\\\\/.test(p)) {
          const match = p.match(/^(\\\\[^\\]+\\[^\\]+)/);
          if (match) {
            device = match[0];
            p = p.slice(device.length);
          }
        }
        const isAbs = p.startsWith('\\');
        if (device && resolvedDevice && device.toLowerCase() !== resolvedDevice.toLowerCase()) {
          continue;
        }
        if (!resolvedDevice && device) {
          resolvedDevice = device;
        }
        if (!resolvedAbsolute) {
          resolvedTail = p + (resolvedTail ? '\\' + resolvedTail : '');
          if (isAbs) {
            resolvedAbsolute = true;
          }
        }
        if (resolvedDevice && resolvedAbsolute) break;
      }
      if (!resolvedAbsolute) {
        const cwd = globalThis.process.cwd().replace(/\//g, '\\');
        return win32.normalize(cwd + '\\' + resolvedTail);
      }
      return win32.normalize(resolvedDevice + resolvedTail);
    },
    dirname(path) {
      path = win32.normalize(path);
      const rootMatch = path.match(/^([A-Za-z]:\\|\\\\[^\\]+\\[^\\]+\\|\\|[A-Za-z]:)/);
      const root = rootMatch ? rootMatch[0] : '';
      const tail = root ? path.slice(root.length) : path;
      const idx = tail.lastIndexOf('\\');
      if (idx < 0) return root || '.';
      return root + tail.slice(0, idx);
    },
    basename(path, ext) {
      path = String(path || '').replace(/\//g, '\\');
      const idx = path.lastIndexOf('\\');
      let base = idx < 0 ? path : path.slice(idx + 1);
      if (base.endsWith(':') && /^[A-Za-z]:$/.test(base)) base = '';
      if (ext && base.endsWith(String(ext))) {
        base = base.slice(0, -String(ext).length);
      }
      return base;
    },
    extname(path) {
      const base = win32.basename(path);
      const idx = base.lastIndexOf('.');
      return idx <= 0 ? '' : base.slice(idx);
    },
    parse(path) {
      path = String(path || '').replace(/\//g, '\\');
      const rootMatch = path.match(/^([A-Za-z]:\\|\\\\[^\\]+\\[^\\]+\\|\\|[A-Za-z]:)/);
      const root = rootMatch ? rootMatch[0] : '';
      const dir = win32.dirname(path);
      const base = win32.basename(path);
      const ext = win32.extname(path);
      const name = ext ? base.slice(0, -ext.length) : base;
      return { root, dir, base, ext, name };
    },
    format(obj) {
      if (!obj || typeof obj !== 'object') return '';
      const dir = obj.dir || obj.root || '';
      const base = obj.base || `${obj.name || ''}${obj.ext || ''}`;
      if (!dir) return base;
      return dir.endsWith('\\') ? dir + base : dir + '\\' + base;
    },
    relative(from, to) {
      const fromResolved = win32.resolve(from);
      const toResolved = win32.resolve(to);
      const fromParts = fromResolved.split('\\').filter(Boolean);
      const toParts = toResolved.split('\\').filter(Boolean);
      let common = 0;
      while (common < fromParts.length && common < toParts.length &&
             fromParts[common].toLowerCase() === toParts[common].toLowerCase()) {
        ++common;
      }
      const up = new Array(fromParts.length - common).fill('..');
      return [...up, ...toParts.slice(common)].join('\\') || '.';
    },
    toNamespacedPath: p => String(p),
  };

  posix.win32 = win32;
  posix.posix = posix;
  win32.win32 = win32;
  win32.posix = posix;

  const isWindows = __xenonPlatform === 'win32';
  const pathModule = isWindows ? win32 : posix;

  const osPlatform = __xenonPlatform;
  const osArch = __xenonArch;
  const osEndianness = typeof __xenonEndianness === 'string' ? __xenonEndianness : '';
  const osNativeCall = request => {
    if (typeof __xenonOsCall !== 'function') {
      throw Object.assign(new Error('Native OS queries are unavailable'),
                          {code: 'ERR_NOT_SUPPORTED'});
    }
    return __xenonOsCall(request);
  };
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
