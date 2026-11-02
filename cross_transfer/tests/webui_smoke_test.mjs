// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run: node cross_transfer/tests/webui_smoke_test.mjs
// Requires Node.js 18+ and Chrome/Chromium. Set CHROME_BINARY if not detected.
// Uses a temporary browser profile and a test-only bridge; transfers no files.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const pageDir = fileURLToPath(new URL(
    '../../xenon_overlay/resources/webui/cross_transfer_test/', import.meta.url));
const candidates = [
  process.env.CHROME_BINARY,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
  '/usr/bin/chromium-browser',
  process.env.PROGRAMFILES && path.join(
      process.env.PROGRAMFILES, 'Google/Chrome/Application/chrome.exe'),
].filter(Boolean);
const executable = candidates.find(candidate => fs.existsSync(candidate));
assert.ok(executable, 'Set CHROME_BINARY to an installed Chrome/Chromium binary.');

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'cross-transfer-webui-'));
const harnessPath = path.join(scratch, 'harness.html');
const html = fs.readFileSync(path.join(pageDir, 'index.html'), 'utf8');
const source = fs.readFileSync(path.join(pageDir, 'index.js'), 'utf8');
// Evaluate the unchanged page functions separately so the fixture can inject a
// bridge. The normal page is also loaded first to test its missing-bridge guard.
fs.writeFileSync(harnessPath, html
    .replace('<script type="module" src="index.js"></script>', '')
    .replace('href="index.css"',
        `href="${pathToFileURL(path.join(pageDir, 'index.css')).href}"`));

const browser = spawn(executable, [
  '--headless',
  '--no-first-run',
  '--no-default-browser-check',
  '--disable-background-networking',
  '--disable-component-update',
  '--disable-sync',
  '--disable-gpu',
  `--user-data-dir=${path.join(scratch, 'profile')}`,
  '--allow-file-access-from-files',
  '--remote-debugging-pipe',
], {stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe']});

let nextId = 0;
let buffer = '';
const pending = new Map();
function rejectPending(error) {
  for (const request of pending.values()) request.reject(error);
  pending.clear();
}
const browserExited = new Promise(resolve => {
  browser.once('exit', () => {
    rejectPending(new Error('Browser exited before completing a CDP request.'));
    resolve();
  });
  browser.once('error', error => {
    rejectPending(error);
    resolve();
  });
});
browser.stdio[4].on('data', chunk => {
  buffer += chunk.toString();
  let end;
  while ((end = buffer.indexOf('\0')) >= 0) {
    const message = JSON.parse(buffer.slice(0, end));
    buffer = buffer.slice(end + 1);
    const request = pending.get(message.id);
    if (!request) continue;
    pending.delete(message.id);
    if (message.error) request.reject(new Error(JSON.stringify(message.error)));
    else request.resolve(message.result);
  }
});
function cdp(method, params = {}, sessionId) {
  return new Promise((resolve, reject) => {
    if (browser.exitCode !== null || browser.signalCode !== null) {
      reject(new Error('Browser is no longer running.'));
      return;
    }
    const id = ++nextId;
    pending.set(id, {resolve, reject});
    browser.stdio[3].write(JSON.stringify({id, method, params, sessionId}) + '\0');
  });
}
const timeout = setTimeout(() => {
  rejectPending(new Error('WebUI smoke test timed out after 30 seconds.'));
  browser.kill();
}, 30000);

try {
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  const call = (method, params = {}) => cdp(method, params, sessionId);
  await call('Page.enable');
  async function run(expression) {
    const result = await call('Runtime.evaluate', {
      expression, awaitPromise: true, returnByValue: true,
    });
    if (result.exceptionDetails) {
      throw new Error(JSON.stringify(result.exceptionDetails));
    }
    return result.result.value;
  }
  async function until(expression) {
    for (let i = 0; i < 100; ++i) {
      if (await run(expression)) return;
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error(`Condition timed out: ${expression}`);
  }

  await call('Page.navigate', {
    url: pathToFileURL(path.join(pageDir, 'index.html')).href,
  });
  await until(`document.getElementById('bridgeBadge')?.textContent === '静态预览'`);
  assert.equal(await run(`document.getElementById('startButton').disabled`), true);
  assert.equal(await run(`document.getElementById('bridgeNotice').hidden`), false);
  await call('Emulation.setDeviceMetricsOverride', {
    width: 390, height: 844, deviceScaleFactor: 1, mobile: false,
  });
  assert.equal(await run(
      'document.documentElement.scrollWidth <= window.innerWidth'), true);

  await call('Page.navigate', {url: pathToFileURL(harnessPath).href});
  await until(`document.getElementById('transferForm') !== null`);
  await run(source.replace('void initialize();', ''));
  await run(`
    updateForm();
    globalThis.calls = [];
    sendWithPromise = async (name, options) => {
      calls.push({name, options});
      return name === 'crossTransferStart' ? {
        active: true, state: 'listening', status: 'Listening',
        taskId: 'a'.repeat(32), listeningPort: 53317,
        transferredBytes: 0, totalBytes: 100, percent: 0,
      } : {
        active: false, state: 'cancelled', taskId: 'a'.repeat(32),
        transferredBytes: 25, totalBytes: 100, percent: 25,
      };
    };
    updateControls();
  `);
  assert.equal(await run(`$('serverTlsFields').hidden`), false);
  assert.equal(await run(`$('sourcePath').disabled`), true);
  assert.equal(await run(`$('peerName').required && !$('peerName').disabled`), true);
  await run(`
    $('outputDir').value = '/tmp/receive';
    $('certificateFile').value = '/tmp/device.pem';
    $('privateKeyFile').value = '/tmp/key.pem';
    $('peerName').value = 'sender.test';
    form.requestSubmit();
  `);
  await until(`calls.some(x => x.name === 'crossTransferStart') && !operationPending`);
  const direct = await run(`calls.find(x => x.name === 'crossTransferStart').options`);
  assert.equal(direct.role, 'receive');
  assert.equal(direct.port, 53317);
  assert.equal(direct.certificateFile, '/tmp/device.pem');
  assert.equal(direct.caFile, undefined);
  assert.equal(await run(`$('taskId').value`), 'a'.repeat(32));
  assert.equal(await run(`$('configFields').disabled`), true);
  assert.equal(await run(`$('cancelButton').disabled`), false);
  await run(`$('cancelButton').click()`);
  await until('!operationPending && !snapshot.active');
  assert.equal(await run(`$('statusHeading').textContent`), '已取消');
  assert.equal(await run(`$('progressPercent').textContent`), '25%');

  await run(`
    form.querySelector('[name="transport"][value="relay"]').checked = true;
    updateForm();
    $('relayHost').value = 'relay.test';
    $('roomId').value = 'room';
    $('connectionId').value = 'b'.repeat(32);
    $('bearerToken').value = 'C'.repeat(64);
    $('caFile').value = '';
  `);
  assert.equal(await run(`$('serverTlsFields').hidden &&
      $('certificateFile').disabled && $('privateKeyFile').disabled &&
      $('peerName').disabled`), true);
  const relay = await run('readOptions()');
  assert.equal(relay.certificateFile, undefined);
  assert.equal(relay.privateKeyFile, undefined);
  assert.equal(relay.peerName, undefined);
  assert.equal(relay.port, 443);
  assert.equal(relay.bearerToken, 'C'.repeat(64));
  assert.equal(await run(
      `redactError('bad token ' + 'C'.repeat(64)).includes('C'.repeat(64))`), false);
  assert.equal(await run(
      `redactError('bad token ' + 'c'.repeat(64)).includes('c'.repeat(64))`), false);
  await run(`
    form.querySelector('[name="role"][value="send"]').checked = true;
    updateForm();
  `);
  assert.equal(await run(`$('bearerToken').value`), '');
  assert.equal(await run(`$('outputDir').disabled && !$('sourcePath').disabled`), true);
  await run(`$('generateConnectionId').click()`);
  assert.match(await run(`$('connectionId').value`), /^[0-9a-f]{32}$/);

  // A status request started before a mutation must not revert the new state.
  await run(`
    clearTimeout(pollTimer);
    sendWithPromise = () => new Promise(resolve => globalThis.resolvePoll = resolve);
    globalThis.pendingPoll = pollStatus();
    void 0;
  `);
  await until(`typeof resolvePoll === 'function'`);
  await run(`(async () => {
    ++snapshotVersion;
    renderSnapshot({active: true, state: 'transferring', taskId: 'd'.repeat(32),
      percent: 41, totalBytes: 100, transferredBytes: 41});
    resolvePoll({active: false, state: 'idle', percent: 0});
    await pendingPoll;
    clearTimeout(pollTimer);
  })()`);
  assert.equal(await run('snapshot.state'), 'transferring');
  assert.equal(await run(`$('progressPercent').textContent`), '41%');
  console.log('PASS: preview guard, mobile layout, direct/relay options, start, ' +
      'cancel, task ID, progress, role credentials, ID generation, stale polling.');
} finally {
  clearTimeout(timeout);
  await cdp('Browser.close').catch(() => {});
  await browserExited;
  fs.rmSync(scratch, {recursive: true, force: true});
}
