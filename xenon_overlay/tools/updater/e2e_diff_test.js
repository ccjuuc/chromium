#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Automated End-to-End Differential Update Verification Test
 *
 * Interacts with running browser via Chrome DevTools Protocol (CDP):
 * 1. Finds the chrome://xenon-update tab on remote debugging port 9222
 * 2. Invokes checkForUpdates against the local mock update server
 * 3. Monitors download and version directory staging logs
 * 4. Captures screenshot upon completion for visual verification
 */

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..', '..', '..');

const PORT = 9222;

http.get(`http://127.0.0.1:${PORT}/json`, (res) => {
  let d = '';
  res.on('data', c => d += c);
  res.on('end', () => {
    const list = JSON.parse(d);
    let page = list.find(x => x.url.includes('chrome://xenon-update'));
    if (!page) {
      console.error('[ERROR] No chrome://xenon-update page found on port', PORT);
      console.log('Available pages:', list.map(x => x.url));
      process.exit(1);
    }

    console.log(`[*] Connecting to: ${page.title || 'Xenon Update'} (${page.url})`);
    const ws = new WebSocket(page.webSocketDebuggerUrl);

    ws.onopen = () => {
      console.log('[*] Triggering checkForUpdates via mock server...');
      ws.send(JSON.stringify({
        id: 1,
        method: 'Runtime.evaluate',
        params: {
          expression: `chrome.send('checkForUpdates', ['http://127.0.0.1:8999/api/v1/update/check'])`
        }
      }));
    };

    ws.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      if (msg.id === 1) {
        console.log('[*] Update check request dispatched. Monitoring progress...');
        let count = 0;
        const timer = setInterval(() => {
          count++;
          ws.send(JSON.stringify({
            id: 10 + count,
            method: 'Runtime.evaluate',
            params: {
              expression: `({
                status: document.getElementById('statusText')?.innerText,
                progress: document.getElementById('progressBar')?.style?.width,
                logs: Array.from(document.querySelectorAll('.log-entry')).map(e => e.textContent).slice(-3)
              })`,
              returnByValue: true
            }
          }));

          if (count >= 16) {
            clearInterval(timer);
            setTimeout(() => {
              ws.send(JSON.stringify({
                id: 999,
                method: 'Page.captureScreenshot',
                params: { format: 'png' }
              }));
            }, 1000);
          }
        }, 1500);
      } else if (msg.id >= 11 && msg.id < 999) {
        const val = msg.result?.result?.value;
        if (val?.logs && val.logs.length > 0) {
          const lastLog = val.logs[val.logs.length - 1];
          console.log(`  [Progress #${msg.id - 10}] Status: ${val.status || 'N/A'} | Log: ${lastLog}`);
        }
      } else if (msg.id === 999 && msg.result?.data) {
        const screenshotPath = path.join(rootDir, 'e2e_diff_screenshot.png');
        fs.writeFileSync(screenshotPath, Buffer.from(msg.result.data, 'base64'));
        console.log(`\n[+] Screenshot saved to: ${screenshotPath}`);
        ws.close();
        process.exit(0);
      }
    };

    ws.onerror = (err) => {
      console.error('[ERROR] WebSocket error:', err);
      process.exit(1);
    };
  });
}).on('error', (err) => {
  console.error(`[ERROR] Failed to connect to CDP endpoint on port ${PORT}:`, err.message);
  process.exit(1);
});
