// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import assert from 'node:assert/strict';
import test from 'node:test';
import {parseScreenLabel, fixtureRegion} from './local_screen_readonly_test.mjs';

test('screen labels bind only an explicit EnumDisplayDevices index', () => {
  assert.deepEqual(parseScreenLabel('screen:0:0'), {deviceIndex: 0, windowId: '0'});
  assert.deepEqual(parseScreenLabel('screen:3:-12'), {deviceIndex: 3, windowId: '-12'});
  for (const label of ['screen:-1:0', 'screen:1:0:s', 'window:1:0',
    'screen:1', ' screen:1:0', 'screen:1:0\n', 'screen:1.5:0',
    'screen:4294967296:0', 'screen:0:9223372036854775808']) {
    assert.throws(() => parseScreenLabel(label), label);
  }
});

test('physical fixture crop handles monitors left/above the primary display', () => {
  assert.deepEqual(fixtureRegion({left: -1920, top: -1080, right: 0, bottom: 0},
    {left: -1728, top: -972, right: -768, bottom: -432}),
  {left: .1, top: .1, right: .6, bottom: .6});
});

test('physical fixture crop refuses partial overlap and invalid monitor geometry', () => {
  const monitor = {left: 100, top: 200, right: 1100, bottom: 1000};
  for (const fixture of [
    {left: 0, top: 200, right: 600, bottom: 600},
    {left: 100, top: 200, right: 1200, bottom: 600},
    {left: 100, top: 200, right: 600, bottom: 1100},
    {left: 100, top: 200, right: 100, bottom: 600},
    {left: 100.5, top: 200, right: 600, bottom: 600},
  ]) assert.throws(() => fixtureRegion(monitor, fixture));
  assert.throws(() => fixtureRegion({...monitor, right: 100}, monitor));
});
