// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

'use strict';

const content = document.querySelector('#content');
const states = {
  enabled: '此扩展已安装。', disabled: '此扩展已安装，但已停用。',
  terminated: '此扩展已安装，请在扩展管理页面重新加载。',
  blacklisted: '浏览器已禁止此扩展。', blocked_by_policy: '管理员策略禁止安装此扩展。',
  can_request: '此扩展需要向管理员申请。', request_pending: '正在等待管理员批准。',
  custodian_approval_required: '此扩展需要家长批准。',
  custodian_approval_required_for_installation: '安装此扩展需要家长批准。',
  force_installed: '此扩展由管理员安装。', corrupted: '此扩展已安装，但需要修复。',
  deprecated_manifest_version: '此扩展使用浏览器不再支持的 Manifest 版本。',
};

function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text) node.textContent = text;
  if (className) node.className = className;
  return node;
}

async function getJson(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`页面加载失败 (${response.status})`);
  return response.json();
}

function callApi(namespace, method, ...args) {
  return new Promise((resolve, reject) => {
    try {
      chrome[namespace][method](...args, result => {
        const error = chrome.runtime.lastError;
        if (error) reject(new Error(error.message));
        else resolve(result);
      });
    } catch (error) {
      reject(error);
    }
  });
}

function summary(item) {
  const article = element('article');
  if (item.iconUrl) {
    const icon = element('img');
    icon.src = item.iconUrl;
    icon.alt = '';
    article.append(icon);
  }
  const title = element('h2');
  const link = element('a', item.name);
  link.href = item.detailUrl;
  title.append(link);
  article.append(title, element('p', item.description),
    element('p', `版本 ${item.version}`, 'muted'));
  return article;
}

async function showDetail(id) {
  const item = await getJson(`/api/items/${id}`);
  content.replaceChildren(element('h1', item.name));
  const article = summary(item);
  const button = element('button', '正在检查…');
  button.disabled = true;
  const status = element('p', '正在检查浏览器安装条件…');
  status.setAttribute('role', 'status');
  let action = null;
  let busy = false;
  let queryVersion = 0;
  const showMessage = (message, isError = false) => {
    status.className = isError ? 'error' : '';
    status.textContent = message;
  };
  const fail = error => {
    action = null;
    button.disabled = true;
    button.textContent = '暂时无法操作';
    showMessage(error.message, true);
  };
  article.append(button, status, element('p', `扩展 ID：${id}`, 'muted'));
  content.append(article);
  if (!globalThis.chrome?.webstorePrivate || !chrome.runtime ||
      !chrome.management?.get || !chrome.management?.uninstall) {
    fail(new Error('当前浏览器未向此商店提供扩展安装和卸载接口。'));
    return;
  }

  async function installedExtension() {
    try {
      return await callApi('management', 'get', id);
    } catch (error) {
      if (error.message === `Failed to find extension with id ${id}.`) return null;
      throw error;
    }
  }

  async function refreshState(message, isError = false) {
    if (busy) return;
    const version = ++queryVersion;
    button.disabled = true;
    try {
      const [current, installed] = await Promise.all([
        callApi('webstorePrivate', 'getExtensionStatus', id, item.manifest),
        installedExtension(),
      ]);
      if (busy || version !== queryVersion) return;
      action = null;
      if (installed) {
        button.textContent = '卸载';
        if (installed.mayDisable === false) {
          showMessage('此扩展由管理员管理，无法卸载。');
        } else {
          action = 'uninstall';
          button.disabled = false;
          showMessage(installed.enabled ? states.enabled :
            (states[current] || states.disabled));
        }
      } else if (current === 'installable') {
        action = 'install';
        button.textContent = '添加至 Chrome';
        button.disabled = false;
        showMessage('');
      } else {
        button.textContent = '暂时无法添加';
        showMessage(states[current] || `当前无法安装 (${current})`);
      }
      if (message !== undefined) showMessage(message, isError);
    } catch (error) {
      if (!busy && version === queryVersion) fail(error);
    }
  }

  try {
    if (await callApi('webstorePrivate', 'isInIncognitoMode')) {
      fail(new Error('请在普通浏览窗口中安装或卸载扩展。'));
      return;
    }
    button.addEventListener('click', async () => {
      if (busy || !action) return;
      const requestedAction = action;
      busy = true;
      ++queryVersion;
      button.disabled = true;
      button.textContent = '等待确认…';
      let message;
      let isError = false;
      try {
        if (requestedAction === 'uninstall') {
          showMessage('请在浏览器弹窗中确认卸载。');
          // Invoke during the click's user gesture; the browser owns confirmation.
          await callApi('management', 'uninstall', id, {showConfirmDialog: true});
          message = '卸载成功。';
        } else {
          showMessage('请在浏览器弹窗中确认扩展权限。');
          const options = {id, manifest: item.manifest};
          if (item.iconUrl) options.iconUrl = item.iconUrl;
          const result = await callApi('webstorePrivate', 'beginInstallWithManifest3', options);
          if (result !== '') throw new Error(`浏览器未批准安装 (${result})`);
          button.textContent = '正在安装…';
          showMessage('正在下载并安装扩展…');
          await callApi('webstorePrivate', 'completeInstall', id);
          message = '安装成功。';
        }
      } catch (error) {
        const canceled = error.message === `Extension ${id} uninstall canceled by user.`;
        message = canceled ? '已取消卸载。' : error.message;
        isError = !canceled;
      } finally {
        busy = false;
        await refreshState(message, isError);
      }
    });
    const onExtensionChanged = extension => {
      if ((typeof extension === 'string' ? extension : extension.id) === id) {
        void refreshState();
      }
    };
    for (const name of ['onInstalled', 'onUninstalled', 'onEnabled', 'onDisabled']) {
      chrome.management[name].addListener(onExtensionChanged);
    }
    window.addEventListener('focus', () => void refreshState());
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) void refreshState();
    });
    await refreshState();
  } catch (error) {
    fail(error);
  }
}

async function main() {
  const detail = /^\/detail\/([a-p]{32})$/.exec(location.pathname);
  if (detail) return showDetail(detail[1]);
  const items = await getJson('/api/items');
  content.replaceChildren(element('h1', '扩展'));
  if (!items.length) content.append(element('p', '暂无扩展。'));
  else content.append(...items.map(summary));
}

main().catch(error => content.replaceChildren(element('p', error.message, 'error')));
