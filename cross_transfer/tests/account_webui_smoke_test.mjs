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
  await run(`
    globalThis.ownAnswer = {transfers: [], job: {active: false}, busy: false, error: ''};
    globalThis.fileAnswer = {name: '<img onerror=alert(1)>.txt', size: 1024};
    sendWithPromise = async (...args) => {
      calls.push(structuredClone(args));
      return structuredClone(args[0] === 'ownTransferChooseFile' ? fileAnswer : ownAnswer);
    };
    renderAccountState({...signedIn, devices: [
      {id: '1'.repeat(32), displayName: 'local', online: true},
      {id: '2'.repeat(32), displayName: '<b>remote</b>', online: true},
      {id: '3'.repeat(32), displayName: 'offline', online: false},
    ]});
    clearTimeout(ownTransferPollTimer);
  `);
  assert.equal(await run(`$('deviceList').querySelectorAll('.device-send').length`), 1);
  assert.equal(await run(`$('ownTransferPanel').hidden`), false);
  assert.equal(await run(`$('ownSendText').disabled`), true);
  await run(`
    $('deviceList').querySelector('.device-send').click();
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
  assert.equal(await run(`$('ownTransferList').querySelectorAll('[data-transfer-action="accept"]').length`), 1);
  assert.match(await run(`$('ownTransferList').lastElementChild.textContent`), /传输已中断.*重新发送/);
  assert.equal(await run(`$('ownTransferList').lastElementChild.querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  assert.equal(await run(`$('ownTransferList').lastElementChild.querySelector('[data-transfer-action="cancel"]').disabled`), false);
  await run(`
    ownAnswer = {...ownAnswer, transfers: [{...incomingText, status: 'completed'}, incomingFile]};
    $('ownTransferList').querySelector('[data-transfer-action="accept"]').click();
  `);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferRespond')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferRespond')`),
      ['ownTransferRespond', {id: 'a'.repeat(32), action: 'accept'}]);
  assert.equal(await run(`$('ownTransferList').querySelector('.own-received-text').textContent`),
      '<img src=x onerror="globalThis.ownInjected=true">');
  assert.equal(await run(`$('ownTransferList').querySelectorAll('img,script').length`), 0);
  assert.equal(await run('Boolean(globalThis.ownInjected)'), false);
  assert.match(await run(`$('ownTransferList').textContent`), /复制文本/);

  // Uncertain acceptance keeps the same file reserved and still cancellable.
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
  assert.equal(await run(`$('ownTransferList').querySelector('[data-file-accept="true"]').disabled`), true);
  assert.equal(await run(`$('ownCancelJob').disabled`), false);

  await run(`
    ownSelectedFile = null;
    ownAnswer = {...ownAnswer,
      transfers: ownAnswer.transfers.map(item => item.id === incomingFile.id ? incomingFile : item),
      job: {active: true, transferId: incomingFile.id,
      state: 'receiving', role: 'receiver', totalBytes: 1000, transferredBytes: 420,
      percent: 42, savedPath: '', error: ''}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownJob').hidden`), false);
  assert.equal(await run(`$('ownJobProgress').value`), 42);
  assert.equal(await run(`$('ownChooseFile').disabled && !$('ownCancelJob').disabled`), true);
  assert.equal(await run(`$('ownTransferList').querySelectorAll('[data-transfer-action="accept"]').length`), 0);
  await run(`$('ownCancelJob').click()`);
  await until(`!ownTransferPending && calls.some(call => call[0] === 'ownTransferCancel')`);
  assert.deepEqual(await run(`calls.find(call => call[0] === 'ownTransferCancel')`), ['ownTransferCancel']);
  assert.equal(await run('document.documentElement.scrollWidth <= innerWidth'), true);

  // Failed result notifications leave the existing manual refresh usable.
  await run(`
    ownAnswer = {...ownAnswer, error: '文件已保存，但完成确认失败；请点击刷新重试确认',
      job: {...ownAnswer.job, active: true, state: 'completed', percent: 100}};
    renderOwnTransferState(ownAnswer);
  `);
  assert.equal(await run(`$('ownTransferRefresh').disabled`), false);
  assert.match(await run(`$('ownTransferError').textContent`), /文件已保存.*点击刷新/);
  await run(`$('ownTransferRefresh').click()`);
  await until(`!ownTransferReadInFlight && calls.some(call => call[0] === 'ownTransferRefresh')`);

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
      'device ordering, focus retention and XSS safety, refresh/logout, device-targeted text/files, ' +
      'receive confirmation and interrupted-transfer guidance, transfer progress/cancel, byte limits, ' +
      'stale responses, mobile layout.');
} finally {
  clearTimeout(timeout);
  await cdp('Browser.close').catch(() => {});
  await browserExited;
  fs.rmSync(scratch, {recursive: true, force: true});
}
