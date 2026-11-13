  // Synchronous module.registerHooks chains owned by one main V8 context.
  // Native CJS/ESM loaders supply their real default resolver/source reader.
  function createSynchronousModuleHooks({changed = () => {}, decodeSource} = {}) {
    const resolveHooks = [];
    const loadHooks = [];
    const registrations = new WeakMap();
    const arrayBufferLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;
    const sharedBufferLength = typeof SharedArrayBuffer === 'function' ?
      Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, 'byteLength').get : undefined;
    const sourceIsBuffer = value => {
      if (ArrayBuffer.isView(value)) return true;
      try { arrayBufferLength.call(value); return true; } catch {}
      if (sharedBufferLength) {
        try { sharedBufferLength.call(value); return true; } catch {}
      }
      return false;
    };
    function invalidResult(hook, field, expected) {
      return Object.assign(new TypeError(`The ${hook} hook must return ${expected} in ${field}`),
        {code: 'ERR_INVALID_RETURN_PROPERTY_VALUE'});
    }
    class ModuleHooks {
      constructor(resolve, load) {
        this.resolve = resolve;
        this.load = load;
        const record = {resolve, load};
        registrations.set(this, record);
        if (resolve) resolveHooks.push(record);
        if (load) loadHooks.push(record);
        Object.freeze(this);
      }
      deregister() {
        const record = registrations.get(this);
        if (!record) return;
        let removed = false;
        for (const hooks of [resolveHooks, loadHooks]) {
          const index = hooks.indexOf(record);
          if (index >= 0) { hooks.splice(index, 1); removed = true; }
        }
        if (removed) changed();
      }
    }
    function registerHooks(hooks) {
      const {resolve, load} = hooks;
      for (const [name, hook] of [['resolve', resolve], ['load', load]]) {
        if (hook && typeof hook !== 'function')
          throw Object.assign(new TypeError(`hooks.${name} must be a function`), {code: 'ERR_INVALID_ARG_TYPE'});
      }
      const registered = new ModuleHooks(resolve, load);
      if (resolve || load) changed();
      return registered;
    }
    function validateResolve(_specifier, result) {
      const {url, format, importAttributes} = result;
      if (typeof url !== 'string') throw invalidResult('resolve', 'url', 'a URL string');
      if (format && typeof format !== 'string') throw invalidResult('resolve', 'format', 'a string');
      if (importAttributes && typeof importAttributes !== 'object')
        throw invalidResult('resolve', 'importAttributes', 'an object');
      return {__proto__: null, url, format, importAttributes};
    }
    function validateLoad(url, result) {
      const {source, format} = result;
      if (format !== 'builtin' && !url.startsWith('node:') && typeof source !== 'string' &&
          !sourceIsBuffer(source) && format !== 'addon')
        throw invalidResult('load', 'source', 'a string, ArrayBuffer, or ArrayBuffer view');
      if (format !== undefined && typeof format !== 'string')
        throw invalidResult('load', 'format', 'a string');
      return result;
    }
    function execute(hooks, name, argument, context, defaultStep, validate) {
      // Capture the chain when a load starts: a hook may deregister itself or
      // install another hook while it is running. Those changes affect later
      // loads, never invalidate the currently executing next function.
      let step = (value, updates) => {
        if (updates && updates !== context) Object.assign(context, updates);
        return validate(value, defaultStep(value, context));
      };
      for (const record of hooks.slice()) {
        const next = step;
        const hook = record[name];
        step = (value, updates) => {
          if (updates && updates !== context) Object.assign(context, updates);
          let continued = false;
          const result = hook(value, context, (specifier, nextContext) => {
            continued = true;
            return next(specifier, nextContext);
          });
          if (!continued && !result.shortCircuit)
            throw invalidResult(name, 'shortCircuit', 'true when next is not called');
          return validate(value, result);
        };
      }
      return step(argument, context);
    }
    return {
      registerHooks,
      hasResolveHooks: () => resolveHooks.length !== 0,
      hasLoadHooks: () => loadHooks.length !== 0,
      resolve(specifier, context, defaultResolve) {
        if (!resolveHooks.length) return defaultResolve(specifier, context);
        return execute(resolveHooks, 'resolve', specifier, {...context}, defaultResolve, validateResolve);
      },
      load(url, context, defaultLoad) {
        if (!loadHooks.length) return defaultLoad(url, context);
        const result = execute(loadHooks, 'load', url, {...context}, defaultLoad, validateLoad);
        if (sourceIsBuffer(result.source) &&
            [undefined, 'module', 'commonjs', 'json', 'module-typescript',
              'commonjs-typescript', 'typescript'].includes(result.format)) {
          result.source = decodeSource(result.source);
        }
        return result;
      },
    };
  }
