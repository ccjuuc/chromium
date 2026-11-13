  // Shared promise helpers for the main and renderer EventEmitter modules.
  EventEmitter.once = async function once(emitter, name, options = {}) {
    const invalidArgument = (argument, type) => Object.assign(
        new TypeError(`The "${argument}" argument must be ${type}`),
        {code: 'ERR_INVALID_ARG_TYPE'});
    if (options === null || typeof options !== 'object' ||
        Array.isArray(options)) {
      throw invalidArgument('options', 'of type object');
    }
    const {signal} = options;
    if (signal !== undefined &&
        (signal === null || typeof signal !== 'object' ||
         !('aborted' in signal))) {
      throw invalidArgument('options.signal', 'an instance of AbortSignal');
    }
    const abortError = () => Object.assign(
        new Error('The operation was aborted', {cause: signal.reason}),
        {name: 'AbortError', code: 'ABORT_ERR'});
    if (signal?.aborted)
      throw abortError();

    return new Promise((resolve, reject) => {
      const nodeEmitter = typeof emitter.on === 'function';
      if (!nodeEmitter && typeof emitter.addEventListener !== 'function') {
        throw invalidArgument('emitter', 'an instance of EventEmitter');
      }
      let settled = false;
      let eventRegistered = false;
      let errorRegistered = false;
      let abortRegistered = false;
      const cleanup = () => {
        if (eventRegistered) {
          if (nodeEmitter)
            emitter.removeListener(name, resolver);
          else
            emitter.removeEventListener(name, resolver);
        }
        if (errorRegistered)
          emitter.removeListener('error', errorListener);
        if (abortRegistered)
          signal.removeEventListener('abort', abortListener);
      };
      const settle = (callback, value) => {
        if (settled)
          return;
        settled = true;
        cleanup();
        callback(value);
      };
      const resolver = (...args) => settle(resolve, args);
      const errorListener = error => settle(reject, error);
      const abortListener = () => settle(reject, abortError());
      try {
        eventRegistered = true;
        if (nodeEmitter)
          emitter.once(name, resolver);
        else
          emitter.addEventListener(name, resolver, {once: true});
        if (!settled && nodeEmitter && name !== 'error') {
          errorRegistered = true;
          emitter.once('error', errorListener);
        }
        if (!settled && signal !== undefined) {
          abortRegistered = true;
          signal.addEventListener('abort', abortListener, {once: true});
          // A newListener callback may have aborted during registration.
          if (signal.aborted)
            abortListener();
        }
        // Custom emitters can settle synchronously while registering. Their
        // listener may only have been inserted after the first cleanup ran.
        if (settled)
          cleanup();
      } catch (error) {
        cleanup();
        reject(error);
      }
    });
  };
