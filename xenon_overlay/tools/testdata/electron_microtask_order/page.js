'use strict';

(() => {
  const contextIsolation = new URLSearchParams(location.search).get('contextIsolation') === '1';
  const result = globalThis.__xenonMicrotaskResult = {
    suite: 'electron-microtask-order',
    contextIsolation,
    actualContextIsolation: null,
    done: false,
    ok: false,
    checks: [],
    errors: [],
  };
  globalThis.__xenonMicrotaskDone = false;
  document.title += contextIsolation ? ': isolated' : ': shared';

  function publish() {
    document.getElementById('result').textContent = JSON.stringify(result, null, 2);
  }

  function runCase(transport) {
    const entry = {name: transport, done: false, ok: false, order: [], error: null};
    result.checks.push(entry);
    return new Promise(resolve => {
      function finish(error) {
        if (entry.done) return;
        clearTimeout(timeout);
        entry.done = true;
        entry.error = error || null;
        entry.ok = !entry.error;
        publish();
        resolve();
      }
      const timeout = setTimeout(() => {
        finish('Timed out waiting for the native ' + transport + ' reply');
      }, 15000);

      try {
        globalThis.microtaskOrderHost.request(transport, payload => {
          if (entry.done) return;
          entry.order.push('callback');
          let cache = new Map();
          Promise.resolve().then(() => {
            entry.order.push('microtask');
            cache = null;
          });

          let failure = null;
          try {
            if (payload.transport !== transport || payload.error) {
              throw new Error(payload.error || 'Native reply used the wrong transport');
            }
            // HTML parsing enters Blink's script/microtask scopes synchronously.
            // A Promise queued by this listener must not clear its cache yet.
            const parsed = new DOMParser().parseFromString(
                '<!doctype html><html><head><title>probe</title></head>' +
                '<body><b>probe</b></body></html>', 'text/html');
            entry.order.push('after-dom');
            cache.set('html', parsed.body.textContent);
            if (cache.get('html') !== 'probe') {
              throw new Error('The synchronous HTML parse did not complete');
            }
            entry.order.push('cache-written');
          } catch (error) {
            failure = String(error);
          } finally {
            entry.order.push('callback-end');
          }

          // Observe the completed native event turn, without forcing a checkpoint.
          setTimeout(() => {
            const expected = 'callback,after-dom,cache-written,callback-end,microtask';
            finish(failure || (entry.order.join(',') !== expected
              ? 'Unexpected callback/microtask order: ' + entry.order.join(',')
              : null));
          }, 0);
        });
      } catch (error) {
        finish(String(error));
      }
    });
  }

  async function run() {
    try {
      const host = globalThis.microtaskOrderHost;
      if (!host || typeof host.request !== 'function') {
        throw new Error('The native preload API was not installed');
      }
      result.actualContextIsolation = host.contextIsolated;
      if (host.contextIsolated !== contextIsolation) {
        throw new Error('The document did not use the requested context isolation');
      }
      await Promise.all(['send', 'postMessage'].map(runCase));
      result.ok = result.checks.length === 2 && result.checks.every(entry => entry.ok);
    } catch (error) {
      result.errors.push(String(error));
    } finally {
      result.done = true;
      globalThis.__xenonMicrotaskDone = true;
      publish();
      console.log('XENON_MICROTASK_ORDER ' + JSON.stringify(result));
    }
  }
  run();
})();
