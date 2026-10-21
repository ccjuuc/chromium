// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import test from 'node:test';
import { verifyStaging } from './e2e_diff_test.js';

function mockCDP(states, { screenshotError = false, evaluationError = false, noResponse = false } = {}) {
  let screenshots = 0;
  let closed = false;
  let poll = 0;
  const expressions = [];
  return {
    get screenshots() { return screenshots; },
    get closed() { return closed; },
    expressions,
    dependencies: {
      getPages: async () => [{ url: 'chrome://xenon-update/', webSocketDebuggerUrl: 'mock:no-network' }],
      log: () => {},
      writeScreenshot: () => { screenshots++; },
      Socket: class {
        constructor() { queueMicrotask(() => this.onopen()); }
        send(raw) {
          const request = JSON.parse(raw);
          if (noResponse) return;
          let result;
          if (request.method === 'Page.captureScreenshot') {
            result = screenshotError ? {} : { data: Buffer.from('test PNG').toString('base64') };
          } else {
            expressions.push(request.params.expression);
            if (evaluationError) result = { exceptionDetails: { text: 'page error' } };
            else result = { result: { value: request.params.expression === 'window.__xenonStagingTest' ?
              states[Math.min(poll++, states.length - 1)] : true } };
          }
          queueMicrotask(() => this.onmessage({ data: JSON.stringify({ id: request.id, result }) }));
        }
        close() { closed = true; }
      },
    },
  };
}

const options = { timeoutMs: 200, pollMs: 1, screenshot: '/unused/mock.png' };

test('persistent upgrade errors fail before screenshot success can hide them', async () => {
  const mock = mockCDP([{ status: 'error', error: 'SHA-256 mismatch' }]);
  await assert.rejects(verifyStaging(options, mock.dependencies), /SHA-256 mismatch/);
  assert.equal(mock.screenshots, 0);
  assert.equal(mock.closed, true);
});

test('staging must reach a matching downloaded terminal event', async () => {
  const manifest = { target_version: '1.0.0.2', is_diff: true };
  const mock = mockCDP([{ status: 'downloading' }, { status: 'staged', target: '1.0.0.2', manifest }]);
  assert.deepEqual(await verifyStaging(options, mock.dependencies), manifest);
  assert.equal(mock.screenshots, 1);
  assert.equal(mock.closed, true);
  assert.match(mock.expressions[0], /addWebUiListener\('update-error'/);
  assert.match(mock.expressions[0], /removeWebUiListener/);
});

test('no-update and mismatched staged versions cannot pass', async () => {
  for (const state of [{ status: 'no-update' },
    { status: 'staged', target: '1.0.0.2', manifest: { target_version: '1.0.0.1' } }]) {
    const mock = mockCDP([state]);
    await assert.rejects(verifyStaging(options, mock.dependencies), /No update available|does not match/);
    assert.equal(mock.screenshots, 0);
  }
});

test('incomplete staging and unresponsive CDP time out and close the socket', async () => {
  for (const noResponse of [false, true]) {
    const mock = mockCDP([{ status: 'downloading' }], { noResponse });
    await assert.rejects(verifyStaging({ ...options, timeoutMs: 20 }, mock.dependencies), /timed out/);
    assert.equal(mock.screenshots, 0);
    assert.equal(mock.closed, true);
  }
});

test('CDP evaluation and screenshot errors fail the test', async () => {
  const state = { status: 'staged', target: '1.0.0.2', manifest: { target_version: '1.0.0.2' } };
  for (const failures of [{ evaluationError: true }, { screenshotError: true }]) {
    const mock = mockCDP([state], failures);
    await assert.rejects(verifyStaging(options, mock.dependencies), /evaluation failed|did not return a screenshot/);
    assert.equal(mock.screenshots, 0);
    assert.equal(mock.closed, true);
  }
});

test('CLI exits nonzero for an update failure using in-process CDP mocks', t => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-e2e-cli-'));
  t.after(() => fs.rmSync(temp, { recursive: true, force: true }));
  const preload = path.join(temp, 'mock.mjs');
  fs.writeFileSync(preload, `
    import http from 'node:http';
    import {EventEmitter} from 'node:events';
    http.get = (_url, callback) => {
      const request = new EventEmitter();
      request.setTimeout = () => request;
      queueMicrotask(() => {
        const response = new EventEmitter();
        response.statusCode = 200;
        response.setEncoding = () => {};
        callback(response);
        response.emit('data', JSON.stringify([{url: 'chrome://xenon-update/', webSocketDebuggerUrl: 'mock:no-network'}]));
        response.emit('end');
      });
      return request;
    };
    globalThis.WebSocket = class {
      constructor() { queueMicrotask(() => this.onopen()); }
      send(raw) {
        const message = JSON.parse(raw);
        const value = message.params.expression === 'window.__xenonStagingTest' ?
          {status: 'error', error: 'simulated SHA-256 mismatch'} : true;
        queueMicrotask(() => this.onmessage({data: JSON.stringify({id: message.id, result: {result: {value}}})}));
      }
      close() {}
    };
  `);
  const script = fileURLToPath(new URL('./e2e_diff_test.js', import.meta.url));
  const result = spawnSync(process.execPath, ['--import', pathToFileURL(preload).href, script, '--timeout-ms', '500'],
    { encoding: 'utf8', timeout: 2000 });
  assert.equal(result.status, 1, result.stderr);
  assert.match(result.stderr, /simulated SHA-256 mismatch/);
  assert.doesNotMatch(result.stdout, /Staging completed/);
});
