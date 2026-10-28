// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const {createHash} = require('node:crypto');
const {createRequire} = require('node:module');

const PLUGIN_ARCHIVE = 'thunder-pan-plugin.asar';
const PLAYER_MANIFEST = Object.freeze({
  schemaVersion: 1,
  adapter: Object.freeze({id: 'xunlei.mac-player', version: 1}),
});

function inside(parent, child) {
  const relative = path.relative(parent, child);
  return relative === '' || (!relative.startsWith(`..${path.sep}`) &&
      relative !== '..' && !path.isAbsolute(relative));
}

function stat(filename) {
  return fs.lstatSync(filename, {throwIfNoEntry: false});
}

function regularFile(filename) {
  if (!stat(filename)?.isFile()) {
    throw new Error(`Expected a regular file, not a symlink: ${filename}`);
  }
}

function regularDirectory(filename) {
  if (!stat(filename)?.isDirectory()) {
    throw new Error(`Expected a directory, not a symlink: ${filename}`);
  }
}

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex');
}

function loadAsar(source) {
  return createRequire(path.join(source, 'app/package.json'))('@electron/asar');
}

function archiveFiles(asar, archive) {
  asar.uncache(archive);
  const seen = new Set();
  return asar.listPackage(archive).map(name => {
    // 官方 ASAR 列表带一个前导分隔符；拒绝穿越、链接和大小写冲突，避免写出静态资源目录。
    const relative = name.replace(/^\//, '');
    const parts = relative.split('/');
    const folded = relative.normalize('NFC').toLowerCase();
    if (!relative || /[\\\0]/.test(relative) ||
        parts.some(part => !part || part === '..' || part === '.') || seen.has(folded)) {
      throw new Error(`Unsafe or duplicate ASAR entry: ${name}`);
    }
    seen.add(folded);
    const info = asar.statFile(archive, relative, false);
    if (info.link || info.unpacked) {
      throw new Error(`Unexpected linked or unpacked ASAR entry: ${name}`);
    }
    return {relative, info};
  });
}

function staticResources(asar, archive) {
  const files = new Map();
  const directories = new Set(['', 'static']);
  for (const {relative, info} of archiveFiles(asar, archive)) {
    if (!relative.startsWith('static/')) continue;
    if ('size' in info) {
      const bytes = Buffer.from(asar.extractFile(archive, relative));
      if (bytes.length !== info.size) throw new Error(`Invalid ASAR file size: ${relative}`);
      files.set(relative, bytes);
    } else {
      directories.add(relative);
    }
    for (let parent = path.posix.dirname(relative); parent !== '.';
      parent = path.posix.dirname(parent)) directories.add(parent);
  }
  if (!files.size) throw new Error('Pan archive contains no static resources');
  return {files, directories};
}

function validateDestination(source, bundle) {
  if (inside(source, bundle)) {
    throw new Error('Staged bundle must be outside the source checkout');
  }
  if (!bundle.endsWith('.app')) throw new Error(`Expected a staged .app bundle: ${bundle}`);
  for (const relative of ['Contents', 'Contents/Resources', 'Contents/Resources/app',
    'Contents/Resources/app/plugins']) regularDirectory(path.join(bundle, relative));
  const resources = path.join(bundle, 'Contents/Resources');
  const plugins = path.join(resources, 'app/plugins');
  const archive = path.join(plugins, PLUGIN_ARCHIVE);
  for (const filename of [path.join(bundle, 'Contents/Info.plist'),
    path.join(resources, 'app/out.asar'), archive]) regularFile(filename);
  const target = path.join(plugins, 'thunder-pan-plugin');
  if (stat(target)) regularDirectory(target);
  const manifest = path.join(resources, 'xenon-native-player.json');
  if (stat(manifest)) {
    regularFile(manifest);
    const value = JSON.parse(fs.readFileSync(manifest, 'utf8'));
    if (value?.schemaVersion !== 1 || value.adapter?.id !== PLAYER_MANIFEST.adapter.id ||
        value.adapter?.version !== PLAYER_MANIFEST.adapter.version ||
        Object.keys(value).sort().join(',') !== 'adapter,schemaVersion' ||
        Object.keys(value.adapter).sort().join(',') !== 'id,version') {
      throw new Error('Existing native-player declaration is incompatible');
    }
  }
  return {archive, target, manifest};
}

function existingResources(target, resources) {
  const existing = new Set();
  const visit = relative => {
    const directory = path.join(target, relative);
    if (!stat(directory)) return;
    regularDirectory(directory);
    for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
      const child = path.posix.join(relative, entry.name);
      const filename = path.join(target, child);
      if (entry.isDirectory() && resources.directories.has(child)) {
        visit(child);
      } else if (entry.isFile() && resources.files.has(child)) {
        if (!fs.readFileSync(filename).equals(resources.files.get(child))) {
          throw new Error(`Existing static resource differs from the archive: ${filename}`);
        }
        existing.add(child);
      } else {
        throw new Error(`Unknown or linked entry in static resource destination: ${filename}`);
      }
    }
  };
  visit('static');
  return existing;
}

function createFile(filename, bytes, createdFiles) {
  // 只新增文件；即使预检之后出现同名文件，也绝不覆盖未知内容或跟随符号链接。
  const descriptor = fs.openSync(filename, 'wx');
  createdFiles.push(filename);
  try {
    fs.writeFileSync(descriptor, bytes);
  } finally {
    fs.closeSync(descriptor);
  }
}

function adaptPackage(options) {
  const source = fs.realpathSync(options.source);
  const bundle = fs.realpathSync(options.bundle);
  const destination = validateDestination(source, bundle);
  const originalArchiveHash = sha256(fs.readFileSync(destination.archive));
  const asar = loadAsar(source);
  const resources = staticResources(asar, destination.archive);
  const existing = existingResources(destination.target, resources);
  const createdFiles = [];
  const createdDirectories = [];
  try {
    // 原版 Mac JS 使用裸目录 publicPath；外挂原字节 static 资源，保持 JS、ASAR 和映射不变。
    const directories = [...resources.directories].sort((a, b) =>
      a.split('/').length - b.split('/').length || a.localeCompare(b));
    for (const relative of directories) {
      const directory = path.join(destination.target, relative);
      if (!stat(directory)) {
        fs.mkdirSync(directory);
        createdDirectories.push(directory);
      }
      regularDirectory(directory);
    }
    for (const [relative, bytes] of resources.files) {
      if (!existing.has(relative)) createFile(path.join(destination.target, relative), bytes, createdFiles);
    }
    if (!stat(destination.manifest)) {
      createFile(destination.manifest, `${JSON.stringify(PLAYER_MANIFEST, null, 2)}\n`, createdFiles);
    }
    const files = [];
    for (const [relative, bytes] of resources.files) {
      const filename = path.join(destination.target, relative);
      regularFile(filename);
      if (!fs.readFileSync(filename).equals(bytes)) {
        throw new Error(`Static resource verification failed: ${filename}`);
      }
      files.push({path: filename, archiveEntry: relative, sha256: sha256(bytes)});
    }
    if (sha256(fs.readFileSync(destination.archive)) !== originalArchiveHash) {
      throw new Error('Source plugin archive changed during resource assembly');
    }
    files.push({path: destination.manifest, sha256: sha256(fs.readFileSync(destination.manifest))});
    return {
      archive: {path: destination.archive, sha256: originalArchiveHash},
      copiedStaticFiles: resources.files.size - existing.size,
      reusedStaticFiles: existing.size,
      requiresSigning: createdFiles.length > 0,
      files,
    };
  } catch (error) {
    // 回滚仅删除本次以独占方式创建的文件和空目录，不递归删除已有内容。
    for (const filename of createdFiles.reverse()) fs.rmSync(filename, {force: true});
    for (const directory of createdDirectories.reverse()) {
      try { fs.rmdirSync(directory); } catch (_) { /* 已有内容留给调用方处理。 */ }
    }
    throw error;
  }
}

function parseArguments(args) {
  const names = {'--source': 'source', '--bundle': 'bundle', '--report': 'report'};
  const result = {};
  for (let index = 0; index < args.length; index += 2) {
    const name = names[args[index]];
    if (!name || !args[index + 1] || args[index + 1].startsWith('--') || name in result) {
      throw new Error(`Invalid or repeated option: ${args[index]}`);
    }
    result[name] = args[index + 1];
  }
  for (const required of ['source', 'bundle']) {
    if (!result[required]) throw new Error(`Missing argument: ${required}`);
  }
  return result;
}

module.exports = {
  adaptPackage, archiveFiles, loadAsar, parseArguments,
  staticResources, validateDestination, PLAYER_MANIFEST,
};

if (require.main === module) {
  try {
    const options = parseArguments(process.argv.slice(2));
    if (options.report) {
      const report = path.resolve(options.report);
      const parent = fs.realpathSync(path.dirname(report));
      if (inside(fs.realpathSync(options.source), parent) ||
          inside(fs.realpathSync(options.bundle), parent) || stat(report)) {
        throw new Error('Report must be a new file outside the source checkout and staged app');
      }
      options.report = path.join(parent, path.basename(report));
    }
    const result = adaptPackage(options);
    const report = `${JSON.stringify(result, null, 2)}\n`;
    if (options.report) fs.writeFileSync(options.report, report, {flag: 'wx'});
    process.stdout.write(report);
  } catch (error) {
    process.stderr.write(`${error.stack || error.message}\n`);
    process.exitCode = 1;
  }
}
