// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(() => {
  'use strict';

  let electron = null;
  let ipcRenderer = null;

  try {
    if (typeof window.require === 'function') {
      electron = window.require('electron');
      ipcRenderer = electron.ipcRenderer;
    } else if (window.electron && window.electron.ipcRenderer) {
      electron = window.electron;
      ipcRenderer = window.electron.ipcRenderer;
    }
  } catch (err) {
    console.error('Failed to acquire electron.ipcRenderer:', err);
  }

  const logContainer = document.getElementById('log-container');
  const statusDot = document.getElementById('status-dot');
  const mainStatus = document.getElementById('main-status');
  const chromeVersion = document.getElementById('chrome-version');
  const v8Version = document.getElementById('v8-version');
  const nodeVersion = document.getElementById('node-version');
  const platformArch = document.getElementById('platform-arch');

  function appendLog(level, message, data) {
    const item = document.createElement('div');
    item.className = 'log-item';

    const now = new Date();
    const timeStr = now.toTimeString().split(' ')[0] + '.' +
        String(now.getMilliseconds()).padStart(3, '0');

    const timeSpan = document.createElement('span');
    timeSpan.className = 'log-time';
    timeSpan.textContent = `[${timeStr}]`;

    const tagSpan = document.createElement('span');
    tagSpan.className = `log-tag ${level}`;
    tagSpan.textContent = `[${level.toUpperCase()}]`;

    const msgSpan = document.createElement('span');
    msgSpan.className = 'log-msg';
    let text = message;
    if (data !== undefined) {
      text += ' ' +
          (typeof data === 'object' ? JSON.stringify(data) : String(data));
    }
    msgSpan.textContent = text;

    item.appendChild(timeSpan);
    item.appendChild(tagSpan);
    item.appendChild(msgSpan);
    logContainer.appendChild(item);
    logContainer.scrollTop = logContainer.scrollHeight;
  }

  async function initRuntimeInfo() {
    if (ipcRenderer) {
      statusDot.className = 'status-dot connected';
      mainStatus.textContent = '已连接 (Connected via Mojo)';
      appendLog('success', 'Xenon Electron ipcRenderer 连接就绪', {
        transport: 'Mojo DocumentService',
        origin: window.location.href,
      });
      try {
        const runtime = await ipcRenderer.invoke('test:runtime-info');
        chromeVersion.textContent = runtime.versions?.chrome || '未知';
        v8Version.textContent = runtime.versions?.v8 || '未知';
        nodeVersion.textContent = runtime.versions?.node || '未知';
        platformArch.textContent = `${runtime.platform} / ${runtime.arch}`;
      } catch (err) {
        chromeVersion.textContent =
            navigator.userAgentData?.brands?.[0]?.version || '未知';
        v8Version.textContent = 'Main 查询失败';
        nodeVersion.textContent = 'Main 查询失败';
        platformArch.textContent = 'Main 查询失败';
        appendLog('warn', '读取 Main 运行时信息失败:', err.message || err);
      }
    } else {
      statusDot.className = 'status-dot';
      statusDot.style.background = '#ef4444';
      mainStatus.textContent = '未检测到 ipcRenderer';
      chromeVersion.textContent =
          navigator.userAgentData?.brands?.[0]?.version || '未知';
      v8Version.textContent = 'IPC 未连接';
      nodeVersion.textContent = 'IPC 未连接';
      platformArch.textContent = 'IPC 未连接';
      appendLog('error', '未找到 window.require("electron").ipcRenderer');
    }
  }

  if (ipcRenderer) {
    ipcRenderer.on('test:reply-result', (_event, value) => {
      appendLog(
          'info', '收到 Main 进程事件推送 (test:reply-result):', value);
    });

  }

  document.getElementById('btn-ping').addEventListener('click', async () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    const start = performance.now();
    try {
      const isReady = await ipcRenderer.invoke('test:is-ready');
      const cost = (performance.now() - start).toFixed(2);
      appendLog(
          'success', `Ping Main 进程成功 (${cost}ms) -> app.isReady():`,
          isReady);
    } catch (err) {
      appendLog('error', 'Ping 失败:', err.message || err);
    }
  });

  document.getElementById('btn-get-counter').addEventListener('click', () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    try {
      ipcRenderer.send('test:increment', 1);
      const val = ipcRenderer.sendSync('test:get-sync');
      appendLog('success', 'ipcRenderer.sendSync("test:get-sync") ->', val);
    } catch (err) {
      appendLog('error', 'sendSync 调用失败:', err.message || err);
    }
  });

  document.getElementById('btn-async-add').addEventListener('click', async () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    try {
      const a = Math.floor(Math.random() * 50);
      const b = Math.floor(Math.random() * 50);
      const sum = await ipcRenderer.invoke('test:async-add', a, b);
      appendLog(
          'success', `ipcRenderer.invoke("test:async-add", ${a}, ${b}) ->`,
          sum);
    } catch (err) {
      appendLog('error', 'invoke 调用失败:', err.message || err);
    }
  });

  document.getElementById('btn-get-paths').addEventListener('click', async () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    try {
      const paths = await ipcRenderer.invoke('test:app-paths');
      appendLog('success', 'app.getPath() 系统路径解析结果:', paths);
    } catch (err) {
      appendLog('error', '读取路径失败:', err.message || err);
    }
  });

  document.getElementById('btn-test-addon').addEventListener('click', async () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    try {
      const result = await ipcRenderer.invoke('native:add', 20, 22);
      appendLog(
          'success', 'N-API (.node) 插件运算结果 (20 + 22) ->', result);
    } catch (err) {
      appendLog(
          'warn', 'N-API 插件测试 (如果当前未指定 native addon main):',
          err.message || err);
    }
  });

  document.getElementById('btn-send-invoke').addEventListener('click', async () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    const channel = document.getElementById('ipc-channel').value.trim();
    let payload = document.getElementById('ipc-payload').value.trim();
    try {
      if (payload) {
        payload = JSON.parse(payload);
      }
    } catch {
      // Keep the original string.
    }
    appendLog('info', `正在调用 invoke("${channel}")`, payload);
    try {
      const res = await ipcRenderer.invoke(channel, payload);
      appendLog('success', `invoke("${channel}") 响应结果:`, res);
    } catch (err) {
      appendLog('error', `invoke("${channel}") 失败:`, err.message || err);
    }
  });

  document.getElementById('btn-send-async').addEventListener('click', () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    const channel = document.getElementById('ipc-channel').value.trim();
    let payload = document.getElementById('ipc-payload').value.trim();
    try {
      if (payload) {
        payload = JSON.parse(payload);
      }
    } catch {
    }
    ipcRenderer.send(channel, payload);
    appendLog('info', `已发送异步消息 send("${channel}"):`, payload);
  });

  document.getElementById('btn-send-sync').addEventListener('click', () => {
    if (!ipcRenderer) {
      return appendLog('error', 'ipcRenderer 不可用');
    }
    const channel = document.getElementById('ipc-channel').value.trim();
    let payload = document.getElementById('ipc-payload').value.trim();
    try {
      if (payload) {
        payload = JSON.parse(payload);
      }
    } catch {
    }
    try {
      const res = ipcRenderer.sendSync(channel, payload);
      appendLog('success', `sendSync("${channel}") 同步返回:`, res);
    } catch (err) {
      appendLog('error', `sendSync("${channel}") 失败:`, err.message || err);
    }
  });

  document.getElementById('btn-clear-log').addEventListener('click', () => {
    logContainer.innerHTML = '';
  });

  document.getElementById('btn-minimize')?.addEventListener('click', () => {
    if (window.xenon && window.xenon.minimize) {
      window.xenon.minimize();
    }
  });
  document.getElementById('btn-maximize')?.addEventListener('click', () => {
    if (window.xenon && window.xenon.maximize) {
      window.xenon.maximize();
    }
  });
  document.getElementById('btn-close')?.addEventListener('click', () => {
    window.close();
  });

  window.addEventListener('DOMContentLoaded', initRuntimeInfo);
})();
