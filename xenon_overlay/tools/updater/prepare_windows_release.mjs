#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Prepare Windows JSON + ZIP releases without executing installer code.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {buildPackage, findExecutable, validateVersion} from './make_directory_diff.js';

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const MAX_FILES = 100000;
const MAX_EXPANDED = 16 * 1024 ** 3;
const MAX_INPUT = 8 * 1024 ** 3;
const EXE = 'xlb153.exe';
const DLL = 'xlbrowser.dll';

function requireThat(condition, message) {
  if (!condition) throw new Error(message);
}

export function fileHash(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const hash = crypto.createHash('sha256');
    const buffer = Buffer.alloc(1024 * 1024);
    let count;
    while ((count = fs.readSync(fd, buffer, 0, buffer.length, null))) hash.update(buffer.subarray(0, count));
    return hash.digest('hex');
  } finally { fs.closeSync(fd); }
}

export function safeRelative(input) {
  const value = input.replaceAll('\\', '/').replace(/\/$/, '');
  requireThat(value && !value.startsWith('/'), 'Unsafe archive path');
  for (const part of value.split('/')) {
    requireThat(part && part !== '.' && part !== '..' && !/[\x00-\x1f\x7f\\/:<>"|?*]/u.test(part) &&
      !/[. ]$/.test(part) && !/^(con|prn|aux|nul|clock\$|conin\$|conout\$|com[1-9¹²³]|lpt[1-9¹²³])(?:\.|$)/i.test(part),
    `Unsafe Windows payload path: ${input}`);
  }
  return value;
}

export function parseListing(output) {
  const text = output.replaceAll('\r\n', '\n');
  const separator = text.indexOf('\n----------\n');
  requireThat(separator >= 0, '7-Zip did not provide an archive member list');
  const entries = new Map();
  const folded = new Set();
  let record = {}, total = 0;
  const finish = () => {
    if (!Object.keys(record).length) return;
    const name = safeRelative(record.Path || '');
    const key = name.toUpperCase();
    requireThat(!folded.has(key), 'Archive paths collide on Windows');
    requireThat(entries.size < MAX_FILES, 'Archive contains too many entries');
    for (const type of ['Symbolic Link', 'Hard Link', 'Reparse Point', 'Alternate Stream']) {
      requireThat(!record[type], 'Archive links and alternate streams are forbidden');
    }
    const attributes = record.Attributes || '';
    requireThat(!attributes.split(/\s+/).some(token => /^[lbcps]/.test(token)), 'Archive special files are forbidden');
    requireThat((record.Encrypted || '-') === '-', 'Encrypted archives are unsupported');
    const sizeText = record.Size || '0';
    requireThat(/^\d+$/.test(sizeText), 'Invalid archive member size');
    const size = Number(sizeText);
    requireThat(Number.isSafeInteger(size) && size <= MAX_EXPANDED, 'Archive member exceeds expanded size limit');
    const directory = record.Folder === '+' || attributes.split(' ')[0].includes('D');
    requireThat(!directory || size === 0, 'Archive directory has data');
    total += size;
    requireThat(total <= MAX_EXPANDED, 'Archive exceeds expanded size limit');
    entries.set(name, {size, directory});
    folded.add(key);
    record = {};
  };
  for (const line of text.slice(separator + 12).split('\n')) {
    if (!line || /^Warnings: \d+$/.test(line)) { finish(); continue; }
    const at = line.indexOf(' = ');
    requireThat(at > 0, 'Invalid 7-Zip member listing');
    const key = line.slice(0, at);
    requireThat(!Object.hasOwn(record, key), 'Ambiguous 7-Zip member listing');
    record[key] = line.slice(at + 3);
  }
  finish();
  requireThat(entries.size, 'Archive has no members');
  const files = new Set([...entries].filter(([, item]) => !item.directory).map(([name]) => name.toUpperCase()));
  const pathSpellings = new Map();
  for (const name of entries.keys()) {
    const parts = name.split('/');
    for (let i = 1; i <= parts.length; ++i) {
      const parent = parts.slice(0, i).join('/');
      const key = parent.toUpperCase();
      requireThat(!pathSpellings.has(key) || pathSpellings.get(key) === parent, 'Archive directory casing collides');
      pathSpellings.set(key, parent);
      if (i < parts.length) requireThat(!files.has(key), 'Archive uses a file as a directory');
    }
  }
  return entries;
}

export function inventory(root) {
  const files = new Map(), directories = new Set(), seen = new Set();
  let total = 0, count = 0;
  const rootStat = fs.lstatSync(root);
  requireThat(rootStat.isDirectory() && !rootStat.isSymbolicLink(), 'Application root must be a real directory');
  const walk = (dir, prefix) => {
    for (const entry of fs.readdirSync(dir).sort()) {
      const name = safeRelative(prefix ? `${prefix}/${entry}` : entry);
      requireThat(!entry.includes('\\'), 'Windows payload uses a literal backslash');
      requireThat(!seen.has(name.toUpperCase()), 'Application paths collide on Windows');
      seen.add(name.toUpperCase());
      requireThat(++count <= MAX_FILES, 'Application contains too many entries');
      const file = path.join(dir, entry);
      const stat = fs.lstatSync(file);
      requireThat(!stat.isSymbolicLink(), 'Application links are forbidden');
      if (stat.isDirectory()) { directories.add(name); walk(file, name); }
      else {
        requireThat(stat.isFile() && stat.nlink === 1, 'Application must contain regular single-link files');
        total += stat.size;
        requireThat(Number.isSafeInteger(stat.size) && total <= MAX_EXPANDED, 'Application exceeds expanded size limit');
        files.set(name, stat.size);
      }
    }
  };
  walk(root, '');
  return {files, directories};
}

export function peMetadata(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const size = fs.fstatSync(fd).size;
    const read = (offset, count) => {
      requireThat(offset >= 0 && count >= 0 && offset <= size && count <= size - offset, 'Truncated PE image');
      const data = Buffer.alloc(count);
      requireThat(fs.readSync(fd, data, 0, count, offset) === count, 'Truncated PE image');
      return data;
    };
    const header = read(0, 64);
    requireThat(header.toString('ascii', 0, 2) === 'MZ', 'Not a PE image');
    const offset = header.readUInt32LE(60), pe = read(offset, 24);
    requireThat(pe.toString('ascii', 0, 4) === 'PE\0\0', 'Invalid PE signature');
    const machine = pe.readUInt16LE(4), count = pe.readUInt16LE(6);
    const arch = new Map([[0x14c, 'x86'], [0x8664, 'x86_64'], [0xaa64, 'arm64']]).get(machine);
    requireThat(arch && count > 0 && count <= 96, 'Unsupported PE architecture or section count');
    const characteristics = pe.readUInt16LE(22), optionalSize = pe.readUInt16LE(20);
    const optional = read(offset + 24, optionalSize);
    requireThat(optional.length >= 2, 'Invalid PE optional header');
    const magic = optional.readUInt16LE(0), dataOffset = magic === 0x10b ? 96 : magic === 0x20b ? 112 : 0;
    requireThat(dataOffset && optional.length >= dataOffset + 24 && (arch !== 'x86') === (magic === 0x20b), 'Invalid PE optional header');
    requireThat(optional.readUInt32LE(dataOffset - 4) >= 3, 'PE has no version resource');
    const resourceRva = optional.readUInt32LE(dataOffset + 16), resourceSize = optional.readUInt32LE(dataOffset + 20);
    const sections = [];
    for (let i = 0; i < count; ++i) {
      const section = read(offset + 24 + optionalSize + i * 40, 40);
      sections.push({address: section.readUInt32LE(12), size: section.readUInt32LE(16), offset: section.readUInt32LE(20)});
    }
    const fromRva = (rva, bytes) => {
      for (const section of sections) {
        if (rva >= section.address && rva - section.address <= section.size && bytes <= section.size - (rva - section.address)) {
          return read(section.offset + rva - section.address, bytes);
        }
      }
      throw new Error('PE resource lies outside its section');
    };
    requireThat(resourceRva && resourceSize > 0 && resourceSize <= 64 * 1024 ** 2, 'Missing or oversized PE resource section');
    const resources = fromRva(resourceRva, resourceSize);
    const resourceRead = (at, bytes) => {
      requireThat(at >= 0 && at <= resources.length && bytes <= resources.length - at, 'Invalid PE resource directory');
      return resources.subarray(at, at + bytes);
    };
    const entries = at => {
      const directory = resourceRead(at, 16);
      const length = directory.readUInt16LE(12) + directory.readUInt16LE(14);
      requireThat(length <= 4096, 'Too many PE resources');
      return Array.from({length}, (_, i) => {
        const item = resourceRead(at + 16 + i * 8, 8);
        return [item.readUInt32LE(0), item.readUInt32LE(4)];
      });
    };
    const versions = new Set();
    for (const [kind, typeEntry] of entries(0)) {
      if (kind !== 16) continue;
      requireThat(typeEntry & 0x80000000, 'Invalid PE version directory');
      for (const [, nameEntry] of entries(typeEntry & 0x7fffffff)) {
        requireThat(nameEntry & 0x80000000, 'Invalid PE version name');
        for (const [, languageEntry] of entries(nameEntry & 0x7fffffff)) {
          requireThat(!(languageEntry & 0x80000000), 'Invalid PE version language');
          const resource = resourceRead(languageEntry, 16);
          const bytes = resource.readUInt32LE(4);
          requireThat(bytes >= 6 && bytes <= 1024 ** 2, 'Invalid PE version resource size');
          const data = fromRva(resource.readUInt32LE(0), bytes);
          const key = Buffer.from('VS_VERSION_INFO\0', 'utf16le');
          const valueOffset = (6 + key.length + 3) & ~3;
          requireThat(data.length >= valueOffset + 52 && data.readUInt16LE(0) <= data.length &&
            data.readUInt16LE(0) >= valueOffset + 52 && data.readUInt16LE(2) === 52 &&
            data.subarray(6, 6 + key.length).equals(key) && data.readUInt32LE(valueOffset) === 0xfeef04bd,
          'Invalid fixed PE version');
          // Chromium FILEVERSION can differ from the branded PRODUCTVERSION.
          const high = data.readUInt32LE(valueOffset + 16), low = data.readUInt32LE(valueOffset + 20);
          versions.add(`${high >>> 16}.${high & 65535}.${low >>> 16}.${low & 65535}`);
        }
      }
    }
    requireThat(versions.size === 1, 'PE must have one consistent product version');
    return {arch, version: [...versions][0], isDll: !!(characteristics & 0x2000), executableImage: !!(characteristics & 2)};
  } finally { fs.closeSync(fd); }
}

export function validateApplication(root, requested = {}) {
  const entries = inventory(root);
  const versions = [...entries.directories].filter(name => !name.includes('/') && /^\d+(?:\.\d+)+$/.test(name));
  requireThat(versions.length === 1, 'Application must contain exactly one version directory');
  const version = versions[0];
  validateVersion(version);
  requireThat(!requested.version || requested.version === version, 'Requested version differs from application version');
  let arch;
  for (const [relative, isDll] of [[EXE, false], [`${version}/${DLL}`, true], [`${version}/Installer/setup.exe`, false]]) {
    requireThat(entries.files.has(relative), `Missing required application file: ${relative}`);
    const meta = peMetadata(path.join(root, relative));
    requireThat(meta.version === version && meta.isDll === isDll && meta.executableImage, `PE product version or type differs: ${relative}`);
    requireThat(!arch || arch === meta.arch, 'Application PE architectures differ');
    arch = meta.arch;
  }
  requireThat(!requested.arch || requested.arch === arch, 'Requested architecture differs from application PE architecture');
  return {version, arch, ...entries};
}

function runCommand(tool, args, options = {}) {
  const result = spawnSync(tool, args, {encoding: 'utf8', maxBuffer: 64 * 1024 ** 2, windowsHide: true, ...options});
  if (result.error || result.status !== 0) {
    const error = new Error(`${path.basename(tool)} failed: ${result.error?.message || result.signal || result.status}\n${result.stderr || result.stdout || ''}`);
    error.toolFailure = true;
    throw error;
  }
  return result.stdout;
}

export function findSevenZip(override, platform = process.platform) {
  const found = findExecutable(override ? [override] : [
    ...(platform === 'win32' ? [path.join(projectRoot, 'third_party/lzma_sdk/bin/win64/7za.exe')] : []), '7zz', '7z', '7za'], platform);
  requireThat(found, '7-Zip is required; configure --7zip with a native executable');
  return found;
}

function extract(source, destination, sevenZip, archiveType) {
  const flags = archiveType ? [`-t${archiveType}`] : [];
  const expected = parseListing(runCommand(sevenZip, ['l', '-slt', '-sccUTF-8', ...flags, '--', source]));
  fs.mkdirSync(destination);
  runCommand(sevenZip, ['x', '-y', '-sccUTF-8', `-o${destination}`, ...flags, '--', source]);
  const actual = inventory(destination);
  const files = new Map([...expected].filter(([, item]) => !item.directory).map(([name, item]) => [name, item.size]));
  requireThat(actual.files.size === files.size && [...files].every(([name, size]) => actual.files.get(name) === size), 'Extracted contents differ from inspected archive');
  const directories = new Set([...expected].filter(([, item]) => item.directory).map(([name]) => name));
  for (const name of expected.keys()) {
    const parts = name.split('/');
    for (let i = 1; i < parts.length; ++i) directories.add(parts.slice(0, i).join('/'));
  }
  requireThat(actual.directories.size === directories.size && [...directories].every(name => actual.directories.has(name)), 'Unexpected extracted directory');
  return destination;
}

function compareVersions(a, b) {
  const left = validateVersion(a), right = validateVersion(b);
  for (let i = 0; i < 4; ++i) if (left[i] !== right[i]) return Math.sign(left[i] - right[i]);
  return 0;
}

function fingerprint(root, files) {
  const hash = crypto.createHash('sha256');
  // A stable content identity independent of ZIP timestamps and host file modes.
  for (const name of [...files.keys()].sort()) {
    hash.update(JSON.stringify([name, files.get(name), fileHash(path.join(root, name))]) + '\n');
  }
  return hash.digest('hex');
}

export function prepareRelease(options) {
  const input = fs.realpathSync(options.input), sourceStat = fs.lstatSync(options.input);
  const extension = path.extname(input).toLowerCase();
  requireThat(sourceStat.isFile() && !sourceStat.isSymbolicLink() && sourceStat.nlink === 1 &&
    sourceStat.size > 0 && sourceStat.size <= MAX_INPUT, 'Input must be a nonempty regular EXE or ZIP up to 8 GiB');
  requireThat(['.exe', '.zip'].includes(extension), 'Windows publishing accepts EXE or ZIP packages');
  const probe = fs.openSync(input, 'r');
  try {
    const magic = Buffer.alloc(4);
    fs.readSync(probe, magic, 0, 4, 0);
    requireThat(extension === '.exe' ? magic.subarray(0, 2).equals(Buffer.from('MZ')) :
      magic.equals(Buffer.from([0x50, 0x4b, 3, 4])), 'Input bytes do not match the EXE or ZIP format');
  } finally { fs.closeSync(probe); }
  if (options.version) validateVersion(options.version);
  requireThat(!options.arch || ['x86', 'x86_64', 'arm64'].includes(options.arch), 'Unsupported Windows architecture');
  const output = path.resolve(options.outDir);
  requireThat(!fs.existsSync(output), 'Output directory already exists');
  fs.mkdirSync(path.dirname(output), {recursive: true});
  const parent = fs.realpathSync(path.dirname(output));
  const finalOutput = path.join(parent, path.basename(output));
  const sevenZip = findSevenZip(options.sevenZip);
  // A missing or broken delta tool must not prevent the full ZIP release.
  const zucchini = options.zucchini || findExecutable(
    ['Release_64', 'release_arm64', 'Release', 'Default'].map(dir => path.join(projectRoot, 'out', dir, process.platform === 'win32' ? 'zucchini.exe' : 'zucchini')).concat('zucchini'));
  const stage = fs.mkdtempSync(path.join(parent, '.xenon-windows-release-'));
  try {
    const snapshot = path.join(stage, `input${extension}`);
    fs.copyFileSync(input, snapshot, fs.constants.COPYFILE_EXCL);
    requireThat(fs.statSync(snapshot).size === sourceStat.size, 'Input changed while copying');
    const browserRoot = root => {
      const nested = path.join(root, 'Chrome-bin');
      return fs.existsSync(path.join(nested, EXE)) ? nested : fs.existsSync(path.join(root, EXE)) ? root : null;
    };
    // Full 7-Zip identifies mini-installers as PE images. Select the embedded
    // archive explicitly so Linux 7zz and Windows 7za see the same payload.
    let first;
    if (extension === '.exe') {
      try { first = extract(snapshot, path.join(stage, 'outer'), sevenZip, 'Cab'); }
      catch (error) {
        if (!error.toolFailure) throw error;
        first = extract(snapshot, path.join(stage, 'sevenzip'), sevenZip, '7z');
      }
    } else first = extract(snapshot, path.join(stage, 'outer'), sevenZip, 'zip');
    const roots = [first];
    let payload = browserRoot(first), embedded = path.join(first, 'chrome.7z');
    if (!payload && !fs.existsSync(embedded) && extension === '.exe') {
      const second = extract(snapshot, path.join(stage, 'resource'), sevenZip, '7z');
      roots.push(second);
      payload = browserRoot(second);
      embedded = path.join(second, 'chrome.7z');
    }
    if (!payload && fs.existsSync(embedded) && extension === '.exe') {
      const inner = extract(embedded, path.join(stage, 'browser'), sevenZip);
      roots.push(inner);
      payload = browserRoot(inner);
    }
    requireThat(payload, 'Package does not contain a complete Chromium application tree');
    const product = peMetadata(path.join(payload, EXE));
    const setup = path.join(payload, product.version, 'Installer/setup.exe');
    if (!fs.existsSync(setup)) {
      const candidates = roots.map(root => path.join(root, 'setup.exe')).filter(file => fs.existsSync(file));
      requireThat(candidates.length && new Set(candidates.map(fileHash)).size === 1, 'Package has missing or ambiguous setup.exe');
      fs.mkdirSync(path.dirname(setup), {recursive: true});
      fs.copyFileSync(candidates[0], setup);
    }
    validateApplication(payload, options);
    const releaseDir = path.join(stage, 'release');
    fs.mkdirSync(releaseDir);
    const application = path.join(releaseDir, 'application');
    fs.renameSync(payload, application);
    const info = validateApplication(application, options);
    const fullPath = path.join(releaseDir, 'package.zip');
    runCommand(sevenZip, ['a', '-tzip', '-mx=6', fullPath, '.'], {cwd: application});
    runCommand(sevenZip, ['t', '--', fullPath]);
    const packed = parseListing(runCommand(sevenZip, ['l', '-slt', '-sccUTF-8', '--', fullPath]));
    const packedFiles = new Map([...packed].filter(([, item]) => !item.directory));
    requireThat(packedFiles.size === info.files.size && [...info.files].every(([name, size]) => packedFiles.get(name)?.size === size), 'Full ZIP does not match application inventory');
    const result = {version: info.version, arch: info.arch,
      tree_sha256: fingerprint(application, info.files),
      full: {file: 'package.zip', size: fs.statSync(fullPath).size, sha256: fileHash(fullPath)},
      differential: [], warnings: []};
    let baselines = options.baselines || [];
    if (typeof baselines === 'string') {
      requireThat(fs.statSync(baselines).size <= 16 * 1024 ** 2, 'Baseline metadata exceeds 16 MiB');
      baselines = JSON.parse(fs.readFileSync(baselines, 'utf8'));
    }
    requireThat(Array.isArray(baselines) && baselines.length <= 4096, 'Baselines must be a JSON array with at most 4096 entries');
    const seen = new Set();
    baselines = baselines.filter(item => {
      requireThat(item && typeof item.tree === 'string' && path.isAbsolute(item.tree), 'Baseline tree must be an absolute path');
      validateVersion(item.version);
      if (item.arch !== info.arch || compareVersions(item.version, info.version) >= 0) return false;
      requireThat(!seen.has(item.version), 'Duplicate baseline version');
      seen.add(item.version);
      return true;
    }).sort((a, b) => compareVersions(b.version, a.version)).slice(0, 32);
    for (const baseline of baselines) {
      const file = `patch-${baseline.version}.zip`, archive = path.join(releaseDir, file);
      try {
        const baselineInfo = validateApplication(baseline.tree, baseline);
        if (baseline.tree_sha256 !== undefined) {
          requireThat(/^[a-f0-9]{64}$/.test(baseline.tree_sha256) &&
            fingerprint(baseline.tree, baselineInfo.files) === baseline.tree_sha256,
          'Baseline application content differs from its published identity');
        }
        const patch = buildPackage({old: baseline.tree, new: application, out: archive, sevenZip, zucchini});
        requireThat(patch.manifest.base_version === baseline.version && patch.manifest.target_version === info.version, 'Generated patch version mismatch');
        if (patch.size < result.full.size) result.differential.push({from_version: baseline.version, file,
          size: patch.size, sha256: patch.sha256.toLowerCase(), deletions: patch.manifest.deletions});
        else {
          fs.unlinkSync(archive);
          result.warnings.push(`${baseline.version}: 差分包不小于完整 ZIP，使用完整包。`);
        }
      } catch (error) {
        if (fs.existsSync(archive)) fs.unlinkSync(archive);
        result.warnings.push(`${baseline.version}: 差分生成失败，使用完整包。${error.message.slice(0, 4000)}`);
      }
    }
    fs.writeFileSync(path.join(releaseDir, 'release.json'), JSON.stringify(result, null, 2) + '\n');
    requireThat(!fs.existsSync(finalOutput), 'Output directory appeared during preparation');
    fs.renameSync(releaseDir, finalOutput);
    return result;
  } finally { fs.rmSync(stage, {recursive: true, force: true}); }
}

function parseArgs(args) {
  const options = {};
  const fields = {'--input': 'input', '--out-dir': 'outDir', '--version': 'version', '--arch': 'arch',
    '--7zip': 'sevenZip', '--zucchini': 'zucchini', '--baselines': 'baselines'};
  for (let i = 0; i < args.length; ++i) {
    requireThat(fields[args[i]] && args[i + 1] && !args[i + 1].startsWith('--'), `Unknown or incomplete argument: ${args[i]}`);
    requireThat(options[fields[args[i]]] === undefined, `Duplicate argument: ${args[i]}`);
    options[fields[args[i]]] = args[++i];
  }
  requireThat(options.input && options.outDir, 'Usage: prepare_windows_release.mjs --input EXE_OR_ZIP --out-dir NEW_DIR [--7zip TOOL] [--zucchini TOOL] [--baselines JSON]');
  return options;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { console.log(JSON.stringify(prepareRelease(parseArgs(process.argv.slice(2))), null, 2)); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}
