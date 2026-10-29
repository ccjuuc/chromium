// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(() => {
  'use strict';
  const {RemoteSession, OperationEpoch, parseIceServers, normalizedPoint,
    modifiersFor, PointerMoveQueue} = globalThis.XenonRemote;
  const $ = id => document.getElementById(id);
  const video = $('remote-video');
  const names = {tab: '页面', window: '浏览器窗口', screen: '桌面'};
  const pairingEpoch = new OperationEpoch();
  let busyPairing = false;
  let activePointer = null;
  let activeButton = null;
  let lastPoint = {x: .5, y: .5};
  let composing = false;
  const pointerMoves = new PointerMoveQueue(input => {
    if (canControl()) session.sendInput(input);
  });

  function showError(error) {
    $('error').textContent = typeof error === 'string' ? error :
        error?.message || '操作未完成，请重试。';
    $('error').hidden = false;
  }

  function clearError() {
    $('error').textContent = '';
    $('error').hidden = true;
  }

  function setStatus(title, description, state = '') {
    $('status-title').textContent = title;
    $('status-description').textContent = description;
    $('status-dot').className = `status-dot ${state}`;
  }

  function canControl() {
    return session.peer?.connectionState === 'connected' &&
        session.channel?.readyState === 'open' &&
        session.source?.active && session.source?.control;
  }

  function showControls(expanded) {
    $('viewer-controls').hidden = !expanded;
    $('viewer-menu').setAttribute('aria-expanded', String(expanded));
  }

  function updateLayout() {
    const connected = session.peer?.connectionState === 'connected' &&
        session.channel?.readyState === 'open';
    if (document.body.classList.contains('viewer-connected') === connected) return;
    document.body.classList.toggle('viewer-connected', connected);
    $('viewer-menu').hidden = !connected;
    showControls(!connected || !$('play-audio').hidden);
    if (!connected) {
      $('pairing-section').open = true;
      if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
    }
  }

  function updateControls() {
    updateLayout();
    const allowed = Boolean(canControl());
    $('viewer-menu').textContent = allowed ? '控制 · 可以操作' : '控制 · 仅观看';
    $('control-badge').textContent = allowed ? '可以操作' : '仅观看';
    $('source-title').textContent = session.source ?
        `远程${names[session.source.mode]}` : '远程画面';
    $('input-hint').textContent = allowed ?
        '点击画面后可使用键盘鼠标。手机与中文输入可使用下方文字输入框。' :
        '当前仅观看。共享端开启“允许接收端操作”后，可以操作远程画面。';
    $('text-input').disabled = !allowed;
    $('send-text').disabled = !allowed;
    video.classList.toggle('controllable', allowed);
    $('disconnect').disabled = !session.peer && !busyPairing;
    $('create-answer').disabled = busyPairing;
    $('copy-answer').disabled = !$('answer-output').value || busyPairing;
    $('ice-servers').disabled = busyPairing;
    $('fullscreen').disabled = !session.source?.active;
    if (!allowed) clearPointer();
  }

  function clearPointer() {
    pointerMoves.cancel();
    const previous = activePointer;
    activePointer = null;
    activeButton = null;
    if (previous !== null && video.hasPointerCapture(previous)) {
      video.releasePointerCapture(previous);
    }
  }

  function releaseInput() {
    pointerMoves.cancel();
    session.sendInput({type: 'release'});
    clearPointer();
  }

  async function playMedia() {
    try {
      await video.play();
      $('play-audio').hidden = true;
    } catch {
      // A user gesture may be required when the stream includes audio.
      $('play-audio').hidden = false;
      showControls(true);
    }
  }

  const session = new RemoteSession('viewer', {
    onState(state) {
      if (state === 'connected') {
        setStatus('已连接', '正在接收共享画面。', 'active');
        $('pairing-section').open = false;
      } else if (state === 'connecting') {
        setStatus('正在连接', '正在建立音视频连接…', 'waiting');
      }
      updateControls();
    },
    onChannel() { updateControls(); },
    onSource(source) {
      session.sendInput({type: 'release'});
      clearPointer();
      $('preview-placeholder').hidden = source.active;
      if (!source.active) {
        setStatus('共享端正在切换目标', '当前目标的操作权限已关闭。', 'waiting');
      } else if (session.peer?.connectionState === 'connected') {
        setStatus('已连接', `正在查看共享端的${names[source.mode]}。`, 'active');
      }
      updateControls();
    },
    onStream(stream) {
      if (video.srcObject !== stream) video.srcObject = stream;
      void playMedia();
    },
    onInterrupted() {
      releaseInput();
      updateControls();
      setStatus('连接已中断', '操作权限已关闭。若无法恢复，请重新配对。');
    },
  });

  function disconnect() {
    pairingEpoch.next();
    releaseInput();
    session.close();
    video.srcObject = null;
    busyPairing = false;
    $('answer-output').value = '';
    $('preview-placeholder').hidden = false;
    $('play-audio').hidden = true;
    $('pairing-section').open = true;
    updateControls();
    setStatus('已断开连接', '粘贴新的共享端配对信息即可重新连接。');
  }

  async function createAnswer() {
    if (busyPairing) return;
    clearError();
    let iceServers;
    try { iceServers = parseIceServers($('ice-servers').value); }
    catch (error) { showError(error); return; }
    const epoch = pairingEpoch.next();
    releaseInput();
    busyPairing = true;
    $('answer-output').value = '';
    setStatus('正在生成回复', '正在收集本机网络地址…', 'waiting');
    updateControls();
    try {
      const answer = await session.createAnswer($('offer-input').value, iceServers);
      if (!pairingEpoch.isCurrent(epoch)) return;
      $('answer-output').value = answer;
      setStatus('等待共享端确认', '复制这段回复，发回共享端并点击“确认连接”。', 'waiting');
    } catch (error) {
      if (!pairingEpoch.isCurrent(epoch)) return;
      session.close();
      video.srcObject = null;
      $('preview-placeholder').hidden = false;
      showError(error);
      setStatus('配对未完成', '请检查共享端信息和网络配置后重试。');
    } finally {
      if (pairingEpoch.isCurrent(epoch)) {
        busyPairing = false;
        updateControls();
      }
    }
  }

  function pointerPoint(event, clampOutside = false) {
    return normalizedPoint(video.getBoundingClientRect(), video.videoWidth,
      video.videoHeight, event.clientX, event.clientY, clampOutside);
  }

  video.addEventListener('pointerdown', event => {
    if (!canControl() || activePointer !== null || !event.isPrimary) return;
    const point = pointerPoint(event);
    const button = event.pointerType === 'mouse' ? event.button : 0;
    if (!point || ![0, 1, 2].includes(button)) return;
    event.preventDefault();
    video.focus({preventScroll: true});
    activePointer = event.pointerId;
    activeButton = button;
    lastPoint = point;
    pointerMoves.flush();
    video.setPointerCapture(event.pointerId);
    session.sendInput({type: 'button', ...point, button, down: true,
      modifiers: modifiersFor(event)});
  });

  video.addEventListener('pointermove', event => {
    if (!canControl() || (activePointer !== null &&
        activePointer !== event.pointerId)) return;
    const point = pointerPoint(event, activePointer !== null);
    if (!point) return;
    if (activePointer !== null) event.preventDefault();
    lastPoint = point;
    pointerMoves.push({type: 'move', ...point, modifiers: modifiersFor(event)});
  });

  video.addEventListener('pointerup', event => {
    if (activePointer !== event.pointerId) return;
    event.preventDefault();
    const point = pointerPoint(event, true) || lastPoint;
    pointerMoves.flush();
    session.sendInput({type: 'button', ...point,
      button: activeButton, down: false, modifiers: modifiersFor(event)});
    clearPointer();
  });
  video.addEventListener('pointercancel', releaseInput);
  video.addEventListener('lostpointercapture', () => {
    if (activePointer !== null) releaseInput();
  });
  video.addEventListener('contextmenu', event => {
    if (canControl()) event.preventDefault();
  });
  video.addEventListener('wheel', event => {
    if (!canControl()) return;
    const point = pointerPoint(event);
    if (!point) return;
    event.preventDefault();
    const multiplier = event.deltaMode === 1 ? 16 :
        event.deltaMode === 2 ? video.clientHeight : 1;
    const deltaX = Math.max(-10000, Math.min(10000, event.deltaX * multiplier));
    const deltaY = Math.max(-10000, Math.min(10000, event.deltaY * multiplier));
    pointerMoves.flush();
    session.sendInput({type: 'wheel', ...point, deltaX, deltaY,
      modifiers: modifiersFor(event)});
  }, {passive: false});

  function sendKey(event, down) {
    if (!canControl() || event.isComposing || event.key === 'Process' ||
        !event.code || event.key === 'Dead') return;
    // Physical key messages are the only path for hardware keyboard text.
    // The native router supplies printable text; do not also insertText here.
    event.preventDefault();
    pointerMoves.flush();
    session.sendInput({type: 'key', code: event.code, key: event.key,
      down, repeat: event.repeat, modifiers: modifiersFor(event)});
  }
  video.addEventListener('keydown', event => sendKey(event, true));
  video.addEventListener('keyup', event => sendKey(event, false));
  video.addEventListener('blur', releaseInput);
  window.addEventListener('blur', releaseInput);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) releaseInput();
  });

  $('text-input').addEventListener('compositionstart', () => { composing = true; });
  $('text-input').addEventListener('compositionend', () => { composing = false; });
  $('text-form').addEventListener('submit', event => {
    event.preventDefault();
    if (composing || !canControl()) return;
    const text = $('text-input').value;
    if (!text) return;
    if (new TextEncoder().encode(text).length > 4096) {
      showError('文字过长，请分段发送（每次最多 4096 字节）。');
      return;
    }
    if (session.sendInput({type: 'text', text})) {
      $('text-input').value = '';
      clearError();
    } else {
      showError('当前未能发送文字，请等待连接恢复后重试。');
    }
  });

  $('create-answer').addEventListener('click', () => void createAnswer());
  $('disconnect').addEventListener('click', disconnect);
  $('copy-answer').addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText($('answer-output').value);
      setStatus('回复已复制', '发回共享端，粘贴到“接收端回复”并确认连接。', 'waiting');
    } catch {
      $('answer-output').focus();
      $('answer-output').select();
      showError('已选中回复，请使用系统复制操作。');
    }
  });
  $('play-audio').addEventListener('click', () => void playMedia());
  $('viewer-menu').addEventListener('click', () => {
    showControls($('viewer-controls').hidden);
  });
  $('viewer-controls').addEventListener('keydown', event => {
    if (event.key === 'Escape' && document.body.classList.contains('viewer-connected')) {
      event.preventDefault();
      event.stopPropagation();
      showControls(false);
      $('viewer-menu').focus();
    }
  });
  $('fullscreen').addEventListener('click', async () => {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await $('viewer-stage').requestFullscreen();
      if (document.body.classList.contains('viewer-connected')) showControls(false);
      video.focus();
    } catch (error) { showError(error); }
  });
  document.addEventListener('fullscreenchange', () => {
    $('fullscreen').textContent = document.fullscreenElement ? '退出全屏' : '全屏';
  });
  window.addEventListener('pagehide', disconnect);
  updateControls();
})();
