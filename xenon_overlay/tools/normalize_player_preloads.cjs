// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {createHash} = require('node:crypto');
const {TextDecoder} = require('node:util');

function validateScript(bytes, filename) {
  const source = new TextDecoder('utf-8', {fatal: true}).decode(bytes)
      .replace(/^\uFEFF/, '').replace(/^#![^\r\n]*/, '');
  // Compile only. Never execute application code during packaging.
  new vm.Script(`(function(exports, require, module, __filename, __dirname) {\n${source}\n})`,
      {filename});
}

function withQuietVendorTool(run) {
  // The original build tool logs its archive keys and decrypted headers.
  // Neither belongs in packaging logs or the JSON result.
  const saved = new Map(['log', 'info', 'warn', 'error', 'debug'].map(
      name => [name, console[name]]));
  for (const name of saved.keys()) console[name] = () => {};
  try {
    return run();
  } finally {
    for (const [name, fn] of saved) console[name] = fn;
  }
}

function extractMatchingScript(source, relative, archive, asarModule) {
  if (!fs.existsSync(archive) || !fs.existsSync(asarModule)) {
    throw new Error(`${relative}: encoded or invalid preload requires the matching ` +
        'vendor archive and ASAR tool; pass --source-archive and --vendor-asar-module');
  }
  return withQuietVendorTool(() => {
    const asar = require(path.resolve(asarModule));
    const disk = require(path.join(path.dirname(path.resolve(asarModule)), 'disk.js'));
    const info = asar.statFile(archive, relative);
    const filesystem = disk.readFilesystemSync(archive);
    if (!info || info.unpacked || !info.encrypted || info.size !== source.length) {
      throw new Error(`${relative}: vendor archive does not match the encoded source file`);
    }
    const offset = 8 + filesystem.headerSize + Number(info.offset);
    if (!Number.isSafeInteger(offset) || offset < 8) {
      throw new Error(`${relative}: invalid vendor archive offset`);
    }
    // Match the original stored bytes before accepting a decoded entry. This
    // prevents a stale archive from silently supplying another build's preload.
    const stored = Buffer.alloc(source.length);
    const fd = fs.openSync(archive, 'r');
    try {
      if (fs.readSync(fd, stored, 0, stored.length, offset) !== stored.length ||
          !stored.equals(source)) {
        throw new Error(`${relative}: vendor archive does not match the encoded source file`);
      }
    } finally {
      fs.closeSync(fd);
    }
    const decoded = Buffer.from(asar.extractFile(archive, relative));
    validateScript(decoded, relative);
    return decoded;
  });
}

function normalizePreloads(sourceRoot, archive, asarModule) {
  const scripts = {};
  let normalized = 0;
  const visit = relative => {
    const filename = path.join(sourceRoot, relative);
    for (const entry of fs.readdirSync(filename, {withFileTypes: true})) {
      const child = path.posix.join(relative, entry.name);
      if (entry.isDirectory()) {
        visit(child);
      } else if (entry.isFile() && /\.[cm]?js$/i.test(entry.name)) {
        let bytes = fs.readFileSync(path.join(sourceRoot, child));
        try {
          validateScript(bytes, child);
        } catch {
          bytes = extractMatchingScript(bytes, child, archive, asarModule);
          ++normalized;
        }
        scripts[child] = bytes.toString('base64');
      }
    }
  };
  for (const relative of ['preload', 'preload-native']) {
    if (fs.existsSync(path.join(sourceRoot, relative))) visit(relative);
  }
  return {scripts, normalized};
}

module.exports = {normalizePreloads};

if (require.main === module) {
  try {
    const [sourceRoot, archive, asarModule, option, output] = process.argv.slice(2);
    if (option && (option !== '--write-to' || !output)) {
      throw new Error('Usage: normalize_player_preloads.cjs SOURCE ARCHIVE ASAR_MODULE [--write-to TARGET]');
    }
    const result = normalizePreloads(sourceRoot, archive, asarModule);
    if (output) {
      // All inputs have passed validation before any target file is changed.
      const files = Object.entries(result.scripts).map(([relative, encoded]) => {
        const original = fs.readFileSync(path.join(sourceRoot, relative));
        const bytes = Buffer.from(encoded, 'base64');
        const filename = path.join(path.resolve(output), relative);
        fs.mkdirSync(path.dirname(filename), {recursive: true});
        fs.writeFileSync(filename, bytes);
        const hash = buffer => createHash('sha256').update(buffer).digest('hex');
        return {path: filename, sourceSha256: hash(original), outputSha256: hash(bytes)};
      });
      process.stdout.write(JSON.stringify({normalized: result.normalized, files}));
    } else {
      process.stdout.write(JSON.stringify(result));
    }
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}
