// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {addWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';
import './session.js';

const {RemoteSession, OperationEpoch, parseIceServers, stopStream} =
    globalThis.XenonRemote;
const $ = id => document.getElementById(id);
const names = {tab: '页面', window: '浏览器窗口', screen: '桌面'};
const captureEpoch = new OperationEpoch();
const pairingEpoch = new OperationEpoch();
let capture = null;
let busyCapture = false;
let busyPairing = false;
let controlPending = false;
let controlEpoch = 0;
let controlEnabled = false;
let capabilities = null;

function setStatus(title, description, state = '') {
  $('status-title').textContent = title;
  $('status-description').textContent = description;
  $('status-dot').className = `status-dot ${state}`;
}

function showError(error) {
  $('error').textContent = typeof error === 'string' ? error :
      error?.message || '操作未完成，请重试。';
  $('error').hidden = false;
}

function clearError() {
  $('error').hidden = true;
  $('error').textContent = '';
}

function sourceState(active = Boolean(capture)) {
  if (!capture) return;
  session.setSource({generation: capture.generation, mode: capture.mode,
    active, control: active && controlEnabled});
}

function releaseInput() {
  chrome.send('remoteReleaseInput');
}

function updateButtons() {
  const connected = session.channel?.readyState === 'open' &&
      session.peer?.connectionState === 'connected';
  $('choose-source').disabled = busyCapture || busyPairing || !capabilities;
  $('choose-source').textContent = capture ? '切换共享目标' : '选择共享目标';
  $('mode-options').disabled = busyCapture || busyPairing;
  $('share-audio').disabled = busyCapture || busyPairing;
  $('stop').disabled = !capture && !busyCapture && !session.peer;
  $('create-offer').disabled = !capture || busyCapture || busyPairing;
  $('create-offer').textContent = session.peer ? '重新生成配对信息' : '生成配对信息';
  $('copy-offer').disabled = !$('offer-output').value || busyPairing;
  $('accept-answer').disabled = busyPairing ||
      session.peer?.signalingState !== 'have-local-offer';
  $('allow-control').disabled = !capture?.controlSupported || !connected ||
      busyCapture || busyPairing || controlPending;
  $('allow-control').checked = controlEnabled;
  $('ice-servers').disabled = busyPairing;
  if (capture && !capture.controlSupported) {
    $('control-hint').textContent = '当前平台或目标暂不支持远程操作，可以观看画面。';
  } else if (controlEnabled) {
    $('control-hint').textContent = '接收端可操作当前目标；关闭此开关可立即收回权限。';
  } else {
    $('control-hint').textContent = connected ? '当前仅观看，可在这里开启操作权限。' :
        '连接后可开启；默认仅观看。';
  }
}

async function disableControl() {
  ++controlEpoch;
  controlEnabled = false;
  controlPending = false;
  releaseInput();
  sourceState();
  updateButtons();
  if (capture) {
    const generation = capture.generation;
    try {
      await sendWithPromise('remoteSetControl', {generation, enabled: false});
    } catch {
      // A stop or a newer capture generation already revoked this target.
    }
  }
}

const session = new RemoteSession('host', {
  onState(state) {
    if (state === 'connected') {
      setStatus('已连接', '正在共享画面。操作权限由本机控制。', 'active');
      sourceState();
    } else if (state === 'connecting') {
      setStatus('正在连接', '正在建立音视频连接…', 'waiting');
    } else if (state === 'failed') {
      setStatus('连接失败', '请检查网络和 ICE 配置，重新生成配对信息。');
    }
    updateButtons();
  },
  onChannel(open) {
    if (open) sourceState();
    updateButtons();
  },
  onInterrupted(reason) {
    void disableControl();
    setStatus('连接已中断', reason === 'failed' ?
        '可重新生成配对信息后重连；跨网络连接可能需要 TURN 服务。' :
        '已收回远程操作权限。连接恢复后需要在本机重新开启。');
  },
  onInput(input) {
    if (!capture || input.generation !== capture.generation ||
        (!controlEnabled && input.type !== 'release')) return;
    chrome.send('remoteInput', [input]);
  },
});

function clearPreview() {
  $('preview').srcObject = null;
  $('preview-placeholder').hidden = false;
  $('capture-size').textContent = '';
  $('mode-badge').textContent = '未选择目标';
}

async function chooseSource() {
  if (busyCapture || busyPairing) return;
  clearError();
  const epoch = captureEpoch.next();
  busyCapture = true;
  ++controlEpoch;
  controlEnabled = false;
  controlPending = false;
  releaseInput();
  sourceState(false);
  const oldStream = capture?.stream;
  capture = null;
  stopStream(oldStream);
  clearPreview();
  updateButtons();
  setStatus('选择共享目标', '请在浏览器的共享窗口中选择目标。', 'waiting');

  let pendingStream = null;
  try {
    await session.replaceStream(null);
    if (!captureEpoch.isCurrent(epoch)) return;
    const mode = document.querySelector('input[name="mode"]:checked').value;
    const result = await sendWithPromise('remoteChooseSource', {
      mode, audio: $('share-audio').checked,
    });
    if (!captureEpoch.isCurrent(epoch)) return;
    if (!result || !Number.isSafeInteger(result.generation) ||
        typeof result.streamId !== 'string' || !result.streamId ||
        result.mode !== mode) throw new Error('共享目标返回了无效信息。');

    const mandatory = {chromeMediaSource: 'desktop',
      chromeMediaSourceId: result.streamId};
    pendingStream = await navigator.mediaDevices.getUserMedia({
      video: {mandatory},
      audio: result.canRequestAudioTrack ? {mandatory} : false,
    });
    if (!captureEpoch.isCurrent(epoch)) {
      stopStream(pendingStream);
      return;
    }
    const stream = pendingStream;
    if (!stream.getVideoTracks().length ||
        stream.getVideoTracks()[0].readyState !== 'live') {
      throw new Error('未能取得共享画面，请重新选择目标。');
    }
    capture = {...result, stream};
    pendingStream = null;
    for (const track of stream.getVideoTracks()) {
      track.addEventListener('ended', () => {
        if (capture?.stream === stream) {
          void stopSharing('共享目标已停止', '共享内容已关闭，或已通过浏览器停止共享。');
        }
      });
    }
    try {
      await session.replaceStream(stream);
    } catch {
      // Some codec / resolution changes require renegotiation. Keep the new
      // capture usable, but never pretend the old peer is receiving it.
      session.close();
      $('offer-output').value = '';
      $('answer-input').value = '';
      showError('新目标需要重新建立媒体连接，请重新生成配对信息。');
    }
    if (!captureEpoch.isCurrent(epoch)) return;
    sourceState();
    $('preview').srcObject = stream;
    $('preview-placeholder').hidden = true;
    $('mode-badge').textContent = names[result.mode];
    const settings = stream.getVideoTracks()[0].getSettings();
    $('capture-size').textContent = settings.width && settings.height ?
        `${settings.width} × ${settings.height}` : '';
    setStatus(session.peer?.connectionState === 'connected' ? '已切换共享目标' :
        '共享内容已就绪', session.peer?.connectionState === 'connected' ?
        '接收端将显示新的画面。需要操作时，请重新开启权限。' :
        '生成配对信息，发送给另一台设备。', 'active');
  } catch (error) {
    stopStream(pendingStream);
    if (!captureEpoch.isCurrent(epoch)) return;
    stopStream(capture?.stream);
    capture = null;
    try { await sendWithPromise('remoteStop'); } catch { /* Already stopped. */ }
    if (!captureEpoch.isCurrent(epoch)) return;
    showError(error);
    setStatus('未开始共享', '可以重新选择页面、浏览器窗口或桌面。');
  } finally {
    if (captureEpoch.isCurrent(epoch)) {
      busyCapture = false;
      updateButtons();
    }
  }
}

async function stopSharing(title = '已停止共享', description = '采集和远程操作均已结束。') {
  const epoch = captureEpoch.next();
  pairingEpoch.next();
  ++controlEpoch;
  controlEnabled = false;
  controlPending = false;
  busyPairing = false;
  busyCapture = true;
  releaseInput();
  sourceState(false);
  session.close();
  stopStream(capture?.stream);
  capture = null;
  clearPreview();
  $('offer-output').value = '';
  $('answer-input').value = '';
  updateButtons();
  setStatus(title, description);
  try {
    await sendWithPromise('remoteStop');
  } catch (error) {
    if (captureEpoch.isCurrent(epoch)) showError(error);
  } finally {
    if (captureEpoch.isCurrent(epoch)) {
      busyCapture = false;
      updateButtons();
    }
  }
}

async function createOffer() {
  if (!capture || busyPairing || busyCapture) return;
  clearError();
  let iceServers;
  try { iceServers = parseIceServers($('ice-servers').value); }
  catch (error) { showError(error); return; }
  const epoch = pairingEpoch.next();
  busyPairing = true;
  $('offer-output').value = '';
  $('answer-input').value = '';
  updateButtons();
  setStatus('正在生成配对信息', '正在收集本机网络地址…', 'waiting');
  await disableControl();
  try {
    if (!pairingEpoch.isCurrent(epoch) || !capture) return;
    const offer = await session.createOffer(capture.stream, iceServers);
    if (!pairingEpoch.isCurrent(epoch)) return;
    sourceState();
    $('offer-output').value = offer;
    setStatus('等待接收端回复', '将配对信息发给接收端，再把对方的回复粘贴回来。', 'waiting');
  } catch (error) {
    if (!pairingEpoch.isCurrent(epoch)) return;
    session.close();
    showError(error);
    setStatus('配对未完成', '请检查连接设置后重试。');
  } finally {
    if (pairingEpoch.isCurrent(epoch)) {
      busyPairing = false;
      updateButtons();
    }
  }
}

async function acceptAnswer() {
  if (busyPairing) return;
  clearError();
  const epoch = pairingEpoch.value;
  busyPairing = true;
  updateButtons();
  try {
    await session.acceptAnswer($('answer-input').value);
    if (!pairingEpoch.isCurrent(epoch)) return;
    $('answer-input').value = '';
    setStatus('正在连接', '等待接收端建立媒体连接…', 'waiting');
  } catch (error) {
    if (pairingEpoch.isCurrent(epoch)) showError(error);
  } finally {
    if (pairingEpoch.isCurrent(epoch)) {
      busyPairing = false;
      updateButtons();
    }
  }
}

async function setControl() {
  const enabled = $('allow-control').checked;
  if (!capture || controlPending) return;
  const generation = capture.generation;
  const epoch = ++controlEpoch;
  controlPending = true;
  if (!enabled) {
    controlEnabled = false;
    releaseInput();
    sourceState();
  }
  updateButtons();
  try {
    const result = await sendWithPromise('remoteSetControl', {generation, enabled});
    if (epoch !== controlEpoch || capture?.generation !== generation) return;
    controlEnabled = result?.enabled === true && enabled &&
        session.channel?.readyState === 'open' &&
        session.peer?.connectionState === 'connected';
    sourceState();
  } catch (error) {
    if (epoch !== controlEpoch) return;
    controlEnabled = false;
    showError(error);
    sourceState();
  } finally {
    if (epoch === controlEpoch) {
      controlPending = false;
      updateButtons();
    }
  }
}

async function copyText(id) {
  const element = $(id);
  try {
    await navigator.clipboard.writeText(element.value);
    setStatus('配对信息已复制', '发送给接收端，在“共享端配对信息”中粘贴。', 'waiting');
  } catch {
    element.focus();
    element.select();
    showError('已选中配对信息，请使用系统复制操作。');
  }
}

$('choose-source').addEventListener('click', () => void chooseSource());
$('stop').addEventListener('click', () => void stopSharing());
$('create-offer').addEventListener('click', () => void createOffer());
$('accept-answer').addEventListener('click', () => void acceptAnswer());
$('copy-offer').addEventListener('click', () => void copyText('offer-output'));
$('allow-control').addEventListener('change', () => void setControl());
addWebUiListener('remote-control-changed', info => {
  if (!capture || info.generation !== capture.generation || info.enabled !== false) return;
  ++controlEpoch;
  controlEnabled = false;
  controlPending = false;
  sourceState();
  updateButtons();
  if (info.reason) showError(info.reason);
});
window.addEventListener('pagehide', () => {
  captureEpoch.next();
  pairingEpoch.next();
  releaseInput();
  session.close();
  stopStream(capture?.stream);
  // The native handler also revokes its target on JavascriptDisallowed and
  // destruction. No async pagehide reply is needed for cleanup.
  void sendWithPromise('remoteStop').catch(() => {});
});

sendWithPromise('remoteGetCapabilities').then(result => {
  capabilities = result;
  updateButtons();
}).catch(error => {
  showError(error);
  setStatus('共享功能不可用', '请使用已启用远程共享功能的 Xenon 浏览器。');
});
updateButtons();
