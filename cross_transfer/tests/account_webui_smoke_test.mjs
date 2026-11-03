// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run with Node.js 18+ and CHROME_BINARY pointing at a Chrome/Chromium binary.
// Tests the actual page code with a mock native bridge in an isolated profile.
// This does not exercise HTTPS, the native account service, or real accounts.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const resources = fileURLToPath(new URL(
    '../../xenon_overlay/resources/webui/', import.meta.url));
const executable = [
  process.env.CHROME_BINARY,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
  process.env.PROGRAMFILES && path.join(
      process.env.PROGRAMFILES, 'Google/Chrome/Application/chrome.exe'),
].filter(Boolean).find(candidate => fs.existsSync(candidate));
assert.ok(executable, 'Set CHROME_BINARY to an installed Chrome/Chromium binary.');

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'account-webui-'));
const pages = new Map();
for (const [folder, name] of [['login', 'login'], ['cross_transfer_test', 'index']]) {
  const directory = path.join(resources, folder);
  const harness = path.join(scratch, `${folder}.html`);
  const html = fs.readFileSync(path.join(directory, `${name}.html`), 'utf8');
  fs.writeFileSync(harness, html
      .replace(`<script type="module" src="${name}.js"></script>`, '')
      .replace(`href="${name}.css"`,
          `href="${pathToFileURL(path.join(directory, `${name}.css`)).href}"`));
  pages.set(folder, {
    harness,
    original: path.join(directory, `${name}.html`),
    source: fs.readFileSync(path.join(directory, `${name}.js`), 'utf8')
        .replace('void initialize();', ''),
  });
}

const browser = spawn(executable, [
  '--headless', '--no-first-run', '--no-default-browser-check',
  '--disable-background-networking', '--disable-component-update',
  '--disable-sync', '--disable-gpu', '--allow-file-access-from-files',
  `--user-data-dir=${path.join(scratch, 'profile')}`,
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
    rejectPending(new Error('Browser exited during a CDP request.'));
    resolve();
  });
  browser.once('error', error => { rejectPending(error); resolve(); });
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
  rejectPending(new Error('Account WebUI smoke test timed out.'));
  browser.kill();
}, 45000);

try {
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  const call = (method, params = {}) => cdp(method, params, sessionId);
  await call('Page.enable');
  async function run(expression) {
    const result = await call('Runtime.evaluate', {
      expression, awaitPromise: true, returnByValue: true,
    });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }
  async function until(expression) {
    for (let attempt = 0; attempt < 100; ++attempt) {
      if (await run(expression)) return;
      await new Promise(resolve => setTimeout(resolve, 30));
    }
    throw new Error(`Condition timed out: ${expression}`);
  }
  const act = expression => run(`(async () => { ${expression} })()`);
  async function open(folder, fixture = true) {
    const page = pages.get(folder);
    await call('Page.navigate', {
      url: pathToFileURL(fixture ? page.harness : page.original).href,
    });
    await until(`document.readyState === 'complete' &&
        document.getElementById('${folder === 'login' ? 'loginForm' : 'accountForm'}')`);
    if (fixture) {
      await run(page.source);
      await run(`
        globalThis.calls = [];
        globalThis.notifications = [];
        globalThis.signedOut = {authenticated: false, restoring: false, busy: false,
          username: '', accountId: '', deviceId: '', devices: [], error: ''};
        globalThis.signedIn = {authenticated: true, restoring: false, busy: false,
          username: 'smoke_user', accountId: 'a'.repeat(32), deviceId: '1'.repeat(32),
          devices: [], error: ''};
        globalThis.answer = structuredClone(signedOut);
        globalThis.chrome = {send: (...args) => notifications.push(args)};
        sendWithPromise = async (...args) => {
          calls.push(structuredClone(args));
          return structuredClone(answer);
        };
      `);
    }
  }

  // An ordinary webpage cannot use the browser's native account bridge.
  await open('login', false);
  await until(`document.getElementById('loginStatus').textContent.includes('静态预览')`);
  assert.equal(await run(`document.getElementById('loginSubmit').disabled`), true);
  await open('cross_transfer_test', false);
  await until(`document.getElementById('bridgeBadge').textContent === '静态预览'`);
  assert.equal(await run(`document.getElementById('accountSubmit').disabled`), true);
  assert.equal(await run(`document.getElementById('advancedDiagnostics').open`), false);
  await call('Emulation.setDeviceMetricsOverride', {
    width: 390, height: 844, deviceScaleFactor: 1, mobile: false,
  });
  assert.equal(await run('document.documentElement.scrollWidth <= innerWidth'), true);

  // Restore only reads native state and continues once, without a password.
  await open('login');
  await act(`answer = {...signedOut, restoring: true, busy: true}; await loadLoginState()`);
  assert.equal(await run(`$('loginSubmit').disabled && $('loginFields').disabled`), true);
  assert.match(await run(`$('loginStatus').textContent`), /恢复/);
  await act(`answer = signedIn; await loadLoginState(); await loadLoginState()`);
  assert.deepEqual(await run('notifications'), [['xenonAccountContinue', []]]);
  assert.equal(await run(`calls.every(call => call.length === 1 &&
      call[0] === 'xenonAccountGetState')`), true);
  assert.equal(await run(`$('password').value`), '');

  // A transient restore failure must not stop observing a later native retry.
  await open('login');
  await run(`
    globalThis.realSetTimeout = setTimeout;
    globalThis.scheduledDelay = null;
    globalThis.scheduledRestore = null;
    globalThis.setTimeout = (callback, delay) => {
      scheduledDelay = delay;
      scheduledRestore = callback;
      return 1;
    };
    answer = {...signedOut, error: '网络暂时不可用'};
  `);
  await act('await loadLoginState()');
  assert.equal(await run('scheduledDelay'), 5000);
  assert.equal(await run(`$('loginSubmit').disabled`), false);
  await act(`answer = signedIn; await scheduledRestore()`);
  assert.deepEqual(await run('notifications'), [['xenonAccountContinue', []]]);
  await run('globalThis.setTimeout = realSetTimeout');

  await open('login');
  await act('await loadLoginState()');
  await run(`
    answer = {...signedOut, error: '用户名或密码不正确'};
    $('username').value = 'smoke_user';
    $('password').value = 'valid-password-123';
    $('loginForm').requestSubmit();
  `);
  await until(`calls.some(call => call[0] === 'xenonAccountLogin') && !loginPending`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'xenonAccountLogin')`),
      ['xenonAccountLogin', {username: 'smoke_user', password: 'valid-password-123'}]);
  assert.equal(await run(`$('loginError').textContent`), '用户名或密码不正确');
  assert.equal(await run(`$('password').value === '' && !$('loginSubmit').disabled`), true);
  assert.equal(await run('notifications.length'), 0);

  await run(`
    $('registerMode').click();
    $('password').value = 'valid-password-123';
    $('confirmPassword').value = 'different-password';
    $('loginForm').requestSubmit();
  `);
  assert.equal(await run(`calls.some(call => call[0] === 'xenonAccountRegister')`), false);
  assert.equal(await run(`$('confirmPassword').validity.customError`), true);
  await run(`
    $('confirmPassword').value = 'valid-password-123';
    $('confirmPassword').dispatchEvent(new Event('input'));
    sendWithPromise = (...args) => {
      calls.push(structuredClone(args));
      return new Promise(resolve => globalThis.finishRegistration = resolve);
    };
    $('loginForm').requestSubmit();
    $('loginForm').dispatchEvent(new Event('submit', {cancelable: true}));
  `);
  assert.equal(await run(`calls.filter(call => call[0] === 'xenonAccountRegister').length`), 1);
  assert.equal(await run(`$('loginSubmit').disabled && $('loginMode').disabled`), true);
  await run('finishRegistration(signedIn)');
  await until('!loginPending');
  assert.deepEqual(await run('notifications'), [['xenonAccountContinue', []]]);
  assert.equal(await run(`$('password').value === '' && $('confirmPassword').value === ''`), true);

  // Network failures unlock the form and discard entered passwords.
  await open('login');
  await act(`
    await loadLoginState();
    sendWithPromise = async () => { throw new Error('offline'); };
    $('username').value = 'smoke_user';
    $('password').value = 'valid-password-123';
    $('loginForm').requestSubmit();
  `);
  await until(`!loginPending && !$('loginError').hidden`);
  assert.match(await run(`$('loginError').textContent`), /无法连接账号服务/);
  assert.equal(await run(`$('password').value === '' && !$('loginSubmit').disabled`), true);

  // Account controls retain the manual diagnostics behind a closed disclosure.
  await open('cross_transfer_test');
  await act('updateForm(); await readAccountState(); clearTimeout(accountPollTimer)');
  assert.equal(await run(`$('advancedDiagnostics').open`), false);
  await run(`
    $('accountRegisterMode').click();
    $('accountUsername').value = 'smoke_user';
    $('accountPassword').value = 'valid-password-123';
    $('accountConfirm').value = 'different-password';
    $('accountForm').requestSubmit();
  `);
  assert.equal(await run(`calls.some(call => call[0] === 'xenonAccountRegister')`), false);
  assert.equal(await run(`$('accountConfirm').validity.customError`), true);
  await run(`
    globalThis.hostileName = '<img src=x onerror="globalThis.injected=true">';
    answer = {...signedIn, devices: [
      {id: '3'.repeat(32), displayName: 'offline device', platform: 'linux', online: false,
        lastSeenAt: 1700000000},
      {id: '2'.repeat(32), displayName: hostileName, platform: '<b>win</b>', online: true},
      {id: '1'.repeat(32), displayName: 'my device', platform: 'windows', online: true},
    ]};
    $('accountConfirm').value = 'valid-password-123';
    $('accountConfirm').dispatchEvent(new Event('input'));
    $('accountForm').requestSubmit();
  `);
  await until(`!accountPending && accountState.authenticated`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'xenonAccountRegister')`),
      ['xenonAccountRegister', {username: 'smoke_user', password: 'valid-password-123'}]);
  assert.equal(await run(`$('accountPassword').value === '' && $('accountConfirm').value === ''`), true);
  assert.equal(await run(`$('deviceList').children.length`), 3);
  assert.equal(await run(`$('deviceList').firstElementChild.querySelector('.device-name').textContent`),
      'my device（本机）');
  assert.equal(await run(`$('deviceList').children[1].querySelector('.device-name').textContent`),
      '<img src=x onerror="globalThis.injected=true">');
  assert.equal(await run(`$('deviceList').querySelectorAll('img,b').length`), 0);
  assert.equal(await run('Boolean(globalThis.injected)'), false);
  assert.equal(await run(`$('accountSignedOut').hidden && !$('accountDevices').hidden`), true);
  assert.match(await run(`$('deviceList').lastElementChild.textContent`), /离线/);
  assert.equal(await run('document.documentElement.scrollWidth <= innerWidth'), true);
  await run(`$('accountRefresh').click()`);
  await until(`!accountReadInFlight && calls.some(call => call[0] === 'xenonAccountRefresh')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'xenonAccountRefresh')`),
      ['xenonAccountRefresh']);

  // Polls and heartbeat timestamps must preserve the focused send button.
  await run(`
    globalThis.focusedDevice = $('deviceList').querySelector('.device-send');
    focusedDevice.focus();
  `);
  await act(`
    answer = {...answer, devices: answer.devices.map(device =>
      device.online ? {...device, lastSeenAt: 1700000060} : device)};
    await readAccountState();
    clearTimeout(accountPollTimer);
    clearTimeout(ownTransferPollTimer);
  `);
  assert.equal(await run('document.activeElement === focusedDevice'), true);
  assert.equal(await run('focusedDevice.isConnected'), true);

  // A changed offline timestamp redraws rows without losing another device's
  // focus. Presence changes still reorder rows and remove unavailable actions.
  await act(`
    answer = {...answer, devices: answer.devices.map(device =>
      device.online ? device : {...device, lastSeenAt: 1700000060})};
    await readAccountState();
    clearTimeout(accountPollTimer);
    clearTimeout(ownTransferPollTimer);
  `);
  assert.equal(await run('document.activeElement.dataset.deviceId'), '2'.repeat(32));
  assert.equal(await run(`$('deviceList').lastElementChild.querySelector('.device-meta')
      .textContent.includes(deviceLastSeen(1700000060))`), true);
  await act(`
    answer = {...answer, devices: answer.devices.map(device => ({...device,
      online: device.id !== '2'.repeat(32)}))};
    await readAccountState();
    clearTimeout(accountPollTimer);
    clearTimeout(ownTransferPollTimer);
  `);
  assert.equal(await run(`$('deviceList').querySelectorAll('.device-send').length`), 1);
  assert.equal(await run(`$('deviceList').children[1].querySelector('.device-send')
      .dataset.deviceId`), '3'.repeat(32));
  assert.equal(await run(`$('deviceList').lastElementChild.querySelector('.device-send')`), null);
  assert.match(await run(`$('deviceList').lastElementChild.textContent`), /离线/);

  // A request sent before logout must not redraw the old authenticated state.
  await run(`
    clearTimeout(accountPollTimer);
    sendWithPromise = (...args) => {
      calls.push(structuredClone(args));
      if (args[0] === 'xenonAccountGetState') {
        return new Promise(resolve => globalThis.finishOldState = resolve);
      }
      return Promise.resolve(signedOut);
    };
    globalThis.oldRead = readAccountState();
    $('accountLogout').click();
  `);
  await until('!accountPending && !accountState.authenticated');
  await act(`finishOldState(signedIn); await oldRead; clearTimeout(accountPollTimer)`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'xenonAccountLogout')`),
      ['xenonAccountLogout']);
  assert.equal(await run('accountState.authenticated'), false);
  assert.equal(await run(`!$('accountSignedOut').hidden && $('accountDevices').hidden &&
      $('deviceList').children.length === 0`), true);
  assert.equal(await run(`$('accountSubmit').disabled`), false);

  // Same-account transfers never ask the page for connection credentials.
  await open('cross_transfer_test');
  await act(`
    globalThis.ownAnswer = {transfers: [], job: {active: false}, busy: false, error: '', useStun: false};
    globalThis.fileAnswer = {name: '<img onerror=alert(1)>.txt', size: 1024};
    sendWithPromise = async (...args) => {
      calls.push(structuredClone(args));
      if (args[0] === 'ownTransferSetUseStun') ownAnswer.useStun = args[1].enabled;
      return structuredClone(args[0] === 'ownTransferChooseFile' ? fileAnswer : ownAnswer);
    };
    renderAccountState({...signedIn, devices: [
      {id: '1'.repeat(32), displayName: 'local', online: true},
      {id: '2'.repeat(32), displayName: '<b>remote</b>', online: true},
      {id: '3'.repeat(32), displayName: 'offline', online: false},
    ]});
    await readOwnTransferState();
    clearTimeout(ownTransferPollTimer);
  `);
  assert.equal(await run(`$('deviceList').querySelectorAll('.device-send').length`), 1);
  assert.equal(await run(`$('ownTransferPanel').hidden`), false);
  assert.equal(await run('ownRecipientId'), '2'.repeat(32));
  assert.equal(await run(`$('ownSendText').disabled`), true);
  assert.equal(await run(`!$('ownUseStun').checked && !$('ownUseStun').disabled`), true);
  assert.equal(await run(`$('ownUseStun').labels[0].textContent.includes('使用 STUN')`), true);
  assert.match(await run(`$($('ownUseStun').getAttribute('aria-describedby')).textContent`),
      /下一次传输/);
  await run(`$('ownUseStun').click()`);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferSetUseStun')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferSetUseStun')`),
      ['ownTransferSetUseStun', {enabled: true}]);
  await act(`await readOwnTransferState(); clearTimeout(ownTransferPollTimer)`);
  assert.equal(await run(`$('ownUseStun').checked`), true);
  // A rejected preference write restores the last confirmed native state.
  await run(`
    globalThis.ownBridge = sendWithPromise;
    sendWithPromise = async (...args) => {
      if (args[0] === 'ownTransferSetUseStun') throw new Error('test rejection');
      return ownBridge(...args);
    };
    $('ownUseStun').click();
  `);
  await until(`!ownTransferPending && !$('ownTransferError').hidden`);
  assert.equal(await run(`$('ownUseStun').checked`), true);
  // Changes remain available during a transfer and do not cancel that job.
  await run(`
    sendWithPromise = ownBridge;
    ownAnswer = {...ownAnswer, job: {active: true}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownUseStun').disabled`), false);
  await run(`$('ownUseStun').click()`);
  await until(`!ownTransferPending && ownTransferState.useStun === false`);
  assert.equal(await run(`calls.some(call => call[0] === 'ownTransferCancel')`), false);
  assert.equal(await run(`ownTransferState.job.active`), true);
  await run(`ownAnswer = {...ownAnswer, job: {active: false}}; renderOwnTransferState(ownAnswer)`);
  await run(`
    $('ownText').value = 'hello 另一台设备 👋';
    $('ownText').dispatchEvent(new Event('input'));
  `);
  assert.equal(await run(`$('ownRecipient').textContent`), '发送到：<b>remote</b>');
  assert.equal(await run(`$('ownRecipient').querySelector('b') === null`), true);
  assert.equal(await run(`$('ownSendText').disabled`), false);
  await run('renderAccountState(structuredClone(accountState))');
  assert.equal(await run(`$('deviceList').querySelector('.device-send').textContent`), '已选择');
  assert.equal(await run(`$('deviceList').querySelector('.device-send')
      .getAttribute('aria-pressed')`), 'true');
  // Keep an explicit selection across polls and fall back when it goes offline.
  await run(`
    globalThis.originalDevices = structuredClone(accountState.devices);
    renderAccountState({...accountState, devices: [...originalDevices,
      {id: '4'.repeat(32), displayName: 'third device', online: true}]});
  `);
  assert.equal(await run(`$('deviceList').querySelectorAll('.device-send')[1].textContent`), '选择');
  await run(`
    $('deviceList').querySelectorAll('.device-send')[1].click();
    renderAccountState(structuredClone(accountState));
  `);
  assert.equal(await run('ownRecipientId'), '4'.repeat(32));
  await run(`renderAccountState({...accountState, devices: accountState.devices.map(
    device => device.id === '4'.repeat(32) ? {...device, online: false} : device)})`);
  assert.equal(await run('ownRecipientId'), '2'.repeat(32));
  await run(`renderAccountState({...accountState, devices: originalDevices.map(
    device => ({...device, online: device.id === accountState.deviceId}))})`);
  assert.equal(await run('ownRecipientId'), '');
  assert.equal(await run(`$('ownSendText').disabled`), true);
  await run('renderAccountState({...accountState, devices: originalDevices})');
  assert.equal(await run('ownRecipientId'), '2'.repeat(32));
  await run(`$('ownTextForm').requestSubmit()`);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferSendText')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferSendText')`),
      ['ownTransferSendText', {deviceId: '2'.repeat(32), text: 'hello 另一台设备 👋'}]);
  assert.equal(await run(`$('ownText').value`), '');

  await run(`
    $('ownText').value = '文'.repeat(6000);
    $('ownText').dispatchEvent(new Event('input'));
  `);
  assert.equal(await run(`$('ownSendText').disabled && $('ownText').validity.customError`), true);
  await run(`$('ownText').value = ''; $('ownText').dispatchEvent(new Event('input'));
    $('ownChooseFile').click()`);
  await until('!ownTransferPending && ownSelectedFile !== null');
  assert.equal(await run(`$('ownFileName').textContent`), '<img onerror=alert(1)>.txt');
  assert.equal(await run(`$('ownFileName').querySelector('img') === null`), true);
  await run(`$('ownSendFile').click()`);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferSendFile')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferSendFile')`),
      ['ownTransferSendFile', {deviceId: '2'.repeat(32)}]);
  assert.equal(await run('ownSelectedFile'), null);

  await run(`
    globalThis.incomingText = {id: 'a'.repeat(32), senderDeviceId: '2'.repeat(32),
      receiverDeviceId: '1'.repeat(32), senderName: '<script>bad</script>',
      receiverName: 'local', kind: 'text', text: '<img src=x onerror="globalThis.ownInjected=true">',
      fileName: '', fileSize: 0, status: 'pending', createdAt: 2};
    globalThis.incomingFile = {...incomingText, id: 'b'.repeat(32), kind: 'file',
      text: '', fileName: 'report.txt', fileSize: 1234, status: 'accepted', createdAt: 1};
    ownAnswer = {...ownAnswer, transfers: [incomingFile, incomingText]};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownTransferList').firstElementChild.dataset.transferId`), 'a'.repeat(32));
  assert.equal(await run(`$('ownTransferList').querySelector('.own-transfer-content').textContent`), '文本消息');
  assert.equal(await run(`$('ownTransferList').querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  assert.match(await run(`$('ownTransferList').firstElementChild.textContent`), /等待自动接收/);
  assert.match(await run(`$('ownTransferList').lastElementChild.textContent`), /传输已中断.*重新发送/);
  assert.equal(await run(`$('ownTransferList').lastElementChild.querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  assert.equal(await run(`$('ownTransferList').lastElementChild.querySelector('[data-transfer-action="cancel"]').disabled`), false);
  await run(`
    ownAnswer = {...ownAnswer, transfers: [{...incomingText, status: 'rejected'}, incomingFile]};
    $('ownTransferList').querySelector('[data-transfer-action="reject"]').click();
  `);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferRespond')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferRespond')`),
      ['ownTransferRespond', {id: 'a'.repeat(32), action: 'reject'}]);
  await run(`
    ownAnswer = {...ownAnswer, transfers: [{...incomingText, status: 'completed'}, incomingFile]};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownTransferList').querySelector('.own-received-text').textContent`),
      '<img src=x onerror="globalThis.ownInjected=true">');
  assert.equal(await run(`$('ownTransferList').querySelectorAll('img,script').length`), 0);
  assert.equal(await run('Boolean(globalThis.ownInjected)'), false);
  assert.match(await run(`$('ownTransferList').textContent`), /复制文本/);

  // An active transfer keeps the file slot reserved and remains cancellable.
  await run(`
    ownSelectedFile = {name: 'another.txt', size: 1};
    ownAnswer = {...ownAnswer,
      transfers: ownAnswer.transfers.map(item => item.id === incomingFile.id ?
        {...item, status: 'pending'} : item),
      job: {active: true, transferId: incomingFile.id, state: 'confirming'}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownJobStatus').textContent`), '正在确认接收结果');
  assert.equal(await run(`$('ownChooseFile').disabled && $('ownSendFile').disabled`), true);
  assert.equal(await run(`$('ownTransferList').querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  assert.equal(await run(`$('ownCancelJob').disabled`), false);

  await run(`
    ownSelectedFile = null;
    ownAnswer = {...ownAnswer, job: {active: false}};
    renderOwnTransferState(ownAnswer);
  `);

  // Streamed text carries only metadata until the accepted payload arrives.
  // It shares the active-job limit and route display with files.
  await run(`
    globalThis.incomingStream = {...incomingText, id: 'c'.repeat(32),
      payloadMode: 'stream', text: '', fileName: 'message.txt', fileSize: 42,
      status: 'pending', createdAt: 3};
    ownAnswer = {...ownAnswer,
      transfers: [incomingStream, {...incomingFile, status: 'pending'}]};
    renderOwnTransferState(ownAnswer);
  `);
  assert.match(await run(`$('ownTransferList').firstElementChild.textContent`), /文本消息.*42 B/);
  assert.equal(await run(`$('ownTransferList').firstElementChild.textContent.includes('message.txt')`), false);
  assert.equal(await run(`$('ownTransferList').firstElementChild.querySelector('.own-received-text')`), null);
  await run(`
    ownAnswer = {...ownAnswer,
      transfers: [{...incomingStream, status: 'accepted'}, {...incomingFile, status: 'pending'}],
      job: {active: true, transferId: incomingStream.id, kind: 'text',
        state: 'connecting', transport: 'auto', totalBytes: 42, transferredBytes: 0,
        savedPath: '/private/temp/message.txt'}};
    renderOwnTransferState(ownAnswer);
  `);
  await until(`!ownTransferPending && ownTransferState.job.active`);
  assert.equal(await run(`$('ownJobTransport').textContent`), '正在选择连接方式');
  assert.equal(await run(`$('ownJobSavedPath').hidden && $('ownJobSavedPath').textContent === ''`), true);
  await run(`$('ownText').value = 'next message'; $('ownText').dispatchEvent(new Event('input'))`);
  assert.equal(await run(`$('ownSendText').disabled && $('ownChooseFile').disabled`), true);
  assert.equal(await run(`$('ownTransferList')
      .querySelector('[data-transfer-action="reject"]').disabled`), false);
  await run(`
    ownAnswer = {...ownAnswer, transfers: [{...incomingStream,
      status: 'completed', text: '<b>received through direct TLS</b>'}, incomingFile],
      job: {...ownAnswer.job, active: false, state: 'completed', transport: 'direct',
        transferredBytes: 42, percent: 100}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownJobTransport').textContent`), '设备直连');
  assert.equal(await run(`$('ownTransferList').firstElementChild.querySelector('.own-received-text')
      .textContent`), '<b>received through direct TLS</b>');
  assert.equal(await run(`$('ownTransferList').firstElementChild.querySelector('b')`), null);
  assert.match(await run(`$('ownTransferList').firstElementChild.textContent`), /复制文本/);
  assert.equal(await run(`$('ownSendText').disabled`), false);
  await run(`
    renderOwnTransferState({...ownAnswer, transfers: [{...incomingStream, status: 'completed'}]});
  `);
  assert.match(await run(`$('ownTransferList').textContent`), /文本内容暂不可用/);
  assert.equal(await run(`$('ownTransferList').textContent.includes('复制文本')`), false);

  // Accepted streamed text without a live job follows the same interrupted
  // transfer guidance as files; retrying a stale session is not offered.
  await run(`
    renderOwnTransferState({...ownAnswer,
      transfers: [{...incomingStream, status: 'accepted'}], job: {active: false}});
  `);
  assert.match(await run(`$('ownTransferList').textContent`), /传输已中断.*重新发送/);
  assert.equal(await run(`$('ownTransferList').querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  assert.equal(await run(`$('ownTransferList').querySelector('[data-transfer-action="cancel"]').disabled`), false);

  await run(`
    ownAnswer = {...ownAnswer, transfers: [incomingFile],
      job: {active: true, transferId: incomingFile.id,
      state: 'receiving', transport: 'relay', role: 'receiver', totalBytes: 1000, transferredBytes: 420,
      percent: 42, savedPath: '', error: ''}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownJob').hidden`), false);
  assert.equal(await run(`$('ownJobProgress').value`), 42);
  assert.equal(await run(`$('ownJobTransport').textContent`), '中继传输');
  assert.equal(await run(`$('ownChooseFile').disabled && !$('ownCancelJob').disabled`), true);
  assert.equal(await run(`$('ownTransferList').querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  await run(`$('ownCancelJob').click()`);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferCancel')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferCancel')`), ['ownTransferCancel']);
  assert.equal(await run('document.documentElement.scrollWidth <= innerWidth'), true);

  // A delayed control message leaves refresh and the next transfer usable.
  await run(`
    ownAnswer = {...ownAnswer, error: '消息暂未送达，稍后重试',
      job: {...ownAnswer.job, active: false, state: 'completed', percent: 100}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownTransferRefresh').disabled`), false);
  assert.match(await run(`$('ownTransferError').textContent`), /消息暂未送达/);
  assert.equal(await run(`$('ownChooseFile').disabled`), false);
  await run(`$('ownTransferRefresh').click()`);
  await until(`!ownTransferReadInFlight && calls.some(call => call[0] === 'ownTransferRefresh')`);

  await run(`
    renderOwnTransferState({...ownAnswer, transfers: [
      {...incomingFile, status: 'completed', savedPath: 'C:/received/report.txt'},
      {...incomingStream, status: 'failed', error: '接收结果未确认，请检查接收设备'}
    ]});
  `);
  assert.match(await run(`$('ownTransferList').textContent`), /已保存到：C:\/received\/report.txt/);
  assert.match(await run(`$('ownTransferList').textContent`), /接收结果未确认/);

  // Late transfer cache replies must not repopulate a signed-out profile.
  await run(`
    clearTimeout(ownTransferPollTimer);
    sendWithPromise = () => new Promise(resolve => globalThis.finishOwnRead = resolve);
    globalThis.oldOwnRead = readOwnTransferState();
    renderAccountState(signedOut);
  `);
  await act('finishOwnRead(ownAnswer); await oldOwnRead');
  assert.equal(await run(`$('ownTransferPanel').hidden &&
      $('ownTransferList').children.length === 0 && $('ownText').value === ''`), true);
  assert.equal(await run('ownRecipientId'), '');

  console.log('PASS: account preview guards, restore/continue, login parameters, ' +
      'registration confirmation, duplicate submit, errors/password clearing, ' +
      'default device selection, manual selection and offline fallback, focus retention and XSS safety, ' +
      'refresh/logout, device-targeted text/files, automatic receive and interrupted-transfer guidance, ' +
      'streamed text and direct/relay routes, ' +
      'transfer progress/cancel, byte limits, ' +
      'stale responses, mobile layout.');
} finally {
  clearTimeout(timeout);
  await cdp('Browser.close').catch(() => {});
  await browserExited;
  fs.rmSync(scratch, {recursive: true, force: true});
}
