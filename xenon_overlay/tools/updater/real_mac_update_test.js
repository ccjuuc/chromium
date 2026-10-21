#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Runs a real browser. Diff mode verifies native replacement and relaunch;
// full/fallback verify only PKG download and handoff to the system Installer.
// --app MUST be an expendable, independent installation.
// No packages are generated or modified, and no existing profile is reused.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {execFile, spawn} from 'node:child_process';
import {promisify} from 'node:util';
import {fileURLToPath} from 'node:url';

const runFile = promisify(execFile);
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const isoNow = () => new Date().toISOString();
// os.homedir() may follow HOME. Authorization uses the account database and
// canonical home instead, so environment overrides cannot authorize another user.
const currentUserHome = fs.realpathSync(os.userInfo().homedir);
const userApplications = path.join(currentUserHome, 'Applications');
const userInstalledApp = path.join(userApplications, 'xl153.app');
const installedAppPaths = new Set(['/Applications/xl153.app', userInstalledApp]);
const launchServicesTool = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister';
let interruptedError;
const usage = `Usage: node real_mac_update_test.js \\
  --app /private/tmp/update-test/xl153.app --expected-version 1.0.0.2 \\
  --target-app /private/tmp/releases/1.0.0.2/xl153.app \\
  --data-dir /private/tmp/update-test/profile-diff --mode diff \\
  --artifacts-dir /private/tmp/update-test/evidence-diff \\
  --patch /private/tmp/releases/patch.zip --package /private/tmp/releases/package.pkg

Required: --app --expected-version --data-dir --mode diff|full|fallback --artifacts-dir
Optional: --target-app (diff only: compare every file/hash/mode/symlink after restart)
          --patch (defaults to artifacts-dir/patch.zip; required for diff/fallback)
          --package (defaults to artifacts-dir/package.pkg; required for full/fallback)
          --timeout-ms (per preparation/handoff/restart phase, default 600000)
          --browser-arg VALUE (repeatable; e.g. --browser-arg=--some-switch)
          --allow-local-adhoc (clear quarantine only on this test copy and its
                               verified staged app; NOT release-trust validation)
          --installed-test-app PATH (explicitly authorize /Applications/xl153.app
                               or the current user's ~/Applications/xl153.app)
          --help | --self-test (does not launch a browser)

The test refuses a nonempty profile directory. The app must be an independent
copy, distinct from --target-app. HTTP and randomly assigned CDP ports bind to
127.0.0.1. Diff success requires installation and restart. Full/fallback success
means PKG HANDOFF ONLY: installationVerified=false, awaiting user installation.
Those modes open the system Installer but never submit authorization or install.
Complete root installation, administrator approval and app startup need a separate
user test. The system Installer is left open; only this test browser is closed.`;

export function parseArgs(args) {
  const result = {mode: 'diff', timeoutMs: 600000, browserArgs: []};
  const keys = {'--app':'app', '--expected-version':'expectedVersion',
    '--target-app':'targetApp', '--data-dir':'dataDir', '--mode':'mode',
    '--artifacts-dir':'artifactsDir', '--patch':'patch', '--package':'package',
    '--timeout-ms':'timeoutMs', '--installed-test-app':'installedTestApp'};
  for (let i = 0; i < args.length; ++i) {
    const split = args[i].indexOf('=');
    const name = split < 0 ? args[i] : args[i].slice(0, split);
    if (name === '--help' || name === '--self-test') {
      result[name.slice(2)] = true;
      continue;
    }
    if (name === '--allow-local-adhoc') {
      if (split >= 0) throw new Error('--allow-local-adhoc does not take a value');
      result.allowLocalAdhoc = true;
      continue;
    }
    const value = split < 0 ? args[++i] : args[i].slice(split + 1);
    if (!value || (!keys[name] && name !== '--browser-arg')) {
      throw new Error(`Unknown or incomplete argument: ${name}`);
    }
    if (name === '--browser-arg') result.browserArgs.push(value);
    else result[keys[name]] = name === '--timeout-ms' ? Number(value) : value;
  }
  if (result.help || result['self-test']) return result;
  for (const key of ['app', 'expectedVersion', 'dataDir', 'artifactsDir']) {
    if (!result[key]) throw new Error(`Missing --${key.replace(/[A-Z]/g, c => '-' + c.toLowerCase())}`);
  }
  if (!['diff', 'full', 'fallback'].includes(result.mode)) throw new Error('Invalid --mode');
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(result.expectedVersion)) throw new Error('Expected version must have four numeric components');
  if (!Number.isSafeInteger(result.timeoutMs) || result.timeoutMs <= 0) throw new Error('Invalid --timeout-ms');
  for (const key of ['app', 'dataDir', 'artifactsDir', 'targetApp', 'patch', 'package']) {
    if (result[key]) result[key] = path.resolve(result[key].startsWith('~/')
      ? path.join(currentUserHome, result[key].slice(2)) : result[key]);
  }
  result.patch ??= path.join(result.artifactsDir, 'patch.zip');
  result.package ??= path.join(result.artifactsDir, 'package.pkg');
  if (!result.package.toLowerCase().endsWith('.pkg')) {
    throw new Error('--package must be a macOS .pkg installer; full ZIP updates are not supported');
  }
  if (result.mode !== 'diff' && result.targetApp) {
    throw new Error('--target-app is only supported in diff mode; PKG handoff does not verify installation');
  }
  if (result.app === result.targetApp) throw new Error('--app and --target-app must be different bundles');
  if (result.installedTestApp === '~/Applications/xl153.app') {
    result.installedTestApp = userInstalledApp;
  }
  if (result.installedTestApp && (!installedAppPaths.has(result.installedTestApp) ||
      result.app !== result.installedTestApp)) {
    throw new Error('--installed-test-app must name /Applications/xl153.app or the current user\'s canonical ~/Applications/xl153.app and exactly match --app');
  }
  for (const arg of result.browserArgs) {
    if (/^--(?:user-data-dir|remote-debugging-|log-file|enable-automation)/.test(arg)) {
      throw new Error(`The test owns this browser switch: ${arg}`);
    }
  }
  return result;
}

async function sha256(file) {
  const hash = crypto.createHash('sha256');
  for await (const bytes of fs.createReadStream(file)) hash.update(bytes);
  return hash.digest('hex');
}

async function bundleInfo(app) {
  const plist = path.join(app, 'Contents', 'Info.plist');
  const read = async key => (await runFile('/usr/bin/plutil',
      ['-extract', key, 'raw', '-o', '-', plist])).stdout.trim();
  return {app, version: await read('CFBundleShortVersionString'),
    executable: await read('CFBundleExecutable')};
}

function isWithin(root, filename) {
  const relative = path.relative(root, filename);
  return relative !== '' && relative !== '..' &&
      !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
}

function isAuthorizedInstallation(filename, canonical, installedTestApp) {
  return installedAppPaths.has(installedTestApp) && canonical === installedTestApp &&
      path.resolve(filename) === installedTestApp;
}

function isAuthorizedPreparation(canonical, installedTestApp, preparedTestApp) {
  return installedAppPaths.has(installedTestApp) &&
      canonical === preparedTestApp &&
      path.dirname(path.dirname(canonical)) === path.dirname(installedTestApp) &&
      path.basename(canonical) === 'xl153.app' &&
      /^\.xenon-update-[A-Za-z0-9]+$/.test(path.basename(path.dirname(canonical)));
}

async function validateTestBundle(filename, allowedRoot, installedTestApp, preparedTestApp) {
  const canonical = await fsp.realpath(filename);
  const root = await fsp.realpath(allowedRoot);
  const authorizedInstallation = isAuthorizedInstallation(filename, canonical, installedTestApp);
  const authorizedPreparation = isAuthorizedPreparation(canonical, installedTestApp, preparedTestApp);
  if ((!isWithin(root, canonical) && !authorizedInstallation) || !canonical.endsWith('.app') ||
      !(await fsp.lstat(filename)).isDirectory()) {
    throw new Error(`Bundle is not a real application directory inside this test: ${filename}`);
  }
  for (const protectedRoot of ['/Applications', '/System', '/Library', userApplications]) {
    if (!authorizedInstallation && !authorizedPreparation &&
        (canonical === protectedRoot || isWithin(protectedRoot, canonical))) {
      throw new Error(`Refusing quarantine changes to an installed/system application: ${canonical}`);
    }
  }
  async function checkEntries(directory) {
    for (const entry of await fsp.readdir(directory, {withFileTypes:true})) {
      const item = path.join(directory, entry.name);
      if (!entry.isDirectory() && (await fsp.lstat(item)).nlink !== 1) {
        throw new Error(`Refusing quarantine changes to a hard-linked entry: ${item}`);
      }
      if (entry.isSymbolicLink()) {
        const resolved = await fsp.realpath(item);
        if (!isWithin(canonical, resolved)) throw new Error(`Bundle symlink escapes this application: ${item}`);
      } else if (entry.isDirectory()) await checkEntries(item);
      else if (!entry.isFile()) throw new Error(`Unsupported application entry: ${item}`);
    }
  }
  await checkEntries(canonical);
  return canonical;
}

async function clearLocalQuarantine(filename, allowedRoot, phase, options, record,
    details = {}, preparedTestApp, mutationGuard = () => {}) {
  const app = await validateTestBundle(filename, allowedRoot,
    phase === 'installed' || phase === 'prepared' ? options.installedTestApp : undefined,
    phase === 'prepared' ? preparedTestApp : undefined);
  const display = await runFile('/usr/bin/codesign', ['--display', '--verbose=4', app]);
  if (!/^Signature=adhoc$/m.test(display.stderr)) throw new Error(`Expected a local ad-hoc signature: ${app}`);
  await runFile('/usr/bin/codesign', ['--verify', '--deep', '--strict', app]);
  const xattr = args => runFile('/usr/bin/xattr', args, {maxBuffer:16 * 1024 * 1024});
  const quarantineLines = value => value.split('\n').filter(line => line.includes('com.apple.quarantine'));
  const before = await xattr(['-r', '-s', app]);
  await fsp.writeFile(path.join(options.artifactsDir, `${phase}-xattrs-before.txt`), before.stdout);
  const args = ['-r', '-s', '-d', 'com.apple.quarantine', app];
  const executed = quarantineLines(before.stdout).length > 0;
  const permissions = [];
  mutationGuard();
  if (executed) {
    try {
      for (const line of quarantineLines(before.stdout)) {
        const suffix = ': com.apple.quarantine';
        if (!line.endsWith(suffix)) throw new Error(`Unexpected xattr output: ${line}`);
        const item = line.slice(0, -suffix.length);
        if (item !== app && !isWithin(app, item)) throw new Error(`Quarantine path escapes verified app: ${item}`);
        const stat = await fsp.lstat(item);
        if ((stat.isFile() || stat.isDirectory()) && !(stat.mode & 0o200)) {
          if (stat.uid !== process.getuid()) throw new Error(`Read-only quarantine entry is not owned by this test user: ${item}`);
          const mode = stat.mode & 0o7777;
          mutationGuard();
          await fsp.chmod(item, mode | 0o200);
          permissions.push({path:item, mode});
          record('local-adhoc-temporary-write-permission', {path:item, originalMode:mode, temporaryMode:mode | 0o200});
        }
      }
      mutationGuard();
      await xattr(args);
      mutationGuard();
    } finally {
      let restoreError;
      for (const item of permissions.reverse()) {
        try {
          await fsp.chmod(item.path, item.mode);
          record('local-adhoc-permission-restored', item);
        } catch (error) { restoreError ??= error; }
      }
      if (restoreError) throw restoreError;
    }
  }
  const after = executed ? await xattr(['-r', '-s', app]) : before;
  await fsp.writeFile(path.join(options.artifactsDir, `${phase}-xattrs-after.txt`), after.stdout);
  record('local-adhoc-quarantine-cleared', {phase, app, command:'/usr/bin/xattr', args, executed,
    ...details, beforeQuarantine:quarantineLines(before.stdout), afterQuarantine:quarantineLines(after.stdout),
    limitation:'Local ad-hoc quarantine check only; does not validate the official release trust chain.'});
  if (quarantineLines(after.stdout).length) throw new Error(`Quarantine remains on ${app}`);
}

async function listPreparationDirectories(parent) {
  return (await fsp.readdir(parent, {withFileTypes:true}))
    .filter(entry => entry.isDirectory() && /^\.xenon-update-[A-Za-z0-9]+$/.test(entry.name))
    .map(entry => entry.name).sort();
}

function selectNewPreparation(current, previous) {
  const fresh = current.filter(name => !previous.includes(name));
  if (fresh.length > 1) throw new Error('More than one new native preparation directory appeared for this test');
  return fresh[0];
}

function matchHelperCommand(output, executable, originalPID, prepared, target) {
  const prefix = `${executable} ${originalPID} 3 ${prepared} ${target} 1`;
  for (const line of output.split('\n')) {
    const parsed = line.match(/^\s*(\d+)\s+(.+)$/);
    if (parsed && (parsed[2] === prefix || parsed[2].startsWith(prefix + ' '))) {
      return Number(parsed[1]);
    }
  }
  return null;
}

async function findPreparationHelper(originalPID, prepared, options) {
  let output;
  try { output = await runFile('/usr/bin/pgrep', ['-P', String(originalPID)]); }
  catch (error) { if (error.code === 1) return null; throw error; }
  const pids = output.stdout.trim().split(/\s+/);
  if (!pids.every(pid => /^\d+$/.test(pid))) throw new Error('Invalid child process list');
  const commands = await runFile('/bin/ps', ['-ww', '-p', pids.join(','), '-o', 'pid=,command=']);
  return matchHelperCommand(commands.stdout,
    path.join(options.app, 'Contents', 'Helpers', 'xenon_update_helper'),
    originalPID, prepared, options.app);
}

async function processState(pid) {
  try { return (await runFile('/bin/ps', ['-p', String(pid), '-o', 'state='])).stdout.trim(); }
  catch (error) { if (error.code === 1) return null; throw error; }
}

async function withStoppedProcess(pid, record, action) {
  let stopRequested = false;
  try {
    process.kill(pid, 'SIGSTOP');
    stopRequested = true;
    record('native-helper-stop-requested', {pid});
    await waitFor(async () => {
      const state = await processState(pid);
      if (!state) throw new Error(`Native helper ${pid} exited before pause confirmation`);
      return state.includes('T');
    }, 5000, `verified native helper ${pid} stopped`, false);
    record('native-helper-stopped', {pid,
      limitation:'Local ad-hoc test pauses this helper during quarantine handling; production scheduling is not reproduced during this interval.'});
    return await action();
  } finally {
    if (stopRequested) {
      try {
        process.kill(pid, 'SIGCONT');
        record('native-helper-resume-requested', {pid});
      } catch (error) {
        if (error.code !== 'ESRCH') throw error;
        record('native-helper-already-exited', {pid});
      }
      // Do not use waitFor here: an interrupted test still must resume its
      // helper, including when an exception occurred inside the paused action.
      const deadline = Date.now() + 5000;
      while ((await processState(pid))?.includes('T')) {
        if (Date.now() >= deadline) throw new Error(`Native helper ${pid} remained stopped after SIGCONT`);
        await pause(50);
      }
      record('native-helper-resumed', {pid});
    }
  }
}

async function clearPreparedCopy(options, initial, preparation, record) {
  const ensureNotCancelled = () => {
    if (preparation.cancelled) throw new Error('Native preparation observation was cancelled');
  };
  const ensureOldAlive = () => {
    ensureNotCancelled();
    if (!alive(initial.pid)) throw new Error('Original browser exited before its verified native preparation could be cleared');
  };
  const selected = await waitFor(async () => {
    ensureOldAlive();
    const name = selectNewPreparation(await listPreparationDirectories(preparation.parent), preparation.existing);
    if (!name) return null;
    const app = path.join(preparation.parent, name, path.basename(options.app));
    const helperPID = await findPreparationHelper(initial.pid, app, options);
    return helperPID ? {app, helperPID} : null;
  }, options.timeoutMs, 'this update helper and its completed native preparation', false);
  ensureOldAlive();
  // The helper is the only process allowed to exchange these bundles. Pause
  // this verified child before reading or mutating its paths, so browser exit
  // cannot swap them underneath codesign, xattr, or permission restoration.
  return withStoppedProcess(selected.helperPID, record, async () => {
    const app = await validateTestBundle(selected.app, preparation.parent,
      options.installedTestApp, selected.app);
    const info = await bundleInfo(app);
    if (info.version !== options.expectedVersion) throw new Error('Native preparation has the wrong target version');
    const actualTree = await snapshotTree(app);
    const differences = compareTrees(preparation.expectedTree, actualTree);
    await fsp.writeFile(path.join(options.artifactsDir, 'prepared-tree.json'), JSON.stringify(actualTree, null, 2));
    await fsp.writeFile(path.join(options.artifactsDir, 'prepared-tree-differences.json'), JSON.stringify(differences, null, 2));
    if (differences.length) throw new Error(`Native preparation differs from the verified ready app at ${differences.length} entries`);
    record('native-preparation-verified', {app, helperPID:selected.helperPID, originalPID:initial.pid,
      version:info.version, entries:Object.keys(actualTree).length, differences:0});
    await clearLocalQuarantine(app, preparation.parent, 'prepared', options, record,
      {helperPID:selected.helperPID, originalPID:initial.pid, version:info.version,
        treeEntries:Object.keys(actualTree).length, treeDifferences:0}, app, ensureNotCancelled);
    return {app, helperPID:selected.helperPID, version:info.version,
      originalAliveAtCompletion:alive(initial.pid), helperPausedForLocalAdhoc:true};
  });
}

async function locateStagedApp(temporaryRoot, options, staged) {
  if (options.mode !== 'diff' || staged.is_diff !== true || staged.is_installer_package) {
    throw new Error('Application staging is only supported for differential updates');
  }
  const root = await fsp.realpath(temporaryRoot);
  const attempt = 1;
  const stageNames = ['prepared'];
  const candidates = [];
  for (const entry of await fsp.readdir(root, {withFileTypes:true})) {
    if (!entry.isDirectory()) continue;
    const downloadDir = path.join(root, entry.name);
    for (const stageName of stageNames) {
      const candidate = path.join(downloadDir, stageName, path.basename(options.app));
      if (fs.existsSync(candidate)) candidates.push({app:candidate, downloadDir});
    }
  }
  if (candidates.length !== 1) {
    throw new Error(`Ready event has ${candidates.length} staged app candidates inside this run's private temporary directory: ${root}`);
  }
  const selected = candidates[0];
  selected.app = await validateTestBundle(selected.app, root);
  const info = await bundleInfo(selected.app);
  if (info.version !== options.expectedVersion || staged.target_version !== options.expectedVersion) {
    throw new Error(`Staged bundle version does not match this ready event: ${info.version}`);
  }
  const archive = path.join(selected.downloadDir, `${attempt}_update_patch.zip`);
  if (!(await fsp.lstat(archive)).isFile() || !isWithin(root, await fsp.realpath(archive))) {
    throw new Error('Downloaded archive is outside this test staging directory');
  }
  const actualHash = await sha256(archive);
  const packageInfo = staged.diff_package;
  const sourceHash = await sha256(options.patch);
  if (actualHash !== packageInfo?.sha256 || actualHash !== sourceHash) {
    throw new Error('Staged application download does not match this test feed/package SHA256');
  }
  return {...selected, archive, archiveSHA256:actualHash, version:info.version,
    binding:'Unique staged app under this process-private MAC_CHROMIUM_TMPDIR, after native ready event, with matching version and downloaded package SHA256'};
}

async function locateDownloadedInstaller(temporaryRoot, options, staged) {
  if (options.mode === 'diff' || staged.is_diff !== false ||
      staged.is_installer_package !== true || staged.target_version !== options.expectedVersion) {
    throw new Error('Ready event does not identify the expected installer package');
  }
  const root = await fsp.realpath(temporaryRoot);
  const name = `${options.mode === 'fallback' ? 2 : 1}_update_package.pkg`;
  const candidates = [];
  for (const entry of await fsp.readdir(root, {withFileTypes:true})) {
    if (!entry.isDirectory()) continue;
    const candidate = path.join(root, entry.name, name);
    if (fs.existsSync(candidate)) candidates.push(candidate);
  }
  if (candidates.length !== 1) throw new Error(`Ready event has ${candidates.length} downloaded PKG candidates`);
  const archive = candidates[0];
  const stat = await fsp.lstat(archive);
  if (!stat.isFile() || stat.nlink !== 1 || !isWithin(root, await fsp.realpath(archive))) {
    throw new Error('Downloaded PKG is not an independent file in this test temporary directory');
  }
  const archiveSHA256 = await sha256(archive);
  if (archiveSHA256 !== staged.full_package?.sha256 ||
      archiveSHA256 !== await sha256(options.package) || stat.size !== staged.full_package.size) {
    throw new Error('Downloaded PKG does not match the ready event and source SHA256/size');
  }
  return {archive, archiveSHA256, size:stat.size};
}

function installerHandoffResult(state, expectedVersion) {
  if (state?.error) throw new Error(`Native installer handoff failed: ${state.error}`);
  if (!state?.installer_opened) return null;
  if (state.is_installer_package !== true || state.state !== 'update-downloaded' ||
      state.manifest?.target_version !== expectedVersion || state.manifest?.is_diff !== false) {
    throw new Error('Installer handoff state does not match the expected PKG');
  }
  return {acceptanceScope:'pkg-handoff', outcome:'awaiting-user-installation',
    installationVerified:false, installerOpened:true,
    limitation:'NSWorkspace accepted the PKG in the system Installer. Administrator authorization, installation, ownership and target app startup require a separate user test.'};
}

export async function snapshotTree(root) {
  const records = {};
  async function visit(relative) {
    const filename = path.join(root, relative);
    const stat = await fsp.lstat(filename);
    if (stat.isSymbolicLink()) {
      records[relative] = {kind:'symlink', target:await fsp.readlink(filename)};
    } else if (stat.isDirectory()) {
      records[relative] = {kind:'directory', mode:stat.mode & 0o777};
      for (const name of (await fsp.readdir(filename)).sort()) {
        await visit(relative ? `${relative}/${name}` : name);
      }
    } else if (stat.isFile()) {
      records[relative] = {kind:'file', size:stat.size, mode:stat.mode & 0o777,
        sha256:await sha256(filename)};
    } else throw new Error(`Unsupported bundle entry: ${filename}`);
  }
  await visit('');
  return records;
}

export function compareTrees(expected, actual) {
  return [...new Set([...Object.keys(expected), ...Object.keys(actual)])].sort()
      .filter(key => JSON.stringify(expected[key]) !== JSON.stringify(actual[key]))
      .map(key => ({path:key, expected:expected[key] ?? null, actual:actual[key] ?? null}));
}

async function waitFor(check, timeoutMs, description, retryErrors = true) {
  const deadline = Date.now() + timeoutMs;
  let lastError;
  do {
    if (interruptedError) throw interruptedError;
    try { const value = await check(); if (value) return value; }
    catch (error) {
      if (!retryErrors || error.fatal) throw error;
      lastError = error;
    }
    await pause(Math.min(250, Math.max(1, deadline - Date.now())));
  } while (Date.now() < deadline);
  throw new Error(`Timed out waiting for ${description}${lastError ? ': ' + lastError.message : ''}`);
}

class CDP {
  constructor(socket) {
    this.socket = socket;
    this.nextId = 1;
    this.pending = new Map();
    this.onEvent = () => {};
    socket.onmessage = event => {
      const message = JSON.parse(event.data);
      const pending = this.pending.get(message.id);
      if (pending) {
        if (message.error) pending.reject(new Error(`${pending.method}: ${message.error.message}`));
        else pending.resolve(message.result);
      } else if (message.method) this.onEvent(message);
    };
    socket.onclose = () => this.fail(new Error('CDP connection closed'));
    socket.onerror = () => this.fail(new Error('CDP WebSocket error'));
  }
  static async connect(url) {
    const socket = new WebSocket(url);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => { socket.close(); reject(new Error('CDP connection timeout')); }, 10000);
      socket.onopen = () => { clearTimeout(timer); resolve(); };
      socket.onerror = () => { clearTimeout(timer); reject(new Error('Could not connect CDP')); };
    });
    return new CDP(socket);
  }
  fail(error) {
    this.error = error;
    for (const request of [...this.pending.values()]) request.reject(error);
  }
  send(method, params = {}, timeoutMs = 30000) {
    if (this.error) return Promise.reject(this.error);
    return new Promise((resolve, reject) => {
      const id = this.nextId++;
      const settle = (callback, value) => { clearTimeout(timer); this.pending.delete(id); callback(value); };
      const timer = setTimeout(() => settle(reject, new Error(`CDP timeout: ${method}`)), timeoutMs);
      this.pending.set(id, {method, resolve:value => settle(resolve, value), reject:error => settle(reject, error)});
      try { this.socket.send(JSON.stringify({id, method, params})); }
      catch (error) { this.pending.get(id)?.reject(error); }
    });
  }
  async evaluate(expression) {
    const result = await this.send('Runtime.evaluate', {expression, awaitPromise:true, returnByValue:true});
    if (result.exceptionDetails) throw new Error(`Page evaluation failed: ${JSON.stringify(result.exceptionDetails)}`);
    return result.result?.value;
  }
  close() { this.fail(new Error('CDP client finished')); this.socket.close(); }
}

async function readJSON(url) {
  const response = await fetch(url, {signal:AbortSignal.timeout(2000)});
  if (!response.ok) throw new Error(`HTTP ${response.status}: ${url}`);
  return response.json();
}

async function discover(dataDir, previousIdentity) {
  const [portText, identity] = (await fsp.readFile(path.join(dataDir, 'DevToolsActivePort'), 'utf8')).trim().split('\n');
  const port = Number(portText);
  if (!Number.isInteger(port) || port < 1 || port > 65535 || !identity?.startsWith('/devtools/browser/')) return null;
  if (identity === previousIdentity) return null;
  const version = await readJSON(`http://127.0.0.1:${port}/json/version`);
  if (!version.webSocketDebuggerUrl?.endsWith(identity)) return null;
  return {port, identity, version, ws:`ws://127.0.0.1:${port}${identity}`};
}

async function startFeed(options, expectedBaseVersion, record) {
  if (!options.package.toLowerCase().endsWith('.pkg')) throw new Error('Full macOS updates require a .pkg input');
  const packages = {};
  for (const [name, filename] of [['patch', options.patch], ['package', options.package]]) {
    const required = name === 'patch' ? options.mode !== 'full' : options.mode !== 'diff';
    if (!required && !fs.existsSync(filename)) continue;
    const stat = await fsp.stat(filename);
    if (!stat.isFile() || stat.size <= 0) throw new Error(`Invalid ${name}: ${filename}`);
    packages[name] = {filename, size:stat.size, sha256:await sha256(filename)};
  }
  const requests = [];
  const server = http.createServer((request, response) => {
    const item = {at:isoNow(), method:request.method, url:request.url, bytes:0};
    requests.push(item);
    response.on('finish', () => { item.status = response.statusCode; item.finishedAt = isoNow(); record('http', item); });
    response.on('close', () => { if (!response.writableFinished) { item.aborted = true; record('http', item); } });
    const url = new URL(request.url, `http://127.0.0.1:${server.address().port}`);
    response.setHeader('Cache-Control', 'no-store');
    if (request.method !== 'GET') { response.writeHead(405).end(); return; }
    if (url.pathname === '/api/v1/update/check') {
      if (url.searchParams.get('version') !== expectedBaseVersion || url.searchParams.get('platform') !== 'mac') {
        response.writeHead(400).end('Unexpected base version or platform'); return;
      }
      const body = Buffer.from(JSON.stringify(server.manifest));
      item.bytes = body.length;
      response.writeHead(200, {'Content-Type':'application/json', 'Content-Length':body.length}).end(body);
      return;
    }
    const key = url.pathname === '/patch.zip' ? 'patch' : url.pathname === '/package.pkg' ? 'package' : null;
    const selected = packages[key];
    if (!selected) { response.writeHead(404).end(); return; }
    response.writeHead(200, {'Content-Type':'application/octet-stream', 'Content-Length':selected.size});
    const stream = fs.createReadStream(selected.filename);
    stream.on('data', bytes => { item.bytes += bytes.length; });
    stream.on('error', error => { item.error = error.message; response.destroy(error); });
    response.on('close', () => stream.destroy());
    stream.pipe(response);
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const baseURL = `http://127.0.0.1:${server.address().port}`;
  const describe = name => ({url:`${baseURL}/${name}.${name === 'package' ? 'pkg' : 'zip'}`, size:packages[name].size, sha256:packages[name].sha256});
  server.manifest = {platform:'mac', target_version:options.expectedVersion, is_diff:options.mode !== 'full', changelog:`Real macOS acceptance: ${options.mode}`};
  if (packages.patch && options.mode !== 'full') {
    server.manifest.diff_package = describe('patch');
    if (options.mode === 'fallback') {
      const hash = server.manifest.diff_package.sha256;
      server.manifest.diff_package.sha256 = (hash[0] === '0' ? '1' : '0') + hash.slice(1);
    }
  }
  if (packages.package) server.manifest.full_package = describe('package');
  return {server, requests, packages, feedURL:`${baseURL}/api/v1/update/check`};
}

async function attachBrowser(discovery, options, record, clients) {
  const browser = await CDP.connect(discovery.ws);
  clients.add(browser);
  const {arguments:commandLine} = await browser.send('Browser.getBrowserCommandLine');
  if (!commandLine.includes(`--user-data-dir=${options.dataDir}`)) throw new Error('CDP connected to the wrong profile');
  const {processInfo} = await browser.send('SystemInfo.getProcessInfo');
  const pid = processInfo.find(process => process.type === 'browser')?.id;
  if (!Number.isInteger(pid) || pid <= 1) throw new Error('CDP did not return the browser PID');
  const {targetId} = await browser.send('Target.createTarget', {url:'chrome://xenon-update/'});
  const target = await waitFor(async () => (await readJSON(`http://127.0.0.1:${discovery.port}/json/list`))
      .find(page => page.id === targetId && page.webSocketDebuggerUrl), 30000, 'update WebUI target');
  const page = await CDP.connect(target.webSocketDebuggerUrl);
  clients.add(page);
  const state = {events:[], initial:null, current:null, staged:null, error:null, noUpdate:false};
  page.onEvent = message => {
    if (message.method !== 'Runtime.bindingCalled' || message.params.name !== '__xenonUpdateEvidence') return;
    const event = JSON.parse(message.params.payload);
    state.events.push(event);
    record('webui', {...event, pid});
    if (event.name === 'initial-state') state.initial = event.args[0];
    if (event.name === 'initial-state' || event.name === 'update-state-changed') state.current = event.args[0];
    if (event.name === 'update-downloaded') state.staged = event.args[0];
    if (event.name === 'update-error') state.error = event.args[0];
    if (event.name === 'update-not-available') state.noUpdate = true;
  };
  await page.send('Page.enable');
  await page.send('Runtime.enable');
  await page.send('Runtime.addBinding', {name:'__xenonUpdateEvidence'});
  await waitFor(async () => await page.evaluate(
      'location.protocol === "chrome:" && location.hostname === "xenon-update" && document.readyState === "complete"'),
      30000, 'WebUI load');
  await page.evaluate(`(async () => {
    const {addWebUiListener} = await import('chrome://resources/js/cr.js');
    for (const name of ['initial-state','checking-for-update','update-available',
        'update-not-available','download-progress','update-downloaded','update-error','update-state-changed']) {
      addWebUiListener(name, (...args) => window.__xenonUpdateEvidence(JSON.stringify({at:new Date().toISOString(),name,args})));
    }
    chrome.send('getInitialState');
    return true;
  })()`);
  await waitFor(() => state.initial, 10000, 'native initial-state event');
  return {browser, page, state, pid, processInfo, commandLine, discovery};
}

async function screenshot(page, filename) {
  const image = await page.send('Page.captureScreenshot', {format:'png'});
  await fsp.writeFile(filename, Buffer.from(image.data, 'base64'));
}

function alive(pid) {
  try { process.kill(pid, 0); return true; }
  catch (error) { if (error.code === 'ESRCH') return false; throw error; }
}

function assertNormalExit(result, description) {
  if (!result || result.code !== 0 || result.signal !== null) {
    throw new Error(`${description} did not exit normally: ${JSON.stringify(result)}`);
  }
}

// LaunchServices owns the restarted process, so Node cannot waitpid() it.
// Darwin permits NOTE_EXITSTATUS for a process we are allowed to signal.
// Register before Browser.close to capture its real exit status, even SIGTRAP.
function watchProcessExit(pid, record) {
  const script = `import json, os, select, sys
pid = int(sys.argv[1])
queue = select.kqueue()
try:
    note_exitstatus = 0x04000000
    change = select.kevent(pid, filter=select.KQ_FILTER_PROC,
                          flags=select.KQ_EV_ADD | select.KQ_EV_ENABLE | select.KQ_EV_ONESHOT,
                          fflags=select.KQ_NOTE_EXIT | note_exitstatus)
    queue.control([change], 0, 0)
    print(json.dumps({'kind':'ready', 'pid':pid}), flush=True)
    event = queue.control(None, 1, None)[0]
    status = event.data
    print(json.dumps({'kind':'exit', 'pid':pid, 'waitStatus':status,
                      'code':os.WEXITSTATUS(status) if os.WIFEXITED(status) else None,
                      'signal':os.WTERMSIG(status) if os.WIFSIGNALED(status) else None}), flush=True)
finally:
    queue.close()
`;
  const watcher = spawn('/usr/bin/python3', ['-u', '-c', script, String(pid)],
      {stdio:['ignore', 'pipe', 'pipe']});
  const monitor = {pid, watcher, result:null, error:null, stopped:false};
  let output = '';
  let errors = '';
  watcher.stdout.setEncoding('utf8');
  watcher.stderr.setEncoding('utf8');
  watcher.stderr.on('data', data => { errors += data; });
  monitor.ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      monitor.error = new Error(`Exit monitor did not attach to PID ${pid}`);
      reject(monitor.error);
      monitor.stop();
    }, 10000);
    watcher.stdout.on('data', data => {
      output += data;
      for (;;) {
        const end = output.indexOf('\n');
        if (end < 0) break;
        const line = output.slice(0, end);
        output = output.slice(end + 1);
        try {
          const event = JSON.parse(line);
          if (event.kind === 'ready') {
            clearTimeout(timer);
            record('exit-monitor-ready', {pid});
            resolve(monitor);
          } else if (event.kind === 'exit') {
            const {kind, ...details} = event;
            monitor.result = details;
            record('monitored-process-exit', details);
          }
        } catch (error) { monitor.error = error; reject(error); }
      }
    });
    watcher.on('error', error => {
      clearTimeout(timer);
      monitor.error = error;
      reject(error);
    });
    watcher.on('close', (code, signal) => {
      clearTimeout(timer);
      if (!monitor.result && !monitor.stopped) {
        monitor.error = new Error(`Exit monitor failed for PID ${pid}: ${code ?? signal}: ${errors.trim()}`);
        reject(monitor.error);
      }
    });
  });
  monitor.stop = () => {
    monitor.stopped = true;
    if (watcher.exitCode === null && watcher.signalCode === null) watcher.kill();
  };
  return monitor;
}

async function closeTestBrowsers(options, connected, child, record, monitors, ensureMonitor, awaitingRelaunch) {
  const known = new Set(connected.map(item => item.pid));
  if (child?.pid) known.add(child.pid);
  let failure;
  const forced = [];
  // Recover a just-relaunched instance even if failure happened before attach.
  try {
    const found = awaitingRelaunch
      ? await waitFor(() => discover(options.dataDir, connected[0]?.discovery.identity),
          30000, 'possible relaunch during cleanup')
      : await discover(options.dataDir);
    if (found) {
      const browser = await CDP.connect(found.ws);
      const {arguments:args} = await browser.send('Browser.getBrowserCommandLine');
      if (args.includes(`--user-data-dir=${options.dataDir}`)) {
        const {processInfo} = await browser.send('SystemInfo.getProcessInfo');
        for (const item of processInfo) {
          if (item.type !== 'browser') continue;
          known.add(item.id);
          await ensureMonitor(item.id).catch(error => { failure ??= error; });
        }
        await browser.send('Browser.close', {}, 5000).catch(() => {});
      }
      browser.close();
    }
  } catch (error) { record('cleanup-discovery', {error:error.message}); }
  for (const item of [...connected].reverse()) {
    if (!item.browser.error) {
      await ensureMonitor(item.pid).catch(error => { failure ??= error; });
      await item.browser.send('Browser.close', {}, 5000).catch(() => {});
    }
  }
  for (const pid of known) {
    if (!alive(pid)) continue;
    try { await waitFor(() => !alive(pid), 120000, `browser ${pid} normal exit`); }
    catch {
      // Only PIDs obtained from our child or verified, profile-specific CDP.
      for (const signal of ['SIGTERM', 'SIGKILL']) {
        if (!alive(pid)) break;
        record('cleanup-signal', {pid, signal});
        forced.push({pid, signal});
        process.kill(pid, signal);
        await waitFor(() => !alive(pid), 5000, `browser ${pid} ${signal}`).catch(() => {});
      }
    }
  }
  const surviving = [...known].filter(alive);
  record('cleanup', {knownPids:[...known], survivingPids:surviving});
  if (surviving.length) throw new Error(`Test browser processes survived cleanup: ${surviving.join(', ')}`);
  if (forced.length) failure ??= new Error(`Browser required forced termination: ${JSON.stringify(forced)}`);
  for (const [pid, monitor] of monitors) {
    try {
      const result = await waitFor(() => {
        if (monitor.error) throw monitor.error;
        return monitor.result;
      }, 5000, `exit status for browser ${pid}`, false);
      assertNormalExit(result, `Browser ${pid}`);
    } catch (error) { failure ??= error; }
  }
  if (child?.pid) {
    try { assertNormalExit({code:child.exitCode, signal:child.signalCode}, 'Original browser'); }
    catch (error) { failure ??= error; }
  }
  if (failure) throw failure;
}

async function runningBundleProcesses(app) {
  const output = await runFile('/bin/ps', ['-ww', '-axo', 'pid=,command=']);
  return output.stdout.split('\n').flatMap(line => {
    const match = line.match(/^\s*(\d+)\s+(.+)$/);
    return match && match[2].startsWith(app + '/Contents/') ? [Number(match[1])] : [];
  });
}

async function unregisterDiscardableTestApp(options, cleanup, record,
    {runCommand = runFile, isAlive = alive, listBundleProcesses = runningBundleProcesses} = {}) {
  // Actual user/system installations remain registered. This is only cleanup
  // for the disposable copy that this harness launched inside its test root.
  if (options.installedTestApp || installedAppPaths.has(options.app)) {
    record('launch-services-unregister-skipped', {app:options.app, reason:'actual-installed-application'});
    return;
  }
  if (!cleanup?.knownPids?.length) {
    record('launch-services-unregister-skipped', {app:options.app, reason:'no-confirmed-test-process-cleanup'});
    return;
  }
  try {
    if (cleanup.survivingPids.length || cleanup.knownPids.some(isAlive)) {
      throw new Error('Refusing LaunchServices cleanup while a test browser is still running');
    }
    const root = await fsp.realpath(path.dirname(options.artifactsDir));
    const app = await validateTestBundle(options.app, root);
    if (app !== path.resolve(options.app)) throw new Error('Refusing LaunchServices cleanup through an application symlink alias');
    const running = await listBundleProcesses(app);
    if (running.length) {
      throw new Error(`Refusing LaunchServices cleanup while bundle processes are running: ${running.join(', ')}`);
    }
    const args = ['-u', app];
    const result = await runCommand(launchServicesTool, args);
    record('launch-services-unregistered', {app, command:launchServicesTool, args,
      stdout:result.stdout, stderr:result.stderr});
  } catch (error) {
    record('launch-services-unregister-failed', {app:options.app, error:error.message});
    throw error;
  }
}

export async function runRealUpdate(options) {
  if (process.platform !== 'darwin') throw new Error('This acceptance test runs on macOS only');
  if (typeof WebSocket !== 'function') throw new Error('Node.js with built-in WebSocket is required');
  if (fs.existsSync(options.dataDir) && (await fsp.readdir(options.dataDir)).length) {
    throw new Error(`Refusing to reuse nonempty profile: ${options.dataDir}`);
  }
  const app = await fsp.realpath(options.app);
  if (options.installedTestApp) {
    if (!isAuthorizedInstallation(options.app, app, options.installedTestApp)) {
      throw new Error('Installed test app must match its exact authorized path without symlink aliases');
    }
    // Validate even without ad-hoc quarantine handling: installed app testing
    // never authorizes a symlink alias or entries shared with another bundle.
    await validateTestBundle(options.app, path.dirname(options.app), options.installedTestApp);
  } else if (installedAppPaths.has(app) || installedAppPaths.has(options.app)) {
    throw new Error(`Updating ${app} requires an explicit matching --installed-test-app`);
  }
  if (options.targetApp && await fsp.realpath(options.targetApp) === app) throw new Error('App and target refer to the same bundle');
  const before = await bundleInfo(options.app);
  const components = value => value.split('.').map(Number);
  const oldParts = components(before.version);
  const newParts = components(options.expectedVersion);
  const different = oldParts.findIndex((value, index) => value !== newParts[index]);
  if (different < 0 || oldParts[different] > newParts[different]) {
    throw new Error('Expected version must be newer than the test application');
  }
  if (!options.app.endsWith('.app') || before.executable.includes('/')) throw new Error('Invalid application bundle/executable');
  for (const directory of [options.dataDir, options.artifactsDir]) {
    for (const bundle of [options.app, options.targetApp].filter(Boolean)) {
      if (directory === bundle || directory.startsWith(bundle + path.sep)) {
        throw new Error('Profile and evidence directories must be outside the application bundles');
      }
    }
  }
  await fsp.mkdir(options.artifactsDir, {recursive:true});
  await fsp.mkdir(options.dataDir, {recursive:true});
  await fsp.writeFile(path.join(options.dataDir, '.xenon-real-update-test'), JSON.stringify({app, createdAt:isoNow()}));
  const eventsFile = path.join(options.artifactsDir, 'events.jsonl');
  await fsp.writeFile(eventsFile, '');
  const evidence = {startedAt:isoNow(), options, before, passed:false, events:[],
    acceptanceScope:options.mode === 'diff' ? 'diff-install-restart' : 'pkg-handoff',
    installationVerified:false};
  const record = (kind, details) => {
    const item = {at:isoNow(), kind, ...details};
    evidence.events.push(item);
    fs.appendFileSync(eventsFile, JSON.stringify(item) + '\n');
  };
  const clients = new Set();
  const connected = [];
  const monitors = new Map();
  const ensureMonitor = async pid => {
    if (!monitors.has(pid)) monitors.set(pid, watchProcessExit(pid, record));
    return monitors.get(pid).ready;
  };
  let feed;
  let child;
  let failure;
  let temporaryRoot;
  let preparation;
  let preparedTask;
  let cleaning = false;
  const onSignal = signal => {
    if (cleaning) return;
    interruptedError = new Error(`Interrupted by ${signal}`);
    record('interrupted', {signal});
  };
  const onSigint = () => onSignal('SIGINT');
  const onSigterm = () => onSignal('SIGTERM');
  process.on('SIGINT', onSigint);
  process.on('SIGTERM', onSigterm);
  try {
    if (options.installedTestApp) record('explicit-installed-app-test', {app:options.installedTestApp});
    temporaryRoot = await fsp.realpath(await fsp.mkdtemp(path.join(options.artifactsDir, 'browser-tmp-')));
    evidence.privateTemporaryRoot = temporaryRoot;
    record('private-temporary-directory', {path:temporaryRoot, variables:['TMPDIR', 'MAC_CHROMIUM_TMPDIR']});
    if (options.allowLocalAdhoc) {
      const testRoot = await fsp.realpath(path.dirname(options.artifactsDir));
      if (!isWithin(testRoot, await fsp.realpath(options.dataDir))) {
        throw new Error('--allow-local-adhoc requires the fresh profile and app inside the artifacts parent directory');
      }
      await clearLocalQuarantine(options.app, testRoot, 'installed', options, record);
      evidence.localAdhoc = {enabled:true, limitation:'Quarantine bypass for locally built ad-hoc test apps only; official release trust chain not validated.'};
      const parent = await fsp.realpath(path.dirname(options.app));
      preparation = {parent, existing:await listPreparationDirectories(parent)};
      record('native-preparation-baseline', preparation);
    }
    console.log(`Preparing real ${options.mode} update feed: ${before.version} -> ${options.expectedVersion}`);
    feed = await startFeed(options, before.version, record);
    evidence.manifest = feed.server.manifest;
    evidence.packages = feed.packages;
    await fsp.writeFile(path.join(options.artifactsDir, 'feed.json'), JSON.stringify(feed.server.manifest, null, 2));
    const args = [`--user-data-dir=${options.dataDir}`, '--remote-debugging-port=0',
      '--remote-debugging-address=127.0.0.1', '--enable-automation', '--no-first-run',
      '--no-default-browser-check', '--disable-background-networking', '--window-size=1280,900',
      '--enable-logging', `--log-file=${path.join(options.artifactsDir, 'browser.log')}`,
      ...options.browserArgs, 'chrome://xenon-update/'];
    const output = fs.openSync(path.join(options.artifactsDir, 'browser-stdio.log'), 'a');
    try { child = spawn(path.join(options.app, 'Contents', 'MacOS', before.executable), args,
      {stdio:['ignore', output, output], env:temporaryRoot
        ? {...process.env, TMPDIR:temporaryRoot, MAC_CHROMIUM_TMPDIR:temporaryRoot} : process.env}); }
    finally { fs.closeSync(output); }
    child.on('error', error => { evidence.launchError = error.message; });
    child.on('exit', (code, signal) => {
      evidence.originalExit = {pid:child.pid, code, signal};
      record('original-process-exit', evidence.originalExit);
    });
    record('launch', {pid:child.pid, args});
    console.log(`Launched independent test app (PID ${child.pid}); waiting for CDP and native WebUI...`);
    const initialDiscovery = await waitFor(async () => {
      if (evidence.launchError || child.exitCode !== null || child.signalCode !== null) {
        throw Object.assign(new Error(evidence.launchError || `Browser exited during startup (${child.exitCode ?? child.signalCode})`), {fatal:true});
      }
      return discover(options.dataDir);
    }, options.timeoutMs, 'initial browser CDP');
    const initial = await attachBrowser(initialDiscovery, options, record, clients);
    connected.push(initial);
    if (initial.pid !== child.pid) throw new Error('Browser launch reused an existing process');
    await ensureMonitor(initial.pid);
    evidence.before = {...before, pid:initial.pid, cdp:initialDiscovery,
      native:initial.state.initial, processes:initial.processInfo, commandLine:initial.commandLine};
    if (initial.state.initial.current_version !== before.version) throw new Error('Initial native version does not match bundle plist');
    console.log(`Native product version ${before.version} verified; downloading from ${feed.feedURL}`);
    await screenshot(initial.page, path.join(options.artifactsDir, 'before.png'));
    await initial.page.evaluate(`document.getElementById('serverUrlInput').value = ${JSON.stringify(feed.feedURL)};
      chrome.send('checkForUpdates', [${JSON.stringify(feed.feedURL)}]);`);
    const staged = await waitFor(() => {
      if (evidence.originalExit) throw new Error(`Browser exited before staging: ${JSON.stringify(evidence.originalExit)}`);
      if (initial.state.error) throw new Error(`Native updater failed: ${initial.state.error}`);
      if (initial.state.noUpdate) throw new Error('Native updater reported no update');
      return initial.state.staged;
    }, options.timeoutMs, 'native update-downloaded event', false);
    if (staged.target_version !== options.expectedVersion) throw new Error('Staged target version is incorrect');
    const expectedDiff = options.mode === 'diff';
    if (staged.is_diff !== expectedDiff) throw new Error(`Unexpected staged package type in ${options.mode} mode`);
    evidence.staged = staged;
    if (options.allowLocalAdhoc && options.mode === 'diff') {
      const stage = await locateStagedApp(temporaryRoot, options, staged);
      evidence.localAdhoc.staged = stage;
      await clearLocalQuarantine(stage.app, temporaryRoot, 'staged', options, record,
        {readyEvent:staged, ...stage});
      preparation.expectedTree = await snapshotTree(stage.app);
      await fsp.writeFile(path.join(options.artifactsDir, 'staged-tree.json'), JSON.stringify(preparation.expectedTree, null, 2));
    }
    await screenshot(initial.page, path.join(options.artifactsDir, 'staged.png'));
    const downloaded = feed.requests.filter(item => item.status === 200).map(item => new URL(item.url, feed.feedURL).pathname);
    if (options.mode !== 'full' && !downloaded.includes('/patch.zip')) throw new Error('Differential package was not requested');
    if (options.mode !== 'diff' && !downloaded.includes('/package.pkg')) throw new Error('Full PKG was not requested');
    if (options.mode === 'diff' && downloaded.includes('/package.pkg')) throw new Error('Diff test unexpectedly fell back to PKG');
    if (options.mode === 'fallback' && downloaded.indexOf('/patch.zip') > downloaded.indexOf('/package.pkg')) throw new Error('Fallback download order is incorrect');
    if (options.mode !== 'diff') {
      evidence.downloadedInstaller = await locateDownloadedInstaller(temporaryRoot, options, staged);
      evidence.installerHandoffRequested = true;
      record('installer-handoff-requested', {pid:initial.pid, ...evidence.downloadedInstaller});
      initial.state.current = null;
      await initial.page.evaluate("chrome.send('quitAndInstall'); true");
      const handoff = await waitFor(() => {
        if (evidence.originalExit) throw new Error('Browser exited before confirming installer handoff');
        if (initial.state.error) throw new Error(`Native installer failed: ${initial.state.error}`);
        return installerHandoffResult(initial.state.current, options.expectedVersion);
      }, options.timeoutMs, 'native installer_opened confirmation', false);
      Object.assign(evidence, handoff);
      record('installer-handoff-confirmed', handoff);
      await screenshot(initial.page, path.join(options.artifactsDir, 'installer-handoff.png'));
      evidence.contentComparison = {performed:false, reason:'PKG handoff only; system installation and target application remain unverified'};
    } else {
      record('quit-and-install', {pid:initial.pid});
      evidence.installRequested = true;
      console.log(`Native staging completed; requesting quitAndInstall for PID ${initial.pid}...`);
      // Observe before sending the command. Matching helper argv proves the
      // native copy is finished; partial copying is never accepted as a target.
      preparedTask = options.allowLocalAdhoc
        ? clearPreparedCopy(options, initial, preparation, record)
            .then(value => ({value}), error => ({error}))
        : null;
      await initial.page.evaluate("chrome.send('quitAndInstall'); true");
      if (preparedTask) {
        const outcome = await preparedTask;
        if (outcome.error) throw outcome.error;
        evidence.localAdhoc.prepared = outcome.value;
      }
      await waitFor(() => evidence.originalExit, options.timeoutMs, 'original browser process exit');
      assertNormalExit(evidence.originalExit, 'Original browser after quitAndInstall');
      const nextDiscovery = await waitFor(() => discover(options.dataDir, initialDiscovery.identity), options.timeoutMs, 'restarted browser CDP');
      const restarted = await attachBrowser(nextDiscovery, options, record, clients);
      connected.push(restarted);
      await ensureMonitor(restarted.pid);
      console.log(`Relaunched browser connected (PID ${restarted.pid}); verifying product version and bundle...`);
      const after = await bundleInfo(options.app);
      const badge = await restarted.page.evaluate("document.getElementById('currentVersionBadge').textContent");
      evidence.after = {...after, pid:restarted.pid, cdp:nextDiscovery,
        native:restarted.state.initial, badge, processes:restarted.processInfo, commandLine:restarted.commandLine};
      if (restarted.pid === initial.pid) throw new Error('Browser did not restart into a new process');
      if (after.version !== options.expectedVersion || restarted.state.initial.current_version !== options.expectedVersion || !badge.includes(`v${options.expectedVersion}`)) {
        throw new Error(`Post-restart version verification failed: ${JSON.stringify(evidence.after)}`);
      }
      await screenshot(restarted.page, path.join(options.artifactsDir, 'after.png'));
      if (options.targetApp) {
        console.log('Comparing installed bundle with the target, including every file SHA256, mode and symlink...');
        const expectedTree = await snapshotTree(options.targetApp);
        const actualTree = await snapshotTree(options.app);
        const differences = compareTrees(expectedTree, actualTree);
        await fsp.writeFile(path.join(options.artifactsDir, 'target-tree.json'), JSON.stringify(expectedTree, null, 2));
        await fsp.writeFile(path.join(options.artifactsDir, 'installed-tree.json'), JSON.stringify(actualTree, null, 2));
        await fsp.writeFile(path.join(options.artifactsDir, 'tree-differences.json'), JSON.stringify(differences, null, 2));
        evidence.contentComparison = {entries:Object.keys(expectedTree).length, differences:differences.length};
        if (differences.length) throw new Error(`Installed bundle differs from target at ${differences.length} entries`);
      } else evidence.contentComparison = {performed:false, reason:'--target-app not provided; external comparison required'};
      evidence.installationVerified = true;
      evidence.outcome = 'installed-and-restarted';
    }
    if (interruptedError) throw interruptedError;
    evidence.passed = true;
  } catch (error) {
    failure = error;
    evidence.error = {message:error.message, stack:error.stack};
    record('test-failure', evidence.error);
    console.error(`Test failed; closing its browsers: ${error.message}`);
    const latest = connected.at(-1);
    if (latest && !latest.page.error) await screenshot(latest.page, path.join(options.artifactsDir, 'failure.png')).catch(() => {});
  } finally {
    cleaning = true;
    interruptedError = undefined;
    if (preparedTask) {
      preparation.cancelled = true;
      const outcome = await preparedTask;
      if (outcome.error && !failure) {
        failure = outcome.error;
        evidence.passed = false;
        evidence.preparationError = outcome.error.message;
      }
    }
    try { await closeTestBrowsers(options, connected, child, record, monitors, ensureMonitor,
      evidence.installRequested && connected.length < 2); }
    catch (error) { evidence.cleanupError = error.message; evidence.passed = false; failure ??= error; }
    try {
      await unregisterDiscardableTestApp(options,
        evidence.events.findLast(event => event.kind === 'cleanup'), record);
    } catch (error) {
      evidence.registrationCleanupError = error.message;
      evidence.passed = false;
      failure ??= error;
    }
    for (const monitor of monitors.values()) monitor.stop();
    for (const client of clients) client.close();
    if (feed) {
      evidence.httpRequests = feed.requests;
      feed.server.closeAllConnections();
      await new Promise(resolve => feed.server.close(resolve));
    }
    evidence.finishedAt = isoNow();
    await fsp.writeFile(path.join(options.artifactsDir, 'result.json'), JSON.stringify(evidence, null, 2));
    process.removeListener('SIGINT', onSigint);
    process.removeListener('SIGTERM', onSigterm);
  }
  if (failure) throw failure;
  if (evidence.acceptanceScope === 'pkg-handoff') {
    console.log(`PASS PKG HANDOFF ONLY (${options.mode}); installation NOT verified. Complete administrator authorization and installation separately. System Installer left open; test browser closed. Evidence: ${options.artifactsDir}`);
  } else {
    console.log(`PASS ${options.mode}: ${evidence.before.version} (PID ${evidence.before.pid}) -> ${evidence.after.version} (PID ${evidence.after.pid}); browser closed, evidence: ${options.artifactsDir}`);
  }
  return evidence;
}

async function selfTest() {
  assert.equal(selectNewPreparation(['old', 'new'], ['old']), 'new');
  assert.equal(selectNewPreparation(['old'], ['old']), undefined);
  assert.throws(() => selectNewPreparation(['first','second'], []), /More than one/);
  const helper = '/test/test.app/Contents/Helpers/xenon_update_helper';
  const prepared = '/test/.xenon-update-abcdef/test.app';
  const helperLine = `123 ${helper} 456 3 ${prepared} /test/test.app 1 --user-data-dir=/test/profile`;
  assert.equal(matchHelperCommand(helperLine, helper, 456, prepared, '/test/test.app'), 123);
  assert.equal(matchHelperCommand(helperLine, helper, 999, prepared, '/test/test.app'), null);
  assert.equal(matchHelperCommand(helperLine, helper, 456, prepared, '/another.app'), null);
  assert.equal(matchHelperCommand(helperLine, helper, 456, '/test/other.app', '/test/test.app'), null);
  assert.equal(isAuthorizedPreparation('/Applications/.xenon-update-ab12CD/xl153.app',
    '/Applications/xl153.app', '/Applications/.xenon-update-ab12CD/xl153.app'), true);
  for (const candidate of ['/Applications/other/xl153.app',
    '/Applications/.xenon-update-ab12CD/other.app', '/other/.xenon-update-ab12CD/xl153.app']) {
    assert.equal(isAuthorizedPreparation(candidate, '/Applications/xl153.app', candidate), false);
  }
  assert.equal(isAuthorizedPreparation('/Applications/.xenon-update-ab12CD/xl153.app',
    undefined, '/Applications/.xenon-update-ab12CD/xl153.app'), false);
  const userPrepared = path.join(userApplications, '.xenon-update-ab12CD', 'xl153.app');
  assert.equal(isAuthorizedPreparation(userPrepared, userInstalledApp, userPrepared), true);
  assert.equal(isAuthorizedPreparation(userPrepared, '/Applications/xl153.app', userPrepared), false);
  assert.equal(isAuthorizedPreparation(userPrepared, userInstalledApp, userPrepared + '-alias'), false);
  for (const candidate of [path.join(userApplications, 'other', 'xl153.app'),
    path.join(userApplications, '.xenon-update-ab12CD', 'Other.app'),
    path.join(currentUserHome, 'outside', '.xenon-update-ab12CD', 'xl153.app'),
    '/Users/another-user/Applications/.xenon-update-ab12CD/xl153.app']) {
    assert.equal(isAuthorizedPreparation(candidate, userInstalledApp, candidate), false);
  }
  for (const installedApp of installedAppPaths) {
    assert.equal(isAuthorizedInstallation(installedApp, installedApp, installedApp), true);
    assert.equal(isAuthorizedInstallation('/tmp/alias.app', installedApp, installedApp), false);
    assert.equal(isAuthorizedInstallation(installedApp, '/tmp/escaped.app', installedApp), false);
  }
  assert.equal(isAuthorizedInstallation('/tmp/Other.app', '/tmp/Other.app', '/tmp/Other.app'), false);
  assert.doesNotThrow(() => assertNormalExit({code:0, signal:null}, 'Browser'));
  assert.throws(() => assertNormalExit({code:null, signal:'SIGTRAP'}, 'Browser'), /did not exit normally/);
  assert.throws(() => assertNormalExit({code:1, signal:null}, 'Browser'), /did not exit normally/);
  assert.throws(() => parseArgs(['--mode', 'bad']), /Missing/);
  const config = parseArgs(['--app','/tmp/a.app',
    '--data-dir','/tmp/profile','--expected-version','1.0.0.2','--mode','fallback',
    '--artifacts-dir','/tmp/artifacts','--browser-arg=--disable-gpu','--allow-local-adhoc']);
  assert.equal(config.mode, 'fallback');
  assert.equal(config.allowLocalAdhoc, true);
  assert.equal(config.package, '/tmp/artifacts/package.pkg');
  const packageArgs = ['--app','/tmp/a.app','--data-dir','/tmp/profile',
    '--expected-version','1.0.0.2','--artifacts-dir','/tmp/evidence','--mode','full'];
  assert.throws(() => parseArgs([...packageArgs,'--package','/tmp/package.zip']), /\.pkg installer/);
  assert.throws(() => parseArgs([...packageArgs,'--target-app','/tmp/b.app']), /only supported in diff/);
  const handoffState = {state:'update-downloaded', is_installer_package:true, installer_opened:true,
    manifest:{target_version:'1.0.0.2', is_diff:false}};
  assert.equal(installerHandoffResult(null, '1.0.0.2'), null);
  assert.equal(installerHandoffResult({...handoffState, installer_opened:false}, '1.0.0.2'), null);
  const handoffResult = installerHandoffResult(handoffState, '1.0.0.2');
  assert.equal(handoffResult.installationVerified, false);
  assert.equal(handoffResult.acceptanceScope, 'pkg-handoff');
  assert.equal(handoffResult.outcome, 'awaiting-user-installation');
  assert.throws(() => installerHandoffResult({...handoffState, error:'authorization failed'}, '1.0.0.2'), /authorization failed/);
  assert.throws(() => installerHandoffResult({...handoffState, is_installer_package:false}, '1.0.0.2'), /does not match/);
  assert.throws(() => installerHandoffResult(handoffState, '1.0.0.3'), /does not match/);
  await assert.rejects(waitFor(() => installerHandoffResult(null, '1.0.0.2'), 5, 'installer handoff'), /Timed out/);
  const installed = parseArgs(['--app','/Applications/xl153.app',
    '--data-dir','/tmp/profile','--expected-version','1.0.0.2',
    '--artifacts-dir','/tmp/evidence','--installed-test-app=/Applications/xl153.app']);
  assert.equal(installed.installedTestApp, installed.app);
  const installationArgs = (app, authorization) => ['--app',app,
    '--data-dir','/tmp/profile','--expected-version','1.0.0.2',
    '--artifacts-dir','/tmp/evidence',`--installed-test-app=${authorization}`];
  for (const app of [userInstalledApp, '~/Applications/xl153.app']) {
    for (const authorization of [userInstalledApp, '~/Applications/xl153.app']) {
      const authorized = parseArgs(installationArgs(app, authorization));
      assert.equal(authorized.app, userInstalledApp);
      assert.equal(authorized.installedTestApp, userInstalledApp);
    }
  }
  for (const candidate of [path.join(userApplications, 'Other.app'),
    path.join(currentUserHome, 'outside', 'xl153.app'),
    '/Users/another-user/Applications/xl153.app',
    '~/Applications/../xl153.app', '~/Applications/../Applications/xl153.app',
    '~another-user/Applications/xl153.app', 'Applications/xl153.app']) {
    assert.throws(() => parseArgs(installationArgs(candidate, candidate)), /exactly match/);
  }
  assert.throws(() => parseArgs(installationArgs(userInstalledApp, '/Applications/xl153.app')), /exactly match/);
  assert.throws(() => parseArgs(installationArgs('/Applications/xl153.app', userInstalledApp)), /exactly match/);
  assert.throws(() => parseArgs(['--app','/Applications/Other.app',
    '--data-dir','/tmp/profile','--expected-version','1.0.0.2',
    '--artifacts-dir','/tmp/evidence','--installed-test-app=/Applications/Other.app']), /exactly match/);
  assert.throws(() => parseArgs(['--allow-local-adhoc=true']), /does not take a value/);
  assert.equal(isWithin('/tmp/test', '/tmp/test/app'), true);
  assert.equal(isWithin('/tmp/test', '/tmp/test-other/app'), false);
  assert.equal(isWithin('/tmp/test', '/tmp/test'), false);
  assert.deepEqual(config.browserArgs, ['--disable-gpu']);
  const root = await fsp.realpath(await fsp.mkdtemp(path.join(os.tmpdir(), 'xenon-real-test-self-')));
  try {
    const fakeHomeApp = path.join(root, 'Applications', 'xl153.app');
    const environmentProbe = `import assert from 'node:assert/strict';
      import {parseArgs} from ${JSON.stringify(import.meta.url)};
      assert.throws(() => parseArgs(${JSON.stringify(installationArgs(fakeHomeApp, fakeHomeApp))}), /exactly match/);
      const result = parseArgs(${JSON.stringify(installationArgs('~/Applications/xl153.app', '~/Applications/xl153.app'))});
      console.log(result.installedTestApp);`;
    const probe = await runFile(process.execPath, ['--input-type=module', '-e', environmentProbe],
      {env:{...process.env, HOME:root}});
    assert.equal(probe.stdout.trim(), userInstalledApp);
    await fsp.mkdir(path.join(root, 'a'));
    await fsp.mkdir(path.join(root, 'b'));
    const disposable = path.join(root, 'disposable.app');
    await fsp.mkdir(disposable);
    const registrationOptions = {app:disposable, artifactsDir:path.join(root, 'evidence')};
    const registrationEvents = [];
    const registrationCommands = [];
    const recordRegistration = (kind, details) => registrationEvents.push({kind, ...details});
    const registrationDeps = {isAlive:() => false, listBundleProcesses:async () => [],
      runCommand:async (command, args) => {
        registrationCommands.push({command, args});
        return {stdout:'', stderr:''};
      }};
    const completeCleanup = {knownPids:[12345], survivingPids:[]};
    await unregisterDiscardableTestApp(registrationOptions, completeCleanup,
      recordRegistration, registrationDeps);
    assert.deepEqual(registrationCommands, [{command:launchServicesTool,
      args:['-u', await fsp.realpath(disposable)]}]);
    assert.equal(registrationEvents.at(-1).kind, 'launch-services-unregistered');
    registrationCommands.length = 0;
    for (const installedApp of installedAppPaths) {
      await unregisterDiscardableTestApp({...registrationOptions, app:installedApp,
        installedTestApp:installedApp}, completeCleanup, recordRegistration, registrationDeps);
      assert.equal(registrationEvents.at(-1).reason, 'actual-installed-application');
      await unregisterDiscardableTestApp({...registrationOptions, app:installedApp},
        completeCleanup, recordRegistration, registrationDeps);
      assert.equal(registrationEvents.at(-1).reason, 'actual-installed-application');
    }
    await unregisterDiscardableTestApp(registrationOptions, undefined, recordRegistration, registrationDeps);
    assert.equal(registrationEvents.at(-1).reason, 'no-confirmed-test-process-cleanup');
    await assert.rejects(unregisterDiscardableTestApp(registrationOptions,
      {...completeCleanup, survivingPids:[12345]}, recordRegistration, registrationDeps), /still running/);
    await assert.rejects(unregisterDiscardableTestApp(registrationOptions, completeCleanup,
      recordRegistration, {...registrationDeps, isAlive:() => true}), /still running/);
    await assert.rejects(unregisterDiscardableTestApp(registrationOptions, completeCleanup,
      recordRegistration, {...registrationDeps, listBundleProcesses:async () => [12346]}), /bundle processes are running/);
    assert.equal(registrationCommands.length, 0);
    await assert.rejects(unregisterDiscardableTestApp(registrationOptions, completeCleanup,
      recordRegistration, {...registrationDeps, runCommand:async () => { throw new Error('Injected lsregister failure'); }}), /Injected lsregister failure/);
    assert.equal(registrationEvents.at(-1).kind, 'launch-services-unregister-failed');
    assert.equal(registrationEvents.at(-1).error, 'Injected lsregister failure');
    for (const directory of ['a','b']) {
      await fsp.writeFile(path.join(root,directory,'file'), 'abc', {mode:0o755});
      await fsp.symlink('file', path.join(root,directory,'link'));
    }
    const expected = await snapshotTree(path.join(root,'a'));
    assert.equal(expected.file.sha256, 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    assert.equal(compareTrees(expected, await snapshotTree(path.join(root,'b'))).length, 0);
    await fsp.chmod(path.join(root,'b','file'), 0o644);
    assert.deepEqual(compareTrees(expected, await snapshotTree(path.join(root,'b'))).map(item => item.path), ['file']);
    await fsp.writeFile(path.join(root,'b','file'), 'changed');
    assert.equal(compareTrees(expected, await snapshotTree(path.join(root,'b'))).length, 1);
    if (process.platform === 'darwin') {
      const version = '1.0.0.2';
      const plist = `<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>
        <key>CFBundleShortVersionString</key><string>${version}</string>
        <key>CFBundleExecutable</key><string>test</string></dict></plist>`;
      const patch = path.join(root, 'patch.zip');
      const full = path.join(root, 'package.pkg');
      await fsp.writeFile(patch, 'stage-discovery-test-patch');
      await fsp.writeFile(full, 'stage-discovery-test-full');
      {
        const mode = 'diff';
        const temporary = path.join(root, mode);
        const attempt = 1;
        const stageName = 'prepared';
        const app = path.join(temporary, 'download', stageName, 'test.app');
        await fsp.mkdir(path.join(app, 'Contents'), {recursive:true});
        await fsp.writeFile(path.join(app, 'Contents', 'Info.plist'), plist);
        await fsp.symlink('Contents/Info.plist', path.join(app, 'inside-link'));
        const localOptions = {mode, app:'/independent/test.app', expectedVersion:version, patch, package:full};
        const selected = patch;
        const archive = path.join(temporary, 'download', `${attempt}_update_patch.zip`);
        await fsp.copyFile(selected, archive);
        const ready = {is_diff:true, target_version:version,
          diff_package:{sha256:await sha256(patch)}};
        assert.equal((await locateStagedApp(temporary, localOptions, ready)).app, await fsp.realpath(app));
        await fsp.writeFile(archive, 'wrong-package');
        await assert.rejects(locateStagedApp(temporary, localOptions, ready), /SHA256/);
        await fsp.copyFile(selected, archive);
        await assert.rejects(locateStagedApp(temporary, localOptions,
          {...ready, target_version:'1.0.0.3'}), /version/);
        await fsp.symlink(patch, path.join(app, 'escaping-link'));
        await assert.rejects(validateTestBundle(app, temporary), /symlink escapes/);
        await fsp.unlink(path.join(app, 'escaping-link'));
        await fsp.link(patch, path.join(app, 'hard-link'));
        await assert.rejects(validateTestBundle(app, temporary), /hard-linked/);
        await fsp.unlink(path.join(app, 'hard-link'));
        await fsp.link(path.join(app, 'inside-link'), path.join(root, 'shared-symlink'));
        await assert.rejects(validateTestBundle(app, temporary), /hard-linked/);
        await fsp.unlink(path.join(root, 'shared-symlink'));
        await assert.rejects(validateTestBundle(app, path.join(root, 'a')), /inside this test/);
        await assert.rejects(validateTestBundle(app, path.join(root, 'a'), app), /inside this test/);
        const alias = path.join(temporary, 'alias.app');
        await fsp.symlink(app, alias);
        await assert.rejects(validateTestBundle(alias, temporary), /real application directory/);
        await fsp.unlink(alias);
        const duplicate = path.join(temporary, 'second-download', stageName, 'test.app');
        await fsp.mkdir(duplicate, {recursive:true});
        await assert.rejects(locateStagedApp(temporary, localOptions, ready), /2 staged app candidates/);
      }
      for (const mode of ['full', 'fallback']) {
        const temporary = path.join(root, mode);
        const options = {mode, app:'/independent/test.app', expectedVersion:version, patch, package:full};
        const ready = {is_diff:false, is_installer_package:true, target_version:version,
          full_package:{sha256:await sha256(full), size:(await fsp.stat(full)).size}};
        const archive = path.join(temporary, 'download', `${mode === 'fallback' ? 2 : 1}_update_package.pkg`);
        await fsp.mkdir(path.dirname(archive), {recursive:true});
        await fsp.copyFile(full, archive);
        assert.equal((await locateDownloadedInstaller(temporary, options, ready)).archive, archive);
        await assert.rejects(locateStagedApp(temporary, options, ready), /only supported for differential/);
        await assert.rejects(locateDownloadedInstaller(temporary, options, {...ready, is_installer_package:false}), /expected installer/);
        await assert.rejects(locateDownloadedInstaller(temporary, options, {...ready, target_version:'1.0.0.3'}), /expected installer/);
        await fsp.writeFile(archive, 'corrupt PKG');
        await assert.rejects(locateDownloadedInstaller(temporary, options, ready), /SHA256/);
        await fsp.unlink(archive);
        await fsp.symlink(full, archive);
        await assert.rejects(locateDownloadedInstaller(temporary, options, ready), /independent file/);
        await fsp.unlink(archive);
        await fsp.link(full, archive);
        await assert.rejects(locateDownloadedInstaller(temporary, options, ready), /independent file/);
        await fsp.unlink(archive);
        await fsp.copyFile(full, archive);
        const duplicate = path.join(temporary, 'second-download', path.basename(archive));
        await fsp.mkdir(path.dirname(duplicate));
        await fsp.copyFile(full, duplicate);
        await assert.rejects(locateDownloadedInstaller(temporary, options, ready), /2 downloaded PKG/);
        const feed = await startFeed(options, '1.0.0.1', () => {});
        try {
          assert.match(feed.server.manifest.full_package.url, /\/package\.pkg$/);
          assert.equal(feed.server.manifest.full_package.sha256, ready.full_package.sha256);
          if (mode === 'fallback') assert.notEqual(feed.server.manifest.diff_package.sha256, await sha256(patch));
          const response = await fetch(feed.server.manifest.full_package.url);
          assert.equal(response.status, 200);
          assert.deepEqual(Buffer.from(await response.arrayBuffer()), await fsp.readFile(full));
          assert.equal((await fetch(feed.server.manifest.full_package.url.replace('.pkg', '.zip'))).status, 404);
        } finally {
          feed.server.closeAllConnections();
          await new Promise(resolve => feed.server.close(resolve));
        }
      }
      const signedApp = path.join(root, 'signed-fixture.app');
      await fsp.mkdir(path.join(signedApp, 'Contents', 'MacOS'), {recursive:true});
      await fsp.writeFile(path.join(signedApp, 'Contents', 'Info.plist'), plist.replace('</dict>',
        '<key>CFBundleIdentifier</key><string>org.xenon.real-update-self-test</string><key>CFBundlePackageType</key><string>APPL</string></dict>'));
      await fsp.copyFile('/usr/bin/true', path.join(signedApp, 'Contents', 'MacOS', 'test'));
      await runFile('/usr/bin/codesign', ['--force', '--sign', '-', '--timestamp=none', signedApp]);
      await runFile('/usr/bin/xattr', ['-w', 'com.apple.quarantine', '0081;0;XenonSelfTest;', signedApp]);
      const readOnlyResource = path.join(signedApp, 'Contents', 'Info.plist');
      await runFile('/usr/bin/xattr', ['-w', 'com.apple.quarantine', '0081;0;XenonSelfTest;', readOnlyResource]);
      await fsp.chmod(readOnlyResource, 0o444);
      await runFile('/usr/bin/xattr', ['-w', 'com.xenon.test-keep', 'preserved', signedApp]);
      const actions = [];
      await clearLocalQuarantine(signedApp, root, 'installed', {artifactsDir:root},
        (kind, details) => actions.push({kind,...details}));
      const mutation = actions.find(event => event.kind === 'local-adhoc-quarantine-cleared');
      assert.equal(mutation.command, '/usr/bin/xattr');
      assert.equal(mutation.beforeQuarantine.length, 2);
      assert.deepEqual(mutation.afterQuarantine, []);
      assert.equal(mutation.executed, true);
      assert.equal((await fsp.stat(readOnlyResource)).mode & 0o777, 0o444);
      assert.equal((await runFile('/usr/bin/xattr', ['-p', 'com.xenon.test-keep', signedApp])).stdout.trim(), 'preserved');
      await clearLocalQuarantine(signedApp, root, 'installed', {artifactsDir:root},
        (kind, details) => actions.push({kind,...details}));
      assert.equal(actions.at(-1).executed, false);
      await runFile('/usr/bin/xattr', ['-w', 'com.apple.quarantine', '0081;0;XenonSelfTest;', signedApp]);
      await assert.rejects(clearLocalQuarantine(signedApp, root, 'prepared', {artifactsDir:root},
        () => {}, {}, signedApp, () => { throw new Error('Original browser exited'); }), /Original browser exited/);
      assert.equal((await runFile('/usr/bin/xattr', ['-p', 'com.apple.quarantine', signedApp])).stdout.trim(), '0081;0;XenonSelfTest;');
    }
  } finally { await fsp.rm(root, {recursive:true, force:true}); }
  if (process.platform === 'darwin') {
    const pauseFixture = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'],
      {stdio:'ignore'});
    const pauseEvents = [];
    const recordPause = (kind, details) => pauseEvents.push({kind,...details});
    try {
      assert.equal(await withStoppedProcess(pauseFixture.pid, recordPause, async () => {
        assert.ok((await processState(pauseFixture.pid)).includes('T'));
        return 42;
      }), 42);
      await assert.rejects(withStoppedProcess(pauseFixture.pid, recordPause, async () => {
        assert.ok((await processState(pauseFixture.pid)).includes('T'));
        throw new Error('Injected verification failure');
      }), /Injected verification failure/);
      assert.ok(!(await processState(pauseFixture.pid)).includes('T'));
      assert.equal(pauseEvents.filter(event => event.kind === 'native-helper-resumed').length, 2);
    } finally {
      if (alive(pauseFixture.pid)) {
        // Test cleanup remains safe even if a regression broke the helper.
        pauseFixture.kill('SIGCONT');
        pauseFixture.kill('SIGTERM');
      }
      await new Promise(resolve => {
        if (pauseFixture.exitCode !== null || pauseFixture.signalCode !== null) resolve();
        else pauseFixture.once('exit', resolve);
      });
    }
    // Real wait-status regression: a sibling process has no parent/child
    // relationship with the Python monitor, just like LaunchServices restart.
    for (const crash of [false, true]) {
      const child = spawn(process.execPath, ['-e', crash
        ? "process.stdin.once('data', () => process.kill(process.pid, 'SIGTERM'))"
        : "process.stdin.once('data', () => process.exit(0))"], {stdio:['pipe','ignore','ignore']});
      const monitor = watchProcessExit(child.pid, () => {});
      try {
        await monitor.ready;
        child.stdin.end('exit');
        const result = await waitFor(() => monitor.result, 10000, 'self-test process exit');
        if (crash) assert.throws(() => assertNormalExit(result, 'Browser'), /did not exit normally/);
        else assertNormalExit(result, 'Browser');
      } finally { monitor.stop(); if (alive(child.pid)) child.kill('SIGKILL'); }
    }
  }
  console.log('Self-test passed (installed-path authorization, scoped LaunchServices cleanup, diff staging, PKG SHA/size and path binding, PKG feed HTTP, handoff-only result/error/timeout, normal/crash exit detection); no browser or Installer launched.');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const options = parseArgs(process.argv.slice(2));
    if (options.help) console.log(usage);
    else if (options['self-test']) await selfTest();
    else await runRealUpdate(options);
  } catch (error) {
    console.error(`FAIL: ${error.message}`);
    process.exitCode = 1;
  }
}
