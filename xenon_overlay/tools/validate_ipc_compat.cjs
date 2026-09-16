// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Usage: node validate_ipc_compat.cjs [--native out/Release_64/ipc_main_container_unittests.exe]
// Run after building native targets. Production objects are built separately.
const {readdirSync} = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

const args = process.argv.slice(2);
if (args.length && (args.length !== 2 || args[0] !== '--native')) {
  throw new Error('Usage: validate_ipc_compat.cjs [--native path/to/ipc_main_container_unittests]');
}
const directory = path.resolve(__dirname, '../resources/ipc');
const tests = readdirSync(directory).filter(name => name.endsWith('_unittest.cjs'))
    .sort().map(name => path.join(directory, name));
if (!tests.length) throw new Error('No IPC compatibility tests found');

function run(binary, options) {
  const result = spawnSync(binary, options, {stdio: 'inherit', windowsHide: true});
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}

console.log(`IPC compatibility: ${tests.length} JS suites; Node ${process.version}, V8 ${process.versions.v8}`);
run(process.execPath, ['--js-base-64', '--expose-gc', '--test', ...tests]);
if (args.length) {
  run(path.resolve(args[1]), ['--test-launcher-jobs=1', '--test-launcher-retry-limit=0']);
}
