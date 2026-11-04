#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

// Runs an explicitly selected browser against an already running local store.
// Uses its own temporary User Data and cleans up only its own browser process.
// Never changes OS trust, disables TLS checks, clicks install, or stops the store.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import * as fs from 'node:fs/promises';
import https from 'node:https';
import path from 'node:path';
import {DEFAULT_DATA, DOMAIN, REPO} from './store.mjs';
import {createTlsValidationFixtures} from './tls-fixtures.mjs';

const usage = `Usage: node tools/xl_extension_store/smoke-browser.mjs [options]
  --browser <absolute executable>  Default: out/Release_64/xlb153.exe
  --config <absolute JSON>         Explicitly test legacy dynamic CA delivery
  --check-tls-validation           Also verify standard DNS/IP TLS validation

The existing HTTPS store must already be running on port 443 and hosts must map
xl.extensions.test.com to loopback. By default a fresh profile uses only the
built-in store CA: its first HTTPS navigation must succeed without downloading
a managed snapshot or reloading. Checks normal TLS, native webstorePrivate APIs,
and enabled buttons for XL/Google fixtures. The TLS validation checks require the
local CA signing key and create only temporary loopback TLS services.
Does not install extensions or modify OS trust.`;

const options = {};
const XL_DEMO_ID = 'mcjbgmlghdgeocallnajjfbblpondljm';
const GOOGLE_DEMO_ID = 'ngjobkbdodapjbbncmagbccommkggmnj';
const STARTUP_URL = `https://${DOMAIN}/detail/${XL_DEMO_ID}`;
for (let index = 2; index < process.argv.length; ++index) {
  const arg = process.argv[index];
  if (arg === '--help' || arg === '-h') {
    console.log(usage);
    process.exit(0);
  }
  if (arg === '--check-tls-validation') {
    options.checkTlsValidation = true;
    continue;
  }
  assert(['--browser', '--config'].includes(arg) && process.argv[index + 1], usage);
  options[arg.slice(2)] = process.argv[++index];
}
const executable = path.resolve(options.browser || path.join(REPO, 'out/Release_64/xlb153.exe'));
assert(!(options.config && options.checkTlsValidation),
  'Built-in CA TLS validation checks must not enable a dynamic CA source');
const config = options.config &&
  JSON.parse(await fs.readFile(path.resolve(options.config), 'utf8')).xenon_managed_ca;
if (config) {
  assert(config.enabled !== false && config.source_id && config.bootstrap_ca_file,
    'The selected managed CA config must enable a local bootstrap CA');
  assert.equal(new URL(config.endpoint).origin, `https://${DOMAIN}`,
    'The dynamic smoke expects the native XL HTTPS origin on port 443');
}
assert((await fs.stat(executable)).isFile(), 'Browser executable does not exist');
assert(typeof WebSocket === 'function', 'Node.js 22+ is required');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const temp = await fs.mkdtemp(path.join(DEFAULT_DATA, 'browser-smoke-'));
const userData = path.join(temp, 'user-data');
await fs.mkdir(userData);
// Explicitly disable any executable-adjacent deployment config. This does not
// provision trust; it proves that first navigation only uses the built-in root.
const configPath = options.config ? path.resolve(options.config) :
  path.join(temp, 'managed-ca-disabled.json');
if (!config) await fs.writeFile(configPath, JSON.stringify({xenon_managed_ca: {enabled: false}}));
let child;
let cdp;
let spawnError;
let browserLog = '';
let cleanupPromise;
const ownedTlsServers = new Set();
const tlsFixtures = [];

async function until(description, action, timeout = 30000, checkProcess = true) {
  const deadline = Date.now() + timeout;
  let lastError;
  while (Date.now() < deadline) {
    if (checkProcess) {
      if (spawnError) throw spawnError;
      assert(child && child.exitCode === null && child.signalCode === null,
        `Browser exited while waiting for ${description}`);
    }
    try {
      const value = await action();
      if (value) return value;
    } catch (error) {
      lastError = error;
    }
    await delay(100);
  }
  throw new Error(`Timed out: ${description}${lastError ? ` (${lastError.message})` : ''}`);
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
      const timer = setTimeout(() => reject(new Error('CDP connection timeout')), 10000);
      socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, {once: true});
      socket.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error('CDP connection failed'));
      }, {once: true});
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
      this.pending.set(id, {resolve, reject, timer});
      this.socket.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
    });
  }
}

async function evaluate(sessionId, expression) {
  const result = await cdp.send('Runtime.evaluate', {
    expression, awaitPromise: true, returnByValue: true,
  }, sessionId);
  assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
  return result.result?.value;
}

async function attachPage(targetId) {
  const {sessionId} = await cdp.send('Target.attachToTarget', {targetId, flatten: true});
  await cdp.send('Page.enable', {}, sessionId);
  await cdp.send('Network.enable', {}, sessionId);
  await cdp.send('Runtime.enable', {}, sessionId);
  return {targetId, sessionId};
}

async function newPage() {
  const {targetId} = await cdp.send('Target.createTarget', {url: 'about:blank'});
  return attachPage(targetId);
}

async function checkOriginalStartupTab() {
  const target = await until('original command-line startup tab', async () => {
    const {targetInfos} = await cdp.send('Target.getTargets');
    return targetInfos.find(info => info.type === 'page' && info.url === STARTUP_URL);
  });
  const {sessionId} = await attachPage(target.targetId);
  // This tab was navigated by the actual browser startup command. Only inspect
  // it; navigating or reloading here could hide the first-navigation race.
  await until('original startup tab enabled install button', async () => {
    const state = await evaluate(sessionId, `({
      url: location.href, secure: isSecureContext,
      api: typeof globalThis.chrome?.webstorePrivate,
      button: document.querySelector('#content button')?.textContent,
      disabled: document.querySelector('#content button')?.disabled,
      status: document.querySelector('[role="status"]')?.textContent,
      interstitial: !!document.querySelector('#main-frame-error, #interstitial-wrapper')
    })`);
    assert.equal(state.interstitial, false, 'The original startup tab showed a certificate error');
    if (['暂时无法添加', '暂时无法操作'].includes(state.button)) {
      throw new Error(state.status || 'Startup button disabled');
    }
    return state.url === STARTUP_URL && state.secure && state.api === 'object' &&
      state.button === '添加至 Chrome' && state.disabled === false;
  });
  console.log('PASS: original command-line startup tab: secure, API available, button enabled; no navigation/reload');
}

async function waitForPrivateTrust() {
  const {targetId, sessionId} = await newPage();
  try {
    const navigation = await cdp.send('Page.navigate', {url: 'chrome://xenon-certificates/'}, sessionId);
    assert(!navigation.errorText, navigation.errorText);
    const status = await until('explicit dynamic CA snapshot', async () => {
      const value = await evaluate(sessionId,
        `import('chrome://resources/js/cr.js').then(({sendWithPromise}) =>
          sendWithPromise('getManagedCertificateStatus'))`);
      assert.equal(value.configurationError, '', `CA config: ${value.configurationError}`);
      if (value.refreshError) throw new Error(`CA delivery: ${value.refreshError}`);
      return value.started && value.ready && value.enabled &&
        value.sourceId === config.source_id && /^[1-9]\d*$/.test(value.snapshot?.generation || '') &&
        value.snapshot.certificates?.length === 1 && value.lastSuccess && value;
    });
    console.log(`PASS: explicit dynamic CA accepted (${status.sourceId}, generation ${status.snapshot.generation})`);
  } finally {
    await cdp.send('Target.closeTarget', {targetId}).catch(() => {});
  }
}

async function assertNoDynamicTrust() {
  const {targetId, sessionId} = await newPage();
  try {
    const navigation = await cdp.send('Page.navigate', {url: 'chrome://xenon-certificates/'}, sessionId);
    assert(!navigation.errorText, navigation.errorText);
    const status = await until('disabled dynamic CA status', () => evaluate(sessionId,
      `import('chrome://resources/js/cr.js').then(({sendWithPromise}) =>
        sendWithPromise('getManagedCertificateStatus'))`));
    assert.equal(status.enabled, false);
    assert.equal(status.downloading, false);
    assert.equal(status.snapshot, null);
    assert(!status.lastAttempt && !status.lastSuccess, 'No dynamic CA request may have run');
    assert.equal(status.configurationError, '');
    await assert.rejects(fs.stat(path.join(userData, 'ManagedCertificates')), {code: 'ENOENT'});
    console.log('PASS: dynamic CA disabled; no snapshot download or managed certificate cache');
  } finally {
    await cdp.send('Target.closeTarget', {targetId}).catch(() => {});
  }
}

async function startTlsServers() {
  for (const fixture of createTlsValidationFixtures(path.join(temp, 'tls-fixtures'))) {
    const server = https.createServer({
      cert: await fs.readFile(fixture.certificatePath),
      key: await fs.readFile(fixture.keyPath),
    }, (_request, response) => {
      ++fixture.requests;
      response.writeHead(200, {'Content-Type': 'text/html'});
      response.end('<!doctype html><title>XL CA TLS validation test</title>');
    });
    fixture.requests = 0;
    server.smokeSockets = new Set();
    server.on('connection', socket => {
      server.smokeSockets.add(socket);
      socket.on('close', () => server.smokeSockets.delete(socket));
    });
    server.on('tlsClientError', () => {});
    ownedTlsServers.add(server);
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    fixture.port = server.address().port;
    // DNS probes keep the standard HTTPS port so a missing store API on the
    // child domain demonstrates host isolation, not just a nonstandard port.
    fixture.url = fixture.name === 'ip' ? `https://${fixture.hostname}:${fixture.port}/` :
      `https://${fixture.hostname}/`;
    tlsFixtures.push(fixture);
  }
}

async function checkTlsValidation() {
  for (const fixture of tlsFixtures) {
    const {targetId, sessionId} = await newPage();
    let documentResponse;
    const listener = message => {
      if (message.sessionId === sessionId && message.method === 'Network.responseReceived' &&
          message.params.type === 'Document' && message.params.response.url === fixture.url) {
        documentResponse = message.params.response;
      }
    };
    cdp.listeners.add(listener);
    try {
      const navigation = await cdp.send('Page.navigate', {url: fixture.url}, sessionId);
      if (fixture.allowed) {
        assert(!navigation.errorText, `${fixture.hostname}: ${navigation.errorText}`);
        await until(`${fixture.hostname} TLS response`, () => documentResponse);
        assert.equal(documentResponse.status, 200);
        assert.equal(documentResponse.securityState, 'secure');
        const api = await evaluate(sessionId, 'typeof globalThis.chrome?.webstorePrivate');
        assert.equal(api, 'undefined', 'Ordinary CA trust must not expose exact-origin store APIs');
        console.log(`PASS: ${fixture.hostname}: secure TLS, no store API`);
      } else {
        assert.equal(navigation.errorText, 'net::ERR_CERT_COMMON_NAME_INVALID',
          `${fixture.hostname}: expected a hostname mismatch rejection; navigation=${JSON.stringify(navigation)}, HTTP requests=${fixture.requests}`);
        assert.equal(documentResponse, undefined, 'A mismatched hostname must not receive an HTTP document');
        assert.equal(fixture.requests, 0, 'TLS must fail before the server handles an HTTP request');
        console.log(`PASS: ${fixture.hostname}: rejected (${navigation.errorText})`);
      }
    } finally {
      cdp.listeners.delete(listener);
      await cdp.send('Target.closeTarget', {targetId}).catch(() => {});
    }
  }
}

async function checkFixture(id, publisher) {
  const {targetId, sessionId} = await newPage();
  const url = `https://${DOMAIN}/detail/${id}`;
  const exceptions = [];
  let documentResponse;
  const listener = message => {
    if (message.sessionId !== sessionId) return;
    if (message.method === 'Runtime.exceptionThrown') exceptions.push(message.params.exceptionDetails);
    if (message.method === 'Network.responseReceived' &&
        message.params.type === 'Document' && message.params.response.url === url) {
      documentResponse = message.params.response;
    }
  };
  cdp.listeners.add(listener);
  try {
    const navigation = await cdp.send('Page.navigate', {url}, sessionId);
    assert(!navigation.errorText, `TLS/page navigation failed: ${navigation.errorText}`);
    const state = await until(`${publisher} enabled install button`, async () => {
      const value = await evaluate(sessionId, `({
        url: location.href, title: document.title,
        api: typeof globalThis.chrome?.webstorePrivate,
        button: document.querySelector('#content button')?.textContent,
        disabled: document.querySelector('#content button')?.disabled,
        status: document.querySelector('[role="status"]')?.textContent,
        interstitial: !!document.querySelector('#main-frame-error, #interstitial-wrapper')
      })`);
      assert.equal(value.interstitial, false, 'Certificate interstitial was shown');
      if (['暂时无法添加', '暂时无法操作'].includes(value.button)) {
        throw new Error(value.status || 'Button disabled');
      }
      return value.url === url && value.title === 'XL 扩展商店' && value.api === 'object' &&
        value.button === '添加至 Chrome' && value.disabled === false && value;
    });
    assert.equal(documentResponse?.status, 200, 'Expected HTTPS document response');
    assert.equal(documentResponse.securityState, 'secure', 'Document TLS state must be secure');
    assert(documentResponse.securityDetails, 'TLS certificate details must be present');
    const apiStatus = await evaluate(sessionId, `(async () => {
      const response = await fetch('/api/items/${id}');
      if (!response.ok) throw new Error('Metadata request failed');
      const item = await response.json();
      const status = await new Promise((resolve, reject) => {
        chrome.webstorePrivate.getExtensionStatus('${id}', item.manifest, value => {
          const error = chrome.runtime.lastError;
          if (error) reject(new Error(error.message)); else resolve(value);
        });
      });
      return {status, publisher: item.publisher};
    })()`);
    assert.equal(apiStatus.publisher, publisher);
    assert.equal(apiStatus.status, 'installable');
    assert.equal(exceptions.length, 0, JSON.stringify(exceptions));
    console.log(`PASS: ${publisher} ${id}: secure TLS, installable, button enabled`);
    return state;
  } finally {
    cdp.listeners.delete(listener);
    await cdp.send('Target.closeTarget', {targetId}).catch(() => {});
  }
}

function cleanup() {
  if (cleanupPromise) return cleanupPromise;
  cleanupPromise = (async () => {
    const exited = () => !child || child.exitCode !== null || child.signalCode !== null;
    if (!exited() && cdp) await cdp.send('Browser.close').catch(() => {});
    if (!exited() && child?.pid) {
      await until('owned browser exit', exited, 10000, false).catch(async () => {
        if (exited()) return;
        if (process.platform === 'win32') {
          await new Promise((resolve, reject) => {
            const killer = spawn('taskkill.exe', ['/PID', String(child.pid), '/T', '/F'],
              {windowsHide: true, stdio: 'ignore'});
            killer.once('error', reject);
            killer.once('exit', resolve);
          });
        } else child.kill('SIGKILL');
        await until('owned browser terminated', exited, 10000, false);
      });
    }
    cdp?.socket.close();
    for (const server of ownedTlsServers) {
      const closed = new Promise(resolve => server.close(resolve));
      for (const socket of server.smokeSockets) socket.destroy();
      await closed;
    }
    ownedTlsServers.clear();
    // The path is returned directly by mkdtemp under our runtime root.
    assert(path.dirname(temp) === DEFAULT_DATA && path.basename(temp).startsWith('browser-smoke-'));
    await fs.rm(temp, {recursive: true, force: true, maxRetries: 10, retryDelay: 100});
  })();
  return cleanupPromise;
}

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.once(signal, () => cleanup().finally(() => process.exit(1)));
}

try {
  if (options.checkTlsValidation) await startTlsServers();
  const bypassHosts = [DOMAIN, ...tlsFixtures.map(fixture => fixture.hostname)];
  const resolverRules = tlsFixtures.filter(fixture => fixture.name !== 'ip')
    .map(fixture => `MAP ${fixture.hostname} 127.0.0.1:${fixture.port}`).join(', ');
  child = spawn(executable, [
    '--headless=new', '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
    `--user-data-dir=${userData}`, `--xenon-managed-ca-config=${configPath}`,
    '--no-first-run', '--no-default-browser-check', `--proxy-bypass-list=${bypassHosts.join(';')}`,
    ...(resolverRules ? [`--host-resolver-rules=${resolverRules}`] : []),
    '--disable-background-networking', '--disable-component-update', '--disable-gpu',
    '--enable-logging=stderr', config ? 'about:blank' : STARTUP_URL,
  ], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
  child.on('error', error => { spawnError = error; });
  child.stderr.on('data', data => { browserLog = (browserLog + data).slice(-65536); });
  const endpoint = await until('browser DevTools endpoint', async () => {
    const [port, endpointPath] = (await fs.readFile(path.join(userData, 'DevToolsActivePort'), 'utf8'))
      .trim().split(/\r?\n/);
    assert(/^\d+$/.test(port) && endpointPath.startsWith('/devtools/browser/'));
    return `ws://127.0.0.1:${port}${endpointPath}`;
  });
  cdp = await Cdp.connect(endpoint);
  console.log(`Browser: ${(await cdp.send('Browser.getVersion')).product}`);
  if (config) await waitForPrivateTrust();
  else await checkOriginalStartupTab();
  await checkFixture(XL_DEMO_ID, 'XL');
  await checkFixture(GOOGLE_DEMO_ID, 'Google');
  if (!config) await assertNoDynamicTrust();
  if (options.checkTlsValidation) await checkTlsValidation();
  console.log('PASS: local store browser smoke completed; no install was attempted');
} catch (error) {
  console.error(error.stack || error.message);
  if (browserLog) console.error(`Browser diagnostics:\n${browserLog}`);
  process.exitCode = 1;
} finally {
  await cleanup();
}
