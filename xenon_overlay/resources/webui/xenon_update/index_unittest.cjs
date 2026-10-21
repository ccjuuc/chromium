// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createPage() {
  const elements = new Map();
  function element() {
    return {style:{}, textContent:'', value:'', disabled:false, children:[], handlers:{},
      addEventListener(name, handler) { this.handlers[name] = handler; },
      appendChild(child) { this.children.push(child); }};
  }
  const listeners = new Map();
  const sent = [];
  const scenarios = ['diff', 'full', 'latest', 'corrupt'].map(mode => ({
    ...element(), getAttribute:() => mode,
  }));
  const context = vm.createContext({
    document: {
      getElementById(id) {
        if (!elements.has(id)) elements.set(id, element());
        return elements.get(id);
      },
      createElement:element,
      querySelectorAll:() => scenarios,
    },
    chrome:{send:(...args) => sent.push(args)},
    addWebUiListener:(name, listener) => listeners.set(name, listener),
  });
  const source = fs.readFileSync(path.join(__dirname, 'index.js'), 'utf8')
    .replace(/^import .*;\r?\n/, '');
  vm.runInContext(source, context, {filename:'xenon_update/index.js'});
  return {element:id => elements.get(id), sent, scenarios,
    emit:(name, value) => listeners.get(name)(value),
    click:id => elements.get(id).handlers.click()};
}

const diff = {target_version:'1.0.0.2', is_diff:true};
const progress = (bytes, total = 279843) => ({
  bytes_downloaded:bytes, total_bytes:total, percent:Math.floor(bytes / total * 100),
});

test('initial snapshot restores active download and ready state without another request', () => {
  const page = createPage();
  page.emit('initial-state', {current_version:'1.0.0.1', state:'downloading',
    manifest:diff, download_progress:progress(131072), error:''});
  assert.equal(page.element('progressPercent').textContent, '46%');
  assert.equal(page.element('progressSpeed').textContent, '128.0 KiB / 273.3 KiB');
  assert.equal(page.element('btnCheckUpdate').disabled, true);
  assert.equal(page.element('btnRestartInstall').style.display, 'none');
  page.emit('initial-state', {state:'update-downloaded', manifest:diff, error:''});
  assert.equal(page.element('btnRestartInstall').style.display, 'inline-block');
  assert.equal(page.element('btnCheckUpdate').style.display, 'none');
  assert.match(page.element('statusDesc').textContent, /1\.0\.0\.2/);
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState']);
});

test('small real download reaches 100 then preparation, without inventing installation readiness', () => {
  const page = createPage();
  page.emit('update-available', diff);
  for (const bytes of [65536, 131072, 196608, 262144, 279843]) {
    page.emit('download-progress', progress(bytes));
  }
  assert.equal(page.element('progressPercent').textContent, '100%');
  assert.equal(page.element('progressFill').style.width, '100%');
  assert.match(page.element('statusTitle').textContent, /校验/);
  assert.equal(page.element('btnRestartInstall').style.display, 'none');
  page.emit('update-state-changed', {state:'patching', manifest:diff,
    download_progress:progress(279843), error:''});
  assert.equal(page.element('statusTitle').textContent, '正在准备更新');
  assert.match(page.element('statusDesc').textContent, /应用差分/);
  assert.equal(page.element('progressPercent').textContent, '100%');
  assert.equal(page.element('btnCheckUpdate').disabled, true);
  page.emit('update-downloaded', diff);
  assert.equal(page.element('btnRestartInstall').style.display, 'inline-block');
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState']);
});

test('autoDownload disabled leaves one explicit download action and suppresses double clicks', () => {
  const page = createPage();
  page.emit('update-available', diff);
  assert.equal(page.element('btnCheckUpdate').textContent, '下载更新');
  assert.equal(page.element('btnCheckUpdate').disabled, false);
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState']);
  page.click('btnCheckUpdate');
  page.click('btnCheckUpdate');
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState', 'downloadUpdate']);
});

test('fallback resets progress to the real full package and labels its preparation correctly', () => {
  const page = createPage();
  page.emit('initial-state', {state:'patching', manifest:diff,
    download_progress:progress(279843), error:''});
  const full = {...diff, is_diff:false};
  page.emit('update-state-changed', {state:'downloading', manifest:full,
    download_progress:progress(0, 1048576), error:''});
  assert.equal(page.element('progressPercent').textContent, '0%');
  assert.equal(page.element('progressSpeed').textContent, '0 B / 1.0 MiB');
  assert.match(page.element('statusDesc').textContent, /完整包/);
  page.emit('update-state-changed', {state:'patching', manifest:full,
    download_progress:progress(1048576, 1048576), error:''});
  assert.match(page.element('statusDesc').textContent, /解压/);
});

test('restored and live installation errors retain the ready update retry action', () => {
  const page = createPage();
  const state = {state:'update-downloaded', manifest:diff, error:'无法启动安装助手'};
  page.emit('initial-state', state);
  assert.equal(page.element('statusTitle').textContent, '安装未完成');
  assert.equal(page.element('statusDesc').textContent, state.error);
  assert.equal(page.element('btnRestartInstall').textContent, '重试安装');
  assert.equal(page.element('btnRestartInstall').disabled, false);
  page.click('btnRestartInstall');
  page.click('btnRestartInstall');
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState', 'quitAndInstall']);
  page.emit('update-error', state.error);
  page.emit('update-state-changed', state);
  assert.equal(page.element('btnRestartInstall').style.display, 'inline-block');
  assert.equal(page.element('btnRestartInstall').disabled, false);
  assert.equal(page.element('btnRestartInstall').textContent, '重试安装');
});

test('download errors never expose installation and restored errors remain visible', () => {
  const page = createPage();
  page.emit('initial-state', {state:'downloading', manifest:diff, download_progress:null, error:''});
  assert.equal(page.element('progressPercent').textContent, '等待下载进度');
  page.emit('update-error', '更新包校验失败');
  page.emit('update-state-changed', {state:'error', manifest:diff, error:'更新包校验失败'});
  assert.equal(page.element('btnRestartInstall').style.display, 'none');
  assert.equal(page.element('btnCheckUpdate').disabled, false);
  assert.equal(page.element('statusDesc').textContent, '更新包校验失败');
});

test('PKG ready event opens Installer without claiming a restart or installed version', () => {
  const page = createPage();
  const pkg = {...diff, is_diff:false, is_installer_package:true};
  page.emit('update-downloaded', pkg);
  assert.equal(page.element('statusTitle').textContent, '下载安装包已就绪');
  assert.equal(page.element('btnRestartInstall').textContent, '打开安装器');
  assert.doesNotMatch(page.element('statusDesc').textContent, /重启|已安装|升级成功/);
  page.click('btnRestartInstall');
  page.click('btnRestartInstall');
  assert.equal(page.element('btnRestartInstall').textContent, '正在打开安装器...');
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState', 'quitAndInstall']);
  page.emit('update-state-changed', {state:'update-downloaded', manifest:pkg,
    is_installer_package:true, installer_opened:true, error:''});
  assert.equal(page.element('statusTitle').textContent, '安装器已打开');
  assert.match(page.element('statusDesc').textContent, /管理员授权/);
  assert.match(page.element('statusDesc').textContent, /尚未确认安装完成/);
  assert.equal(page.element('btnRestartInstall').textContent, '再次打开安装器');
  assert.equal(page.element('btnRestartInstall').disabled, false);
});

test('PKG snapshot restores opened Installer and retains retry after an open failure', () => {
  const page = createPage();
  const state = {state:'update-downloaded', manifest:{...diff, is_diff:false},
    is_installer_package:true, installer_opened:true, error:''};
  page.emit('initial-state', state);
  assert.equal(page.element('statusTitle').textContent, '安装器已打开');
  assert.equal(page.element('btnRestartInstall').textContent, '再次打开安装器');
  page.emit('update-error', '系统安装器未能打开');
  page.emit('update-state-changed', {...state, installer_opened:false, error:'系统安装器未能打开'});
  assert.equal(page.element('statusTitle').textContent, '打开安装器失败');
  assert.equal(page.element('btnRestartInstall').textContent, '重试打开安装器');
  assert.equal(page.element('btnRestartInstall').disabled, false);
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState']);
});

test('privileged diff waits for approval and offers explicit PKG alternative', () => {
  const page = createPage();
  const manifest = {...diff, full_package:{url:'https://example.test/full.pkg'}};
  page.emit('update-downloaded', {...manifest, requires_elevation:true});
  assert.equal(page.element('btnRestartInstall').textContent, '授权并安装');
  page.click('btnRestartInstall');
  page.emit('update-state-changed', {state:'update-downloaded', manifest,
    requires_elevation:true, elevation_approval_required:true, error:''});
  assert.equal(page.element('statusTitle').textContent, '等待管理员授权');
  assert.equal(page.element('btnRestartInstall').disabled, false);
  assert.equal(page.element('btnRestartInstall').textContent, '授权后继续安装');
  assert.equal(page.element('btnCheckUpdate').textContent, '改用完整安装包');
  assert.equal(page.element('btnCheckUpdate').style.display, 'inline-block');
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState', 'quitAndInstall']);
  page.click('btnCheckUpdate');
  page.click('btnCheckUpdate');
  assert.deepEqual(page.sent.map(args => args[0]),
    ['getInitialState', 'quitAndInstall', 'downloadFullInstaller']);
  page.emit('update-state-changed', {state:'downloading',
    manifest:{...manifest, is_diff:false}, requires_elevation:false,
    elevation_approval_required:false, download_progress:progress(0)});
  assert.equal(page.element('btnRestartInstall').style.display, 'none');
  assert.equal(page.element('btnCheckUpdate').disabled, true);
});

test('system updater snapshot locks custom feed and mock scenarios including direct click handlers', () => {
  const page = createPage();
  page.emit('initial-state', {state:'idle', system_updater:true,
    system_update_installed:false, feed_url:'https://updates.example.test/service/update2/json'});
  assert.equal(page.element('serverUrlInput').disabled, true);
  assert.equal(page.element('serverUrlInput').readOnly, true);
  assert.equal(page.element('btnSaveFeedUrl').disabled, true);
  page.click('btnSaveFeedUrl');
  for (const scenario of page.scenarios) {
    assert.equal(scenario.disabled, true);
    scenario.handlers.click();
  }
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState']);
  page.element('serverUrlInput').value = 'http://untrusted.test/feed';
  page.click('btnCheckUpdate');
  assert.equal(page.sent.at(-1)[0], 'checkForUpdates');
  assert.equal(page.sent.at(-1)[1][0], '');
});

test('system updater leaves payload choice to the service and keeps real download progress', () => {
  const page = createPage();
  page.emit('initial-state', {state:'idle', system_updater:true});
  page.emit('update-available', diff);
  assert.match(page.element('statusDesc').textContent, /系统更新服务选择/);
  assert.doesNotMatch(page.element('statusDesc').textContent, /差分|完整包/);
  assert.doesNotMatch(page.element('logConsole').children.at(-1).textContent, /差分包|完整包/);
  page.click('btnCheckUpdate');
  assert.equal(page.sent.at(-1)[0], 'downloadUpdate');
  page.emit('download-progress', progress(131072));
  assert.equal(page.element('progressPercent').textContent, '46%');
  assert.doesNotMatch(page.element('statusDesc').textContent, /差分|完整包/);
  page.emit('update-state-changed', {state:'patching', system_updater:true, manifest:diff});
  assert.match(page.element('statusDesc').textContent, /尚未确认安装完成/);
  assert.doesNotMatch(page.element('statusDesc').textContent, /差分|完整|重启/);
});

test('system installer waiting for browser exit is not reported installed or auto-relaunching', () => {
  const page = createPage();
  page.emit('update-downloaded', {...diff, system_updater:true, system_update_installed:false});
  assert.equal(page.element('statusTitle').textContent, '等待退出浏览器以安装');
  assert.equal(page.element('btnRestartInstall').textContent, '退出并安装');
  assert.match(page.element('statusDesc').textContent, /手动重新打开/);
  assert.doesNotMatch(page.element('statusTitle').textContent, /完成|成功/);
  assert.doesNotMatch(page.element('statusDesc').textContent, /自动重启|已安装|升级成功/);
  page.click('btnRestartInstall');
  page.click('btnRestartInstall');
  assert.equal(page.element('btnRestartInstall').textContent, '正在退出浏览器...');
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState', 'quitAndInstall']);
});

test('only explicit system installed result renders installation complete, restored or live', () => {
  for (const restored of [false, true]) {
    const page = createPage();
    if (restored) page.emit('initial-state', {state:'update-downloaded', manifest:diff,
      system_updater:true, system_update_installed:true});
    else page.emit('update-downloaded', {...diff, system_updater:true, system_update_installed:true});
    assert.equal(page.element('statusTitle').textContent, '安装完成');
    assert.equal(page.element('btnRestartInstall').textContent, '退出浏览器');
    assert.match(page.element('statusDesc').textContent, /手动重新打开/);
    assert.doesNotMatch(page.element('statusDesc').textContent, /启动成功|自动重启|已通过.*验证/);
    page.emit('checking-for-update');
    page.emit('update-downloaded', {...diff, system_updater:true, system_update_installed:false});
    assert.equal(page.element('statusTitle').textContent, '等待退出浏览器以安装');
  }
});

test('legacy snapshots keep feed settings and scenarios available', () => {
  const page = createPage();
  page.emit('initial-state', {state:'idle', system_updater:false, feed_url:'http://127.0.0.1:8999/check'});
  assert.equal(page.element('serverUrlInput').disabled, false);
  assert.equal(page.element('btnSaveFeedUrl').disabled, false);
  page.click('btnSaveFeedUrl');
  page.scenarios[0].handlers.click();
  assert.deepEqual(page.sent.map(args => args[0]), ['getInitialState', 'setFeedURL', 'switchScenario']);
});
