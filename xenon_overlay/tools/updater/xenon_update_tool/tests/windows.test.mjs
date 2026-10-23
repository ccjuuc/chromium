// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Native Windows JSON + ZIP publication. No installer or installed browser runs.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import crypto from 'node:crypto';
import {once} from 'node:events';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import test from 'node:test';
import {fileURLToPath} from 'node:url';

const tool = process.env.XENON_UPDATE_TOOL;
const oldPackage = process.env.XENON_WINDOWS_OLD_PACKAGE;
const newPackage = process.env.XENON_WINDOWS_NEW_PACKAGE;
const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../../..');
const sevenZip = process.env.XENON_WINDOWS_7ZIP || (process.platform === 'win32'
  ? path.join(repository, 'third_party/lzma_sdk/bin/win64/7za.exe') : '7zz');
const zucchini = process.env.XENON_WINDOWS_ZUCCHINI || path.join(repository,
  'out/Release_64', process.platform === 'win32' ? 'zucchini.exe' : 'zucchini');

async function freePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const result = server.address().port;
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  return result;
}

async function eventually(check, description, timeout = 60000) {
  const deadline = Date.now() + timeout;
  let last;
  while (Date.now() < deadline) {
    try { return await check(); } catch (error) { last = error; }
    await delay(250);
  }
  throw new Error(`${description}: ${last?.message}`, {cause: last});
}

async function stop(child) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const finished = once(child, 'exit');
  child.kill();
  const timeout = setTimeout(() => child.kill('SIGKILL'), 5000);
  try { await finished; } finally { clearTimeout(timeout); }
}

function runNative(executable, args) {
  const result = spawnSync(executable, args, {windowsHide: true, encoding: 'utf8',
    maxBuffer: 16 * 1024 * 1024, timeout: 5 * 60 * 1000});
  assert.equal(result.error, undefined, result.error?.message);
  assert.equal(result.status, 0, `${executable}: ${result.stdout}\n${result.stderr}`);
}

async function sha256(file) {
  const hash = crypto.createHash('sha256');
  for await (const bytes of fs.createReadStream(file)) hash.update(bytes);
  return hash.digest('hex').toUpperCase();
}

async function download(packageInfo, destination) {
  const response = await fetch(packageInfo.url);
  assert.equal(response.status, 200);
  assert.equal(Number(response.headers.get('content-length')), packageInfo.size);
  const hash = crypto.createHash('sha256');
  let size = 0;
  const output = destination ? await fsp.open(destination, 'wx') : null;
  try {
    for await (const chunk of response.body) {
      hash.update(chunk);
      size += chunk.length;
      if (output) {
        let written = 0;
        while (written < chunk.length) {
          const result = await output.write(chunk, written, chunk.length - written);
          written += result.bytesWritten;
        }
      }
    }
  } finally { await output?.close(); }
  assert.equal(size, packageInfo.size);
  assert.equal(hash.digest('hex').toUpperCase(), packageInfo.sha256.toUpperCase());
}

async function filesUnder(directory, prefix = '') {
  const result = [];
  for (const item of await fsp.readdir(directory, {withFileTypes: true})) {
    const relative = prefix ? `${prefix}/${item.name}` : item.name;
    assert.equal(item.isSymbolicLink(), false);
    if (item.isDirectory()) result.push(...await filesUnder(path.join(directory, item.name), relative));
    else { assert.equal(item.isFile(), true); result.push(relative); }
  }
  return result.sort();
}

function safePath(root, relative) {
  assert.equal(typeof relative, 'string');
  assert.ok(relative && !relative.includes('\\') && !relative.includes(':'));
  assert.ok(relative.split('/').every(part => part && part !== '.' && part !== '..'));
  return path.join(root, relative);
}

async function verifyReconstruction(diffZip, beforeZip, expectedTree, directory) {
  const oldTree = path.join(directory, 'old-tree');
  const diffTree = path.join(directory, 'diff-tree');
  runNative(sevenZip, ['x', '-tzip', beforeZip, `-o${oldTree}`, '-y']);
  runNative(sevenZip, ['x', '-tzip', diffZip, `-o${diffTree}`, '-y']);
  const manifest = JSON.parse(await fsp.readFile(path.join(diffTree, 'manifest.json'), 'utf8'));
  assert.equal(manifest.platform, 'win');
  assert.deepEqual(manifest.actions.map(action => action.target).sort(), await filesUnder(expectedTree));
  const counts = {copy: 0, patch: 0, add: 0};
  for (const action of manifest.actions) {
    const expected = safePath(expectedTree, action.target);
    assert.equal(await sha256(expected), action.sha256.toUpperCase());
    let reconstructed;
    if (action.action === 'copy') reconstructed = safePath(oldTree, action.base);
    else if (action.action === 'add') reconstructed = safePath(diffTree, action.source);
    else {
      assert.equal(action.action, 'patch');
      reconstructed = path.join(directory, 'verified-patch.tmp');
      runNative(zucchini, ['-apply', safePath(oldTree, action.base), safePath(diffTree, action.patch), reconstructed]);
    }
    assert.equal((await fsp.stat(reconstructed)).size, action.size);
    assert.equal(await sha256(reconstructed), action.sha256.toUpperCase(), action.target);
    counts[action.action] += 1;
    if (action.action === 'patch') await fsp.unlink(reconstructed);
  }
  assert.ok(counts.patch > 0, 'Real fixture must exercise native Zucchini patch replay');
  for (const relative of manifest.deletions) {
    assert.equal(fs.existsSync(safePath(expectedTree, relative)), false);
  }
  await fsp.rm(oldTree, {recursive: true, force: true});
  await fsp.rm(diffTree, {recursive: true, force: true});
  return {base_version: manifest.base_version, target_version: manifest.target_version, actions: counts};
}

test('Windows publishing keeps JSON + ZIP, adds late baselines, and survives restart', {
  skip: !(tool && oldPackage && newPackage) && 'Set XENON_UPDATE_TOOL and XENON_WINDOWS_OLD_PACKAGE/NEW_PACKAGE to native ZIP fixtures',
  timeout: 30 * 60 * 1000,
}, async t => {
  const oldMetadata = JSON.parse(await fsp.readFile(path.join(path.dirname(oldPackage), 'release.json'), 'utf8'));
  const newMetadata = JSON.parse(await fsp.readFile(path.join(path.dirname(newPackage), 'release.json'), 'utf8'));
  assert.equal(oldMetadata.arch, newMetadata.arch);
  const expectedTree = process.env.XENON_WINDOWS_NEW_TREE || path.join(path.dirname(newPackage), 'application');
  const outputRoot = process.env.XENON_WINDOWS_TEST_ROOT || os.tmpdir();
  await fsp.mkdir(outputRoot, {recursive: true});
  const directory = await fsp.mkdtemp(path.join(outputRoot, 'xenon-windows-http-'));
  const configPath = path.join(directory, 'state', 'xenon-update.json');
  const port = await freePort();
  const origin = `http://127.0.0.1:${port}`;
  const init = spawnSync(tool, ['--config', configPath, 'init', '--port', String(port)],
    {encoding: 'utf8', windowsHide: true, timeout: 30000});
  assert.equal(init.status, 0, `${init.stdout}\n${init.stderr}`);
  let child, token, diagnostics = '';
  let packageServer;
  const result = {origin, old_version: oldMetadata.version, new_version: newMetadata.version,
    arch: newMetadata.arch, started_at: new Date().toISOString()};
  t.after(async () => {
    await stop(child);
    if (packageServer) await new Promise(resolve => packageServer.close(resolve));
    result.finished_at = new Date().toISOString();
    if (process.env.XENON_WINDOWS_RESULT) {
      await fsp.writeFile(process.env.XENON_WINDOWS_RESULT, JSON.stringify(result, null, 2) + '\n');
      await fsp.writeFile(`${process.env.XENON_WINDOWS_RESULT}.server.log`, diagnostics);
    }
    await fsp.rm(directory, {recursive: true, force: true});
  });
  async function start() {
    child = spawn(tool, ['--config', configPath, 'serve'], {windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
    child.stdout.on('data', bytes => { diagnostics = (diagnostics + bytes).slice(-1000000); });
    child.stderr.on('data', bytes => { diagnostics = (diagnostics + bytes).slice(-1000000); });
    const home = await eventually(async () => {
      assert.equal(child.exitCode, null, diagnostics);
      const response = await fetch(origin);
      assert.equal(response.status, 200, diagnostics);
      return response.text();
    }, 'server starts');
    token = home.match(/name="xenon-admin-token"\s+content="([a-f0-9]{64})"/)?.[1];
    assert.ok(token);
  }
  async function admin(route, body) {
    const response = await fetch(`${origin}${route}`, {method: body === undefined ? 'GET' : 'POST',
      headers: {'x-xenon-admin': token, origin, ...(body === undefined ? {} : {'content-type': 'application/json'})},
      ...(body === undefined ? {} : {body: JSON.stringify(body)})});
    const value = await response.json();
    assert.ok(response.ok, `${route}: HTTP ${response.status} ${JSON.stringify(value)}`);
    return value;
  }
  async function finish(job) {
    let stage = '';
    const state = await eventually(async () => {
      const state = await admin('/admin/state');
      assert.equal(state.job?.id, job.id);
      if (state.job?.stage !== stage) { stage = state.job?.stage; t.diagnostic(stage); }
      assert.equal(state.busy, false);
      return state;
    }, 'publication finishes', 15 * 60 * 1000);
    assert.equal(state.job.status, 'succeeded', state.job.error || diagnostics);
    return state;
  }
  async function feed(version, arch) {
    const query = new URLSearchParams({platform: 'win', version, ...(arch ? {arch} : {})});
    const response = await fetch(`${origin}/api/v1/update/check?${query}`);
    assert.equal(response.status, 200, await response.clone().text());
    return response.json();
  }
  await start();
  const settings = {node: process.execPath, seven_zip: sevenZip, zucchini,
    default_arch: newMetadata.arch, changelog: 'Windows JSON + ZIP 原生回归验证', force_update: false};

  await t.test('Windows settings are independent and do not require CRX signing', async () => {
    const before = await admin('/admin/state');
    assert.equal(before.config.key_path, null);
    assert.equal((await admin('/admin/windows/config', settings)).saved, true);
    const after = await admin('/admin/state');
    assert.deepEqual(after.config, before.config);
    assert.equal(after.windows.config.default_arch, newMetadata.arch);
    assert.equal(after.windows.endpoint, `${origin}/api/v1/update/check`);
    assert.deepEqual(after.windows.history, []);
    for (const route of ['/admin/windows/config', '/admin/windows/import', '/admin/windows/upload']) {
      assert.equal((await fetch(`${origin}${route}`, {method: 'POST'})).status, 403);
    }
  });

  let originalFull;
  await t.test('newest path import publishes a full ZIP without a signing key', async () => {
    const accepted = await admin('/admin/windows/import', {source_type: 'path', source: newPackage});
    const state = await finish(accepted.job);
    assert.equal(state.windows.history.length, 1);
    assert.equal(state.windows.releases[0].version, newMetadata.version);
    assert.equal(state.job.platform, 'win');
    const update = await feed(oldMetadata.version);
    assert.equal(update.target_version, newMetadata.version);
    assert.equal(update.is_diff, false);
    assert.equal(update.changelog, settings.changelog);
    assert.match(update.full_package.url, /\/windows\/artifacts\/[a-f0-9]{64}\.zip$/);
    originalFull = update.full_package;
  });

  let differential;
  await t.test('an older URL import adds an exact baseline without downgrading latest', async () => {
    packageServer = http.createServer((request, response) => {
      if (request.url !== '/older.zip') { response.writeHead(404); response.end(); return; }
      response.writeHead(200, {'content-type': 'application/zip', 'content-length': fs.statSync(oldPackage).size});
      fs.createReadStream(oldPackage).pipe(response);
    });
    packageServer.listen(0, '127.0.0.1');
    await once(packageServer, 'listening');
    const source = `http://127.0.0.1:${packageServer.address().port}/older.zip`;
    const accepted = await admin('/admin/windows/import', {source_type: 'url', source});
    const state = await finish(accepted.job);
    assert.equal(state.windows.history.length, 2);
    assert.equal(state.windows.releases[0].version, newMetadata.version);
    differential = await feed(oldMetadata.version);
    assert.equal(differential.is_diff, true);
    assert.ok(differential.diff_package.size < differential.full_package.size);
    assert.deepEqual(differential.full_package, originalFull);
    assert.equal((await feed('0.0.0.0')).is_diff, false);
    assert.equal((await feed(newMetadata.version)).is_diff, false);
    const otherArch = newMetadata.arch === 'arm64' ? 'x86_64' : 'arm64';
    assert.ok((await fetch(`${origin}/api/v1/update/check?platform=win&version=${oldMetadata.version}&arch=${otherArch}`)).status >= 400);
    await admin('/admin/windows/config', {...settings, default_arch: otherArch});
    assert.ok((await fetch(`${origin}/api/v1/update/check?platform=win&version=${oldMetadata.version}`)).status >= 400);
    assert.equal((await feed(oldMetadata.version, newMetadata.arch)).is_diff, true);
    await admin('/admin/windows/config', settings);
    result.full = differential.full_package;
    result.diff = differential.diff_package;
  });

  await t.test('multipart reimport of an existing baseline preserves history and full download URLs', async () => {
    const before = await admin('/admin/state');
    const form = new FormData();
    form.append('file', await fs.openAsBlob(oldPackage, {type: 'application/zip'}), 'older.zip');
    form.append('version', oldMetadata.version);
    form.append('arch', oldMetadata.arch);
    const response = await fetch(`${origin}/admin/windows/upload`, {method: 'POST',
      headers: {'x-xenon-admin': token, origin}, body: form});
    const accepted = await response.json();
    assert.equal(response.status, 202, JSON.stringify(accepted));
    const state = await finish(accepted.job);
    assert.equal(state.job.platform, 'win');
    assert.equal(state.job.result.imported_version, oldMetadata.version);
    assert.deepEqual(state.windows.history, before.windows.history);
    assert.equal(state.windows.history.length, 2);
    assert.equal(state.windows.releases[0].version, newMetadata.version);
    differential = await feed(oldMetadata.version);
    assert.equal(differential.is_diff, true);
    assert.deepEqual(differential.full_package, originalFull);
    assert.equal((await fetch(originalFull.url, {method: 'HEAD'})).status, 200);
    result.multipart_reimport = {imported_version: state.job.result.imported_version,
      history_count: state.windows.history.length, full_url_unchanged: true};
    result.diff = differential.diff_package;
  });

  const diffZip = path.join(directory, 'downloaded-diff.zip');
  await t.test('full and diff downloads verify hashes and support HEAD and Range', async () => {
    await download(differential.full_package);
    await download(differential.diff_package, diffZip);
    for (const spec of [differential.full_package, differential.diff_package]) {
      const head = await fetch(spec.url, {method: 'HEAD'});
      assert.equal(head.status, 200);
      assert.equal(Number(head.headers.get('content-length')), spec.size);
      assert.equal((await head.arrayBuffer()).byteLength, 0);
      const range = await fetch(spec.url, {headers: {range: 'bytes=0-3'}});
      assert.equal(range.status, 206);
      assert.equal(range.headers.get('content-range'), `bytes 0-3/${spec.size}`);
      assert.deepEqual(Buffer.from(await range.arrayBuffer()), Buffer.from('PK\x03\x04'));
    }
  });

  await t.test('native patch actions reconstruct every target file from the installed-tree baseline', async () => {
    result.reconstruction = await verifyReconstruction(diffZip, oldPackage, expectedTree, directory);
    assert.equal(result.reconstruction.base_version, oldMetadata.version);
    assert.equal(result.reconstruction.target_version, newMetadata.version);
  });

  await t.test('restart retains history, exact diff selection and earlier download URLs', async () => {
    await stop(child);
    await start();
    const state = await admin('/admin/state');
    assert.equal(state.windows.history.length, 2);
    const after = await feed(oldMetadata.version);
    assert.deepEqual(after, differential);
    assert.equal((await fetch(originalFull.url, {method: 'HEAD'})).status, 200);
    await download(after.diff_package);
    result.ok = true;
  });
});
