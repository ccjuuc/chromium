// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Runs a real browser/native account integration against the configured server.
// Set CHROME_BINARY to a freshly built Xenon executable. Two temporary browser
// profiles and one generated server account are used. No bridge is mocked.
// The result file records only the generated username/account ID so an operator
// can remove that account from the server after this test; no password/token is
// printed or written by this script. Native profile preferences are deleted.
// --with-transfers also tests text acceptance, rejection and cancellation.
// --with-file additionally opens a visible native chooser for operator-driven
// selection, then verifies a real 1 MiB file transfer and its SHA-256.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash, randomBytes} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const executable = process.env.CHROME_BINARY || path.join(root,
    'out', 'Release_64', process.platform === 'win32' ? 'xlb153.exe' : 'chrome');
assert.ok(fs.existsSync(executable), 'CHROME_BINARY must point at a built Xenon executable.');

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-account-native-'));
const resultFile = path.join(root, 'out', 'account_browser_integration_result.json');
const username = `acct_smoke_${Date.now().toString(36)}_${randomBytes(5).toString('hex')}`;
const password = `Xenon-smoke-${randomBytes(24).toString('base64url')}`;
const report = {
  startedAt: new Date().toISOString(), username, accountId: null,
  passed: false, checks: {},
};
const browsers = new Set();
const initializeOnly = process.argv.includes('--initialize-only');
const withFile = process.argv.includes('--with-file');
const withTransfers = withFile || process.argv.includes('--with-transfers');
const selectionFile = path.join(root, 'out', 'own_transfer_file_selection.json');
let selectionWritten = false;
let step = 'startup';

function redact(message) {
  return String(message)
      .replaceAll(password, '[redacted-password]')
      .replaceAll(username, '[redacted-username]')
      .replace(/[a-f0-9]{64}/gi, '[redacted-token]')
      .slice(0, 1500);
}

function writeReport() {
  fs.mkdirSync(path.dirname(resultFile), {recursive: true});
  fs.writeFileSync(resultFile, JSON.stringify(report, null, 2) + '\n');
}
function checkpoint(name) {
  report.checks[name] = true;
  writeReport();
  console.log(`PASS: ${name}`);
}
function inspectPublicState(state) {
  assert.equal(typeof state, 'object', 'Native state is missing.');
  const inspect = value => {
    if (!value || typeof value !== 'object') return;
    for (const [key, child] of Object.entries(value)) {
      assert.equal(/token|password|secret|seed|private/i.test(key), false,
          'Native state exposed a credential field.');
      inspect(child);
    }
  };
  inspect(state);
  assert.equal(JSON.stringify(state).includes(password), false,
      'Native state exposed the generated password.');
  return state;
}

function inspectState(state) {
  inspectPublicState(state);
  assert.equal(typeof state.authenticated, 'boolean', 'Invalid native account state.');
  return state;
}

function inspectTransferState(state) {
  inspectPublicState(state);
  assert.ok(Array.isArray(state.transfers), 'Native transfer list is missing.');
  assert.ok(state.job && typeof state.job === 'object', 'Native transfer job is missing.');
  return state;
}

class Browser {
  constructor(profile) {
    this.profile = profile;
    this.pending = new Map();
    this.nextId = 0;
    this.buffer = '';
    this.sessionId = null;
    this.closePromise = null;
    this.child = spawn(executable, [
      ...(withFile ? [] : ['--headless']),
      '--no-first-run', '--no-default-browser-check', '--disable-xenon-login-gate',
      '--disable-background-networking', '--disable-component-update',
      '--disable-sync', '--disable-gpu', '--remote-debugging-pipe',
      `--user-data-dir=${profile}`,
    ], {windowsHide: true, stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe']});
    browsers.add(this);
    this.exited = new Promise(resolve => {
      const finish = () => {
        for (const pending of this.pending.values()) {
          clearTimeout(pending.timer);
          pending.reject(new Error('Browser closed during a native account request.'));
        }
        this.pending.clear();
        resolve();
      };
      this.child.once('exit', finish);
      this.child.once('error', finish);
    });
    this.child.stdio[3].on('error', () => {});
    this.child.stdio[4].on('data', chunk => {
      this.buffer += chunk.toString();
      let end;
      while ((end = this.buffer.indexOf('\0')) >= 0) {
        const message = JSON.parse(this.buffer.slice(0, end));
        this.buffer = this.buffer.slice(end + 1);
        const pending = this.pending.get(message.id);
        if (!pending) continue;
        this.pending.delete(message.id);
        clearTimeout(pending.timer);
        if (message.error) pending.reject(new Error(
            `Browser rejected a CDP request: ${redact(message.error.message)}`));
        else pending.resolve(message.result);
      }
    });
  }

  cdp(method, params = {}, sessionId = this.sessionId, timeoutMs = 45000) {
    return new Promise((resolve, reject) => {
      if (this.child.exitCode !== null || this.child.signalCode !== null) {
        reject(new Error('Browser is no longer running.'));
        return;
      }
      const id = ++this.nextId;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Native browser request exceeded ${timeoutMs / 1000} seconds.`));
      }, timeoutMs);
      this.pending.set(id, {resolve, reject, timer});
      const message = {id, method, params};
      if (sessionId) message.sessionId = sessionId;
      this.child.stdio[3].write(JSON.stringify(message) + '\0');
    });
  }

  async evaluate(expression, timeoutMs = 45000, userGesture = false) {
    const result = await this.cdp('Runtime.evaluate', {
      expression, awaitPromise: true, returnByValue: true, userGesture,
    }, this.sessionId, timeoutMs);
    if (result.exceptionDetails) {
      const exception = result.exceptionDetails.exception;
      const description = exception?.description || result.exceptionDetails.text;
      throw new Error(`Native account WebUI JavaScript failed: ${redact(description)}`);
    }
    return result.result.value;
  }

  async open() {
    const {targetId} = await this.cdp('Target.createTarget', {url: 'about:blank'}, null);
    const {sessionId} = await this.cdp('Target.attachToTarget',
        {targetId, flatten: true}, null);
    this.sessionId = sessionId;
    await this.cdp('Page.enable');
    await this.cdp('Page.navigate', {url: 'chrome://cross-transfer-test/'});
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
      if (await this.evaluate(`location.href === 'chrome://cross-transfer-test/' &&
          document.readyState === 'complete' && !!document.getElementById('accountForm')`)) {
        return this;
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error('Native account WebUI did not load in 20 seconds.');
  }

  async account(method, options) {
    const args = [method];
    if (options !== undefined) args.push(options);
    // Load Chromium's real promise bridge from the trusted WebUI origin.
    // Credentials are only passed in memory to the native authentication call.
    return inspectState(await this.evaluate(`(async () => {
      const {sendWithPromise} = await import('chrome://resources/js/cr.js');
      return sendWithPromise(...${JSON.stringify(args)});
    })()`));
  }

  async transfer(method, options, timeoutMs = 45000) {
    const args = [method];
    if (options !== undefined) args.push(options);
    const value = await this.evaluate(`(async () => {
      const {sendWithPromise} = await import('chrome://resources/js/cr.js');
      return sendWithPromise(...${JSON.stringify(args)});
    })()`, timeoutMs, method === 'ownTransferChooseFile');
    return method === 'ownTransferChooseFile' ? inspectPublicState(value) :
        inspectTransferState(value);
  }

  async ready() {
    const deadline = Date.now() + 40000;
    while (Date.now() < deadline) {
      const state = await this.account('xenonAccountGetState');
      if (!state.busy && !state.restoring) return state;
      await new Promise(resolve => setTimeout(resolve, 150));
    }
    throw new Error('Native account initialization exceeded 40 seconds.');
  }

  close() {
    if (!this.closePromise) {
      this.closePromise = (async () => {
        const force = setTimeout(() => this.child.kill(), 8000);
        try {
          if (this.child.exitCode === null && this.child.signalCode === null) {
            await this.cdp('Browser.close', {}, null).catch(() => {});
          }
          await this.exited;
        } finally {
          clearTimeout(force);
        }
      })();
    }
    return this.closePromise;
  }
}

writeReport();
const overallTimeout = setTimeout(() => {
  for (const browser of browsers) browser.child.kill();
}, withFile ? 600000 : 240000);

async function waitTransfer(browser, id, expected, timeoutMs = 40000) {
  const deadline = Date.now() + timeoutMs;
  let nextProgress = Date.now() + 10000;
  while (Date.now() < deadline) {
    const state = await browser.transfer('ownTransferRefresh');
    const transfer = state.transfers.find(item => item.id === id);
    if (transfer && expected.includes(transfer.status)) return {state, transfer};
    if (Date.now() >= nextProgress) {
      console.log(`WAIT: ${step}; invitation ${transfer?.status || 'not yet visible'}`);
      nextProgress = Date.now() + 10000;
    }
    await new Promise(resolve => setTimeout(resolve, 350));
  }
  throw new Error(`Transfer did not reach ${expected.join('/')} within ${timeoutMs / 1000} seconds.`);
}

async function sendText(first, deviceA, deviceB, text) {
  const state = await first.transfer('ownTransferSendText', {deviceId: deviceB, text});
  assert.equal(state.error || '', '', 'Native text send failed.');
  const transfer = state.transfers.find(item => item.kind === 'text' && item.text === text &&
      item.senderDeviceId === deviceA && item.receiverDeviceId === deviceB);
  assert.ok(transfer, 'Sent text invitation was not returned.');
  assert.equal(transfer.status, 'pending', 'Text must wait for receiver acceptance.');
  return transfer.id;
}

async function runTransfers(first, second, deviceA, deviceB, receiveDirectory) {
  step = 'native Unicode text transfer';
  console.log(`STEP: ${step}`);
  const unicodeText = `跨设备文本\nHello 👋 — <b>literal text</b> ${randomBytes(4).toString('hex')}`;
  const textId = await sendText(first, deviceA, deviceB, unicodeText);
  const pending = await waitTransfer(second, textId, ['pending']);
  assert.equal(pending.transfer.senderDeviceId, deviceA);
  assert.equal(pending.transfer.receiverDeviceId, deviceB);
  const accepted = await second.transfer('ownTransferRespond', {id: textId, action: 'accept'});
  assert.equal(accepted.error || '', '', 'Receiver could not accept text.');
  const received = await waitTransfer(second, textId, ['completed']);
  assert.equal(received.transfer.text, unicodeText, 'Unicode text changed in transit.');
  const senderDone = await waitTransfer(first, textId, ['completed']);
  assert.equal(senderDone.transfer.text, unicodeText);
  checkpoint('native_unicode_text_received');

  step = 'receiver rejects a text invitation';
  const rejectId = await sendText(first, deviceA, deviceB,
      `reject this test ${randomBytes(4).toString('hex')}`);
  await waitTransfer(second, rejectId, ['pending']);
  const rejected = await second.transfer('ownTransferRespond', {id: rejectId, action: 'reject'});
  assert.equal(rejected.error || '', '', 'Receiver rejection failed.');
  await waitTransfer(first, rejectId, ['rejected']);
  checkpoint('native_receiver_rejects_invitation');

  step = 'sender cancels a pending invitation';
  const cancelId = await sendText(first, deviceA, deviceB,
      `cancel this test ${randomBytes(4).toString('hex')}`);
  const cancelled = await first.transfer('ownTransferRespond', {id: cancelId, action: 'cancel'});
  assert.equal(cancelled.error || '', '', 'Sender cancellation failed.');
  await waitTransfer(second, cancelId, ['cancelled']);
  checkpoint('native_sender_cancels_invitation');

  if (!withFile) return;
  step = 'native file selection';
  const sourcePath = path.join(scratch, 'native-transfer-1MiB.bin');
  const fileContents = randomBytes(1024 * 1024);
  fs.writeFileSync(sourcePath, fileContents);
  const sourceHash = createHash('sha256').update(fileContents).digest('hex');
  // The path is for the operator selecting a file in the OS dialog. It is never
  // passed to the WebUI bridge or used to bypass the native file chooser.
  fs.writeFileSync(selectionFile, JSON.stringify({sourcePath}, null, 2) + '\n');
  selectionWritten = true;
  await first.cdp('Page.bringToFront');
  const selectionProgress = setInterval(() => {
    console.log('WAIT_FILE_SELECTION: select the generated file in the native chooser.');
  }, 20000);
  let selectedFile;
  try {
    const selection = first.transfer('ownTransferChooseFile', undefined, 300000);
    console.log(`WAIT_FILE_SELECTION: ${selectionFile}`);
    selectedFile = await selection;
  } finally {
    clearInterval(selectionProgress);
  }
  assert.equal(Boolean(selectedFile.cancelled), false, 'The file selection was cancelled.');
  assert.equal(selectedFile.name, path.basename(sourcePath), 'The wrong file was selected.');
  assert.equal(selectedFile.size, fileContents.length, 'Selected file has the wrong size.');
  checkpoint('native_file_picker_selected');

  step = 'native file invitation and acceptance';
  const offered = await first.transfer('ownTransferSendFile', {deviceId: deviceB});
  assert.equal(offered.error || '', '', 'Native file send failed.');
  const fileTransfer = offered.transfers.find(item => item.kind === 'file' &&
      item.senderDeviceId === deviceA && item.receiverDeviceId === deviceB &&
      item.fileName === path.basename(sourcePath));
  assert.ok(fileTransfer, 'File invitation was not returned.');
  assert.equal(fileTransfer.fileSize, fileContents.length);
  await waitTransfer(second, fileTransfer.id, ['pending']);
  const fileAccepted = await second.transfer('ownTransferRespond',
      {id: fileTransfer.id, action: 'accept'});
  assert.equal(fileAccepted.error || '', '', 'Receiver could not accept the file.');
  checkpoint('native_file_invitation_accepted');

  step = 'native file transfer and SHA-256 verification';
  const deadline = Date.now() + 120000;
  let senderState;
  let receiverState;
  let nextProgress = 0;
  let complete = false;
  while (Date.now() < deadline) {
    [senderState, receiverState] = await Promise.all([
      first.transfer('ownTransferRefresh'), second.transfer('ownTransferRefresh'),
    ]);
    const fileRecord = receiverState.transfers.find(item => item.id === fileTransfer.id);
    const senderRecord = senderState.transfers.find(item => item.id === fileTransfer.id);
    const senderJob = senderState.job;
    const receiverJob = receiverState.job;
    const completedJob = job => job.transferId === fileTransfer.id && !job.active &&
        ['completed', 'complete', 'succeeded'].includes(job.state);
    complete = completedJob(senderJob) && completedJob(receiverJob) &&
        senderRecord?.status === 'completed' && fileRecord?.status === 'completed';
    if (complete) break;
    if ([senderJob.state, receiverJob.state, fileRecord?.status].some(value =>
      ['failed', 'cancelled', 'expired'].includes(value))) {
      throw new Error(`Native file transfer failed: ${redact(senderJob.error || receiverJob.error || fileRecord.status)}`);
    }
    if (Date.now() >= nextProgress) {
      console.log(`WAIT: file sender=${senderJob.state || 'waiting'} receiver=${receiverJob.state || 'waiting'}`);
      nextProgress = Date.now() + 10000;
    }
    await new Promise(resolve => setTimeout(resolve, 400));
  }
  assert.equal(complete, true, 'Both native file jobs did not complete.');
  const savedPath = receiverState.job.savedPath;
  assert.equal(typeof savedPath, 'string', 'The receiver did not report a saved file.');
  const relative = path.relative(receiveDirectory, savedPath);
  assert.ok(relative && !relative.startsWith(`..${path.sep}`) && relative !== '..' &&
      !path.isAbsolute(relative), 'The received file escaped the isolated test download directory.');
  const receivedFile = fs.readFileSync(savedPath);
  assert.equal(receivedFile.length, fileContents.length, 'Received file size differs.');
  assert.equal(createHash('sha256').update(receivedFile).digest('hex') === sourceHash, true,
      'Received file SHA-256 differs.');
  checkpoint('native_file_saved_and_sha256_verified');
}

async function runScenario() {
  step = 'first browser initialization';
  const profileA = path.join(scratch, 'profile-a');
  const profileB = path.join(scratch, 'profile-b');
  const receiveDirectory = path.join(scratch, 'received-files');
  if (withFile) {
    fs.mkdirSync(path.join(profileB, 'Default'), {recursive: true});
    fs.mkdirSync(receiveDirectory, {recursive: true});
    fs.writeFileSync(path.join(profileB, 'Default', 'Preferences'), JSON.stringify({
      download: {default_directory: receiveDirectory, prompt_for_download: false},
    }));
  }
  let first = await new Browser(profileA).open();
  const initial = await first.ready();
  assert.equal(initial.authenticated, false, 'A fresh profile must start signed out.');
  if (initializeOnly) {
    checkpoint('native_initialization_without_registration');
    report.passed = true;
    return;
  }

  step = 'native registration and first device';
  const registered = await first.account('xenonAccountRegister', {username, password});
  if (typeof registered.accountId === 'string' && /^[a-f0-9]{32}$/.test(registered.accountId)) {
    report.accountId = registered.accountId;
    writeReport();
  }
  assert.equal(registered.authenticated, true, 'Native registration failed.');
  assert.equal(registered.error, '', 'Native device registration failed.');
  assert.equal(registered.username, username);
  assert.equal(registered.devices.length, 1, 'New account must contain its first device.');
  const deviceA = registered.deviceId;
  assert.equal(registered.devices[0].id, deviceA);
  assert.equal(registered.devices[0].online, true);
  checkpoint('native_registration_and_device');

  step = 'second independent profile login';
  const second = await new Browser(profileB).open();
  await second.ready();
  const loggedIn = await second.account('xenonAccountLogin', {username, password});
  assert.equal(loggedIn.authenticated, true, 'Second profile login failed.');
  assert.equal(loggedIn.error, '', 'Second device registration failed.');
  assert.equal(loggedIn.accountId, report.accountId);
  const deviceB = loggedIn.deviceId;
  assert.notEqual(deviceB, deviceA, 'Independent profiles must have independent device IDs.');
  assert.deepEqual(loggedIn.devices.map(device => device.id).sort(), [deviceA, deviceB].sort());
  checkpoint('same_account_two_devices');

  step = 'stored session after clean browser exit';
  await first.close();
  const preferencesText = fs.readFileSync(path.join(profileA, 'Default', 'Preferences'), 'utf8');
  const preferences = JSON.parse(preferencesText);
  const storedSession = preferences.xenon?.account?.credentials;
  assert.equal(typeof storedSession, 'string', 'The profile session preference is missing.');
  const session = JSON.parse(storedSession);
  // Use boolean assertions so a failed assertion cannot print a token value.
  assert.equal(typeof session.token === 'string' && /^[a-f0-9]{64}$/.test(session.token), true,
      'The session token must be stored directly as requested.');
  assert.equal(preferencesText.includes(password), false,
      'The profile preferences must not contain the account password.');
  assert.equal(Object.hasOwn(session, 'password'), false);
  checkpoint('plain_token_without_password');

  step = 'restart and native token restoration';
  first = await new Browser(profileA).open();
  const restored = await first.ready();
  assert.equal(restored.authenticated, true, 'Persisted token did not restore the session.');
  assert.equal(restored.error, '', 'Restored session could not synchronize devices.');
  assert.equal(restored.accountId, report.accountId);
  assert.equal(restored.deviceId, deviceA, 'Restart must preserve the local device identity.');
  assert.deepEqual(restored.devices.map(device => device.id).sort(), [deviceA, deviceB].sort());
  checkpoint('restart_restores_same_device');

  if (withTransfers) {
    await runTransfers(first, second, deviceA, deviceB, receiveDirectory);
  }

  step = 'second profile logout and device removal';
  const loggedOut = await second.account('xenonAccountLogout');
  assert.equal(loggedOut.authenticated, false);
  assert.equal(loggedOut.devices.length, 0);
  assert.equal(loggedOut.deviceId, '');
  // Logout clears local state immediately and revokes the device asynchronously.
  // Keep that browser alive while the other profile observes the revocation.
  const removalDeadline = Date.now() + 30000;
  let afterLogout;
  do {
    afterLogout = await first.account('xenonAccountRefresh');
    if (!afterLogout.busy && afterLogout.devices.length === 1 &&
        afterLogout.devices[0].id === deviceA) break;
    await new Promise(resolve => setTimeout(resolve, 400));
  } while (Date.now() < removalDeadline);
  assert.equal(afterLogout.authenticated, true, 'Logging out one profile must preserve the other.');
  assert.equal(afterLogout.error, '');
  assert.deepEqual(afterLogout.devices.map(device => device.id), [deviceA]);
  checkpoint('logout_removes_only_second_device');

  step = 'final local logout';
  const finalState = await first.account('xenonAccountLogout');
  assert.equal(finalState.authenticated, false);
  assert.equal(finalState.devices.length, 0);
  await first.close();
  const cleared = JSON.parse(fs.readFileSync(path.join(profileA, 'Default', 'Preferences'), 'utf8'));
  assert.equal(Boolean(cleared.xenon?.account?.credentials), false,
      'Logout must clear the stored session token.');
  checkpoint('final_logout_clears_persisted_session');
  report.passed = true;
  console.log('PASS: real native account WebUI integration; no credentials were printed.');
}

try {
  await runScenario();
} catch (error) {
  report.failedStep = step;
  report.failureType = error.name;
  report.failureMessage = redact(error.message);
  // Never print runtime arguments, returned credential fields, or assertions'
  // inspected actual values. The failed stage is enough to locate this check.
  console.error(`FAIL: ${step} (${error.name}): ${report.failureMessage}`);
  process.exitCode = 1;
} finally {
  clearTimeout(overallTimeout);
  await Promise.allSettled([...browsers].map(browser => browser.close()));
  if (selectionWritten) fs.rmSync(selectionFile, {force: true});
  // Only remove the directory returned by this invocation's mkdtempSync.
  fs.rmSync(scratch, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
  report.completedAt = new Date().toISOString();
  writeReport();
  console.log(`Result: ${resultFile}`);
}
