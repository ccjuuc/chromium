// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
import '/strings.m.js';

import {addWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

const $ = id => document.getElementById(id);
const gmEnabled = loadTimeData.getBoolean('gmEnabled');
if (gmEnabled) {
  $('gm-test-presets').hidden = false;
  $('gm-pin-hint').hidden = false;
} else {
  $('gm-test-presets').remove();
  $('gm-pin-hint').remove();
}
const dateFormatter = new Intl.DateTimeFormat('zh-CN', {
  year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit',
  minute: '2-digit', second: '2-digit', hour12: false, timeZoneName: 'short',
});
let refreshing = false;
let testing = false;
let receivedStatus = false;
let statusUpdateSequence = 0;

function text(value, fallback = '—') {
  return typeof value === 'string' && value.length ? value : fallback;
}

function dateText(value) {
  if (typeof value !== 'string' || !value) {
    return '—';
  }
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? '—' : dateFormatter.format(date);
}

function setDate(id, value) {
  $(id).textContent = dateText(value);
  $(id).title = text(value, '');
}

function badge(id, label, tone = 'neutral') {
  const element = $(id);
  element.textContent = label;
  element.className = `badge ${tone}`;
}

function showError(id, message) {
  $(id).textContent = message;
  $(id).hidden = !message;
}

function addCell(row, value, className = '') {
  const cell = document.createElement('td');
  cell.textContent = value;
  cell.className = className;
  row.appendChild(cell);
  return cell;
}

function renderCertificateRows(certificates, body) {
  const rows = document.createDocumentFragment();
  for (const certificate of certificates) {
    const row = document.createElement('tr');
    addCell(row, text(certificate.name, '未命名证书'), 'certificate-name');
    addCell(row, certificate.role === 'trust_anchor' ? '信任锚' :
        certificate.role === 'intermediate' ? '中间 CA' : '未知');
    const constraints = addCell(row, '');
    for (const [label, entries] of [
      ['DNS', certificate.dns], ['IP', certificate.cidrs],
    ]) {
      if (Array.isArray(entries) && entries.length) {
        const line = document.createElement('div');
        line.className = 'constraint-line';
        line.textContent = `${label}：${entries.join('、')}`;
        constraints.appendChild(line);
      }
    }
    if (!constraints.childElementCount) {
      constraints.textContent = '无额外约束';
      constraints.className = 'subtle';
    }
    addCell(row, text(certificate.sha256), 'monospace fingerprint');
    rows.appendChild(row);
  }
  body.replaceChildren(rows);
}

function renderCertificateTable(certificates) {
  renderCertificateRows(certificates, $('certificate-table-body'));
  $('certificate-count').textContent = `${certificates.length} 张`;
  $('certificate-table-container').hidden = !certificates.length;
  $('snapshot-revoked').hidden = !!certificates.length;
}

function renderStatus(status) {
  const builtinStoreCertificate = status.builtinStoreCertificate;
  $('builtin-store-section').hidden = !builtinStoreCertificate;
  if (builtinStoreCertificate) {
    renderCertificateRows(
        [builtinStoreCertificate], $('builtin-store-certificate-body'));
  }
  const snapshot = status.snapshot;
  const configurationError = text(status.configurationError, '');
  const refreshError = text(status.refreshError, '');
  $('delivery-metadata').hidden = false;
  $('configured-source').textContent = text(status.sourceId);
  setDate('last-attempt', status.lastAttempt);
  setDate('last-success', status.lastSuccess);
  $('configuration-error-code').textContent = configurationError;
  $('configuration-error-row').hidden = !configurationError;
  $('refresh-error-code').textContent = refreshError;
  $('refresh-error-row').hidden = !refreshError;

  let label = snapshot ? '已就绪' : '等待快照';
  let tone = snapshot ? 'success' : 'neutral';
  let description = snapshot ?
      '证书服务已就绪，下方显示当前已接受的快照。' :
      '证书服务已启动，正在等待可用的托管快照。状态会自动更新。';
  let emptyTitle = '暂无可用的托管证书快照';
  let emptyDescription = '尚未返回已接受的快照。其他来源的证书信任不由此列表表示。';
  if (status.started === false) {
    label = '未配置';
    tone = 'neutral';
    description = '当前浏览器未启动证书下发服务。';
    emptyTitle = '未配置证书下发';
    emptyDescription = '没有可展示的托管快照；浏览器仍按现有证书信任配置工作。';
  } else if (configurationError) {
    label = '配置失败';
    tone = 'error';
    description = configurationError === 'config_read_failed' ?
        '证书服务已启动，但未能读取配置。默认读取浏览器程序目录下的 xenon-managed-ca.json，也可通过启动参数指定配置。' :
        '证书下发配置不可用，具体原因见配置错误代码。';
    emptyTitle = '证书下发配置不可用';
    emptyDescription = '当前没有可用的托管快照，请查看上方诊断信息。';
  } else if (status.ready === false) {
    label = '正在准备';
    tone = 'neutral';
    description = '正在读取本地配置与缓存，完成后将自动显示结果。';
    emptyTitle = '证书服务正在准备';
    emptyDescription = '当前尚未返回可展示的快照。';
  } else if (status.enabled === false) {
    label = '已停用';
    tone = 'neutral';
    description = '本地配置未启用证书下发。';
    emptyTitle = '证书下发已停用';
    emptyDescription = '当前不提供本来源的活动快照，其他来源的证书信任不由此列表表示。';
  } else if (status.downloading === true) {
    label = '正在拉取';
    tone = 'neutral';
    description = snapshot ?
        '正在拉取最新配置；下方仍显示当前已接受的快照。' :
        '正在向配置的服务端拉取证书，完成后将自动显示结果。';
    emptyTitle = '正在等待证书快照';
    emptyDescription = '拉取结果尚未返回，当前没有可展示的快照。';
  } else if (refreshError) {
    label = snapshot ? '刷新失败' : '拉取失败';
    tone = snapshot ? 'warning' : 'error';
    description = snapshot ?
        '最近一次拉取失败，浏览器仍保留已接受的快照。' :
        '最近一次拉取未成功，具体原因见最近拉取错误代码。';
    emptyTitle = '暂未取得可用快照';
    emptyDescription = '请结合上方最近尝试时间与错误代码判断原因。';
  }
  badge('status-badge', label, tone);
  $('download-status-description').textContent = description;
  $('snapshot-empty').hidden = !!snapshot;
  $('snapshot-details').hidden = !snapshot;
  if (!snapshot) {
    $('certificate-table-body').replaceChildren();
    $('snapshot-empty-title').textContent = emptyTitle;
    $('snapshot-empty-description').textContent = emptyDescription;
    return;
  }
  const certificates = Array.isArray(snapshot.certificates) ? snapshot.certificates : [];
  $('snapshot-source').textContent = text(snapshot.sourceId);
  $('snapshot-generation').textContent = text(snapshot.generation);
  setDate('snapshot-issued', snapshot.issuedAt);
  renderCertificateTable(certificates);
}

function acceptStatus(status) {
  if (!status || typeof status !== 'object') {
    throw new Error('Invalid status response');
  }
  renderStatus(status);
  receivedStatus = true;
  showError('status-error', '');
  $('status-checked-at').textContent = `更新于 ${dateFormatter.format(new Date())}`;
}

async function refreshStatus() {
  if (refreshing) {
    return;
  }
  refreshing = true;
  $('refresh-status').disabled = true;
  $('refresh-label').textContent = '读取中…';
  $('snapshot-section').setAttribute('aria-busy', 'true');
  showError('status-error', '');
  const sequence = statusUpdateSequence;
  try {
    const status = await sendWithPromise('getManagedCertificateStatus');
    // A pushed state can arrive while an explicit query is pending. Keep the
    // newer state instead of repainting it with the earlier query's snapshot.
    if (sequence === statusUpdateSequence) {
      acceptStatus(status);
    }
  } catch {
    if (sequence === statusUpdateSequence) {
      badge('status-badge', '状态读取失败', 'error');
      showError('status-error', receivedStatus ?
          '未能读取最新状态，下方保留上次读取结果。' :
          '暂未取得浏览器状态。');
      if (!receivedStatus) {
        $('snapshot-empty-title').textContent = '状态暂不可用';
        $('snapshot-empty-description').textContent = '此结果不表示证书信任已经改变。';
      }
    }
  } finally {
    refreshing = false;
    $('refresh-status').disabled = false;
    $('refresh-label').textContent = '刷新状态';
    $('snapshot-section').setAttribute('aria-busy', 'false');
  }
}

function renderPeerCertificate(certificate) {
  $('peer-certificate').hidden = !certificate;
  $('peer-unavailable').hidden = !!certificate;
  if (!certificate) {
    return;
  }
  $('peer-subject').textContent = text(certificate.subject);
  $('peer-issuer').textContent = text(certificate.issuer);
  setDate('peer-valid-from', certificate.validFrom);
  setDate('peer-valid-to', certificate.validTo);
  $('peer-sha256').textContent = text(certificate.sha256);
  $('peer-known-root').textContent = certificate.knownRoot === true ? '是' :
      certificate.knownRoot === false ? '否；请结合上方 TLS 结果判断' : '未提供';
  $('peer-cert-status').textContent = Number.isInteger(certificate.certStatus) ?
      `0x${(certificate.certStatus >>> 0).toString(16).padStart(8, '0')}` : '—';
}

function renderTestResult(result, requestedUrl) {
  if (!result || typeof result !== 'object') {
    throw new Error('Invalid test response');
  }
  const passed = result.tlsPassed === true;
  const errorName = text(result.netErrorName, '未提供');
  const tlsError = /^(?:net::)?ERR_(?:CERT_|SSL_|BAD_SSL_)/.test(errorName);
  const status = Number.isInteger(result.httpStatus) && result.httpStatus > 0 ?
      result.httpStatus : null;
  $('tested-url').textContent = text(result.url, requestedUrl);
  badge('test-summary', passed ? 'TLS 校验通过' : tlsError ? 'TLS 校验失败' : '连接未完成',
      passed ? 'success' : tlsError ? 'error' : 'warning');
  $('tls-card').dataset.tone = passed ? 'success' : tlsError ? 'error' : 'warning';
  $('tls-status').dataset.status = passed ? 'passed' : tlsError ? 'failed' : 'incomplete';
  $('tls-status').textContent = passed ? '通过' : tlsError ? '失败' : '未完成';
  $('tls-description').textContent = passed ? '已完成 TLS 握手和证书校验。' : tlsError ?
      'TLS 连接或证书校验失败，请结合网络错误与证书信息排查。' :
      '未能确认 TLS 校验完成，请先查看下方网络错误。';
  $('http-card').dataset.tone = status === null ? 'neutral' :
      status >= 200 && status < 300 ? 'success' : 'warning';
  $('http-status').textContent = status === null ? '未收到响应' : `HTTP ${status}`;
  $('http-description').textContent = status === null ? '没有可展示的 HTTP 状态码。' :
      status >= 300 && status < 400 ? '服务器返回重定向；本次测试未跟随跳转。' :
      status === 405 ? '服务器不支持 HEAD；此状态本身不表示证书校验失败。' :
      status >= 400 ? '服务器返回业务错误状态；请与 TLS 校验结果分别判断。' :
      '已收到服务器响应；HTTP 结果与证书校验分别显示。';
  $('network-error').textContent = Number.isInteger(result.netError) ?
      `${errorName} (${result.netError})` : errorName;
  $('connection-details').hidden = !result.connection;
  $('connection-protocol').textContent = text(result.connection?.protocol);
  $('connection-cipher').textContent = text(result.connection?.cipher);
  $('connection-suite').textContent = text(result.connection?.cipherSuite);
  renderPeerCertificate(result.certificate);
  $('result-details').hidden = false;
}

function validatedTestUrl() {
  let url;
  try {
    url = new URL($('test-url').value.trim());
    if (url.protocol !== 'https:' || !url.hostname || url.username || url.password ||
        url.href.includes('#') || url.href.length > 2048) {
      throw new Error('HTTPS URL required');
    }
  } catch {
    showError('url-error', '请输入完整 HTTPS 地址，不含账号、密码或 # 片段，规范化后最长 2048 个字符。');
    $('test-url').setAttribute('aria-invalid', 'true');
    $('test-url').focus();
    return null;
  }
  showError('url-error', '');
  $('test-url').removeAttribute('aria-invalid');
  return url;
}

function openTestUrl() {
  if (testing) {
    return;
  }
  const url = validatedTestUrl();
  if (!url) {
    return;
  }
  $('test-result').hidden = true;
  window.open(url.href, '_blank', 'noopener');
}

async function testUrl(event) {
  event.preventDefault();
  if (testing) {
    return;
  }
  const url = validatedTestUrl();
  if (!url) {
    return;
  }
  testing = true;
  $('test-button').disabled = true;
  $('test-button').textContent = '正在测试…';
  $('open-button').disabled = true;
  $('test-url').disabled = true;
  $('test-preset').disabled = true;
  $('test-result').hidden = false;
  $('test-result').setAttribute('aria-busy', 'true');
  $('result-details').hidden = true;
  $('tls-status').dataset.status = 'pending';
  $('tested-url').textContent = url.href;
  showError('test-error', '');
  badge('test-summary', '正在请求');
  try {
    const result = await sendWithPromise('testManagedCertificateUrl', url.href);
    renderTestResult(result, url.href);
  } catch {
    badge('test-summary', '测试未完成', 'error');
    showError('test-error', '未能取得测试结果，请稍后重试或重新打开此页面。');
  } finally {
    testing = false;
    $('test-button').disabled = false;
    $('test-button').textContent = '测试连接';
    $('open-button').disabled = false;
    $('test-url').disabled = false;
    $('test-preset').disabled = false;
    updatePresetHint();
    $('test-result').setAttribute('aria-busy', 'false');
  }
}

function findTestPreset(value) {
  try {
    const url = new URL(value.trim());
    return [...$('test-preset').options].find(option => option.value === url.href);
  } catch {
    return undefined;
  }
}

function updatePresetHint() {
  const preset = findTestPreset($('test-url').value);
  $('preset-hint').textContent = preset?.dataset.hint ||
      '选择后填入下方地址，点击“测试连接”执行校验。';
}

$('refresh-status').addEventListener('click', refreshStatus);
$('test-form').addEventListener('submit', testUrl);
$('open-button').addEventListener('click', openTestUrl);
$('test-preset').addEventListener('change', () => {
  if ($('test-preset').value) {
    $('test-url').value = $('test-preset').value;
  }
  updatePresetHint();
  showError('url-error', '');
  $('test-url').removeAttribute('aria-invalid');
  $('test-url').focus();
});
$('test-url').addEventListener('input', () => {
  $('test-preset').value = findTestPreset($('test-url').value)?.value || '';
  updatePresetHint();
  showError('url-error', '');
  $('test-url').removeAttribute('aria-invalid');
});
addWebUiListener('managed-certificate-status-changed', status => {
  ++statusUpdateSequence;
  acceptStatus(status);
});
refreshStatus();
