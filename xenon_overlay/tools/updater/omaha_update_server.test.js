// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { buildResponse, compareVersions, createOmahaServer, loadCatalog, RESPONSE_PREFIX } from './omaha_update_server.js';

const source = fileURLToPath(new URL('../../../', import.meta.url));
const data = path.join(source, 'components/test/data/update_client/puffin_patch_test');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const origin = 'http://127.0.0.1:9001';
async function fixture(t, transform = () => {}) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'xenon-omaha-'));
  t.after(() => fs.rm(directory, { recursive: true, force: true }));
  // Copy real, checked-in CRX/Puffin fixtures; the server has no package builder.
  const fullBytes = await fs.readFile(path.join(data, 'puffin_app_v2.crx3'));
  const old = await fs.readFile(path.join(data, 'puffin_app_v1.crx3'));
  const patchBytes = await fs.readFile(path.join(data, 'puffin_app_v1_to_v2.puff'));
  await fs.writeFile(path.join(directory, 'app.crx3'), fullBytes);
  await fs.writeFile(path.join(directory, 'delta.puff'), patchBytes);
  const manifest = { schema_version: 1, appid: 'org.chromium.Chromium', version: '1.0.0.2', platform: 'mac', arch: 'arm64',
    full: { file: 'app.crx3', size: fullBytes.length, sha256: sha(fullBytes) }, differential: [{ type: 'puff',
      file: 'delta.puff', size: patchBytes.length, sha256: sha(patchBytes), from_version: '1.0.0.1', previous_sha256: sha(old) }] };
  await transform(manifest, directory);
  const manifestPath = path.join(directory, 'release.json');
  await fs.writeFile(manifestPath, JSON.stringify(manifest));
  const load = async () => {
    const catalog = await loadCatalog(manifestPath);
    t.after(() => catalog.close());
    return catalog;
  };
  return { directory, manifest, manifestPath, load, fullBytes, patchBytes };
}
function request(catalog, patch = {}, requestPatch = {}) {
  return { request: { protocol: '4.0', acceptformat: 'crx3,download,puff,run,xz,zucc', '@os': 'mac', arch: 'arm64',
    os: { platform: 'Mac OS X', arch: 'arm64' }, apps: [{ appid: catalog.manifest.appid, version: '1.0.0.1', updatecheck: {}, ...patch }], ...requestPatch } };
}
const update = (catalog, patch = {}, requestPatch = {}) => buildResponse(catalog, request(catalog, patch, requestPatch), origin).response.apps[0].updatecheck;
async function listen(t, catalog, options) {
  const server = createOmahaServer(catalog, options);
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  t.after(() => new Promise(resolve => { server.close(resolve); server.closeAllConnections(); }));
  return `http://127.0.0.1:${server.address().port}`;
}
const post = (base, body, suffix = '', headers = {}) => fetch(`${base}/service/update2/json${suffix}`, {
  method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) });

test('numeric versions reject malformed/overflow values and never use lexicographic order', () => {
  assert.equal(compareVersions('1.10', '1.9'), 1);
  assert.equal(compareVersions('1.0', '1.0.0.0'), 0);
  assert.equal(compareVersions('0', '1'), -1);
  for (const value of ['', '01.2', '1.-1', '1.2.3.4.5', '4294967296', '1e2', 1]) assert.throws(() => compareVersions(value, '1'));
});
test('full pipeline exactly follows current Omaha 4.0 download/crx3 schema', async t => {
  const catalog = await (await fixture(t)).load();
  const result = update(catalog);
  assert.equal(result.status, 'ok');
  assert.equal(result.nextversion, '1.0.0.2');
  assert.deepEqual(result.pipelines[0].operations, [{ type: 'download', size: catalog.manifest.full.size,
    out: { sha256: catalog.manifest.full.sha256 }, urls: [{ url: `${origin}/artifacts/app.crx3` }] },
  { type: 'crx3', in: { sha256: catalog.manifest.full.sha256 }, size: catalog.manifest.full.size, path: '.', arguments: '' }]);
});
test('native Chromium parser accepts full and differential response pipelines', {
  skip: !process.env.XENON_VERIFY_TOOL,
}, async t => {
  const fixtureData = await fixture(t);
  const catalog = await fixtureData.load();
  for (const differential of [false, true]) {
    const body = request(catalog, differential ? {
      cached_items: [{ sha256: catalog.manifest.differential[0].previous_sha256 }],
    } : {});
    const response = buildResponse(catalog, body, origin);
    const responsePath = path.join(fixtureData.directory, `response-${differential}.json`);
    await fs.writeFile(responsePath, RESPONSE_PREFIX + JSON.stringify(response));
    const result = spawnSync(process.env.XENON_VERIFY_TOOL, [`--response=${responsePath}`], { encoding: 'utf8' });
    assert.equal(result.error, undefined);
    assert.equal(result.status, 0, result.stderr);
    const parsed = JSON.parse(result.stdout).apps;
    assert.equal(parsed.length, 1);
    assert.equal(parsed[0].appid, catalog.manifest.appid);
    assert.equal(parsed[0].version, catalog.manifest.version);
    assert.deepEqual(parsed[0].pipelines, response.response.apps[0].updatecheck.pipelines.map(
      pipeline => pipeline.operations.map(operation => operation.type)));
  }
});
test('equal or larger differentials are omitted even with matching cached CRX', async t => {
  for (const extraBytes of [0, 1]) {
    const fixtureData = await fixture(t, async (manifest, directory) => {
      const patch = Buffer.alloc(manifest.full.size + extraBytes);
      patch.write('PUF1');
      await fs.writeFile(path.join(directory, 'delta.puff'), patch);
      manifest.differential[0].size = patch.length;
      manifest.differential[0].sha256 = sha(patch);
    });
    const catalog = await fixtureData.load();
    const result = update(catalog, { cached_items: [{ sha256: catalog.manifest.differential[0].previous_sha256 }] });
    assert.equal(result.pipelines.length, 1);
    assert.equal(result.pipelines[0].operations[0].out.sha256, catalog.manifest.full.sha256);
  }
});
test('matching cached CRX and base version offers differential then full fallback', async t => {
  const catalog = await (await fixture(t)).load();
  const diff = catalog.manifest.differential[0];
  const result = update(catalog, { cached_items: [{ sha256: diff.previous_sha256 }] });
  assert.equal(result.pipelines.length, 2);
  assert.deepEqual(result.pipelines[0].operations[1], { type: 'puff', previous: { sha256: diff.previous_sha256 }, out: { sha256: catalog.manifest.full.sha256 } });
  assert.deepEqual(result.pipelines[1], update(catalog).pipelines[0]);
});
test('fingerprint alone, wrong version/hash, or unsupported patch format always chooses full', async t => {
  const catalog = await (await fixture(t)).load();
  const previous = catalog.manifest.differential[0].previous_sha256;
  for (const app of [{ fp: `1.${previous}` }, { fingerprint: previous }, { cached_items: [{ sha256: '0'.repeat(64) }] },
    { version: '1.0.0.0', cached_items: [{ sha256: previous }] }]) assert.equal(update(catalog, app).pipelines.length, 1);
  assert.equal(update(catalog, { cached_items: [{ sha256: previous }] }, { acceptformat: 'crx3,download' }).pipelines.length, 1);
});
test('current/newer versions, disabled updates and target prefix cannot force downgrade', async t => {
  const catalog = await (await fixture(t)).load();
  for (const app of [{ version: '1.0.0.2' }, { version: '1.0.0.3' }, { version: '2', updatecheck: { rollback_allowed: true } },
    { updatecheck: { updatedisabled: true } }, { updatecheck: { targetversionprefix: '2.' } },
    { version: '1.0.0.2', updatecheck: { sameversionupdate: true } }]) assert.equal(update(catalog, app).status, 'noupdate');
});
test('minimum version, OS, binary architecture and accepted operations are enforced', async t => {
  const catalog = await (await fixture(t, manifest => { manifest.minimum_version = '1.0.0.1'; })).load();
  assert.equal(update(catalog, { version: '0' }).status, 'noupdate');
  assert.equal(update(catalog, {}, { os: { platform: 'win' } }).status, 'error-osnotsupported');
  assert.equal(update(catalog, {}, { '@os': 'win' }).status, 'error-osnotsupported');
  assert.equal(update(catalog, {}, { arch: 'x64' }).status, 'error-hwnotsupported');
  assert.equal(update(catalog, {}, { arch: '' }).status, 'error-hwnotsupported');
  assert.equal(update(catalog, {}, { acceptformat: 'download' }).status, 'error-inexpressible');
});
test('appid is case insensitive; unknown app is rejected; ping and empty requests acknowledge without offering install', async t => {
  const catalog = await (await fixture(t)).load();
  assert.equal(update(catalog, { appid: 'ORG.CHROMIUM.CHROMIUM' }).status, 'ok');
  assert.deepEqual(buildResponse(catalog, request(catalog, { appid: 'other.app' }), origin).response.apps[0], { appid: 'other.app', status: 'error-unknownApplication' });
  const ping = request(catalog, { updatecheck: undefined, event: [{ eventtype: 3, eventresult: 1 }] });
  assert.deepEqual(buildResponse(catalog, ping, origin).response.apps[0], { appid: catalog.manifest.appid, status: 'ok' });
  assert.deepEqual(buildResponse(catalog, { request: { protocol: '4.0', apps: [] } }, origin).response.apps, []);
});
test('malformed request/protocol/cached hash/duplicate app is rejected', async t => {
  const catalog = await (await fixture(t)).load();
  for (const body of [{}, { request: {} }, request(catalog, {}, { protocol: '3.1' }), request(catalog, { version: 'bad' }),
    request(catalog, { updatecheck: [] }), request(catalog, { cached_items: [{ sha256: 'bad' }] }),
    request(catalog, {}, { apps: [{ appid: 'x' }, { appid: 'X' }] })]) assert.throws(() => buildResponse(catalog, body, origin));
});
test('one endpoint routes browser and updater app IDs to separate pinned releases', async t => {
  const f = await fixture(t);
  const browser = { ...f.manifest }; delete browser.schema_version;
  const updater = { ...browser, appid: 'com.xenon.XenonUpdater', version: '153.0.8004.0',
    full: { ...browser.full, file: 'updater.crx3' }, differential: [] };
  await fs.copyFile(path.join(f.directory, browser.full.file), path.join(f.directory, updater.full.file));
  await fs.writeFile(f.manifestPath, JSON.stringify({ schema_version: 1, apps: [browser, updater] }));
  const catalog = await f.load();
  const body = request({ manifest: browser }, {}, { apps: [
    { appid: browser.appid, version: '1.0.0.1', updatecheck: {} },
    { appid: updater.appid, version: '153.0.8003.0', updatecheck: {} },
    { appid: 'unknown', version: '0', updatecheck: {} }] });
  const apps = buildResponse(catalog, body, origin).response.apps;
  assert.equal(apps[0].updatecheck.nextversion, browser.version);
  assert.equal(apps[1].updatecheck.nextversion, updater.version);
  assert.match(apps[1].updatecheck.pipelines[0].operations[0].urls[0].url, /\/updater\.crx3$/);
  assert.equal(apps[2].status, 'error-unknownApplication');
  await fs.writeFile(f.manifestPath, JSON.stringify({ schema_version: 1, apps: [browser, browser] }));
  await assert.rejects(f.load, /Duplicate manifest appid/);
});
test('manifest cannot configure executable paths, arguments, URLs or directory traversal', async t => {
  for (const change of [m => { m.arguments = '--run-anything'; }, m => { m.full.url = 'https://other.test/app'; },
    m => { m.full.file = '../app.crx3'; }, m => { m.full.file = '/tmp/app.crx3'; }, m => { m.full.file = 'folder/app.crx3'; },
    m => { m.full.file = '%2e%2e.crx3'; }]) {
    const f = await fixture(t, change); await assert.rejects(f.load);
  }
});
test('manifest validates exact artifact size/hash and actual container header', async t => {
  for (const change of [m => { m.full.size++; }, m => { m.full.sha256 = '0'.repeat(64); }, m => { m.full.sha256 = 'AA'.repeat(32); },
    m => { m.full.size = Number.MAX_SAFE_INTEGER + 1; }, m => { m.full = { ...m.differential[0] }; delete m.full.from_version; delete m.full.previous_sha256; delete m.full.type; }]) {
    const f = await fixture(t, change); await assert.rejects(f.load);
  }
});
test('differential manifest rejects wrong format, duplicate base and non-upgrades', async t => {
  for (const change of [m => { m.differential[0].type = 'zip'; }, m => { m.differential[0].type = 'zucc'; },
    m => { m.differential[0].from_version = m.version; }, m => { m.differential.push({ ...m.differential[0] }); },
    m => { m.differential[0].previous_sha256 = ''; }]) {
    const f = await fixture(t, change); await assert.rejects(f.load);
  }
});
test('symlinks, hardlinks, missing files and non-regular artifact nodes fail closed', async t => {
  for (const kind of ['symlink', 'hardlink', 'missing', 'directory']) {
    const f = await fixture(t, async (m, directory) => {
      const full = path.join(directory, m.full.file), original = path.join(directory, 'original.crx3');
      await fs.rename(full, original);
      if (kind === 'symlink') await fs.symlink(original, full);
      if (kind === 'hardlink') await fs.link(original, full);
      if (kind === 'directory') await fs.mkdir(full);
    });
    await assert.rejects(f.load);
  }
});
test('HTTP endpoint adds anti-XSSI prefix and does not emit a fabricated CUP proof', async t => {
  const catalog = await (await fixture(t)).load();
  const base = await listen(t, catalog);
  const response = await post(base, request(catalog));
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('x-cup-server-proof'), null);
  const body = await response.text();
  assert.ok(body.startsWith(RESPONSE_PREFIX));
  assert.equal(JSON.parse(body.slice(RESPONSE_PREFIX.length)).response.protocol, '4.0');
  for (const query of ['?cup2key=16:nonce', '?cup2hreq=123', '?cup2key=ML-DSA-44-16:nonce']) {
    const result = await post(base, request(catalog), query);
    assert.equal(result.status, 501); assert.match(await result.text(), /CUP/);
  }
});
test('HTTP downloads are exact verified bytes with single range and HEAD support', async t => {
  const f = await fixture(t), catalog = await f.load();
  const base = await listen(t, catalog);
  const full = await fetch(`${base}/artifacts/app.crx3`);
  assert.deepEqual(Buffer.from(await full.arrayBuffer()), f.fullBytes);
  const [range, suffix] = await Promise.all([fetch(`${base}/artifacts/app.crx3`, { headers: { Range: 'bytes=2-13' } }),
    fetch(`${base}/artifacts/app.crx3`, { headers: { Range: 'bytes=-10' } })]);
  assert.equal(range.status, 206);
  assert.deepEqual(Buffer.from(await range.arrayBuffer()), f.fullBytes.subarray(2, 14));
  assert.deepEqual(Buffer.from(await suffix.arrayBuffer()), f.fullBytes.subarray(-10));
  const head = await fetch(`${base}/artifacts/app.crx3`, { method: 'HEAD' });
  assert.equal(Number(head.headers.get('content-length')), f.fullBytes.length); assert.equal(await head.text(), '');
  for (const value of ['bytes=1000000000-', 'bytes=10-2', 'bytes=0-1,3-4', 'bytes=-0']) {
    const result = await fetch(`${base}/artifacts/app.crx3`, { headers: { Range: value } });
    assert.equal(result.status, 416);
  }
});
test('unlisted paths, query tricks, hostile origin and non-local Host are refused', async t => {
  const catalog = await (await fixture(t)).load();
  const base = await listen(t, catalog);
  for (const route of ['/release.json', '/artifacts/../release.json', '/artifacts/%2e%2e%2fapp.crx3', '/artifacts/app.crx3?path=release.json'])
    assert.equal((await fetch(base + route)).status, 404);
  assert.equal((await post(base, request(catalog), '', { Origin: 'https://malicious.example' })).status, 403);
  const status = await new Promise((resolve, reject) => {
    http.get(`${base}/artifacts/app.crx3`, { headers: { Host: 'untrusted.example' } }, res => { res.resume(); resolve(res.statusCode); }).on('error', reject);
  });
  assert.equal(status, 403);
});
test('changed artifact refuses download', async t => {
  const f = await fixture(t), catalog = await f.load();
  const base = await listen(t, catalog);
  const artifact = path.join(f.directory, 'app.crx3');
  await fs.appendFile(artifact, 'tampered');
  assert.equal((await fetch(`${base}/artifacts/app.crx3`)).status, 409);
});
test('replacing a listed artifact path cannot redirect an already verified descriptor', async t => {
  const f = await fixture(t), catalog = await f.load();
  const base = await listen(t, catalog);
  const artifact = path.join(f.directory, 'app.crx3');
  await fs.rename(artifact, artifact + '.old');
  await fs.writeFile(artifact, 'unlisted replacement bytes');
  const response = await fetch(`${base}/artifacts/app.crx3`);
  if (response.status === 200) assert.deepEqual(Buffer.from(await response.arrayBuffer()), f.fullBytes);
  else assert.equal(response.status, 409); // Renaming can change ctime.
});
test('aborted download does not poison a later retry', async t => {
  const f = await fixture(t), catalog = await f.load();
  const base = await listen(t, catalog);
  await new Promise((resolve, reject) => {
    http.get(`${base}/artifacts/app.crx3`, res => {
      res.once('data', () => res.destroy()); res.once('close', resolve);
    }).on('error', reject);
  });
  const retry = await fetch(`${base}/artifacts/app.crx3`);
  assert.equal(retry.status, 200);
  assert.deepEqual(Buffer.from(await retry.arrayBuffer()), f.fullBytes);
});
test('TLS reverse proxy origin must be HTTPS and cannot inject URL paths or credentials', async t => {
  const catalog = await (await fixture(t)).load();
  for (const publicOrigin of ['http://example.test', 'https://example.test/path', 'https://user:pass@example.test', 'https://example.test?q=x'])
    assert.throws(() => createOmahaServer(catalog, { publicOrigin }));
  const base = await listen(t, catalog, { publicOrigin: 'https://updates.example.test' });
  const result = JSON.parse((await (await post(base, request(catalog))).text()).slice(RESPONSE_PREFIX.length));
  assert.equal(result.response.apps[0].updatecheck.pipelines[0].operations[0].urls[0].url, 'https://updates.example.test/artifacts/app.crx3');
});
test('HTTP rejects bad methods, body types, old protocol and oversized request', async t => {
  const catalog = await (await fixture(t)).load();
  const base = await listen(t, catalog);
  assert.equal((await fetch(`${base}/service/update2/json`)).status, 405);
  assert.equal((await post(base, request(catalog), '', { 'Content-Type': 'text/plain' })).status, 415);
  assert.equal((await post(base, request(catalog, {}, { protocol: '3.1' }))).status, 400);
  assert.equal((await post(base, { padding: 'x'.repeat(1024 * 1024) })).status, 413);
});
