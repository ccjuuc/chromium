#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {DEFAULT_DATA, SOURCE} from './store.mjs';

// Version 1: magic | 16-byte scrypt salt | 12-byte GCM nonce | ciphertext | tag.
// The fixed KDF parameters are part of this version, not supplied by the file.
const MAGIC = Buffer.from('XLSTORE1');
const HEADER_SIZE = MAGIC.length + 16 + 12;
const TAG_SIZE = 16;
const DEFAULT_PASSWORD = path.join(DEFAULT_DATA, 'keys/runtime-bundle-password.txt');
const DEFAULT_BUNDLE = path.join(SOURCE, 'xl-store-full.zip.enc');
const DEFAULT_RESTORED = path.join(DEFAULT_DATA, 'exports/restored-xl-store-full.zip');

const usage = `Usage: node tools/xl_extension_store/protect-runtime.mjs <command> [options]
  encrypt                 Encrypt an exported ZIP; creates a random password if absent
  decrypt                 Authenticate and decrypt the checked-in environment bundle
Options:
  --input <file>          Encrypt: required ZIP; decrypt: tools/xl_extension_store/xl-store-full.zip.enc
  --output <file>         Encrypt: tools/xl_extension_store/xl-store-full.zip.enc
                         Decrypt: out/xl_extension_store/exports/restored-xl-store-full.zip
  --password-file <file>  Default: out/xl_extension_store/keys/runtime-bundle-password.txt
  --help                 Show this help

Uses scrypt and AES-256-GCM from Node.js. No additional packages are required.
Existing outputs are never overwritten. For private archives, keep the password
outside Git; its contents are never printed. The shared test bundle deliberately
ships with test-bundle-password.txt; setup-runtime.mjs restores it automatically.`;

function ensureNewOutput(filename) {
  if (fs.existsSync(filename)) {
    throw new Error(`Refusing to overwrite existing output: ${filename}`);
  }
}

function canonicalPath(filename) {
  let ancestor = path.resolve(filename);
  const suffix = [];
  while (!fs.existsSync(ancestor)) {
    const parent = path.dirname(ancestor);
    if (parent === ancestor) break;
    suffix.unshift(path.basename(ancestor));
    ancestor = parent;
  }
  const resolved = path.join(fs.realpathSync.native(ancestor), ...suffix);
  return process.platform === 'win32' ? resolved.toLowerCase() : resolved;
}

function separatePaths(input, output, passwordFile) {
  const paths = [input, output, passwordFile].map(filename => path.resolve(filename));
  if (new Set(paths.map(canonicalPath)).size !== paths.length) {
    throw new Error('Input, output and password file must be different paths');
  }
  return paths;
}

function readPassword(filename, create) {
  if (create && !fs.existsSync(filename)) {
    fs.mkdirSync(path.dirname(filename), {recursive: true});
    fs.writeFileSync(filename, crypto.randomBytes(32).toString('base64url') + '\n',
      {flag: 'wx', mode: 0o600});
  }
  const password = fs.readFileSync(filename, 'utf8').replace(/\r?\n$/, '');
  if (password.length < 16) {
    throw new Error('Password file must contain at least 16 characters');
  }
  return password;
}

function deriveKey(password, salt) {
  return crypto.scryptSync(password, salt, 32,
    {N: 32768, r: 8, p: 1, maxmem: 64 * 1024 * 1024});
}

function writeNew(filename, data) {
  fs.mkdirSync(path.dirname(filename), {recursive: true});
  fs.writeFileSync(filename, data, {flag: 'wx', mode: 0o600});
}

export function encryptArchive({input, output = DEFAULT_BUNDLE,
  passwordFile = DEFAULT_PASSWORD} = {}) {
  if (!input) throw new Error('encrypt requires --input <exported ZIP>');
  const [inputPath, outputPath, passwordPath] = separatePaths(input, output, passwordFile);
  ensureNewOutput(outputPath);
  const archive = fs.readFileSync(inputPath);
  if (archive.length < 4 || archive.readUInt32LE(0) !== 0x04034b50) {
    throw new Error('Input must be an exported ZIP archive');
  }
  const password = readPassword(passwordPath, true);
  const salt = crypto.randomBytes(16);
  const nonce = crypto.randomBytes(12);
  const header = Buffer.concat([MAGIC, salt, nonce]);
  const key = deriveKey(password, salt);
  try {
    const cipher = crypto.createCipheriv('aes-256-gcm', key, nonce,
      {authTagLength: TAG_SIZE});
    cipher.setAAD(header);
    const encrypted = Buffer.concat([
      header, cipher.update(archive), cipher.final(), cipher.getAuthTag(),
    ]);
    writeNew(outputPath, encrypted);
  } finally {
    key.fill(0);
  }
  return {output: outputPath, passwordFile: passwordPath, algorithm: 'scrypt + AES-256-GCM'};
}

export function decryptArchive({input = DEFAULT_BUNDLE, output = DEFAULT_RESTORED,
  passwordFile = DEFAULT_PASSWORD} = {}) {
  const [inputPath, outputPath, passwordPath] = separatePaths(input, output, passwordFile);
  ensureNewOutput(outputPath);
  const encrypted = fs.readFileSync(inputPath);
  if (encrypted.length < HEADER_SIZE + TAG_SIZE + 4 ||
      !encrypted.subarray(0, MAGIC.length).equals(MAGIC)) {
    throw new Error('Invalid or unsupported encrypted runtime bundle');
  }
  const header = encrypted.subarray(0, HEADER_SIZE);
  const salt = header.subarray(MAGIC.length, MAGIC.length + 16);
  const nonce = header.subarray(MAGIC.length + 16);
  const key = deriveKey(readPassword(passwordPath, false), salt);
  let archive;
  try {
    const decipher = crypto.createDecipheriv('aes-256-gcm', key, nonce,
      {authTagLength: TAG_SIZE});
    decipher.setAAD(header);
    decipher.setAuthTag(encrypted.subarray(-TAG_SIZE));
    try {
      archive = Buffer.concat([
        decipher.update(encrypted.subarray(HEADER_SIZE, -TAG_SIZE)), decipher.final(),
      ]);
    } catch {
      throw new Error('Cannot authenticate bundle: wrong password or damaged file');
    }
  } finally {
    key.fill(0);
  }
  // Never write even a partial plaintext file before authentication succeeds.
  writeNew(outputPath, archive);
  return {output: outputPath};
}

function main() {
  const args = process.argv.slice(2);
  if (!args.length || args.includes('--help') || args.includes('-h')) {
    console.log(usage);
    return;
  }
  const command = args.shift();
  if (!['encrypt', 'decrypt'].includes(command)) throw new Error('Expected encrypt or decrypt');
  const options = {};
  for (let index = 0; index < args.length; index += 2) {
    const name = {'--input': 'input', '--output': 'output', '--password-file': 'passwordFile'}[args[index]];
    if (!name || !args[index + 1] || args[index + 1].startsWith('--')) {
      throw new Error('Invalid arguments. Use --help for usage.');
    }
    options[name] = args[index + 1];
  }
  console.log(JSON.stringify(command === 'encrypt' ? encryptArchive(options) :
    decryptArchive(options), null, 2));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
