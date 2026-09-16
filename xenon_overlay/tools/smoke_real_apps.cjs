// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Usage: node smoke_real_apps.cjs --out out/Release_64 [--app all|th|ple]
// This invokes real native code. Run only against a trusted packaged payload.
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const args = process.argv.slice(2);
const value = (name, fallback) => {
  const index = args.indexOf(name);
  if (index === -1) return fallback;
  if (!args[index + 1] || args[index + 1].startsWith('--')) throw new Error(`${name} requires a value`);
  return args[index + 1];
};
const root = path.resolve(value('--out', 'out/Release_64'));
const executable = path.resolve(value('--native', path.join(root, 'ipc_main_container_unittests.exe')));
const application = value('--app', 'all');
if (!['all', 'th', 'ple'].includes(application)) throw new Error('--app must be all, th, or ple');
const observeMs = Number(value('--observe-ms', '1000'));
if (!Number.isInteger(observeMs) || observeMs < 0 || observeMs > 10000) {
  throw new Error('--observe-ms must be an integer from 0 through 10000');
}
const output = path.resolve(value('--results', path.join(root, '..',
    `real-app-smoke-${new Date().toISOString().replace(/[:.]/g, '-')}`)));
fs.mkdirSync(output, {recursive: true});
const applications = [['th', 'ThunderMain'], ['ple', 'PlayerMain']]
    .filter(([name]) => application === 'all' || name === application);
const summaries = [];
for (const [name, test] of applications) {
  const directory = path.join(output, name);
  const directories = Object.fromEntries(['profile', 'roaming', 'local', 'temp']
      .map(kind => [kind, path.join(directory, kind)]));
  for (const target of Object.values(directories)) fs.mkdirSync(target, {recursive: true});
  const reportPath = path.join(directory, 'report.json');
  const result = spawnSync(executable, [
    `--gtest_filter=XenonRealAppSmokeTest.${test}`, '--single-process-tests',
    '--test-launcher-jobs=1', '--test-launcher-retry-limit=0',
    '--enable-logging=stderr', '--log-level=0',
    `--xenon-app-smoke-root=${root}`,
    `--xenon-app-smoke-profile=${directories.profile}`,
    `--xenon-app-smoke-result=${reportPath}`,
    `--xenon-app-smoke-observe-ms=${observeMs}`,
  ], {
    windowsHide: true, encoding: 'utf8', timeout: 30000 + observeMs,
    maxBuffer: 64 * 1024 * 1024,
    env: {...process.env, APPDATA: directories.roaming, LOCALAPPDATA: directories.local,
      TEMP: directories.temp, TMP: directories.temp},
  });
  const log = `${result.stdout || ''}\n${result.stderr || ''}`;
  fs.writeFileSync(path.join(directory, 'process.log'), log);
  let report = null;
  if (fs.existsSync(reportPath)) report = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
  const readyErrors = log.split(/\r?\n/).filter(line =>
    /Electron app ready (?:listener|microtask) failed|ipcMain[^\n]*(?:unhandledRejection|uncaughtException)/.test(line));
  const summary = {application: name, exitCode: result.status,
    processError: result.error?.message || null, report: reportPath,
    log: path.join(directory, 'process.log'),
    mainEvaluated: report?.main_evaluated === true,
    observationComplete: report?.phase === 'observation-complete',
    readyErrors: readyErrors.map(line => line.slice(0, 8192)),
    businessStartupVerified: false};
  summary.smokePassed = result.status === 0 && summary.mainEvaluated &&
      summary.observationComplete && !readyErrors.length;
  summaries.push(summary);
  console.log(JSON.stringify(summary));
}
fs.writeFileSync(path.join(output, 'summary.json'), JSON.stringify(summaries, null, 2));
if (summaries.some(summary => !summary.smokePassed)) process.exitCode = 1;
