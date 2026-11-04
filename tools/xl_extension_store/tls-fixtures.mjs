// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {BUILTIN_CA, BUILTIN_CA_FILE, DEFAULT_DATA, DOMAIN, REPO} from './store.mjs';

// Separate valid DNS/IP leaves and a deliberate hostname mismatch exercise the
// browser's standard TLS checks with the same ordinary private CA.
export function createTlsValidationFixtures(directory, openssl) {
  const root = path.resolve(directory);
  const runtimeRoot = path.resolve(DEFAULT_DATA);
  assert(root.startsWith(runtimeRoot + path.sep), 'TLS fixtures must stay in ignored runtime data');
  fs.mkdirSync(root, {recursive: true});
  const caPrivate = path.join(DEFAULT_DATA, 'keys/local_root_ca_private.pem');
  const privateKey = crypto.createPrivateKey(fs.readFileSync(caPrivate));
  const publicKey = crypto.createPublicKey(privateKey).export({type: 'spki', format: 'der'});
  assert(publicKey.equals(BUILTIN_CA.publicKey.export({type: 'spki', format: 'der'})),
    'CA signing key must match the browser built-in root');

  const executable = openssl || path.join(REPO, 'third_party/perl/c/bin/openssl.exe');
  const run = args => {
    const result = spawnSync(executable, args, {encoding: 'utf8', windowsHide: true});
    assert.equal(result.status, 0, result.error?.message || result.stderr);
  };
  const leafKeyPath = path.join(root, 'test-leaf-private.pem');
  if (!fs.existsSync(leafKeyPath)) {
    const pair = crypto.generateKeyPairSync('rsa', {modulusLength: 2048});
    fs.writeFileSync(leafKeyPath, pair.privateKey.export({type: 'pkcs8', format: 'pem'}),
      {mode: 0o600});
  }
  const fixtures = [
    {name: 'child', hostname: `child.${DOMAIN}`, san: `DNS:child.${DOMAIN}`, allowed: true},
    {name: 'outside', hostname: 'outside.test.com', san: 'DNS:outside.test.com', allowed: true},
    {name: 'ip', hostname: '127.0.0.1', san: 'IP:127.0.0.1', allowed: true},
    {name: 'hostname-mismatch', hostname: 'mismatch.test.com',
      certificateHostname: 'different.test.com', san: 'DNS:different.test.com', allowed: false},
  ];
  for (const fixture of fixtures) {
    const config = path.join(root, `${fixture.name}.cnf`);
    const request = path.join(root, `${fixture.name}.csr`);
    fixture.certificatePath = path.join(root, `${fixture.name}.pem`);
    fixture.keyPath = leafKeyPath;
    const certificateHostname = fixture.certificateHostname || fixture.hostname;
    fs.writeFileSync(config, '[req]\nprompt=no\ndistinguished_name=dn\n' +
      `[dn]\nCN=${certificateHostname}\n[extensions]\n` +
      `subjectAltName=${fixture.san}\nbasicConstraints=critical,CA:FALSE\n` +
      'keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' +
      'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n');
    run(['req', '-new', '-sha256', '-key', leafKeyPath, '-config', config, '-out', request]);
    run(['x509', '-req', '-sha256', '-days', '3650', '-in', request,
      '-CA', BUILTIN_CA_FILE, '-CAkey', caPrivate,
      '-set_serial', `0x${crypto.randomBytes(16).toString('hex')}`,
      '-out', fixture.certificatePath, '-extfile', config, '-extensions', 'extensions']);
    const certificate = new crypto.X509Certificate(fs.readFileSync(fixture.certificatePath));
    assert(!certificate.ca && certificate.verify(BUILTIN_CA.publicKey));
    const matchedName = fixture.name === 'ip' ? certificate.checkIP(certificateHostname) :
      certificate.checkHost(certificateHostname);
    assert.equal(matchedName, certificateHostname, 'The issued leaf must contain its intended SAN');
    if (!fixture.allowed) assert.equal(certificate.checkHost(fixture.hostname), undefined);
  }
  return fixtures;
}
