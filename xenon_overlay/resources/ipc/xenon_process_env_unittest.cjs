// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const snapshot = vm.runInNewContext(fs.readFileSync(
  path.join(__dirname, 'main/process_env.js'), 'utf8') + ';processEnvironmentSnapshot');

test('Windows environment aliases preserve enumeration, values and mutations', () => {
  const input = {Path:'C:/fixture', EMPTY:'', 中文:'路径/😀'};
  Object.defineProperty(input, '__proto__', {value:'own value', writable:true, enumerable:true, configurable:true});
  const env = snapshot(input, true);
  assert.equal(env.PATH, 'C:/fixture');
  assert.equal(env.path, 'C:/fixture');
  assert.equal(env.empty, '');
  assert.equal(env.中文, '路径/😀');
  assert.equal(env.__PROTO__, 'own value');
  assert.equal(Object.hasOwn(env, 'pAtH'), true);
  assert.equal('pAtH' in env, true);
  assert.deepEqual(Object.keys(env), ['Path','EMPTY','中文','__proto__']);
  env.PATH = 'updated';
  assert.equal(env.Path, 'updated');
  assert.equal(Object.keys(env).includes('PATH'), false);
  Object.defineProperty(env, 'pAtH', {value:'defined', writable:true, enumerable:true, configurable:true});
  assert.equal(env.PATH, 'defined');
  delete env.path;
  assert.equal(Object.hasOwn(env, 'Path'), false);
  env.paTH = 'recreated';
  assert.equal(env.PATH, 'recreated');
  assert.equal(Object.keys(env).includes('paTH'), true);
  assert.throws(() => Object.defineProperty(env, 'locked', {value:'no'}),
    {code:'ERR_INVALID_OBJECT_DEFINE_PROPERTY'});
});

test('POSIX and Windows Worker snapshots remain case-sensitive', () => {
  const input = {Path:'one', PATH:'two'};
  const env = snapshot(input, false);
  assert.equal(env, input);
  assert.equal(env.path, undefined);
  assert.equal(env.Path, 'one');
  assert.equal(env.PATH, 'two');
  delete env.PATH;
  assert.equal(env.Path, 'one');
});
