'use strict';

const IDS = ['xl1', 'xl2', 'xl3', 'xr1'];
const statusEl = document.getElementById('status');

function readForm() {
  /** @type {Record<string, boolean>} */
  const config = {};
  for (const id of IDS) {
    config[id] = /** @type {HTMLInputElement} */ (document.getElementById(id))
      .checked;
  }
  return config;
}

function writeForm(config) {
  for (const id of IDS) {
    /** @type {HTMLInputElement} */ (document.getElementById(id)).checked =
      Boolean(config[id]);
  }
}

async function load() {
  const res = await chrome.runtime.sendMessage({
    source: 'xl-video-crx-sw',
    cmd: 'getButtonConfig',
  });
  if (res?.ok) {
    writeForm(res.config);
  }
}

document.getElementById('save').addEventListener('click', async () => {
  statusEl.textContent = '保存中…';
  const res = await chrome.runtime.sendMessage({
    source: 'xl-video-crx-sw',
    cmd: 'setButtonConfig',
    config: readForm(),
  });
  statusEl.textContent = res?.ok ? '已保存' : `失败: ${res?.error ?? 'unknown'}`;
});

load();
