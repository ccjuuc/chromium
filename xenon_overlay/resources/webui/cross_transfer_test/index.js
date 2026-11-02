// This page deliberately keeps all form values in memory only.
const $ = id => document.getElementById(id);
const form = $('transferForm');
const directHosts = {receive: '0.0.0.0', send: ''};
let currentRole = 'receive';
let sendWithPromise = null;
let snapshot = {active: false};
let operationPending = false;
let pollTimer = null;
let pollInFlight = false;
let snapshotVersion = 0;
let lastTaskId = '';

const stateLabels = new Map([
  ['idle', '准备开始'],
  ['starting', '正在启动'],
  ['preparing', '正在准备'],
  ['listening', '正在监听'],
  ['waiting', '等待另一端'],
  ['connecting', '正在连接'],
  ['handshaking', '正在验证连接'],
  ['negotiating', '正在协商传输'],
  ['transferring', '正在传输'],
  ['sending', '正在发送'],
  ['receiving', '正在接收'],
  ['verifying', '正在校验文件'],
  ['completed', '传输完成'],
  ['complete', '传输完成'],
  ['succeeded', '传输完成'],
  ['canceling', '正在取消'],
  ['cancelling', '正在取消'],
  ['cancelled', '已取消'],
  ['canceled', '已取消'],
  ['failed', '传输失败'],
  ['error', '传输失败'],
]);

function selected(name) {
  return form.querySelector(`input[name="${name}"]:checked`).value;
}

function showFields(id, visible) {
  const container = $(id);
  container.hidden = !visible;
  for (const input of container.querySelectorAll('input, button')) {
    input.disabled = !visible;
  }
}

function updateControls() {
  const locked = operationPending || Boolean(snapshot.active);
  $('configFields').disabled = locked;
  $('startButton').disabled = !sendWithPromise || locked;
  $('startButton').textContent = operationPending ? '请稍候…' :
      selected('role') === 'send' ? '开始发送' : '开始接收';
  $('cancelButton').disabled = !sendWithPromise || !snapshot.active ||
      operationPending;
  $('copyTaskId').disabled = !lastTaskId;
}

function updateForm() {
  const role = selected('role');
  const relay = selected('transport') === 'relay';
  const receive = role === 'receive';
  if (role !== currentRole) {
    directHosts[currentRole] = $('directHost').value;
    $('directHost').value = directHosts[role];
    currentRole = role;
    // A token belongs to one role; never silently reuse it after switching.
    $('bearerToken').value = '';
  }
  showFields('sendPathFields', !receive);
  showFields('receivePathFields', receive);
  showFields('directFields', !relay);
  showFields('relayFields', relay);
  showFields('serverTlsFields', !relay);
  showFields('directPeerFields', !relay);
  $('directHostLabel').textContent = receive ? '监听地址' : '接收端地址';
  $('directHost').placeholder = receive ? '0.0.0.0' : '例如 192.168.1.20';
  $('directHelp').textContent = receive ?
      '0.0.0.0 监听所有 IPv4 网卡。发送端应填写这台设备的实际 IP 地址。' :
      '填写接收端的实际 IP 或主机名；端口须与接收端一致。';
  $('caHelp').textContent = relay ?
      '留空使用 TLS 默认信任库。仅当服务器证书不在默认信任库中时填写 CA 文件。' :
      '用于验证对端证书。建议显式指定 CA；留空使用 TLS 默认信任库。';
  $('actionHelp').textContent = receive ?
      '准备好后，先在接收端开始。' : '请先确认另一台设备已开始接收。';
  $('guideFirst').textContent = relay ?
      '两端填写相同服务器、房间 ID 和本次连接 ID，分别使用各自角色的令牌。' :
      '确保两台设备可互相访问，两端各自准备证书、私钥及用于信任对端的 CA。';
  $('guideSecond').textContent = relay ?
      '在接收端填写目录并开始，等待中继连接就绪。' :
      '在接收端填写目录并开始，等待状态显示正在监听。';
  $('guideThird').textContent = relay ?
      '在发送端填写文件路径并开始发送。重试时换一个新的连接 ID，两端保持一致。' :
      '在发送端填写文件路径和接收端 IP，使用相同端口开始发送。';
  updateControls();
}

function redactError(error) {
  let message = error instanceof Error ? error.message : String(error || '未知错误');
  const token = $('bearerToken').value.trim();
  if (token) {
    message = message.replaceAll(token, '[令牌已隐藏]');
    message = message.replaceAll(token.toLowerCase(), '[令牌已隐藏]');
  }
  return message;
}

function showError(id, message) {
  $(id).textContent = message;
  $(id).hidden = !message;
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value < 0) {
    return '—';
  }
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let index = 0;
  while (value >= 1024 && index < units.length - 1) {
    value /= 1024;
    ++index;
  }
  return `${value.toLocaleString('zh-CN', {maximumFractionDigits: index ? 1 : 0})} ${units[index]}`;
}

function renderSnapshot(next) {
  if (!next || typeof next !== 'object' || Array.isArray(next)) {
    throw new Error('浏览器返回了无效的传输状态。');
  }
  snapshot = next;
  const state = String(next.state || 'idle').toLowerCase();
  $('statusHeading').textContent = stateLabels.get(state) ||
      (next.active ? '任务进行中' : '任务已停止');
  $('statusMessage').textContent = redactError(next.status ||
      (next.active ? '正在等待传输状态…' : '填写连接信息后开始传输。'));
  $('activityDot').classList.toggle('active', Boolean(next.active));
  $('activityDot').classList.toggle('failed', Boolean(next.error));
  const transferred = Math.max(0, Number(next.transferredBytes) || 0);
  const total = Math.max(0, Number(next.totalBytes) || 0);
  const reportedPercent = typeof next.percent === 'number' &&
      Number.isFinite(next.percent) ? next.percent : null;
  const percent = reportedPercent ?? (total > 0 ? transferred / total * 100 : null);
  const boundedPercent = percent === null ? 0 : Math.min(100, Math.max(0, percent));
  $('transferProgress').value = boundedPercent;
  $('progressPercent').textContent = percent === null ? '—' :
      `${boundedPercent.toLocaleString('zh-CN', {maximumFractionDigits: 1})}%`;
  $('transferredBytes').textContent = formatBytes(transferred);
  $('totalBytes').textContent = total > 0 ? `共 ${formatBytes(total)}` : '总大小未知';
  $('listeningPort').textContent = next.listeningPort ? String(next.listeningPort) : '—';
  $('savedPath').textContent = next.savedPath || '—';
  const taskId = typeof next.taskId === 'string' ? next.taskId : '';
  if (taskId && taskId !== lastTaskId) {
    lastTaskId = taskId;
    $('taskId').value = taskId;
    $('copyTaskId').textContent = '复制';
  }
  $('resultTaskId').textContent = taskId || '—';
  if (!taskId) {
    lastTaskId = '';
  }
  showError('transferError', next.error ? redactError(next.error) : '');
  updateControls();
}

function schedulePoll() {
  clearTimeout(pollTimer);
  if (sendWithPromise) {
    pollTimer = setTimeout(pollStatus, 500);
  }
}

async function pollStatus() {
  if (!sendWithPromise || pollInFlight || operationPending) {
    schedulePoll();
    return;
  }
  pollInFlight = true;
  const version = snapshotVersion;
  try {
    const next = await sendWithPromise('crossTransferGetStatus');
    // A status request made before start/cancel must not overwrite its result.
    if (version === snapshotVersion) {
      renderSnapshot(next);
      $('bridgeBadge').textContent = '浏览器已连接';
      $('bridgeBadge').classList.add('connected');
      $('bridgeNotice').hidden = true;
    }
  } catch (error) {
    if (version === snapshotVersion) {
      $('bridgeBadge').textContent = '状态暂不可用';
      $('bridgeBadge').classList.remove('connected');
      showError('bridgeNotice', `无法获取传输状态：${redactError(error)}。正在重试。`);
    }
  } finally {
    pollInFlight = false;
    schedulePoll();
  }
}

function readOptions() {
  const role = selected('role');
  const transport = selected('transport');
  const relay = transport === 'relay';
  const value = id => $(id).value.trim();
  const options = {
    role,
    transport,
    host: value(relay ? 'relayHost' : 'directHost'),
    port: Number(value(relay ? 'relayPort' : 'directPort')),
    taskId: value('taskId'),
    allowOverwrite: role === 'receive' && $('allowOverwrite').checked,
  };
  const pathIds = [role === 'send' ? 'sourcePath' : 'outputDir'];
  if (!relay) {
    pathIds.push('certificateFile', 'privateKeyFile');
  }
  if (value('caFile')) {
    pathIds.push('caFile');
  }
  if (!relay) {
    options.peerName = value('peerName');
  }
  for (const id of pathIds) {
    const path = value(id);
    if (!/^(?:\/|[a-zA-Z]:[\\/]|\\\\)/.test(path)) {
      $(id).focus();
      throw new Error('文件与证书路径必须是本机绝对路径，不支持 ~ 或相对路径。');
    }
    options[id] = path;
  }
  if (!options.host || options.host.includes('://') || /[\s/]/.test(options.host)) {
    $(relay ? 'relayHost' : 'directHost').focus();
    throw new Error('服务器地址请只填写 IP 或主机名，不包含协议、端口或路径。');
  }
  if (!Number.isInteger(options.port) || options.port < 1 || options.port > 65535) {
    throw new Error('端口必须是 1 到 65535 之间的整数。');
  }
  if (relay) {
    options.roomId = value('roomId');
    options.connectionId = value('connectionId').toLowerCase();
    options.bearerToken = value('bearerToken');
    if (!options.roomId) {
      $('roomId').focus();
      throw new Error('请填写房间 ID。');
    }
  }
  return options;
}

form.addEventListener('change', event => {
  if (event.target.name === 'role' || event.target.name === 'transport') {
    showError('formError', '');
    updateForm();
  }
});

form.addEventListener('submit', async event => {
  event.preventDefault();
  if (!sendWithPromise || operationPending || snapshot.active) {
    return;
  }
  showError('formError', '');
  if (!form.reportValidity()) {
    return;
  }
  let options;
  try {
    options = readOptions();
  } catch (error) {
    showError('formError', redactError(error));
    return;
  }
  operationPending = true;
  ++snapshotVersion;
  updateControls();
  try {
    renderSnapshot(await sendWithPromise('crossTransferStart', options));
  } catch (error) {
    showError('formError', `无法开始传输：${redactError(error)}`);
  } finally {
    operationPending = false;
    updateControls();
    schedulePoll();
  }
});

$('cancelButton').addEventListener('click', async () => {
  if (!sendWithPromise || operationPending || !snapshot.active) {
    return;
  }
  showError('formError', '');
  operationPending = true;
  ++snapshotVersion;
  updateControls();
  try {
    renderSnapshot(await sendWithPromise('crossTransferCancel'));
  } catch (error) {
    showError('formError', `无法取消传输：${redactError(error)}`);
  } finally {
    operationPending = false;
    updateControls();
    schedulePoll();
  }
});

$('generateConnectionId').addEventListener('click', () => {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  $('connectionId').value = Array.from(bytes, byte =>
    byte.toString(16).padStart(2, '0')).join('');
  $('connectionId').focus();
  $('connectionId').select();
});

$('copyTaskId').addEventListener('click', async () => {
  if (!lastTaskId) {
    return;
  }
  try {
    await navigator.clipboard.writeText(lastTaskId);
    $('copyTaskId').textContent = '已复制';
  } catch {
    const selection = window.getSelection();
    const range = document.createRange();
    range.selectNodeContents($('resultTaskId'));
    selection.removeAllRanges();
    selection.addRange(range);
    $('copyTaskId').textContent = '请手动复制';
  }
});

async function initialize() {
  updateForm();
  if (location.protocol !== 'chrome:' ||
      location.hostname !== 'cross-transfer-test' ||
      typeof globalThis.chrome?.send !== 'function') {
    $('bridgeBadge').textContent = '静态预览';
    showError('bridgeNotice', '当前仅能预览界面。请在包含 Cross Transfer 的 Chromium 中打开 chrome://cross-transfer-test/，使用真实文件传输。');
    return;
  }
  try {
    const bridge = await import('chrome://resources/js/cr.js');
    sendWithPromise = bridge.sendWithPromise;
    if (typeof sendWithPromise !== 'function') {
      throw new Error('浏览器通信接口不可用');
    }
    renderSnapshot(await sendWithPromise('crossTransferGetStatus'));
    $('bridgeBadge').textContent = '浏览器已连接';
    $('bridgeBadge').classList.add('connected');
    updateControls();
    schedulePoll();
  } catch (error) {
    sendWithPromise = null;
    updateControls();
    $('bridgeBadge').textContent = '连接失败';
    showError('bridgeNotice', `无法连接传输服务：${redactError(error)}。请确认此浏览器包含 Cross Transfer，并刷新页面。`);
  }
}

window.addEventListener('pagehide', () => clearTimeout(pollTimer));
void initialize();
