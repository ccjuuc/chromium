// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// HTTP management tests only. No installer is executed or browser installed.
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';

const tool = process.env.XENON_UPDATE_TOOL;

async function port() {
  const listener = net.createServer();
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const value = listener.address().port;
  await new Promise((resolve, reject) => listener.close(e => e ? reject(e) : resolve()));
  return value;
}

async function eventually(check, description) {
  const deadline = Date.now() + 8000;
  let last;
  while (Date.now() < deadline) {
    try { return await check(); } catch (error) { last = error; }
    await delay(30);
  }
  throw new Error(`${description}: ${last?.message}`, { cause: last });
}

async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill();
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 3000);
  try { await exited; } finally { clearTimeout(watchdog); }
}

test('browser management endpoints and interrupted uploads', {
  skip: !tool && 'Set XENON_UPDATE_TOOL to the built executable', timeout: 45000,
}, async t => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'xenon-admin-test-'));
  const config = path.join(directory, 'state', 'xenon-update.json');
  const listen = await port();
  const origin = `http://127.0.0.1:${listen}`;
  const init = spawnSync(tool, ['--config', config, 'init', '--port', String(listen)],
    { cwd: directory, encoding: 'utf8', timeout: 15000, windowsHide: true });
  assert.equal(init.error, undefined, init.error?.message);
  assert.equal(init.status, 0, `${init.stdout}\n${init.stderr}`);
  const initial = JSON.parse(await fs.readFile(config, 'utf8'));
  const child = spawn(tool, ['--config', config, 'serve'], { cwd: directory, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let diagnostics = '';
  child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-20000); });
  child.stdout.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-20000); });
  t.after(async () => { await stop(child); await fs.rm(directory, { recursive: true, force: true }); });
  const home = await eventually(async () => {
    const response = await fetch(origin);
    assert.equal(response.status, 200, diagnostics);
    return response.text();
  }, 'service readiness');
  const token = home.match(/<meta\s+name="xenon-admin-token"\s+content="([a-f0-9]{64})"/i)?.[1];
  assert.ok(token, 'Homepage must supply the session token');
  const headers = { 'x-xenon-admin': token, origin };
  async function request(endpoint, { method = 'GET', body, extra = {} } = {}) {
    return fetch(`${origin}${endpoint}`, { method, headers: { ...headers, ...(body === undefined ? {} : { 'content-type': 'application/json' }), ...extra },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
  }
  async function state() {
    const response = await request('/admin/state');
    assert.equal(response.status, 200);
    return response.json();
  }
  async function idle() {
    return eventually(async () => { const value = await state(); assert.equal(value.busy, false); return value; }, 'job becomes idle');
  }

  await t.test('root, script and initial state are usable', async () => {
    assert.match(home, /Xenon 更新服务/);
    assert.match(home, /还没有发布版本/);
    assert.ok(!home.includes('{{ADMIN_TOKEN}}'));
    const js = await fetch(`${origin}/assets/home.js`);
    assert.equal(js.status, 200);
    assert.match(js.headers.get('content-type'), /javascript/);
    assert.ok((await js.text()).toLowerCase().includes('x-xenon-admin'));
    const value = await state();
    assert.equal(value.enabled, true);
    assert.equal(value.busy, false);
    assert.equal(value.job, null);
    assert.deepEqual(value.history, []);
  });

  await t.test('every administrative route requires the session token', async () => {
    for (const endpoint of ['/admin/state', '/admin/config', '/admin/import', '/admin/upload', '/admin/windows/config', '/admin/windows/import', '/admin/windows/upload']) {
      const response = await fetch(`${origin}${endpoint}`, { method: endpoint === '/admin/state' ? 'GET' : 'POST' });
      assert.equal(response.status, 403, endpoint);
    }
    assert.equal((await request('/admin/state', { extra: { 'x-xenon-admin': '0'.repeat(64) } })).status, 403);
  });

  await t.test('cross-origin, cross-site, forwarding and rebinding headers are denied', async () => {
    for (const extra of [{ origin: 'https://evil.example' }, { 'sec-fetch-site': 'cross-site' },
      { forwarded: 'for=198.51.100.1' }, { 'x-forwarded-for': '198.51.100.1' },
      { 'x-forwarded-host': 'example.com' }, { 'x-real-ip': '198.51.100.1' }]) {
      assert.equal((await request('/admin/state', { extra })).status, 403, JSON.stringify(extra));
    }
    // Fetch implementations may normalize Host; use the raw HTTP API here.
    const rebound = await new Promise((resolve, reject) => {
      const outgoing = http.get(`${origin}/admin/state`, { headers: { ...headers, host: `evil.example:${listen}` } }, response => {
        response.resume(); resolve(response.statusCode);
      });
      outgoing.on('error', reject);
    });
    assert.equal(rebound, 403);
    const response = await fetch(origin, { headers: { 'x-forwarded-for': '198.51.100.1' } });
    assert.equal(response.status, 200);
    assert.ok(!(await response.text()).includes(token), 'Proxy homepage must not disclose management token');
  });

  await t.test('GET requests cannot perform configuration or import mutations', async () => {
    const before = await fs.readFile(config, 'utf8');
    for (const endpoint of ['/admin/config', '/admin/import', '/admin/upload', '/admin/windows/config', '/admin/windows/import', '/admin/windows/upload']) {
      assert.ok((await request(endpoint)).status >= 400);
    }
    assert.equal(await fs.readFile(config, 'utf8'), before);
    assert.equal((await state()).job, null);
  });

  await t.test('settings validate fields and preserve server configuration', async () => {
    const before = await fs.readFile(config, 'utf8');
    for (const body of [{ host: '0.0.0.0' }, { key_hash: 'invalid' }, { team_id: 'bad' },
      { tools_dir: path.join(directory, 'missing') }, { python: directory }]) {
      assert.equal((await request('/admin/config', { method: 'POST', body })).status, 400);
      assert.equal(await fs.readFile(config, 'utf8'), before);
    }
    const response = await request('/admin/config', { method: 'POST', body: { key_hash: 'ab'.repeat(32), tools_dir: directory } });
    assert.equal(response.status, 200);
    assert.equal((await response.json()).saved, true);
    const current = JSON.parse(await fs.readFile(config, 'utf8'));
    assert.equal(current.port, initial.port);
    assert.equal(current.schema_version, initial.schema_version);
    assert.equal(path.basename(current.manifest), path.basename(initial.manifest));
    assert.equal(current.public_origin, initial.public_origin);
    assert.equal(current.crx_public_key_sha256, Buffer.alloc(32, 0xab).toString('base64'));
    assert.equal((await state()).config.key_hash, current.crx_public_key_sha256);
  });

  await t.test('import validates source type, extension, version and architecture before queuing', async () => {
    for (const body of [
      { source_type: 'path', source: 'setup.exe' }, { source_type: 'command', source: 'application.pkg' }, { source_type: 'path', source: 'setup.cmd' },
      { source_type: 'path', source: 'application.pkg', version: '01.2.3.4' },
      { source_type: 'path', source: 'application.pkg', arch: 'x86' },
      { source_type: 'url', source: 'https://example.com/application.pkg', run: true },
    ]) assert.equal((await request('/admin/import', { method: 'POST', body })).status, 400);
    assert.equal((await state()).job, null);
  });

  await t.test('Windows settings persist independently and preserve macOS signing configuration', async () => {
    const before = JSON.parse(await fs.readFile(config, 'utf8'));
    assert.equal((await state()).windows.config.default_arch, 'x86_64');
    for (const body of [{ default_arch: 'mips' }, { key_path: 'not-a-windows-field' },
      { seven_zip: directory }, { node: path.join(directory, 'missing-node') }, { changelog: 'a'.repeat(16385) }]) {
      assert.equal((await request('/admin/windows/config', { method: 'POST', body })).status, 400);
      assert.deepEqual(JSON.parse(await fs.readFile(config, 'utf8')), before);
    }
    const settings = { default_arch: 'arm64', changelog: 'Windows 更新说明', force_update: true };
    assert.equal((await request('/admin/windows/config', { method: 'POST', body: settings })).status, 200);
    const saved = JSON.parse(await fs.readFile(config, 'utf8'));
    assert.deepEqual(saved.packaging, before.packaging);
    assert.equal(saved.crx_public_key_sha256, before.crx_public_key_sha256);
    assert.equal(saved.windows.default_arch, 'arm64');
    assert.equal(saved.windows.changelog, settings.changelog);
    assert.equal(saved.windows.force_update, true);
    const current = await state();
    assert.equal(current.windows.endpoint, `${origin}/api/v1/update/check`);
    assert.deepEqual(current.windows.history, []);
    assert.equal(current.windows.config.force_update, true);
    assert.equal((await request('/admin/config', { method: 'POST', body: { key_hash: 'ab'.repeat(32), tools_dir: directory } })).status, 200);
    assert.deepEqual(JSON.parse(await fs.readFile(config, 'utf8')).windows, saved.windows);
  });

  await t.test('Windows imports validate their protocol before creating a job', async () => {
    for (const body of [
      { source_type: 'path', source: 'application.pkg' },
      { source_type: 'path', source: 'setup.exe', version: '1.2' },
      { source_type: 'path', source: 'setup.zip', arch: 'mips' },
      { source_type: 'command', source: 'setup.exe' },
    ]) assert.equal((await request('/admin/windows/import', { method: 'POST', body })).status, 400);
    assert.equal((await state()).job, null);
  });

  await t.test('missing signing configuration fails asynchronously and allows retry', async () => {
    for (let attempt = 0; attempt < 2; ++attempt) {
      const response = await request('/admin/import', { method: 'POST', body: { source_type: 'path', source: path.join(directory, 'missing.pkg') } });
      assert.equal(response.status, 202);
      const accepted = await response.json();
      const result = await idle();
      assert.equal(result.job.id, accepted.job.id);
      assert.equal(result.job.status, 'failed');
      assert.equal(result.job.platform, 'mac');
      assert.match(result.job.error, /签名私钥/);
    }
  });

  await t.test('invalid multipart extensions do not leave a busy job', async () => {
    const form = new FormData();
    form.append('file', new Blob(['not an installer']), 'setup.cmd');
    const response = await fetch(`${origin}/admin/upload`, { method: 'POST', headers, body: form });
    assert.equal(response.status, 400);
    assert.match((await response.json()).error, /macOS.*\.pkg/);
    const current = await idle();
    assert.equal(current.job.status, 'failed');
  });

  await t.test('an active upload prevents imports/settings and disconnect releases busy state', async () => {
    const upload = http.request(`${origin}/admin/upload`, { method: 'POST', headers: {
      ...headers, 'content-type': 'multipart/form-data; boundary=XENON', 'content-length': '100000',
    }});
    upload.on('error', () => {});
    upload.write('--XENON\r\nContent-Disposition: form-data; name="file"; filename="installer.pkg"\r\nContent-Type: application/octet-stream\r\n\r\nxar!partial');
    try {
      await eventually(async () => assert.equal((await state()).busy, true), 'upload busy state');
      const oldConfig = await fs.readFile(config, 'utf8');
      assert.ok((await request('/admin/config', { method: 'POST', body: {} })).status >= 400);
      assert.equal(await fs.readFile(config, 'utf8'), oldConfig);
      const competing = await request('/admin/import', { method: 'POST', body: { source_type: 'path', source: 'other.pkg' } });
      assert.ok(competing.status >= 400);
    } finally { upload.destroy(); }
    const current = await idle();
    assert.equal(current.job.status, 'failed');
    const jobs = await fs.readdir(path.join(directory, 'state', 'publisher'));
    assert.ok(jobs.every(name => !name.startsWith('job-')), 'Aborted upload staging must be removed');
  });

  await t.test('completed upload returns a job and failure still cleans staging', async () => {
    const form = new FormData();
    form.append('file', new Blob(['xar!fixture']), '../../application.PKG');
    form.append('version', '1.2.3.4');
    form.append('arch', 'x86_64');
    const response = await fetch(`${origin}/admin/upload`, { method: 'POST', headers, body: form });
    assert.equal(response.status, 202);
    const current = await idle();
    assert.equal(current.job.status, 'failed');
    assert.match(current.job.error, /签名私钥/);
    const jobs = await fs.readdir(path.join(directory, 'state', 'publisher'));
    assert.ok(jobs.every(name => !name.startsWith('job-')));
    assert.equal(JSON.parse(await fs.readFile(config, 'utf8')).port, listen);
  });
});

test('public update listener never exposes the separate local management session', {
  skip: !tool && 'Set XENON_UPDATE_TOOL to the built executable', timeout: 20000,
}, async t => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'xenon-public-admin-test-'));
  const config = path.join(directory, 'state', 'xenon-update.json');
  const listen = await port();
  let adminPort = await port();
  while (adminPort === listen) adminPort = await port();
  const publicBase = `http://127.0.0.1:${listen}`;
  const adminBase = `http://127.0.0.1:${adminPort}`;
  const init = spawnSync(tool, ['--config', config, 'init', '--port', String(listen), '--public-origin', 'https://updates.example.test'],
    { cwd: directory, encoding: 'utf8', timeout: 15000, windowsHide: true });
  assert.equal(init.status, 0, `${init.stdout}\n${init.stderr}`);
  const value = JSON.parse(await fs.readFile(config, 'utf8'));
  value.admin_port = adminPort;
  await fs.writeFile(config, JSON.stringify(value));
  const child = spawn(tool, ['--config', config, 'serve'], { cwd: directory, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let diagnostics = '';
  child.stdout.on('data', data => { diagnostics = (diagnostics + data).slice(-8000); });
  child.stderr.on('data', data => { diagnostics = (diagnostics + data).slice(-8000); });
  t.after(async () => { await stop(child); await fs.rm(directory, { recursive: true, force: true }); });
  const publicHome = await eventually(async () => {
    const response = await fetch(publicBase);
    assert.equal(response.status, 200, diagnostics);
    return response.text();
  }, 'public listener readiness');
  assert.ok(!/<meta\s+name="xenon-admin-token"\s+content="[a-f0-9]{64}"/i.test(publicHome));
  assert.equal((await fetch(`${publicBase}/admin/state`)).status, 404);
  const privateHome = await eventually(async () => {
    const response = await fetch(adminBase);
    assert.equal(response.status, 200, diagnostics);
    return response.text();
  }, 'management listener readiness');
  const token = privateHome.match(/<meta\s+name="xenon-admin-token"\s+content="([a-f0-9]{64})"/i)?.[1];
  assert.ok(token);
  const local = await fetch(`${adminBase}/admin/state`, { headers: { 'x-xenon-admin': token, origin: adminBase } });
  assert.equal(local.status, 200);
  assert.equal((await local.json()).enabled, true);
  // A remote client that spoofs local browser headers still has no admin API
  // on the port behind its update reverse proxy.
  const remote = await fetch(`${publicBase}/admin/state`, { headers: { 'x-xenon-admin': token, origin: publicBase } });
  assert.equal(remote.status, 404);
  const forwarded = await fetch(`${adminBase}/admin/state`, { headers: { 'x-xenon-admin': token, origin: adminBase, 'x-forwarded-proto': 'https' } });
  assert.equal(forwarded.status, 403);
  const check = await fetch(`${publicBase}/service/update2/json`, { method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ request: { protocol: '4.0', apps: [] } }) });
  assert.equal(check.status, 200);
  assert.equal(JSON.parse((await check.text()).slice(5)).response.protocol, '4.0');
});
