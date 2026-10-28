'use strict';
// Select only the local fixture page on the dedicated debugging port.
const fs = require('node:fs');
const path = require('node:path');
const endpoint = 'http://127.0.0.1:9223';
const command = process.argv[2] || 'inspect';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function call(url, method, params = {}) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url);
    const timer = setTimeout(() => {
      socket.close();
      reject(new Error('CDP_TIMEOUT_' + method));
    }, 15000);
    socket.onopen = () => socket.send(JSON.stringify({id: 1, method, params}));
    socket.onmessage = message => {
      const response = JSON.parse(message.data);
      if (response.id !== 1) return;
      clearTimeout(timer);
      socket.close();
      if (response.error) reject(new Error('CDP_METHOD_FAILED_' + method));
      else resolve(response.result);
    };
    socket.onerror = () => { clearTimeout(timer); reject(new Error('CDP_SOCKET_FAILED')); };
    socket.onclose = () => {
      if (method === 'Browser.close') { clearTimeout(timer); resolve({closed: true}); }
    };
  });
}
async function pages() {
  const targets = await (await fetch(endpoint + '/json/list')).json();
  return targets.filter(page => page.type === 'page' &&
      (page.url === 'http://127.0.0.1:9339/index.html' ||
       page.url === 'http://localhost:9339/delayed-navigation'));
}
async function inspect(page) {
  const expression = `({done: globalThis.__xenonSmokeDone === true,
    visibility: document.visibilityState, ready: document.readyState,
    bridge: typeof globalThis.smokeHost, require: typeof require,
    process: typeof process, transport: typeof xenonIpcRenderer,
    results: globalThis.__xenonSmokeResults || []})`;
  const reply = await call(page.webSocketDebuggerUrl, 'Runtime.evaluate', {
    expression, returnByValue: true,
  });
  if (reply.exceptionDetails) throw new Error('CDP_INSPECT_FAILED');
  return reply.result.value;
}
async function nativeSnapshot(page) {
  const reply = await call(page.webSocketDebuggerUrl, 'Runtime.evaluate', {
    expression: 'smokeHost.snapshot()', awaitPromise: true, returnByValue: true,
  });
  return reply.result?.value || {nativeSnapshotUnavailable: true};
}
async function ordinaryWorkerSnapshot() {
  const version = await (await fetch(endpoint + '/json/version')).json();
  const browser = version.webSocketDebuggerUrl;
  // A browser-created tab uses the default partition, without app preloads.
  const {targetId} = await call(browser, 'Target.createTarget', {
    url: 'http://127.0.0.1:9339/health',
  });
  try {
    let page;
    for (let attempt = 0; attempt < 30; ++attempt) {
      const targets = await (await fetch(endpoint + '/json/list')).json();
      page = targets.find(target => target.id === targetId &&
          target.url === 'http://127.0.0.1:9339/health');
      if (page) break;
      await delay(100);
    }
    if (!page) throw new Error('ORDINARY_WORKER_PAGE_NOT_READY');
    const reply = await call(page.webSocketDebuggerUrl, 'Runtime.evaluate', {
      expression: `(async () => {
        const registration = await navigator.serviceWorker.register('/worker.js');
        try {
          const ready = await navigator.serviceWorker.ready;
          return await new Promise((resolve, reject) => {
            const channel = new MessageChannel();
            const timer = setTimeout(() => {
              channel.port1.close(); reject(new Error('Worker reply timeout'));
            }, 5000);
            channel.port1.onmessage = event => {
              clearTimeout(timer); channel.port1.close(); resolve(event.data);
            };
            ready.active.postMessage({kind: 'smoke-probe'}, [channel.port2]);
          });
        } finally {
          await registration.unregister();
        }
      })()`,
      awaitPromise: true, returnByValue: true,
    });
    if (reply.exceptionDetails) throw new Error('ORDINARY_WORKER_PROBE_FAILED');
    return reply.result.value;
  } finally {
    await call(browser, 'Target.closeTarget', {targetId});
  }
}
(async () => {
  if (command === 'close') {
    const version = await (await fetch(endpoint + '/json/version')).json();
    console.log(JSON.stringify(await call(version.webSocketDebuggerUrl, 'Browser.close')));
    return;
  }
  if (command === 'wait') {
    const deadline = Date.now() + 45000;
    let snapshot = {done: false, results: []};
    let selectedPage;
    while (Date.now() < deadline) {
      try {
        const page = (await pages())[0];
        if (page) { selectedPage = page; snapshot = await inspect(page); }
      } catch { /* The browser may still be starting. */ }
      if (snapshot.done) {
        try { snapshot.native = await nativeSnapshot(selectedPage); }
        catch { snapshot.native = {nativeSnapshotUnavailable: true}; }
        const nativePassed = snapshot.native.results?.some(
            item => item.check === 'suite-complete' && item.ok);
        try { snapshot.ordinaryWorker = await ordinaryWorkerSnapshot(); }
        catch (error) { snapshot.ordinaryWorker = {error: error.message}; }
        const ordinaryWorkerPassed = snapshot.ordinaryWorker.preload === false &&
            snapshot.ordinaryWorker.nativeTransportAbsent === true;
        if (nativePassed) {
          const navigation = await call(selectedPage.webSocketDebuggerUrl, 'Runtime.evaluate', {
            expression: 'smokeHost.beginShutdownNavigation()', returnByValue: true,
          });
          snapshot.shutdownNavigationRequested = !navigation.exceptionDetails;
        }
        console.log(JSON.stringify(snapshot));
        process.exitCode = snapshot.results.length > 0 &&
            snapshot.results.every(item => item.ok === true) &&
            nativePassed && ordinaryWorkerPassed &&
            snapshot.shutdownNavigationRequested ? 0 : 1;
        return;
      }
      await delay(250);
    }
    console.log(JSON.stringify({...snapshot, timedOut: true}));
    process.exitCode = 1;
    return;
  }
  const selected = await pages();
  if (!selected.length) throw new Error('SMOKE_PAGE_NOT_FOUND');
  for (const page of selected) {
    if (command === 'screenshot') {
      const result = await call(page.webSocketDebuggerUrl, 'Page.captureScreenshot');
      const output = path.join(process.cwd(), 'smoke-result.png');
      fs.writeFileSync(output, Buffer.from(result.data, 'base64'));
      console.log(output);
    } else if (command === 'inspect') {
      console.log(JSON.stringify(await inspect(page)));
    } else if (command === 'native') {
      console.log(JSON.stringify(await nativeSnapshot(page)));
    } else if (command === 'list') {
      console.log(JSON.stringify({id: page.id, url: page.url, title: page.title}));
    } else {
      throw new Error('UNKNOWN_SMOKE_CDP_COMMAND');
    }
  }
})().catch(error => {
  console.error(error.message);
  process.exitCode = 1;
});
