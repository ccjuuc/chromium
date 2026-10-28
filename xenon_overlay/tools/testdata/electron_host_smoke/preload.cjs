'use strict';
const {contextBridge, ipcRenderer} = require('electron');
if (process.type === 'service-worker') {
  const url = contextBridge.executeInMainWorld({func: () => globalThis.location.href});
  if (url === 'http://127.0.0.1:9339/worker.js') {
    contextBridge.exposeInMainWorld('smokeWorkerHost', {
      probe: () => ipcRenderer.invoke('smoke:worker-probe'),
      timers: async () => {
        const order = [];
        let cancelledCallbackRan = false;
        clearTimeout(setTimeout(() => { cancelledCallbackRan = true; }, 0));
        clearInterval(setInterval(() => { cancelledCallbackRan = true; }, 1));
        let invalidCallbackRejected = false;
        try { queueMicrotask(null); }
        catch (error) { invalidCallbackRejected = error.name === 'TypeError'; }
        const argument = {};
        const receiverAndArguments = await new Promise(resolve => {
          queueMicrotask(() => order.push('microtask'));
          Promise.resolve().then(() => order.push('promise'));
          setTimeout(function(value) {
            order.push('timeout');
            resolve(this === globalThis && value === argument);
          }, 0, argument);
        });
        let ticks = 0;
        await new Promise(resolve => {
          const timer = setInterval(() => {
            if (++ticks === 2) {
              // Timeout and interval cancellation share the browser ID pool.
              clearTimeout(timer);
              resolve();
            }
          }, 1);
        });
        await new Promise(resolve => setTimeout(resolve, 20));
        return receiverAndArguments && invalidCallbackRejected &&
            order.join(',') === 'microtask,promise,timeout' &&
            ticks === 2 && !cancelledCallbackRan;
      },
      isolated: process.contextIsolated === true,
    });
  }
} else if (globalThis.location.href === 'http://127.0.0.1:9339/index.html') {
  contextBridge.exposeInMainWorld('smokeHost', {
    ready: () => ipcRenderer.invoke('smoke:frame-ready'),
    enableThrottling: () => ipcRenderer.invoke('smoke:enable-throttling'),
    showWindow: () => ipcRenderer.invoke('smoke:show-window'),
    report: (check, ok) => ipcRenderer.send('smoke:page-report', check, ok),
    snapshot: () => ipcRenderer.invoke('smoke:snapshot'),
    beginShutdownNavigation: () => ipcRenderer.send('smoke:begin-shutdown-navigation'),
    isolated: process.contextIsolated === true,
  });
}
