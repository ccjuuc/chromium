'use strict';
self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('message', event => {
  if (event.data?.kind !== 'smoke-probe' || !event.ports[0]) return;
  event.waitUntil((async () => {
    const result = {
      preload: typeof globalThis.smokeWorkerHost?.probe === 'function',
      nativeTransportAbsent: typeof require === 'undefined' &&
          typeof process === 'undefined' && typeof __xenonWorker === 'undefined',
      isolation: globalThis.smokeWorkerHost?.isolated === true &&
          typeof require === 'undefined' && typeof process === 'undefined' &&
          typeof __xenonWorker === 'undefined',
      identity: false,
      timers: false,
    };
    if (result.preload) {
      // Browser's running-status event installs handlers before this message
      // in normal startup. Allow a bounded initial delivery race only.
      for (let attempt = 0; attempt < 5; ++attempt) {
        try {
          result.identity = (await smokeWorkerHost.probe()).ok === true;
          break;
        } catch (error) {
          if (!String(error?.message).includes('No handler registered')) break;
          await new Promise(resolve => setTimeout(resolve, 100));
        }
      }
      try { result.timers = await smokeWorkerHost.timers(); }
      catch (error) { console.error('SMOKE_WORKER_TIMER_FAILURE', error); }
    }
    event.ports[0].postMessage(result);
    event.ports[0].close();
  })());
});
