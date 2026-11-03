// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Runs a real browser/native account integration against the configured server.
// Set CHROME_BINARY to a freshly built Xenon executable. Two temporary browser
// profiles and one generated server account are used. No bridge is mocked.
// The result file records only the generated username/account ID so an operator
// can remove that account from the server after this test; no password/token is
// printed or written by this script. Native profile preferences are deleted.
// --with-transfers also tests automatic text reception without UI acceptance.
// --with-file additionally opens a visible native chooser for operator-driven
// selection, then verifies a real 1 MiB file transfer and its SHA-256.
// --expect-transport=direct|relay also enables text tests and checks that both
// endpoints used the requested route. File routes are checked with --with-file.
// --expect-small-transport=direct|relay additionally verifies a separate text
// payload below 1024 bytes against the server's configured small-payload policy.
// --fresh-direct-runs=N requires --expect-transport=direct and exercises 1..64
// new direct connections on the same account, clearing both caches each round.
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
const transportArguments = process.argv.filter(argument => argument.startsWith('--expect-transport='));
assert.ok(transportArguments.length <= 1, 'Specify --expect-transport only once.');
const expectedTransport = transportArguments[0]?.split('=')[1];
assert.ok(expectedTransport === undefined || ['direct', 'relay'].includes(expectedTransport),
    '--expect-transport must be direct or relay.');
const smallTransportArguments = process.argv.filter(argument =>
  argument.startsWith('--expect-small-transport='));
assert.ok(smallTransportArguments.length <= 1, 'Specify --expect-small-transport only once.');
const expectedSmallTransport = smallTransportArguments[0]?.split('=')[1];
assert.ok(expectedSmallTransport === undefined || ['direct', 'relay'].includes(expectedSmallTransport),
    '--expect-small-transport must be direct or relay.');
const freshDirectArguments = process.argv.filter(argument =>
  argument === '--fresh-direct-runs' || argument.startsWith('--fresh-direct-runs='));
assert.ok(freshDirectArguments.length <= 1, 'Specify --fresh-direct-runs only once.');
const freshDirectValue = freshDirectArguments[0]?.slice('--fresh-direct-runs='.length);
const freshDirectRuns = freshDirectValue === undefined ? 0 : Number(freshDirectValue);
if (freshDirectValue !== undefined) {
  assert.ok(/^[1-9][0-9]?$/.test(freshDirectValue) && freshDirectRuns <= 64,
      '--fresh-direct-runs must be an integer from 1 to 64.');
  assert.equal(expectedTransport, 'direct',
      '--fresh-direct-runs requires --expect-transport=direct.');
  assert.equal(process.argv.includes('--initialize-only'), false,
      '--fresh-direct-runs cannot be combined with --initialize-only.');
}

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-account-native-'));
const resultFile = path.join(root, 'out', 'account_browser_integration_result.json');
const username = `acct_smoke_${Date.now().toString(36)}_${randomBytes(5).toString('hex')}`;
const password = `Xenon-smoke-${randomBytes(24).toString('base64url')}`;
const report = {
  startedAt: new Date().toISOString(), username, accountId: null,
  passed: false, checks: {},
};
if (freshDirectRuns) {
  report.freshDirect = {requestedRuns: freshDirectRuns, completedRuns: 0, runs: []};
}
const browsers = new Set();
const initializeOnly = process.argv.includes('--initialize-only');
const withFile = process.argv.includes('--with-file');
const withTransfers = withFile || process.argv.includes('--with-transfers') ||
    Boolean(expectedTransport) || Boolean(expectedSmallTransport);
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
// Each extra round allows the 120-second payload wait plus invitation/setup.
const overallTimeout = setTimeout(() => {
  for (const browser of browsers) browser.child.kill();
}, (withFile ? 600000 : 240000) + freshDirectRuns * 180000);

async function sendText(first, deviceA, deviceB, text) {
  const state = await first.transfer('ownTransferSendText', {deviceId: deviceB, text});
  assert.equal(state.error || '', '', 'Native text send failed.');
  const transfer = state.transfers.find(item => item.kind === 'text' && item.text === text &&
      item.senderDeviceId === deviceA && item.receiverDeviceId === deviceB);
  assert.ok(transfer, 'Sent text invitation was not returned.');
  assert.equal(transfer.payloadMode, 'stream', 'Text must use the device payload channel.');
  assert.equal(transfer.fileSize, Buffer.byteLength(text, 'utf8'));
  assert.equal(transfer.status, 'pending', 'Text must wait for receiver acceptance.');
  return transfer.id;
}

async function waitPayload(first, second, id, kind, expectedRoute = expectedTransport) {
  const deadline = Date.now() + 120000;
  let nextProgress = 0;
  while (Date.now() < deadline) {
    const [senderState, receiverState] = await Promise.all([
      first.transfer('ownTransferGetState'), second.transfer('ownTransferGetState'),
    ]);
    const senderRecord = senderState.transfers.find(item => item.id === id);
    const receiverRecord = receiverState.transfers.find(item => item.id === id);
    const senderJob = senderState.job;
    const receiverJob = receiverState.job;
    const completedJob = job => job.transferId === id && !job.active &&
        ['completed', 'complete', 'succeeded'].includes(job.state);
    if (completedJob(senderJob) && completedJob(receiverJob) &&
        senderRecord?.status === 'completed' && receiverRecord?.status === 'completed') {
      assert.ok(['direct', 'relay'].includes(senderJob.transport),
          'The sender did not report its actual payload route.');
      assert.equal(receiverJob.transport, senderJob.transport,
          'The two endpoints did not agree on their payload route.');
      assert.equal(senderJob.protocol, senderJob.transport === 'direct' ? 'webrtc' : 'https',
          'The payload did not use the native WebRTC or HTTPS adapter.');
      assert.equal(receiverJob.protocol, senderJob.protocol,
          'The two endpoints did not agree on their payload protocol.');
      if (expectedRoute) {
        assert.equal(senderJob.transport, expectedRoute,
            `${kind} did not use the expected route.`);
      }
      report.transports ||= {};
      report.transports[kind] = senderJob.transport;
      report.protocols ||= {};
      report.protocols[kind] = senderJob.protocol;
      checkpoint(`native_${kind}_transport_verified`);
      return {senderState, receiverState, senderRecord, receiverRecord};
    }
    if ([senderJob.state, receiverJob.state, senderRecord?.status, receiverRecord?.status]
        .some(value => ['failed', 'cancelled', 'expired'].includes(value))) {
      throw new Error(`Native ${kind} transfer failed: ${redact(
          senderJob.error || receiverJob.error || receiverRecord?.status || senderRecord?.status)}`);
    }
    if (Date.now() >= nextProgress) {
      console.log(`WAIT: ${kind} sender=${senderJob.state || 'waiting'} receiver=${receiverJob.state || 'waiting'}`);
      nextProgress = Date.now() + 10000;
    }
    await new Promise(resolve => setTimeout(resolve, 400));
  }
  throw new Error(`Both native ${kind} jobs did not complete within 120 seconds.`);
}

async function runTransfers(first, second, deviceA, deviceB, receiveDirectory) {
  if (expectedSmallTransport) {
    step = 'native small text without a cached direct connection';
    const smallText = `小消息 👋 ${randomBytes(4).toString('hex')}`;
    assert.ok(Buffer.byteLength(smallText, 'utf8') < 1024);
    const smallId = await sendText(first, deviceA, deviceB, smallText);
    const smallReceived = await waitPayload(first, second, smallId, 'small_text', expectedSmallTransport);
    assert.equal(smallReceived.receiverRecord.text, smallText, 'Small text changed in transit.');
    assert.equal(smallReceived.senderRecord.text, smallText);
    assert.equal(smallReceived.receiverState.job.savedPath || '', '');
    checkpoint('native_small_text_received');
  }

  step = 'native Unicode text transfer';
  console.log(`STEP: ${step}`);
  const unicodeSample = `跨设备文本\nHello 👋 — <b>literal text</b> ${randomBytes(4).toString('hex')}`;
  // The default server policy sends payloads below 1024 bytes straight to
  // relay. A requested direct check must exercise a larger payload.
  const unicodeText = expectedTransport === 'direct' ?
      unicodeSample.repeat(Math.ceil(1024 / Buffer.byteLength(unicodeSample, 'utf8'))) : unicodeSample;
  const textId = await sendText(first, deviceA, deviceB, unicodeText);
  const received = await waitPayload(first, second, textId, 'text');
  assert.equal(received.receiverRecord.senderDeviceId, deviceA);
  assert.equal(received.receiverRecord.receiverDeviceId, deviceB);
  assert.equal(received.receiverRecord.payloadMode, 'stream');
  assert.equal(received.receiverRecord.text, unicodeText, 'Unicode text changed in transit.');
  assert.equal(received.senderRecord.text, unicodeText);
  assert.equal(received.receiverState.job.savedPath || '', '',
      'A text transfer must not expose its temporary payload path.');
  checkpoint('native_unicode_text_received');
  checkpoint('native_text_received_without_ui_acceptance');

  if (expectedTransport === 'direct') {
    for (const reverse of [false, true]) {
      step = reverse ? 'reuse direct connection in reverse' : 'reuse direct connection for small text';
      const sender = reverse ? second : first;
      const receiver = reverse ? first : second;
      const text = `复用直连 ${randomBytes(4).toString('hex')}`;
      assert.ok(Buffer.byteLength(text, 'utf8') < 1024);
      const id = await sendText(sender, reverse ? deviceB : deviceA,
          reverse ? deviceA : deviceB, text);
      const received = await waitPayload(sender, receiver, id,
          reverse ? 'reused_reverse_text' : 'reused_small_text', 'direct');
      assert.equal(received.receiverRecord.text, text);
    }
    checkpoint('native_direct_reuse_without_server_change');

    step = 'native direct transfer with STUN disabled';
    for (const browser of [first, second]) {
      const state = await browser.transfer('ownTransferSetUseStun', {enabled: false});
      assert.equal(state.useStun, false);
    }
    const hostText = `清空连接缓存后的小消息 ${randomBytes(4).toString('hex')}`;
    assert.ok(Buffer.byteLength(hostText, 'utf8') < 1024);
    const hostId = await sendText(first, deviceA, deviceB, hostText);
    const hostReceived = await waitPayload(first, second, hostId, 'text_without_stun', 'direct');
    assert.equal(hostReceived.receiverRecord.text, hostText);
    checkpoint('native_stun_optional');
    checkpoint('native_small_text_prefers_direct_after_cache_clear');
  }

  // Pending rejection/cancellation is covered by service tests that hold the
  // receive slot busy. An idle receiver now accepts automatically, so racing
  // a UI response against polling would make this integration test flaky.

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

  step = 'native file invitation and automatic reception';
  const offered = await first.transfer('ownTransferSendFile', {deviceId: deviceB});
  assert.equal(offered.error || '', '', 'Native file send failed.');
  const fileTransfer = offered.transfers.find(item => item.kind === 'file' &&
      item.senderDeviceId === deviceA && item.receiverDeviceId === deviceB &&
      item.fileName === path.basename(sourcePath));
  assert.ok(fileTransfer, 'File invitation was not returned.');
  assert.equal(fileTransfer.fileSize, fileContents.length);

  step = 'native file transfer and SHA-256 verification';
  const {receiverState} = await waitPayload(first, second, fileTransfer.id, 'file');
  checkpoint('native_file_received_without_ui_acceptance');
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

async function runFreshDirectTransfers(first, second, deviceA, deviceB) {
  for (let round = 1; round <= freshDirectRuns; ++round) {
    step = `fresh direct connection ${round}/${freshDirectRuns}`;
    console.log(`STEP: ${step}`);
    const run = {round, startedAt: new Date().toISOString(), passed: false};
    report.freshDirect.runs.push(run);
    writeReport();
    // Setting false again would leave an already-disabled profile's cache
    // intact. Toggle both ways while idle to force new ICE on every round.
    for (const enabled of [true, false]) {
      for (const browser of [first, second]) {
        const state = await browser.transfer('ownTransferSetUseStun', {enabled});
        assert.equal(state.useStun, enabled);
        assert.equal(state.job.active, false, 'Cache reset requires an idle transfer.');
      }
    }
    const text = `新直连 ${round}/${freshDirectRuns} ${randomBytes(8).toString('hex')}`;
    assert.ok(Buffer.byteLength(text, 'utf8') < 1024);
    const id = await sendText(first, deviceA, deviceB, text);
    run.transferId = id;
    writeReport();
    const kind = `fresh_direct_${String(round).padStart(3, '0')}`;
    const received = await waitPayload(first, second, id, kind, 'direct');
    assert.equal(received.senderRecord.text, text);
    assert.equal(received.receiverRecord.text, text, 'Fresh direct text changed in transit.');
    Object.assign(run, {
      passed: true,
      completedAt: new Date().toISOString(),
      senderTransport: received.senderState.job.transport,
      receiverTransport: received.receiverState.job.transport,
      protocol: received.senderState.job.protocol,
    });
    report.freshDirect.completedRuns = round;
    writeReport();
  }
  if (freshDirectRuns) checkpoint('native_fresh_direct_stress_completed');
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

  const initialTransfer = await first.transfer('ownTransferGetState');
  assert.equal(initialTransfer.useStun, false, 'STUN must be optional and disabled by default.');
  const enabledTransfer = await first.transfer('ownTransferSetUseStun', {enabled: true});
  assert.equal(enabledTransfer.useStun, true);

  step = 'second independent profile login';
  let second = await new Browser(profileB).open();
  await second.ready();
  const loggedIn = await second.account('xenonAccountLogin', {username, password});
  assert.equal(loggedIn.authenticated, true, 'Second profile login failed.');
  assert.equal(loggedIn.error, '', 'Second device registration failed.');
  assert.equal(loggedIn.accountId, report.accountId);
  const deviceB = loggedIn.deviceId;
  assert.notEqual(deviceB, deviceA, 'Independent profiles must have independent device IDs.');
  assert.deepEqual(loggedIn.devices.map(device => device.id).sort(), [deviceA, deviceB].sort());
  checkpoint('same_account_two_devices');
  const otherTransfer = await second.transfer('ownTransferGetState');
  assert.equal(otherTransfer.useStun, false, 'STUN preference must be local to each profile.');
  assert.equal((await second.transfer('ownTransferSetUseStun', {enabled: true})).useStun, true);

  step = 'stored session after clean browser exit';
  await first.close();
  const preferencesText = fs.readFileSync(path.join(profileA, 'Default', 'Preferences'), 'utf8');
  const preferences = JSON.parse(preferencesText);
  assert.equal(preferences.xenon?.account?.transfer_use_stun, true,
      'The STUN preference did not persist.');
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
  assert.equal((await first.transfer('ownTransferGetState')).useStun, true);
  checkpoint('native_stun_setting_persists');

  if (withTransfers) {
    await runTransfers(first, second, deviceA, deviceB, receiveDirectory);
    await runFreshDirectTransfers(first, second, deviceA, deviceB);

    step = 'completed transfer records survive both browser restarts';
    const states = await Promise.all([
      first.transfer('ownTransferGetState'), second.transfer('ownTransferGetState'),
    ]);
    const completed = states.map(state => state.transfers.filter(
        transfer => transfer.status === 'completed'));
    assert.ok(completed.every(records => records.length > 0),
        'Both profiles must have completed transfer records.');
    await Promise.all([first.close(), second.close()]);
    for (const [index, profile] of [profileA, profileB].entries()) {
      const saved = JSON.parse(fs.readFileSync(path.join(profile, 'Default', 'Preferences'), 'utf8'));
      const history = saved.xenon?.account?.transfer_history;
      const deviceId = index === 0 ? deviceA : deviceB;
      const scope = history?.[`${report.accountId}:${deviceId}`];
      assert.ok(scope && typeof scope === 'object', 'Local task history was not saved.');
      assert.equal(scope.accountId, report.accountId);
      assert.equal(scope.deviceId, deviceId);
      if (expectedTransport === 'direct') {
        assert.ok(scope.directPeers?.[index === 0 ? deviceB : deviceA] > 0,
            'Direct success must persist independently of live connections.');
      }
      for (const transfer of completed[index]) {
        const task = scope.tasks?.[transfer.id];
        assert.equal(task?.status, 'completed');
      }
    }
    [first, second] = await Promise.all([
      new Browser(profileA).open(), new Browser(profileB).open(),
    ]);
    const restoredAccounts = await Promise.all([first.ready(), second.ready()]);
    assert.deepEqual(restoredAccounts.map(state => state.deviceId), [deviceA, deviceB]);
    const restoredTransfers = await Promise.all([
      first.transfer('ownTransferGetState'), second.transfer('ownTransferGetState'),
    ]);
    for (let index = 0; index < completed.length; ++index) {
      assert.equal(restoredTransfers[index].job.active, false,
          'Restoring completed transfers must not reserve a transfer slot.');
      for (const transfer of completed[index]) {
        const record = restoredTransfers[index].transfers.find(item => item.id === transfer.id);
        assert.equal(record?.status, 'completed', 'A completed transfer was lost after restart.');
        if (transfer.kind === 'text') assert.equal(record.text, transfer.text);
      }
    }
    checkpoint('native_completed_transfers_survive_restart');
    if (expectedTransport === 'direct') {
      const text = `本地直连偏好 ${randomBytes(4).toString('hex')}`;
      const id = await sendText(first, deviceA, deviceB, text);
      const received = await waitPayload(first, second, id,
          'small_text_with_persisted_direct_preference', 'direct');
      assert.equal(received.receiverRecord.text, text);
      checkpoint('native_direct_preference_survives_cache_loss');
    }
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
