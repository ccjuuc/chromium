// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.

import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {makeZip} from './crx.mjs';
import {decryptArchive, encryptArchive} from './protect-runtime.mjs';

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'xl-protect-runtime-test-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const archive = makeZip({
    'MIGRATION.txt': 'Synthetic migration fixture; no real keys.\n',
    'sample.bin': Buffer.from([0, 255, 1, 128, 2, 127]),
  });
  const input = path.join(directory, 'source.zip');
  fs.writeFileSync(input, archive);
  return {
    directory, archive, input,
    output: path.join(directory, 'bundle.zip.enc'),
    passwordFile: path.join(directory, 'password.txt'),
    restored: path.join(directory, 'restored.zip'),
  };
}

function assertFileBytes(filename, expected) {
  // A failed assertion must not print password or plaintext Buffer contents.
  assert.equal(fs.readFileSync(filename).equals(expected), true,
    'File bytes must remain unchanged');
}

function assertNoPlaintext(output) {
  assert.equal(fs.existsSync(output), false, 'Rejected input must not create plaintext');
  assert.equal(fs.existsSync(path.dirname(output)), false,
    'Rejected input must not create its output directory');
}

test('missing password is generated and the ZIP round-trips byte-for-byte', t => {
  const f = fixture(t);
  const result = encryptArchive(f);
  const password = fs.readFileSync(f.passwordFile);
  assert.equal(/^[A-Za-z0-9_-]{43}\n$/.test(password.toString('utf8')), true,
    'Generated password must contain 32 random bytes encoded as base64url');
  assert.equal(result.output, f.output);
  assert.equal(result.passwordFile, f.passwordFile);
  decryptArchive({input: f.output, output: f.restored, passwordFile: f.passwordFile});
  assertFileBytes(f.restored, f.archive);
  assertFileBytes(f.passwordFile, password);
});

test('same password and ZIP produce different authenticated bundles', t => {
  const f = fixture(t);
  encryptArchive(f);
  const first = fs.readFileSync(f.output);
  const password = fs.readFileSync(f.passwordFile);
  const secondPath = path.join(f.directory, 'second.zip.enc');
  encryptArchive({...f, output: secondPath});
  const second = fs.readFileSync(secondPath);
  assert.equal(first.equals(second), false, 'Encryption must use fresh randomness');
  assertFileBytes(f.passwordFile, password);
  decryptArchive({input: secondPath, output: f.restored, passwordFile: f.passwordFile});
  assertFileBytes(f.restored, f.archive);
});

test('wrong password creates neither plaintext nor its output directory', t => {
  const f = fixture(t);
  encryptArchive(f);
  const wrongPassword = path.join(f.directory, 'wrong-password.txt');
  fs.writeFileSync(wrongPassword, crypto.randomBytes(32).toString('base64url') + '\n');
  const output = path.join(f.directory, 'rejected', 'restored.zip');
  assert.throws(() => decryptArchive({input: f.output, output, passwordFile: wrongPassword}),
    /Cannot authenticate/);
  assertNoPlaintext(output);
});

test('modified magic, salt, nonce, ciphertext and tag are rejected before writing', async t => {
  const f = fixture(t);
  encryptArchive(f);
  const original = fs.readFileSync(f.output);
  // Version 1 wire layout: 8-byte magic, 16-byte salt, 12-byte nonce, data, tag.
  for (const [name, offset] of [
    ['magic', 0], ['salt', 8], ['nonce', 24], ['ciphertext', 36],
    ['tag', original.length - 1],
  ]) {
    await t.test(name, () => {
      const changed = Buffer.from(original);
      changed[offset] ^= 1;
      const input = path.join(f.directory, `${name}.zip.enc`);
      fs.writeFileSync(input, changed);
      const output = path.join(f.directory, `rejected-${name}`, 'restored.zip');
      assert.throws(() => decryptArchive({input, output, passwordFile: f.passwordFile}),
        /Invalid or unsupported|Cannot authenticate/);
      assertNoPlaintext(output);
    });
  }
});

test('encrypt refuses an existing output before generating a password', t => {
  const f = fixture(t);
  const original = Buffer.from('existing output');
  fs.writeFileSync(f.output, original);
  assert.throws(() => encryptArchive(f), /overwrite/);
  assertFileBytes(f.output, original);
  assert.equal(fs.existsSync(f.passwordFile), false);
  assertFileBytes(f.input, f.archive);
});

test('decrypt refuses an existing output without changing it or the password', t => {
  const f = fixture(t);
  encryptArchive(f);
  const password = fs.readFileSync(f.passwordFile);
  const original = Buffer.from('existing restored output');
  fs.writeFileSync(f.restored, original);
  assert.throws(() => decryptArchive({input: f.output, output: f.restored,
    passwordFile: f.passwordFile}), /overwrite/);
  assertFileBytes(f.restored, original);
  assertFileBytes(f.passwordFile, password);
});

for (const command of ['encrypt', 'decrypt']) {
  test(`${command} rejects all pairs of overlapping paths without writing`, async t => {
    for (const pair of ['input-output', 'input-password', 'output-password']) {
      await t.test(pair, child => {
        const f = fixture(child);
        if (command === 'decrypt') encryptArchive(f);
        const input = command === 'encrypt' ? f.input : f.output;
        const inputBytes = fs.readFileSync(input);
        const destination = path.join(f.directory, 'must-not-exist.zip.enc');
        const passwordFile = command === 'encrypt' ? f.passwordFile :
            path.join(f.directory, 'unused-password.txt');
        const options = {input, output: destination, passwordFile};
        if (pair === 'input-output') options.output = input;
        if (pair === 'input-password') options.passwordFile = input;
        if (pair === 'output-password') options.passwordFile = destination;
        const action = command === 'encrypt' ? encryptArchive : decryptArchive;
        assert.throws(() => action(options), /same|distinct|overlap|conflict|different/i);
        assertFileBytes(input, inputBytes);
        assert.equal(fs.existsSync(destination), false,
          'A path conflict must not leave a password disguised as an encrypted bundle');
        assert.equal(fs.existsSync(passwordFile), false);
      });
    }
  });
}

test('Windows case aliases cannot leave a password at the encrypted output path',
  {skip: process.platform !== 'win32'}, t => {
    const f = fixture(t);
    assert.throws(() => encryptArchive({...f, passwordFile: f.output.toUpperCase()}),
      /same|distinct|overlap|conflict|different/i);
    assert.equal(fs.existsSync(f.output), false);
    assert.equal(fs.existsSync(f.passwordFile), false);
  });

test('directory aliases resolve conflicts even below a nonexistent child directory', t => {
  const f = fixture(t);
  const realDirectory = path.join(f.directory, 'real');
  const aliasDirectory = path.join(f.directory, 'alias');
  fs.mkdirSync(realDirectory);
  try {
    fs.symlinkSync(realDirectory, aliasDirectory, process.platform === 'win32' ? 'junction' : 'dir');
  } catch (error) {
    if (!['EPERM', 'EACCES', 'ENOSYS'].includes(error.code)) throw error;
    t.skip('This host cannot create a directory alias');
    return;
  }
  const output = path.join(realDirectory, 'new', 'bundle.zip.enc');
  const passwordFile = path.join(aliasDirectory, 'new', 'bundle.zip.enc');
  assert.throws(() => encryptArchive({...f, output, passwordFile}),
    /same|distinct|overlap|conflict|different/i);
  assert.equal(fs.existsSync(output), false);
  assert.equal(fs.existsSync(path.dirname(output)), false);
});
