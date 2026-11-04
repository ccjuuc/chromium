#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {makeZip, readCrxFile, sha256, verifyCrx} from './crx.mjs';
import {
  BUILTIN_CA, BUILTIN_CA_FILE, DEFAULT_DATA, DOMAIN, loadCatalog, XL_HASH,
} from './store.mjs';

const usage = `Usage: node tools/xl_extension_store/export-runtime.mjs [options]
  --data-dir <directory>   Default: out/xl_extension_store
  --output <file.zip>      Default: <data-dir>/exports/xl-store-runtime.zip
  --include-signing-keys   Include publisher/CA keys and an existing demo key;
                          default output name becomes xl-store-full.zip
  --help                  Show this help

Exports a standard ZIP for Expand-Archive. Existing output is never overwritten.
The default package contains the HTTPS server private key. Full packages also
contain signing keys. Keep these local migration files private and out of Git.
Browser profiles, logs, old configurations and certificate backups are excluded.`;

function privateKey(dataDir, relative) {
  const bytes = fs.readFileSync(path.join(dataDir, relative));
  let key;
  try {
    key = crypto.createPrivateKey(bytes);
  } catch {
    throw new Error(`Cannot read an unencrypted private key: ${relative}`);
  }
  return {bytes, publicKey: crypto.createPublicKey(key).export({type: 'spki', format: 'der'})};
}

function migrationInstructions(includeSigningKeys, hasDemoKey) {
  const mode = includeSigningKeys ?
      'Full environment: HTTPS, XL package signing and HTTPS certificate renewal.' :
      'Runtime environment: serve existing verified XL/Google packages over HTTPS.';
  const extra = includeSigningKeys ? `
Signing and renewal
- keys/xl_publisher_private.pem signs CRX publisher proofs.
- keys/local_root_ca_private.pem renews the HTTPS server certificate.
- ${hasDemoKey ? 'The existing demo developer key is included to preserve its extension ID.' :
    'No demo developer key existed; running demo creates a new extension ID.'}
- Existing demo/developer-only.crx and demo/xl-signed.crx caches are included
  so rerunning demo reuses the original signed bytes of an imported version.
- After extraction, optionally run:
    node tools/xl_extension_store/store.mjs init
  This reuses valid TLS material and rebuilds optional managed-CA configuration
  with this machine's absolute paths. Do not copy old managed-ca.json files.
- OpenSSL is required only when issuing/renewing certificates. The default is
  third_party/perl/c/bin/openssl.exe; init also accepts --openssl <executable>.
- To create/import a demo, run:
    node tools/xl_extension_store/store.mjs demo
  Restart serve after importing or signing and importing a new package.
` : `
This runtime package does not contain CA or publisher signing keys. Start serve
directly; init requires the publisher key. To renew the certificate or sign new
XL packages, obtain a full environment from the existing key holder. Importing
an already signed Google/XL CRX does not require those signing keys.
`;
  return `XL extension store local migration

${mode}
This ZIP contains private keys. Transfer it separately from source control.

1. Use Node.js 24 and the matching Chromium checkout/custom browser version.
   Browser trust and publisher pins must match the public material in that
   checkout. No npm packages are needed.
2. Extract into an empty out/xl_extension_store directory in the new checkout:
     Expand-Archive -LiteralPath '<migration.zip>' -DestinationPath '<new-checkout>/out/xl_extension_store'
   The ZIP contains runtime files only; obtain tools/xl_extension_store from the
   matching checkout. Do not move the old browser profile or old local configs.
3. Add this mapping to the new machine's hosts file using administrator access:
     127.0.0.1 ${DOMAIN}
4. In PowerShell at the new checkout root, start the service:
     node tools/xl_extension_store/store.mjs serve
   Keep that terminal open. Ctrl+C stops the service. It listens on 127.0.0.1:443.
5. Open https://${DOMAIN}/ in the matching custom browser, or use its EXT test
   page for the browser command. The browser already trusts the built-in CA;
   do not install this CA into the system certificate store.

Certificates and private keys are portable files, not bound to the old machine
or drive path. The TLS certificate must still be within its validity period.
The public root is included as tls/local-root-ca.pem. Ordinary store operation
does not need managed-ca.json or managed-ca-snapshot.json; neither is exported.
${extra}`;
}

export function exportRuntime({dataDir = DEFAULT_DATA, output,
  includeSigningKeys = false} = {}) {
  dataDir = path.resolve(dataDir);
  const outputPath = path.resolve(output || path.join(dataDir, 'exports',
    includeSigningKeys ? 'xl-store-full.zip' : 'xl-store-runtime.zip'));
  if (fs.existsSync(outputPath)) {
    throw new Error(`Refusing to overwrite existing output: ${outputPath}`);
  }

  const certificateBytes = fs.readFileSync(path.join(dataDir, 'tls/server.crt'));
  const certificate = new crypto.X509Certificate(certificateBytes);
  const tlsKey = privateKey(dataDir, 'keys/tls_server_private.pem');
  const now = Date.now();
  if (certificate.ca || certificate.checkHost(DOMAIN) !== DOMAIN ||
      !certificate.checkIssued(BUILTIN_CA) || !certificate.verify(BUILTIN_CA.publicKey) ||
      !(Date.parse(certificate.validFrom) <= now && now < Date.parse(certificate.validTo))) {
    throw new Error('TLS certificate must be currently valid for the store domain and signed by the built-in CA');
  }
  if (!certificate.publicKey.export({type: 'spki', format: 'der'}).equals(tlsKey.publicKey)) {
    throw new Error('TLS private key does not match tls/server.crt');
  }

  const entries = {
    'tls/server.crt': certificateBytes,
    'tls/local-root-ca.pem': fs.readFileSync(BUILTIN_CA_FILE),
    'keys/tls_server_private.pem': tlsKey.bytes,
  };
  const privateKeyTypes = ['HTTPS server'];
  let hasDemoKey = false;
  if (includeSigningKeys) {
    const publisherKey = privateKey(dataDir, 'keys/xl_publisher_private.pem');
    if (sha256(publisherKey.publicKey) !== XL_HASH) {
      throw new Error('Publisher private key does not match the pinned XL public key');
    }
    const caKey = privateKey(dataDir, 'keys/local_root_ca_private.pem');
    if (!caKey.publicKey.equals(BUILTIN_CA.publicKey.export({type: 'spki', format: 'der'}))) {
      throw new Error('CA private key does not match the built-in public root');
    }
    entries['keys/xl_publisher_private.pem'] = publisherKey.bytes;
    entries['keys/local_root_ca_private.pem'] = caKey.bytes;
    privateKeyTypes.push('XL publisher', 'CA signing');
    const demoPath = 'keys/demo_developer_private.pem';
    if (fs.existsSync(path.join(dataDir, demoPath))) {
      entries[demoPath] = privateKey(dataDir, demoPath).bytes;
      privateKeyTypes.push('Demo developer');
      hasDemoKey = true;
    }
    for (const [relative, requirePublisher] of [
      ['demo/developer-only.crx', false],
      ['demo/xl-signed.crx', true],
    ]) {
      const filename = path.join(dataDir, relative);
      if (fs.existsSync(filename)) {
        const bytes = readCrxFile(filename);
        verifyCrx(bytes, XL_HASH, requirePublisher);
        entries[relative] = bytes;
      }
    }
  }

  const packages = [];
  const catalog = loadCatalog(dataDir);
  for (const id of [...catalog.keys()].sort()) {
    const versions = catalog.get(id);
    for (const version of [...versions.keys()].sort()) {
      const info = versions.get(version);
      const relative = `packages/${id}/${version}.crx`;
      entries[relative] = info.data;
      packages.push({path: relative, sha256: info.sha256});
    }
  }
  entries['MIGRATION.txt'] = migrationInstructions(includeSigningKeys, hasDemoKey);
  const files = Object.keys(entries);
  // makeZip writes ordinary ZIP records, not ZIP64 records.
  const zipSize = Object.entries(entries).reduce((size, [name, data]) =>
    size + 76 + 2 * Buffer.byteLength(name) + Buffer.byteLength(data), 22);
  if (files.length > 65535 || zipSize > 0xffffffff) {
    throw new Error('Runtime exceeds the supported standard ZIP size or entry count');
  }
  const zip = makeZip(entries);
  fs.mkdirSync(path.dirname(outputPath), {recursive: true});
  fs.writeFileSync(outputPath, zip, {flag: 'wx', mode: 0o600});

  return {
    output: outputPath,
    mode: includeSigningKeys ? 'full' : 'runtime',
    files,
    privateKeyTypes,
    publicSha256: {
      serverCertificate: sha256(certificate.raw),
      rootCertificate: sha256(BUILTIN_CA.raw),
      publisherPublicKey: XL_HASH,
      packages,
    },
  };
}

function main() {
  const options = {};
  const args = process.argv.slice(2);
  for (let index = 0; index < args.length; ++index) {
    const arg = args[index];
    if (arg === '--help' || arg === '-h') {
      console.log(usage);
      return;
    }
    if (arg === '--include-signing-keys') {
      options.includeSigningKeys = true;
    } else if (['--data-dir', '--output'].includes(arg) &&
        args[index + 1] && !args[index + 1].startsWith('--')) {
      options[arg === '--data-dir' ? 'dataDir' : 'output'] = args[++index];
    } else {
      throw new Error(`Invalid argument: ${arg}. Use --help for usage.`);
    }
  }
  console.log(JSON.stringify(exportRuntime(options), null, 2));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
