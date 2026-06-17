// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {addWebUiListener} from 'chrome://resources/js/cr.js';

function addLog(message, isError = false) {
  const logList = document.getElementById('log-list');
  const emptyState = logList.querySelector('.empty-state');
  if (emptyState) {
    emptyState.remove();
  }

  const logItem = document.createElement('div');
  logItem.className = 'log-item' + (isError ? ' error' : '');
  
  const timestamp = document.createElement('span');
  timestamp.className = 'log-time';
  timestamp.textContent = new Date().toLocaleTimeString();

  const content = document.createElement('span');
  content.className = 'log-text';
  content.textContent = message;

  logItem.appendChild(timestamp);
  logItem.appendChild(content);
  logList.appendChild(logItem);

  // Auto scroll to bottom
  logList.scrollTop = logList.scrollHeight;
}

document.addEventListener('DOMContentLoaded', () => {
  addLog('WebUI 页面已初始化，已注册监听器');

  // Register WebUI listener for dialog result
  addWebUiListener('dialog-result', (accepted, checkboxChecked) => {
    addLog(`[C++ -> WebUI] 收到结果反馈: 用户点击确定 = ${accepted}, 复选框勾选状态 = ${checkboxChecked}`);
  });

  const btnShow = document.getElementById('btn-show');
  if (btnShow) {
    btnShow.addEventListener('click', () => {
      const style = document.getElementById('dialog-style').value;
      const title = document.getElementById('dialog-title').value;
      const bodyText = document.getElementById('dialog-body').value;
      const checkboxText = document.getElementById('dialog-checkbox-text').value;
      const checkboxChecked = document.getElementById('dialog-checkbox-checked').checked;
      const confirmText = document.getElementById('dialog-confirm-text').value;
      const cancelText = document.getElementById('dialog-cancel-text').value;
      const showMask = document.getElementById('dialog-show-mask').checked;

      addLog(`[WebUI -> C++] 发送请求 'showCommonDialog', 参数: style=${style}, show_mask=${showMask}`);
      
      chrome.send('showCommonDialog', [
        style,
        title,
        bodyText,
        checkboxText,
        checkboxChecked,
        cancelText,
        confirmText,
        showMask
      ]);
    });
  }

  const btnShowWeb = document.getElementById('btn-show-web');
  if (btnShowWeb) {
    btnShowWeb.addEventListener('click', () => {
      const options = {
        url: document.getElementById('web-dialog-url').value,
        title: document.getElementById('web-dialog-title').value,
        width: parseInt(document.getElementById('web-dialog-width').value, 10) || 0,
        height: parseInt(document.getElementById('web-dialog-height').value, 10) || 0,
        modal: document.getElementById('web-dialog-modal').checked,
      };
      addLog(`[WebUI -> C++] 发送请求 'showWebDialog', 参数: ${JSON.stringify(options)}`);
      chrome.send('showWebDialog', [options]);
    });
  }

  const shadowPairs = [
    { idPrefix: 'btn-shadow-default', type: 'kDefault' },
    { idPrefix: 'btn-shadow-none', type: 'kNone' },
    { idPrefix: 'btn-shadow-drop', type: 'kDrop' },
  ];

  shadowPairs.forEach(pair => {
    const btnStd = document.getElementById(`${pair.idPrefix}-std`);
    if (btnStd) {
      btnStd.addEventListener('click', () => {
        const showBackdrop = document.getElementById('shadow-show-backdrop').checked;
        addLog(`[WebUI -> C++] 发送请求 'showWidgetShadowSample', 参数: type=${pair.type}, borderless=false, show_backdrop=${showBackdrop}`);
        chrome.send('showWidgetShadowSample', [pair.type, false, showBackdrop]);
      });
    }
    const btnBorderless = document.getElementById(`${pair.idPrefix}-borderless`);
    if (btnBorderless) {
      btnBorderless.addEventListener('click', () => {
        const showBackdrop = document.getElementById('shadow-show-backdrop').checked;
        addLog(`[WebUI -> C++] 发送请求 'showWidgetShadowSample', 参数: type=${pair.type}, borderless=true, show_backdrop=${showBackdrop}`);
        chrome.send('showWidgetShadowSample', [pair.type, true, showBackdrop]);
      });
    }
  });

  const btnShowExtension = document.getElementById('btn-show-extension');
  if (btnShowExtension) {
    btnShowExtension.addEventListener('click', () => {
      addLog(`[WebUI -> C++] 发送请求 'showExtension'`);
      chrome.send('showExtension');
    });
  }

  const btnClearLogs = document.getElementById('btn-clear-logs');
  if (btnClearLogs) {
    btnClearLogs.addEventListener('click', () => {
      const logList = document.getElementById('log-list');
      logList.innerHTML = '<div class="log-item empty-state">暂无日志，点击“触发弹窗”进行测试</div>';
    });
  }
});
