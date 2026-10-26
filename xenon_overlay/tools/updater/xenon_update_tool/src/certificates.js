// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Same-origin local administration; server-provided values are always text.
(() => {
  'use strict';

  const byId = id => document.getElementById(id);
  const token = document.querySelector('meta[name="xenon-admin-token"]')?.content ?? '';
  const enabled = document.body.dataset.adminEnabled === 'true';
  if (!enabled || !token) {
    byId('read-only-message').hidden = false;
    byId('snapshot-status').textContent = '仅限本机管理';
    byId('snapshot-status').dataset.status = 'disabled';
    byId('snapshot-summary').textContent = '请在服务所在电脑打开本机管理页面，查看下发状态。';
    return;
  }
  byId('admin-workspace').hidden = false;

  let state = null;
  let initialized = false;
  let loading = false;
  let submitting = false;
  let catalogSignature = null;
  const selectedFiles = () => [...byId('certificate-catalog').querySelectorAll('input:checked')].map(input => input.value);
  const withdrawal = () => byId('mode-empty').checked;

  function notice(id, message, kind = 'error') {
    const target = byId(id);
    target.textContent = message;
    target.dataset.kind = kind;
    target.hidden = !message;
  }

  function dateText(value) {
    const date = new Date(value);
    if (!value || !Number.isFinite(date.getTime())) return value || '—';
    return date.toLocaleString('zh-CN', { hour12: false });
  }

  function setStatus(kind, title, summary) {
    byId('snapshot-status').dataset.status = kind;
    byId('snapshot-status').textContent = title;
    byId('snapshot-summary').textContent = summary;
  }

  async function api(path, options = {}) {
    const headers = new Headers(options.headers);
    headers.set('X-Xenon-Admin', token);
    const response = await fetch(path, { ...options, headers, credentials: 'same-origin', cache: 'no-store' });
    let result;
    try { result = await response.json(); }
    catch { throw new Error(`无法读取服务响应（HTTP ${response.status}）。`); }
    if (!response.ok) throw new Error(typeof result.error === 'string' ? result.error : `请求失败（HTTP ${response.status}）。`);
    return result;
  }

  function updateControls() {
    const blocked = loading || submitting || !state?.can_publish;
    const empty = withdrawal();
    const count = selectedFiles().length;
    byId('publish-fields').disabled = blocked;
    byId('catalog-fields').disabled = blocked || empty;
    byId('select-all').disabled = blocked || empty;
    byId('clear-selection').disabled = blocked || empty;
    byId('refresh-state').disabled = loading || submitting;
    byId('refresh-state').textContent = loading ? '正在刷新…' : '刷新状态';
    byId('source-id').readOnly = Boolean(state?.source_id);
    byId('publish-button').disabled = blocked || (!empty && count === 0);
    byId('publish-button').textContent = submitting ? '正在发布…' : empty ? '发布撤回版本' : '发布所选证书';
    byId('publish-button').classList.toggle('danger', empty);
    byId('withdrawal-notice').hidden = !empty;
    byId('selection-count').textContent = `已选择 ${count} 张`;
    byId('publish-selection-summary').textContent = empty ? '发布 0 张证书' : count ? `下发 ${count} 张证书` : '请先选择证书';
    byId('publish-availability').textContent = !state ? '正在读取服务配置。'
      : !state.enabled ? '证书下发未启用，请先配置服务端 managed_certificates。'
      : !state.can_publish ? '当前状态无法发布，请查看上方提示。' : '仅发布证书集合；浏览器是否获取成功，请在客户端验证。';
  }

  function addDetail(list, name, value) {
    const term = document.createElement('dt');
    term.textContent = name;
    const description = document.createElement('dd');
    description.textContent = value || '—';
    list.append(term, description);
  }

  function renderCatalog(catalog, resetSelection) {
    const signature = JSON.stringify(catalog);
    const selected = resetSelection
      ? new Set((state.published_snapshot?.certificates ?? []).map(certificate => certificate.file).filter(Boolean))
      : new Set(selectedFiles());
    if (catalogSignature !== signature) {
      catalogSignature = signature;
      const fragment = document.createDocumentFragment();
      for (const [index, certificate] of catalog.entries()) {
        const row = document.createElement('article');
        row.className = 'certificate';
        const label = document.createElement('label');
        label.className = 'certificate-label';
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.id = `certificate-${index}`;
        input.value = certificate.file;
        const content = document.createElement('span');
        const name = document.createElement('span');
        name.className = 'certificate-name';
        name.textContent = certificate.name;
        const validity = document.createElement('span');
        validity.className = 'certificate-validity';
        validity.textContent = `证书有效至 ${dateText(certificate.not_after)}`;
        content.append(name, validity);
        label.append(input, content);
        const details = document.createElement('details');
        const summary = document.createElement('summary');
        summary.textContent = '证书详情与指纹';
        const list = document.createElement('dl');
        addDetail(list, '证书文件', certificate.file);
        addDetail(list, '有效期开始', dateText(certificate.not_before));
        addDetail(list, 'SHA-256', certificate.sha256);
        addDetail(list, 'SPKI SHA-256', certificate.spki_sha256);
        details.append(summary, list);
        row.append(label, details);
        fragment.append(row);
      }
      byId('certificate-catalog').replaceChildren(fragment);
    }
    for (const input of byId('certificate-catalog').querySelectorAll('input')) input.checked = selected.has(input.value);
    byId('catalog-loading').hidden = catalog.length !== 0;
    byId('catalog-loading').textContent = '暂无可发布证书。';
  }

  function renderState(resetForm) {
    const published = state.published_snapshot;
    const serving = state.serving_snapshot;
    const current = serving ?? published;
    const unpublished = !published && !serving && !state.source_id && !state.publication_error;
    const servingError = unpublished ? null : state.serving_error;
    const servingErrorMessages = {
      version_rejected: '服务未接受当前版本：存在版本回退或同版本内容变化。请递增发布版本后重试。',
      reload_failed: '服务未能加载证书快照。请检查已发布文件，修正后刷新状态。',
      no_valid_snapshot: '当前没有可下发的有效快照。请递增版本并发布有效的证书集合。',
    };
    const servingMessage = servingError
      ? servingErrorMessages[servingError] || '证书下发暂不可用。请检查发布状态，修正后刷新。' : '';
    if (!state.enabled) setStatus('disabled', '未启用', '服务端尚未启用证书下发。');
    else if (serving) {
      if (servingError || !published || published.generation !== serving.generation || published.source_id !== serving.source_id) {
        setStatus('fallback', '继续下发旧版', '当前发布文件未生效，服务仍下发最后接受的版本。');
      } else if (serving.count === 0) setStatus('active', '已发布撤回', '当前下发空集合，客户端获取后会移除此来源下发的证书。');
      else setStatus('active', '下发中', '当前版本持续保留，直到发布更新或撤回；连接时校验证书自身有效期。');
    } else if (state.publication_error || servingError) setStatus('error', '下发不可用', '当前没有可下发的有效快照，请检查下方提示。');
    else setStatus('missing', '尚未发布', '选择需要下发的证书，发布第一个版本。');

    byId('snapshot-source').textContent = current?.source_id ?? state.source_id ?? '—';
    byId('snapshot-generation').textContent = current?.generation ?? '—';
    byId('snapshot-count').textContent = current ? String(current.count) : '—';
    byId('snapshot-issued-at').textContent = dateText(current?.issued_at);
    byId('delivery-endpoint').textContent = state.endpoint || '尚未配置 HTTPS 下发地址';
    byId('delivery-endpoint-note').textContent = state.endpoint
      ? (['127.0.0.1', 'localhost', '[::1]'].includes(new URL(state.endpoint).hostname)
        ? '本机 HTTPS 地址，仅适用于服务所在电脑。其他电脑需配置公开 HTTPS 入口。'
        : '浏览器客户端使用此 HTTPS 地址；请确认对应证书受客户端信任。')
      : `浏览器要求 HTTPS。请配置证书 HTTPS 监听器或部署 HTTPS 代理。接口调试地址：${state.internal_endpoint || '—'}`;
    byId('delivery-auth').textContent = state.auth_required ? '接口鉴权：需要 Bearer 凭据' : '接口鉴权：未启用 Bearer';
    byId('snapshot-file-state').textContent = published
      ? `已发布文件：版本 ${published.generation} · ${published.count} 张证书 · 发布于 ${dateText(published.issued_at)}`
      : '已发布文件：暂无可读取的有效快照。';
    notice('service-notice', state.publication_error || servingMessage);

    const list = byId('published-list');
    const fragment = document.createDocumentFragment();
    for (const certificate of published?.certificates ?? []) {
      const row = document.createElement('li');
      row.textContent = `${certificate.name || certificate.sha256} · ${certificate.role === 'trust_anchor' ? '信任锚' : '中间证书'}`;
      fragment.append(row);
    }
    list.replaceChildren(fragment);
    byId('published-details').hidden = !published?.certificates?.length;
    renderCatalog(state.catalog ?? [], resetForm);
    if (resetForm) {
      byId('source-id').value = state.source_id || published?.source_id || 'company-ca';
      byId('generation').value = state.next_generation ?? '1';
      byId('mode-include').checked = true;
    }
    if (state.source_id) byId('source-id').value = state.source_id;
  }

  async function loadState(resetForm = false) {
    if (loading) return;
    loading = true;
    updateControls();
    try {
      state = await api('/admin/certificates/state');
      renderState(resetForm || !initialized);
      initialized = true;
    } catch (error) {
      notice('service-notice', error.message);
      if (!state) {
        setStatus('error', '读取失败', '无法获取服务状态，请点击刷新重试。');
        byId('catalog-loading').textContent = '证书目录暂不可用。';
      }
      return false;
    } finally {
      loading = false;
      updateControls();
    }
    return true;
  }

  byId('refresh-state').addEventListener('click', () => loadState());
  byId('certificate-catalog').addEventListener('change', updateControls);
  for (const id of ['mode-include', 'mode-empty']) byId(id).addEventListener('change', updateControls);
  for (const [id, checked] of [['select-all', true], ['clear-selection', false]]) {
    byId(id).addEventListener('click', () => {
      for (const input of byId('certificate-catalog').querySelectorAll('input')) input.checked = checked;
      updateControls();
    });
  }

  byId('publish-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (submitting || loading || !state?.can_publish) return;
    notice('publish-result', '');
    const source = byId('source-id').value.trim();
    const generation = byId('generation').value.trim();
    const include = selectedFiles();
    const empty = withdrawal();
    if (!/^[A-Za-z0-9._-]{1,64}$/.test(source)) {
      notice('publish-result', '证书来源需为 1–64 个英文字母、数字、点、下划线或连字符。');
      byId('source-id').focus();
      return;
    }
    if (!/^[1-9][0-9]{0,19}$/.test(generation) || BigInt(generation) > 18446744073709551615n) {
      notice('publish-result', '请输入有效的正整数发布版本，最大为 18446744073709551615。');
      byId('generation').focus();
      return;
    }
    if (state.next_generation && BigInt(generation) < BigInt(state.next_generation)) {
      notice('publish-result', `发布版本至少应为 ${state.next_generation}。`);
      byId('generation').focus();
      return;
    }
    if (!empty && include.length === 0) { notice('publish-result', '请至少选择一张证书；撤回操作请明确选择“撤回全部”。'); return; }
    submitting = true;
    updateControls();
    try {
      await api('/admin/certificates/publish', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ source_id: source, generation, selection: empty ? 'empty' : 'include',
          include: empty ? [] : include, encoding: byId('encoding').value }),
      });
      notice('publish-result', empty ? `撤回版本 ${generation} 已发布。客户端获取后生效。`
        : `版本 ${generation} 已发布，共 ${include.length} 张证书。`, 'success');
      const refreshed = await loadState(true);
      if (!refreshed) notice('publish-result', `版本 ${generation} 已发布，但状态刷新失败。请刷新状态后再发布。`, 'success');
    } catch (error) {
      notice('publish-result', error.message);
    } finally {
      submitting = false;
      updateControls();
    }
  });

  loadState();
})();
