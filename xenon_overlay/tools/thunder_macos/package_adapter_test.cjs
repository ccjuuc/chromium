// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const {test} = require('node:test');
const {
  adaptPackage, archiveFiles, loadAsar, parseArguments,
  validateDestination, PLAYER_MANIFEST,
} = require('./package_adapter.cjs');

const source = process.env.THUNDER_MAC_TEST_SOURCE;
const dependencies = {skip: source ? false : 'Set THUNDER_MAC_TEST_SOURCE to an installed TH checkout'};

function write(root, relative, contents) {
  const filename = path.join(root, relative);
  fs.mkdirSync(path.dirname(filename), {recursive: true});
  fs.writeFileSync(filename, contents);
  return filename;
}

async function fixture(t) {
  const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-static-test-')));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const bundle = path.join(root, 'Thunder.app');
  const contents = path.join(bundle, 'Contents');
  const plugins = path.join(contents, 'Resources/app/plugins');
  const target = path.join(plugins, 'thunder-pan-plugin');
  const manifest = path.join(contents, 'Resources/xenon-native-player.json');
  write(bundle, 'Contents/Info.plist', 'fixture');
  write(bundle, 'Contents/Resources/app/out.asar', 'original main archive');
  write(target, 'index.js', 'unrelated existing script, preserve me');
  write(root, 'source-maps.asar', 'original source maps');
  const input = path.join(root, 'pan-input');
  const javascript = 'const publicPath = "../../plugins/thunder-pan-plugin/";';
  write(input, 'index.js', javascript);
  write(input, 'index.html', '<script src="index.js"></script>');
  const images = new Map([
    ['static/image/empty.png', Buffer.from([0, 1, 255, 3])],
    ['static/image/network.png', Buffer.from([5, 3, 7])],
    ['static/font/ui.woff2', Buffer.from([9, 5, 6])],
  ]);
  for (const [relative, bytes] of images) write(input, relative, bytes);
  const archive = path.join(plugins, 'thunder-pan-plugin.asar');
  await loadAsar(source).createPackage(input, archive);
  const originals = new Map([
    archive, path.join(target, 'index.js'), path.join(root, 'source-maps.asar'),
    path.join(contents, 'Resources/app/out.asar'),
  ].map(filename => [filename, fs.readFileSync(filename)]));
  return {root, bundle, plugins, target, manifest, archive, images, originals};
}

function assertUnchanged(originals) {
  for (const [filename, bytes] of originals) assert.deepEqual(fs.readFileSync(filename), bytes, filename);
}

test('CLI accepts only source, bundle and optional report', () => {
  assert.deepEqual(parseArguments(['--source', 'repo', '--bundle', 'app']), {source: 'repo', bundle: 'app'});
  assert.throws(() => parseArguments(['--source', 'repo', '--source', 'other']), /repeated/);
  assert.throws(() => parseArguments(['--work-dir', 'work']), /Invalid/);
  assert.throws(() => parseArguments(['--source', '--bundle']), /Invalid/);
  assert.throws(() => parseArguments(['--source', 'repo']), /Missing/);
});

test('archive validation rejects traversal, conflicting names, links and unpacked entries', () => {
  for (const name of ['/../outside', '//outside', '/folder/../outside', '/folder\\outside', '/a//b']) {
    assert.throws(() => archiveFiles({uncache() {}, listPackage: () => [name]}, 'fixture'), /Unsafe/);
  }
  assert.throws(() => archiveFiles({uncache() {}, listPackage: () => ['/static/A', '/static/a'],
    statFile: () => ({size: 1})}, 'fixture'), /duplicate/);
  for (const info of [{link: '../../outside'}, {unpacked: true, size: 1}]) {
    assert.throws(() => archiveFiles({uncache() {}, listPackage: () => ['/file'],
      statFile: () => info}, 'fixture'), /linked or unpacked/);
  }
});

test('static resources match official ASAR bytes; archives, scripts and source maps remain unchanged', dependencies, async t => {
  const f = await fixture(t);
  const result = adaptPackage({source, bundle: f.bundle});
  assert.equal(result.copiedStaticFiles, f.images.size);
  assert.equal(result.reusedStaticFiles, 0);
  assert.equal(result.requiresSigning, true);
  for (const [relative, bytes] of f.images) assert.deepEqual(fs.readFileSync(path.join(f.target, relative)), bytes);
  assert.deepEqual(JSON.parse(fs.readFileSync(f.manifest, 'utf8')), PLAYER_MANIFEST);
  assertUnchanged(f.originals);
  const times = result.files.map(({path: filename}) => fs.statSync(filename, {bigint: true}).mtimeNs);
  const repeated = adaptPackage({source, bundle: f.bundle});
  assert.equal(repeated.copiedStaticFiles, 0);
  assert.equal(repeated.reusedStaticFiles, f.images.size);
  assert.equal(repeated.requiresSigning, false);
  assert.deepEqual(repeated.files.map(({path: filename}) => fs.statSync(filename, {bigint: true}).mtimeNs), times);
  assertUnchanged(f.originals);
});

test('partial matching resources are reused and missing files are added without touching sibling files', dependencies, async t => {
  const f = await fixture(t);
  const [relative, bytes] = [...f.images][0];
  const filename = write(f.target, relative, bytes);
  const before = fs.statSync(filename, {bigint: true}).mtimeNs;
  const result = adaptPackage({source, bundle: f.bundle});
  assert.equal(result.reusedStaticFiles, 1);
  assert.equal(result.copiedStaticFiles, f.images.size - 1);
  assert.equal(fs.statSync(filename, {bigint: true}).mtimeNs, before);
  assertUnchanged(f.originals);
});

test('unknown and conflicting target files fail before any resource or manifest is created', dependencies, async t => {
  for (const relative of ['static/unknown.txt', 'static/image/empty.png']) {
    const f = await fixture(t);
    const filename = write(f.target, relative, 'keep this content');
    assert.throws(() => adaptPackage({source, bundle: f.bundle}), /Unknown|differs/);
    assert.equal(fs.readFileSync(filename, 'utf8'), 'keep this content');
    assert.equal(fs.existsSync(f.manifest), false);
    assert.equal(fs.existsSync(path.join(f.target, 'static/font')), false);
    assertUnchanged(f.originals);
  }
});

test('target-directory, ancestor and manifest symlinks are rejected without following them', dependencies, async t => {
  for (const kind of ['static', 'plugins', 'manifest']) {
    const f = await fixture(t);
    const outside = path.join(f.root, 'outside');
    if (kind === 'plugins') {
      fs.renameSync(f.plugins, outside);
      fs.symlinkSync(outside, f.plugins);
    } else if (kind === 'static') {
      fs.mkdirSync(outside);
      fs.symlinkSync(outside, path.join(f.target, 'static'));
    } else {
      fs.writeFileSync(outside, JSON.stringify(PLAYER_MANIFEST));
      fs.symlinkSync(outside, f.manifest);
    }
    assert.throws(() => adaptPackage({source, bundle: f.bundle}), /symlink/);
    if (kind === 'static') assert.deepEqual(fs.readdirSync(outside), []);
    if (kind === 'manifest') assert.deepEqual(JSON.parse(fs.readFileSync(outside, 'utf8')), PLAYER_MANIFEST);
  }
});

test('source-overlapping bundles and incompatible manifests fail before resource installation', dependencies, async t => {
  const f = await fixture(t);
  assert.throws(() => validateDestination(f.root, f.bundle), /outside the source/);
  fs.writeFileSync(f.manifest, JSON.stringify({schemaVersion: 1, adapter: {id: 'unknown', version: 1}}));
  assert.throws(() => adaptPackage({source, bundle: f.bundle}), /incompatible/);
  assert.equal(fs.existsSync(path.join(f.target, 'static')), false);
  assertUnchanged(f.originals);
});

test('CLI emits JSON and an external report using the configured packaging Node runtime', dependencies, async t => {
  const f = await fixture(t);
  const report = path.join(f.root, 'report.json');
  const result = spawnSync(process.env.THUNDER_MAC_TEST_NODE || process.execPath, [
    path.join(__dirname, 'package_adapter.cjs'), '--source', source,
    '--bundle', f.bundle, '--report', report,
  ], {encoding: 'utf8'});
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(JSON.parse(result.stdout), JSON.parse(fs.readFileSync(report, 'utf8')));
  assertUnchanged(f.originals);
});
