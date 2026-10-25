// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Real Windows crash tests. Only processes launched by this script are stopped.
// Node.js 22+: node test_crash_upload.mjs --browser ABS.exe --server ABS.exe
//   --output ABS_DIRECTORY [--database-tool ABS.exe]
import {spawn, execFile} from 'node:child_process';
import {createWriteStream} from 'node:fs';
import {mkdir, readFile, readdir, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:net';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {promisify} from 'node:util';
import {fileURLToPath} from 'node:url';

const run = promisify(execFile);
const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
const options = {};
for (let i = 2; isMain && i < process.argv.length; i += 2) {
  const key = process.argv[i];
  if (!['--browser', '--server', '--output', '--database-tool', '--scenario', '--format', '--window-mode'].includes(key) || !process.argv[i + 1]) {
    throw new Error('Usage: node test_crash_upload.mjs --browser ABS.exe --server ABS.exe --output ABS_DIRECTORY [--database-tool ABS.exe] [--scenario renderer|browser|gpu] [--format multipart|binary] [--window-mode windowed|headless]');
  }
  options[key.slice(2)] = process.argv[i + 1];
}
const formats = options.format ? [options.format] : ['multipart', 'binary'];
const scenarios = [
  {name: 'renderer', url: 'chrome://crash', ptype: 'renderer'},
  {name: 'browser', url: 'chrome://inducebrowsercrashforrealz/', ptype: 'browser'},
  {name: 'gpu', url: 'chrome://gpucrash', ptype: 'gpu-process'},
].filter(scenario => !options.scenario || scenario.name === options.scenario);
if (!scenarios.length || formats.some(format => !['multipart', 'binary'].includes(format))) {
  throw new Error('Invalid --scenario or --format');
}
const windowMode = options['window-mode'] ?? 'windowed';
if (!['windowed', 'headless'].includes(windowMode)) {
  throw new Error('Invalid --window-mode');
}
if (options['database-tool'] && !path.isAbsolute(options['database-tool'])) {
  throw new Error('--database-tool must be an absolute path');
}
for (const key of isMain ? ['browser', 'server', 'output'] : []) {
  if (!options[key] || !path.isAbsolute(options[key])) {
    throw new Error(`--${key} must be an absolute path`);
  }
}
if (isMain && (process.platform !== 'win32' || typeof WebSocket === 'undefined')) {
  throw new Error('These tests require Windows and Node.js 22 or later');
}

const results = {
  started_at: new Date().toISOString(), browser: options.browser,
  server: options.server, output: options.output, window_mode: windowMode,
  tests: [], cleanup: [],
};
const active = new Set();
const logStreams = [];
let stopping = false;
let outputReady = false;

async function save() {
  if (!outputReady) return;
  await writeFile(path.join(options.output, 'results.json'), JSON.stringify(results, null, 2));
}

export async function openLogFile(logFile) {
  const stream = createWriteStream(logFile, {flags: 'ax'});
  const log = {stream, file: logFile, error: null};
  // Keep the listener installed after open: asynchronous disk errors must not
  // escape the runner's finally block as unhandled EventEmitter errors.
  stream.on('error', error => { log.error = error; });
  await new Promise((resolve, reject) => {
    stream.once('open', resolve);
    stream.once('error', reject);
  });
  return log;
}

export async function finishLog(log) {
  if (!log.stream.closed) {
    await new Promise(resolve => {
      log.stream.once('close', resolve);
      log.stream.end();
    });
  }
  if (log.error) throw new Error(`Log ${log.file}: ${log.error.message}`);
}

async function launch(executable, args, logFile, env = process.env) {
  const log = await openLogFile(logFile);
  logStreams.push(log);
  const child = spawn(executable, args, {
    env, windowsHide: true, cwd: path.dirname(executable),
    detached: executable === options.browser,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stdout.pipe(log.stream, {end: false});
  child.stderr.pipe(log.stream, {end: false});
  const item = {child, pid: child.pid, descendants: [], executable, logFile, log};
  child.on('error', error => { item.error = String(error); });
  child.on('exit', (code, signal) => { item.exit = {code, signal}; });
  active.add(item);
  try {
    // Record identity before waiting for DevTools. Cleanup must also work when
    // startup fails, and must never terminate a reused PID.
    const snapshot = await processSnapshot();
    const root = snapshot.find(process => process.pid === item.pid);
    if (!root) throw new Error(`Process ${item.pid} exited before identity could be recorded`);
    item.created = root.created;
    item.descendants = ownedProcesses(item, snapshot).filter(process => process.pid !== item.pid);
  } catch (error) {
    await stop(item);
    throw error;
  }
  return item;
}

function assertRunning(item) {
  if (item.log.error) throw new Error(`Log ${item.logFile}: ${item.log.error.message}`);
  if (item.error || item.exit) {
    throw new Error(`Process ${item.pid} stopped: ${item.error || JSON.stringify(item.exit)}; see ${item.logFile}`);
  }
}

// Read only the structures needed to identify a report. Layouts are defined in
// Crashpad's minidump_extensions.h and Windows' MINIDUMP_MISC_INFO. Every RVA,
// descriptor and count is bounded before use; byte searches could mistake
// unrelated memory in the dump for actual crash annotations.
export function readMinidumpIdentity(bytes) {
  function bounds(offset, length) {
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) ||
        offset < 0 || length < 0 || offset + length > bytes.length) {
      throw new Error('Minidump structure exceeds file bounds');
    }
  }
  function u32(offset) { bounds(offset, 4); return bytes.readUInt32LE(offset); }
  function location(offset) {
    const size = u32(offset);
    const rva = u32(offset + 4);
    bounds(rva, size);
    return {rva, size};
  }
  function array(descriptor, width) {
    if (!descriptor.size) return [];
    if (descriptor.size < 4) throw new Error('Truncated minidump array');
    const count = u32(descriptor.rva);
    if (count > (descriptor.size - 4) / width) throw new Error('Invalid minidump array count');
    return Array.from({length: count}, (_, i) => descriptor.rva + 4 + i * width);
  }
  function string(rva) {
    const size = u32(rva);
    bounds(rva + 4, size);
    return bytes.toString('utf8', rva + 4, rva + 4 + size);
  }
  const types = new Set();
  function annotation(name, value) { if (name === 'ptype') types.add(value); }
  function dictionary(descriptor) {
    for (const entry of array(descriptor, 8)) annotation(string(u32(entry)), string(u32(entry + 4)));
  }
  bounds(0, 32);
  if (bytes.toString('ascii', 0, 4) !== 'MDMP') throw new Error('Invalid minidump signature');
  const count = u32(8);
  const directory = u32(12);
  bounds(directory, count * 12);
  let pid = null;
  for (let i = 0; i < count; i++) {
    const entry = directory + i * 12;
    const type = u32(entry);
    const stream = location(entry + 4);
    if (type === 15 && stream.size >= 12 && (u32(stream.rva + 4) & 1)) {
      pid = u32(stream.rva + 8);
    }
    if (type !== 0x43500001) continue;
    if (stream.size < 52 || u32(stream.rva) < 1) throw new Error('Unsupported Crashpad info stream');
    dictionary(location(stream.rva + 36));
    for (const link of array(location(stream.rva + 44), 12)) {
      const module = location(link + 4);
      if (module.size < 20 || u32(module.rva) < 1) throw new Error('Unsupported Crashpad module info');
      dictionary(location(module.rva + 12));
      if (module.size >= 28) {
        for (const object of array(location(module.rva + 20), 12)) {
          if (bytes.readUInt16LE(object + 4) === 1) {
            annotation(string(u32(object)), string(u32(object + 8)));
          }
        }
      }
    }
  }
  if (types.size !== 1 || !pid) throw new Error('Missing or conflicting minidump process identity');
  return {pid, ptype: [...types][0]};
}

export function matchesReport(report, baseline, format, ptype, expectedPids) {
  return !baseline.has(report.report_id) && report.metadata.format === format &&
    report.identity?.ptype === ptype && expectedPids.includes(report.identity.pid) &&
    (format !== 'multipart' || report.ptype === ptype);
}

export async function processSnapshot() {
  const script = `ConvertTo-Json -InputObject @(Get-CimInstance Win32_Process | ForEach-Object { @{ pid = $_.ProcessId; parent_pid = $_.ParentProcessId; created = $_.CreationDate.ToUniversalTime().ToString('o'); command_line = $_.CommandLine } }) -Compress`;
  const {stdout} = await run('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], {windowsHide: true, timeout: 15000, maxBuffer: 4 * 1024 * 1024});
  return JSON.parse(stdout || '[]');
}

export function ownedProcesses(item, snapshot) {
  const known = [item, ...item.descendants];
  const owned = snapshot.filter(process => known.some(identity =>
    process.pid === identity.pid && process.created === identity.created));
  for (let i = 0; i < owned.length; i++) {
    const parent = owned[i];
    for (const process of snapshot) {
      if (process.parent_pid === parent.pid && process.created >= parent.created &&
          !owned.some(entry => entry.pid === process.pid)) owned.push(process);
    }
  }
  return owned.sort((a, b) => Number(b.pid === item.pid) - Number(a.pid === item.pid));
}

function rememberProcesses(item, processes) {
  for (const process of processes) {
    if (process.pid !== item.pid && !item.descendants.some(identity =>
      identity.pid === process.pid && identity.created === process.created)) {
      item.descendants.push(process);
    }
  }
}

async function rememberDescendants(item) {
  const owned = ownedProcesses(item, await processSnapshot());
  rememberProcesses(item, owned);
  return owned;
}

// The process handle is opened before checking its creation time and remains
// open until termination. A PID reused between enumeration and cleanup cannot
// select a different process, unlike a later taskkill /PID command.
export async function terminateProcesses(processes) {
  const identities = Buffer.from(JSON.stringify(processes.map(({pid, created}) => ({pid, created}))), 'utf8').toString('base64');
  const script = `
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class CrashTestCleanup {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessTimes(IntPtr handle, out long created, out long exited, out long kernel, out long user);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateProcess(IntPtr handle, uint code);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  public static void Stop(uint pid, string expected) {
    IntPtr handle = OpenProcess(0x00101001, false, pid);
    if (handle == IntPtr.Zero) {
      int error = Marshal.GetLastWin32Error();
      if (error == 87) return;
      throw new Win32Exception(error);
    }
    try {
      long created, exited, kernel, user;
      if (!GetProcessTimes(handle, out created, out exited, out kernel, out user)) throw new Win32Exception();
      long expectedTime = DateTime.Parse(expected, null, System.Globalization.DateTimeStyles.RoundtripKind).ToUniversalTime().ToFileTimeUtc();
      // CIM CreationDate has microsecond precision; FILETIME has 100 ns precision.
      if (Math.Abs(created - expectedTime) >= 10 || WaitForSingleObject(handle, 0) == 0) return;
      if (!TerminateProcess(handle, 1)) throw new Win32Exception();
      if (WaitForSingleObject(handle, 5000) != 0) throw new Exception("Process did not exit: " + pid);
    } finally { CloseHandle(handle); }
  }
}
'@
$identities = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('${identities}')) | ConvertFrom-Json
$failures = @()
foreach ($identity in $identities) {
  try { [CrashTestCleanup]::Stop($identity.pid, $identity.created) }
  catch { $failures += "PID $($identity.pid): $_" }
}
if ($failures.Count) { throw ($failures -join '; ') }
`;
  await run('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], {windowsHide: true, timeout: 30000});
}

export async function cleanupTrackedProcess(item, snapshot, terminate, maxAttempts = 2) {
  const outcome = {pid: item.pid, created: item.created, success: false, attempts: 0, errors: [], remaining: []};
  for (let attempt = 0; attempt < maxAttempts; attempt++) {
    outcome.attempts++;
    try {
      const before = await snapshot();
      if (!item.created && before.some(process => process.pid === item.pid)) {
        throw new Error('Cannot clean up a process without its recorded creation time');
      }
      const owned = ownedProcesses(item, before);
      rememberProcesses(item, owned);
      outcome.remaining = owned;
      if (owned.length) await terminate(owned);
      outcome.remaining = ownedProcesses(item, await snapshot());
      rememberProcesses(item, outcome.remaining);
      if (!outcome.remaining.length) {
        outcome.success = true;
        return outcome;
      }
    } catch (error) { outcome.errors.push(String(error)); }
  }
  return outcome;
}

async function stop(item) {
  if (!active.has(item)) return;
  const outcome = await cleanupTrackedProcess(item, processSnapshot, terminateProcesses);
  const previous = results.cleanup.find(entry => entry.pid === item.pid && entry.created === item.created);
  if (previous) {
    outcome.attempts += previous.attempts;
    outcome.errors.unshift(...previous.errors);
    Object.assign(previous, outcome);
  } else results.cleanup.push(outcome);
  if (outcome.success) active.delete(item);
}

async function freePort() {
  const listener = createServer();
  await new Promise((resolve, reject) => {
    listener.once('error', reject);
    listener.listen(0, '127.0.0.1', resolve);
  });
  const port = listener.address().port;
  await new Promise(resolve => listener.close(resolve));
  return port;
}

async function poll(fn, timeoutMs, description) {
  const end = Date.now() + timeoutMs;
  while (!stopping && Date.now() < end) {
    const result = await fn();
    if (result) return result;
    await delay(250);
  }
  throw new Error(`Timed out waiting for ${description}`);
}

async function connectCdp(url) {
  const socket = new WebSocket(url);
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => { socket.close(); reject(new Error('CDP connection timeout')); }, 10000);
    socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, {once: true});
    socket.addEventListener('error', () => { clearTimeout(timer); reject(new Error('CDP connection failed')); }, {once: true});
  });
  let sequence = 0;
  const pending = new Map();
  socket.addEventListener('message', event => {
    const response = JSON.parse(event.data);
    const request = pending.get(response.id);
    if (request) {
      clearTimeout(request.timer);
      pending.delete(response.id);
      if (response.error) request.reject(new Error(JSON.stringify(response.error)));
      else request.resolve(response.result);
    }
  });
  socket.addEventListener('close', () => {
    for (const request of pending.values()) {
      clearTimeout(request.timer);
      request.reject(new Error('CDP connection closed (expected for a browser crash)'));
    }
    pending.clear();
  });
  return {
    close: () => socket.close(),
    send: (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++sequence;
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(new Error(`CDP ${method} timed out; crash navigation may not reply`));
      }, 5000);
      pending.set(id, {resolve, reject, timer});
      socket.send(JSON.stringify({id, method, params}));
    }),
  };
}

async function reportsFor(testCase) {
  let names;
  try { names = await readdir(path.join(options.output, 'service', 'crashes')); }
  catch (error) { if (error.code === 'ENOENT') return []; throw error; }
  const found = [];
  for (const name of names.filter(name => /^[a-f0-9]{32}$/.test(name))) {
    const directory = path.join(options.output, 'service', 'crashes', name);
    const metadata = JSON.parse(await readFile(path.join(directory, 'metadata.json'), 'utf8'));
    if (!metadata.query.some(([key, value]) => key === 'test_case' && value === testCase)) continue;
    const dump = path.join(directory, 'minidump.dmp');
    const size = (await stat(dump)).size;
    const report = {report_id: name, directory, dump, size,
      ptype: metadata.annotations.ptype ?? null, metadata};
    try {
      if (size > 256 * 1024 * 1024) throw new Error('Minidump exceeds runner parsing limit');
      const bytes = await readFile(dump);
      report.magic = bytes.toString('ascii', 0, 4);
      report.identity = readMinidumpIdentity(bytes);
    } catch (error) { report.identity_error = String(error); }
    found.push(report);
  }
  return found;
}

async function databaseInfo(crashDirectory, profile) {
  if (!options['database-tool']) return null;
  const {stdout, stderr} = await run(options['database-tool'], [
    `--database=${crashDirectory}`, '--show-uploads-enabled',
    '--show-pending-reports', '--show-completed-reports', '--show-all-report-info', '--utc',
  ], {windowsHide: true, timeout: 10000});
  const info = stdout + stderr;
  await writeFile(path.join(profile, 'database.log'), info);
  return info;
}

async function testScenario(service, port, format, scenario) {
  const name = `${format}-${scenario.name}`;
  const profile = path.join(options.output, name);
  await mkdir(profile);
  const crashDirectory = path.join(profile, 'Crashpad');
  await mkdir(crashDirectory);
  const env = {...process.env,
    CHROME_CRASHPAD_SERVER_URL: `http://127.0.0.1:${port}/api/v1/crashes?test_case=${name}`,
    XENON_CRASH_UPLOAD_ENABLED: '1', XENON_CRASH_UPLOAD_FORMAT: format,
    BREAKPAD_DUMP_LOCATION: crashDirectory,
  };
  delete env.CHROME_HEADLESS;
  delete env.CHROME_CRASHPAD_PIPE_NAME;
  const result = {name, format, url: scenario.url, expected_ptype: scenario.ptype,
    profile, crash_directory: crashDirectory, started_at: new Date().toISOString()};
  results.tests.push(result);
  console.log(`Starting ${name}: ${scenario.url}`);
  let browser;
  let cdp;
  try {
    browser = await launch(options.browser, [
      `--user-data-dir=${profile}`, '--remote-debugging-port=0',
      ...(windowMode === 'headless' ? ['--headless=new'] : []),
      '--force-fieldtrials=MetricsAndCrashSampling/InReportingSample',
      '--no-first-run', '--no-default-browser-check', 'about:blank',
    ], path.join(profile, 'browser.log'), env);
    result.browser_pid = browser.pid;
    const debugPort = await poll(async () => {
      assertRunning(browser);
      assertRunning(service);
      try {
        const contents = await readFile(path.join(profile, 'DevToolsActivePort'), 'utf8');
        const value = Number(contents.split(/\r?\n/)[0]);
        return value > 0 ? value : null;
      } catch (error) { if (error.code === 'ENOENT') return null; throw error; }
    }, 30000, 'isolated browser DevTools port');
    const tabs = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`, {signal: AbortSignal.timeout(5000)})).json();
    const target = tabs.find(tab => tab.type === 'page' && tab.url === 'about:blank');
    if (!target) throw new Error('Isolated browser did not expose its about:blank page');
    cdp = await connectCdp(target.webSocketDebuggerUrl);
    // Allow GPU startup and capture Crashpad's PID before crashing the browser.
    await delay(2500);
    const owned = await rememberDescendants(browser);
    const typeArgument = new RegExp(`(?:^|\\s)--type=["']?${scenario.ptype}["']?(?=\\s|$)`);
    result.expected_pids = scenario.name === 'browser' ? [browser.pid] :
      owned.filter(process => typeArgument.test(process.command_line ?? '')).map(process => process.pid);
    if (!result.expected_pids.length) throw new Error(`No owned ${scenario.ptype} process exists before crash`);
    result.spawned_descendants = [...browser.descendants];
    const baseline = new Set((await reportsFor(name)).map(report => report.report_id));
    result.baseline_report_ids = [...baseline];
    assertRunning(browser);
    result.triggered_at = new Date().toISOString();
    // Browser/GPU debug URLs require explicit address-bar navigation.
    try {
      result.navigation = await cdp.send('Page.navigate', {
        url: scenario.url, transitionType: 'address_bar',
      });
    }
    catch (error) { result.navigation = {message: String(error)}; }
    const reports = await poll(async () => {
      assertRunning(service);
      if (browser.log.error) throw new Error(`Browser log failed: ${browser.log.error.message}`);
      const received = await reportsFor(name);
      return received.some(report => matchesReport(report, baseline, format, scenario.ptype, result.expected_pids)) ? received : null;
    }, 60000, `${name} server report`);
    result.reports = reports;
    result.browser_alive_after_upload = !browser.exit && !browser.error;
    result.browser_exit = browser.exit ?? null;
    const matching = reports.filter(report => matchesReport(report, baseline, format, scenario.ptype, result.expected_pids));
    result.matched_report_ids = matching.map(report => report.report_id);
    if (scenario.name === 'browser' && result.browser_alive_after_upload) {
      await poll(() => browser.exit, 5000, 'intentional browser-process exit');
      result.browser_alive_after_upload = false;
      result.browser_exit = browser.exit;
    }
    if (scenario.name !== 'browser' && !result.browser_alive_after_upload) {
      throw new Error('Browser exited unexpectedly during a child-process crash');
    }
    if (options['database-tool']) {
      result.database_info = await poll(async () => {
        const info = await databaseInfo(crashDirectory, profile);
        return matching.some(report => info.includes(`Remote ID: ${report.report_id}`)) ? info : null;
      }, 10000, 'Crashpad database upload acknowledgement');
    }
    result.status = 'passed';
    console.log(`Passed ${name}: ${result.matched_report_ids.join(', ')}`);
  } catch (error) {
    result.status = 'failed';
    result.error = error.stack || String(error);
    result.reports = await reportsFor(name).catch(() => []);
    result.database_info = await databaseInfo(crashDirectory, profile).catch(error => String(error));
    if (browser) {
      result.browser_alive_after_upload = !browser.exit && !browser.error;
      result.browser_exit = browser.exit ?? null;
    }
    console.error(`Failed ${name}: ${error.message}`);
  } finally {
    cdp?.close();
    if (browser) await stop(browser);
    result.client_database_files = await readdir(crashDirectory, {recursive: true}).catch(() => []);
    result.finished_at = new Date().toISOString();
    await save();
  }
}

async function main() {
  for (const key of ['browser', 'server']) {
    if (!(await stat(options[key])).isFile()) throw new Error(`--${key} is not a file`);
  }
  if (options['database-tool'] && !(await stat(options['database-tool'])).isFile()) {
    throw new Error('--database-tool is not a file');
  }
  await mkdir(options.output, {recursive: true});
  // Refuse to reuse a service/profile tree: artifacts from a prior run cannot
  // become a false positive, and no existing profile is opened or overwritten.
  const serviceDirectory = path.join(options.output, 'service');
  await mkdir(serviceDirectory);
  outputReady = true;
  const config = path.join(serviceDirectory, 'xenon-update.json');
  const port = await freePort();
  results.port = port;
  const initialized = await run(options.server, ['--config', config, 'init', '--port', String(port)], {
    cwd: serviceDirectory, windowsHide: true, timeout: 30000,
  });
  await writeFile(path.join(serviceDirectory, 'init.log'), initialized.stdout + initialized.stderr);
  const service = await launch(options.server, ['--config', config, 'serve'], path.join(serviceDirectory, 'server.log'));
  results.server_pid = service.pid;
  await poll(async () => {
    assertRunning(service);
    try {
      const response = await fetch(`http://127.0.0.1:${port}/api/v1/crashes`, {signal: AbortSignal.timeout(1000)});
      return response.status === 405;
    } catch { return false; }
  }, 15000, 'local crash receiver');
  for (const format of formats) {
    for (const scenario of scenarios) {
      if (stopping) return;
      await testScenario(service, port, format, scenario);
    }
  }
}

if (isMain) {
  for (const signal of ['SIGINT', 'SIGTERM']) {
    process.on(signal, () => { stopping = true; });
  }
  try {
    await main();
  } catch (error) {
    results.error = error.stack || String(error);
    console.error(results.error);
  } finally {
    stopping = true;
    for (const item of [...active]) await stop(item);
    results.log_errors = [];
    for (const log of logStreams) {
      await finishLog(log).catch(error => results.log_errors.push(String(error)));
    }
    // A failed cleanup must remain visible and must not keep the runner alive
    // forever through its stdio handles. The retained PIDs are in results.json.
    for (const item of active) {
      item.child.stdout.destroy();
      item.child.stderr.destroy();
      item.child.unref();
    }
    results.finished_at = new Date().toISOString();
    results.status = !results.error && !results.log_errors.length &&
      results.cleanup.every(entry => entry.success) &&
      results.tests.length === formats.length * scenarios.length &&
      results.tests.every(test => test.status === 'passed') ? 'passed' : 'failed';
    await save().catch(error => {
      results.status = 'failed';
      console.error(`Cannot save results: ${error.message}`);
    });
    console.log(`Result: ${results.status}; ${path.join(options.output, 'results.json')}`);
    if (results.status !== 'passed') process.exitCode = 1;
  }
}
