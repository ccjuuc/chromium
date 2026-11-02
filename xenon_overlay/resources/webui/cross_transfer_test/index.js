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
let pageClosed = false;
let accountState = {authenticated: false, restoring: true};
let accountRegisterMode = false;
let accountPending = false;
let accountReadInFlight = false;
let accountVersion = 0;
let accountPollTimer = null;
let accountDeviceListKey = '';
let ownTransferState = {transfers: [], job: {active: false}, busy: false, error: ''};
let ownRecipientId = '';
let ownSelectedFile = null;
let ownTransferPending = false;
let ownTransferReadInFlight = false;
let ownTransferVersion = 0;
let ownTransferPollTimer = null;
let ownTransferListKey = '';

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
  if (sendWithPromise && !pageClosed) {
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

function updateAccountControls() {
  const locked = !sendWithPromise || accountPending || accountState.restoring ||
      accountState.busy || accountState.authenticated;
  $('accountFields').disabled = locked;
  $('accountSubmit').disabled = locked;
  $('accountLoginMode').disabled = locked;
  $('accountRegisterMode').disabled = locked;
  $('accountLoginMode').setAttribute('aria-pressed', String(!accountRegisterMode));
  $('accountRegisterMode').setAttribute('aria-pressed', String(accountRegisterMode));
  $('accountConfirmField').hidden = !accountRegisterMode;
  $('accountConfirm').disabled = !accountRegisterMode;
  $('accountPassword').autocomplete = accountRegisterMode ? 'new-password' : 'current-password';
  $('accountSubmit').textContent = accountPending ? '请稍候…' :
      accountRegisterMode ? '注册并登录' : '登录';
  $('accountRefresh').disabled = !sendWithPromise || accountPending ||
      accountReadInFlight || accountState.busy || !accountState.authenticated;
  $('accountLogout').disabled = !sendWithPromise || accountPending ||
      accountState.restoring || !accountState.authenticated;
  $('accountForm').setAttribute('aria-busy',
      String(Boolean(accountPending || accountState.restoring || accountState.busy)));
  updateOwnTransferControls();
}

function deviceLastSeen(value) {
  // The service uses Unix seconds. Accept ISO timestamps for future clients.
  const date = typeof value === 'number' ?
      new Date(value < 1e12 ? value * 1000 : value) : new Date(value);
  return value && Number.isFinite(date.getTime()) ?
      `上次在线 ${date.toLocaleString('zh-CN')}` : '尚无在线记录';
}

function renderAccountDevices(devices, currentDeviceId) {
  // Online heartbeats do not change the displayed rows. Keep their existing
  // buttons so polling cannot interrupt keyboard navigation.
  const rows = devices.map(device => ({
    id: device.id,
    name: device.displayName || '未命名设备',
    meta: [device.platform || '未知平台',
      device.online ? '' : deviceLastSeen(device.lastSeenAt)].filter(Boolean).join(' · '),
    online: Boolean(device.online),
  }));
  const listKey = JSON.stringify([currentDeviceId, rows]);
  if (listKey === accountDeviceListKey) return;
  accountDeviceListKey = listKey;
  const list = $('deviceList');
  const focusedDeviceId = list.contains(document.activeElement) ?
      document.activeElement.dataset.deviceId : null;
  list.replaceChildren();
  for (const device of rows) {
    const item = document.createElement('li');
    item.className = 'device';
    const icon = document.createElement('span');
    icon.className = 'device-icon';
    icon.setAttribute('aria-hidden', 'true');
    icon.textContent = '▣';
    const info = document.createElement('div');
    info.className = 'device-info';
    const name = document.createElement('strong');
    name.className = 'device-name';
    name.textContent = `${device.name}${device.id === currentDeviceId ? '（本机）' : ''}`;
    const meta = document.createElement('p');
    meta.className = 'device-meta';
    meta.textContent = device.meta;
    const status = document.createElement('span');
    status.className = device.online ? 'device-status online' : 'device-status';
    status.textContent = device.online ? '在线' : '离线';
    info.append(name, meta);
    item.append(icon, info, status);
    if (device.online && device.id !== currentDeviceId) {
      const send = document.createElement('button');
      send.className = 'button subtle device-send';
      send.type = 'button';
      send.dataset.deviceId = device.id;
      send.setAttribute('aria-label', `发送到 ${device.name}`);
      send.addEventListener('click', () => {
        ownRecipientId = device.id;
        updateOwnTransferControls();
        $('ownText').focus();
      });
      item.append(send);
    }
    list.append(item);
  }
  if (focusedDeviceId) {
    Array.from(list.querySelectorAll('.device-send'))
        .find(button => button.dataset.deviceId === focusedDeviceId)?.focus();
  }
}

function renderAccountState(next) {
  if (!next || typeof next.authenticated !== 'boolean') {
    throw new Error('浏览器返回了无效的账号状态。');
  }
  const accountChanged = accountState.authenticated !== next.authenticated ||
      accountState.accountId !== next.accountId || accountState.deviceId !== next.deviceId;
  accountState = next;
  if (accountChanged) resetOwnTransfers();
  $('accountHeading').textContent = next.authenticated ?
      `已登录：${String(next.username || '')}` : '登录你的账号';
  $('accountSignedOut').hidden = next.authenticated;
  $('accountActions').hidden = !next.authenticated;
  $('accountDevices').hidden = !next.authenticated;
  $('accountStatus').textContent = next.restoring ? '正在恢复登录状态…' :
      next.busy ? '正在更新账号状态…' :
      next.authenticated ? '此浏览器会自动保持登录。' : '';
  showError('accountError', typeof next.error === 'string' ? next.error : '');

  const devices = next.authenticated && Array.isArray(next.devices) ?
      next.devices.filter(device => device && typeof device.id === 'string') : [];
  devices.sort((a, b) => Number(b.id === next.deviceId) - Number(a.id === next.deviceId) ||
      Number(Boolean(b.online)) - Number(Boolean(a.online)));
  renderAccountDevices(devices, next.deviceId);
  $('deviceCount').textContent = `${devices.length} 台设备`;
  $('devicesEmpty').hidden = devices.length > 0;
  updateAccountControls();
  updateOwnTransferControls();
  scheduleOwnTransferPoll();
}

const invitationLabels = new Map([
  ['pending', '等待接收'], ['accepted', '正在传输'], ['completed', '已完成'],
  ['rejected', '已拒绝'], ['cancelled', '已取消'], ['failed', '传输失败'],
  ['expired', '已过期'],
]);

function resetOwnTransfers() {
  ++ownTransferVersion;
  clearTimeout(ownTransferPollTimer);
  ownTransferState = {transfers: [], job: {active: false}, busy: false, error: ''};
  ownTransferListKey = '';
  ownRecipientId = '';
  ownSelectedFile = null;
  $('ownText').value = '';
  $('ownTransferList').replaceChildren();
  $('ownTransferCount').textContent = '';
  $('ownTransfersEmpty').hidden = false;
  $('ownJob').hidden = true;
  showError('ownTransferError', '');
}

function ownRecipientDevice() {
  return accountState.devices?.find(device => device.id === ownRecipientId &&
      device.id !== accountState.deviceId && device.online);
}

function updateOwnTransferControls() {
  const authenticated = accountState.authenticated === true;
  $('ownTransferPanel').hidden = !authenticated;
  const recipient = authenticated && ownRecipientDevice();
  const locked = !sendWithPromise || !authenticated || accountPending ||
      ownTransferPending || ownTransferState.busy;
  const fileActive = Boolean(ownTransferState.job?.active);
  $('ownRecipient').textContent = recipient ?
      `发送到：${recipient.displayName || '未命名设备'}` : '请在上方选择一台在线设备。';
  $('ownText').disabled = locked;
  const textBytes = new TextEncoder().encode($('ownText').value).length;
  $('ownTextCount').textContent = textBytes ? `${textBytes.toLocaleString()} / 16,384 字节` : '最多 16 KB';
  $('ownText').setCustomValidity(textBytes > 16384 ? '文本不能超过 16 KB。' : '');
  $('ownSendText').disabled = locked || !recipient || !$('ownText').value || textBytes > 16384;
  $('ownChooseFile').disabled = locked || fileActive;
  $('ownSendFile').disabled = locked || fileActive || !recipient || !ownSelectedFile;
  $('ownFileName').textContent = ownSelectedFile?.name || '尚未选择文件';
  $('ownFileSize').textContent = ownSelectedFile ? formatBytes(ownSelectedFile.size) : '';
  $('ownTransferRefresh').disabled = locked || ownTransferReadInFlight;
  $('ownCancelJob').disabled = locked || !fileActive;
  $('ownCancelJob').hidden = !fileActive;
  for (const button of $('deviceList').querySelectorAll('.device-send')) {
    const selected = button.dataset.deviceId === ownRecipientId;
    button.textContent = selected ? '已选择' : '发送';
    button.setAttribute('aria-pressed', String(selected));
    button.disabled = locked;
  }
  for (const button of $('ownTransferList').querySelectorAll('[data-transfer-action]')) {
    button.disabled = locked || (fileActive && button.dataset.fileAccept === 'true');
  }
}

function makeTransferAction(label, action, transfer) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = action === 'accept' ? 'button primary' : 'button subtle';
  button.textContent = label;
  button.dataset.transferAction = action;
  button.dataset.fileAccept = String(action === 'accept' && transfer.kind === 'file');
  button.addEventListener('click', () => void mutateOwnTransfer(
      'ownTransferRespond', {id: transfer.id, action}));
  return button;
}

function renderOwnTransferState(next) {
  if (!next || !Array.isArray(next.transfers) || !next.job || typeof next.job !== 'object') {
    throw new Error('浏览器返回了无效的传输状态。');
  }
  ownTransferState = next;
  showError('ownTransferError', typeof next.error === 'string' ? next.error : '');
  const list = $('ownTransferList');
  const transfers = next.transfers.filter(transfer => transfer &&
      typeof transfer.id === 'string' &&
      (transfer.senderDeviceId === accountState.deviceId ||
       transfer.receiverDeviceId === accountState.deviceId));
  transfers.sort((a, b) => Number(b.status === 'pending' && b.receiverDeviceId === accountState.deviceId) -
      Number(a.status === 'pending' && a.receiverDeviceId === accountState.deviceId) ||
      Number(b.createdAt || 0) - Number(a.createdAt || 0));
  // Preserve focus and text selection while the cached list is unchanged.
  const listKey = JSON.stringify([transfers, Boolean(next.job.active), next.job.transferId]);
  if (listKey !== ownTransferListKey) {
    ownTransferListKey = listKey;
    list.replaceChildren();
    for (const transfer of transfers) {
      const incoming = transfer.receiverDeviceId === accountState.deviceId;
      const item = document.createElement('li');
      item.className = 'own-transfer-item';
      item.dataset.transferId = transfer.id;
      const heading = document.createElement('div');
      heading.className = 'own-transfer-item-heading';
      const title = document.createElement('strong');
      title.textContent = `${incoming ? '来自' : '发送到'} ${
        (incoming ? transfer.senderName : transfer.receiverName) || '另一台设备'}`;
      const status = document.createElement('span');
      status.className = 'own-transfer-status';
      status.textContent = transfer.status === 'pending' && incoming ? '待你接收' :
          invitationLabels.get(transfer.status) || '正在更新';
      heading.append(title, status);
      const content = document.createElement('p');
      content.className = 'own-transfer-content';
      if (transfer.kind === 'file') {
        content.textContent = `${transfer.fileName || '文件'} · ${formatBytes(transfer.fileSize)}`;
      } else if (transfer.status === 'completed') {
        content.classList.add('own-received-text');
        content.textContent = typeof transfer.text === 'string' ? transfer.text : '';
      } else {
        content.textContent = '文本消息';
      }
      const actions = document.createElement('div');
      actions.className = 'own-transfer-item-actions';
      if (transfer.status === 'pending' && incoming) {
        actions.append(makeTransferAction('接受', 'accept', transfer),
            makeTransferAction('拒绝', 'reject', transfer));
      } else if (transfer.status === 'accepted' && incoming && transfer.kind === 'file' &&
          !(next.job.active && next.job.transferId === transfer.id)) {
        actions.append(makeTransferAction('继续接收', 'accept', transfer),
            makeTransferAction('取消', 'cancel', transfer));
      } else if ((transfer.status === 'pending' && !incoming) || transfer.status === 'accepted') {
        actions.append(makeTransferAction('取消', 'cancel', transfer));
      }
      if (transfer.status === 'completed' && transfer.kind === 'text') {
        const copy = document.createElement('button');
        copy.type = 'button';
        copy.className = 'button subtle';
        copy.textContent = '复制文本';
        copy.addEventListener('click', async () => {
          try {
            await navigator.clipboard.writeText(transfer.text || '');
            copy.textContent = '已复制';
          } catch {
            const selection = window.getSelection();
            const range = document.createRange();
            range.selectNodeContents(content);
            selection.removeAllRanges();
            selection.addRange(range);
            copy.textContent = '已选中，请复制';
          }
        });
        actions.append(copy);
      }
      item.append(heading, content, actions);
      list.append(item);
    }
  }
  $('ownTransfersEmpty').hidden = transfers.length > 0;
  $('ownTransferCount').textContent = transfers.length ? `${transfers.length} 条` : '';
  const job = next.job;
  $('ownJob').hidden = !job.active && !job.transferId;
  $('ownJobStatus').textContent = stateLabels.get(job.state) || (job.active ? '正在传输' : '文件传输');
  const percent = Math.max(0, Math.min(100, Number(job.percent) || 0));
  $('ownJobProgress').value = percent;
  $('ownJobProgressText').textContent = `${formatBytes(job.transferredBytes)} / ${formatBytes(job.totalBytes)} · ${Math.round(percent)}%`;
  $('ownJobSavedPath').textContent = job.savedPath ? `已保存到：${job.savedPath}` : '';
  $('ownJobSavedPath').hidden = !job.savedPath;
  showError('ownJobError', typeof job.error === 'string' ? job.error : '');
  updateOwnTransferControls();
}

function scheduleOwnTransferPoll() {
  clearTimeout(ownTransferPollTimer);
  if (sendWithPromise && accountState.authenticated && !pageClosed && !document.hidden) {
    ownTransferPollTimer = setTimeout(readOwnTransferState, 1000);
  }
}

async function readOwnTransferState(refresh = false) {
  if (!sendWithPromise || !accountState.authenticated || pageClosed || document.hidden ||
      ownTransferReadInFlight || ownTransferPending || accountPending) {
    scheduleOwnTransferPoll();
    return;
  }
  const version = ownTransferVersion;
  ownTransferReadInFlight = true;
  try {
    const next = await sendWithPromise(refresh ? 'ownTransferRefresh' : 'ownTransferGetState');
    if (version === ownTransferVersion && accountState.authenticated && !pageClosed) {
      renderOwnTransferState(next);
    }
  } catch {
    if (version === ownTransferVersion && !pageClosed) {
      showError('ownTransferError', '暂时无法获取传输状态，请稍后刷新。');
    }
  } finally {
    ownTransferReadInFlight = false;
    updateOwnTransferControls();
    scheduleOwnTransferPoll();
  }
}

async function mutateOwnTransfer(method, options) {
  if (!sendWithPromise || !accountState.authenticated || ownTransferPending ||
      accountPending || pageClosed) return false;
  const version = ++ownTransferVersion;
  ownTransferPending = true;
  clearTimeout(ownTransferPollTimer);
  showError('ownTransferError', '');
  updateOwnTransferControls();
  try {
    const next = options ? await sendWithPromise(method, options) : await sendWithPromise(method);
    if (version !== ownTransferVersion || !accountState.authenticated || pageClosed) return false;
    renderOwnTransferState(next);
    return !next.error;
  } catch {
    if (version === ownTransferVersion && !pageClosed) {
      showError('ownTransferError', '操作未完成，请检查网络后重试。');
    }
    return false;
  } finally {
    ownTransferPending = false;
    updateOwnTransferControls();
    scheduleOwnTransferPoll();
  }
}

$('ownText').addEventListener('input', updateOwnTransferControls);
$('ownTextForm').addEventListener('submit', async event => {
  event.preventDefault();
  updateOwnTransferControls();
  if ($('ownSendText').disabled || !$('ownTextForm').reportValidity()) return;
  const text = $('ownText').value;
  if (await mutateOwnTransfer('ownTransferSendText', {deviceId: ownRecipientId, text})) {
    $('ownText').value = '';
    updateOwnTransferControls();
  }
});
$('ownChooseFile').addEventListener('click', async () => {
  if ($('ownChooseFile').disabled) return;
  const version = ++ownTransferVersion;
  ownTransferPending = true;
  updateOwnTransferControls();
  try {
    const file = await sendWithPromise('ownTransferChooseFile');
    if (version !== ownTransferVersion || !accountState.authenticated || pageClosed) return;
    if (!file?.cancelled) {
      if (!file || typeof file.name !== 'string' || !Number.isFinite(file.size) || file.size < 0) {
        throw new Error('Invalid file selection');
      }
      ownSelectedFile = {name: file.name, size: file.size};
    }
  } catch {
    if (version === ownTransferVersion && !pageClosed) {
      showError('ownTransferError', '无法读取所选文件，请重新选择。');
    }
  } finally {
    ownTransferPending = false;
    updateOwnTransferControls();
    scheduleOwnTransferPoll();
  }
});
$('ownSendFile').addEventListener('click', async () => {
  if ($('ownSendFile').disabled) return;
  if (await mutateOwnTransfer('ownTransferSendFile', {deviceId: ownRecipientId})) {
    ownSelectedFile = null;
    updateOwnTransferControls();
  }
});
$('ownTransferRefresh').addEventListener('click', () => void readOwnTransferState(true));
$('ownCancelJob').addEventListener('click', () => void mutateOwnTransfer('ownTransferCancel'));

function scheduleAccountPoll() {
  clearTimeout(accountPollTimer);
  if (sendWithPromise && !pageClosed && !document.hidden) {
    accountPollTimer = setTimeout(readAccountState,
        accountState.restoring || accountState.busy ? 1000 : 5000);
  }
}

async function readAccountState(refresh = false) {
  if (!sendWithPromise || pageClosed || document.hidden || accountReadInFlight ||
      accountPending) {
    return;
  }
  const version = accountVersion;
  accountReadInFlight = true;
  updateAccountControls();
  try {
    // The native service maintains the heartbeat and cached device directory.
    const next = await sendWithPromise(
        refresh ? 'xenonAccountRefresh' : 'xenonAccountGetState');
    if (version === accountVersion && !pageClosed) {
      renderAccountState(next);
    }
  } catch {
    if (version === accountVersion && !pageClosed) {
      if (accountState.restoring) {
        accountState = {authenticated: false, restoring: false};
      }
      $('accountStatus').textContent = '';
      showError('accountError', '无法获取账号状态，请稍后重试。');
    }
  } finally {
    accountReadInFlight = false;
    updateAccountControls();
    scheduleAccountPoll();
  }
}

async function mutateAccount(method, credentials) {
  if (!sendWithPromise || accountPending || pageClosed) {
    return;
  }
  const version = ++accountVersion;
  accountPending = true;
  clearTimeout(accountPollTimer);
  showError('accountError', '');
  $('accountStatus').textContent = method === 'xenonAccountLogout' ? '正在退出登录…' :
      method === 'xenonAccountRegister' ? '正在创建账号…' : '正在登录…';
  updateAccountControls();
  try {
    const next = credentials ? await sendWithPromise(method, credentials) :
        await sendWithPromise(method);
    if (version === accountVersion && !pageClosed) {
      renderAccountState(next);
    }
  } catch {
    if (version === accountVersion && !pageClosed) {
      $('accountStatus').textContent = '';
      showError('accountError', '账号操作失败，请稍后重试。');
    }
  } finally {
    if (credentials) {
      credentials.password = '';
    }
    $('accountPassword').value = '';
    $('accountConfirm').value = '';
    accountPending = false;
    updateAccountControls();
    scheduleAccountPoll();
  }
}

function setAccountMode(register) {
  if (accountPending || accountState.restoring || accountState.busy ||
      accountState.authenticated) {
    return;
  }
  accountRegisterMode = register;
  $('accountPassword').value = '';
  $('accountConfirm').value = '';
  $('accountConfirm').setCustomValidity('');
  showError('accountError', '');
  updateAccountControls();
}

$('accountLoginMode').addEventListener('click', () => setAccountMode(false));
$('accountRegisterMode').addEventListener('click', () => setAccountMode(true));
$('accountPassword').addEventListener('input', () =>
  $('accountConfirm').setCustomValidity(''));
$('accountConfirm').addEventListener('input', () =>
  $('accountConfirm').setCustomValidity(''));
$('accountForm').addEventListener('submit', event => {
  event.preventDefault();
  if (!sendWithPromise || accountPending || accountState.restoring ||
      accountState.busy || accountState.authenticated || pageClosed) {
    return;
  }
  $('accountUsername').value = $('accountUsername').value.trim();
  $('accountConfirm').setCustomValidity(accountRegisterMode &&
      $('accountPassword').value !== $('accountConfirm').value ? '两次输入的密码不一致。' : '');
  if (!$('accountForm').reportValidity()) {
    return;
  }
  void mutateAccount(accountRegisterMode ? 'xenonAccountRegister' : 'xenonAccountLogin', {
    username: $('accountUsername').value,
    password: $('accountPassword').value,
  });
});
$('accountRefresh').addEventListener('click', () => void readAccountState(true));
$('accountLogout').addEventListener('click', () => void mutateAccount('xenonAccountLogout'));
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    clearTimeout(accountPollTimer);
    clearTimeout(ownTransferPollTimer);
  } else {
    void readAccountState();
    void readOwnTransferState();
  }
});

async function initialize() {
  updateForm();
  updateAccountControls();
  updateOwnTransferControls();
  if (location.protocol !== 'chrome:' ||
      location.hostname !== 'cross-transfer-test' ||
      typeof globalThis.chrome?.send !== 'function') {
    $('bridgeBadge').textContent = '静态预览';
    $('accountStatus').textContent = '静态预览不支持账号登录。';
    showError('bridgeNotice', '当前仅能预览界面。请在包含 Cross Transfer 的 Chromium 中打开 chrome://cross-transfer-test/，使用真实文件传输。');
    return;
  }
  try {
    const bridge = await import('chrome://resources/js/cr.js');
    sendWithPromise = bridge.sendWithPromise;
    if (typeof sendWithPromise !== 'function') {
      throw new Error('浏览器通信接口不可用');
    }
    updateControls();
    updateAccountControls();
    await Promise.all([pollStatus(), readAccountState()]);
  } catch (error) {
    sendWithPromise = null;
    updateControls();
    updateAccountControls();
    $('accountStatus').textContent = '账号服务不可用，请刷新页面重试。';
    $('bridgeBadge').textContent = '连接失败';
    showError('bridgeNotice', `无法连接传输服务：${redactError(error)}。请确认此浏览器包含 Cross Transfer，并刷新页面。`);
  }
}

window.addEventListener('pagehide', () => {
  pageClosed = true;
  ++accountVersion;
  ++ownTransferVersion;
  clearTimeout(accountPollTimer);
  clearTimeout(ownTransferPollTimer);
  clearTimeout(pollTimer);
  $('accountPassword').value = '';
  $('accountConfirm').value = '';
});
void initialize();
