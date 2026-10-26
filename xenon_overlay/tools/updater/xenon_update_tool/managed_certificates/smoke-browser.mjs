#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Node.js 22+, no npm dependencies. Exercises an explicitly supplied NEW build.
// Never uses --ignore-certificate-errors, Security.setIgnoreCertificateErrors,
// the OS trust store, or an existing browser profile/process.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { constants, createHash, randomUUID, X509Certificate } from 'node:crypto';
import * as fs from 'node:fs/promises';
import https from 'node:https';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const usage = `Usage: node smoke-browser.mjs --browser <absolute chrome.exe path>

Runs a real headless browser with temporary user data and two loopback HTTPS
servers. Checks bootstrap isolation, managed trust in normal/incognito contexts,
legacy elapsed snapshot expiry, offline cache after restart, HTTPS redirect
diagnostics, and a higher-generation empty snapshot revocation.
Requires a browser built with the managed certificate changes and Node.js 22+.
All browser processes and files created by this script are cleaned up on exit.
The checked-in Chromium TLS fixtures must still be within their validity dates.`;
if (process.argv.includes('--help')) {
  console.log(usage);
  process.exit(0);
}
assert.equal(process.argv.length, 4, usage);
assert.equal(process.argv[2], '--browser', usage);
const executable = process.argv[3];
assert(path.isAbsolute(executable), '--browser must be an absolute path');
assert(typeof WebSocket === 'function', 'Node.js 22+ with built-in WebSocket is required');
assert((await fs.stat(executable)).isFile(), 'Browser executable does not exist');

const sourceRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../../..');
const fixtureDirectory = path.join(sourceRoot, 'net', 'data', 'ssl', 'certificates');
const sourceId = 'managed-ca-browser-smoke';
const marker = `managed-ca-smoke-${randomUUID()}`;
const hash = data => createHash('sha256').update(data).digest('hex');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const timestamp = milliseconds => new Date(milliseconds).toISOString().replace(/\.\d{3}Z$/, 'Z');
const temp = await fs.mkdtemp(path.join(os.tmpdir(), 'xenon-managed-ca-smoke-'));
const userData = path.join(temp, 'user-data');
const configPath = path.join(temp, 'managed-ca.json');
const bootstrapPath = path.join(temp, 'bootstrap-ca.pem');
const statePath = path.join(userData, 'ManagedCertificates', hash(sourceId), 'state.json');
const ownedServers = new Set();
let browser;
let cleanupPromise;
let requests = 0;
let pendingResponses = [];
let holdRequests = true;
let servedSnapshot;
let browserLog = '';
let protocolFailure;
let stopping = false;

async function until(description, action, timeout = 30000,
  { checkProtocol = true, fatalError = () => undefined } = {}) {
  const deadline = Date.now() + timeout;
  let lastError;
  while (Date.now() < deadline) {
    if (checkProtocol && stopping) throw new Error('Smoke run is stopping');
    if (checkProtocol && protocolFailure) throw protocolFailure;
    const fatal = fatalError();
    if (fatal) throw fatal;
    try {
      const result = await action();
      if (result) return result;
    } catch (error) {
      lastError = error;
    }
    await delay(100);
  }
  throw new Error(`Timed out: ${description}${lastError ? ` (${lastError.message})` : ''}`);
}

function pemBlock(text, label) {
  const blocks = [...text.matchAll(new RegExp(`-----BEGIN ${label}-----[\\s\\S]*?-----END ${label}-----`, 'g'))];
  assert.equal(blocks.length, 1, `Fixture must contain exactly one ${label} block`);
  return `${blocks[0][0]}\n`;
}

async function startServer(tls, handler, port = 0) {
  assert(!stopping, 'Cannot start a server during cleanup');
  const server = https.createServer(tls, handler);
  server.smokeSockets = new Set();
  server.on('connection', socket => {
    server.smokeSockets.add(socket);
    socket.on('close', () => server.smokeSockets.delete(socket));
  });
  server.on('tlsClientError', () => {}); // Expected for the untrusted probes.
  ownedServers.add(server);
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => {
      server.removeListener('error', reject);
      resolve();
    });
  });
  return server;
}

async function stopServer(server) {
  if (!ownedServers.delete(server)) return;
  const closed = new Promise(resolve => server.close(resolve));
  for (const socket of server.smokeSockets) socket.destroy();
  await closed;
}

class Cdp {
  constructor(socket) {
    this.socket = socket;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Set();
    socket.addEventListener('message', event => {
      const message = JSON.parse(event.data);
      if (!message.id) {
        for (const listener of this.listeners) listener(message);
        return;
      }
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      clearTimeout(pending.timer);
      if (message.error) pending.reject(new Error(JSON.stringify(message.error)));
      else pending.resolve(message.result);
    });
    socket.addEventListener('close', () => {
      for (const pending of this.pending.values()) {
        clearTimeout(pending.timer);
        pending.reject(new Error('CDP connection closed'));
      }
      this.pending.clear();
    });
  }

  static async connect(url) {
    const socket = new WebSocket(url);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('CDP connection timed out')), 10000);
      socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, { once: true });
      socket.addEventListener('error', () => { clearTimeout(timer); reject(new Error('CDP connection failed')); }, { once: true });
    });
    return new Cdp(socket);
  }

  send(method, params = {}, sessionId) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`CDP timeout: ${method}`));
      }, 15000);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
  }
}

async function startBrowser() {
  assert(!stopping, 'Cannot start a browser during cleanup');
  assert(!browser, 'A smoke browser is already running');
  await fs.mkdir(userData, { recursive: true });
  await fs.rm(path.join(userData, 'DevToolsActivePort'), { force: true });
  const child = spawn(executable, [
    '--headless=new', '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
    `--user-data-dir=${userData}`, `--xenon-managed-ca-config=${configPath}`,
    '--no-first-run', '--no-default-browser-check', '--no-proxy-server',
    '--disable-background-networking', '--disable-component-update', '--disable-extensions',
    '--disable-gpu', '--enable-logging=stderr', 'about:blank',
  ], { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });
  const owned = { child, cdp: undefined, spawnError: undefined };
  browser = owned;
  child.on('error', error => { owned.spawnError = error; browserLog += `${error.message}\n`; });
  child.stderr.on('data', chunk => { browserLog = (browserLog + chunk.toString()).slice(-65536); });
  const endpoint = await until('browser DevTools endpoint', async () => {
    assert(child.exitCode === null && child.signalCode === null, `Browser exited (${child.exitCode})`);
    const [port, endpointPath] = (await fs.readFile(path.join(userData, 'DevToolsActivePort'), 'utf8')).trim().split(/\r?\n/);
    assert(/^\d+$/.test(port) && endpointPath.startsWith('/devtools/browser/'), 'Invalid DevTools endpoint');
    return `ws://127.0.0.1:${port}${endpointPath}`;
  }, 30000, { fatalError: () => owned.spawnError });
  owned.cdp = await Cdp.connect(endpoint);
  const version = await owned.cdp.send('Browser.getVersion');
  console.log(`Browser: ${version.product}`);
}

async function stopBrowser() {
  const owned = browser;
  if (!owned) return;
  if (!owned.child.pid && owned.spawnError) {
    owned.cdp?.socket.close();
    browser = undefined;
    return;
  }
  const exited = () => owned.child.exitCode !== null || owned.child.signalCode !== null;
  if (!exited() && owned.cdp) {
    await owned.cdp.send('Browser.close').catch(() => {});
  }
  if (!exited()) {
    await until('graceful browser exit', exited, 10000, { checkProtocol: false }).catch(async () => {
      if (exited()) return;
      // Only the process tree created above is eligible for forced cleanup.
      if (process.platform === 'win32') {
        await new Promise((resolve, reject) => {
          const killer = spawn('taskkill.exe', ['/PID', String(owned.child.pid), '/T', '/F'],
            { windowsHide: true, stdio: 'ignore' });
          killer.once('error', reject);
          killer.once('exit', resolve);
        });
      } else {
        owned.child.kill('SIGKILL');
      }
      await until('owned browser exit after termination', exited, 10000, { checkProtocol: false });
    });
  }
  owned.cdp?.socket.close();
  browser = undefined;
}

async function probe(url, incognito = false) {
  const cdp = browser.cdp;
  const contextId = incognito ? (await cdp.send('Target.createBrowserContext')).browserContextId : undefined;
  let targetId;
  try {
    ({ targetId } = await cdp.send('Target.createTarget', {
      url: 'about:blank', ...(contextId ? { browserContextId: contextId } : {}),
    }));
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    await cdp.send('Page.enable', {}, sessionId);
    await cdp.send('Network.enable', {}, sessionId);
    await cdp.send('Network.setCacheDisabled', { cacheDisabled: true }, sessionId);
    const navigation = await cdp.send('Page.navigate', { url: `${url}/?probe=${randomUUID()}` }, sessionId);
    if (navigation.errorText) return { error: navigation.errorText };
    await until('test page document', async () => {
      const result = await cdp.send('Runtime.evaluate', {
        expression: 'document.body && document.body.textContent', returnByValue: true,
      }, sessionId);
      return result.result?.value === marker;
    }, 10000);
    return { trusted: true };
  } finally {
    if (targetId) await cdp.send('Target.closeTarget', { targetId }).catch(() => {});
    if (contextId) await cdp.send('Target.disposeBrowserContext', { browserContextId: contextId }).catch(() => {});
  }
}

async function waitGeneration(generation) {
  return until(`durably cached generation ${generation}`, async () => {
    const state = JSON.parse(await fs.readFile(statePath, 'utf8'));
    return state.highest_generation === String(generation) &&
      JSON.parse(state.body).generation === String(generation);
  });
}

// Keep WebUI checks separate so the TLS probes above never depend on the
// diagnostics page's own interpretation of a connection result.
async function checkCertificatePage(siteUrl, expectedTrusted,
  { httpStatus, certificateSha256 } = {}) {
  const cdp = browser.cdp;
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const exceptions = [];
  const listener = message => {
    if (message.sessionId === sessionId && message.method === 'Runtime.exceptionThrown') {
      exceptions.push(message.params.exceptionDetails);
    }
  };
  cdp.listeners.add(listener);
  try {
    await cdp.send('Runtime.enable', {}, sessionId);
    await cdp.send('Page.enable', {}, sessionId);
    const result = await cdp.send('Page.navigate', { url: 'chrome://xenon-certificates/' }, sessionId);
    assert(!result.errorText, `Certificate diagnostics navigation failed: ${result.errorText}`);
    await until('certificate diagnostics title', async () => {
      const evaluated = await cdp.send('Runtime.evaluate', {
        expression: 'document.readyState === "complete" && document.title', returnByValue: true,
      }, sessionId);
      return /证书|certificate/i.test(evaluated.result?.value || '');
    });
    const delivery = await cdp.send('Runtime.evaluate', {
      expression: `import('chrome://resources/js/cr.js').then(({sendWithPromise}) =>
        sendWithPromise('getManagedCertificateStatus'))`,
      awaitPromise: true, returnByValue: true,
    }, sessionId);
    assert(!delivery.exceptionDetails, 'Certificate delivery status request failed');
    const status = delivery.result?.value;
    assert(status?.started && status.enabled && status.ready, 'Managed service must report enabled and ready');
    assert.equal(status.sourceId, sourceId);
    assert.equal(status.configurationError, '');
    assert.equal(status.refreshError, '');
    assert(status.lastAttempt && status.lastSuccess, 'Successful delivery timestamps must be reported');
    assert.equal(status.snapshot?.generation, expectedTrusted ? '1' : '2');
    assert.equal(status.snapshot?.certificates.length, expectedTrusted ? 1 : 0);
    assert(!Object.hasOwn(status.snapshot, 'expiresAt'), 'Diagnostics must not expose a snapshot expiry');
    const submitted = await cdp.send('Runtime.evaluate', {
      expression: `(() => {
        const input = document.getElementById('test-url');
        const form = document.getElementById('test-form');
        if (!input || !form) throw new Error('Missing certificate test form');
        input.value = ${JSON.stringify(siteUrl)};
        input.dispatchEvent(new Event('input', {bubbles: true}));
        form.requestSubmit();
        return true;
      })()`, returnByValue: true,
    }, sessionId);
    assert(!submitted.exceptionDetails, 'Certificate URL test form could not be submitted');
    const outcome = await until('certificate diagnostics TLS test result', async () => {
      const evaluated = await cdp.send('Runtime.evaluate', {
        expression: `(() => {
          const result = document.getElementById('test-result');
          const status = document.getElementById('tls-status');
          const button = document.getElementById('test-button');
          if (!result || result.hidden || !status || !button || button.disabled) return null;
          return {
            status: status.dataset.status, message: result.textContent,
            url: document.getElementById('tested-url').textContent,
            httpStatus: document.getElementById('http-status').textContent,
            certificateVisible: !document.getElementById('peer-certificate').hidden,
            certificateSha256: document.getElementById('peer-sha256').textContent,
          };
        })()`, returnByValue: true,
      }, sessionId);
      const value = evaluated.result?.value;
      return value && ['passed', 'failed', 'incomplete'].includes(value.status) ? value : false;
    });
    assert.equal(outcome.status, expectedTrusted ? 'passed' : 'failed', outcome.message);
    if (httpStatus !== undefined) assert.equal(outcome.httpStatus, `HTTP ${httpStatus}`, outcome.message);
    if (certificateSha256 !== undefined) {
      assert(outcome.certificateVisible, 'Diagnostics must display the original server certificate');
      assert.equal(outcome.certificateSha256, certificateSha256);
      assert.equal(outcome.url, siteUrl, 'Diagnostics must retain the original requested URL');
    }
    assert.equal(exceptions.length, 0, `Certificate diagnostics JavaScript exception: ${JSON.stringify(exceptions)}`);
    console.log(`PASS: chrome://xenon-certificates/ reports TLS ${expectedTrusted ? 'passed' : 'failed'} without JavaScript exceptions`);
  } finally {
    cdp.listeners.delete(listener);
    await cdp.send('Target.closeTarget', { targetId }).catch(() => {});
  }
}

function snapshot(generation, certificates, legacyExpiry = false) {
  return JSON.stringify({
    schema_version: 1, source_id: sourceId, generation: String(generation),
    issued_at: timestamp(Date.now() - (legacyExpiry ? 2 * 86400000 : 60000)),
    ...(legacyExpiry ? { expires_at: timestamp(Date.now() - 86400000) } : {}),
    certificates,
  });
}

function respondSnapshot(request, response) {
  if (response.destroyed) return;
  const etag = `"${hash(servedSnapshot)}"`;
  const unchanged = request.headers['if-none-match'] === etag;
  response.writeHead(unchanged ? 304 : 200, {
    'Content-Type': 'application/json; charset=utf-8', ETag: etag,
    'Cache-Control': 'no-store', Connection: 'close',
  });
  response.end(unchanged ? undefined : servedSnapshot);
}

function controlHandler(request, response) {
  if (request.url !== '/snapshot' || request.method !== 'GET') {
    response.writeHead(404).end();
    return;
  }
  try {
    requests++;
    assert.equal(request.headers['accept-encoding'], 'identity');
    assert.equal(request.headers.cookie, undefined);
    assert.equal(request.headers.authorization, undefined);
    if (holdRequests) pendingResponses.push([request, response]);
    else respondSnapshot(request, response);
  } catch (error) {
    protocolFailure = error;
    response.writeHead(500).end();
  }
}

async function cleanup() {
  stopping = true;
  cleanupPromise ??= (async () => {
    let stopError;
    await stopBrowser().catch(error => { stopError = error; });
    await Promise.all([...ownedServers].map(stopServer));
    if (stopError) throw stopError; // Retain the profile if its browser is alive.
    // This exact mkdtemp result is the only directory the script removes.
    assert(path.dirname(temp) === path.resolve(os.tmpdir()));
    assert(path.basename(temp).startsWith('xenon-managed-ca-smoke-'));
    await fs.rm(temp, { recursive: true, force: true, maxRetries: 8, retryDelay: 250 });
  })();
  return cleanupPromise;
}

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.once(signal, () => {
    cleanup().then(() => process.exit(130), error => {
      console.error(`Cleanup failed: ${error.message}; temporary directory: ${temp}`);
      process.exit(1);
    });
  });
}

try {
  const leafFixture = await fs.readFile(path.join(fixtureDirectory, 'ok_cert.pem'), 'utf8');
  const rootFixture = await fs.readFile(path.join(fixtureDirectory, 'root_ca_cert.pem'), 'utf8');
  const leafPem = pemBlock(leafFixture, 'CERTIFICATE');
  const rootPem = pemBlock(rootFixture, 'CERTIFICATE');
  const leaf = new X509Certificate(leafPem);
  const root = new X509Certificate(rootPem);
  assert.equal(leaf.checkIP('127.0.0.1'), '127.0.0.1');
  assert(leaf.verify(root.publicKey), 'Fixture leaf must be signed by fixture root');
  for (const certificate of [leaf, root]) {
    assert(Date.now() >= Date.parse(certificate.validFrom) && Date.now() < Date.parse(certificate.validTo),
      `TLS fixture outside validity: ${certificate.subject}`);
  }
  const tls = { key: pemBlock(leafFixture, 'PRIVATE KEY'), cert: leafPem,
    ALPNProtocols: ['http/1.1'], secureOptions: constants.SSL_OP_NO_TICKET };
  const redirectMethods = [];
  let redirectTargetRequests = 0;
  const siteServer = await startServer(tls, (request, response) => {
    if (request.url === '/redirect') {
      redirectMethods.push(request.method);
      response.writeHead(302, { Location: '/redirect-target', 'Cache-Control': 'no-store', Connection: 'close' });
      response.end();
      return;
    }
    if (request.url === '/redirect-target') redirectTargetRequests++;
    response.writeHead(200, { 'Content-Type': 'text/plain', 'Cache-Control': 'no-store', Connection: 'close' });
    response.end(marker);
  });
  const siteUrl = `https://127.0.0.1:${siteServer.address().port}`;
  let controlServer = await startServer(tls, controlHandler);
  const controlPort = controlServer.address().port;
  await fs.writeFile(bootstrapPath, rootPem);
  await fs.writeFile(configPath, JSON.stringify({ xenon_managed_ca: {
    enabled: true, source_id: sourceId, endpoint: `https://127.0.0.1:${controlPort}/snapshot`,
    bootstrap_ca_file: bootstrapPath, auth: { mode: 'none' }, request_timeout_seconds: 120,
  } }));
  // Legacy snapshots may contain an elapsed delivery deadline. It must not
  // withdraw the still-valid CA or prevent restoration from the offline cache.
  servedSnapshot = snapshot(1, [{ name: 'Chromium smoke fixture root', role: 'trust_anchor', encoding: 'pem', data: rootPem }], true);

  await startBrowser();
  await until('bootstrap-authenticated control request', () => requests > 0);
  assert.equal((await probe(siteUrl)).error, 'net::ERR_CERT_AUTHORITY_INVALID',
    'Bootstrap must not trust the website before a managed snapshot is accepted');
  console.log('PASS: bootstrap reaches control HTTPS while website remains untrusted');
  holdRequests = false;
  for (const [request, response] of pendingResponses) respondSnapshot(request, response);
  pendingResponses = [];
  await waitGeneration(1);
  await until('managed trust applied to normal context', async () => (await probe(siteUrl)).trusted);
  assert((await probe(siteUrl, true)).trusted, 'Managed trust must reach incognito');
  console.log('PASS: legacy snapshot with elapsed expires_at trusts normal and incognito HTTPS');
  await checkCertificatePage(siteUrl, true);
  await checkCertificatePage(`${siteUrl}/redirect`, true, {
    httpStatus: 302, certificateSha256: leaf.fingerprint256.replaceAll(':', ''),
  });
  assert.deepEqual(redirectMethods, ['HEAD'], 'Diagnostics must send one HEAD request to the redirecting URL');
  assert.equal(redirectTargetRequests, 0, 'Diagnostics must not follow the HTTPS redirect');
  console.log('PASS: HTTPS 302 retains TLS success, HTTP status and certificate without following the redirect');

  await stopBrowser();
  await stopServer(controlServer);
  await startBrowser();
  assert((await probe(siteUrl)).trusted, 'Persisted cache must restore trust with control server offline');
  assert((await probe(siteUrl, true)).trusted, 'Offline cached trust must reach incognito');
  console.log('PASS: restart offline restores cached CA despite legacy elapsed expires_at');
  await stopBrowser();

  servedSnapshot = snapshot(2, []);
  controlServer = await startServer(tls, controlHandler, controlPort);
  await startBrowser();
  await waitGeneration(2);
  await until('empty snapshot removes normal-context trust', async () =>
    (await probe(siteUrl)).error === 'net::ERR_CERT_AUTHORITY_INVALID');
  assert.equal((await probe(siteUrl, true)).error, 'net::ERR_CERT_AUTHORITY_INVALID',
    'Empty snapshot must remove incognito trust');
  console.log('PASS: higher-generation empty snapshot rejects new normal/incognito TLS handshakes');
  await checkCertificatePage(siteUrl, false);
  console.log('PASS: managed certificate browser smoke completed');
} catch (error) {
  console.error(`FAIL: ${error.stack || error.message}`);
  if (browserLog) console.error(`Browser stderr (tail):\n${browserLog.slice(-16000)}`);
  process.exitCode = 1;
} finally {
  await cleanup().catch(error => {
    console.error(`Cleanup failed: ${error.message}; temporary directory: ${temp}`);
    process.exitCode = 1;
  });
}
