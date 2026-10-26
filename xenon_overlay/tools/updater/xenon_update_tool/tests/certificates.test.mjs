// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Exercises the actual Axum certificate management routes in isolated storage.
// No user server, browser profile, or system certificate store is changed.
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
const downloadPath = '/api/v1/browser/ca-certificates';
const statePath = '/admin/certificates/state';
const publishPath = '/admin/certificates/publish';

async function unusedPort() {
  const listener = net.createServer();
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const value = listener.address().port;
  await new Promise((resolve, reject) => listener.close(error => error ? reject(error) : resolve()));
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
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill();
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 3000);
  try { await exited; } finally { clearTimeout(watchdog); }
}

async function removeOwnedDirectory(directory, temporaryRoot) {
  const resolved = await fs.realpath(directory);
  assert.equal(path.dirname(resolved), temporaryRoot, 'Cleanup must remain immediately under the temporary directory');
  assert.match(path.basename(resolved), /^xenon-certificates-test-/);
  await fs.rm(resolved, { recursive: true, force: true });
}

test('certificate management page and Axum publishing lifecycle', {
  skip: !tool && 'Set XENON_UPDATE_TOOL to the built executable', timeout: 60000,
}, async t => {
  const temporaryRoot = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(path.join(temporaryRoot, 'xenon-certificates-test-'));
  let child;
  t.after(async () => {
    await stop(child);
    await removeOwnedDirectory(directory, temporaryRoot);
  });
  const config = path.join(directory, 'state', 'xenon-update.json');
  const listen = await unusedPort();
  const origin = `http://127.0.0.1:${listen}`;
  const init = spawnSync(tool, ['--config', config, 'init', '--port', String(listen)],
    { cwd: directory, encoding: 'utf8', timeout: 15000, windowsHide: true });
  assert.equal(init.error, undefined, init.error?.message);
  assert.equal(init.status, 0, `${init.stdout}\n${init.stderr}`);
  const initialConfig = await fs.readFile(config, 'utf8');
  const snapshotFile = path.resolve(path.dirname(config), JSON.parse(initialConfig).managed_certificates.snapshot_file);
  let token;
  let html;
  let pageHeaders;
  async function start() {
    let diagnostics = '';
    child = spawn(tool, ['--config', config, 'serve'], {
      cwd: directory, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
    });
    child.on('error', error => { diagnostics += error.message; });
    child.stdout.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-20000); });
    child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-20000); });
    await eventually(async () => {
      assert.equal(child.exitCode, null, diagnostics);
      const response = await fetch(`${origin}/certificates`);
      assert.equal(response.status, 200, diagnostics);
      html = await response.text();
      pageHeaders = response.headers;
      token = html.match(/<meta\s+name="xenon-admin-token"\s+content="([a-f0-9]{64})"/i)?.[1];
      assert.ok(token, 'The certificate page must supply a session token');
    }, 'certificate page readiness');
  }
  async function request(endpoint, { method = 'GET', body, raw, extra = {} } = {}) {
    return fetch(`${origin}${endpoint}`, {
      method,
      headers: { 'x-xenon-admin': token, origin,
        ...(body === undefined && raw === undefined ? {} : { 'content-type': 'application/json' }),
        ...extra },
      ...(raw === undefined ? body === undefined ? {} : { body: JSON.stringify(body) } : { body: raw }),
    });
  }
  async function state() {
    const response = await request(statePath);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('cache-control'), 'no-store');
    return response.json();
  }
  function publication(generation, overrides = {}) {
    return { source_id: 'company-ca', generation, selection: 'all', include: [],
      encoding: 'der_base64', ...overrides };
  }
  async function publish(generation, overrides = {}) {
    const response = await request(publishPath, { method: 'POST', body: publication(generation, overrides) });
    const result = await response.json();
    assert.equal(response.status, 200, JSON.stringify(result));
    return result;
  }
  async function download(generation, count) {
    const response = await fetch(`${origin}${downloadPath}`);
    assert.equal(response.status, 200);
    const document = await response.json();
    assert.equal(document.source_id, 'company-ca');
    assert.equal(document.generation, generation);
    assert.equal(document.certificates.length, count);
    return { document, etag: response.headers.get('etag') };
  }
  await start();
  let catalog;

  await t.test('page, asset and homepage navigation have usable security headers', async () => {
    assert.match(pageHeaders.get('content-type'), /text\/html/);
    assert.equal(pageHeaders.get('cache-control'), 'no-store');
    assert.equal(pageHeaders.get('x-content-type-options'), 'nosniff');
    const csp = pageHeaders.get('content-security-policy');
    assert.match(csp, /default-src 'none'/);
    assert.match(csp, /script-src 'self'/);
    assert.match(csp, /connect-src 'self'/);
    assert.match(csp, /frame-ancestors 'none'/);
    assert.equal(pageHeaders.get('referrer-policy'), 'no-referrer');
    assert.match(html, /src="\/assets\/certificates\.js"/);
    assert.ok(!html.includes('{{ADMIN_TOKEN}}'));
    const home = await fetch(origin);
    assert.equal(home.status, 200);
    assert.match(await home.text(), /href="\/certificates"/);
    const script = await fetch(`${origin}/assets/certificates.js`);
    assert.equal(script.status, 200);
    assert.match(script.headers.get('content-type'), /javascript/);
    assert.equal(script.headers.get('cache-control'), 'no-store');
    assert.match(await script.text(), /x-xenon-admin/i);
    const head = await fetch(`${origin}/certificates`, { method: 'HEAD' });
    assert.equal(head.status, 200);
    assert.equal(await head.text(), '');
    assert.equal(head.headers.get('content-length'), String(Buffer.byteLength(html)));
  });

  await t.test('initial state describes the catalog without publishing implicitly', async () => {
    const value = await state();
    assert.equal(value.enabled, true);
    assert.equal(value.endpoint, null);
    assert.equal(value.internal_endpoint, `${origin}${downloadPath}`);
    assert.equal(path.resolve(value.snapshot_file), snapshotFile);
    assert.equal(value.auth_required, false);
    assert.equal(value.source_id, null);
    assert.equal(value.next_generation, '1');
    assert.equal(value.can_publish, true);
    assert.equal(value.publication_error, null);
    assert.equal(value.published_snapshot, null);
    assert.equal(value.serving_snapshot, null);
    catalog = value.catalog;
    assert.equal(catalog.length, 6);
    assert.equal(new Set(catalog.map(entry => entry.file)).size, 6);
    for (const entry of catalog) {
      assert.match(entry.sha256, /^[a-f0-9]{64}$/);
      assert.ok(entry.name && entry.file && entry.not_after);
    }
    await assert.rejects(fs.stat(snapshotFile), { code: 'ENOENT' });
    assert.equal((await fetch(`${origin}${downloadPath}`)).status, 503);
  });

  await t.test('read and publication routes require the local management token', async () => {
    for (const [endpoint, method] of [[statePath, 'GET'], [publishPath, 'POST']]) {
      const absent = await fetch(`${origin}${endpoint}`, { method });
      assert.equal(absent.status, 403, endpoint);
      const wrong = await request(endpoint, { method, extra: { 'x-xenon-admin': '0'.repeat(64) } });
      assert.equal(wrong.status, 403, endpoint);
    }
  });

  await t.test('both routes reject cross-origin, cross-site, forwarded and rebound requests', async () => {
    for (const [endpoint, method] of [[statePath, 'GET'], [publishPath, 'POST']]) {
      for (const extra of [
        { origin: 'https://evil.example' }, { 'sec-fetch-site': 'cross-site' },
        { 'sec-fetch-site': 'same-site' }, { forwarded: 'for=198.51.100.1' },
        { 'x-forwarded-for': '198.51.100.1' }, { 'x-forwarded-host': 'example.com' },
        { 'x-forwarded-proto': 'https' }, { 'x-real-ip': '198.51.100.1' },
      ]) {
        const response = await request(endpoint, { method,
          ...(method === 'POST' ? { body: publication('1') } : {}), extra });
        assert.equal(response.status, 403, `${endpoint}: ${JSON.stringify(extra)}`);
      }
      // Use the raw HTTP API because fetch may normalize Host.
      const status = await new Promise((resolve, reject) => {
        const outgoing = http.request(`${origin}${endpoint}`, { method,
          headers: { 'x-xenon-admin': token, origin, host: `evil.example:${listen}` } }, response => {
          response.resume(); resolve(response.statusCode);
        });
        outgoing.on('error', reject);
        outgoing.end();
      });
      assert.equal(status, 403, endpoint);
    }
    for (const extra of [{ forwarded: 'for=198.51.100.1' }, { 'x-forwarded-proto': 'https' }]) {
      const response = await fetch(`${origin}/certificates`, { headers: extra });
      assert.equal(response.status, 403);
      assert.ok(!(await response.text()).includes(token), 'A forwarded page must not expose the admin token');
    }
    await assert.rejects(fs.stat(snapshotFile), { code: 'ENOENT' });
  });

  await t.test('invalid method, query, content type and JSON do not create a snapshot', async () => {
    assert.equal((await request(publishPath)).status, 400);
    assert.equal((await request(statePath, { method: 'POST', body: {} })).status, 400);
    assert.equal((await request(`${statePath}?source=other`)).status, 400);
    assert.equal((await request(`${publishPath}?out=elsewhere`, { method: 'POST', body: publication('1') })).status, 400);
    for (const contentType of ['text/plain', 'application/x-www-form-urlencoded', 'application/json; charset=utf-16']) {
      assert.equal((await request(publishPath, { method: 'POST', body: publication('1'),
        extra: { 'content-type': contentType } })).status, 400);
    }
    for (const body of [publication('1', { out: path.join(directory, 'elsewhere.json') }),
      publication('1', { generation: 1 }), publication('1', { selection: 'include', include: [] }),
      publication('1', { include: [catalog[0].file] }), publication('1', { valid_days: 7 })]) {
      assert.equal((await request(publishPath, { method: 'POST', body })).status, 400);
    }
    for (const raw of ['{', `${JSON.stringify(publication('1')).slice(0, -1)},"generation":"2"}`]) {
      assert.equal((await request(publishPath, { method: 'POST', raw })).status, 400);
    }
    await assert.rejects(fs.stat(snapshotFile), { code: 'ENOENT' });
    assert.equal(await fs.readFile(config, 'utf8'), initialConfig);
    assert.equal((await state()).next_generation, '1');
  });

  await t.test('publishing all roots updates the actual download and conditional GET', async () => {
    await publish('1');
    const { document, etag } = await download('1', 6);
    assert.ok(etag);
    assert.equal(Object.hasOwn(document, 'expires_at'), false);
    assert.deepEqual(JSON.parse(await fs.readFile(snapshotFile, 'utf8')), document);
    const conditional = await fetch(`${origin}${downloadPath}`, { headers: { 'if-none-match': etag } });
    assert.equal(conditional.status, 304);
    assert.equal(await conditional.text(), '');
    const value = await state();
    assert.equal(value.source_id, 'company-ca');
    assert.equal(value.next_generation, '2');
    assert.equal(value.published_snapshot.generation, '1');
    assert.equal(value.published_snapshot.count, 6);
    assert.equal(value.serving_snapshot.generation, '1');
    assert.equal(value.serving_snapshot.count, 6);
  });

  await t.test('subset publication replaces the complete snapshot and generation is immutable', async () => {
    const include = catalog.slice(0, 2).map(entry => entry.file);
    await publish('2', { selection: 'include', include });
    await download('2', 2);
    const value = await state();
    assert.deepEqual(value.published_snapshot.certificates.map(entry => entry.file), include);
    assert.equal(value.next_generation, '3');
    const bytes = await fs.readFile(snapshotFile, 'utf8');
    const duplicate = await request(publishPath, { method: 'POST', body: publication('2') });
    assert.equal(duplicate.status, 400);
    assert.match((await duplicate.json()).error, /generation|immutable/i);
    assert.equal(await fs.readFile(snapshotFile, 'utf8'), bytes);
    await download('2', 2);
  });

  await t.test('empty publication withdraws trust and restart restores the accepted version', async () => {
    await publish('3', { selection: 'empty' });
    await download('3', 0);
    const oldToken = token;
    const acceptedBytes = await fs.readFile(`${snapshotFile}.accepted.json`, 'utf8');
    await stop(child);
    await start();
    assert.notEqual(token, oldToken, 'Each server session must have its own token');
    assert.equal((await request(statePath, { extra: { 'x-xenon-admin': oldToken } })).status, 403);
    const value = await state();
    assert.equal(value.next_generation, '4');
    assert.equal(value.published_snapshot.generation, '3');
    assert.equal(value.serving_snapshot.generation, '3');
    assert.equal(value.serving_snapshot.count, 0);
    await download('3', 0);
    assert.equal(await fs.readFile(`${snapshotFile}.accepted.json`, 'utf8'), acceptedBytes);
    assert.equal(await fs.readFile(config, 'utf8'), initialConfig);
  });

  await t.test('legacy expiry keeps accepted bytes across restart and newer publication removes only the metadata', async () => {
    const legacy = { ...JSON.parse(await fs.readFile(snapshotFile, 'utf8')),
      generation: '4', issued_at: '2000-01-01T00:00:00Z', expires_at: '2000-01-02T00:00:00Z' };
    const bytes = `${JSON.stringify(legacy, null, 2)}\n`;
    await fs.writeFile(snapshotFile, bytes);
    const { document, etag } = await download('4', 0);
    assert.deepEqual(document, legacy);
    assert.equal(await fs.readFile(`${snapshotFile}.accepted.json`, 'utf8'), bytes);
    await stop(child);
    // Recover from accepted state while the published file is unavailable.
    await fs.unlink(snapshotFile);
    await start();
    const response = await fetch(`${origin}${downloadPath}`);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('etag'), etag);
    assert.equal(await response.text(), bytes);
    const conditional = await fetch(`${origin}${downloadPath}`, { headers: { 'if-none-match': etag } });
    assert.equal(conditional.status, 304);
    assert.equal(await fs.readFile(`${snapshotFile}.accepted.json`, 'utf8'), bytes);
    const value = await state();
    assert.equal(value.serving_snapshot.generation, '4');
    assert.equal(value.next_generation, '5');
    assert.equal(value.can_publish, true);
    await publish('5', { selection: 'empty' });
    const updated = await download('5', 0);
    assert.equal(Object.hasOwn(updated.document, 'expires_at'), false);
  });

  await t.test('generation strings retain precision beyond JavaScript integers and at uint64 maximum', async () => {
    await publish('9007199254740993', { selection: 'empty' });
    assert.equal((await state()).next_generation, '9007199254740994');
    await download('9007199254740993', 0);
    await publish('18446744073709551614', { selection: 'empty' });
    assert.equal((await state()).next_generation, '18446744073709551615');
    await publish('18446744073709551615', { selection: 'empty' });
    await download('18446744073709551615', 0);
    const value = await state();
    assert.equal(value.published_snapshot.generation, '18446744073709551615');
    assert.equal(value.serving_snapshot.generation, '18446744073709551615');
    assert.equal(value.next_generation, null);
    assert.equal(value.can_publish, false);
    assert.match(value.publication_error, /uint64 maximum/i);
    const before = await fs.readFile(snapshotFile, 'utf8');
    assert.equal((await request(publishPath, { method: 'POST', body: publication('18446744073709551616') })).status, 400);
    assert.equal(await fs.readFile(snapshotFile, 'utf8'), before);
  });
});
