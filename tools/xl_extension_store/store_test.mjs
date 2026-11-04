// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import https from 'node:https';
import {spawnSync} from 'node:child_process';
import test from 'node:test';
import {addPublisherProof, makeDeveloperCrx, makeZip, parseCrx, readZip,
  sha256, verifyCrx} from './crx.mjs';
import {BUILTIN_CA, BUILTIN_CA_FILE, CA_SOURCE, createStoreServer, DEFAULT_DATA, DOMAIN, importPackage,
  initRuntime, loadCatalog, REPO, packageInfo, XL_HASH} from './store.mjs';

const publisherKey = fs.readFileSync(path.join(DEFAULT_DATA, 'keys/xl_publisher_private.pem'));
const developerKey = crypto.generateKeyPairSync('rsa', {modulusLength: 2048})
  .privateKey.export({type: 'pkcs8', format: 'pem'});
const manifest = {manifest_version: 3, name: 'Store integration test',
  version: '1.0.0', icons: {'128': 'icon.png'}};
const icon = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=', 'base64');
const developerOnly = makeDeveloperCrx(makeZip({
  'manifest.json': JSON.stringify(manifest), 'icon.png': icon,
}), developerKey);
const signed = addPublisherProof(developerOnly, publisherKey, XL_HASH);

test('developer-only packages are rejected; XL proof preserves developer, ID and ZIP', () => {
  assert.throws(() => verifyCrx(developerOnly, XL_HASH), /publisher proof/);
  const before = parseCrx(developerOnly);
  const after = verifyCrx(signed, XL_HASH);
  assert.equal(after.publisher, 'XL');
  assert.equal(after.id, before.id);
  assert.deepEqual(after.archive, before.archive);
  assert.deepEqual(after.signedHeader, before.signedHeader);
  assert.deepEqual(after.header.subarray(0, before.header.length), before.header);
  assert.deepEqual(after.proofs[0], before.proofs[0]);
  assert.equal(after.proofs.length, before.proofs.length + 1);
  assert.deepEqual(addPublisherProof(signed, publisherKey, XL_HASH), signed);
});

test('all CRX proofs are verified, including publisher signature and developer signature', () => {
  const tamperedArchive = Buffer.from(signed);
  tamperedArchive[tamperedArchive.length - 1] ^= 1;
  assert.throws(() => verifyCrx(tamperedArchive, XL_HASH), /signature verification/);
  const tamperedPublisher = Buffer.from(signed);
  const parsed = parseCrx(signed);
  const signatureOffset = tamperedPublisher.indexOf(parsed.proofs.at(-1).signature);
  tamperedPublisher[signatureOffset] ^= 1;
  assert.throws(() => verifyCrx(tamperedPublisher, XL_HASH), /signature verification/);
  const wrongKey = crypto.generateKeyPairSync('ec', {namedCurve: 'prime256v1'})
    .privateKey.export({type: 'pkcs8', format: 'pem'});
  assert.throws(() => addPublisherProof(developerOnly, wrongKey, XL_HASH), /does not match/);
});

test('metadata comes from verified archive, including manifest and icon', () => {
  const info = packageInfo(signed);
  assert.equal(info.name, manifest.name);
  assert.equal(info.version, manifest.version);
  assert.deepEqual(info.icon, icon);
  assert.equal(info.iconMime, 'image/png');
  assert.deepEqual(JSON.parse(info.manifestText), manifest);
  assert.throws(() => packageInfo(developerOnly), /publisher proof/);
  const archive = readZip(parseCrx(signed).archive);
  assert.deepEqual(JSON.parse(archive.get('manifest.json')), manifest);
});

test('genuine Google publisher package verifies and imports byte-for-byte unchanged', t => {
  const filename = path.join(REPO, 'chrome/test/data/extensions',
    'signin_screen_manual_test_extension/extension_signed_by_webstore.crx');
  const google = fs.readFileSync(filename);
  const verified = verifyCrx(google, XL_HASH);
  assert.equal(verified.publisher, 'Google');
  assert.equal(verified.id, 'ngjobkbdodapjbbncmagbccommkggmnj');
  const metadata = packageInfo(google);
  assert.equal(metadata.manifest.manifest_version, 3);
  assert.equal(metadata.version, '3.0');
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'google-test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const imported = importPackage(filename, temp);
  assert.deepEqual(fs.readFileSync(imported.path), google);
  const withXl = addPublisherProof(google, publisherKey, XL_HASH);
  const after = verifyCrx(withXl, XL_HASH);
  assert.equal(after.publisher, 'Google');
  assert.deepEqual(after.archive, verified.archive);
  assert.deepEqual(after.header.subarray(0, verified.header.length), verified.header);
});

test('ZIP limits and CRC prevent invalid metadata extraction', () => {
  const zip = makeZip({'manifest.json': JSON.stringify(manifest)});
  assert.throws(() => readZip(zip).get('manifest.json', 1), /oversized/);
  const tampered = Buffer.from(zip);
  tampered[30 + 'manifest.json'.length] ^= 1;
  assert.throws(() => readZip(tampered).get('manifest.json'), /CRC mismatch/);
  assert.throws(() => readZip(Buffer.from('not a ZIP')), /end record/);
  assert.throws(() => readZip(makeZip({'../manifest.json': '{}'})), /ZIP path/);
});

function request(port, route, headers = {}) {
  return new Promise((resolve, reject) => {
    const req = https.get({hostname: '127.0.0.1', port, path: route, headers,
      ca: fs.readFileSync(BUILTIN_CA_FILE),
      servername: DOMAIN}, response => {
      const parts = [];
      response.on('data', chunk => parts.push(chunk));
      response.on('end', () => resolve({status: response.statusCode,
        headers: response.headers, data: Buffer.concat(parts)}));
    });
    req.on('error', reject);
  });
}

test('HTTPS catalog, detail, manifest, icon, CRX MIME and independent update XML', async t => {
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const source = path.join(temp, 'source.crx');
  const badSource = path.join(temp, 'developer.crx');
  fs.writeFileSync(source, signed);
  fs.writeFileSync(badSource, developerOnly);
  fs.writeFileSync(path.join(temp, 'managed-ca-snapshot.json'), JSON.stringify({
    schema_version: 1, source_id: CA_SOURCE, generation: '1',
    issued_at: '2026-11-04T00:00:00Z', certificates: [{
      name: 'XL local store CA', role: 'trust_anchor', encoding: 'pem',
      data: fs.readFileSync(BUILTIN_CA_FILE, 'utf8'),
    }],
  }));
  assert.throws(() => importPackage(badSource, temp), /publisher proof/);
  const imported = importPackage(source, temp);
  assert.equal(imported.sha256, sha256(signed));
  assert.deepEqual(fs.readFileSync(imported.path), signed);
  const {server} = createStoreServer({dataDir: temp, port: 443,
    cert: fs.readFileSync(path.join(DEFAULT_DATA, 'tls/server.crt')),
    key: fs.readFileSync(path.join(DEFAULT_DATA, 'keys/tls_server_private.pem'))});
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const port = server.address().port;
  const snapshotResponse = await request(port, '/managed-ca/snapshot');
  assert.equal(snapshotResponse.status, 200);
  const snapshot = JSON.parse(snapshotResponse.data);
  assert.equal(snapshot.schema_version, 1);
  assert.equal(snapshot.source_id, CA_SOURCE);
  assert.match(snapshot.generation, /^[1-9]\d*$/);
  assert.match(snapshot.issued_at, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/);
  assert.equal(snapshot.certificates.length, 1);
  assert.equal(snapshot.certificates[0].role, 'trust_anchor');
  assert.equal(Object.hasOwn(snapshot.certificates[0], 'constraints'), false);
  assert.equal(new crypto.X509Certificate(snapshot.certificates[0].data).ca, true);
  assert.ok(snapshotResponse.headers.etag);
  const unchanged = await request(port, '/managed-ca/snapshot',
    {'If-None-Match': snapshotResponse.headers.etag});
  assert.equal(unchanged.status, 304);
  assert.equal(unchanged.data.length, 0);
  const list = await request(port, '/api/items');
  assert.equal(list.status, 200);
  assert.equal(JSON.parse(list.data)[0].id, imported.id);
  const page = await request(port, `/detail/${imported.id}`);
  assert.equal(page.status, 200);
  const detail = JSON.parse((await request(port, `/api/items/${imported.id}`)).data);
  assert.equal(detail.manifest, JSON.stringify(manifest));
  assert.deepEqual((await request(port, detail.iconUrl)).data, icon);
  assert.equal((await request(port, detail.manifestUrl)).data.toString(), JSON.stringify(manifest));
  const download = await request(port, `/crx/${imported.id}.crx`);
  assert.equal(download.headers['content-type'], 'application/x-chrome-extension');
  assert.deepEqual(download.data, signed);
  const xml = await request(port, `/updates/${imported.id}.xml?x=id%3D${imported.id}%26v%3D0.9%26uc`);
  assert.match(xml.data.toString(), /protocol="2.0"/);
  assert.match(xml.data.toString(), /version="1.0.0"/);
  assert.ok(xml.data.toString().includes(`https://xl.extensions.test.com/crx/${imported.id}/1.0.0.crx`));
  assert.deepEqual((await request(port, `/crx/${imported.id}/1.0.0.crx`)).data, signed);
  assert.equal((await request(port, '/items/../../keys/xl_publisher_private.pem')).status, 404);
  assert.equal((await request(port, '/api/items/not-an-id')).status, 404);
  // Startup does not trust manually copied packages either.
  fs.writeFileSync(imported.path, developerOnly);
  assert.throws(() => loadCatalog(temp), /publisher proof/);
});

test('HTTPS store operates without optional dynamic CA distribution files', async t => {
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'builtin-only-test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const {server} = createStoreServer({dataDir: temp,
    cert: fs.readFileSync(path.join(DEFAULT_DATA, 'tls/server.crt')),
    key: fs.readFileSync(path.join(DEFAULT_DATA, 'keys/tls_server_private.pem'))});
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const port = server.address().port;
  assert.equal((await request(port, '/')).status, 200);
  assert.equal((await request(port, '/api/items')).status, 200);
  assert.equal((await request(port, '/managed-ca/snapshot')).status, 404);
});

test('init migrates a self-signed leaf to the pinned CA and preserves publisher/TLS keys', t => {
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'init-test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const keyDir = path.join(temp, 'keys');
  const tlsDir = path.join(temp, 'tls');
  fs.mkdirSync(keyDir);
  fs.mkdirSync(tlsDir);
  const publisherFile = path.join(keyDir, 'xl_publisher_private.pem');
  const tlsKeyFile = path.join(keyDir, 'tls_server_private.pem');
  const leafFile = path.join(tlsDir, 'server.crt');
  fs.writeFileSync(publisherFile, publisherKey);
  const tlsKey = fs.readFileSync(path.join(DEFAULT_DATA, 'keys/tls_server_private.pem'));
  fs.writeFileSync(tlsKeyFile, tlsKey);
  fs.copyFileSync(path.join(DEFAULT_DATA, 'keys/local_root_ca_private.pem'),
    path.join(keyDir, 'local_root_ca_private.pem'));
  const config = path.join(temp, 'self-signed.cnf');
  fs.writeFileSync(config, `[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n` +
    `[dn]\nCN=${DOMAIN}\n[ext]\nbasicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:${DOMAIN}\n`);
  const openssl = path.join(REPO, 'third_party/perl/c/bin/openssl.exe');
  const initial = spawnSync(openssl, ['req', '-new', '-x509', '-sha256',
    '-key', tlsKeyFile, '-config', config, '-days', '1', '-out', leafFile], {windowsHide: true});
  assert.equal(initial.status, 0, initial.stderr?.toString());
  const oldLeaf = fs.readFileSync(leafFile);
  const result = initRuntime(temp, openssl);
  const caPem = fs.readFileSync(result.localCa);
  const ca = new crypto.X509Certificate(caPem);
  const leaf = new crypto.X509Certificate(fs.readFileSync(leafFile));
  assert.equal(ca.ca, true);
  assert.deepEqual(ca.raw, BUILTIN_CA.raw);
  assert.equal(result.browserTrust, 'built-in');
  assert.equal(result.builtinCaSha256, sha256(BUILTIN_CA.raw));
  assert.equal(leaf.ca, false);
  assert.equal(leaf.checkHost(DOMAIN), DOMAIN);
  assert.equal(leaf.verify(ca.publicKey), true);
  assert.equal(new crypto.X509Certificate(oldLeaf).verify(ca.publicKey), false);
  assert.deepEqual(fs.readFileSync(publisherFile), publisherKey);
  assert.deepEqual(fs.readFileSync(tlsKeyFile), tlsKey);
  const clientConfig = JSON.parse(fs.readFileSync(result.managedConfig)).xenon_managed_ca;
  assert.equal(clientConfig.endpoint, `https://${DOMAIN}/managed-ca/snapshot`);
  assert.equal(clientConfig.source_id, CA_SOURCE);
  assert.equal(path.isAbsolute(clientConfig.bootstrap_ca_file), true);
  assert.deepEqual(clientConfig.auth, {mode: 'none'});
  assert.equal(fs.readFileSync(clientConfig.bootstrap_ca_file).toString(), caPem.toString());
  const snapshotBefore = fs.readFileSync(result.snapshot);
  assert.equal(Object.hasOwn(JSON.parse(snapshotBefore).certificates[0], 'constraints'), false);
  const certBefore = fs.readFileSync(leafFile);
  initRuntime(temp, openssl);
  assert.deepEqual(fs.readFileSync(result.snapshot), snapshotBefore);
  assert.deepEqual(fs.readFileSync(leafFile), certBefore);
});

test('init refuses missing or mismatched built-in CA signing material', t => {
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'init-pin-test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const keyDir = path.join(temp, 'keys');
  const tlsDir = path.join(temp, 'tls');
  fs.mkdirSync(keyDir);
  fs.mkdirSync(tlsDir);
  fs.writeFileSync(path.join(keyDir, 'xl_publisher_private.pem'), publisherKey);
  assert.throws(() => initRuntime(temp), /Matching built-in CA private key required/);
  assert.equal(fs.existsSync(path.join(keyDir, 'local_root_ca_private.pem')), false);
  assert.equal(fs.existsSync(path.join(tlsDir, 'server.crt')), false);

  fs.copyFileSync(BUILTIN_CA_FILE, path.join(tlsDir, 'local-root-ca.pem'));
  fs.writeFileSync(path.join(keyDir, 'local_root_ca_private.pem'), developerKey);
  assert.throws(() => initRuntime(temp), /private key does not match/);
  fs.copyFileSync(path.join(DEFAULT_DATA, 'tls/server.crt'),
    path.join(tlsDir, 'local-root-ca.pem'));
  assert.throws(() => initRuntime(temp), /Local CA does not match/);
});

test('init upgrades the same-key legacy CA and advances the managed snapshot generation', t => {
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'init-ca-migration-test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const keyDir = path.join(temp, 'keys');
  const tlsDir = path.join(temp, 'tls');
  fs.mkdirSync(keyDir);
  fs.mkdirSync(tlsDir);
  fs.writeFileSync(path.join(keyDir, 'xl_publisher_private.pem'), publisherKey);
  const caKeyFile = path.join(keyDir, 'local_root_ca_private.pem');
  fs.copyFileSync(path.join(DEFAULT_DATA, 'keys/local_root_ca_private.pem'), caKeyFile);
  fs.copyFileSync(path.join(DEFAULT_DATA, 'keys/tls_server_private.pem'),
    path.join(keyDir, 'tls_server_private.pem'));
  const originalLeaf = fs.readFileSync(path.join(DEFAULT_DATA, 'tls/server.crt'));
  fs.writeFileSync(path.join(tlsDir, 'server.crt'), originalLeaf);
  const oldRootFile = path.join(tlsDir, 'local-root-ca.pem');
  const config = path.join(temp, 'legacy-root.cnf');
  fs.writeFileSync(config, '[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n' +
    '[dn]\nCN=XL Local Extension Store CA\n[ext]\n' +
    'basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n' +
    'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n');
  const openssl = path.join(REPO, 'third_party/perl/c/bin/openssl.exe');
  const generated = spawnSync(openssl, ['req', '-new', '-x509', '-sha256',
    '-key', caKeyFile, '-config', config, '-days', '10', '-out', oldRootFile],
    {encoding: 'utf8', windowsHide: true});
  assert.equal(generated.status, 0, generated.stderr);
  const legacyPem = fs.readFileSync(oldRootFile, 'utf8');
  const snapshotPath = path.join(temp, 'managed-ca-snapshot.json');
  fs.writeFileSync(snapshotPath, JSON.stringify({schema_version: 1, source_id: CA_SOURCE,
    generation: '8', issued_at: '2026-11-04T00:00:00Z', certificates: [{
      name: 'XL local store CA', role: 'trust_anchor', encoding: 'pem', data: legacyPem,
      constraints: {permitted_dns_names: [DOMAIN]},
    }]}));
  initRuntime(temp, openssl);
  assert.deepEqual(new crypto.X509Certificate(fs.readFileSync(oldRootFile)).raw, BUILTIN_CA.raw);
  assert.deepEqual(fs.readFileSync(path.join(tlsDir, 'server.crt')), originalLeaf);
  const snapshot = JSON.parse(fs.readFileSync(snapshotPath));
  assert.equal(snapshot.generation, '9');
  assert.deepEqual(new crypto.X509Certificate(snapshot.certificates[0].data).raw, BUILTIN_CA.raw);
  assert.equal(Object.hasOwn(snapshot.certificates[0], 'constraints'), false);
  const snapshotBytes = fs.readFileSync(snapshotPath);
  initRuntime(temp, openssl);
  assert.deepEqual(fs.readFileSync(snapshotPath), snapshotBytes);

  // Removing old trust parameters is also a snapshot change when its CA DER
  // already matches the built-in root, so it must advance the generation.
  snapshot.certificates[0].constraints = {permitted_dns_names: [DOMAIN]};
  fs.writeFileSync(snapshotPath, JSON.stringify(snapshot));
  initRuntime(temp, openssl);
  const unconstrainedBytes = fs.readFileSync(snapshotPath);
  const unconstrained = JSON.parse(unconstrainedBytes);
  assert.equal(unconstrained.generation, '10');
  assert.equal(Object.hasOwn(unconstrained.certificates[0], 'constraints'), false);
  assert.deepEqual(new crypto.X509Certificate(unconstrained.certificates[0].data).raw, BUILTIN_CA.raw);
  initRuntime(temp, openssl);
  assert.deepEqual(fs.readFileSync(snapshotPath), unconstrainedBytes);
});

test('init reuses a valid pinned leaf without retaining the CA private key', t => {
  const temp = fs.mkdtempSync(path.join(DEFAULT_DATA, 'init-leaf-test-'));
  t.after(() => fs.rmSync(temp, {recursive: true, force: true}));
  const keyDir = path.join(temp, 'keys');
  const tlsDir = path.join(temp, 'tls');
  fs.mkdirSync(keyDir);
  fs.mkdirSync(tlsDir);
  fs.writeFileSync(path.join(keyDir, 'xl_publisher_private.pem'), publisherKey);
  fs.copyFileSync(path.join(DEFAULT_DATA, 'keys/tls_server_private.pem'),
    path.join(keyDir, 'tls_server_private.pem'));
  const originalLeaf = fs.readFileSync(path.join(DEFAULT_DATA, 'tls/server.crt'));
  fs.writeFileSync(path.join(tlsDir, 'server.crt'), originalLeaf);
  const result = initRuntime(temp);
  assert.deepEqual(fs.readFileSync(result.certificate), originalLeaf);
  assert.deepEqual(new crypto.X509Certificate(fs.readFileSync(result.localCa)).raw,
    BUILTIN_CA.raw);
  assert.equal(fs.existsSync(path.join(keyDir, 'local_root_ca_private.pem')), false);
});
