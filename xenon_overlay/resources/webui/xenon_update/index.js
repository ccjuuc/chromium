import {addWebUiListener} from 'chrome://resources/js/cr.js';

// UI 元素
const currentVersionBadge = document.getElementById('currentVersionBadge');
const statusDot = document.getElementById('statusDot');
const statusTitle = document.getElementById('statusTitle');
const statusDesc = document.getElementById('statusDesc');
const progressWrapper = document.getElementById('progressWrapper');
const progressFill = document.getElementById('progressFill');
const progressPercent = document.getElementById('progressPercent');
const progressSpeed = document.getElementById('progressSpeed');
const btnCheckUpdate = document.getElementById('btnCheckUpdate');
const btnRestartInstall = document.getElementById('btnRestartInstall');
const serverUrlInput = document.getElementById('serverUrlInput');
const btnSaveFeedUrl = document.getElementById('btnSaveFeedUrl');
const logConsole = document.getElementById('logConsole');
const btnClearLog = document.getElementById('btnClearLog');
let installBeforeRestart = false;

function appendLog(type, text) {
  const time = new Date().toLocaleTimeString();
  const entry = document.createElement('div');
  entry.className = `log-entry ${type}`;
  entry.textContent = `[${time}] ${text}`;
  logConsole.appendChild(entry);
  logConsole.scrollTop = logConsole.scrollHeight;
}

function setStatus(state, title, desc) {
  statusDot.className = `status-dot ${state}`;
  statusTitle.textContent = title;
  statusDesc.textContent = desc;
}

function showReady(manifest) {
  const ver = manifest?.target_version || '新版本';
  setStatus('downloaded', '新版本已就绪', installBeforeRestart
      ? `版本 v${ver} 已安装，重启后生效`
      : `版本 v${ver} 已准备完成，重启后生效`);
  progressWrapper.style.display = 'none';
  btnCheckUpdate.style.display = 'none';
  btnRestartInstall.style.display = 'inline-block';
}

function showInstalling() {
  setStatus('downloading', '正在安装更新...', '安装完成后即可重启使用新版本');
  progressWrapper.style.display = 'none';
  btnCheckUpdate.disabled = true;
}

// 1. 注册 Chromium 原生 WebUI 监听器
addWebUiListener('initial-state', (info) => {
  installBeforeRestart = !!info.install_before_restart;
  if (info.current_version) {
    currentVersionBadge.textContent = `当前版本: v${info.current_version}`;
  }
  if (info.feed_url) {
    serverUrlInput.value = info.feed_url;
  }
  appendLog('info', `初始化就绪: 当前版本 v${info.current_version || '未知'}, Feed: ${info.feed_url || '未设置'}`);
  if (info.state === 'update-downloaded') {
    showReady(info.manifest);
  } else if (info.state === 'installing') {
    showInstalling();
  } else if (['checking-for-update', 'downloading', 'patching'].includes(info.state)) {
    btnCheckUpdate.disabled = true;
    setStatus('downloading', '正在处理更新...', '请等待更新完成');
  }
});

addWebUiListener('checking-for-update', () => {
  setStatus('checking', '正在检查更新...', '正在向更新服务器请求最新版本清单');
  appendLog('info', '向服务器请求检查最新更新...');
  btnCheckUpdate.disabled = true;
});

addWebUiListener('update-available', (manifest) => {
  const ver = manifest?.target_version || '新版本';
  const isDiff = manifest?.is_diff;
  const typeText = isDiff ? '差分增量补丁 (.zucc)' : '全量安装包';
  setStatus('downloading', `发现新版本: v${ver}`, `类型: ${typeText}，正在后台静默下载并处理...`);
  appendLog('success', `发现新版本: v${ver}，包类型: ${typeText}，立即开始下载...`);
  progressWrapper.style.display = 'flex';
  progressFill.style.width = '0%';
  progressPercent.textContent = '0%';
  // 自动触发下载
  chrome.send('downloadUpdate');
});

addWebUiListener('update-not-available', (version) => {
  setStatus('', '已是最新版本', `当前版本 v${version} 已是最新，无需更新`);
  appendLog('info', `检查完成：当前版本已是最新 (v${version})。`);
  btnCheckUpdate.disabled = false;
  progressWrapper.style.display = 'none';
});

addWebUiListener('download-progress', (progress) => {
  progressWrapper.style.display = 'flex';
  setStatus('downloading', '正在下载更新...', '正在下载更新包');
  const percent = Math.round(progress.percent || 0);
  progressFill.style.width = `${percent}%`;
  progressPercent.textContent = `${percent}%`;
  if (percent >= 100) {
    statusDesc.textContent = '下载完成，正在校验并准备更新...';
    appendLog('info', '下载完成，正在校验并准备更新');
  } else {
    statusDesc.textContent = `正在下载中... ${percent}%`;
    appendLog('info', `下载进度: ${percent}%`);
  }
});

addWebUiListener('update-downloaded', (manifest) => {
  showReady(manifest);
  appendLog('success', statusDesc.textContent);
});

addWebUiListener('installing-update', () => {
  showInstalling();
  appendLog('info', '正在安装更新');
});

addWebUiListener('update-error', (errorMsg) => {
  setStatus('error', '更新失败', errorMsg || '未知错误');
  appendLog('error', `更新异常: ${errorMsg}`);
  btnCheckUpdate.disabled = false;
  btnCheckUpdate.style.display = 'inline-block';
  btnRestartInstall.style.display = 'none';
  progressWrapper.style.display = 'none';
});

// 2. 按钮操作通过原生 chrome.send 调用 C++ Handler
btnCheckUpdate.addEventListener('click', () => {
  const feedUrl = serverUrlInput.value.trim();
  appendLog('info', `发起检查更新 (chrome.send), Feed: ${feedUrl}`);
  chrome.send('checkForUpdates', [feedUrl]);
});

btnRestartInstall.addEventListener('click', () => {
  appendLog('warn', '调用 quitAndInstall 重启并应用更新...');
  chrome.send('quitAndInstall');
});

btnSaveFeedUrl.addEventListener('click', () => {
  const feedUrl = serverUrlInput.value.trim();
  chrome.send('setFeedURL', [feedUrl]);
  appendLog('success', `更新源地址已保存: ${feedUrl}`);
});

btnClearLog.addEventListener('click', () => {
  logConsole.innerHTML = '';
});

// 3. 场景快捷切换
document.querySelectorAll('.btn-scenario').forEach(btn => {
  btn.addEventListener('click', () => {
    const mode = btn.getAttribute('data-mode');
    const feedUrl = serverUrlInput.value.trim();
    appendLog('info', `请求服务端切换场景: [${mode}]...`);
    chrome.send('switchScenario', [mode, feedUrl]);
  });
});

addWebUiListener('scenario-switched', (mode) => {
  appendLog('success', `服务端场景已切换为: [${mode}]，正在自动检查更新...`);
});

// 页面加载完成后立即拉取初始状态
chrome.send('getInitialState');

