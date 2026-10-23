// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Same-origin local administration. All server-provided text is rendered as text.
(() => {
  'use strict';

  const byId = id => document.getElementById(id);
  const platforms = {
    mac: { label: 'macOS', prefix: '', route: '/admin', extension: /\.pkg$/i, format: 'macOS .pkg',
      configKeys: ['key_path', 'key_hash', 'tools_dir', 'python', 'openssl'] },
    win: { label: 'Windows', prefix: 'windows-', route: '/admin/windows', extension: /\.(exe|zip)$/i,
      format: 'Windows .exe 或安装目录 .zip',
      configKeys: ['node', 'seven_zip', 'zucchini', 'default_arch', 'changelog', 'force_update'] },
  };
  const element = (platform, name) => byId(platforms[platform].prefix + name);
  const configInput = (platform, key) => element(platform, `config-${key.replaceAll('_', '-')}`);
  let selectedPlatform = 'mac';

  function selectPlatform(platform, focus = false) {
    selectedPlatform = platform;
    for (const name of Object.keys(platforms)) {
      const id = name === 'win' ? 'windows' : 'mac';
      const tab = byId(`${id}-tab`);
      const selected = name === platform;
      tab.setAttribute('aria-selected', String(selected));
      tab.tabIndex = selected ? 0 : -1;
      byId(`${id}-panel`).hidden = !selected;
      if (selected && focus) tab.focus();
    }
    const fragment = platform === 'win' ? '#windows' : '#mac';
    byId('page-refresh').href = `/${fragment}`;
    if (location.hash !== fragment) history.replaceState(null, '', fragment);
  }

  for (const name of Object.keys(platforms)) {
    const tab = byId(`${name === 'win' ? 'windows' : 'mac'}-tab`);
    tab.addEventListener('click', () => selectPlatform(name));
    tab.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      event.preventDefault();
      selectPlatform(event.key === 'Home' ? 'mac' : event.key === 'End' ? 'win'
        : selectedPlatform === 'mac' ? 'win' : 'mac', true);
    });
  }
  selectPlatform(location.hash === '#windows' ? 'win' : 'mac');
  window.addEventListener('hashchange', () => selectPlatform(location.hash === '#windows' ? 'win' : 'mac'));

  const token = document.querySelector('meta[name="xenon-admin-token"]')?.content ?? '';
  const enabled = document.body.dataset.adminEnabled === 'true';
  const readOnly = byId('read-only-message');
  function showReadOnly() {
    for (const platform of Object.keys(platforms)) element(platform, 'admin-workspace').hidden = true;
    readOnly.hidden = false;
    byId('windows-read-only-message').hidden = false;
  }
  if (!enabled || !token) { showReadOnly(); return; }
  for (const platform of Object.keys(platforms)) element(platform, 'admin-workspace').hidden = false;

  let currentState = null;
  const configLoaded = new Set();
  const lastJobs = { mac: null, win: null };
  let submitting = null;
  let saving = null;
  let loading = null;
  let pollTimer = null;
  let disposed = false;

  const labels = { queued: '等待处理', running: '处理中', succeeded: '发布完成', failed: '发布失败' };
  const stages = {
    queued: '任务已加入队列。', downloading: '正在下载安装包…', uploading: '正在上传安装包…',
    extracting: '正在解包并识别版本…', packaging: '正在生成更新包…',
    generating_deltas: '正在生成历史版本差分并回放校验…', publishing: '正在发布更新…',
    completed: '处理完成，可刷新当前版本查看结果。',
  };

  function notice(id, message, kind = 'error') {
    const target = byId(id);
    target.textContent = message;
    target.dataset.kind = kind;
    target.hidden = !message;
  }
  function platformNotice(platform, name, message, kind) {
    notice(platforms[platform].prefix + name, message, kind);
  }
  function busy() { return submitting || saving || Boolean(currentState?.busy); }
  function platformConfig(platform) { return platform === 'win' ? currentState?.windows?.config : currentState?.config; }

  function updateControls() {
    const blocked = Boolean(busy() || !currentState);
    const active = platforms[currentState?.job?.platform ?? 'mac']?.label ?? '发布';
    for (const platform of Object.keys(platforms)) {
      const unavailable = platform === 'win' && !currentState?.windows;
      for (const name of ['import-fields', 'config-fields', 'publish-button', 'save-config-button']) {
        element(platform, name).disabled = blocked || unavailable;
      }
      element(platform, 'publish-button').textContent = submitting === platform ? '正在提交…'
        : currentState?.busy ? `正在处理 ${active} 任务…` : !currentState ? '正在读取配置…'
        : unavailable ? 'Windows 发布暂不可用' : platform === 'mac' && !platformConfig(platform)?.key_path
          ? '先配置发布签名' : '开始发布';
      element(platform, 'save-config-button').textContent = saving === platform ? '正在保存…' : '保存发布配置';
    }
  }

  async function api(path, options = {}) {
    const headers = new Headers(options.headers);
    headers.set('X-Xenon-Admin', token);
    const response = await fetch(path, { ...options, headers, credentials: 'same-origin', cache: 'no-store' });
    let value;
    try { value = await response.json(); } catch { throw new Error(`服务返回了无法读取的响应（HTTP ${response.status}）。`); }
    if (!response.ok) throw new Error(typeof value.error === 'string' ? value.error : `请求失败（HTTP ${response.status}）。`);
    return value;
  }

  function emptyRow(rows, columns, message) {
    const row = document.createElement('tr');
    const cell = document.createElement('td');
    cell.colSpan = columns;
    cell.textContent = message;
    row.append(cell);
    rows.append(row);
  }
  function appendCells(row, values) {
    for (const value of values) {
      const cell = document.createElement('td');
      cell.textContent = typeof value === 'string' || typeof value === 'number' ? value : '—';
      row.append(cell);
    }
  }
  function renderHistory(platform, historyEntries) {
    const rows = element(platform, 'history-rows');
    rows.replaceChildren();
    const entries = Array.isArray(historyEntries) ? historyEntries : [];
    element(platform, 'history-count').textContent = `${entries.length} 个版本记录`;
    if (!entries.length) { emptyRow(rows, 3, '还没有历史版本。导入安装包后，版本会保留在这里。'); return; }
    for (const entry of entries) {
      const row = document.createElement('tr');
      appendCells(row, [entry.version, platforms[entry.platform]?.label ?? platforms[platform].label, entry.arch]);
      rows.append(row);
    }
  }

  function fileSize(bytes) {
    if (!Number.isFinite(bytes) || bytes < 0) return '—';
    if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(1)} GiB`;
    if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
    if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
    return `${bytes} B`;
  }
  function renderWindowsReleases(releases) {
    const entries = Array.isArray(releases) ? releases : [];
    const rows = byId('windows-release-rows');
    rows.replaceChildren();
    let diffs = 0;
    byId('windows-release-count').textContent = String(entries.length);
    for (const entry of entries) {
      const usable = (Array.isArray(entry.differential) ? entry.differential : [])
        .filter(diff => diff.size > 0 && diff.size < entry.full?.size);
      diffs += usable.length;
      const row = document.createElement('tr');
      appendCells(row, [entry.version, entry.arch, fileSize(entry.full?.size), usable.length ? `${usable.length} 个补丁` : '全量升级']);
      const cell = document.createElement('td');
      const file = entry.full?.file;
      if (typeof file === 'string' && /^[a-zA-Z0-9][a-zA-Z0-9._-]*\.zip$/i.test(file)) {
        const link = document.createElement('a');
        link.href = `/windows/artifacts/${encodeURIComponent(file)}`;
        link.download = file;
        link.textContent = '下载 ZIP';
        cell.append(link);
      } else cell.textContent = '—';
      row.append(cell);
      rows.append(row);
    }
    byId('windows-diff-count').textContent = String(diffs);
    if (!entries.length) emptyRow(rows, 5, '还没有发布版本。导入 Windows 安装包后会显示在这里。');
  }

  function renderJob(platform, job) {
    const status = element(platform, 'job-status');
    const warnings = element(platform, 'job-warnings');
    warnings.replaceChildren();
    element(platform, 'job-error').hidden = true;
    element(platform, 'job-result').hidden = true;
    element(platform, 'job-refresh').hidden = true;
    warnings.hidden = true;
    if (!job) {
      status.textContent = '暂无任务';
      status.dataset.status = '';
      element(platform, 'job-stage').textContent = '选择安装包后开始发布。处理期间可在这里查看进度。';
      return;
    }
    status.textContent = labels[job.status] ?? '任务处理中';
    status.dataset.status = String(job.status ?? '');
    element(platform, 'job-stage').textContent = typeof job.stage === 'string' ? (stages[job.stage] ?? job.stage) : '';
    if (job.error) {
      element(platform, 'job-error').textContent = String(job.error);
      element(platform, 'job-error').hidden = false;
    }
    if (job.result && typeof job.result === 'object') {
      element(platform, 'job-result-text').textContent = JSON.stringify(job.result, null, 2);
      element(platform, 'job-result').hidden = false;
      const items = Array.isArray(job.result.warnings) ? job.result.warnings : [];
      for (const item of items) {
        const li = document.createElement('li');
        li.textContent = typeof item === 'string' ? item : JSON.stringify(item);
        warnings.append(li);
      }
      warnings.hidden = items.length === 0;
    }
    element(platform, 'job-refresh').hidden = job.status !== 'succeeded';
  }

  function updateJobs(job) {
    if (job) {
      const platform = job.platform ?? 'mac';
      if (Object.hasOwn(platforms, platform)) lastJobs[platform] = job;
    }
    for (const platform of Object.keys(platforms)) renderJob(platform, lastJobs[platform]);
  }

  function renderState(state, resetPlatform = null) {
    if (state.enabled === false) { showReadOnly(); disposed = true; return; }
    currentState = state;
    const host = ({ win: 'Windows', windows: 'Windows', mac: 'macOS', macos: 'macOS', darwin: 'macOS', linux: 'Linux' })[state.config?.host] ?? state.config?.host ?? '未知';
    for (const platform of Object.keys(platforms)) {
      const config = platformConfig(platform) ?? {};
      // A poll or a save on the other platform must never overwrite form drafts.
      if (!configLoaded.has(platform) || resetPlatform === platform) {
        for (const key of platforms[platform].configKeys) {
          const input = configInput(platform, key);
          if (key === 'force_update') input.checked = config[key] === true;
          else input.value = typeof config[key] === 'string' ? config[key] : key === 'default_arch' ? 'x86_64' : '';
        }
        configLoaded.add(platform);
      }
      element(platform, 'host-hint').textContent = `当前制包主机：${host}`;
      element(platform, 'config-host').textContent = `当前主机：${host}`;
      if (Number.isFinite(state.max_upload_bytes) && state.max_upload_bytes > 0) {
        element(platform, 'upload-hint').textContent = `支持 ${platforms[platform].format}，单个文件上限 ${fileSize(state.max_upload_bytes)}。`;
      }
    }
    renderHistory('mac', state.history);
    renderHistory('win', state.windows?.history);
    renderWindowsReleases(state.windows?.releases);
    if (typeof state.windows?.endpoint === 'string') byId('windows-endpoint').textContent = state.windows.endpoint;
    byId('windows-endpoint-hint').textContent = `客户端未指定 arch 时使用 ${state.windows?.config?.default_arch ?? 'x86_64'}；可在更新地址添加 ?arch=x86_64、?arch=arm64 或 ?arch=x86。`;
    updateJobs(state.job);
    updateControls();
  }

  function schedulePoll(milliseconds = 2000) {
    clearTimeout(pollTimer);
    if (!disposed) pollTimer = setTimeout(() => loadState(), milliseconds);
  }
  async function loadState(resetPlatform = null) {
    if (disposed || (!resetPlatform && (submitting || saving))) return;
    if (loading) return loading;
    loading = (async () => {
      try {
        const state = await api('/admin/state');
        renderState(state, resetPlatform);
        notice('service-notice', '');
        if (state.busy || ['queued', 'running'].includes(state.job?.status)) schedulePoll();
      } catch (error) {
        notice('service-notice', `无法读取服务状态：${error.message}`);
        schedulePoll(5000);
      } finally {
        updateControls();
      }
    })();
    try { await loading; } finally { loading = null; }
  }

  function sourceType(platform) {
    return document.querySelector(`input[name="${platforms[platform].prefix}source-type"]:checked`)?.value ?? 'upload';
  }
  function selectSource(platform) {
    const selected = sourceType(platform);
    for (const type of ['upload', 'url', 'path']) element(platform, `${type}-field`).hidden = type !== selected;
    element(platform, 'package-url').disabled = selected !== 'url';
    element(platform, 'package-path').disabled = selected !== 'path';
    element(platform, 'package-file').disabled = selected !== 'upload';
  }

  for (const [platform, details] of Object.entries(platforms)) {
    for (const input of document.querySelectorAll(`input[name="${details.prefix}source-type"]`)) {
      input.addEventListener('change', () => selectSource(platform));
    }
    selectSource(platform);

    element(platform, 'import-form').addEventListener('submit', async event => {
      event.preventDefault();
      if (busy() || !currentState || (platform === 'win' && !currentState.windows)) return;
      platformNotice(platform, 'import-notice', '');
      if (platform === 'mac' && !currentState.config?.key_path) {
        element(platform, 'settings-panel').open = true;
        configInput(platform, 'key_path').focus();
        platformNotice(platform, 'config-notice', '请先填写发布签名私钥在服务器上的路径并保存配置。', 'info');
        return;
      }
      const type = sourceType(platform);
      const version = element(platform, 'package-version').value.trim();
      const arch = element(platform, 'package-arch').value;
      const extra = { ...(version ? { version } : {}), ...(arch ? { arch } : {}) };
      let options;
      let route;
      try {
        const pattern = platform === 'win' ? /^(0|[1-9]\d*)(\.(0|[1-9]\d*)){3}$/ : /^(0|[1-9]\d*)(\.(0|[1-9]\d*)){0,3}$/;
        if (version && (!pattern.test(version) || version.length > 43 || version.split('.').some(part => Number(part) > 0xffffffff))) {
          throw new Error(`版本号应为${platform === 'win' ? '四段' : ' 1–4 段'}数字，例如 1.0.0.2。`);
        }
        if (type === 'upload') {
          const file = element(platform, 'package-file').files[0];
          if (!file || !details.extension.test(file.name)) throw new Error(`请选择 ${details.format} 安装包。`);
          if (!file.size) throw new Error('安装包不能为空文件。');
          if (currentState.max_upload_bytes && file.size > currentState.max_upload_bytes) throw new Error('安装包超过当前服务允许的上传大小。');
          const data = new FormData();
          data.append('file', file);
          for (const [key, value] of Object.entries(extra)) data.append(key, value);
          options = { method: 'POST', body: data };
          route = `${details.route}/upload`;
        } else {
          const source = element(platform, type === 'url' ? 'package-url' : 'package-path').value.trim();
          if (!source) throw new Error(type === 'url' ? '请填写安装包下载链接。' : '请填写服务器上的安装包完整路径。');
          let sourcePath = source;
          if (type === 'url') {
            let parsed;
            try { parsed = new URL(source); } catch { throw new Error('请填写完整的 HTTP 或 HTTPS 下载链接。'); }
            const loopback = ['localhost', '127.0.0.1', '[::1]'].includes(parsed.hostname.toLowerCase());
            if (parsed.username || parsed.password || parsed.hash || (parsed.protocol !== 'https:' && !(parsed.protocol === 'http:' && loopback))) {
              throw new Error('下载链接需要使用 HTTPS，且不能包含账号或片段；仅本机回环地址允许 HTTP。');
            }
            sourcePath = parsed.pathname;
          }
          if (!details.extension.test(sourcePath)) throw new Error(`安装包路径需要以 ${platform === 'win' ? '.exe 或 .zip' : '.pkg'} 结尾。`);
          options = { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ source_type: type, source, ...extra }) };
          route = `${details.route}/import`;
        }
        submitting = platform;
        updateControls();
        platformNotice(platform, 'import-notice', type === 'upload' ? '正在上传安装包，请保持页面打开…' : '正在提交发布任务…', 'info');
        // Finish a pre-existing poll before changing state so it cannot undo the queued job.
        if (loading) await loading;
        const result = await api(route, options);
        currentState = { ...currentState, busy: true, job: result.job };
        updateJobs(result.job);
        platformNotice(platform, 'import-notice', '任务已提交，发布进度将在右侧更新。', 'success');
        schedulePoll(200);
      } catch (error) {
        platformNotice(platform, 'import-notice', error.message);
        if (submitting) schedulePoll(500);
      } finally {
        submitting = null;
        updateControls();
      }
    });

    element(platform, 'config-form').addEventListener('submit', async event => {
      event.preventDefault();
      if (busy() || !currentState || (platform === 'win' && !currentState.windows)) return;
      const config = Object.fromEntries(details.configKeys.map(key => {
        const input = configInput(platform, key);
        return [key, key === 'force_update' ? input.checked : ['default_arch', 'changelog'].includes(key) ? input.value.trim() : input.value.trim() || null];
      }));
      // These optional macOS flags are managed outside this form. Preserve them.
      if (platform === 'mac') {
        config.team_id = currentState.config?.team_id ?? null;
        config.development = currentState.config?.development === true;
      }
      saving = platform;
      updateControls();
      platformNotice(platform, 'config-notice', '正在保存配置…', 'info');
      try {
        if (loading) await loading;
        await api(`${details.route}/config`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(config) });
        await loadState(platform);
        platformNotice(platform, 'config-notice', `${details.label} 发布配置已保存。`, 'success');
      } catch (error) {
        platformNotice(platform, 'config-notice', error.message);
      } finally {
        saving = null;
        updateControls();
      }
    });
  }

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !disposed) loadState();
  });
  window.addEventListener('pagehide', () => { disposed = true; clearTimeout(pollTimer); });
  window.addEventListener('pageshow', event => { if (event.persisted) { disposed = false; loadState(); } });
  loadState();
})();
