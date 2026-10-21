#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Lightweight Mock Update Server for Xenon Application Updater (Node.js).
 * Native Node.js implementation with zero external dependencies.
 *
 * Provides:
 *   - GET /api/v1/update/check : Returns update manifest (diff / full / latest / corrupt)
 *   - GET /downloads/*         : Serves binary patches (.zip / .zucc) and full packages
 *   - GET /api/v1/scenario     : Switches server scenario on the fly (diff, full, latest, corrupt)
 */

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..', '..', '..');

const DEFAULT_PORT = 8999;

export class UpdateState {
  constructor(dataDir = null, host = '127.0.0.1', port = DEFAULT_PORT) {
    this.dataDir = dataDir ? path.resolve(dataDir) : path.resolve(rootDir, 'test_packages');
    this.host = host;
    this.port = port;
    this.mode = 'diff'; // 'diff', 'full', 'latest', 'corrupt'
    this.latestVersion = '1.0.0.2';
    this.changelog = '1. 支持原生全目录差分更新\n2. 修复已知问题与性能优化';
  }

  getBaseUrl() {
    return `http://${this.host}:${this.port}`;
  }

  computeFileInfo(filePath) {
    // Hash the actual file without synthesizing packages or loading a large
    // installer into memory. A missing package is a server configuration error.
    const fd = fs.openSync(filePath, 'r');
    try {
      const stat = fs.fstatSync(fd);
      if (!stat.isFile() || stat.size === 0) {
        throw new Error(`Not a non-empty package file: ${path.basename(filePath)}`);
      }
      const hash = crypto.createHash('sha256');
      const buffer = Buffer.alloc(1024 * 1024);
      let count;
      while ((count = fs.readSync(fd, buffer, 0, buffer.length, null)) > 0) {
        hash.update(buffer.subarray(0, count));
      }
      return { size: stat.size, sha256: hash.digest('hex').toUpperCase() };
    } finally {
      fs.closeSync(fd);
    }
  }

  buildManifest(clientVersion = null, platform = 'win') {
    const baseUrl = this.getBaseUrl();

    const patchZip = path.join(this.dataDir, 'patch.zip');
    const diffFile = fs.existsSync(patchZip) ? patchZip : path.join(this.dataDir, 'patch.zucc');
    const fullPkgName = platform === 'mac' ? 'package.pkg' : 'package.zip';
    const fullFile = path.join(this.dataDir, fullPkgName);

    const fullInfo = this.computeFileInfo(fullFile);

    if (this.mode === 'latest') {
      const ver = clientVersion || '1.0.0.1';
      return {
        target_version: ver,
        platform: platform,
        is_diff: false,
        changelog: '已经是最新版本',
        full_package: {
          url: `${baseUrl}/downloads/${fullPkgName}`,
          size: fullInfo.size,
          sha256: fullInfo.sha256
        },
        force_update: false
      };
    }

    let deletions = [];
    const deletionsFile = path.join(this.dataDir, 'deletions.json');
    if (fs.existsSync(deletionsFile)) {
      try {
        deletions = JSON.parse(fs.readFileSync(deletionsFile, 'utf-8'));
      } catch (e) {}
    }

    const isDiffMode = this.mode === 'diff';
    const manifest = {
      target_version: this.latestVersion,
      platform: platform,
      is_diff: isDiffMode,
      changelog: this.changelog,
      force_update: false,
      deletions: deletions,
      full_package: {
        url: `${baseUrl}/downloads/${fullPkgName}`,
        size: fullInfo.size,
        sha256: this.mode === 'corrupt' ? '0000000000000000000000000000000000000000000000000000000000000000' : fullInfo.sha256
      }
    };

    if (isDiffMode) {
      const diffInfo = this.computeFileInfo(diffFile);
      const isZip = fs.existsSync(patchZip);
      manifest.diff_package = {
        url: isZip ? `${baseUrl}/downloads/patch.zip` : `${baseUrl}/downloads/patch.zucc`,
        size: diffInfo.size,
        sha256: this.mode === 'corrupt' ? '0000000000000000000000000000000000000000000000000000000000000000' : diffInfo.sha256
      };
    }

    return manifest;
  }
}

export function createServer(state = new UpdateState()) {
  const server = http.createServer((req, res) => {
    const parsedUrl = new URL(req.url, state.getBaseUrl());
    const pathname = parsedUrl.pathname;

    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
      return;
    }

    console.log(`[UpdateServer] ${req.socket.remoteAddress} - ${req.method} ${pathname}`);

    if (pathname === '/api/v1/update/check') {
      const clientVersion = parsedUrl.searchParams.get('version') || null;
      const platformParam = parsedUrl.searchParams.get('platform');
      const ua = req.headers['user-agent'] || '';
      const platform = platformParam || (ua.includes('Macintosh') || ua.includes('Mac OS') ? 'mac' : 'win');
      let json;
      try {
        json = JSON.stringify(state.buildManifest(clientVersion, platform), null, 2);
      } catch (error) {
        console.error('[UpdateServer] Cannot build update manifest:', error.message);
        res.writeHead(503, {
          'Content-Type': 'application/json; charset=utf-8',
          'Cache-Control': 'no-store'
        });
        res.end(JSON.stringify({ error: 'Update package unavailable', detail: error.message }));
        return;
      }
      res.writeHead(200, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': Buffer.byteLength(json),
        'Cache-Control': 'no-cache, no-store, must-revalidate'
      });
      res.end(json);
      return;
    }

    if (pathname === '/api/v1/scenario') {
      const mode = parsedUrl.searchParams.get('mode');
      if (mode && ['diff', 'full', 'latest', 'corrupt'].includes(mode)) {
        state.mode = mode;
        const msg = JSON.stringify({ status: 'ok', mode: state.mode });
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(msg);
      } else {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'invalid mode, must be diff|full|latest|corrupt' }));
      }
      return;
    }

    if (pathname.startsWith('/downloads/')) {
      const filename = path.basename(pathname);
      const filePath = path.join(state.dataDir, filename);

      if (!fs.existsSync(filePath)) {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end(`File not found: ${filename}`);
        return;
      }

      const stat = fs.statSync(filePath);
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': stat.size,
        'Content-Disposition': `attachment; filename="${filename}"`,
        'Cache-Control': 'no-cache'
      });

      const readStream = fs.createReadStream(filePath);
      readStream.on('error', error => res.destroy(error));
      readStream.pipe(res);
      return;
    }

    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found', path: pathname }));
  });

  return { server, state };
}

function parseCliArgs() {
  const args = process.argv.slice(2);
  const options = {
    port: DEFAULT_PORT,
    mode: 'diff',
    dataDir: path.resolve(rootDir, 'test_packages')
  };

  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--port' && args[i + 1]) options.port = parseInt(args[++i], 10);
    else if (args[i] === '--mode' && args[i + 1]) options.mode = args[++i];
    else if (args[i] === '--data-dir' && args[i + 1]) options.dataDir = path.resolve(args[++i]);
  }
  return options;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const opts = parseCliArgs();
  const state = new UpdateState(opts.dataDir, '127.0.0.1', opts.port);
  state.mode = opts.mode;

  const { server } = createServer(state);
  server.listen(opts.port, '127.0.0.1', () => {
    console.log('============================================================');
    console.log(`[*] Xenon Mock Update Server (Node.js) running on http://127.0.0.1:${opts.port}`);
    console.log(`[*] Data directory: ${state.dataDir}`);
    console.log(`[*] Mode          : ${state.mode} (diff / full / latest / corrupt)`);
    console.log(`[*] Check endpoint: http://127.0.0.1:${opts.port}/api/v1/update/check`);
    console.log('============================================================');
  });

  process.on('SIGINT', () => {
    server.close(() => process.exit(0));
  });
}
