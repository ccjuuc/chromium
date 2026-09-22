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
 * 7. Bundles everything into patch.zip using 7za
 */

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..', '..', '..');

function computeSha256(filePath) {
  const hash = crypto.createHash('sha256');
  const buffer = fs.readFileSync(filePath);
  hash.update(buffer);
  return hash.digest('hex').toUpperCase();
}

function getAllFiles(dir, baseDir = dir) {
  let results = [];
  if (!fs.existsSync(dir)) return results;
  const list = fs.readdirSync(dir);
  for (const item of list) {
    const fullPath = path.join(dir, item);
    let stat;
    try {
      stat = fs.lstatSync(fullPath);
    } catch (err) {
      continue;
    }
    const relPath = path.relative(baseDir, fullPath).replace(/\\/g, '/');
    if (stat.isSymbolicLink()) {
      results.push({
        fullPath,
        relPath,
        size: 0,
        symlink: fs.readlinkSync(fullPath)
      });
    } else if (stat.isDirectory()) {
      results = results.concat(getAllFiles(fullPath, baseDir));
    } else {
      results.push({ fullPath, relPath, size: stat.size });
    }
  }
  return results;
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
    process.exit(1);
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
      process.exit(1);
    }
  }
}

function normalizeToVersionRelative(relPath, verStr) {
  if (relPath.startsWith(verStr + '/')) {
    return { isVersionScope: true, subPath: relPath.substring(verStr.length + 1) };
  }
  return { isVersionScope: false, subPath: relPath };
}

function main() {
  const args = process.argv.slice(2);
  let oldPath = '';
  let newPath = '';
  let outputPath = path.join(rootDir, 'test_packages', 'patch.zip');

  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--old' && args[i + 1]) oldPath = args[++i];
    else if (args[i] === '--new' && args[i + 1]) newPath = args[++i];
    else if (args[i] === '--out' && args[i + 1]) outputPath = args[++i];
  }
  outputPath = path.resolve(outputPath);

  const zucchiniExe = path.join(rootDir, 'out', 'Release_64', 'zucchini.exe');
  const sevenZip = path.join(rootDir, 'third_party', 'lzma_sdk', 'bin', 'win64', '7za.exe');

  if (!oldPath || !newPath) {
    // Default to test_baselines installers if not specified
    const base1001 = path.join(rootDir, 'test_baselines', '1.0.0.1', 'xlb153_installer_1.0.0.1.exe');
    const base1002 = path.join(rootDir, 'test_baselines', '1.0.0.2', 'xlb153_installer_1.0.0.2.exe');
    if (fs.existsSync(base1001) && fs.existsSync(base1002)) {
      oldPath = base1001;
      newPath = base1002;
    } else {
      console.error('[ERROR] Please specify --old <dir|exe> and --new <dir|exe>');
      process.exit(1);
    }
  }

  const stagingDir = path.join(rootDir, 'test_packages', 'diff_staging');
  ensureDir(stagingDir);

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

  function getMacAppVersion(appDir) {
    const plistPath = path.join(appDir, 'Contents', 'Info.plist');
    if (fs.existsSync(plistPath)) {
      const content = fs.readFileSync(plistPath, 'utf8');
      const m = content.match(/<key>CFBundleShortVersionString<\/key>\s*<string>([^<]+)<\/string>/) ||
                content.match(/<key>CFBundleVersion<\/key>\s*<string>([^<]+)<\/string>/);
      if (m) return m[1];
    }
    return '1.0.0.1';
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
      process.exit(1);
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

  const oldFilesList = getAllFiles(oldRootDir);
  const newFilesList = getAllFiles(newRootDir);

  // Map normalized key (version-neutral) to old file info
  const oldFilesMap = new Map();
  for (const f of oldFilesList) {
    const norm = isMacMode ? { isVersionScope: false, subPath: f.relPath } : normalizeToVersionRelative(f.relPath, baseVersion);
    const key = (norm.isVersionScope ? '<VER>/' : '') + norm.subPath;
    oldFilesMap.set(key, f);
  }

  const actions = [];
  let copyCount = 0;
  let patchCount = 0;
  let addCount = 0;
  let totalSavedBytes = 0;

  console.log(`\n[*] Analyzing ${newFilesList.length} files in target version...`);

  for (const newFile of newFilesList) {
    const norm = isMacMode ? { isVersionScope: false, subPath: newFile.relPath } : normalizeToVersionRelative(newFile.relPath, targetVersion);
    const key = (norm.isVersionScope ? '<VER>/' : '') + norm.subPath;
    const targetRel = newFile.relPath;
    if (newFile.symlink) {
      actions.push({
        target: targetRel,
        action: 'symlink',
        link: newFile.symlink
      });
      copyCount++;
      continue;
    }
    const newSha = computeSha256(newFile.fullPath);

    const oldFile = oldFilesMap.get(key);
    if (oldFile) {
      const oldSha = computeSha256(oldFile.fullPath);
      if (oldSha === newSha) {
        // 1. Bit-for-bit identical -> copy locally (0 KB network transfer)
        actions.push({
          target: targetRel,
          action: 'copy',
          base: oldFile.relPath,
          sha256: newSha,
          size: newFile.size
        });
        copyCount++;
        totalSavedBytes += newFile.size;
        continue;
      }

      // 2. Content modified: check if eligible for Zucchini differential patch
      const ext = path.extname(newFile.fullPath).toLowerCase();
      const isPe = (ext === '.dll' || ext === '.exe');
      const isDiffEligible = newFile.size > 64 * 1024;

      if (isDiffEligible) {
        const patchRel = `patches/${targetRel}.zucc`;
        const patchFull = path.join(bundleDir, patchRel);
        ensureDir(path.dirname(patchFull));

        const cmdArgs = isPe ? ['-gen', oldFile.fullPath, newFile.fullPath, patchFull]
                             : ['-gen', '-raw', oldFile.fullPath, newFile.fullPath, patchFull];
        const res = spawnSync(zucchiniExe, cmdArgs, { stdio: 'ignore' });
        if (res.status === 0 && fs.existsSync(patchFull)) {
          const patchStat = fs.statSync(patchFull);
          // Verify roundtrip
          const verifyTmp = path.join(stagingDir, 'verify.tmp');
          const resApply = spawnSync(zucchiniExe, ['-apply', oldFile.fullPath, patchFull, verifyTmp], { stdio: 'ignore' });
          const verified = resApply.status === 0 && fs.existsSync(verifyTmp) && computeSha256(verifyTmp) === newSha;
          if (fs.existsSync(verifyTmp)) fs.unlinkSync(verifyTmp);

          if (verified && patchStat.size < newFile.size * 0.85) {
            actions.push({
              target: targetRel,
              action: 'patch',
              base: oldFile.relPath,
              patch: patchRel,
              sha256: newSha,
              size: newFile.size,
              patch_size: patchStat.size
            });
            patchCount++;
            totalSavedBytes += (newFile.size - patchStat.size);
            console.log(`  [PATCH] ${targetRel} (${isPe ? 'PE' : 'RAW'}): ${(newFile.size / 1024 / 1024).toFixed(2)} MB -> ${(patchStat.size / 1024).toFixed(1)} KB (Saved ${((1 - patchStat.size / newFile.size) * 100).toFixed(1)}%)`);
            continue;
          }
        }
      }
    }

    // 3. Newly added file or non-PE modified asset -> bundle in files/
    const sourceRel = `files/${targetRel}`;
    const destFull = path.join(bundleDir, sourceRel);
    ensureDir(path.dirname(destFull));
    fs.copyFileSync(newFile.fullPath, destFull);

    actions.push({
      target: targetRel,
      action: 'add',
      source: sourceRel,
      sha256: newSha,
      size: newFile.size
    });
    addCount++;
    console.log(`  [ADD]   ${targetRel}: ${(newFile.size / 1024).toFixed(1)} KB`);
  }

  // 4. Identify deletions
  const newKeysSet = new Set(newFilesList.map(f => {
    const norm = isMacMode ? { isVersionScope: false, subPath: f.relPath } : normalizeToVersionRelative(f.relPath, targetVersion);
    return (norm.isVersionScope ? '<VER>/' : '') + norm.subPath;
  }));

  const deletions = [];
  for (const oldFile of oldFilesList) {
    const norm = isMacMode ? { isVersionScope: false, subPath: oldFile.relPath } : normalizeToVersionRelative(oldFile.relPath, baseVersion);
    const key = (norm.isVersionScope ? '<VER>/' : '') + norm.subPath;
    if (!newKeysSet.has(key)) {
      deletions.push(oldFile.relPath);
    }
  }

  const manifest = {
    platform: isMacMode ? 'mac' : 'win',
    base_version: baseVersion,
    target_version: targetVersion,
    actions: actions,
    deletions: deletions
  };

  const manifestPath = path.join(bundleDir, 'manifest.json');
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2), 'utf8');

  console.log(`\n=== Manifest Summary ===`);
  console.log(`Total Target Files:  ${actions.length}`);
  console.log(`  - Local Copy (0 KB): ${copyCount} files`);
  console.log(`  - Differential Patch: ${patchCount} files`);
  console.log(`  - Added/Modified:     ${addCount} files`);
  console.log(`  - Deletions:          ${deletions.length} files`);
  console.log(`Bandwidth Saved:       ${(totalSavedBytes / 1024 / 1024).toFixed(2)} MB`);

  // 5. Pack bundle into patch.zip using 7za
  console.log(`\n[*] Packing into ${outputPath}...`);
  if (fs.existsSync(outputPath)) {
    fs.unlinkSync(outputPath);
  }
  ensureDir(path.dirname(outputPath));

  const resZip = spawnSync(sevenZip, ['a', '-tzip', outputPath, '*'], { cwd: bundleDir, stdio: 'inherit' });
  if (resZip.status !== 0) {
    console.error(`[ERROR] 7za zip creation failed with code: ${resZip.status}`);
    process.exit(1);
  }

  const finalStat = fs.statSync(outputPath);
  const finalSha = computeSha256(outputPath);
  console.log(`\n[+] Successfully created differential package!`);
  console.log(`    File:   ${outputPath}`);
  console.log(`    Size:   ${(finalStat.size / 1024 / 1024).toFixed(2)} MB (${finalStat.size} bytes)`);
  console.log(`    SHA256: ${finalSha}`);

  // Cleanup staging
  try { fs.rmSync(stagingDir, { recursive: true, force: true }); } catch (e) {}
}

main();
