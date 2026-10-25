// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Real-process Crashpad HTTP intake tests; no browser or real crash is required.
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';
import { gzipSync } from 'node:zlib';

const tool = process.env.XENON_UPDATE_TOOL;

async function freePort() {
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
    await delay(40);
  }
  throw new Error(`${description}: ${last?.message}`, { cause: last });
}

async function stop(child) {
  if (!child || !child.pid || child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill();
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 3000);
  try { await exited; } finally { clearTimeout(watchdog); }
}

async function removeFixture(directory) {
  // Only remove the exact mkdtemp child of the system temporary directory.
  const root = await fs.realpath(os.tmpdir());
  const resolved = await fs.realpath(directory);
  const relative = path.relative(root, resolved);
  assert.equal(path.dirname(relative), '.');
  assert.match(path.basename(relative), /^xenon-crashes-test-/);
  assert.equal((await fs.lstat(directory)).isSymbolicLink(), false);
  await fs.rm(resolved, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}

function multipart(dump, attachment, annotations = []) {
  const boundary = 'XENON-CRASH-TEST-BOUNDARY';
  const parts = [
    Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="prod"\r\n\r\nXenon\r\n`),
    Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="ver"\r\n\r\n1.0.0.1\r\n`),
    ...annotations.flatMap(([name, value]) => [
      Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="${name}"\r\n\r\n`),
      value,
      Buffer.from('\r\n'),
    ]),
    Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="upload_file_minidump"; filename="report.dmp"\r\nContent-Type: application/octet-stream\r\n\r\n`),
    dump,
    Buffer.from(`\r\n--${boundary}\r\nContent-Disposition: form-data; name="log"; filename="../../outside.log"\r\nContent-Type: application/octet-stream\r\n\r\n`),
    attachment,
    Buffer.from(`\r\n--${boundary}--\r\n`),
  ];
  return { type: `multipart/form-data; boundary=${boundary}`, bytes: Buffer.concat(parts) };
}

test('Crashpad uploads persist across server restarts and respect configuration', {
  skip: !tool && 'Set XENON_UPDATE_TOOL to the built executable', timeout: 45000,
}, async t => {
  assert.ok(path.isAbsolute(tool), 'XENON_UPDATE_TOOL must be an absolute executable path');
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'xenon-crashes-test-'));
  const children = [];
  t.after(async () => {
    for (const child of children) await stop(child);
    await removeFixture(directory);
  });
  const config = path.join(directory, 'state', 'xenon-update.json');
  const reports = path.join(directory, 'state', 'crashes');
  const listen = await freePort();
  const origin = `http://127.0.0.1:${listen}`;
  const endpoint = `${origin}/api/v1/crashes`;
  const init = spawnSync(tool, ['--config', config, 'init', '--port', String(listen)],
    { cwd: directory, encoding: 'utf8', timeout: 15000, windowsHide: true });
  assert.equal(init.error, undefined, init.error?.message);
  assert.equal(init.status, 0, `${init.stdout}\n${init.stderr}`);
  const settings = JSON.parse(await fs.readFile(config, 'utf8'));
  assert.deepEqual(settings.crashes, {
    enabled: true, directory: 'crashes', max_upload_bytes: 64 * 1024 * 1024,
  });
  // Existing configurations without a crashes block must acquire the same defaults.
  delete settings.crashes;
  await fs.writeFile(config, JSON.stringify(settings));

  async function start() {
    const child = spawn(tool, ['--config', config, 'serve'],
      { cwd: directory, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    children.push(child);
    let diagnostics = '';
    let spawnError;
    child.on('error', error => { spawnError = error; });
    for (const stream of [child.stdout, child.stderr]) {
      stream.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-20000); });
    }
    await eventually(async () => {
      assert.equal(spawnError, undefined, spawnError?.message);
      assert.equal(child.exitCode, null, diagnostics);
      const response = await fetch(origin, { signal: AbortSignal.timeout(1000) });
      assert.equal(response.status, 200, diagnostics);
      await response.arrayBuffer();
    }, 'service readiness');
    return child;
  }

  const dump = Buffer.concat([Buffer.from('MDMP'), Buffer.alloc(28), Buffer.from([0, 255, 10, 0, 42])]);
  const attachment = Buffer.from('diagnostic\0data\n', 'utf8');
  const saved = [];
  let child = await start();

  async function upload(body, type, encoding, query = '') {
    const response = await fetch(`${endpoint}${query}`, { method: 'POST',
      headers: { 'content-type': type, ...(encoding ? { 'content-encoding': encoding } : {}) }, body,
      signal: AbortSignal.timeout(5000) });
    assert.equal(response.status, 200, await response.clone().text());
    assert.match(response.headers.get('content-type'), /^text\/plain\b/);
    const id = await response.text();
    assert.match(id, /^[a-f0-9]{32}$/, 'Crashpad expects the report ID without JSON or whitespace');
    assert.ok(!saved.includes(id), 'Every upload gets its own report ID');
    saved.push(id);
    const report = path.join(reports, id);
    assert.deepEqual(await fs.readFile(path.join(report, 'minidump.dmp')), dump);
    const metadata = JSON.parse(await fs.readFile(path.join(report, 'metadata.json'), 'utf8'));
    assert.equal(metadata.id, id);
    assert.equal(metadata.dump_size, dump.length);
    assert.ok(metadata.received_at > 0);
    return { id, report, metadata };
  }

  await t.test('old configuration enables binary intake beside the configuration file', async () => {
    const result = await upload(dump, 'application/octet-stream', undefined,
      '?product=Xenon&version=1.0.0.1&guid=test-client');
    assert.equal(result.metadata.format, 'binary');
    assert.deepEqual(result.metadata.annotations, {});
    assert.deepEqual(result.metadata.query,
      [['product', 'Xenon'], ['version', '1.0.0.1'], ['guid', 'test-client']]);
    assert.deepEqual((await fs.readdir(result.report)).sort(), ['metadata.json', 'minidump.dmp']);
    await assert.rejects(fs.stat(path.join(directory, 'crashes')), { code: 'ENOENT' });
  });

  for (const compressed of [false, true]) {
    await t.test(`${compressed ? 'gzip' : 'plain'} multipart stores annotations and attachments`, async () => {
      const form = multipart(dump, attachment);
      const result = await upload(compressed ? gzipSync(form.bytes) : form.bytes,
        form.type, compressed ? 'gzip' : undefined);
      assert.equal(result.metadata.format, 'multipart');
      assert.deepEqual(result.metadata.annotations, { prod: 'Xenon', ver: '1.0.0.1' });
      assert.deepEqual(result.metadata.attachments, [{ field: 'log', original_name: '../../outside.log',
        file: 'attachment-000.bin', size: attachment.length }]);
      assert.deepEqual(await fs.readFile(path.join(result.report, 'attachment-000.bin')), attachment);
      assert.deepEqual((await fs.readdir(result.report)).sort(),
        ['attachment-000.bin', 'metadata.json', 'minidump.dmp']);
      await assert.rejects(fs.stat(path.join(directory, 'state', 'outside.log')), { code: 'ENOENT' });
    });
  }

  await t.test('byte-truncated UTF-8 annotations retain both readable and original values', async () => {
    const original = Buffer.from('中'.repeat(22)).subarray(0, 64);
    const form = multipart(dump, attachment, [['switch-1', original]]);
    const result = await upload(gzipSync(form.bytes), form.type, 'gzip');
    assert.equal(result.metadata.annotations['switch-1'], `${'中'.repeat(21)}\ufffd`);
    assert.deepEqual(Buffer.from(result.metadata.annotations_base64['switch-1'], 'base64'), original);
    assert.equal(result.metadata.dropped_fields, 0);
  });

  await t.test('excess annotations are bounded without rejecting a later minidump', async () => {
    const annotations = Array.from({ length: 4096 }, (_, index) =>
      [`feature-${index}`, Buffer.from('enabled')]);
    const form = multipart(dump, attachment, annotations);
    const result = await upload(form.bytes, form.type);
    assert.equal(Object.keys(result.metadata.annotations).length, 4096);
    // prod and ver precede the feature fields, leaving room for 4094 features.
    assert.equal(result.metadata.annotations['feature-4093'], 'enabled');
    assert.equal(result.metadata.annotations['feature-4094'], undefined);
    assert.equal(result.metadata.dropped_fields, 2);
    assert.deepEqual(await fs.readFile(path.join(result.report, 'attachment-000.bin')), attachment);
  });

  await t.test('reports and metadata have no public download route', async () => {
    for (const route of [`/crashes/${saved[0]}/minidump.dmp`, `/crashes/${saved[0]}/metadata.json`,
      `/api/v1/crashes/${saved[0]}`, `/artifacts/${saved[0]}/minidump.dmp`]) {
      const response = await fetch(`${origin}${route}`);
      assert.equal(response.status, 404, route);
      await response.arrayBuffer();
    }
  });

  await t.test('restarting preserves existing reports and accepts further reports', async () => {
    await stop(child);
    child = await start();
    assert.deepEqual((await fs.readdir(reports)).sort(), [...saved].sort());
    for (const id of saved) assert.deepEqual(await fs.readFile(path.join(reports, id, 'minidump.dmp')), dump);
    await upload(gzipSync(dump), 'application/octet-stream', 'gzip');
  });

  await t.test('disabling intake after restart returns 404 and retains saved reports', async () => {
    await stop(child);
    settings.crashes = { enabled: false };
    await fs.writeFile(config, JSON.stringify(settings));
    child = await start();
    const response = await fetch(endpoint, { method: 'POST',
      headers: { 'content-type': 'application/octet-stream' }, body: dump });
    assert.equal(response.status, 404);
    await response.arrayBuffer();
    assert.deepEqual((await fs.readdir(reports)).sort(), [...saved].sort());
  });
});
