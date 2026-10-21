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
let currentState = 'idle';
let currentVersion = '';
let currentManifest = null;
let currentProgress = null;
let currentError = '';
let isInstallerPackage = false;
let installerOpened = false;
let requiresElevation = false;
let elevationApprovalRequired = false;
let systemUpdater = false;
let systemUpdateInstalled = false;

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

function formatBytes(value) {
  if (!Number.isFinite(value) || value < 0) return '';
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
}

function renderState(info) {
  currentState = info.state || currentState;
  if ('manifest' in info) currentManifest = info.manifest;
  if ('download_progress' in info) currentProgress = info.download_progress;
  if ('error' in info) currentError = info.error || '';
  if ('is_installer_package' in info) isInstallerPackage = !!info.is_installer_package;
  if ('installer_opened' in info) installerOpened = !!info.installer_opened;
  if ('requires_elevation' in info) requiresElevation = !!info.requires_elevation;
  if ('elevation_approval_required' in info) elevationApprovalRequired = !!info.elevation_approval_required;
  if ('system_updater' in info) systemUpdater = !!info.system_updater;
  if ('system_update_installed' in info) systemUpdateInstalled = !!info.system_update_installed;
  serverUrlInput.disabled = systemUpdater;
  serverUrlInput.readOnly = systemUpdater;
  serverUrlInput.title = systemUpdater ? '系统更新源由安装包配置' : '';
  btnSaveFeedUrl.disabled = systemUpdater;
  document.querySelectorAll('.btn-scenario').forEach(btn => { btn.disabled = systemUpdater; });
  const version = currentManifest?.target_version || '新版本';
  const packageType = systemUpdater ? '更新包' : (currentManifest?.is_diff ? '差分包' : '完整包');
  const busy = ['checking-for-update', 'downloading', 'patching'].includes(currentState);
  btnCheckUpdate.disabled = busy;
  btnCheckUpdate.style.display = 'inline-block';
  btnCheckUpdate.textContent = currentState === 'update-available' ? '下载更新' : '检查更新';
  btnRestartInstall.style.display = 'none';
  btnRestartInstall.disabled = false;
  btnRestartInstall.textContent = '重启并安装';
  progressWrapper.style.display = 'none';

  const percent = Number.isFinite(currentProgress?.percent)
    ? Math.max(0, Math.min(100, currentProgress.percent)) : null;
  progressFill.style.width = `${percent ?? 0}%`;
  progressPercent.textContent = percent === null ? '等待下载进度' : `${percent}%`;
  progressSpeed.textContent = currentProgress
    ? `${formatBytes(currentProgress.bytes_downloaded)} / ${formatBytes(currentProgress.total_bytes)}` : '';

  switch (currentState) {
    case 'checking-for-update':
      setStatus('checking', '正在检查更新...', '正在向更新服务器请求最新版本清单');
      break;
    case 'update-available':
      setStatus('', `发现新版本: v${version}`, systemUpdater
        ? '由系统更新服务选择并下载适用的更新包' : `${packageType}已可下载`);
      break;
    case 'downloading':
      progressWrapper.style.display = 'flex';
      setStatus('downloading', percent === 100 ? '下载完成，正在校验更新包' : '正在下载更新',
        percent === 100 ? '已收到全部数据，校验通过后开始准备更新' : `正在接收${packageType}，小更新可能瞬间完成下载`);
      break;
    case 'patching':
      progressWrapper.style.display = currentProgress ? 'flex' : 'none';
      setStatus('checking', '正在准备更新', systemUpdater
        ? '系统更新服务正在校验并准备安装，尚未确认安装完成'
        : currentManifest?.is_diff
        ? '正在应用差分并校验文件，完成后可重启安装'
        : '正在解压并校验完整更新，完成后可重启安装');
      break;
    case 'update-downloaded':
      if (systemUpdater) {
        setStatus('downloaded', systemUpdateInstalled ? '安装完成' : '等待退出浏览器以安装',
          systemUpdateInstalled
            ? `v${version} 已完成安装。退出后请手动重新打开浏览器`
            : `系统更新服务正在等待浏览器退出以安装 v${version}。安装完成后请手动重新打开浏览器`);
        btnRestartInstall.textContent = systemUpdateInstalled ? '退出浏览器' : '退出并安装';
      } else if (isInstallerPackage) {
        setStatus('downloaded', installerOpened ? '安装器已打开' : '下载安装包已就绪',
          installerOpened
            ? `请按系统安装器提示完成管理员授权并退出浏览器。v${version} 尚未确认安装完成`
            : `v${version} 安装包已下载并验证，请打开系统安装器完成安装`);
        btnRestartInstall.textContent = installerOpened ? '再次打开安装器' : '打开安装器';
      } else if (requiresElevation) {
        setStatus('downloaded', elevationApprovalRequired ? '等待管理员授权' : '差分更新已准备好',
          elevationApprovalRequired
            ? '请在系统设置中允许更新服务，完成后继续安装。已准备的差分包会保留'
            : `v${version} 差分包已验证，安装时需要管理员授权以保留系统所有权`);
        btnRestartInstall.textContent = elevationApprovalRequired ? '授权后继续安装' : '授权并安装';
      } else {
        setStatus('downloaded', '更新包已准备好', `v${version} 更新包已验证并准备好，重启后开始安装`);
      }
      btnCheckUpdate.style.display = !systemUpdater && elevationApprovalRequired && currentManifest?.full_package ? 'inline-block' : 'none';
      if (!systemUpdater && elevationApprovalRequired) btnCheckUpdate.textContent = '改用完整安装包';
      btnRestartInstall.style.display = 'inline-block';
      break;
    case 'update-not-available':
      setStatus('', '已是最新版本', `当前版本 v${currentVersion} 已是最新，无需更新`);
      break;
    case 'error':
      setStatus('error', '更新失败', currentError || '请重试检查更新');
      break;
    default:
      setStatus('', '就绪', systemUpdater ? '通过系统更新服务检查更新' : '准备好检查更新或配置更新源');
  }
  if (currentError) {
    const ready = currentState === 'update-downloaded';
    setStatus('error', ready ? (isInstallerPackage ? '打开安装器失败' : '安装未完成') : '更新失败', currentError);
    if (ready) btnRestartInstall.textContent = systemUpdater ? '退出并安装' : (isInstallerPackage ? '重试打开安装器' : '重试安装');
  }
}

// 1. 注册 Chromium 原生 WebUI 监听器
addWebUiListener('initial-state', (info) => {
  if (info.current_version) {
    currentVersion = info.current_version;
    currentVersionBadge.textContent = `当前版本: v${info.current_version}`;
  }
  if (info.feed_url) {
    serverUrlInput.value = info.feed_url;
  }
  renderState(info);
  appendLog('info', `初始化就绪: 当前版本 v${info.current_version || '未知'}, Feed: ${info.feed_url || '未设置'}`);
});

addWebUiListener('checking-for-update', () => {
  renderState({state:'checking-for-update', manifest:null, download_progress:null, error:'',
    is_installer_package:false, installer_opened:false,
    requires_elevation:false, elevation_approval_required:false,
    system_update_installed:false});
  appendLog('info', '向服务器请求检查最新更新...');
});

addWebUiListener('update-available', (manifest) => {
  const ver = manifest?.target_version || '新版本';
  renderState({state:'update-available', manifest, download_progress:null, error:'',
    is_installer_package:false, installer_opened:false,
    requires_elevation:false, elevation_approval_required:false,
    system_update_installed:false});
  appendLog('success', `发现新版本: v${ver}，包类型: ${systemUpdater ? '由系统更新服务选择' : (manifest?.is_diff ? '差分包' : '完整包')}`);
  // Native autoDownload owns automatic initiation. If disabled, the primary
  // button remains available for an explicit user download.
});

addWebUiListener('update-state-changed', renderState);

addWebUiListener('update-not-available', (version) => {
  currentVersion = version;
  renderState({state:'update-not-available', error:''});
  appendLog('info', `检查完成：当前版本已是最新 (v${version})。`);
});

addWebUiListener('download-progress', (progress) => {
  renderState({state:['patching', 'update-downloaded'].includes(currentState) ? currentState : 'downloading',
    download_progress:progress});
  appendLog('info', `下载进度: ${progress.percent}% (${formatBytes(progress.bytes_downloaded)} / ${formatBytes(progress.total_bytes)})`);
});

addWebUiListener('update-downloaded', (manifest) => {
  const ver = manifest?.target_version || '新版本';
  renderState({state:'update-downloaded', manifest, error:'',
    is_installer_package:manifest?.is_installer_package || false,
    installer_opened:manifest?.installer_opened || false,
    requires_elevation:manifest?.requires_elevation || false,
    elevation_approval_required:manifest?.elevation_approval_required || false,
    system_updater:manifest?.system_updater ?? systemUpdater,
    system_update_installed:manifest?.system_update_installed || false});
  appendLog('info', systemUpdater && systemUpdateInstalled
    ? `系统更新服务报告 v${ver} 安装完成，退出后请手动重新打开浏览器`
    : `v${ver} 更新包已准备完毕，尚未安装`);
});

addWebUiListener('update-error', (errorMsg) => {
  renderState({state:currentState === 'update-downloaded' ? currentState : 'error',
    error:errorMsg || '未知错误'});
  appendLog('error', `更新异常: ${errorMsg}`);
});

// 2. 按钮操作通过原生 chrome.send 调用 C++ Handler
btnCheckUpdate.addEventListener('click', () => {
  if (btnCheckUpdate.disabled) return;
  if (!systemUpdater && currentState === 'update-downloaded' && elevationApprovalRequired) {
    btnCheckUpdate.disabled = true;
    chrome.send('downloadFullInstaller');
    return;
  }
  if (currentState === 'update-available') {
    btnCheckUpdate.disabled = true;
    chrome.send('downloadUpdate');
    return;
  }
  const feedUrl = systemUpdater ? '' : serverUrlInput.value.trim();
  appendLog('info', `发起检查更新 (chrome.send), Feed: ${feedUrl}`);
  chrome.send('checkForUpdates', [feedUrl]);
});

btnRestartInstall.addEventListener('click', () => {
  if (btnRestartInstall.disabled || currentState !== 'update-downloaded') return;
  btnRestartInstall.disabled = true;
  btnRestartInstall.textContent = systemUpdater ? '正在退出浏览器...' : (isInstallerPackage ? '正在打开安装器...' : '正在启动安装...');
  appendLog('warn', systemUpdater ? '请求退出浏览器；安装完成后请手动重新打开'
    : (isInstallerPackage ? '请求打开系统安装器，尚未完成安装' : '调用 quitAndInstall 重启并应用更新...'));
  chrome.send('quitAndInstall');
});

btnSaveFeedUrl.addEventListener('click', () => {
  if (systemUpdater || btnSaveFeedUrl.disabled) return;
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
    if (systemUpdater || btn.disabled) return;
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
