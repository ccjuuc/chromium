#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {readZip, sha256} from './crx.mjs';
import {decryptArchive} from './protect-runtime.mjs';
import {
  BUILTIN_CA, createDemo, DEFAULT_DATA, DOMAIN, importPackage, initRuntime,
  loadCatalog, REPO, SOURCE, XL_HASH,
} from './store.mjs';

// These are deliberately shared development credentials. Keeping the password
// beside the archive makes a fresh checkout usable without a separate transfer;
// the encrypted archive does not keep these test keys secret from repo readers.
const TEST_PASSWORD_FILE = path.join(SOURCE, 'test-bundle-password.txt');
const TEST_BUNDLE = path.join(SOURCE, 'xl-store-full.zip.enc');
const GOOGLE_FIXTURE = path.join(REPO,
  'chrome/test/data/extensions/signin_screen_manual_test_extension/extension_signed_by_webstore.crx');
const FIXED_ENTRIES = new Set([
  'tls/server.crt', 'tls/local-root-ca.pem',
  'keys/tls_server_private.pem', 'keys/local_root_ca_private.pem',
  'keys/xl_publisher_private.pem', 'keys/demo_developer_private.pem',
  'demo/developer-only.crx', 'demo/xl-signed.crx', 'MIGRATION.txt',
]);
const PACKAGE_ENTRY = /^packages\/[a-p]{32}\/(?:0|[1-9]\d*)(?:\.(?:0|[1-9]\d*)){0,3}\.crx$/;
const usage = `Usage: node tools/xl_extension_store/setup-runtime.mjs [options]
  --data-dir <directory>  Default: out/xl_extension_store
  --openssl <executable>  Used by init when the HTTPS certificate needs renewal
  --help                 Show this help

Restores the shared test bundle with its bundled test password, initializes TLS,
and prepares the XL and original Google samples. Existing matching runtimes keep
their keys and packages; incomplete or unrelated directories are rejected.
Does not change hosts, system certificates, browser pins, or start a server.`;

function validateIdentity(dataDir) {
  const required = ['keys/xl_publisher_private.pem',
    'keys/local_root_ca_private.pem', 'tls/local-root-ca.pem'];
  if (required.some(name => !fs.existsSync(path.join(dataDir, name)))) {
    throw new Error('Incomplete runtime. Select a new or empty --data-dir, or ' +
      'restore the missing files from your existing environment. No files were replaced.');
  }
  const publicKey = name => crypto.createPublicKey(
    fs.readFileSync(path.join(dataDir, 'keys', name)))
    .export({type: 'spki', format: 'der'});
  if (sha256(publicKey('xl_publisher_private.pem')) !== XL_HASH) {
    throw new Error('Publisher key does not match the browser pin; existing runtime was not changed');
  }
  const ca = new crypto.X509Certificate(
    fs.readFileSync(path.join(dataDir, 'tls/local-root-ca.pem')));
  if (!ca.raw.equals(BUILTIN_CA.raw) ||
      !publicKey('local_root_ca_private.pem').equals(
        BUILTIN_CA.publicKey.export({type: 'spki', format: 'der'}))) {
    throw new Error('CA identity does not match the built-in root; existing runtime was not changed');
  }
  // Check all existing packages before init/demo can update runtime files.
  loadCatalog(dataDir);
}

function prepareRuntime(dataDir, openssl) {
  validateIdentity(dataDir);
  const tls = initRuntime(dataDir, openssl);
  const xl = createDemo(dataDir);
  const google = importPackage(GOOGLE_FIXTURE, dataDir);
  return {tls, samples: {xl: xl.id, google: google.id}};
}

function extractBundle(archive, directory) {
  const zip = readZip(archive);
  // readZip is also used for CRX metadata. This restore operation accepts only
  // the exporter's runtime paths, excluding drive/ADS/case aliases on Windows.
  for (const name of zip.names) {
    if (!FIXED_ENTRIES.has(name) && !PACKAGE_ENTRY.test(name)) {
      throw new Error(`Unexpected runtime archive entry: ${name}`);
    }
  }
  for (const name of FIXED_ENTRIES) {
    if (!zip.names.includes(name)) throw new Error(`Incomplete test bundle: ${name}`);
  }
  for (const name of zip.names) {
    const filename = path.join(directory, name);
    const bytes = zip.get(name, 256 * 1024 * 1024);
    fs.mkdirSync(path.dirname(filename), {recursive: true});
    fs.writeFileSync(filename, bytes, {flag: 'wx', mode: 0o600});
  }
}

export function setupRuntime({dataDir = DEFAULT_DATA, openssl} = {}) {
  dataDir = path.resolve(dataDir);
  const existing = fs.existsSync(dataDir);
  if (existing && (!fs.lstatSync(dataDir).isDirectory() ||
      fs.lstatSync(dataDir).isSymbolicLink())) {
    throw new Error('Runtime must be a regular directory');
  }
  let restored = false;
  if (!existing || fs.readdirSync(dataDir).length === 0) {
    const parent = path.dirname(dataDir);
    fs.mkdirSync(parent, {recursive: true});
    const staging = fs.mkdtempSync(path.join(parent, '.xl-store-setup-'));
    try {
      const archive = path.join(staging, 'runtime.zip');
      decryptArchive({input: TEST_BUNDLE, output: archive,
        passwordFile: TEST_PASSWORD_FILE});
      const runtime = path.join(staging, 'runtime');
      extractBundle(fs.readFileSync(archive), runtime);
      prepareRuntime(runtime, openssl);
      // Complete validation and any certificate renewal before publishing the
      // restored directory. Never merge the bundle into a populated runtime.
      if (existing) fs.rmdirSync(dataDir);
      fs.renameSync(runtime, dataDir);
      restored = true;
    } finally {
      fs.rmSync(staging, {recursive: true, force: true});
    }
  }
  // init rewrites the optional managed-CA config with the final absolute paths.
  const result = prepareRuntime(dataDir, openssl);
  const quote = value => `'${value.replace(/['\u2018-\u201b]/g, char => char + char)}'`;
  return {dataDir, restored, ...result,
    hosts: `127.0.0.1 ${DOMAIN}`,
    serveCommand: `& node ${quote(path.join(SOURCE, 'store.mjs'))} serve --data-dir ${quote(dataDir)}`,
    browserPage: 'chrome://xenon-extension-store/',
    note: 'Shared test keys restored. Use the matching browser; restart an existing store after setup.'};
}

function main() {
  const args = process.argv.slice(2);
  const options = {};
  for (let index = 0; index < args.length; index += 2) {
    if (args[index] === '--help' || args[index] === '-h') {
      console.log(usage);
      return;
    }
    const name = {'--data-dir': 'dataDir', '--openssl': 'openssl'}[args[index]];
    if (!name || !args[index + 1] || args[index + 1].startsWith('--')) {
      throw new Error('Invalid arguments. Use --help for usage.');
    }
    options[name] = args[index + 1];
  }
  console.log(JSON.stringify(setupRuntime(options), null, 2));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
