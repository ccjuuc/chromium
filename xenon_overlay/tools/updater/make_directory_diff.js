#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** Build a complete reconstruction manifest and a ZIP of changed payloads. */
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const rootDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

function sha256(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const hash = crypto.createHash('sha256');
    const buffer = Buffer.alloc(1024 * 1024);
    let count;
    while ((count = fs.readSync(fd, buffer, 0, buffer.length, null)) > 0) {
      hash.update(buffer.subarray(0, count));
    }
    return hash.digest('hex').toUpperCase();
  } finally {
    fs.closeSync(fd);
  }
}

function runChecked(run, tool, args, options = {}) {
  const result = run(tool, args, { encoding: 'utf8', ...options });
  if (result.error || result.status !== 0) {
    const reason = result.error?.message || (result.signal ? `signal ${result.signal}` : `exit ${result.status}`);
    throw new Error(`${tool} failed: ${reason}${result.stderr ? `\n${result.stderr}` : ''}`);
  }
  return result;
}

export function findExecutable(candidates, platform = process.platform) {
  for (const candidate of candidates.filter(Boolean)) {
    const dirs = candidate.includes('/') || candidate.includes('\\') ? [''] :
      (process.env.PATH || '').split(path.delimiter);
    const suffixes = platform === 'win32' && !path.extname(candidate) ?
      ['', ...(process.env.PATHEXT || '.EXE;.CMD;.BAT').split(';')] : [''];
    for (const dir of dirs) {
      for (const suffix of suffixes) {
        const executable = path.resolve(dir, candidate + suffix);
        try {
          fs.accessSync(executable, platform === 'win32' ? fs.constants.F_OK : fs.constants.X_OK);
          if (fs.statSync(executable).isFile()) return executable;
        } catch {}
      }
    }
  }
  return undefined;
}

function resolveTool(override, candidates, flag, platform) {
  const tool = findExecutable(override ? [override] : candidates, platform);
  if (override && !tool) throw new Error(`${flag} executable not found: ${override}`);
  return tool;
}

export function validateVersion(version) {
  if (!/^(0|[1-9]\d*)(\.(0|[1-9]\d*)){3}$/.test(version) ||
      version.split('.').some(part => BigInt(part) > 0xffffffffn)) {
    throw new Error(`Expected a four-component version, got "${version}"`);
  }
  return version.split('.').map(Number);
}

function requireNewer(base, target) {
  const before = validateVersion(base);
  const after = validateVersion(target);
  for (let i = 0; i < 4; i++) {
    if (after[i] > before[i]) return;
    if (after[i] < before[i]) break;
  }
  throw new Error(`Target version ${target} must be newer than base ${base}`);
}

function appVersion(dir, run, platform) {
  const plist = path.join(dir, 'Contents', 'Info.plist');
  if (!fs.existsSync(plist)) throw new Error(`Missing app Info.plist: ${plist}`);
  let version;
  if (platform === 'darwin') {
    version = runChecked(run, '/usr/bin/plutil',
        ['-extract', 'CFBundleShortVersionString', 'raw', plist]).stdout.trim();
  } else {
    const xml = fs.readFileSync(plist, 'utf8');
    version = xml.match(/<key>CFBundleShortVersionString<\/key>\s*<string>([^<]+)<\/string>/)?.[1];
    if (!version) throw new Error(`Cannot read CFBundleShortVersionString from ${plist}; binary plists require macOS`);
  }
  validateVersion(version);
  return version;
}

function windowsVersion(dir) {
  const versions = fs.readdirSync(dir).filter(name =>
    /^\d+(\.\d+)+$/.test(name) && fs.lstatSync(path.join(dir, name)).isDirectory());
  if (versions.length !== 1) {
    throw new Error(`Expected exactly one version directory in ${dir}, found ${versions.length}`);
  }
  validateVersion(versions[0]);
  return versions[0];
}

function safeRelative(relative) {
  if (!relative || relative.includes('\\') || relative.includes(':') ||
      path.posix.isAbsolute(relative) || relative.split('/').some(p => !p || p === '.' || p === '..')) {
    throw new Error(`Unsupported manifest path: ${relative}`);
  }
}

export function listFiles(dir, base = dir) {
  const files = [];
  for (const item of fs.readdirSync(dir).sort()) {
    const fullPath = path.join(dir, item);
    const relPath = path.relative(base, fullPath).split(path.sep).join('/');
    safeRelative(relPath);
    const stat = fs.lstatSync(fullPath);
    if (stat.isSymbolicLink()) {
      const link = fs.readlinkSync(fullPath);
      const resolved = path.posix.normalize(path.posix.join(path.posix.dirname(relPath), link));
      if (!link || path.posix.isAbsolute(link) || link.includes('\\') || link.includes(':') ||
          resolved === '..' || resolved.startsWith('../')) {
        throw new Error(`Symlink escapes the input tree: ${relPath} -> ${link}`);
      }
      files.push({ fullPath, relPath, symlink: link });
    } else if (stat.isDirectory()) {
      files.push(...listFiles(fullPath, base));
    } else if (stat.isFile()) {
      files.push({ fullPath, relPath, size: stat.size, mode: stat.mode & 0o777 });
    } else {
      throw new Error(`Unsupported file type: ${fullPath}`);
    }
  }
  return files;
}

function includeSetup(files, root, version) {
  const target = `${version}/Installer/setup.exe`;
  const setup = path.join(path.basename(root) === 'Chrome-bin' ? path.dirname(root) : root, 'setup.exe');
  if (!files.some(file => file.relPath === target) && fs.existsSync(setup)) {
    const stat = fs.lstatSync(setup);
    if (!stat.isFile()) throw new Error(`Installer setup must be a regular file: ${setup}`);
    // Record a virtual path; never write into either source installation.
    files.push({ fullPath: setup, relPath: target, size: stat.size, mode: stat.mode & 0o777 });
  }
}

export function createManifest({ oldRoot, newRoot, baseVersion, targetVersion, isMac,
                                  bundleDir, stagingDir, zucchini, run = spawnSync }) {
  requireNewer(baseVersion, targetVersion);
  const oldFiles = listFiles(oldRoot);
  const newFiles = listFiles(newRoot);
  if (!isMac) {
    includeSetup(oldFiles, oldRoot, baseVersion);
    includeSetup(newFiles, newRoot, targetVersion);
  }
  const keyFor = (file, version) => {
    if (!isMac) return file.relPath.startsWith(`${version}/`) ?
      `<VER>/${file.relPath.slice(version.length + 1)}` : file.relPath;
    const components = file.relPath.split('/');
    // Chromium's framework version directory changes with the app version.
    // Match only that exact version in a framework's Versions directory: a
    // resource's numeric directory or a third-party framework version is not
    // interchangeable. These comparison keys never replace manifest paths.
    // A null component cannot collide with a real directory named "<VER>".
    return JSON.stringify(components.map((component, index) =>
      component === version && index >= 2 && index + 1 < components.length &&
      components[index - 1] === 'Versions' && components[index - 2].endsWith('.framework') ?
        null : component));
  };
  const oldByKey = new Map(oldFiles.map(file => [keyFor(file, baseVersion), file]));
  const actions = [];
  for (const file of newFiles) {
    if (file.symlink !== undefined) {
      if (!isMac) throw new Error(`Windows symlink actions are not supported: ${file.relPath}`);
      actions.push({ target: file.relPath, action: 'symlink', link: file.symlink });
      continue;
    }
    const metadata = { target: file.relPath, sha256: sha256(file.fullPath), size: file.size, mode: file.mode };
    const before = oldByKey.get(keyFor(file, targetVersion));
    // A link and a regular file are different even if their contents match.
    if (before && before.symlink === undefined) {
      if (sha256(before.fullPath) === metadata.sha256) {
        actions.push({ ...metadata, action: 'copy', base: before.relPath });
        continue;
      }
      if (file.size > 64 * 1024) {
        if (!zucchini) throw new Error('Zucchini is required for modified files over 64 KiB; pass --zucchini <native executable>');
        const patch = `patches/${file.relPath}.zucc`;
        const output = path.join(bundleDir, patch);
        fs.mkdirSync(path.dirname(output), { recursive: true });
        const raw = /\.(dll|exe)$/i.test(file.relPath) ? [] : ['-raw'];
        runChecked(run, zucchini, ['-gen', ...raw, before.fullPath, file.fullPath, output]);
        const verification = path.join(stagingDir, 'verify.tmp');
        runChecked(run, zucchini, ['-apply', before.fullPath, output, verification]);
        if (sha256(verification) !== metadata.sha256) throw new Error(`Patch roundtrip failed for ${file.relPath}`);
        fs.unlinkSync(verification);
        const patchSize = fs.statSync(output).size;
        if (patchSize < file.size * 0.85) {
          actions.push({ ...metadata, action: 'patch', base: before.relPath, patch, patch_size: patchSize });
          continue;
        }
        fs.unlinkSync(output);
      }
    }
    const source = `files/${file.relPath}`;
    const payload = path.join(bundleDir, source);
    fs.mkdirSync(path.dirname(payload), { recursive: true });
    fs.copyFileSync(file.fullPath, payload);
    fs.chmodSync(payload, file.mode);
    actions.push({ ...metadata, action: 'add', source });
  }
  const targetKeys = new Set(newFiles.map(file => keyFor(file, targetVersion)));
  return {
    platform: isMac ? 'mac' : 'win', base_version: baseVersion, target_version: targetVersion,
    actions, deletions: oldFiles.filter(file => !targetKeys.has(keyFor(file, baseVersion))).map(file => file.relPath),
  };
}

function within(parent, child) {
  const relative = path.relative(parent, child);
  return relative === '' || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}

function canonicalOutput(output) {
  let ancestor = output;
  const suffix = [];
  while (!fs.existsSync(ancestor)) {
    suffix.unshift(path.basename(ancestor));
    const next = path.dirname(ancestor);
    if (next === ancestor) throw new Error(`Cannot resolve output path: ${output}`);
    ancestor = next;
  }
  return path.join(fs.realpathSync(ancestor), ...suffix);
}

export function buildPackage(options, { run = spawnSync, platform = process.platform,
                                        tempRoot = os.tmpdir(), projectRoot = rootDir } = {}) {
  const oldPath = fs.realpathSync(options.old);
  const newPath = fs.realpathSync(options.new);
  const output = canonicalOutput(path.resolve(options.out || path.join(projectRoot, 'test_packages/patch.zip')));
  if (within(oldPath, output) || within(newPath, output)) throw new Error('Output must be outside both input trees');
  const sevenZip = resolveTool(options.sevenZip, platform === 'win32' ?
    [path.join(projectRoot, 'third_party/lzma_sdk/bin/win64/7za.exe'), '7z', '7za'] :
    ['7zz', '7z', '7za'], '--7zip', platform);
  const zip = resolveTool(options.zip, platform === 'win32' ? [] : ['/usr/bin/zip', 'zip'], '--zip', platform);
  const zucchiniName = platform === 'win32' ? 'zucchini.exe' : 'zucchini';
  const zucchini = resolveTool(options.zucchini,
    ['Release_64', 'release_arm64', 'Release', 'Default'].map(dir => path.join(projectRoot, 'out', dir, zucchiniName)).concat(zucchiniName),
    '--zucchini', platform);
  if (!sevenZip && !zip) throw new Error('No ZIP archiver found; pass --7zip or --zip');
  const staging = fs.mkdtempSync(path.join(tempRoot, 'xenon-diff-'));
  let publishTemp;
  try {
    const extract = (input, name) => {
      let dir = input;
      if (fs.statSync(input).isFile()) {
        if (!sevenZip) throw new Error('Installer extraction requires --7zip <native executable>');
        dir = path.join(staging, name);
        fs.mkdirSync(dir);
        runChecked(run, sevenZip, ['x', input, `-o${dir}`, '-y']);
        const embedded = path.join(dir, 'chrome.7z');
        const hasBrowserTree = () => fs.existsSync(path.join(dir, 'Chrome-bin')) ||
          fs.readdirSync(dir).some(entry => /^\d+(\.\d+){3}$/.test(entry) &&
            fs.lstatSync(path.join(dir, entry)).isDirectory());
        // Chromium mini-installers can contain both a CAB setup resource and a
        // separate 7z browser resource. Auto-detection stops at the first CAB,
        // so explicitly select 7z when it did not expose the browser payload.
        if (path.extname(input).toLowerCase() === '.exe' &&
            !fs.existsSync(embedded) && !hasBrowserTree()) {
          runChecked(run, sevenZip, ['x', '-t7z', input, `-o${dir}`, '-y']);
          if (!fs.existsSync(embedded) && !hasBrowserTree()) {
            throw new Error(`Installer contains no browser payload: ${input}`);
          }
        }
        if (fs.existsSync(embedded)) runChecked(run, sevenZip, ['x', embedded, `-o${dir}`, '-y']);
      }
      return fs.existsSync(path.join(dir, 'Chrome-bin')) ? path.join(dir, 'Chrome-bin') : dir;
    };
    const oldRoot = extract(oldPath, 'old');
    const newRoot = extract(newPath, 'new');
    const isApp = dir => path.extname(dir) === '.app' || fs.existsSync(path.join(dir, 'Contents/Info.plist'));
    if (isApp(oldRoot) !== isApp(newRoot)) throw new Error('Both inputs must use the same platform layout');
    const isMac = isApp(oldRoot);
    const baseVersion = isMac ? appVersion(oldRoot, run, platform) : windowsVersion(oldRoot);
    const targetVersion = isMac ? appVersion(newRoot, run, platform) : windowsVersion(newRoot);
    const bundleDir = path.join(staging, 'bundle');
    fs.mkdirSync(bundleDir);
    const manifest = createManifest({ oldRoot, newRoot, baseVersion, targetVersion, isMac,
      bundleDir, stagingDir: staging, zucchini, run });
    fs.writeFileSync(path.join(bundleDir, 'manifest.json'), JSON.stringify(manifest, null, 2));
    const archive = path.join(staging, 'patch.zip');
    if (options.zip || !sevenZip) runChecked(run, zip, ['-q', '-r', archive, '.'], { cwd: bundleDir });
    else runChecked(run, sevenZip, ['a', '-tzip', archive, '*'], { cwd: bundleDir });
    const size = fs.statSync(archive).size;
    if (!size) throw new Error('Archiver produced an empty package');
    const hash = sha256(archive);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    publishTemp = path.join(path.dirname(output), `.xenon-diff-${crypto.randomUUID()}.tmp`);
    fs.copyFileSync(archive, publishTemp, fs.constants.COPYFILE_EXCL);
    fs.renameSync(publishTemp, output);
    return { output, size, sha256: hash, manifest };
  } finally {
    if (publishTemp) fs.rmSync(publishTemp, { force: true });
    fs.rmSync(staging, { recursive: true, force: true });
  }
}

function parseArgs(args) {
  const options = {};
  const names = { '--old': 'old', '--new': 'new', '--out': 'out', '--zucchini': 'zucchini', '--7zip': 'sevenZip', '--zip': 'zip' };
  for (let i = 0; i < args.length; i++) {
    if (!names[args[i]] || !args[i + 1] || args[i + 1].startsWith('--')) throw new Error(`Unknown or incomplete argument: ${args[i]}`);
    options[names[args[i]]] = args[++i];
  }
  if (!options.old && !options.new) {
    options.old = path.join(rootDir, 'test_baselines/1.0.0.1/xlb153_installer_1.0.0.1.exe');
    options.new = path.join(rootDir, 'test_baselines/1.0.0.2/xlb153_installer_1.0.0.2.exe');
  }
  if (!options.old || !options.new) throw new Error('Specify both --old <dir|exe> and --new <dir|exe>');
  return options;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const result = buildPackage(parseArgs(process.argv.slice(2)));
    console.log(`[+] ${result.manifest.base_version} -> ${result.manifest.target_version}: ${result.output}`);
    console.log(`    ${result.size} bytes, SHA256: ${result.sha256}`);
  } catch (error) {
    console.error(`[ERROR] ${error.message}`);
    process.exitCode = 1;
  }
}
