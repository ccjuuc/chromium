#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Full-Directory Differential Package Generator
 *
 * Compares two complete installation trees (or installer exes), generates:
 * 1. Zucchini disassembly-aware patches (.zucc) for modified PE binaries (.dll, .exe)
 * 2. Zucchini raw-delta patches (.zucc) for large modified binary resource files (> 64KB)
 * 3. Copy directives for bit-identical files (0 byte network transfer)
 * 4. Full payload files for newly added files or tiny modified assets (< 64KB)
 * 5. Deletions list for removed files
 * 6. A declarative manifest.json describing the exact reconstruction tree
 * 7. Preserves macOS bundle modes/links and archives with ditto, zip, or 7za
 */

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..', '..', '..');

export function computeSha256(filePath) {
  const hash = crypto.createHash('sha256');
  const buffer = fs.readFileSync(filePath);
  hash.update(buffer);
  return hash.digest('hex').toUpperCase();
}

export function getAllFiles(dir, baseDir = dir, isMac = false) {
  const results = [];
  for (const item of fs.readdirSync(dir).sort()) {
    const fullPath = path.join(dir, item);
    const relPath = path.relative(baseDir, fullPath).split(path.sep).join('/');
    if (isMac && (relPath.includes('\\') || relPath.includes('\0') ||
        relPath.split('/').some(part => !part || part === '.' || part === '..'))) {
      throw new Error(`Unsafe bundle path: ${relPath}`);
    }
    // Framework links are metadata, not another copy of the linked tree.
    const stat = isMac ? fs.lstatSync(fullPath) : fs.statSync(fullPath);
    const entry = { fullPath, relPath, size: stat.size, mode: stat.mode & 0o777 };
    if (stat.isSymbolicLink()) {
      const rawLinkTarget = fs.readlinkSync(fullPath);
      const linkTarget = rawLinkTarget.split(path.sep).join('/');
      const resolved = path.posix.normalize(path.posix.join(
          path.posix.dirname(relPath), linkTarget));
      if (!linkTarget || linkTarget.includes('\\') || path.isAbsolute(rawLinkTarget) ||
          path.posix.isAbsolute(linkTarget) ||
          resolved === '..' || resolved.startsWith('../')) {
        throw new Error(`Symbolic link escapes app: ${relPath} -> ${linkTarget}`);
      }
      results.push({ ...entry, type: 'symlink', link_target: linkTarget });
    } else if (stat.isDirectory()) {
      if (isMac) results.push({ ...entry, type: 'directory' });
      results.push(...getAllFiles(fullPath, baseDir, isMac));
    } else if (stat.isFile()) {
      results.push({ ...entry, type: 'file' });
    } else {
      throw new Error(`Unsupported file type: ${fullPath}`);
    }
  }
  return results;
}

export function getMacAppVersion(appDir, {
  hostPlatform = process.platform, runCommand = spawnSync,
} = {}) {
  const plistPath = path.join(appDir, 'Contents', 'Info.plist');
  for (const key of ['CFBundleShortVersionString', 'CFBundleVersion']) {
    let version;
    if (hostPlatform === 'darwin') {
      // plutil reads binary and XML plists without modifying the source bundle.
      const result = runCommand('/usr/bin/plutil',
          ['-extract', key, 'raw', '-o', '-', plistPath], { encoding: 'utf8' });
      if (result.status === 0) version = result.stdout.trim();
    } else {
      const content = fs.readFileSync(plistPath);
      if (content.subarray(0, 8).toString() === 'bplist00') {
        throw new Error('Binary Info.plist requires macOS plutil');
      }
      const match = content.toString('utf8').match(new RegExp(
          `<key>${key}</key>\\s*<string>([^<]+)</string>`));
      if (match) version = match[1].trim();
    }
    if (version && /^\d+(\.\d+)*$/.test(version)) return version;
  }
  throw new Error(`No numeric bundle version in ${plistPath}`);
}

export function normalizedPath(relPath, version, isMac) {
  if (!isMac) {
    const norm = normalizeToVersionRelative(relPath, version);
    return (norm.isVersionScope ? '<VER>/' : '') + norm.subPath;
  }
  const parts = relPath.split('/');
  for (let i = 2; i < parts.length; i++) {
    if (parts[i] === version && parts[i - 1] === 'Versions' &&
        parts[i - 2].endsWith('.framework')) parts[i] = '<VER>';
  }
  return parts.join('/');
}

function ensureDir(dirPath) {
  if (!fs.existsSync(dirPath)) {
    fs.mkdirSync(dirPath, { recursive: true });
  }
}

function extractInstaller(installerExe, destDir, sevenZip) {
  ensureDir(destDir);
  console.log(`[*] Extracting installer: ${installerExe} -> ${destDir}`);
  const res = spawnSync(sevenZip, ['x', installerExe, `-o${destDir}`, '-y'], { stdio: 'ignore' });
  if (res.status !== 0) {
    console.error(`[ERROR] Failed to extract installer: ${installerExe}`);
    throw new Error('Directory diff generation failed');
  }

  // Look for chrome.7z in destDir or beside installerExe
  let chrome7z = path.join(destDir, 'chrome.7z');
  if (!fs.existsSync(chrome7z)) {
    const candidate = path.join(path.dirname(installerExe), 'chrome.7z');
    if (fs.existsSync(candidate)) {
      chrome7z = candidate;
    }
  }
  if (fs.existsSync(chrome7z)) {
    console.log(`[*] Extracting chrome.7z: ${chrome7z} -> ${destDir}`);
    const res7z = spawnSync(sevenZip, ['x', chrome7z, `-o${destDir}`, '-y'], { stdio: 'ignore' });
    if (res7z.status !== 0) {
      console.error(`[ERROR] Failed to extract chrome.7z`);
      throw new Error('Directory diff generation failed');
    }
  }
}

function normalizeToVersionRelative(relPath, verStr) {
  if (relPath.startsWith(verStr + '/')) {
    return { isVersionScope: true, subPath: relPath.substring(verStr.length + 1) };
  }
  return { isVersionScope: false, subPath: relPath };
}

export function defaultTools(hostPlatform = process.platform) {
  return {
    zucchiniExe: path.join(rootDir, 'out', 'Release_64',
        hostPlatform === 'win32' ? 'zucchini.exe' : 'zucchini'),
    sevenZip: hostPlatform === 'win32' ? path.join(rootDir,
        'third_party', 'lzma_sdk', 'bin', 'win64', '7za.exe') : undefined,
  };
}

// Keep manifest generation separate from CLI startup and external tools so it
// can be tested against real directory trees without a Chromium build.
export function buildDiffManifest({
  oldRootDir, newRootDir, bundleDir, baseVersion, targetVersion, isMacMode,
  zucchiniExe = defaultTools().zucchiniExe, runCommand = spawnSync,
}) {
  for (const root of [oldRootDir, newRootDir]) {
    if (!fs.lstatSync(root).isDirectory()) {
      throw new Error(`Input is not a directory: ${root}`);
    }
  }
  const oldEntries = getAllFiles(oldRootDir, oldRootDir, isMacMode);
  const newEntries = getAllFiles(newRootDir, newRootDir, isMacMode);
  const oldMap = new Map(oldEntries.map(entry => [
    normalizedPath(entry.relPath, baseVersion, isMacMode), entry,
  ]));
  ensureDir(bundleDir);
  const actions = [];
  for (const entry of newEntries) {
    if (entry.type === 'directory') {
      actions.push({ target: entry.relPath, action: 'directory', mode: entry.mode });
      continue;
    }
    if (entry.type === 'symlink') {
      actions.push({ target: entry.relPath, action: 'symlink',
        link_target: entry.link_target });
      continue;
    }
    const metadata = { sha256: computeSha256(entry.fullPath), size: entry.size,
      ...(isMacMode ? { mode: entry.mode } : {}) };
    const oldEntry = oldMap.get(normalizedPath(
        entry.relPath, targetVersion, isMacMode));
    if (oldEntry?.type === 'file') {
      if (computeSha256(oldEntry.fullPath) === metadata.sha256) {
        actions.push({ target: entry.relPath, action: 'copy',
          base: oldEntry.relPath, ...metadata });
        continue;
      }
      if (entry.size > 64 * 1024) {
        const patchRel = `patches/${entry.relPath}.zucc`;
        const patchFile = path.join(bundleDir, patchRel);
        ensureDir(path.dirname(patchFile));
        const isPe = /\.(dll|exe)$/i.test(entry.relPath);
        const result = runCommand(zucchiniExe,
            ['-gen', ...(isPe ? [] : ['-raw']),
              oldEntry.fullPath, entry.fullPath, patchFile], { stdio: 'ignore' });
        if (result.status === 0 && fs.existsSync(patchFile)) {
          const verifyFile = patchFile + '.verify';
          const verify = runCommand(zucchiniExe,
              ['-apply', oldEntry.fullPath, patchFile, verifyFile],
              { stdio: 'ignore' });
          const verified = verify.status === 0 && fs.existsSync(verifyFile) &&
              computeSha256(verifyFile) === metadata.sha256;
          fs.rmSync(verifyFile, { force: true });
          const patchSize = fs.statSync(patchFile).size;
          if (verified && patchSize < entry.size * 0.85) {
            actions.push({ target: entry.relPath, action: 'patch',
              base: oldEntry.relPath, patch: patchRel, ...metadata,
              patch_size: patchSize });
            continue;
          }
        }
        fs.rmSync(patchFile, { force: true });
      }
    }
    const source = `files/${entry.relPath}`;
    const payload = path.join(bundleDir, source);
    ensureDir(path.dirname(payload));
    fs.copyFileSync(entry.fullPath, payload);
    if (isMacMode) fs.chmodSync(payload, entry.mode);
    actions.push({ target: entry.relPath, action: 'add', source, ...metadata });
  }
  const newKeys = new Set(newEntries.map(entry =>
    normalizedPath(entry.relPath, targetVersion, isMacMode)));
  return {
    ...(isMacMode ? { schema_version: 2,
      root_mode: fs.lstatSync(newRootDir).mode & 0o777 } : {}),
    platform: isMacMode ? 'mac' : 'win',
    base_version: baseVersion,
    target_version: targetVersion,
    actions,
    deletions: oldEntries.filter(entry => !newKeys.has(
        normalizedPath(entry.relPath, baseVersion, isMacMode)))
        .map(entry => entry.relPath),
  };
}

export function packBundle(bundleDir, outputPath, {
  sevenZip, hostPlatform = process.platform, runCommand = spawnSync,
} = {}) {
  outputPath = path.resolve(outputPath);
  ensureDir(path.dirname(outputPath));
  fs.rmSync(outputPath, { force: true });
  let result;
  if (sevenZip) {
    result = runCommand(sevenZip, ['a', '-tzip', outputPath, '*'],
        { cwd: bundleDir, stdio: 'inherit' });
  } else if (hostPlatform === 'darwin') {
    result = runCommand('/usr/bin/ditto',
        ['-c', '-k', '--norsrc', bundleDir, outputPath], { stdio: 'inherit' });
  } else {
    result = runCommand('zip', ['-r', '-y', outputPath, '.'],
        { cwd: bundleDir, stdio: 'inherit' });
  }
  if (result.status !== 0) {
    throw new Error(`Archive creation failed: ${result.error?.message ?? result.status}`);
  }
}

export function main(args = process.argv.slice(2)) {
  let oldPath = '';
  let newPath = '';
  let outputPath = path.join(rootDir, 'test_packages', 'patch.zip');
  let { zucchiniExe, sevenZip } = defaultTools();

  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--old' && args[i + 1]) oldPath = args[++i];
    else if (args[i] === '--new' && args[i + 1]) newPath = args[++i];
    else if (args[i] === '--out' && args[i + 1]) outputPath = args[++i];
    else if (args[i] === '--zucchini' && args[i + 1]) zucchiniExe = args[++i];
    else if (args[i] === '--seven-zip' && args[i + 1]) sevenZip = args[++i];
    else throw new Error(`Unknown or incomplete argument: ${args[i]}`);
  }
  outputPath = path.resolve(outputPath);


  if (!oldPath || !newPath) {
    // Default to test_baselines installers if not specified
    const base1001 = path.join(rootDir, 'test_baselines', '1.0.0.1', 'xlb153_installer_1.0.0.1.exe');
    const base1002 = path.join(rootDir, 'test_baselines', '1.0.0.2', 'xlb153_installer_1.0.0.2.exe');
    if (fs.existsSync(base1001) && fs.existsSync(base1002)) {
      oldPath = base1001;
      newPath = base1002;
    } else {
      console.error('[ERROR] Please specify --old <dir|exe> and --new <dir|exe>');
      throw new Error('Directory diff generation failed');
    }
  }

  const stagingDir = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-directory-diff-'));
  try {

    let oldRootDir = oldPath;
    let newRootDir = newPath;

    // If inputs are installer executables, extract them first
    if (fs.statSync(oldPath).isFile()) {
      oldRootDir = path.join(stagingDir, 'old_tree');
      extractInstaller(oldPath, oldRootDir, sevenZip);
    }
    if (fs.statSync(newPath).isFile()) {
      newRootDir = path.join(stagingDir, 'new_tree');
      extractInstaller(newPath, newRootDir, sevenZip);
    }

    // Adjust for Chrome-bin subdir if present
    if (fs.existsSync(path.join(oldRootDir, 'Chrome-bin'))) {
      oldRootDir = path.join(oldRootDir, 'Chrome-bin');
    }
    if (fs.existsSync(path.join(newRootDir, 'Chrome-bin'))) {
      newRootDir = path.join(newRootDir, 'Chrome-bin');
    }

    // Detect macOS App Bundle vs Windows multi-version directory structure
    const isMacApp = dir => fs.existsSync(path.join(dir, 'Contents', 'Info.plist')) || dir.endsWith('.app');
    const isMacMode = isMacApp(oldRootDir) || isMacApp(newRootDir);

    if (isMacApp(oldRootDir) !== isMacApp(newRootDir)) {
      throw new Error('Old and new inputs must use the same installation layout');
    }

    let baseVersion = '1.0.0.1';
    let targetVersion = '1.0.0.2';

    if (isMacMode) {
      baseVersion = getMacAppVersion(oldRootDir);
      targetVersion = getMacAppVersion(newRootDir);
      console.log(`\n=== macOS App Bundle Differential Comparison ===`);
      console.log(`Base Version:   ${baseVersion} (${oldRootDir})`);
      console.log(`Target Version: ${targetVersion} (${newRootDir})`);
    } else {
      // Detect base_version and target_version from subdirectories
      const isVersionDir = name => /^\d+(\.\d+)+$/.test(name);
      const oldVersions = fs.readdirSync(oldRootDir).filter(n => isVersionDir(n) && fs.statSync(path.join(oldRootDir, n)).isDirectory());
      const newVersions = fs.readdirSync(newRootDir).filter(n => isVersionDir(n) && fs.statSync(path.join(newRootDir, n)).isDirectory());

      if (oldVersions.length === 0 || newVersions.length === 0) {
        console.error('[ERROR] Could not detect version directories in old or new trees!');
        throw new Error('Directory diff generation failed');
      }

      baseVersion = oldVersions[0];
      targetVersion = newVersions[0];
      console.log(`\n=== Full Directory Differential Comparison ===`);
      console.log(`Base Version:   ${baseVersion} (${oldRootDir})`);
      console.log(`Target Version: ${targetVersion} (${newRootDir})`);

      // Ensure setup.exe is placed under <version>/Installer/setup.exe if present in extracted root
      for (const [rDir, vStr] of [[oldRootDir, baseVersion], [newRootDir, targetVersion]]) {
        const rootSetup = path.join(rDir.replace('Chrome-bin', ''), 'setup.exe');
        const destSetup = path.join(rDir, vStr, 'Installer', 'setup.exe');
        if (fs.existsSync(rootSetup) && !fs.existsSync(destSetup)) {
          ensureDir(path.dirname(destSetup));
          fs.copyFileSync(rootSetup, destSetup);
        }
      }
    }


    const bundleDir = path.join(stagingDir, 'bundle');
    const patchesDir = path.join(bundleDir, 'patches');
    const filesDir = path.join(bundleDir, 'files');
    ensureDir(patchesDir);
    ensureDir(filesDir);

    const manifest = buildDiffManifest({ oldRootDir, newRootDir, bundleDir,
      baseVersion, targetVersion, isMacMode, zucchiniExe });
    const manifestPath = path.join(bundleDir, 'manifest.json');
    fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2), 'utf8');

    console.log(`Actions: ${manifest.actions.length}; deletions: ${manifest.deletions.length}`);
    packBundle(bundleDir, outputPath, { sevenZip });

    const finalStat = fs.statSync(outputPath);
    const finalSha = computeSha256(outputPath);
    console.log(`\n[+] Successfully created differential package!`);
    console.log(`    File:   ${outputPath}`);
    console.log(`    Size:   ${(finalStat.size / 1024 / 1024).toFixed(2)} MB (${finalStat.size} bytes)`);
    console.log(`    SHA256: ${finalSha}`);

  } finally {
    fs.rmSync(stagingDir, { recursive: true, force: true });
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === __filename) {
  try {
    main();
  } catch (error) {
    console.error(`[ERROR] ${error.message}`);
    process.exitCode = 1;
  }
}
