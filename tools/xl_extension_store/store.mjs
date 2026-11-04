#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import fs from 'node:fs';
import path from 'node:path';
import https from 'node:https';
import crypto from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {
  addPublisherProof, makeDeveloperCrx, makeZip, readCrxFile, readZip, sha256,
  verifyCrx,
} from './crx.mjs';

export const SOURCE = path.dirname(fileURLToPath(import.meta.url));
export const REPO = path.resolve(SOURCE, '../..');
export const DEFAULT_DATA = path.join(REPO, 'out/xl_extension_store');
export const DOMAIN = 'xl.extensions.test.com';
export const CA_SOURCE = 'xl-extension-store-local';
export const BUILTIN_CA_FILE = path.join(SOURCE, 'xl_store_root_ca.pem');
export const BUILTIN_CA = new crypto.X509Certificate(fs.readFileSync(BUILTIN_CA_FILE));
export const XL_HASH = sha256(fs.readFileSync(path.join(SOURCE, 'xl_publisher_public_key.der')));
const ID_PATTERN = /^[a-p]{32}$/;
const VERSION_PATTERN = /^(?:0|[1-9]\d*)(?:\.(?:0|[1-9]\d*)){0,3}$/;

function json(data) {
  return JSON.stringify(data, null, 2) + '\n';
}

function hasBuiltinCaIdentity(certificate) {
  return certificate.ca && certificate.subject === BUILTIN_CA.subject &&
    certificate.verify(certificate.publicKey) &&
    certificate.publicKey.export({type: 'spki', format: 'der'}).equals(
      BUILTIN_CA.publicKey.export({type: 'spki', format: 'der'}));
}

function checkVersion(version) {
  if (typeof version !== 'string' || !VERSION_PATTERN.test(version) ||
      version.split('.').some(part => Number(part) > 65535)) {
    throw new Error('Invalid extension version');
  }
  return version;
}

export function compareVersion(a, b) {
  const left = a.split('.').map(Number);
  const right = b.split('.').map(Number);
  for (let index = 0; index < 4; ++index) {
    if ((left[index] || 0) !== (right[index] || 0)) {
      return (left[index] || 0) - (right[index] || 0);
    }
  }
  return 0;
}

function localized(value, messages) {
  const match = typeof value === 'string' && /^__MSG_(.+)__$/.exec(value);
  if (!match) return typeof value === 'string' ? value : '';
  return messages[match[1]]?.message || value;
}

export function packageInfo(data, filename = '') {
  const verified = verifyCrx(data, XL_HASH);
  const zip = readZip(verified.archive);
  const manifestBytes = zip.get('manifest.json', 1024 * 1024);
  if (!manifestBytes) throw new Error('CRX contains no manifest.json');
  const manifestText = manifestBytes.toString('utf8').replace(/^\uFEFF/, '');
  const manifest = JSON.parse(manifestText);
  if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) {
    throw new Error('Manifest must be a JSON object');
  }
  const version = checkVersion(manifest.version);
  let messages = {};
  if (typeof manifest.default_locale === 'string' &&
      /^[A-Za-z0-9_@-]+$/.test(manifest.default_locale)) {
    const content = zip.get(`_locales/${manifest.default_locale}/messages.json`);
    if (content) messages = JSON.parse(content.toString('utf8'));
  }
  const icons = Object.entries(manifest.icons || {})
    .filter(([size, name]) => /^\d+$/.test(size) && typeof name === 'string')
    .sort((a, b) => Number(b[0]) - Number(a[0]));
  let icon = null;
  let iconMime = '';
  for (const [, name] of icons) {
    const mime = {'.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
      '.webp': 'image/webp'}[path.extname(name).toLowerCase()];
    if (!mime) continue;
    const content = zip.get(name.replace(/^\//, ''));
    if (content) {
      icon = content;
      iconMime = mime;
      break;
    }
  }
  return {
    id: verified.id, version, publisher: verified.publisher,
    sha256: verified.sha256, filename, data, manifest, manifestText, icon, iconMime,
    name: localized(manifest.name, messages) || verified.id,
    description: localized(manifest.description, messages),
  };
}

export function importPackage(filename, dataDir = DEFAULT_DATA) {
  const data = readCrxFile(filename);
  const info = packageInfo(data, filename);
  const destination = path.join(dataDir, 'packages', info.id, `${info.version}.crx`);
  fs.mkdirSync(path.dirname(destination), {recursive: true});
  if (fs.existsSync(destination) && sha256(fs.readFileSync(destination)) !== info.sha256) {
    throw new Error(`Refusing to replace different bytes at ${info.id}/${info.version}`);
  }
  fs.writeFileSync(destination, data);
  return {id: info.id, version: info.version, publisher: info.publisher,
    sha256: info.sha256, path: destination};
}

export function loadCatalog(dataDir = DEFAULT_DATA) {
  const directory = path.join(dataDir, 'packages');
  const catalog = new Map();
  if (!fs.existsSync(directory)) return catalog;
  for (const id of fs.readdirSync(directory)) {
    if (!ID_PATTERN.test(id)) continue;
    const packages = new Map();
    for (const name of fs.readdirSync(path.join(directory, id))) {
      if (!name.endsWith('.crx')) continue;
      const filename = path.join(directory, id, name);
      // Reverify the bytes at startup: manually copying a developer-only or
      // tampered package into this directory must not make it installable.
      const info = packageInfo(readCrxFile(filename), filename);
      if (info.id !== id || `${info.version}.crx` !== name) {
        throw new Error(`Catalog path does not match signed package: ${filename}`);
      }
      packages.set(info.version, info);
    }
    if (packages.size) catalog.set(id, packages);
  }
  return catalog;
}

function latest(packages) {
  return [...packages.values()].sort((a, b) => compareVersion(b.version, a.version))[0];
}

function publicInfo(info) {
  return {
    id: info.id, name: info.name, description: info.description,
    version: info.version, publisher: info.publisher, sha256: info.sha256,
    iconUrl: info.icon ? `/items/${info.id}/icon` : null,
    manifestUrl: `/items/${info.id}/manifest.json`,
    detailUrl: `/detail/${info.id}`,
  };
}

function xmlEscape(value) {
  return value.replaceAll('&', '&amp;').replaceAll('"', '&quot;')
    .replaceAll('<', '&lt;').replaceAll('>', '&gt;');
}

export function createStoreServer({dataDir = DEFAULT_DATA, port = 443,
  host = '127.0.0.1', cert, key} = {}) {
  const catalog = loadCatalog(dataDir);
  const snapshotPath = path.join(dataDir, 'managed-ca-snapshot.json');
  const snapshot = fs.existsSync(snapshotPath) ? fs.readFileSync(snapshotPath) : null;
  if (snapshot) {
    const snapshotDocument = JSON.parse(snapshot);
    if (snapshotDocument.schema_version !== 1 || snapshotDocument.source_id !== CA_SOURCE ||
        !snapshotDocument.certificates?.length) throw new Error('Invalid managed CA snapshot');
  }
  const snapshotEtag = snapshot && `"${sha256(snapshot)}"`;
  const origin = `https://${DOMAIN}${port === 443 ? '' : `:${port}`}`;
  const server = https.createServer({
    cert: cert || fs.readFileSync(path.join(dataDir, 'tls/server.crt')),
    key: key || fs.readFileSync(path.join(dataDir, 'keys/tls_server_private.pem')),
  }, (request, response) => {
    const send = (status, type, content, headers = {}) => {
      const data = Buffer.isBuffer(content) ? content : Buffer.from(content);
      response.writeHead(status, {
        'Content-Type': type, 'Content-Length': data.length,
        'X-Content-Type-Options': 'nosniff', 'Cache-Control': 'no-store',
        'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
        ...headers,
      });
      response.end(request.method === 'HEAD' ? undefined : data);
    };
    try {
      if (request.method !== 'GET' && request.method !== 'HEAD') {
        return send(405, 'text/plain; charset=utf-8', 'Method not allowed');
      }
      const url = new URL(request.url, origin);
      const pathname = decodeURIComponent(url.pathname);
      if (pathname === '/managed-ca/snapshot') {
        if (!snapshot) return send(404, 'text/plain', 'Managed CA distribution is not configured');
        const unchanged = request.headers['if-none-match'] === snapshotEtag;
        return send(unchanged ? 304 : 200, 'application/json; charset=utf-8',
          unchanged ? '' : snapshot,
          {'ETag': snapshotEtag, 'Cache-Control': 'private, no-cache'});
      }
      const staticFiles = {
        '/': ['index.html', 'text/html; charset=utf-8'],
        '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
        '/style.css': ['style.css', 'text/css; charset=utf-8'],
      };
      const staticFile = staticFiles[pathname] ||
        (/^\/detail\/[a-p]{32}$/.test(pathname) && staticFiles['/']);
      if (staticFile) {
        return send(200, staticFile[1], fs.readFileSync(path.join(SOURCE, 'public', staticFile[0])));
      }
      if (pathname === '/api/items') {
        return send(200, 'application/json; charset=utf-8',
          json([...catalog.values()].map(packages => publicInfo(latest(packages)))));
      }
      const match = /^\/(api\/items|items|crx|updates)\/([a-p]{32})(.*)$/.exec(pathname);
      if (!match || !catalog.has(match[2])) return send(404, 'text/plain', 'Not found');
      const [, category, id, suffix] = match;
      const packages = catalog.get(id);
      const info = latest(packages);
      if (category === 'api/items' && !suffix) {
        return send(200, 'application/json; charset=utf-8',
          json({...publicInfo(info), manifest: info.manifestText}));
      }
      if (category === 'items' && suffix === '/manifest.json') {
        return send(200, 'application/json; charset=utf-8', info.manifestText);
      }
      if (category === 'items' && suffix === '/icon' && info.icon) {
        return send(200, info.iconMime, info.icon);
      }
      if (category === 'crx') {
        const versionMatch = /^\/([0-9.]+)\.crx$/.exec(suffix);
        const target = suffix === '.crx' ? info :
          versionMatch && packages.get(versionMatch[1]);
        if (target) return send(200, 'application/x-chrome-extension', target.data);
      }
      if (category === 'updates' && suffix === '.xml') {
        const codebase = `${origin}/crx/${id}/${info.version}.crx`;
        return send(200, 'application/xml; charset=utf-8',
          '<?xml version="1.0" encoding="UTF-8"?>\n' +
          '<gupdate xmlns="http://www.google.com/update2/response" protocol="2.0">\n' +
          `  <app appid="${id}"><updatecheck version="${info.version}" ` +
          `codebase="${xmlEscape(codebase)}" hash_sha256="${info.sha256}"/></app>\n` +
          '</gupdate>\n');
      }
      return send(404, 'text/plain', 'Not found');
    } catch (error) {
      console.error(`Request failed: ${error.message}`);
      return send(400, 'text/plain', 'Invalid request');
    }
  });
  return {server, catalog, origin, host, port};
}

export function initRuntime(dataDir = DEFAULT_DATA, openssl, port = 443) {
  const keyDir = path.join(dataDir, 'keys');
  const tlsDir = path.join(dataDir, 'tls');
  fs.mkdirSync(keyDir, {recursive: true});
  fs.mkdirSync(tlsDir, {recursive: true});
  const publisherPrivate = path.join(keyDir, 'xl_publisher_private.pem');
  if (!fs.existsSync(publisherPrivate)) {
    throw new Error(`Publisher private key missing: ${publisherPrivate}. ` +
      'Use the matching development key or explicitly provision a new pinned publisher.');
  }
  const publisherPublic = crypto.createPublicKey(fs.readFileSync(publisherPrivate))
    .export({type: 'spki', format: 'der'});
  if (sha256(publisherPublic) !== XL_HASH) throw new Error('Publisher key does not match pin');
  const cert = path.join(tlsDir, 'server.crt');
  const tlsPrivate = path.join(keyDir, 'tls_server_private.pem');
  const caFile = path.join(tlsDir, 'local-root-ca.pem');
  const caPrivate = path.join(keyDir, 'local_root_ca_private.pem');
  const executable = openssl || path.join(REPO, 'third_party/perl/c/bin/openssl.exe');
  const runOpenSsl = args => {
    const result = spawnSync(executable, args, {encoding: 'utf8', windowsHide: true});
    if (result.error || result.status !== 0) {
      throw new Error(`OpenSSL certificate creation failed: ${result.error?.message || result.stderr}`);
    }
  };
  // The browser embeds this exact public CA. A newly generated random CA would
  // not acquire that trust, even if it reused the same subject name.
  const builtinCaPem = fs.readFileSync(BUILTIN_CA_FILE);
  let caPem = fs.existsSync(caFile) ? fs.readFileSync(caFile) : builtinCaPem;
  let caCertificate = new crypto.X509Certificate(caPem);
  const migrateCa = !caCertificate.raw.equals(BUILTIN_CA.raw);
  if (migrateCa) {
    if (!hasBuiltinCaIdentity(caCertificate)) {
      throw new Error('Local CA does not match the browser built-in XL store CA; ' +
        'restore the matching certificate instead of generating a new root');
    }
    // Replace a previous certificate for this same CA identity with the exact
    // public root embedded in the current browser. Existing leaf certificates
    // still chain to this key; no signing keys are replaced.
    caPem = builtinCaPem;
    caCertificate = BUILTIN_CA;
  }
  if (!caCertificate.ca || !caCertificate.verify(caCertificate.publicKey) ||
      Date.parse(caCertificate.validFrom) > Date.now() ||
      Date.parse(caCertificate.validTo) <= Date.now()) {
    throw new Error('Built-in XL store CA is invalid or outside its validity period');
  }
  if (fs.existsSync(caPrivate)) {
    const caPublic = crypto.createPublicKey(fs.readFileSync(caPrivate))
      .export({type: 'spki', format: 'der'});
    if (!caCertificate.publicKey.export({type: 'spki', format: 'der'}).equals(caPublic)) {
      throw new Error('Local CA private key does not match the browser built-in XL store CA');
    }
  }
  let validLeaf = false;
  if (fs.existsSync(cert) && fs.existsSync(tlsPrivate)) {
    const leaf = new crypto.X509Certificate(fs.readFileSync(cert));
    const publicKey = crypto.createPublicKey(fs.readFileSync(tlsPrivate))
      .export({type: 'spki', format: 'der'});
    validLeaf = !leaf.ca && leaf.checkHost(DOMAIN) === DOMAIN &&
      leaf.verify(caCertificate.publicKey) &&
      leaf.publicKey.export({type: 'spki', format: 'der'}).equals(publicKey) &&
      Date.parse(leaf.validFrom) <= Date.now() &&
      Date.parse(leaf.validTo) > Date.now();
  }
  if (!validLeaf && !fs.existsSync(caPrivate)) {
    throw new Error(`Matching built-in CA private key required to issue the server certificate: ${caPrivate}. ` +
      'Provision it securely, or provision an already signed server certificate and its TLS key.');
  }
  if (!fs.existsSync(caFile) || migrateCa) fs.writeFileSync(caFile, caPem);
  if (!validLeaf) {
    // Migrate the earlier self-signed server leaf without changing publisher
    // identity or the existing TLS private key. No system trust store is touched.
    const config = path.join(tlsDir, 'server.cnf');
    const csr = path.join(tlsDir, 'server.csr');
    fs.writeFileSync(config, '[req]\nprompt=no\ndistinguished_name=dn\n' +
      '[dn]\nCN=' + DOMAIN + '\n[extensions]\n' +
      'subjectAltName=DNS:' + DOMAIN + '\nbasicConstraints=critical,CA:FALSE\n' +
      'keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' +
      'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n');
    if (!fs.existsSync(tlsPrivate)) {
      const pair = crypto.generateKeyPairSync('rsa', {modulusLength: 2048});
      fs.writeFileSync(tlsPrivate, pair.privateKey.export({type: 'pkcs8', format: 'pem'}),
        {mode: 0o600});
    }
    runOpenSsl(['req', '-new', '-sha256', '-key', tlsPrivate, '-config', config, '-out', csr]);
    runOpenSsl(['x509', '-req', '-sha256', '-days', '365', '-in', csr,
      '-CA', caFile, '-CAkey', caPrivate, '-CAcreateserial', '-out', cert,
      '-extfile', config, '-extensions', 'extensions']);
  }
  const snapshotFile = path.join(dataDir, 'managed-ca-snapshot.json');
  if (fs.existsSync(snapshotFile)) {
    const existing = JSON.parse(fs.readFileSync(snapshotFile));
    if (existing.source_id !== CA_SOURCE || existing.certificates?.length !== 1 ||
        existing.certificates[0].encoding !== 'pem' ||
        existing.certificates[0].role !== 'trust_anchor') {
      throw new Error('Existing managed snapshot does not match the local store source');
    }
    const snapshotCa = new crypto.X509Certificate(existing.certificates[0].data);
    if (!snapshotCa.raw.equals(caCertificate.raw) ||
        Object.hasOwn(existing.certificates[0], 'constraints')) {
      if (!hasBuiltinCaIdentity(snapshotCa) ||
          typeof existing.generation !== 'string' ||
          !/^[1-9]\d*$/.test(existing.generation) ||
          BigInt(existing.generation) >= (1n << 64n) - 1n) {
        throw new Error('Existing snapshot differs from pinned CA identity or has an invalid generation');
      }
      // Certificate or trust-parameter changes require a new generation, even
      // with the same key. This store uses ordinary CA trust without extra
      // restrictions on DNS names or IP addresses.
      existing.generation = String(BigInt(existing.generation) + 1n);
      existing.issued_at = new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
      existing.certificates[0].data = caPem.toString();
      delete existing.certificates[0].constraints;
      fs.writeFileSync(snapshotFile, json(existing));
    }
  } else {
    fs.writeFileSync(snapshotFile, json({
      schema_version: 1, source_id: CA_SOURCE, generation: '1',
      issued_at: new Date().toISOString().replace(/\.\d{3}Z$/, 'Z'),
      certificates: [{name: 'XL local store CA', role: 'trust_anchor', encoding: 'pem',
        data: caPem.toString()}],
    }));
  }
  const managedConfig = path.join(dataDir, 'managed-ca.json');
  const origin = `https://${DOMAIN}${port === 443 ? '' : `:${port}`}`;
  fs.writeFileSync(managedConfig, json({xenon_managed_ca: {
    enabled: true, source_id: CA_SOURCE, endpoint: `${origin}/managed-ca/snapshot`,
    bootstrap_ca_file: caFile.replaceAll('\\', '/'), auth: {mode: 'none'},
    refresh_interval_seconds: 300, request_timeout_seconds: 30,
  }}));
  return {publisherHash: XL_HASH, publisherPublicKey: path.join(SOURCE, 'xl_publisher_public_key.pem'),
    certificate: cert, certificateSha256: new crypto.X509Certificate(fs.readFileSync(cert)).fingerprint256,
    localCa: caFile, builtinCaSha256: sha256(caCertificate.raw), browserTrust: 'built-in',
    managedConfig, snapshot: snapshotFile};
}

export function createDemo(dataDir = DEFAULT_DATA, port = 443) {
  const demoDir = path.join(dataDir, 'demo');
  const keyDir = path.join(dataDir, 'keys');
  fs.mkdirSync(demoDir, {recursive: true});
  fs.mkdirSync(keyDir, {recursive: true});
  const devKeyFile = path.join(keyDir, 'demo_developer_private.pem');
  if (!fs.existsSync(devKeyFile)) {
    const pair = crypto.generateKeyPairSync('ec', {namedCurve: 'prime256v1'});
    fs.writeFileSync(devKeyFile, pair.privateKey.export({type: 'pkcs8', format: 'pem'}), {mode: 0o600});
  }
  const devKey = fs.readFileSync(devKeyFile);
  const publicKey = crypto.createPublicKey(devKey).export({type: 'spki', format: 'der'});
  const id = [...Buffer.from(sha256(publicKey).slice(0, 32), 'hex')].map(byte =>
    String.fromCharCode(97 + (byte >> 4), 97 + (byte & 15))).join('');
  const origin = `https://${DOMAIN}${port === 443 ? '' : `:${port}`}`;
  const manifest = {
    manifest_version: 3, name: 'XL local install test', version: '1.0.0',
    description: 'Local extension store installation test.',
    update_url: `${origin}/updates/${id}.xml`,
    action: {default_title: 'XL local test', default_popup: 'popup.html'},
  };
  const archive = makeZip({
    'manifest.json': json(manifest),
    'popup.html': '<!doctype html><meta charset="utf-8"><p>XL extension installed.</p>\n',
  });
  fs.writeFileSync(path.join(demoDir, 'manifest.json'), json(manifest));
  const developerPath = path.join(demoDir, 'developer-only.crx');
  let developerCrx;
  if (fs.existsSync(developerPath)) {
    const existing = readCrxFile(developerPath);
    if (verifyCrx(existing, XL_HASH, false).archive.equals(archive)) developerCrx = existing;
  }
  developerCrx ||= makeDeveloperCrx(archive, devKey);
  fs.writeFileSync(developerPath, developerCrx);
  const publisherPrivate = fs.readFileSync(path.join(keyDir, 'xl_publisher_private.pem'));
  const signedPath = path.join(demoDir, 'xl-signed.crx');
  let signed;
  if (fs.existsSync(signedPath)) {
    const existing = readCrxFile(signedPath);
    const verified = verifyCrx(existing, XL_HASH);
    if (verified.archive.equals(archive)) signed = existing;
  }
  signed ||= addPublisherProof(developerCrx, publisherPrivate, XL_HASH);
  fs.writeFileSync(signedPath, signed);
  return {...importPackage(signedPath, dataDir), developerOnly: developerPath,
    xlSigned: signedPath, detailUrl: `${origin}/detail/${id}`};
}

function usage() {
  console.log(`Usage: node tools/xl_extension_store/store.mjs <command> [argument] [options]
  init                         Reuse pinned store CA, issue/reuse TLS, create optional managed config
  demo                         Create and import the XL-signed MV3 sample
  inspect <package.crx>        Verify developer/proofs and report publisher
  sign <package.crx>           Add XL proof, preserve original signed bytes
  import <package.crx>         Import verified Google/XL package unchanged
  serve                        Serve HTTPS at 127.0.0.1:443
Options:
  --data-dir <directory>        Default: out/xl_extension_store
  --port <number>               Default: 443; also used for demo update_url
  --output <file>               Output path for sign
  --openssl <executable>        Certificate-generation executable
  --help`);
}

async function main() {
  const args = process.argv.slice(2);
  const options = {};
  const positionals = [];
  for (let index = 0; index < args.length; ++index) {
    if (args[index] === '--help' || args[index] === '-h') return usage();
    if (args[index].startsWith('--')) {
      const name = args[index].slice(2);
      if (!['data-dir', 'port', 'output', 'openssl'].includes(name) ||
          !args[index + 1] || args[index + 1].startsWith('--')) {
        throw new Error(`Invalid option: ${args[index]}`);
      }
      options[name] = args[++index];
    } else positionals.push(args[index]);
  }
  const [command, filename] = positionals;
  const dataDir = path.resolve(options['data-dir'] || DEFAULT_DATA);
  const port = Number(options.port || 443);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Invalid port');
  if (!command) return usage();
  if (command === 'init') return console.log(json(initRuntime(dataDir, options.openssl, port)));
  if (command === 'demo') return console.log(json(createDemo(dataDir, port)));
  if (command === 'inspect') {
    if (!filename) throw new Error('Input CRX path required');
    const verified = verifyCrx(readCrxFile(filename), XL_HASH, false);
    return console.log(json({id: verified.id, publisher: verified.publisher,
      sha256: verified.sha256, proofs: verified.proofs.map(proof => ({
        algorithm: proof.algorithm, publicKeySha256: sha256(proof.publicKey),
      }))}));
  }
  if (command === 'sign') {
    if (!filename) throw new Error('Input CRX path required');
    const signed = addPublisherProof(readCrxFile(filename),
      fs.readFileSync(path.join(dataDir, 'keys/xl_publisher_private.pem')), XL_HASH);
    const output = path.resolve(options.output || path.join(dataDir, 'signed', path.basename(filename)));
    fs.mkdirSync(path.dirname(output), {recursive: true});
    fs.writeFileSync(output, signed);
    return console.log(json({output, id: verifyCrx(signed, XL_HASH).id, sha256: sha256(signed)}));
  }
  if (command === 'import') {
    if (!filename) throw new Error('Input CRX path required');
    return console.log(json(importPackage(filename, dataDir)));
  }
  if (command === 'serve') {
    const {server, catalog, origin, host} = createStoreServer({dataDir, port});
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(port, host, resolve);
    });
    console.log(`XL extension store: ${origin}/ (${catalog.size} items; bind ${host}:${port})`);
    console.log('Catalog is verified at startup. Restart after importing packages.');
    return;
  }
  throw new Error(`Unknown command: ${command}`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
