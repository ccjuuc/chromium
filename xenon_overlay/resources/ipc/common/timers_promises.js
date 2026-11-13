  // Promise timers share the host's actual timer queue. Browser timers have no
  // process-reference controls: reject ref:false there rather than pretending
  // to unref a timer that still keeps native work alive.
  function createTimersPromises(timers) {
    const invalid = (name, type) => Object.assign(
        new TypeError(`The "${name}" argument must be ${type}`),
        {code: 'ERR_INVALID_ARG_TYPE'});
    const abortError = signal => Object.assign(new Error('The operation was aborted',
        {cause: signal.reason}), {name: 'AbortError', code: 'ABORT_ERR'});
    function optionsOf(options = {}) {
      if (options === null || typeof options !== 'object') throw invalid('options', 'an object');
      const {signal, ref = true} = options;
      if (typeof ref !== 'boolean') throw invalid('options.ref', 'a boolean');
      if (signal !== undefined && (!signal || typeof signal.aborted !== 'boolean' ||
          typeof signal.addEventListener !== 'function' ||
          typeof signal.removeEventListener !== 'function')) {
        throw invalid('options.signal', 'an AbortSignal');
      }
      return {signal, ref};
    }
    function applyRef(timer, ref) {
      if (ref) return;
      if (timer && typeof timer.unref === 'function') timer.unref();
      else throw Object.assign(new Error('Timer ref:false is unavailable in this host'),
          {code: 'ERR_NOT_SUPPORTED'});
    }
    const timeoutDelay = delay => {
      const number = Number(delay);
      return Number.isFinite(number) && number >= 1 && number <= 2147483647 ?
          Math.trunc(number) : 1;
    };
    async function schedule(set, clear, args, value, options) {
      const {signal, ref} = optionsOf(options);
      if (signal?.aborted) throw abortError(signal);
      return new Promise((resolve, reject) => {
        let timer;
        const cleanup = () => signal?.removeEventListener('abort', aborted);
        const aborted = () => {
          clear(timer);
          cleanup();
          reject(abortError(signal));
        };
        try {
          timer = set(() => { cleanup(); resolve(value); }, ...args);
          applyRef(timer, ref);
          signal?.addEventListener('abort', aborted, {once: true});
          if (signal?.aborted) aborted();
        } catch (error) {
          clear(timer);
          cleanup();
          reject(error);
        }
      });
    }
    const promises = {
      async setTimeout(delay = 1, value, options) {
        return schedule(timers.setTimeout, timers.clearTimeout,
            [timeoutDelay(delay)], value, options);
      },
      async setImmediate(value, options) {
        return schedule(timers.setImmediate, timers.clearImmediate, [], value, options);
      },
      async *setInterval(delay = 1, value, options) {
        const {signal, ref} = optionsOf(options);
        if (signal?.aborted) throw abortError(signal);
        let timer;
        let pending = 0;
        let wake;
        const notify = () => { const callback = wake; wake = undefined; callback?.(); };
        const aborted = () => { timers.clearInterval(timer); notify(); };
        try {
          timer = timers.setInterval(() => { ++pending; notify(); }, timeoutDelay(delay));
          applyRef(timer, ref);
          signal?.addEventListener('abort', aborted, {once: true});
          while (!signal?.aborted) {
            if (pending === 0) await new Promise(resolve => { wake = resolve; });
            if (signal?.aborted) break;
            while (pending > 0 && !signal?.aborted) { --pending; yield value; }
          }
          throw abortError(signal);
        } finally {
          timers.clearInterval(timer);
          signal?.removeEventListener('abort', aborted);
        }
      },
    };
    promises.scheduler = {
      wait(delay, options) { return promises.setTimeout(delay, undefined, options); },
      yield() { return promises.setImmediate(); },
    };
    return promises;
  }
