  // Both loaders own their resolution and cache. This module only converts the
  // Node createRequire input into the filename to which that loader is bound.
  function createModuleModule(pathModule, urlModule, createRequireForFilename) {
    return {
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
