  let hostedCjsLoadingFile = '';
  let rendererHttpServerModule;
  const moduleModule = createModuleModule(pathModule, urlModule,
      filename => bindHostedRequire(filename, createHostedModuleParent(filename)),
      name => builtinModuleNames.has(name.startsWith('node:') ? name.slice(5) : name));
  const defaultModuleLookupPaths = moduleModule._resolveLookupPaths;
  const defaultNodeModulePaths = moduleModule._nodeModulePaths;
  const hostedModuleParents = new Map();
  const hostedModuleParentIds = new WeakMap();
  let nextHostedModuleParentId = 0;

  function createHostedModuleParent(filename) {
    return {filename, id: filename, path: pathModule.dirname(filename),
      paths: moduleModule._nodeModulePaths(pathModule.dirname(filename))};
  }

  function hostedModuleParent(filename) {
    if (hostedCjsCache[filename]) return hostedCjsCache[filename];
    if (!hostedModuleParents.has(filename)) {
      hostedModuleParents.set(filename, createHostedModuleParent(filename));
    }
    return hostedModuleParents.get(filename);
  }

  function hostedResolutionCacheKey(parent, request) {
    // A custom lookup must run on every lookup: its closure and returned roots
    // can change even while the function identity remains the same.
    if (moduleModule._resolveLookupPaths !== defaultModuleLookupPaths) return null;
    if (!hostedModuleParentIds.has(parent)) {
      hostedModuleParentIds.set(parent, ++nextHostedModuleParentId);
    }
    return `${hostedModuleParentIds.get(parent)}\0${request}\0${JSON.stringify(parent.paths)}`;
  }

  function bindHostedRequire(filename, parent) {
    const boundRequire = request => electronRequire(request, filename, parent);
    boundRequire.resolve = request => resolveHostedCjs(request, filename, undefined, parent);
    boundRequire.resolve.paths = request => moduleModule._resolveLookupPaths(request, parent);
    boundRequire.cache = hostedCjsCache;
    return boundRequire;
  }

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
    // CommonJS parents are already native filenames. Skip URL parsing (and
    // its exception for POSIX paths) without changing file: or hosted URLs.
    if (pathModule.isAbsolute(filename)) return filename;
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

  // Callers convert source URLs before resolving so the successful-load cache
  // uses the same parent filename for both lookup and insertion.
  function resolveHostedCjs(request, parentFile, resolutionContext, parentModule) {
    const builtin = builtinModuleId(request);
    if (builtin) return request.startsWith('node:') ? request : builtin;
    parentModule ||= hostedModuleParent(parentFile);
    const cacheKey = hostedResolutionCacheKey(parentModule, request);
    const cachedPath = cacheKey && moduleResolutionCache.get(cacheKey);
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
    const hasExports = pkg => pkg && pkg.exports !== null &&
        (typeof pkg.exports === 'string' || typeof pkg.exports === 'object');
    const packageScope = start => {
      let directory = start;
      while (inRoot(directory) && modulePathKey(pathModule.basename(directory)) !== 'node_modules') {
        const pkg = packageJson(directory);
        if (pkg) return {directory, pkg};
        const parent = pathModule.dirname(directory);
        if (parent === directory) break;
        directory = parent;
      }
      return null;
    };
    // Keep metadata and canonical-path checks within this one load operation;
    // format selection can reuse them without a second filesystem round trip.
    if (resolutionContext) resolutionContext.packageScope = packageScope;
    const mappedTarget = (directory, pkg, key, isImports = false) => {
      const target = resolvePackageMap(pkg, key, ['node', 'require', 'node-addons'], isImports);
      if (isImports && !target.startsWith('./')) {
        // Bare imports targets use package resolution, not imports resolution:
        // an alias whose target starts with # is not another imports alias.
        const builtin = builtinModuleId(target);
        return builtin ? target : barePackage(target, directory, true);
      }
      const packageUrl = urlModule.pathToFileURL(pathModule.join(directory, 'package.json'));
      const filename = urlModule.fileURLToPath(new URL(target, packageUrl));
      const entry = inspect(filename);
      if (!entry || entry.type !== 'file') {
        throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${filename}'`);
      }
      return entry.filename;
    };
    const barePackage = (specifier, start, fromImports = false) => {
      const parts = (runtimePlatform === 'win32' ? specifier.replace(/\\/g, '/') : specifier).split('/');
      const packageParts = parts[0].startsWith('@') ? 2 : 1;
      const packageName = parts.slice(0, packageParts).join('/');
      const subpath = parts.length > packageParts ? './' + parts.slice(packageParts).join('/') : '.';
      const rawParts = specifier.split('/');
      const rawName = rawParts.slice(0, rawParts[0].startsWith('@') ? 2 : 1).join('/');
      if (fromImports && (!rawName || rawName.startsWith('.') || /[%\\:]/.test(rawName) ||
          (rawName.startsWith('@') && (!rawParts[1] || rawParts[0] === '@')))) {
        throw moduleError('ERR_INVALID_MODULE_SPECIFIER', `Invalid package name '${rawName}'`);
      }
      const scope = packageScope(start);
      if (scope && scope.pkg.name === packageName && hasExports(scope.pkg)) {
        return mappedTarget(scope.directory, scope.pkg, subpath);
      }
      const lookupPaths = fromImports ? defaultNodeModulePaths(start) :
          moduleModule._resolveLookupPaths(specifier, parentModule);
      if (lookupPaths !== null && (!Array.isArray(lookupPaths) ||
          lookupPaths.some(directory => typeof directory !== 'string' ||
              directory.includes('\0') || !pathModule.isAbsolute(directory)))) {
        throw moduleError('ERR_INVALID_ARG_VALUE', 'Module lookup paths must be an array of absolute paths');
      }
      for (const lookupPath of lookupPaths || []) {
        const modules = pathModule.resolve(lookupPath);
        if (inRoot(modules)) {
          const packageDirectory = pathModule.join(modules, packageName);
          const pkg = packageJson(packageDirectory);
          if (hasExports(pkg)) return mappedTarget(packageDirectory, pkg, subpath);
          if (fromImports && inspect(packageDirectory)?.type === 'dir') {
            // imports targets use PACKAGE_RESOLVE even when invoked by require:
            // legacy package mains may probe extensions, package subpaths cannot.
            if (subpath === '.') {
              const resolved = asPath(packageDirectory);
              if (resolved) return resolved;
            } else {
              if (/%(?:2f|5c)/i.test(subpath)) {
                throw moduleError('ERR_INVALID_MODULE_SPECIFIER',
                    `Encoded separator in package subpath '${specifier}'`);
              }
              const packageUrl = urlModule.pathToFileURL(pathModule.join(packageDirectory, 'package.json'));
              const filename = urlModule.fileURLToPath(new URL(subpath, packageUrl));
              const entry = inspect(filename);
              if (entry?.type === 'file') return entry.filename;
            }
            throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${specifier}'`);
          }
          const resolved = asPath(pathModule.join(modules, specifier));
          if (resolved) return resolved;
        }
      }
      return '';
    };
    let resolved = '';
    const relativeRequest = runtimePlatform === 'win32' ?
        /^\.\.?([\\/]|$)/.test(request) : /^\.\.?(\/|$)/.test(request);
    if (request.startsWith('#')) {
      const scope = packageScope(pathModule.dirname(parentFile));
      resolved = mappedTarget(scope?.directory || pathModule.dirname(parentFile),
          scope?.pkg || {}, request, true);
    } else if (pathModule.isAbsolute(request) || relativeRequest) {
      resolved = asPath(pathModule.resolve(pathModule.dirname(parentFile), request));
    } else {
      resolved = barePackage(request, pathModule.dirname(parentFile));
    }
    if (!resolved) throw moduleError('MODULE_NOT_FOUND', `Cannot find module '${request}'`);
    if (cacheKey && hostedCjsCache[resolved]) moduleResolutionCache.set(cacheKey, resolved);
    return resolved;
  }

  function hostedModuleHasEsmFormat(filename, preloadEntry, packageScope) {
    const extension = pathModule.extname(filename);
    if (extension === '.mjs') return true;
    // Electron preloads only opt into ESM with .mjs. Ordinary require follows
    // package.type for .js; unknown and extensionless CommonJS files do not.
    if (extension !== '.js' || preloadEntry) return false;
    return packageScope(pathModule.dirname(filename))?.pkg.type === 'module';
  }

  function loadHostedPreload(filename, registeredSource) {
    const resolved = registeredSource === undefined ?
        resolveHostedCjs(filename, moduleFilenameFromSource(globalThis.__filename)) : filename;
    return loadHostedCjsModule(resolved, registeredSource, true);
  }

  function loadHostedCjsModule(resolved, registeredSource, preloadEntry = false, packageScope) {
    if (hostedCjsCache[resolved]) return hostedCjsCache[resolved].exports;
    if (hostedModuleHasEsmFormat(resolved, preloadEntry, packageScope)) {
      throw moduleError('ERR_REQUIRE_ESM',
          `The renderer CommonJS loader does not support ES module ${resolved}`);
    }
    installBindingsFileNameShim();
    const moduleObject = {
      exports: {},
      filename: resolved,
      id: resolved,
      path: pathModule.dirname(resolved),
      loaded: false,
      children: [],
      parent: null,
      paths: moduleModule._nodeModulePaths(pathModule.dirname(resolved)),
    };
    hostedCjsCache[resolved] = moduleObject;
    const previousLoading = hostedCjsLoadingFile;
    hostedCjsLoadingFile = resolved;
    try {
      // Bind relative requires to this module, including requires called by
      // exported functions after the module's initial evaluation has finished.
      const moduleRequire = bindHostedRequire(resolved, moduleObject);
      moduleObject.require = moduleRequire;
      if (pathModule.extname(resolved) === '.node') {
        moduleObject.exports = loadNativeNodeModule(resolved);
      } else {
        const code = (registeredSource === undefined ? readHostedCjsSource(resolved) :
            registeredSource).replace(/^\uFEFF/, '');
        if (pathModule.extname(resolved) === '.json') {
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
        fsReadResult(fsNativeSync('read_file', filename, {returnBytes: true}), 'utf8');
  }

  function loadBuiltinModule(request) {
    if (typeof request !== 'string') {
      throw new TypeError('Module name must be a string');
    }

    const norm = request;
    if (norm === 'module') return moduleModule;
    if (norm === 'original-fs') return getOriginalFsModule();
    if (norm === 'original-fs/promises') return getOriginalFsModule().promises;
    if (norm === 'stream/promises') return streamModule.promises;
    if (norm === 'stream/web') return webStreamsModule;
    if (norm === 'http2') return http2Module;
    if (norm === 'path/win32') return pathModule.win32;
    if (norm === 'path/posix') return pathModule.posix;

    // Electron
    if (norm === 'electron' || norm === 'node:electron') {
      return electron;
    }

    if (norm === 'process') return process;
    if (norm === 'console') return consoleModule;

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
      return createBufferModule(Buffer, injectedPaths.bufferLimits);
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
    if (norm === 'timers/promises') return electronRequire('timers').promises;
    if (norm === 'timers' || norm === 'node:timers') {
      if (!rendererNodeTimers) {
        const immediateTasks = createImmediateTaskScheduler(globalThis.MessageChannel);
        rendererNodeTimers = createNodeTimers({
          setTimeout: globalThis.setTimeout.bind(globalThis),
          clearTimeout: globalThis.clearTimeout.bind(globalThis),
          setInterval: globalThis.setInterval.bind(globalThis),
          clearInterval: globalThis.clearInterval.bind(globalThis),
          setImmediate: callback => immediateTasks.setImmediate(captureAsyncCallback(callback)),
          clearImmediate: immediateTasks.clearImmediate,
        });
        rendererNodeTimers.promises = createTimersPromises(rendererNodeTimers);
      }
      return rendererNodeTimers;
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
      return stringDecoderModule;
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
      const serverModule = rendererHttpServerModule ||= createHttpServerModule(netModule);
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

      const createClientRequest = (opt, settings, cb) => {
        if (typeof settings === 'function') { cb = settings; settings = undefined; }
        const req = new EventEmitter();
        req.maxHeadersCount = null;
        if (typeof cb === 'function') req.once('response', cb);
        const bodyChunks = [];
        let finished = false;
        let timeoutId = null;
        let aborted = false;
        const options = {...(typeof opt === 'string' || opt instanceof URL ?
          {href: String(opt)} : opt || {}), ...settings};
        const headers = Object.create(null);
        for (const [name, value] of Object.entries(options.headers || {})) {
          serverModule.validateHeaderName(name); serverModule.validateHeaderValue(name, value);
          headers[name.toLowerCase()] = value;
        }
        let upgradeTimeout = Number(options.timeout) || 0;
        let upgradeAbort;
        const closeUpgrade = () => {
          if (req._closed) return;
          req._closed = true;
          if (timeoutId !== null) { clearTimeout(timeoutId); timeoutId = null; }
          if (upgradeAbort) options.signal.removeEventListener('abort', upgradeAbort);
          req.emit('close');
        };
        req.setHeader = (name, value) => {
          if (finished) throw moduleError('ERR_HTTP_HEADERS_SENT', 'Headers already sent');
          serverModule.validateHeaderName(name); serverModule.validateHeaderValue(name, value);
          headers[name.toLowerCase()] = value; return req;
        };
        req.getHeader = name => headers[String(name).toLowerCase()];
        req.getHeaders = () => ({...headers});
        req.hasHeader = name => Object.hasOwn(headers, String(name).toLowerCase());
        req.removeHeader = name => {
          if (finished) throw moduleError('ERR_HTTP_HEADERS_SENT', 'Headers already sent');
          delete headers[String(name).toLowerCase()];
        };

        const cancelPendingRequest = () => {
          aborted = true;
          bodyChunks.length = 0;
          if (req.res) req.res._abort();
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
          upgradeTimeout = delay;
          if (typeof onTimeout === 'function') req.once('timeout', onTimeout);
          if (req._upgradeTransport || isHttpUpgradeRequest(headers)) {
            req._upgradeTransport?.setTimeout(delay); return req;
          }
          if (!(delay > 0)) {
            return req;
          }
          timeoutId = setTimeout(() => {
            if (aborted) return;
            cancelPendingRequest();
            const err = new Error('Request timeout');
            err.code = 'ETIMEDOUT';
            req.emit('timeout');
            req.emit('error', err);
          }, delay);
          return req;
        };
        req.abort = () => {
          if (req._upgradeTransport) {
            if (!req.aborted) { req.aborted = true; req.emit('abort'); req.destroy(); }
            return req;
          }
          cancelPendingRequest();
          return req;
        };
        req.destroy = (err) => {
          if (req._upgradeTransport) {
            if (req.destroyed || req._upgradeTransport.detached) return req;
            req.destroyed = true; aborted = true;
            req._upgradeTransport.destroy();
            queueMicrotask(() => { if (err) req.emit('error', err); closeUpgrade(); });
            return req;
          }
          cancelPendingRequest();
          if (err) {
            req.emit('error', err);
          }
          return req;
        };
        req.end = (chunk, encoding, callback) => {
          if (typeof chunk === 'function') { callback = chunk; chunk = undefined; }
          else if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
          if (finished || aborted) {
            return req;
          }
          finished = true;
          if (chunk != null) {
            req.write(chunk, encoding);
          }
          if (isHttpUpgradeRequest(headers)) {
            req.writableEnded = true;
            if (callback) req.once('finish', callback);
            if (timeoutId !== null) { clearTimeout(timeoutId); timeoutId = null; }
            try {
              req._upgradeTransport = startHttpUpgradeRequest(netModule, serverModule, {
                url: buildRequestUrl(options), method: String(options.method || 'GET').toUpperCase(),
                headers, body: Buffer.concat(bodyChunks.map(part =>
                  typeof part === 'string' ? Buffer.from(part) : part)),
                settings: options, timeout: upgradeTimeout,
                getMaxHeadersCount: () => req.maxHeadersCount,
              }, {
                socket: socket => { req.socket = req.connection = socket; req.emit('socket', socket); },
                finish: () => { req.writableFinished = true; req.emit('finish'); },
                information: response => { if (response.statusCode === 100) req.emit('continue'); req.emit('information', response); },
                response: response => { req.res = response; req.emit('response', response); },
                upgrade: (response, socket, head) => {
                  req.res = response;
                  if (!req.emit('upgrade', response, socket, head)) socket.destroy();
                },
                error: reason => req.destroy(reason), close: closeUpgrade,
                timeout: () => req.emit('timeout'),
              });
              if (options.signal) {
                upgradeAbort = () => req.destroy(options.signal.reason ||
                  moduleError('ABORT_ERR', 'The operation was aborted'));
                if (options.signal.aborted) upgradeAbort();
                else options.signal.addEventListener('abort', upgradeAbort, {once: true});
              }
            } catch (reason) {
              aborted = true; req.destroyed = true;
              queueMicrotask(() => { req.emit('error', reason); closeUpgrade(); });
            }
            bodyChunks.length = 0;
            return req;
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
              let bodyBuffer = Buffer.alloc(0);
              if (method !== 'GET' && method !== 'HEAD' && bodyChunks.length) {
                bodyBuffer = Buffer.concat(bodyChunks.map(part =>
                    typeof part === 'string' ? Buffer.from(part) : part));
              }
              // The encoded request owns its payload after submission. Do not
              // keep the caller's queued upload snapshots on the request.
              bodyChunks.length = 0;
              // Node requests use the browser network service with its cookie
              // jar and HTTP cache disabled. This preserves caller headers and
              // avoids applying renderer CORS to a Node networking API.
              const response = await transport.invoke('__xenon:net-request', {
                url: buildRequestUrl(options),
                method,
                headers: {...headers},
                bodyBase64: bodyBuffer.toString('base64'),
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
              const incoming = new serverModule.IncomingMessage();
              // Preserve the buffered client's existing automatic delivery;
              // an explicit pause in the response handler still takes effect.
              incoming._flow = true;
              incoming.statusCode = Number(response && response.statusCode) || 0;
              incoming.statusMessage =
                  String(response && response.statusMessage || '');
              incoming.headers = Object.assign({}, response && response.headers);
              incoming.url = String(response && response.finalUrl || '');
              incoming.setEncoding = (encoding) => {
                // Client responses arrive as one complete native buffer, so
                // they do not need the server request's streaming decoder.
                incoming._encoding = String(encoding || 'utf8');
                return incoming;
              };
              req.res = incoming;
              req.emit('response', incoming);
              queueMicrotask(() => {
                if (aborted || incoming.destroyed) return;
                if (responseBody.length) {
                  incoming._push(responseBody);
                }
                if (aborted || incoming.destroyed) return;
                incoming._finish();
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
        ...(norm.replace(/^node:/, '') === 'http' ? serverModule : {}),
        request: (opt, settings, cb) => createClientRequest(opt, settings, cb),
        get: (url, settings, cb) => {
          const req = createClientRequest(url, settings, cb);
          req.end();
          return req;
        },
        ...(norm.replace(/^node:/, '') === 'https' ? {createServer: () => {
          throw moduleError('ERR_NOT_SUPPORTED', 'https.createServer is not supported by this runtime');
        }} : {}),
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
        if (options.dictionary) options = {...options,
          dictionary: Buffer.from(options.dictionary).toString('base64')};
        const request = {operation, dataBase64: data.toString('base64'), options};
        if (asynchronous) {
          return transport.invoke('__xenon:zlib', request)
              .then(value => Buffer.from(value, 'base64'));
        }
        return Buffer.from(transport.sendSync('__xenon:zlib', request), 'base64');
      }, {
        Transform: streamModule.Transform,
        streamCall(request, asynchronous) {
          request = {...request};
          if (request.data) request.data = Buffer.from(request.data).toString('base64');
          if (request.options?.dictionary) request.options = {...request.options,
            dictionary: Buffer.from(request.options.dictionary).toString('base64')};
          const decode = value => request.action === 'process'
            ? {...value, output: Buffer.from(value.output, 'base64')} : value;
          return asynchronous
            ? transport.invoke('__xenon:zlib-stream', request).then(decode)
            : decode(transport.sendSync('__xenon:zlib-stream', request));
        },
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

  const electronRequire = globalThis.require = function(request, parentFile = globalThis.__filename, parentModule) {
    const builtin = builtinModuleId(request);
    if (builtin) {
      if (!builtinModuleCache.has(builtin)) {
        builtinModuleCache.set(builtin, loadBuiltinModule(builtin));
      }
      return builtinModuleCache.get(builtin);
    }
    parentFile = moduleFilenameFromSource(parentFile);
    parentModule ||= hostedModuleParent(parentFile);
    const resolutionContext = {};
    const resolved = resolveHostedCjs(request, parentFile, resolutionContext, parentModule);
    if (builtinModuleId(resolved)) return electronRequire(resolved, parentFile);
    const exports = loadHostedCjsModule(resolved, undefined, false, resolutionContext.packageScope);
    const cacheKey = hostedResolutionCacheKey(parentModule, request);
    if (cacheKey) moduleResolutionCache.set(cacheKey, resolved);
    return exports;
  };

  globalThis.Buffer = Buffer;
  globalThis.require.resolve = request => resolveHostedCjs(
      request, moduleFilenameFromSource(globalThis.__filename));
  globalThis.require.resolve.paths = request => moduleModule._resolveLookupPaths(
      request, hostedModuleParent(moduleFilenameFromSource(globalThis.__filename)));
  globalThis.require.cache = hostedCjsCache;
  globalThis.require.main = undefined;
  globalThis.require.extensions = { '.js': () => {}, '.json': () => {}, '.node': () => {} };
  globalThis.__xenonElectronRequire = globalThis.require;
