#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Triggers atomic update install and relaunch (quitAndInstall) via CDP.
 */

import http from 'node:http';

const PORT = 9222;

http.get(`http://127.0.0.1:${PORT}/json`, (res) => {
  let d = '';
  res.on('data', c => d += c);
  res.on('end', () => {
    const list = JSON.parse(d);
    const page = list.find(x => x.url.includes('chrome://xenon-update'));
    if (!page) {
      console.error('[ERROR] No chrome://xenon-update page found on port', PORT);
      process.exit(1);
    }

    const ws = new WebSocket(page.webSocketDebuggerUrl);
    ws.onopen = () => {
      console.log('[*] Invoking chrome.send("quitAndInstall")...');
      ws.send(JSON.stringify({
        id: 1,
        method: 'Runtime.evaluate',
        params: { expression: "chrome.send('quitAndInstall')" }
      }));
      setTimeout(() => {
        console.log('[+] quitAndInstall dispatched successfully.');
        ws.close();
        process.exit(0);
      }, 1500);
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
