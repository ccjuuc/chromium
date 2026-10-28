// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const {createHash} = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {test} = require('node:test');

const source = process.env.THUNDER_MAC_TEST_SOURCE;
const dependencies = {skip: source ? false : 'Set THUNDER_MAC_TEST_SOURCE to an installed TH checkout'};
const digest = bytes => createHash('sha256').update(bytes).digest('hex');

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((accept, decline) => {
    resolve = accept;
    reject = decline;
  });
  return {promise, resolve, reject};
}

async function withinDeadline(promise) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('SinglePromise did not settle')), 1000);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

test('the actual compatibility patch compiles and settles shared operations', dependencies, async t => {
  const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'single-promise-patch-')));
  try {
    const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, 'js-compatibility.json')));
    const patch = manifest.patches.find(entry => entry.path === 'patches/app-js.patch');
    const relative = 'app/src/common/single-promise.ts';
    const entry = patch.files.find(file => file.path === relative);
    assert.ok(entry, 'the fix must be part of the shipped patch manifest');
    const patchPath = path.join(__dirname, patch.path);
    assert.equal(digest(fs.readFileSync(patchPath)), patch.sha256);
    const originalPath = path.join(source, relative);
    const original = fs.readFileSync(originalPath);
    const before = digest(original) === entry.before_sha256 ? original : execFileSync(
      'git', ['show', `${patch.base_revision}:${relative}`], {cwd: source});
    assert.equal(digest(before), entry.before_sha256);
    const fixture = path.join(temporary, relative);
    fs.mkdirSync(path.dirname(fixture), {recursive: true});
    fs.writeFileSync(fixture, before);
    execFileSync('git', ['apply', '--whitespace=error-all', `--include=${relative}`, patchPath], {
      cwd: temporary,
      env: {...process.env, GIT_CEILING_DIRECTORIES: path.dirname(temporary)},
    });
    assert.equal(digest(fs.readFileSync(fixture)), entry.after_sha256);
    fs.writeFileSync(path.join(temporary, 'tsconfig.json'), JSON.stringify({
      compilerOptions: {
        target: 'ES2020', module: 'commonjs', strict: true, types: [],
        noEmitOnError: true, outDir: 'compiled', rootDir: '.',
      },
      files: [relative],
    }));
    execFileSync(process.execPath, [path.join(source, 'app/node_modules/typescript/bin/tsc'),
      '--project', path.join(temporary, 'tsconfig.json')], {stdio: 'pipe'});
    const {SinglePromise} = require(path.join(temporary, 'compiled/app/src/common/single-promise.js'));

    await t.test('same-key requests share one invocation and result, then allow retry', async () => {
      const shared = new SinglePromise();
      const first = deferred();
      let calls = 0;
      const one = shared.run('request', () => { calls++; return first.promise; });
      assert.equal(calls, 1, 'the original synchronous start behavior is preserved');
      const two = shared.run('request', () => { calls++; return Promise.resolve('wrong'); });
      assert.equal(one, two);
      first.resolve('completed');
      assert.deepEqual(await withinDeadline(Promise.all([one, two])), ['completed', 'completed']);
      assert.equal(calls, 1);
      assert.equal(await shared.run('request', () => { calls++; return Promise.resolve('retry'); }),
        'retry');
      assert.equal(calls, 2);
    });

    await t.test('a rejected refresh reaches every waiter and clears the key for retry', async () => {
      const shared = new SinglePromise();
      const refresh = deferred();
      const failure = new Error('network unreachable');
      const one = shared.run('rapid-play', () => refresh.promise);
      const two = shared.run('rapid-play', () => assert.fail('must share the refresh'));
      const settled = Promise.allSettled([one, two]);
      refresh.reject(failure);
      const results = await withinDeadline(settled);
      assert.deepEqual(results.map(result => result.status), ['rejected', 'rejected']);
      results.forEach(result => assert.equal(result.reason, failure));
      assert.equal(await withinDeadline(shared.run('rapid-play', () => Promise.resolve('recovered'))),
        'recovered');
    });

    await t.test('synchronous throws become shared rejections and permit another operation', async () => {
      const shared = new SinglePromise();
      const failure = new Error('synchronous failure');
      let one;
      assert.doesNotThrow(() => { one = shared.run('request', () => { throw failure; }); });
      const two = shared.run('request', () => assert.fail('must reuse the failing request'));
      assert.equal(one, two);
      const results = await withinDeadline(Promise.allSettled([one, two]));
      results.forEach(result => {
        assert.equal(result.status, 'rejected');
        assert.equal(result.reason, failure);
      });
      assert.equal(await shared.run('request', () => Promise.resolve('retry')), 'retry');
    });

    await t.test('different keys run independently while the first is still pending', async () => {
      const shared = new SinglePromise();
      const first = deferred();
      const one = shared.run('first', () => first.promise);
      assert.equal(await withinDeadline(shared.run('second', () => Promise.resolve('second'))),
        'second');
      first.resolve('first');
      assert.equal(await one, 'first');
    });

    await t.test('prototype property names are ordinary independent keys', async () => {
      const shared = new SinglePromise();
      const keys = ['__proto__', 'constructor', 'toString'];
      assert.deepEqual(await withinDeadline(Promise.all(keys.map(key =>
        shared.run(key, () => Promise.resolve(key))))), keys);
      assert.deepEqual(await Promise.all(keys.map(key =>
        shared.run(key, () => Promise.resolve('retried')))), keys.map(() => 'retried'));
    });

    await t.test('a synchronous reentrant request finds the pending operation', async () => {
      const shared = new SinglePromise();
      const completion = deferred();
      let reentrant;
      const pending = shared.run('request', () => {
        reentrant = shared.run('request', () => assert.fail('must not start a second request'));
        return completion.promise;
      });
      assert.equal(reentrant, pending);
      completion.resolve('complete');
      assert.equal(await pending, 'complete');
    });
    assert.deepEqual(fs.readFileSync(originalPath), original, 'the source checkout must remain unchanged');
  } finally {
    fs.rmSync(temporary, {recursive: true, force: true});
  }
});
