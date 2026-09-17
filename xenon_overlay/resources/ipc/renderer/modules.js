  let hostedCjsLoadingFile = '';

  function moduleError(code, message) {
    return Object.assign(new Error(message), {code});
  }

  function modulePathKey(filename) {
    let normalized = pathModule.normalize(filename);
    const rootLength = pathModule.parse(normalized).root.length;
    while (normalized.length > rootLength && normalized.endsWith(pathModule.sep)) {
      normalized = normalized.slice(0, -1);
    }
    return runtimePlatform === 'win32' ? normalized.toLowerCase() : normalized;
  }

  function modulePathWithin(filename, root) {
    const candidate = modulePathKey(filename);
    let prefix = modulePathKey(root);
    // Keep the separator on filesystem roots, including / and UNC shares.
    // Prefix matching otherwise needs a component boundary (app != app-old).
    if (!prefix.endsWith(pathModule.sep)) prefix += pathModule.sep;
    return candidate === modulePathKey(root) || candidate.startsWith(prefix);
  }

  function builtinModuleId(request) {
    if (typeof request !== 'string') {
      throw Object.assign(new TypeError('Module name must be a string'),
                          {code: 'ERR_INVALID_ARG_TYPE'});
    }
    if (!request || request.includes('\0')) {
      throw Object.assign(new TypeError('Module name must not be empty or contain null bytes'),
                          {code: 'ERR_INVALID_ARG_VALUE'});
    }
    if (request.startsWith('node:')) {
      const name = request.slice(5);
      if (!builtinModuleNames.has(name)) {
        throw moduleError('ERR_UNKNOWN_BUILTIN_MODULE',
                          `No such built-in module: ${request}`);
      }
      return name;
    }
    return builtinModuleNames.has(request) || request === 'electron' ||
        request === 'xenon:sqlite3' ? request : '';
  }

  function moduleFilenameFromSource(filename) {
    if (typeof filename !== 'string' || !filename) return filename;
    let sourceUrl;
    try { sourceUrl = new URL(filename); } catch (_error) { return filename; }
    if (sourceUrl.protocol === 'file:') return urlModule.fileURLToPath(sourceUrl);
    const configuredMappings = injectedPaths.rendererUrlMappings;
    const mappings = Array.isArray(configuredMappings) ? configuredMappings : [];
    const candidates = mappings.map(mapping => {
      try {
        return {root: mapping.sourcePathPrefix, url: new URL(mapping.targetBaseUrl)};
      } catch (_error) { return null; }
    }).filter(mapping => mapping && typeof mapping.root === 'string' &&
        pathModule.isAbsolute(mapping.root));
    // Old hosts only expose the current document's path. This fallback is for
    // that one origin, and is disabled whenever the host supplies mappings.
    if (!mappings.length && injectedPaths.documentPath && globalThis.location) {
      try {
        const location = globalThis.location;
        const documentUrl = new URL(location.href ||
            `${location.protocol}//${location.hostname}${location.pathname || '/index.html'}`);
        const documentName = decodeURIComponent(documentUrl.pathname);
        const documentParts = documentName.split('/').filter(Boolean);
        let root = pathModule.normalize(injectedPaths.documentPath);
        // Preserve the corresponding relative filename instead of assigning a
        // script in another origin to this application's module directory.
        for (const part of documentParts.slice().reverse()) {
          if (modulePathKey(pathModule.basename(root)) !== modulePathKey(part)) {
            root = '';
            break;
          }
          root = pathModule.dirname(root);
        }
        if (root) candidates.push({root, url: new URL('/', documentUrl)});
      } catch (_error) {}
    }
    candidates.sort((a, b) => b.url.pathname.length - a.url.pathname.length);
    for (const mapping of candidates) {
      const target = mapping.url;
      const prefix = target.pathname.endsWith('/') ? target.pathname : target.pathname + '/';
      if (sourceUrl.protocol !== target.protocol || sourceUrl.host !== target.host ||
          sourceUrl.username || sourceUrl.password || !sourceUrl.pathname.startsWith(prefix)) continue;
      const suffix = sourceUrl.pathname.slice(prefix.length);
      // Encoded separators must not turn a URL component into a filesystem
      // traversal or a different drive. Canonical root checks still run at load.
      if (/%(?:2f|5c)/i.test(suffix)) return filename;
      let relative;
      try { relative = decodeURIComponent(suffix); } catch (_error) { return filename; }
      if (relative.includes('\0') || relative.includes('\\') ||
          relative.split('/').some(part => part === '..' || part.includes(':'))) return filename;
      const root = pathModule.normalize(mapping.root);
      const resolved = pathModule.resolve(root, relative || '.');
      if (modulePathWithin(resolved, root)) {
        return resolved;
      }
    }
    return filename;
  }

  function installBindingsFileNameShim() {
    if (globalThis.__xenonBindingsFileNameShim) {
      return;
    }
    globalThis.__xenonBindingsFileNameShim = true;
    let innerPST = Error.prepareStackTrace;
    const wrappedFormatters = new WeakSet();
    Object.defineProperty(Error, 'prepareStackTrace', {
      configurable: true,
      enumerable: false,
      get() {
        return innerPST;
      },
      set(fn) {
        // bindings temporarily installs a formatter and restores the previous
        // value (usually undefined). Preserve V8's default string stack then,
        // and don't wrap an already saved formatter again on restoration.
        if (typeof fn !== 'function' || wrappedFormatters.has(fn)) {
          innerPST = fn;
          return;
        }
        innerPST = function(err, stack) {
          const fallback = hostedCjsLoadingFile;
          const mapped = (stack || []).map(site => new Proxy(site, {
            get(target, property) {
              if (property === 'getFileName' || property === 'getScriptNameOrSourceURL') {
                return () => {
                  try { return moduleFilenameFromSource(target[property]()) || fallback; }
                  catch (_error) { return fallback; }
                };
              }
              // V8 CallSite methods require the original internal receiver.
              const value = Reflect.get(target, property, target);
              return typeof value === 'function' ? value.bind(target) : value;
            },
          }));
          return fn(err, mapped);
        };
        wrappedFormatters.add(innerPST);
      },
    });
  }

  // Browser-loaded bundles use bindings before any hosted CommonJS module is
  // required, so install the filename mapping for the document itself.
  installBindingsFileNameShim();

  function resolveHostedCjs(request, parentFile = globalThis.__filename) {
    parentFile = moduleFilenameFromSource(parentFile);
    const builtin = builtinModuleId(request);
    if (builtin) return request.startsWith('node:') ? request : builtin;
    if (request.startsWith('#')) {
      throw moduleError('ERR_NOT_SUPPORTED', 'Package imports are not supported');
    }
    const cacheKey = `${parentFile}\0${request}`;
    const cachedPath = moduleResolutionCache.get(cacheKey);
    if (cachedPath) {
      // A deleted or failed module must be resolved again, including realpath
      // and the permitted-root check. Successful cached modules need no IPC.
      if (hostedCjsCache[cachedPath]) return cachedPath;
      moduleResolutionCache.delete(cacheKey);
    }
    const canonicalPath = filename => {
      const normalized = pathModule.normalize(filename);
      // ASAR members retain their virtual identity in the Browser bridge;
      // they are not standalone paths that the OS can normalize.
      return fsUsesVirtualMount(normalized) ? normalized :
          pathModule.normalize(fsNativeSync('realpath', normalized));
    };
    if (!canonicalModuleRoots) {
      canonicalModuleRoots = [];
      for (const root of new Set([appPath, exeDir].filter(Boolean))) {
        const configured = pathModule.normalize(root);
        try {
          canonicalModuleRoots.push({configured, canonical: canonicalPath(configured)});
        } catch (error) {
          if (error.code !== 'ENOENT' && error.code !== 'ENOTDIR') throw error;
        }
      }
    }
    const within = (filename, roots) => {
      return roots.some(root => modulePathWithin(filename, root));
    };
    const realRoots = canonicalModuleRoots.map(root => root.canonical);
    const permittedPaths = canonicalModuleRoots.flatMap(
        root => [root.configured, root.canonical]);
    const inRoot = filename => within(filename, permittedPaths);
    const entries = new Map();
    const inspect = filename => {
      filename = pathModule.normalize(filename);
      if (!inRoot(filename)) return null;
      if (entries.has(filename)) return entries.get(filename);
      const virtual = memFiles.get(normalizeFsPath(filename));
      if (virtual) {
        const entry = {type: virtual.type, filename};
        entries.set(filename, entry);
        return entry;
      }
      try {
        const stat = fsNativeSync('stat', filename);
        const canonical = canonicalPath(filename);
        if (!within(canonical, realRoots)) {
          throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${request}'`);
        }
        const entry = {
          type: stat.isFile ? 'file' : stat.isDirectory ? 'dir' : '',
          filename: canonical,
        };
        entries.set(filename, entry);
        return entry;
      } catch (error) {
        if (error.code === 'ENOENT' || error.code === 'ENOTDIR') {
          entries.set(filename, null);
          return null;
        }
        throw error;
      }
    };
    // exports and main resolution can inspect the same package. Reuse its
    // parsed config only for this resolution; a later retry or cache deletion
    // must observe the current file, including changed main/exports fields.
    let packageConfigs;
    const packageJson = directory => {
      const entry = inspect(pathModule.join(directory, 'package.json'));
      if (!entry || entry.type !== 'file') return null;
      const filename = entry.filename;
      packageConfigs ||= new Map();
      if (packageConfigs.has(filename)) return packageConfigs.get(filename);
      try {
        const config = JSON.parse(readHostedCjsSource(filename));
        packageConfigs.set(filename, config);
        return config;
      } catch (error) {
        if (error instanceof SyntaxError) {
          throw moduleError('ERR_INVALID_PACKAGE_CONFIG',
                            `Invalid package config ${filename}: ${error.message}`);
        }
        throw error;
      }
    };
    const asFile = filename => {
      const exact = inspect(filename);
      if (exact && exact.type === 'file') return exact.filename;
      for (const extension of ['.js', '.json', '.node']) {
        const entry = inspect(filename + extension);
        if (entry && entry.type === 'file') return entry.filename;
      }
      return '';
    };
    const asPath = (filename, depth = 0) => {
      if (depth > 32) {
        throw moduleError('ERR_INVALID_PACKAGE_CONFIG', 'Package main resolution is too deeply nested');
      }
      const file = asFile(filename);
      if (file) return file;
      const directory = inspect(filename);
      if (!directory || directory.type !== 'dir') return '';
      filename = directory.filename;
      const pkg = packageJson(filename);
      if (pkg && typeof pkg.main === 'string' && pkg.main) {
        const main = pathModule.resolve(filename, pkg.main);
        if (main !== filename) {
          const resolved = asPath(main, depth + 1);
          if (resolved) return resolved;
        }
      }
      for (const index of ['index.js', 'index.json', 'index.node']) {
        const entry = inspect(pathModule.join(filename, index));
        if (entry && entry.type === 'file') return entry.filename;
      }
      return '';
    };
    let resolved = '';
    const relativeRequest = runtimePlatform === 'win32' ?
        /^\.\.?([\\/]|$)/.test(request) : /^\.\.?(\/|$)/.test(request);
    if (pathModule.isAbsolute(request) || relativeRequest) {
      resolved = asPath(pathModule.resolve(pathModule.dirname(parentFile), request));
    } else {
      const parts = (runtimePlatform === 'win32' ? request.replace(/\\/g, '/') : request).split('/');
      const packageName = parts[0].startsWith('@') ? parts.slice(0, 2).join('/') : parts[0];
      let directory = pathModule.dirname(parentFile);
      while (inRoot(directory)) {
        if (modulePathKey(pathModule.basename(directory)) !== 'node_modules') {
          const modules = pathModule.join(directory, 'node_modules');
          const pkg = packageJson(pathModule.join(modules, packageName));
          if (pkg && Object.prototype.hasOwnProperty.call(pkg, 'exports')) {
            throw moduleError('ERR_NOT_SUPPORTED',
                              `Package exports are not supported: ${packageName}`);
          }
          resolved = asPath(pathModule.join(modules, request));
          if (resolved) break;
        }
        const parent = pathModule.dirname(directory);
        if (parent === directory) break;
        directory = parent;
      }
    }
    if (!resolved) throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${request}'`);
    if (hostedCjsCache[resolved]) moduleResolutionCache.set(cacheKey, resolved);
    return resolved;
  }

  function loadHostedCjsModule(resolved) {
    if (hostedCjsCache[resolved]) return hostedCjsCache[resolved].exports;
    installBindingsFileNameShim();
    const moduleObject = {
      exports: {},
      filename: resolved,
      id: resolved,
      loaded: false,
      children: [],
      parent: null,
      paths: [],
    };
    hostedCjsCache[resolved] = moduleObject;
    const previousLoading = hostedCjsLoadingFile;
    hostedCjsLoadingFile = resolved;
    try {
      // Bind relative requires to this module, including requires called by
      // exported functions after the module's initial evaluation has finished.
      const moduleRequire = request => electronRequire(request, resolved);
      moduleRequire.resolve = request => resolveHostedCjs(request, resolved);
      moduleRequire.cache = hostedCjsCache;
      moduleObject.require = moduleRequire;
      if (/\.node$/i.test(resolved)) {
        moduleObject.exports = loadNativeNodeModule(resolved);
      } else {
        const code = readHostedCjsSource(resolved).replace(/^\uFEFF/, '');
        if (/\.json$/i.test(resolved)) {
          moduleObject.exports = JSON.parse(code);
        } else {
          const fn = new Function(
              'exports', 'require', 'module', '__filename', '__dirname', 'process', 'Buffer',
              code.replace(/^#![^\r\n]*/, '') + '\n//# sourceURL=' + resolved.replace(/\\/g, '/'));
          fn.call(moduleObject.exports, moduleObject.exports, moduleRequire, moduleObject, resolved,
              pathModule.dirname(resolved), process, Buffer);
        }
      }
      moduleObject.loaded = true;
    } catch (error) {
      delete hostedCjsCache[resolved];
      throw error;
    } finally {
      hostedCjsLoadingFile = previousLoading;
    }
    return moduleObject.exports;
  }

  function readHostedCjsSource(filename) {
    // Preloads and their dependencies are real application modules. Do not
    // hide on-disk scripts behind the renderer's synthetic filesystem mount.
    return memFiles.has(normalizeFsPath(filename)) ?
        String(fsModule.readFileSync(filename, 'utf8')) :
        fsReadResult(fsNativeSync('read_file', filename), 'utf8');
  }

  function loadBuiltinModule(request) {
    if (typeof request !== 'string') {
      throw new TypeError('Module name must be a string');
    }

    const norm = request;
    if (norm === 'http2') return http2Module;
    if (norm === 'path/win32') return pathModule.win32;
    if (norm === 'path/posix') return pathModule.posix;

    // Electron
    if (norm === 'electron' || norm === 'node:electron') {
      return electron;
    }

    if (norm === 'process') return process;

    // Path
    if (norm === 'path' || norm === 'node:path') {
      return pathModule;
    }

    // OS
    if (norm === 'os' || norm === 'node:os') {
      return osModule;
    }

    // Events
    if (norm === 'events' || norm === 'node:events') {
      return EventEmitter;
    }

    // Async hooks
    if (norm === 'async_hooks' || norm === 'node:async_hooks') {
      return asyncHooksModule;
    }

    // Buffer
    if (norm === 'buffer' || norm === 'node:buffer') {
      return { Buffer };
    }

    // Util
    if (norm === 'util' || norm === 'node:util') {
      return utilModule;
    }

    // Net
    if (norm === 'net' || norm === 'node:net') {
      return netModule;
    }

    // Stream
    if (norm === 'stream' || norm === 'node:stream') {
      return streamModule;
    }

    // Crypto
    if (norm === 'crypto' || norm === 'node:crypto') {
      return cryptoModule;
    }

    // URL
    if (norm === 'url' || norm === 'node:url') {
      return urlModule;
    }

    // TTY (readable-stream / debug / winston often require this)
    if (norm === 'tty' || norm === 'node:tty') {
      return {
        isatty: () => false,
        ReadStream: class extends EventEmitter {},
        WriteStream: class extends EventEmitter {
          constructor() {
            super();
            this.columns = 80;
            this.rows = 24;
            this.isTTY = false;
          }
          write() { return true; }
        },
      };
    }

    // DNS
    if (norm === 'dns' || norm === 'node:dns') {
      const unsupported = (hostname) => {
        const error = new Error(
            `DNS resolution is unavailable in the hosted runtime: ${hostname}`);
        error.code = 'ENOTSUP';
        error.syscall = 'getaddrinfo';
        error.hostname = String(hostname || '');
        return error;
      };
      return {
        lookup: (hostname, opts, cb) => {
          const callback = typeof opts === 'function' ? opts : cb;
          if (typeof callback === 'function') {
            queueMicrotask(() => callback(unsupported(hostname)));
          }
        },
        resolve: (hostname, cb) => {
          if (typeof cb === 'function') {
            queueMicrotask(() => cb(unsupported(hostname)));
          }
        },
        promises: {
          lookup: async hostname => { throw unsupported(hostname); },
          resolve: async hostname => { throw unsupported(hostname); },
        },
      };
    }

    // Timers
    if (norm === 'timers' || norm === 'node:timers') {
      return {
        setTimeout: globalThis.setTimeout.bind(globalThis),
        clearTimeout: globalThis.clearTimeout.bind(globalThis),
        setInterval: globalThis.setInterval.bind(globalThis),
        clearInterval: globalThis.clearInterval.bind(globalThis),
        setImmediate: (fn, ...args) =>
            globalThis.setTimeout(() => fn(...args), 0),
        clearImmediate: (id) => globalThis.clearTimeout(id),
      };
    }

    // Querystring
    if (norm === 'querystring' || norm === 'node:querystring') {
      return {
        parse: (str) => Object.fromEntries(new URLSearchParams(str)),
        stringify: (obj) => new URLSearchParams(obj || {}).toString(),
        escape: encodeURIComponent,
        unescape: decodeURIComponent,
      };
    }

    // StringDecoder
    if (norm === 'string_decoder' || norm === 'node:string_decoder') {
      const cls = class StringDecoder {
        constructor(enc = 'utf8') { this.encoding = enc; }
        write(buf) { return new TextDecoder(this.encoding).decode(buf); }
        end(buf) { return buf ? new TextDecoder(this.encoding).decode(buf) : ''; }
      };
      return { StringDecoder: cls, default: cls };
    }

    // TLS
    if (norm === 'tls' || norm === 'node:tls') {
      return tlsModule;
    }

    // HTTP / HTTPS — Node ClientRequest surface backed by the browser network
    // service.
    // A no-op stub never fires response/error callbacks, so any await on
    // https.request hangs forever.
    if (norm === 'http' || norm === 'node:http' || norm === 'https' ||
        norm === 'node:https') {
      const buildRequestUrl = (opt) => {
        if (typeof opt === 'string') {
          return opt;
        }
        if (opt && typeof opt === 'object' && opt.href) {
          return String(opt.href);
        }
        const o = opt && typeof opt === 'object' ? opt : {};
        const protocol =
            o.protocol || (norm.indexOf('https') >= 0 ? 'https:' : 'http:');
        const host = o.hostname || o.host || 'localhost';
        const port = o.port ? `:${o.port}` : '';
        const path = o.path || o.pathname || '/';
        return `${protocol}//${host}${port}${path}`;
      };

      const createClientRequest = (opt, cb) => {
        const req = new EventEmitter();
        if (typeof cb === 'function') req.once('response', cb);
        const bodyChunks = [];
        let finished = false;
        let timeoutId = null;
        let aborted = false;
        const options = typeof opt === 'string' ? {href: opt} : (opt || {});

        const cancelPendingRequest = () => {
          aborted = true;
          bodyChunks.length = 0;
          if (timeoutId !== null) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
        };
        req.write = (chunk, encoding) => {
          if (aborted) return false;
          if (chunk == null) {
            return true;
          }
          if (typeof chunk === 'string') {
            const format = typeof encoding === 'string' ? encoding.toLowerCase() : 'utf8';
            if (['latin1', 'binary', 'ascii'].includes(format)) {
              const bytes = Buffer.alloc(chunk.length);
              for (let i = 0; i < chunk.length; ++i) bytes[i] = chunk.charCodeAt(i) & 255;
              bodyChunks.push(bytes);
            } else if (['utf16le', 'utf-16le', 'ucs2', 'ucs-2'].includes(format)) {
              const bytes = Buffer.alloc(chunk.length * 2);
              for (let i = 0; i < chunk.length; ++i) {
                const code = chunk.charCodeAt(i);
                bytes[i * 2] = code & 255;
                bytes[i * 2 + 1] = code >>> 8;
              }
              bodyChunks.push(bytes);
            } else if (['utf8', 'utf-8', 'hex', 'base64', 'base64url'].includes(format)) {
              bodyChunks.push(Buffer.from(chunk, format));
            } else {
              const error = new TypeError(`Unknown encoding: ${encoding}`);
              error.code = 'ERR_UNKNOWN_ENCODING';
              throw error;
            }
          } else if (ArrayBuffer.isView(chunk)) {
            // Snapshot the visible raw bytes once at write time, including
            // non-byte typed arrays and DataView subviews.
            bodyChunks.push(Buffer.from(new Uint8Array(
                chunk.buffer, chunk.byteOffset, chunk.byteLength)));
          } else {
            bodyChunks.push(String(chunk));
          }
          return true;
        };
        req.setTimeout = (ms, onTimeout) => {
          if (aborted) return req;
          if (timeoutId) {
            clearTimeout(timeoutId);
            timeoutId = null;
          }
          const delay = Number(ms);
          if (!(delay > 0)) {
            return req;
          }
          timeoutId = setTimeout(() => {
            if (aborted) return;
            cancelPendingRequest();
            const err = new Error('Request timeout');
            err.code = 'ETIMEDOUT';
            req.emit('timeout');
            if (typeof onTimeout === 'function') {
              onTimeout();
            }
            req.emit('error', err);
          }, delay);
          return req;
        };
        req.abort = () => {
          cancelPendingRequest();
          return req;
        };
        req.destroy = (err) => {
          cancelPendingRequest();
          if (err) {
            req.emit('error', err);
          }
          return req;
        };
        req.end = (chunk, encoding) => {
          if (finished || aborted) {
            return req;
          }
          finished = true;
          if (chunk != null) {
            req.write(chunk, encoding);
          }
          const timeoutMs = Number(options.timeout);
          if (timeoutMs > 0 && !timeoutId) {
            req.setTimeout(timeoutMs);
          }
          queueMicrotask(async () => {
            try {
              if (aborted) {
                return;
              }
              const method = String(options.method || 'GET').toUpperCase();
              const headers = Object.assign({}, options.headers || {});
              let bodyBuffer = Buffer.alloc(0);
              if (method !== 'GET' && method !== 'HEAD' && bodyChunks.length) {
                bodyBuffer = Buffer.concat(bodyChunks.map(part =>
                    typeof part === 'string' ? Buffer.from(part) : part));
              }
              // The encoded request owns its payload after submission. Do not
              // keep the caller's queued upload snapshots on the request.
              bodyChunks.length = 0;
              // Electron/Node requests use the browser network service rather
              // than renderer fetch. This preserves caller headers and avoids
              // applying renderer CORS policy to a Node networking API.
              const response = await transport.invoke('__xenon:net-request', {
                url: buildRequestUrl(options),
                method,
                headers,
                bodyBase64: bodyBuffer.toString('base64'),
                useSessionCookies: Boolean(options.useSessionCookies),
              });
              if (timeoutId) {
                clearTimeout(timeoutId);
                timeoutId = null;
              }
              if (aborted) {
                return;
              }
              const responseBody = Buffer.from(
                  String(response && response.bodyBase64 || ''), 'base64');
              const incoming = new EventEmitter();
              incoming.statusCode = Number(response && response.statusCode) || 0;
              incoming.statusMessage =
                  String(response && response.statusMessage || '');
              incoming.headers = Object.assign({}, response && response.headers);
              incoming.url = String(response && response.finalUrl || '');
              let responseEncoding = null;
              incoming.setEncoding = (encoding) => {
                responseEncoding = String(encoding || 'utf8');
                return incoming;
              };
              req.emit('response', incoming);
              queueMicrotask(() => {
                if (aborted) return;
                if (responseBody.length) {
                  incoming.emit('data', responseEncoding ?
                      responseBody.toString(responseEncoding) : responseBody);
                }
                if (aborted) return;
                incoming.emit('end');
              });
            } catch (error) {
              if (timeoutId) {
                clearTimeout(timeoutId);
                timeoutId = null;
              }
              if (!aborted) {
                req.emit('error', error instanceof Error ? error
                                                         : new Error(String(error)));
              }
            }
          });
          return req;
        };
        return req;
      };

      return {
        request: (opt, cb) => createClientRequest(opt, cb),
        get: (url, cb) => {
          const req = createClientRequest(url, cb);
          req.end();
          return req;
        },
        createServer: () => {
          throw moduleError('ERR_NOT_SUPPORTED',
              `${norm.replace(/^node:/, '')}.createServer is not supported by this runtime`);
        },
        Agent: class {},
      };
    }

    // Child Process
    if (norm === 'child_process' || norm === 'node:child_process') {
      return childProcessModule;
    }

    // Zlib
    if (norm === 'zlib' || norm === 'node:zlib') {
      return createZlibModule((operation, data, options, asynchronous) => {
        const request = {operation, dataBase64: data.toString('base64'), options};
        if (asynchronous) {
          return transport.invoke('__xenon:zlib', request)
              .then(value => Buffer.from(value, 'base64'));
        }
        return Buffer.from(transport.sendSync('__xenon:zlib', request), 'base64');
      });
    }

    // Assert
    if (norm === 'assert' || norm === 'node:assert') {
      const assertFn = (val, msg) => { if (!val) throw new Error(msg || 'Assertion failed'); };
      assertFn.ok = assertFn;
      assertFn.strictEqual = (a, b) => { if (a !== b) throw new Error('Assertion failed'); };
      return assertFn;
    }

    // Constants
    if (norm === 'constants' || norm === 'node:constants') {
      return {};
    }

    // Readline
    if (norm === 'readline' || norm === 'node:readline') {
      return readlineModule;
    }

    // FS
    if (norm === 'fs' || norm === 'node:fs' || norm === 'fs/promises' || norm === 'node:fs/promises') {
      return norm.includes('promises') ? fsModule.promises : fsModule;
    }

    if (norm === 'xenon:sqlite3') {
      return sqlite3Module;
    }
    throw moduleError('ERR_NOT_SUPPORTED', `Module '${request}' is not supported by this runtime`);
  }

  const electronRequire = globalThis.require = function(request, parentFile = globalThis.__filename) {
    const builtin = builtinModuleId(request);
    if (builtin) {
      if (!builtinModuleCache.has(builtin)) {
        builtinModuleCache.set(builtin, loadBuiltinModule(builtin));
      }
      return builtinModuleCache.get(builtin);
    }
    const resolved = resolveHostedCjs(request, parentFile);
    const exports = loadHostedCjsModule(resolved);
    moduleResolutionCache.set(`${parentFile}\0${request}`, resolved);
    return exports;
  };

  globalThis.Buffer = Buffer;
  globalThis.require.resolve = request => resolveHostedCjs(request);
  globalThis.require.cache = hostedCjsCache;
  globalThis.require.main = undefined;
  globalThis.require.extensions = { '.js': () => {}, '.json': () => {}, '.node': () => {} };
  globalThis.__xenonElectronRequire = globalThis.require;
