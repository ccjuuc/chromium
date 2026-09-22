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

// 1. 注册 Chromium 原生 WebUI 监听器
addWebUiListener('initial-state', (info) => {
  if (info.current_version) {
    currentVersionBadge.textContent = `当前版本: v${info.current_version}`;
  }
  if (info.feed_url) {
    serverUrlInput.value = info.feed_url;
  }
  appendLog('info', `初始化就绪: 当前版本 v${info.current_version || '未知'}, Feed: ${info.feed_url || '未设置'}`);
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
  const percent = Math.round(progress.percent || 0);
  progressFill.style.width = `${percent}%`;
  progressPercent.textContent = `${percent}%`;
  if (percent >= 100) {
    statusDesc.textContent = '下载完成 (100%)，正在进行差分合成与方案B目录准备...';
    appendLog('info', '下载完成 (100%)，后台正在组织版本隔离目录...');
  } else {
    statusDesc.textContent = `正在下载中... ${percent}%`;
    appendLog('info', `下载进度: ${percent}%`);
  }
});

addWebUiListener('update-downloaded', (manifest) => {
  const ver = manifest?.target_version || '新版本';
  setStatus('downloaded', '新版本已就绪！', `版本 v${ver} 方案B目录构建完成，可立即重启切换`);
  appendLog('success', `【方案B更新就绪】v${ver} 目录与资源准备完毕，点击重启即可秒级切换！`);
  progressWrapper.style.display = 'none';
  btnCheckUpdate.style.display = 'none';
  btnRestartInstall.style.display = 'inline-block';
});

addWebUiListener('update-error', (errorMsg) => {
  setStatus('error', '更新失败', errorMsg || '未知错误');
  appendLog('error', `更新异常: ${errorMsg}`);
  btnCheckUpdate.disabled = false;
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

