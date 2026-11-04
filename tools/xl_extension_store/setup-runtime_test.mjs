// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import https from 'node:https';
import path from 'node:path';
import test from 'node:test';
import {sha256} from './crx.mjs';
import {setupRuntime} from './setup-runtime.mjs';
import {
  BUILTIN_CA, BUILTIN_CA_FILE, createStoreServer, DEFAULT_DATA, DOMAIN,
  loadCatalog, REPO, XL_HASH,
} from './store.mjs';

function fixture(t) {
  const parent = path.dirname(DEFAULT_DATA);
  fs.mkdirSync(parent, {recursive: true});
  const directory = fs.mkdtempSync(path.join(parent, 'xl-store-setup-test-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  return {directory, dataDir: path.join(directory, 'runtime')};
}

function snapshot(directory, prefix = '') {
  const files = new Map();
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    const relative = path.join(prefix, entry.name);
    const filename = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      for (const [name, bytes] of snapshot(filename, relative)) files.set(name, bytes);
    } else {
      files.set(relative, fs.readFileSync(filename));
    }
  }
  return files;
}

function assertUnchanged(directory, before) {
  const after = snapshot(directory);
  assert.deepEqual([...after.keys()].sort(), [...before.keys()].sort());
  for (const [name, bytes] of before) {
    // Never include private key or password bytes in assertion diagnostics.
    assert.equal(after.get(name).equals(bytes), true, `${name} must remain unchanged`);
  }
}

function publicKey(filename) {
  return crypto.createPublicKey(fs.readFileSync(filename))
    .export({type: 'spki', format: 'der'});
}

function request(port, route) {
  return new Promise((resolve, reject) => {
    const req = https.get({hostname: '127.0.0.1', port, path: route,
      servername: DOMAIN, ca: fs.readFileSync(BUILTIN_CA_FILE)}, response => {
      const authorized = response.socket.authorized;
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('error', reject);
      response.on('end', () => resolve({status: response.statusCode, authorized,
        data: Buffer.concat(chunks)}));
    });
    req.on('error', reject);
  });
}

test('fresh setup serves XL and unchanged Google packages with verified HTTPS', async t => {
  const f = fixture(t);
  const result = setupRuntime({dataDir: f.dataDir});
  assert.equal(result.dataDir, f.dataDir);
  assert.equal(result.restored, true);
  assert.equal(fs.existsSync(path.join(f.dataDir, 'keys/runtime-bundle-password.txt')), false,
    'Setup must not seed private archive encryption with the shared test password');
  assert.equal(sha256(publicKey(path.join(f.dataDir, 'keys/xl_publisher_private.pem'))), XL_HASH);
  assert.equal(publicKey(path.join(f.dataDir, 'keys/local_root_ca_private.pem'))
    .equals(BUILTIN_CA.publicKey.export({type: 'spki', format: 'der'})), true);
  const config = JSON.parse(fs.readFileSync(path.join(f.dataDir, 'managed-ca.json')));
  assert.equal(config.xenon_managed_ca.bootstrap_ca_file,
    path.join(f.dataDir, 'tls/local-root-ca.pem').replaceAll('\\', '/'));
  assert.deepEqual(fs.readdirSync(f.directory), ['runtime'],
    'Temporary plaintext archives and staging directories must be removed');

  const {server} = createStoreServer({dataDir: f.dataDir});
  t.after(() => new Promise(resolve => server.close(resolve)));
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  const response = await request(port, '/api/items');
  assert.equal(response.status, 200);
  assert.equal(response.authorized, true);
  const items = JSON.parse(response.data);
  assert.deepEqual(items.map(item => item.publisher).sort(), ['Google', 'XL']);
  const catalog = loadCatalog(f.dataDir);
  for (const item of items) {
    const download = await request(port, `/crx/${item.id}.crx`);
    assert.equal(download.status, 200);
    assert.equal(download.authorized, true);
    assert.equal(download.data.equals(catalog.get(item.id).get(item.version).data), true);
    if (item.publisher === 'Google') {
      const original = fs.readFileSync(path.join(REPO, 'chrome/test/data/extensions',
        'signin_screen_manual_test_extension/extension_signed_by_webstore.crx'));
      assert.equal(download.data.equals(original), true,
        'Google CRX must retain its original bytes and signatures');
    }
  }
});

test('repeated setup preserves keys, valid certificates, packages and existing files', t => {
  const f = fixture(t);
  setupRuntime({dataDir: f.dataDir});
  fs.mkdirSync(path.join(f.dataDir, 'browser-profile'));
  fs.writeFileSync(path.join(f.dataDir, 'browser-profile/marker'), 'keep profile');
  fs.writeFileSync(path.join(f.dataDir, 'notes.txt'), 'keep local notes');
  fs.writeFileSync(path.join(f.dataDir, 'keys/runtime-bundle-password.txt'),
    crypto.randomBytes(32).toString('base64url') + '\n', {mode: 0o600});
  const before = snapshot(f.dataDir);
  const result = setupRuntime({dataDir: f.dataDir,
    openssl: path.join(f.directory, 'openssl-must-not-run.exe')});
  assert.equal(result.restored, false);
  assertUnchanged(f.dataDir, before);
});

test('a missing HTTPS certificate is reissued under the same CA without replacing keys', t => {
  const f = fixture(t);
  setupRuntime({dataDir: f.dataDir});
  const keys = snapshot(path.join(f.dataDir, 'keys'));
  const packages = snapshot(path.join(f.dataDir, 'packages'));
  const certFile = path.join(f.dataDir, 'tls/server.crt');
  fs.unlinkSync(certFile);
  const result = setupRuntime({dataDir: f.dataDir});
  assert.equal(result.restored, false);
  const certificate = new crypto.X509Certificate(fs.readFileSync(certFile));
  assert.equal(certificate.checkHost(DOMAIN), DOMAIN);
  assert.equal(certificate.verify(BUILTIN_CA.publicKey), true);
  assert.equal(certificate.checkIssued(BUILTIN_CA), true);
  assert.ok(Date.parse(certificate.validFrom) <= Date.now());
  assert.ok(Date.parse(certificate.validTo) > Date.now());
  assert.equal(certificate.publicKey.export({type: 'spki', format: 'der'})
    .equals(publicKey(path.join(f.dataDir, 'keys/tls_server_private.pem'))), true);
  assertUnchanged(path.join(f.dataDir, 'keys'), keys);
  assertUnchanged(path.join(f.dataDir, 'packages'), packages);
});

test('an incomplete nonempty runtime is rejected without overwriting its files', t => {
  const f = fixture(t);
  fs.mkdirSync(path.join(f.dataDir, 'keys'), {recursive: true});
  fs.writeFileSync(path.join(f.dataDir, 'keys/owner.txt'), 'existing material');
  const before = snapshot(f.dataDir);
  assert.throws(() => setupRuntime({dataDir: f.dataDir}), /incomplete|empty|restore|runtime/i);
  assertUnchanged(f.dataDir, before);
  assert.deepEqual(fs.readdirSync(f.directory), ['runtime']);
});

for (const name of ['xl_publisher_private.pem', 'local_root_ca_private.pem']) {
  test(`a mismatched ${name} is rejected without changing the runtime`, t => {
    const f = fixture(t);
    setupRuntime({dataDir: f.dataDir});
    const wrong = crypto.generateKeyPairSync('ec', {namedCurve: 'prime256v1'})
      .privateKey.export({type: 'pkcs8', format: 'pem'});
    fs.writeFileSync(path.join(f.dataDir, 'keys', name), wrong, {mode: 0o600});
    const before = snapshot(f.dataDir);
    assert.throws(() => setupRuntime({dataDir: f.dataDir}), /match|publisher|CA|identity/i);
    assertUnchanged(f.dataDir, before);
  });
}
