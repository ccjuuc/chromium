'use strict';
globalThis.__xenonSmokeResults = [];
globalThis.__xenonSmokeDone = false;
const expectedBytes = [0, 1, 127, 128, 254, 255, 83, 77, 79, 75, 69];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function report(check, ok) {
  const result = {check, ok: ok === true};
  __xenonSmokeResults.push(result);
  const row = document.createElement('li');
  row.className = result.ok ? 'pass' : 'fail';
  row.textContent = (result.ok ? 'PASS ' : 'FAIL ') + check;
  document.getElementById('results').appendChild(row);
  console.log('XENON_GAME_SMOKE_PAGE ' + JSON.stringify(result));
  globalThis.smokeHost?.report(check, result.ok);
  return result.ok;
}
async function visibility(expected) {
  const deadline = performance.now() + 5000;
  while (document.visibilityState !== expected && performance.now() < deadline) await delay(100);
  return document.visibilityState === expected;
}
async function testWorker() {
  const registration = await navigator.serviceWorker.register('/worker.js', {scope: '/'});
  const ready = await navigator.serviceWorker.ready;
  const worker = ready.active || registration.active;
  if (!worker) throw new Error('NO_ACTIVE_SMOKE_WORKER');
  const result = await new Promise((resolve, reject) => {
    const channel = new MessageChannel();
    const timeout = setTimeout(() => reject(new Error('SMOKE_WORKER_TIMEOUT')), 8000);
    channel.port1.onmessage = event => {
      clearTimeout(timeout);
      channel.port1.close();
      resolve(event.data);
    };
    worker.postMessage({kind: 'smoke-probe'}, [channel.port2]);
  });
  report('worker-preload', result.preload === true);
  report('worker-isolation', result.isolation === true);
  report('worker-identity', result.identity === true);
  report('worker-timers', result.timers === true);
}
(async () => {
  if (!globalThis.smokeHost) {
    report('frame-isolation', false);
    throw new Error('SMOKE_PRELOAD_MISSING');
  }
  await smokeHost.ready();
  report('frame-isolation', smokeHost.isolated === true &&
      typeof require === 'undefined' && typeof process === 'undefined' &&
      typeof xenonIpcRenderer === 'undefined' && typeof __xenonElectronRequire === 'undefined');
  report('hidden-with-throttling-disabled', await visibility('visible'));
  try {
    const response = await fetch('xenonsmoke://fixture/binary');
    const bytes = new Uint8Array(await response.arrayBuffer());
    let unsafeHeaderRejected = false;
    try {
      await fetch('xenonsmoke://fixture/binary', {headers: {'X-Smoke-Unsafe': '1'}});
    } catch { unsafeHeaderRejected = true; }
    report('protocol-binary', response.status === 200 && bytes.length === expectedBytes.length &&
        bytes.every((value, index) => value === expectedBytes[index]) && unsafeHeaderRejected);
    const details = {status: response.status, type: response.type,
      byteLength: bytes.length, bytes: [...bytes.subarray(0, 32)], unsafeHeaderRejected};
    __xenonSmokeResults[__xenonSmokeResults.length - 1].details = details;
    console.log('XENON_GAME_SMOKE_PROTOCOL ' + JSON.stringify(details));
  } catch (error) {
    report('protocol-binary', false);
    const details = {error: String(error?.message || error).slice(0, 300),
      name: String(error?.name || '').slice(0, 80)};
    __xenonSmokeResults[__xenonSmokeResults.length - 1].details = details;
    console.log('XENON_GAME_SMOKE_PROTOCOL ' + JSON.stringify(details));
  }

  await smokeHost.enableThrottling();
  report('hidden-with-throttling-enabled', await visibility('hidden'));
  await smokeHost.showWindow();
  report('reshown-visible', await visibility('visible'));
  try { await testWorker(); }
  catch {
    report('worker-preload', false);
    report('worker-isolation', false);
    report('worker-identity', false);
    report('worker-timers', false);
  }
})().catch(() => report('frame-flow', false)).finally(() => {
  const ok = __xenonSmokeResults.length >= 9 && __xenonSmokeResults.every(item => item.ok);
  report('page-complete', ok);
  __xenonSmokeDone = true;
  document.getElementById('status').textContent = ok ? 'All page checks passed.' : 'A check failed. Inspect the fixed JSON markers.';
});
