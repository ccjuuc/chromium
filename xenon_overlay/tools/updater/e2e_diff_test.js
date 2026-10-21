#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** Verify download and staging through CDP. Restart/installation is a separate test. */
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const rootDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

function readPages(port, timeoutMs) {
  return new Promise((resolve, reject) => {
    const request = http.get(`http://127.0.0.1:${port}/json`, response => {
      if (response.statusCode !== 200) {
        response.resume();
        reject(new Error(`CDP discovery returned HTTP ${response.statusCode}`));
        return;
      }
      let data = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { data += chunk; });
      response.on('error', reject);
      response.on('end', () => {
        try {
          const pages = JSON.parse(data);
          if (!Array.isArray(pages)) throw new Error('Invalid CDP page list');
          resolve(pages);
        } catch (error) { reject(error); }
      });
    });
    request.setTimeout(timeoutMs, () => request.destroy(new Error('CDP discovery timed out')));
    request.on('error', reject);
  });
}

export async function verifyStaging({ port = 9222,
  feedURL = 'http://127.0.0.1:8999/api/v1/update/check', timeoutMs = 120000,
  pollMs = 500, screenshot = path.join(rootDir, 'e2e_diff_screenshot.png') } = {},
  { getPages = readPages, Socket = globalThis.WebSocket,
    writeScreenshot = (file, data) => fs.writeFileSync(file, data), log = console.log } = {}) {
  if (!Number.isFinite(timeoutMs) || timeoutMs <= 0 || !Number.isFinite(pollMs) || pollMs <= 0) {
    throw new Error('Timeout and polling interval must be positive numbers');
  }
  if (!Socket) throw new Error('This test requires Node.js with a built-in WebSocket client');
  const deadline = Date.now() + timeoutMs;
  const remaining = () => Math.max(1, deadline - Date.now());
  const pages = await getPages(port, remaining());
  const page = pages.find(item => typeof item.url === 'string' &&
    /^chrome:\/\/xenon-update\//.test(item.url));
  if (!page?.webSocketDebuggerUrl) throw new Error(`No chrome://xenon-update/ tab on port ${port}`);
  const socket = new Socket(page.webSocketDebuggerUrl);
  let nextId = 1;
  let connectionError;
  let rejectOpening;
  const pending = new Map();
  const failPending = error => {
    connectionError = error;
    rejectOpening?.(error);
    for (const request of pending.values()) request.reject(error);
  };
  socket.onerror = () => failPending(new Error('CDP WebSocket failed'));
  socket.onclose = () => failPending(new Error('CDP WebSocket closed before test completed'));
  socket.onmessage = event => {
    try {
      const message = JSON.parse(event.data);
      const request = pending.get(message.id);
      if (!request) return;
      if (message.error) request.reject(new Error(message.error.message || 'CDP command failed'));
      else if (message.result?.exceptionDetails) request.reject(new Error('Page evaluation failed: ' + JSON.stringify(message.result.exceptionDetails)));
      else request.resolve(message.result);
    } catch (error) { failPending(error); }
  };
  const command = (method, params) => {
    if (connectionError) return Promise.reject(connectionError);
    return new Promise((resolve, reject) => {
      const id = nextId++;
      const timer = setTimeout(() => settle(reject, new Error('Staging test timed out')), remaining());
      const settle = (callback, value) => {
        clearTimeout(timer);
        pending.delete(id);
        callback(value);
      };
      pending.set(id, { resolve: value => settle(resolve, value), reject: error => settle(reject, error) });
      try { socket.send(JSON.stringify({ id, method, params })); }
      catch (error) { pending.get(id)?.reject(error); }
    });
  };
  const evaluate = async expression => (await command('Runtime.evaluate', {
    expression, returnByValue: true, awaitPromise: true,
  }))?.result?.value;
  let openTimer;
  try {
    await new Promise((resolve, reject) => {
      rejectOpening = reject;
      openTimer = setTimeout(() => reject(new Error('CDP connection timed out')), remaining());
      socket.onopen = resolve;
    });
    clearTimeout(openTimer);
    rejectOpening = undefined;
    // Observe machine-readable events from this check only. Existing UI text
    // could describe a previous update, and must never satisfy this test.
    await evaluate(`(async () => {
      const {addWebUiListener, removeWebUiListener} = await import('chrome://resources/js/cr.js');
      for (const listener of window.__xenonStagingTestListeners || []) removeWebUiListener(listener);
      const state = {status: 'checking'};
      window.__xenonStagingTest = state;
      window.__xenonStagingTestListeners = [
        addWebUiListener('update-available', manifest => {
          state.status = 'downloading'; state.target = manifest.target_version;
        }),
        addWebUiListener('update-downloaded', manifest => {
          state.status = 'staged'; state.manifest = manifest;
        }),
        addWebUiListener('update-error', error => { state.status = 'error'; state.error = error; }),
        addWebUiListener('update-not-available', () => { state.status = 'no-update'; })
      ];
      chrome.send('checkForUpdates', [${JSON.stringify(feedURL)}]);
      return true;
    })()`);
    while (Date.now() < deadline) {
      const state = await evaluate('window.__xenonStagingTest');
      if (!state || typeof state.status !== 'string') throw new Error('Missing update event state');
      if (state.status === 'error') throw new Error(`Update failed: ${state.error || 'unknown error'}`);
      if (state.status === 'no-update') throw new Error('No update available; no staging was tested');
      if (state.status === 'staged') {
        if (!state.target || state.manifest?.target_version !== state.target) {
          throw new Error('Staged version does not match the announced update');
        }
        const capture = await command('Page.captureScreenshot', { format: 'png' });
        if (!capture?.data) throw new Error('CDP did not return a screenshot');
        writeScreenshot(screenshot, Buffer.from(capture.data, 'base64'));
        log(`[+] Staging completed for ${state.target}; screenshot: ${screenshot}`);
        log('    Restart and installation were not tested.');
        return state.manifest;
      }
      await new Promise(resolve => setTimeout(resolve, Math.min(pollMs, remaining())));
    }
    throw new Error('Staging test timed out before update-downloaded');
  } finally {
    clearTimeout(openTimer);
    rejectOpening = undefined;
    failPending(new Error('Staging test finished'));
    socket.onclose = null;
    socket.onerror = null;
    socket.close();
  }
}

function parseArgs(args) {
  const options = {};
  const names = { '--port': 'port', '--feed-url': 'feedURL', '--timeout-ms': 'timeoutMs', '--screenshot': 'screenshot' };
  for (let i = 0; i < args.length; i++) {
    const key = names[args[i]];
    if (!key || !args[i + 1] || args[i + 1].startsWith('--')) throw new Error(`Unknown or incomplete argument: ${args[i]}`);
    const value = args[++i];
    options[key] = key === 'port' || key === 'timeoutMs' ? Number(value) : value;
  }
  if (options.port !== undefined && (!Number.isInteger(options.port) || options.port < 1 || options.port > 65535)) {
    throw new Error('Port must be between 1 and 65535');
  }
  return options;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    await verifyStaging(parseArgs(process.argv.slice(2)));
  } catch (error) {
    console.error(`[ERROR] ${error.message}`);
    process.exitCode = 1;
  }
}
