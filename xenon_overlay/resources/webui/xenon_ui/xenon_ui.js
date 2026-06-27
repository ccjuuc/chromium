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

function parseBoxShadow(cssStr) {
  let offsetX = 0;
  let offsetY = 0;
  let blur = 0;
  let spread = 0;
  let colorHex = '#000000';
  let opacity = 1.0;

  if (!cssStr) {
    return { offsetX, offsetY, blur, spread, colorHex, opacity };
  }

  cssStr = cssStr.trim();

  // 1. Extract color first (rgb, rgba, hex)
  const rgbaRegex = /rgba?\([^)]+\)/i;
  const hexRegex = /#[0-9a-f]{3,8}/i;

  let colorPart = '';
  let remainingStr = cssStr;

  const rgbaMatch = cssStr.match(rgbaRegex);
  if (rgbaMatch) {
    colorPart = rgbaMatch[0];
    remainingStr = cssStr.replace(colorPart, ' ');
  } else {
    const hexMatch = cssStr.match(hexRegex);
    if (hexMatch) {
      colorPart = hexMatch[0];
      remainingStr = cssStr.replace(colorPart, ' ');
    }
  }

  // 2. Parse the extracted color
  if (colorPart) {
    colorPart = colorPart.trim();
    if (colorPart.startsWith('#')) {
      if (colorPart.length === 4) {
        colorHex = '#' + colorPart[1] + colorPart[1] + colorPart[2] + colorPart[2] + colorPart[3] + colorPart[3];
        opacity = 1.0;
      } else if (colorPart.length === 7) {
        colorHex = colorPart;
        opacity = 1.0;
      } else if (colorPart.length === 9) {
        colorHex = colorPart.slice(0, 7);
        const alphaHex = colorPart.slice(7, 9);
        opacity = parseInt(alphaHex, 16) / 255;
      } else if (colorPart.length === 5) {
        colorHex = '#' + colorPart[1] + colorPart[1] + colorPart[2] + colorPart[2] + colorPart[3] + colorPart[3];
        const alphaHex = colorPart[4] + colorPart[4];
        opacity = parseInt(alphaHex, 16) / 255;
      }
    } else if (colorPart.toLowerCase().startsWith('rgb')) {
      const parts = colorPart.match(/[\d.]+/g);
      if (parts && parts.length >= 3) {
        const r = parseInt(parts[0], 10);
        const g = parseInt(parts[1], 10);
        const b = parseInt(parts[2], 10);
        const toHex = (x) => {
          const hex = Math.max(0, Math.min(255, x)).toString(16);
          return hex.length === 1 ? '0' + hex : hex;
        };
        colorHex = `#${toHex(r)}${toHex(g)}${toHex(b)}`;
        if (parts.length >= 4) {
          opacity = parseFloat(parts[3]);
        } else {
          opacity = 1.0;
        }
      }
    }
  } else {
    // Look for named colors
    const tokens = remainingStr.trim().split(/\s+/);
    const knownColors = {
      'black': '#000000',
      'white': '#ffffff',
      'red': '#ff0000',
      'green': '#00ff00',
      'blue': '#0000ff',
      'gray': '#808080',
      'grey': '#808080',
      'yellow': '#ffff00',
      'purple': '#800080',
      'silver': '#c0c0c0',
      'maroon': '#800000',
      'olive': '#808000',
      'lime': '#00ff00',
      'teal': '#008080',
      'aqua': '#00ffff',
      'navy': '#000080',
      'fuchsia': '#ff00ff'
    };
    for (let i = 0; i < tokens.length; i++) {
      const tok = tokens[i].toLowerCase();
      if (knownColors[tok]) {
        colorHex = knownColors[tok];
        opacity = 1.0;
        tokens.splice(i, 1);
        remainingStr = tokens.join(' ');
        break;
      }
    }
  }

  // 3. Parse lengths
  const lengthRegex = /(-?\d+(?:\.\d+)?)(px|em|rem|%)?/g;
  const matches = [];
  let match;
  while ((match = lengthRegex.exec(remainingStr)) !== null) {
    matches.push(parseFloat(match[1]));
  }

  if (matches.length >= 1) offsetX = Math.round(matches[0]);
  if (matches.length >= 2) offsetY = Math.round(matches[1]);
  if (matches.length >= 3) blur = Math.round(matches[2]);
  if (matches.length >= 4) spread = Math.round(matches[3]);

  return { offsetX, offsetY, blur, spread, colorHex, opacity };
}

document.addEventListener('DOMContentLoaded', () => {
  addLog('WebUI 页面已初始化，已注册监听器');

  // Register WebUI listener for dialog result
  addWebUiListener('dialog-result', (accepted, checkboxChecked) => {
    addLog(`[C++ -> WebUI] 收到结果反馈: 用户点击确定 = ${accepted}, 复选框勾选状态 = ${checkboxChecked}`);
  });

  addWebUiListener('toast-action', () => {
    addLog('[C++ -> WebUI] Toast 操作按钮被点击');
  });

  addWebUiListener('xenon-menu-command', (commandId) => {
    addLog(`[C++ -> WebUI] XenonMenuRunner 执行菜单项: ${commandId}`);
  });

  function buildToastOptions(overrides = {}) {
    return {
      type: overrides.type ?? document.getElementById('toast-type').value,
      text: overrides.text ?? document.getElementById('toast-text').value,
      action_text: overrides.action_text ??
          document.getElementById('toast-action-text').value,
      duration_ms: overrides.duration_ms ??
          (parseInt(document.getElementById('toast-duration').value, 10) || 0),
    };
  }

  function sendShowToast(options) {
    addLog(`[WebUI -> C++] 发送请求 'showToast', 参数: ${JSON.stringify(options)}`);
    chrome.send('showToast', [options]);
  }

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
        frame: document.getElementById('web-dialog-frame').checked,
        dwm: document.getElementById('web-dialog-dwm').checked,
        resizable: document.getElementById('web-dialog-resizable').checked,
        alwaysOnTop: document.getElementById('web-dialog-always-on-top').checked,
        skipTaskbar: document.getElementById('web-dialog-skip-taskbar').checked,
        minimizable: document.getElementById('web-dialog-minimizable').checked,
        maximizable: document.getElementById('web-dialog-maximizable').checked,
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

  const btnShowViewShadowTest = document.getElementById('btn-show-view-shadow-test');
  if (btnShowViewShadowTest) {
    btnShowViewShadowTest.addEventListener('click', () => {
      addLog("[WebUI -> C++] 发送请求 'showViewShadowTestWindow'");
      chrome.send('showViewShadowTestWindow');
    });
  }

  const btnShowExtension = document.getElementById('btn-show-extension');
  if (btnShowExtension) {
    btnShowExtension.addEventListener('click', () => {
      addLog(`[WebUI -> C++] 发送请求 'showExtension'`);
      chrome.send('showExtension');
    });
  }

  const btnShowToast = document.getElementById('btn-show-toast');
  if (btnShowToast) {
    btnShowToast.addEventListener('click', () => {
      sendShowToast(buildToastOptions());
    });
  }

  const btnToastLoadingSuccess = document.getElementById('btn-toast-loading-success');
  if (btnToastLoadingSuccess) {
    btnToastLoadingSuccess.addEventListener('click', () => {
      sendShowToast({
        type: 'loading',
        text: '正在处理…',
        action_text: '',
        duration_ms: 0,
      });
      window.setTimeout(() => {
        sendShowToast({
          type: 'success',
          text: '处理完成',
          action_text: '',
          duration_ms: 3000,
        });
      }, 1200);
    });
  }

  function getCurrentShadowArgs() {
    const shadowStyle = document.getElementById('menu-shadow-style').value;
    let shadowElevation =
        parseInt(document.getElementById('menu-shadow-elevation').value, 10);
    let shadowOpacity =
        parseFloat(document.getElementById('menu-shadow-opacity').value);
    let shadowOffsetX =
        parseInt(document.getElementById('menu-shadow-offset-x').value, 10);
    let shadowOffsetY =
        parseInt(document.getElementById('menu-shadow-offset-y').value, 10);
    let shadowSpread =
        parseInt(document.getElementById('menu-shadow-spread').value, 10);
    let shadowColor = document.getElementById('menu-shadow-color').value;

    if (!Number.isFinite(shadowElevation)) shadowElevation = 12;
    if (!Number.isFinite(shadowOpacity)) shadowOpacity = 0.2;
    if (!Number.isFinite(shadowOffsetX)) shadowOffsetX = 0;
    if (!Number.isFinite(shadowOffsetY)) shadowOffsetY = 0;
    if (!Number.isFinite(shadowSpread)) shadowSpread = 0;

    if (shadowStyle === 'kBoxShadow') {
      const cssStr = document.getElementById('menu-shadow-css').value;
      const parsed = parseBoxShadow(cssStr);
      shadowElevation = parsed.blur;
      shadowOpacity = parsed.opacity;
      shadowOffsetX = parsed.offsetX;
      shadowOffsetY = parsed.offsetY;
      shadowSpread = parsed.spread;
      shadowColor = parsed.colorHex;
    }

    return {
      style: shadowStyle,
      elevation: shadowElevation,
      opacity: shadowOpacity,
      offsetX: shadowOffsetX,
      offsetY: shadowOffsetY,
      spread: shadowSpread,
      color: shadowColor,
      args: [
        shadowStyle,
        shadowElevation,
        shadowOpacity,
        shadowOffsetX,
        shadowOffsetY,
        shadowSpread,
        shadowColor,
      ],
    };
  }

  function addShadowRequestLog(messageName, shadow) {
    addLog(`[WebUI -> C++] 发送请求 '${messageName}', 参数: style=${shadow.style}, elevation=${shadow.elevation}, opacity=${shadow.opacity}, offset_x=${shadow.offsetX}, offset_y=${shadow.offsetY}, spread=${shadow.spread}, color=${shadow.color}`);
  }

  const btnShowXenonMenu = document.getElementById('btn-show-xenon-menu');
  if (btnShowXenonMenu) {
    btnShowXenonMenu.addEventListener('click', () => {
      const shadow = getCurrentShadowArgs();
      addShadowRequestLog('showXenonMenuRunner', shadow);
      chrome.send('showXenonMenuRunner', shadow.args);
    });
  }

  // Helper to sync CSS box-shadow string from color picker and opacity slider
  function updateCssStringFromIndividualInputs() {
    const cssInput = document.getElementById('menu-shadow-css');
    if (!cssInput) return;

    const parsed = parseBoxShadow(cssInput.value);
    const colorHex = document.getElementById('menu-shadow-color').value;
    const opacityInput = document.getElementById('menu-shadow-opacity');
    const opacity = opacityInput ? parseFloat(opacityInput.value) : 1.0;

    const r = parseInt(colorHex.slice(1, 3), 16);
    const g = parseInt(colorHex.slice(3, 5), 16);
    const b = parseInt(colorHex.slice(5, 7), 16);
    const rgbaStr = `rgba(${r}, ${g}, ${b}, ${opacity})`;

    cssInput.value = `${parsed.offsetX}px ${parsed.offsetY}px ${parsed.blur}px ${parsed.spread}px ${rgbaStr}`;
  }

  // Helper to sync color picker and opacity slider from CSS box-shadow string
  function updateIndividualInputsFromCssString() {
    const cssInput = document.getElementById('menu-shadow-css');
    if (!cssInput) return;

    const parsed = parseBoxShadow(cssInput.value);
    const colorPicker = document.getElementById('menu-shadow-color');
    if (colorPicker) {
      colorPicker.value = parsed.colorHex;
    }
    const opacityInput = document.getElementById('menu-shadow-opacity');
    if (opacityInput) {
      opacityInput.value = parsed.opacity;
    }
  }

  // Handle visibility of shadow inputs based on style selection
  const shadowStyleSelect = document.getElementById('menu-shadow-style');
  const cssShadowGroup = document.getElementById('css-shadow-group');
  if (shadowStyleSelect && cssShadowGroup) {
    const updateShadowInputsVisibility = () => {
      const style = shadowStyleSelect.value;
      const isNone = style === 'kNone';
      const isBoxShadow = style === 'kBoxShadow';

      const elevationGroup = document.getElementById('menu-shadow-elevation')?.closest('.form-group');
      const opacityGroup = document.getElementById('menu-shadow-opacity')?.closest('.form-group');
      const offsetXGroup = document.getElementById('menu-shadow-offset-x')?.closest('.form-group');
      const offsetYGroup = document.getElementById('menu-shadow-offset-y')?.closest('.form-group');
      const spreadGroup = document.getElementById('menu-shadow-spread')?.closest('.form-group');
      const colorGroup = document.getElementById('menu-shadow-color')?.closest('.form-group');

      if (isNone) {
        cssShadowGroup.style.display = 'none';
        if (elevationGroup) elevationGroup.style.display = 'none';
        if (opacityGroup) opacityGroup.style.display = 'none';
        if (offsetXGroup) offsetXGroup.style.display = 'none';
        if (offsetYGroup) offsetYGroup.style.display = 'none';
        if (spreadGroup) spreadGroup.style.display = 'none';
        if (colorGroup) colorGroup.style.display = 'none';
      } else if (isBoxShadow) {
        cssShadowGroup.style.display = '';
        if (elevationGroup) elevationGroup.style.display = 'none';
        if (opacityGroup) opacityGroup.style.display = '';
        if (offsetXGroup) offsetXGroup.style.display = 'none';
        if (offsetYGroup) offsetYGroup.style.display = 'none';
        if (spreadGroup) spreadGroup.style.display = 'none';
        if (colorGroup) colorGroup.style.display = '';
      } else {
        cssShadowGroup.style.display = 'none';
        if (elevationGroup) elevationGroup.style.display = '';
        if (opacityGroup) opacityGroup.style.display = '';
        if (offsetXGroup) offsetXGroup.style.display = '';
        if (offsetYGroup) offsetYGroup.style.display = '';
        if (spreadGroup) spreadGroup.style.display = '';
        if (colorGroup) colorGroup.style.display = '';
      }
    };

    shadowStyleSelect.addEventListener('change', updateShadowInputsVisibility);
    // Initial sync
    updateShadowInputsVisibility();
  }

  // Setup synchronization listeners
  const cssInput = document.getElementById('menu-shadow-css');
  if (cssInput) {
    cssInput.addEventListener('input', updateIndividualInputsFromCssString);
  }

  const colorInput = document.getElementById('menu-shadow-color');
  if (colorInput) {
    colorInput.addEventListener('input', () => {
      if (shadowStyleSelect && shadowStyleSelect.value === 'kBoxShadow') {
        updateCssStringFromIndividualInputs();
      }
    });
  }

  const opacityInput = document.getElementById('menu-shadow-opacity');
  if (opacityInput) {
    opacityInput.addEventListener('input', () => {
      if (shadowStyleSelect && shadowStyleSelect.value === 'kBoxShadow') {
        updateCssStringFromIndividualInputs();
      }
    });
  }

  const btnColorGreen = document.getElementById('btn-color-preset-green');
  if (btnColorGreen) {
    btnColorGreen.addEventListener('click', () => {
      document.getElementById('menu-shadow-color').value = '#0f522e';
      addLog('已将阴影颜色快捷设置为：暗绿色 (#0f522e)');
      if (shadowStyleSelect && shadowStyleSelect.value === 'kBoxShadow') {
        updateCssStringFromIndividualInputs();
      }
    });
  }

  const btnColorBlack = document.getElementById('btn-color-preset-black');
  if (btnColorBlack) {
    btnColorBlack.addEventListener('click', () => {
      document.getElementById('menu-shadow-color').value = '#000000';
      addLog('已将阴影颜色快捷设置为：黑色 (#000000)');
      if (shadowStyleSelect && shadowStyleSelect.value === 'kBoxShadow') {
        updateCssStringFromIndividualInputs();
      }
    });
  }

  const btnShowXenonBubble = document.getElementById('btn-show-xenon-bubble');
  if (btnShowXenonBubble) {
    btnShowXenonBubble.addEventListener('click', () => {
      const shadow = getCurrentShadowArgs();
      addShadowRequestLog('showXenonCommonBubble', shadow);
      chrome.send('showXenonCommonBubble', shadow.args);
    });
  }

  const btnShowXenonWebUiBubble = document.getElementById(
      'btn-show-xenon-webui-bubble');
  if (btnShowXenonWebUiBubble) {
    btnShowXenonWebUiBubble.addEventListener('click', () => {
      const shadow = getCurrentShadowArgs();
      addShadowRequestLog('showXenonWebUIBubble', shadow);
      chrome.send('showXenonWebUIBubble', shadow.args);
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
