  // Both loaders own their resolution and cache. This module binds createRequire
  // to a loader filename and exposes the runtime's compile-cache capabilities.
  function createModuleModule(pathModule, urlModule, createRequireForFilename, isBuiltinModule) {
    const compileCacheStatus = Object.freeze({
      FAILED: 0,
      ENABLED: 1,
      ALREADY_ENABLED: 2,
      DISABLED: 3,
    });
    return {
      // This application-scoped runtime has no implicit global module roots.
      // Like Node, mutating this published snapshot does not change lookups.
      globalPaths: [],
      _nodeModulePaths(from) {
        if (typeof from !== 'string') {
          throw Object.assign(new TypeError('The module path must be a string'),
                              {code: 'ERR_INVALID_ARG_TYPE'});
        }
        const paths = [];
        let directory = pathModule.resolve(from);
        for (;;) {
          if (pathModule.basename(directory) !== 'node_modules') {
            paths.push(pathModule.join(directory, 'node_modules'));
          }
          const parent = pathModule.dirname(directory);
          if (parent === directory) return paths;
          directory = parent;
        }
      },
      _resolveLookupPaths(request, parent) {
        if (isBuiltinModule?.(request)) return null;
        const relative = pathModule.sep === '\\' ?
            /^\.\.?([\\/]|$)/.test(request) : /^\.\.?(\/|$)/.test(request);
        if (relative) return [parent?.id && parent.filename ? pathModule.dirname(parent.filename) : '.'];
        return parent?.paths?.length ? [...parent.paths] : [];
      },
      isBuiltin(name) {
        return typeof name === 'string' && isBuiltinModule(name);
      },
      constants: Object.freeze({compileCacheStatus}),
      getCompileCacheDir() {
        return undefined;
      },
      flushCompileCache() {
        // Like Node with caching disabled, there is no pending cache to flush.
      },
      enableCompileCache() {
        // A CommonJS module cache is not Node's persistent V8 compile cache.
        // Report the unsupported optimization without affecting module loads.
        return {
          status: compileCacheStatus.DISABLED,
          message: 'Persistent module compile caching is not supported by this runtime',
        };
      },
      createRequire(filename) {
        const invalidFilename = () => Object.assign(new TypeError(
            'The argument \'filename\' must be a file URL object, file URL string, or absolute path string'),
            {code: 'ERR_INVALID_ARG_VALUE'});
        const windows = pathModule.sep === '\\';
        let parent = filename;
        if (typeof parent !== 'string' || !pathModule.isAbsolute(parent)) {
          const URLConstructor = urlModule.URL;
          if (typeof parent !== 'string' &&
              !(typeof URLConstructor === 'function' && parent instanceof URLConstructor)) {
            throw invalidFilename();
          }
          try {
            parent = urlModule.fileURLToPath(parent, {windows});
          } catch (_error) {
            // Node reports malformed URLs and non-file schemes as invalid
            // createRequire arguments, without leaking url module error codes.
            throw invalidFilename();
          }
          if (typeof parent !== 'string' || !pathModule.isAbsolute(parent)) {
            throw invalidFilename();
          }
        }
        // A trailing separator explicitly names a directory. Otherwise Node
        // treats the argument as a filename even when it happens to be a dir.
        if (parent.endsWith('/') || (windows && parent.endsWith('\\'))) {
          parent = pathModule.join(parent, 'noop.js');
        }
        return createRequireForFilename(parent);
      },
    };
  }
