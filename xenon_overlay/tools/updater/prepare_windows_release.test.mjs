// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import test from 'node:test';
import {fileHash, findSevenZip, inventory, parseListing, peMetadata, prepareRelease,
  safeRelative, validateApplication} from './prepare_windows_release.mjs';

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-windows-release-test-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  return root;
}

function peFixture(file, version = '1.0.0.2', {dll = false, machine = 0x8664} = {}) {
  const data = Buffer.alloc(1024);
  data.write('MZ'); data.writeUInt32LE(128, 60); data.write('PE\0\0', 128);
  data.writeUInt16LE(machine, 132); data.writeUInt16LE(1, 134);
  data.writeUInt16LE(240, 148); data.writeUInt16LE(dll ? 0x2022 : 0x22, 150);
  data.writeUInt16LE(machine === 0x14c ? 0x10b : 0x20b, 152);
  const directory = machine === 0x14c ? 96 : 112;
  data.writeUInt32LE(16, 152 + directory - 4);
  data.writeUInt32LE(0x1000, 152 + directory + 16); data.writeUInt32LE(180, 152 + directory + 20);
  data.write('.rsrc', 392); data.writeUInt32LE(180, 400); data.writeUInt32LE(0x1000, 404);
  data.writeUInt32LE(512, 408); data.writeUInt32LE(512, 412);
  for (const [offset, id, target] of [[0, 16, 0x80000018], [24, 1, 0x80000030], [48, 1033, 72]]) {
    data.writeUInt16LE(1, 512 + offset + 14);
    data.writeUInt32LE(id, 512 + offset + 16); data.writeUInt32LE(target, 512 + offset + 20);
  }
  data.writeUInt32LE(0x1000 + 88, 584); data.writeUInt32LE(92, 588);
  data.writeUInt16LE(92, 600); data.writeUInt16LE(52, 602);
  data.write('VS_VERSION_INFO\0', 606, 'utf16le');
  const [major, minor, build, patch] = version.split('.').map(Number);
  [0xfeef04bd, 0x10000, 142 << 16, 0, major * 65536 + minor, build * 65536 + patch,
    0x3f, 0, 4, 1, 0, 0, 0].forEach((value, i) => data.writeUInt32LE(value, 640 + i * 4));
  fs.mkdirSync(path.dirname(file), {recursive: true}); fs.writeFileSync(file, data);
  return data;
}

function application(root, version = '1.0.0.2', machine = 0x8664) {
  peFixture(path.join(root, 'xlb153.exe'), version, {machine});
  peFixture(path.join(root, version, 'xlbrowser.dll'), version, {machine, dll: true});
  peFixture(path.join(root, version, 'Installer/setup.exe'), version, {machine});
  fs.writeFileSync(path.join(root, 'empty.dat'), '');
  return root;
}

let sevenZip;
try { sevenZip = findSevenZip(); } catch {}
function command(args, cwd) {
  const result = spawnSync(sevenZip, args, {cwd, encoding: 'utf8', windowsHide: true});
  assert.equal(result.status, 0, result.stderr || result.stdout || result.error?.message);
}
function zip(source, file) { command(['a', '-tzip', file, '.'], source); return file; }
const native = {skip: !sevenZip && 'A native 7-Zip executable is required'};

test('PE reader uses PRODUCTVERSION and validates all supported architectures', t => {
  const root = fixture(t);
  for (const [machine, arch] of [[0x14c, 'x86'], [0x8664, 'x86_64'], [0xaa64, 'arm64']]) {
    const tree = application(path.join(root, arch), '1.0.0.2', machine);
    assert.equal(validateApplication(tree).arch, arch);
    const info = peMetadata(path.join(tree, '1.0.0.2/xlbrowser.dll'));
    assert.equal(info.version, '1.0.0.2');
    assert.equal(info.isDll, true);
  }
});

test('PE truncation, unsafe offsets, and conflicting payload metadata are rejected', t => {
  const root = fixture(t), tree = application(path.join(root, 'app'));
  const launcher = path.join(tree, 'xlb153.exe'), original = fs.readFileSync(launcher);
  for (const bad of [original.subarray(0, 20), original.subarray(0, 650), Buffer.alloc(1024)]) {
    fs.writeFileSync(launcher, bad); assert.throws(() => peMetadata(launcher));
  }
  const badOffset = Buffer.from(original); badOffset.writeUInt32LE(0xfffffff0, 584);
  fs.writeFileSync(launcher, badOffset); assert.throws(() => peMetadata(launcher), /outside/);
  fs.writeFileSync(launcher, original);
  assert.throws(() => validateApplication(tree, {version: '1.0.0.1'}), /Requested version/);
  assert.throws(() => validateApplication(tree, {arch: 'arm64'}), /Requested architecture/);
  peFixture(path.join(tree, '1.0.0.2/xlbrowser.dll'), '1.0.0.1', {dll: true});
  assert.throws(() => validateApplication(tree), /product version/);
  peFixture(path.join(tree, '1.0.0.2/xlbrowser.dll'), '1.0.0.2', {dll: true, machine: 0xaa64});
  assert.throws(() => validateApplication(tree), /architectures differ/);
});

test('application requires setup and exactly one version directory', t => {
  const root = fixture(t), tree = application(path.join(root, 'app'));
  fs.mkdirSync(path.join(tree, '1.0.0.1'));
  assert.throws(() => validateApplication(tree), /exactly one/);
  fs.rmdirSync(path.join(tree, '1.0.0.1'));
  fs.unlinkSync(path.join(tree, '1.0.0.2/Installer/setup.exe'));
  assert.throws(() => validateApplication(tree), /Missing required/);
});

const listing = records => '\n----------\n' + records.map(record => Object.entries(record)
  .map(([key, value]) => `${key} = ${value}`).join('\n')).join('\n\n') + '\n';
test('archive preflight rejects traversal, Windows aliases, links, bombs, and casing conflicts', () => {
  for (const value of ['../outside', '/outside', 'C:/outside', 'x/../outside', 'x//y', 'a:stream', 'AUX.txt', 'dir/file.', 'x\nother']) {
    assert.throws(() => safeRelative(value), /Unsafe/);
  }
  assert.equal(safeRelative('Chrome-bin\\1.0.0.2\\file'), 'Chrome-bin/1.0.0.2/file');
  for (const records of [
    [{Path: '../out', Size: 1}],
    [{Path: 'a', Size: 1}, {Path: 'A', Size: 1}],
    [{Path: 'A/one', Size: 1}, {Path: 'a/two', Size: 1}],
    [{Path: 'file', Size: 1}, {Path: 'file/child', Size: 1}],
    [{Path: 'file', Size: 1, 'Symbolic Link': '../outside'}],
    [{Path: 'file', Size: 1, 'Hard Link': 'other'}],
    [{Path: 'file', Size: 1, Attributes: 'A lrwxrwxrwx'}],
    [{Path: 'file', Size: 1, Encrypted: '+'}],
    [{Path: 'file', Size: 16 * 1024 ** 3 + 1}],
  ]) assert.throws(() => parseListing(listing(records)));
  const parsed = parseListing(listing([{Path: 'Chrome-bin', Folder: '+', Size: 0}, {Path: 'Chrome-bin/file', Size: 4}]));
  assert.equal(parsed.size, 2);
  assert.equal(parsed.get('Chrome-bin/file').size, 4);
});

test('application rejects hard links before packaging', t => {
  const root = fixture(t), tree = application(path.join(root, 'app'));
  fs.linkSync(path.join(tree, 'empty.dat'), path.join(root, 'alias'));
  assert.throws(() => inventory(tree), /single-link/);
});

test('native ZIP import publishes exact full tree, small baseline delta, and content identity', native, t => {
  const root = fixture(t);
  const before = application(path.join(root, 'old'), '1.0.0.1');
  const after = application(path.join(root, 'new/Chrome-bin'), '1.0.0.2');
  const same = crypto.randomBytes(256 * 1024);
  for (const dir of [before, after]) fs.writeFileSync(path.join(dir, 'resources.pak'), same);
  fs.writeFileSync(path.join(before, 'obsolete.txt'), 'remove this');
  const source = zip(path.dirname(after), path.join(root, 'input.zip'));
  const outDir = path.join(root, 'release');
  const result = prepareRelease({input: source, outDir, sevenZip,
    baselines: [{version: '1.0.0.1', arch: 'x86_64', tree: before}]});
  assert.equal(result.version, '1.0.0.2'); assert.equal(result.arch, 'x86_64');
  assert.equal(result.full.file, 'package.zip'); assert.match(result.tree_sha256, /^[a-f0-9]{64}$/);
  assert.equal(result.full.sha256, fileHash(path.join(outDir, 'package.zip')));
  assert.equal(result.differential.length, 1);
  assert.ok(result.differential[0].size < result.full.size);
  assert.deepEqual(result.differential[0].deletions, ['obsolete.txt']);
  assert.deepEqual(result.warnings, []);
  assert.equal(validateApplication(path.join(outDir, 'application')).version, '1.0.0.2');
  const unpacked = path.join(root, 'verification');
  command(['x', `-o${unpacked}`, path.join(outDir, 'package.zip')]);
  const expected = inventory(path.join(outDir, 'application'));
  assert.deepEqual([...inventory(unpacked).files.keys()], [...expected.files.keys()]);
  for (const name of expected.files.keys()) assert.equal(fileHash(path.join(unpacked, name)), fileHash(path.join(outDir, 'application', name)));
  const reimported = prepareRelease({input: path.join(outDir, 'package.zip'), outDir: path.join(root, 'again'), sevenZip});
  assert.equal(reimported.tree_sha256, result.tree_sha256);
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(outDir, 'release.json'))), result);
});

test('native ZIP import restores outer setup and requires matching user overrides', native, t => {
  const root = fixture(t), tree = application(path.join(root, 'payload/Chrome-bin'));
  fs.renameSync(path.join(tree, '1.0.0.2/Installer/setup.exe'), path.join(root, 'payload/setup.exe'));
  const source = zip(path.dirname(tree), path.join(root, 'input.zip'));
  const result = prepareRelease({input: source, outDir: path.join(root, 'release'), sevenZip, arch: 'x86_64', version: '1.0.0.2'});
  assert.equal(result.arch, 'x86_64');
  assert.ok(fs.existsSync(path.join(root, 'release/application/1.0.0.2/Installer/setup.exe')));
  for (const options of [{version: '1.0.0.3'}, {arch: 'arm64'}]) {
    const outDir = path.join(root, 'bad');
    assert.throws(() => prepareRelease({input: source, outDir, sevenZip, ...options}), /Requested/);
    assert.equal(fs.existsSync(outDir), false);
  }
});

test('invalid baseline safely falls back, while same/newer and other architectures are ignored', native, t => {
  const root = fixture(t), tree = application(path.join(root, 'payload'));
  const input = zip(tree, path.join(root, 'input.zip'));
  const baselines = ['1.0.0.1', '1.0.0.2', '1.0.0.3'].map(version => ({version, arch: 'x86_64', tree: path.join(root, 'missing')}));
  baselines.push({version: '1.0.0.0', arch: 'arm64', tree: path.join(root, 'missing')});
  const outDir = path.join(root, 'release');
  const result = prepareRelease({input, outDir, sevenZip, baselines});
  assert.equal(result.differential.length, 0); assert.equal(result.warnings.length, 1);
  assert.match(result.warnings[0], /1.0.0.1/);
  assert.ok(fs.existsSync(path.join(outDir, 'package.zip')));
});

test('larger baseline patch is omitted in favor of the full ZIP', native, t => {
  const root = fixture(t), before = application(path.join(root, 'old'), '1.0.0.1');
  const after = application(path.join(root, 'new'), '1.0.0.2');
  const input = zip(after, path.join(root, 'input.zip'));
  const result = prepareRelease({input, outDir: path.join(root, 'release'), sevenZip,
    baselines: [{version: '1.0.0.1', arch: 'x86_64', tree: before}]});
  assert.equal(result.differential.length, 0); assert.equal(result.warnings.length, 1);
  assert.match(result.warnings[0], /不小于/);
});

test('missing configured Zucchini preserves a usable full ZIP release', native, t => {
  const root = fixture(t), before = application(path.join(root, 'old'), '1.0.0.1');
  const after = application(path.join(root, 'new'), '1.0.0.2');
  const input = zip(after, path.join(root, 'input.zip'));
  const outDir = path.join(root, 'release');
  const result = prepareRelease({input, outDir, sevenZip, zucchini: path.join(root, 'missing-zucchini'),
    baselines: [{version: '1.0.0.1', arch: 'x86_64', tree: before}]});
  assert.equal(result.differential.length, 0); assert.equal(result.warnings.length, 1);
  assert.match(result.warnings[0], /executable not found/);
  assert.equal(result.full.sha256, fileHash(path.join(outDir, 'package.zip')));
});

test('modified published baseline is rejected and full release stays available', native, t => {
  const root = fixture(t), before = application(path.join(root, 'old'), '1.0.0.1');
  const after = application(path.join(root, 'new'), '1.0.0.2');
  const input = zip(after, path.join(root, 'input.zip')), outDir = path.join(root, 'release');
  const result = prepareRelease({input, outDir, sevenZip,
    baselines: [{version: '1.0.0.1', arch: 'x86_64', tree: before, tree_sha256: '0'.repeat(64)}]});
  assert.equal(result.differential.length, 0); assert.equal(result.warnings.length, 1);
  assert.match(result.warnings[0], /published identity/);
  assert.ok(fs.existsSync(path.join(outDir, 'package.zip')));
});

test('existing output and disguised packages are rejected without replacing data', native, t => {
  const root = fixture(t), tree = application(path.join(root, 'payload'));
  const input = zip(tree, path.join(root, 'input.zip')), outDir = path.join(root, 'existing');
  fs.mkdirSync(outDir); fs.writeFileSync(path.join(outDir, 'keep'), 'untouched');
  assert.throws(() => prepareRelease({input, outDir, sevenZip}), /already exists/);
  assert.equal(fs.readFileSync(path.join(outDir, 'keep'), 'utf8'), 'untouched');
  const disguised = path.join(root, 'fake.zip'); fs.copyFileSync(path.join(tree, 'xlb153.exe'), disguised);
  assert.throws(() => prepareRelease({input: disguised, outDir: path.join(root, 'bad'), sevenZip}), /bytes do not match/);
});

test('native EXE extraction works with full 7-Zip without executing its PE image', native, t => {
  const root = fixture(t), tree = application(path.join(root, 'payload/Chrome-bin'));
  const archive = path.join(root, 'browser.7z');
  command(['a', '-t7z', archive, '.'], path.dirname(tree));
  const installer = path.join(root, 'installer.exe');
  fs.writeFileSync(installer, Buffer.concat([fs.readFileSync(path.join(tree, 'xlb153.exe')), fs.readFileSync(archive)]));
  const fullTool = process.platform === 'win32' && fs.existsSync(path.join(path.dirname(sevenZip), '7zz.exe')) ?
    path.join(path.dirname(sevenZip), '7zz.exe') : sevenZip;
  const result = prepareRelease({input: installer, outDir: path.join(root, 'release'), sevenZip: fullTool});
  assert.equal(result.version, '1.0.0.2');
  assert.equal(result.arch, 'x86_64');
});

function storedZip(file, name, contents, mode = 0o100644) {
  const filename = Buffer.from(name), data = Buffer.from(contents);
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  crc = (crc ^ 0xffffffff) >>> 0;
  const local = Buffer.alloc(30), central = Buffer.alloc(46), end = Buffer.alloc(22);
  local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4);
  local.writeUInt32LE(crc, 14); local.writeUInt32LE(data.length, 18); local.writeUInt32LE(data.length, 22);
  local.writeUInt16LE(filename.length, 26);
  central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(0x0314, 4); central.writeUInt16LE(20, 6);
  central.writeUInt32LE(crc, 16); central.writeUInt32LE(data.length, 20); central.writeUInt32LE(data.length, 24);
  central.writeUInt16LE(filename.length, 28); central.writeUInt32LE((mode << 16) >>> 0, 38);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(central.length + filename.length, 12); end.writeUInt32LE(local.length + filename.length + data.length, 16);
  fs.writeFileSync(file, Buffer.concat([local, filename, data, central, filename, end]));
}

test('native archive preflight blocks malicious paths and symlinks before extraction', native, t => {
  const root = fixture(t);
  for (const [name, mode] of [['../escape.txt', 0o100644], ['link', 0o120777], ['AUX.txt', 0o100644]]) {
    const input = path.join(root, 'unsafe.zip'), outDir = path.join(root, 'release');
    storedZip(input, name, '../outside', mode);
    assert.throws(() => prepareRelease({input, outDir, sevenZip}), /Unsafe|special files|links/);
    assert.equal(fs.existsSync(outDir), false);
    assert.equal(fs.existsSync(path.join(root, 'escape.txt')), false);
    assert.equal(fs.readdirSync(root).some(entry => entry.startsWith('.xenon-windows-release-')), false);
  }
});
