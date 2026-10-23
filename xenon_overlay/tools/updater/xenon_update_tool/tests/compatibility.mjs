// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Optional integration checks. Node is required only to run this test, not
// to deploy the Rust service. Fixtures test protocol/container behavior;
// they are intentionally not signed, installable browser releases.
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';
import { buildResponse, loadCatalog, RESPONSE_PREFIX } from '../../omaha_update_server.js';

const hash = value => createHash('sha256').update(value).digest('hex');
const appid = 'org.chromium.Chromium';
const tool = process.env.XENON_UPDATE_TOOL;
const verifier = process.env.XENON_VERIFY_TOOL;

function run(executable, args, cwd) {
  const result = spawnSync(executable, args, { cwd, encoding: 'utf8', timeout: 30_000, windowsHide: true });
  assert.equal(result.error, undefined, result.error?.message);
  assert.equal(result.status, 0, `${args.join(' ')}\n${result.stdout}\n${result.stderr}`);
  return JSON.parse(result.stdout);
}

async function unusedPort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const port = server.address().port;
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  return port;
}

async function eventually(action, description, milliseconds = 12_000) {
  const deadline = Date.now() + milliseconds;
  let last;
  while (Date.now() < deadline) {
    try { return await action(); } catch (error) { last = error; }
    await delay(100);
  }
  throw new Error(`${description}: ${last?.message}`, { cause: last });
}

async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill();
  await Promise.race([exited, delay(3000).then(() => { child.kill('SIGKILL'); })]);
  if (child.exitCode === null && child.signalCode === null) {
    await Promise.race([exited, delay(3000).then(() => { throw new Error('Server did not terminate'); })]);
  }
}

function body(platform = 'mac', patch = {}, top = {}) {
  return { request: { protocol: '4.0', acceptformat: 'download,crx3,puff', '@os': platform,
    arch: 'x64',
    os: { platform: ({ mac: 'Mac OS X', win: 'Windows', linux: 'Linux' })[platform] ?? platform, arch: 'arm64' },
    apps: [{ appid, version: '1.0.0.1', updatecheck: {}, ...patch }], ...top } };
}

async function release(directory, version, previous) {
  await fs.mkdir(directory, { recursive: true });
  const apps = [];
  for (const arch of ['x86_64', 'arm64']) {
    const bytes = Buffer.alloc(4096, arch === 'x86_64' ? 0x61 : 0x62);
    bytes.write('Cr24');
    bytes.writeUInt32LE(3, 4);
    bytes.writeUInt32LE(1, 8);
    bytes.write(version, 32);
    const file = `mac-${arch}.crx3`;
    await fs.writeFile(path.join(directory, file), bytes);
    const app = { appid, version, platform: 'mac', arch,
      minimum_version: '1.0.0.0', full: { file, size: bytes.length, sha256: hash(bytes) } };
    if (previous) {
      const patch = Buffer.from(`PUF1protocol-fixture-mac-${arch}-${version}`);
      const file = `mac-${arch}.puff`;
      await fs.writeFile(path.join(directory, file), patch);
      app.differential = [{ type: 'puff', file, size: patch.length, sha256: hash(patch),
        from_version: previous.apps.find(app => app.arch === arch).version,
        previous_sha256: previous.apps.find(app => app.arch === arch).full.sha256 }];
    }
    apps.push(app);
  }
  const manifest = { schema_version: 1, apps };
  const filename = path.join(directory, 'release.json');
  await fs.writeFile(filename, JSON.stringify(manifest));
  return { ...manifest, filename };
}

test('Rust CLI, Node Omaha compatibility, native parser and catalog reload', { skip: !tool, timeout: 60_000 }, async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'xenon-rust-compat-'));
  let child;
  let catalog;
  t.after(async () => {
    if (catalog) await catalog.close();
    if (child) await stop(child);
    await fs.rm(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  });
  const port = await unusedPort();
  const origin = `http://127.0.0.1:${port}`;
  const config = path.join(root, 'service/xenon-update.json');
  const cli = (...args) => run(tool, ['--config', config, ...args], root);
  const initialized = cli('init', '--port', String(port), '--key-hash', 'ab'.repeat(32));
  const catalogPath = initialized.manifest;
  assert.equal((await fs.readFile(path.join(root, 'service/client-updater.gni'), 'utf8')).includes(origin), true);
  const base = await release(path.join(root, 'base'), '1.0.0.1');
  cli('publish', '--release', base.filename, '--full-only');
  assert.equal(cli('doctor').ok, true);

  await t.test('both implementations reject Windows releases without changing the active catalog', async () => {
    const windows = structuredClone(base.apps[0]);
    windows.platform = 'win';
    const filename = path.join(root, 'base/windows-release.json');
    for (const manifest of [
      { schema_version: 1, ...windows },
      { schema_version: 1, apps: [base.apps[1], windows] },
    ]) {
      await fs.writeFile(filename, JSON.stringify(manifest));
      const before = await fs.readFile(catalogPath);
      await assert.rejects(loadCatalog(filename), /Unsupported manifest platform/);
      const rejected = spawnSync(tool, ['--config', config, 'publish', '--release', filename, '--full-only'],
        { cwd: root, encoding: 'utf8', timeout: 30_000, windowsHide: true });
      assert.equal(rejected.error, undefined, rejected.error?.message);
      assert.notEqual(rejected.status, 0);
      assert.match(rejected.stderr, /macOS only|Windows uses JSON/);
      assert.deepEqual(await fs.readFile(catalogPath), before);
    }
  });

  child = spawn(tool, ['--config', config, 'serve'], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  let logs = '';
  child.stdout.on('data', chunk => { logs += chunk; });
  child.stderr.on('data', chunk => { logs += chunk; });
  child.on('error', error => { logs += error.message; });
  const post = (request, suffix = '', headers = {}) => fetch(`${origin}/service/update2/json${suffix}`, {
    method: 'POST', headers: { 'content-type': 'application/json', ...headers },
    body: JSON.stringify(request), signal: AbortSignal.timeout(3000),
  });
  const response = async request => {
    const received = await post(request);
    assert.equal(received.status, 200, await received.clone().text());
    const raw = await received.text();
    assert.equal(raw.startsWith(RESPONSE_PREFIX), true);
    return { raw, value: JSON.parse(raw.slice(RESPONSE_PREFIX.length)) };
  };
  const old = await eventually(async () => {
    assert.equal(child.exitCode, null, logs);
    const result = await response(body('mac', { version: '1.0.0.0' }));
    assert.equal(result.value.response.apps[0].updatecheck.nextversion, '1.0.0.1');
    return result.value;
  }, 'Initial service readiness');
  const oldUrl = old.response.apps[0].updatecheck.pipelines[0].operations[0].urls[0].url;
  const target = await release(path.join(root, 'target'), '1.0.0.2', base);
  const publication = cli('publish', '--release', target.filename);
  assert.deepEqual(publication.warnings, []);
  assert.equal(publication.generated_deltas, 0); // supplied patches, never invoke Puffin on these protocol fixtures
  await eventually(async () => {
    const actual = await response(body());
    assert.equal(actual.value.response.apps[0].updatecheck.nextversion, '1.0.0.2');
  }, 'Published catalog reload');
  catalog = await loadCatalog(catalogPath);
  const baseline = arch => base.apps.find(app => app.arch === arch).full.sha256;

  const scenarios = [
    ['macOS x86_64 full', body()],
    ['macOS x86_64 differential', body('mac', { cached_items: [{ sha256: baseline('x86_64') }] })],
    ['macOS arm64 full', body('mac', {}, { arch: 'arm64' })],
    ['macOS arm64 differential', body('mac', { cached_items: [{ sha256: baseline('arm64') }] }, { arch: 'arm64' })],
    ['cache miss', body('mac', { cached_items: [{ sha256: '0'.repeat(64) }] })],
    ['wrong delta version', body('mac', { version: '1.0.0.0', cached_items: [{ sha256: baseline('x86_64') }] })],
    ['unsupported delta format', body('mac', { cached_items: [{ sha256: baseline('x86_64') }] }, { acceptformat: 'download,crx3' })],
    ['current version', body('mac', { version: '1.0.0.2' })],
    ['newer version', body('mac', { version: '1.0.0.3' })],
    ['minimum version', body('mac', { version: '0' })],
    ['unsupported architecture', body('mac', {}, { arch: 'x86' })],
    ['unsupported platform', body('linux')],
    ['Windows x64 rejected', body('win')],
    ['Windows arm64 rejected', body('win', {}, { arch: 'arm64' })],
    ['conflicting platform', body('win', {}, { '@os': 'mac' })],
    ['disabled updates', body('mac', { updatecheck: { updatedisabled: true } })],
    ['excluded version prefix', body('mac', { updatecheck: { targetversionprefix: '2.' } })],
    ['matching version prefix', body('mac', { updatecheck: { targetversionprefix: '1.' } })],
    ['case insensitive app id', body('mac', { appid: appid.toUpperCase() })],
    ['unknown application', body('mac', { appid: 'unknown.application' })],
    ['ping', body('mac', { updatecheck: undefined })],
    ['empty applications', body('mac', {}, { apps: [] })],
    ['unsupported operation', body('mac', {}, { acceptformat: 'download' })],
  ];
  for (const [name, request] of scenarios) {
    await t.test(name, async () => {
      const actual = await response(request);
      const expected = buildResponse(catalog, request, origin);
      delete actual.value.response.server;
      delete expected.response.server;
      assert.deepEqual(actual.value, expected);
      if (name.startsWith('Windows ')) {
        assert.equal(actual.value.response.apps[0].updatecheck.status, 'error-osnotsupported');
        assert.equal(actual.value.response.apps[0].updatecheck.pipelines, undefined);
      }
      if (verifier && /^macOS (x86_64|arm64) (full|differential)$/.test(name)) {
        const filename = path.join(root, `${name.replaceAll(' ', '-')}.response.json`);
        await fs.writeFile(filename, actual.raw);
        const parsed = run(verifier, [`--response=${filename}`], root);
        assert.equal(parsed.apps[0].version, '1.0.0.2');
        assert.deepEqual(parsed.apps[0].pipelines, expected.response.apps[0].updatecheck.pipelines.map(
          pipeline => pipeline.operations.map(operation => operation.type)));
      }
    });
  }

  await t.test('malformed protocol requests are rejected by both implementations', async () => {
    for (const request of [{}, body('mac', {}, { protocol: '3.1' }), body('mac', { version: '01.2' }),
      body('mac', { cached_items: [{ sha256: 'bad' }] }), body('mac', {}, { apps: [{ appid }, { appid }] })]) {
      assert.throws(() => buildResponse(catalog, request, origin));
      assert.equal((await post(request)).status, 400);
    }
    assert.equal((await post(body(), '', { origin: 'https://untrusted.example' })).status, 403);
    assert.equal((await post(body(), '?cup2key=untrusted')).status, 501);
  });

  await t.test('old artifact URLs, concurrent ranges and HEAD survive catalog replacement', async () => {
    const oldResponse = await fetch(oldUrl);
    assert.equal(oldResponse.status, 200);
    assert.equal(hash(Buffer.from(await oldResponse.arrayBuffer())), baseline('x86_64'));
    const app = catalog.releases.find(app => app.arch === 'x86_64');
    const url = `${origin}/artifacts/${app.full.file}`;
    const source = await fs.readFile(path.join(root, 'target/mac-x86_64.crx3'));
    await Promise.all(Array.from({ length: 8 }, async (_, index) => {
      const start = index * 20;
      const received = await fetch(url, { headers: { range: `bytes=${start}-${start + 19}` } });
      assert.equal(received.status, 206);
      assert.deepEqual(Buffer.from(await received.arrayBuffer()), source.subarray(start, start + 20));
    }));
    const head = await fetch(url, { method: 'HEAD' });
    assert.equal(head.status, 200);
    assert.equal(Number(head.headers.get('content-length')), source.length);
    assert.equal((await head.arrayBuffer()).byteLength, 0);
  });

  await t.test('invalid reload keeps previous release and a repaired catalog recovers', async () => {
    const valid = await fs.readFile(catalogPath);
    await fs.writeFile(catalogPath, '{"schema_version":99}');
    await eventually(async () => assert.match(logs, /Catalog validation failed/), 'Invalid reload rejection');
    assert.equal((await response(body())).value.response.apps[0].updatecheck.nextversion, '1.0.0.2');
    await fs.writeFile(catalogPath, valid);
    const third = await release(path.join(root, 'third'), '1.0.0.3', target);
    cli('publish', '--release', third.filename);
    await eventually(async () => {
      assert.equal((await response(body())).value.response.apps[0].updatecheck.nextversion, '1.0.0.3');
    }, 'Recovered catalog reload');
  });
});
