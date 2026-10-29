#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A static viewer host. Pairing and media stay in the two browser endpoints;
// this server never receives an offer, answer, control event or media frame.
import {createServer as createHttpServer} from 'node:http';
import {createServer as createHttpsServer} from 'node:https';
import {readFile, mkdir, copyFile} from 'node:fs/promises';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const defaultResourceDir = resolve(
    dirname(fileURLToPath(import.meta.url)), '../../resources/webui/xenon_remote');
const resources = new Map([
  ['/', ['viewer.html', 'text/html; charset=utf-8']],
  ['/viewer.html', ['viewer.html', 'text/html; charset=utf-8']],
  ['/viewer.js', ['viewer.js', 'text/javascript; charset=utf-8']],
  ['/session.js', ['session.js', 'text/javascript; charset=utf-8']],
  ['/index.css', ['index.css', 'text/css; charset=utf-8']],
]);

export function createViewerHandler(resourceDir = defaultResourceDir) {
  return async (request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('Referrer-Policy', 'no-referrer');
    response.setHeader('Content-Security-Policy',
        "default-src 'none'; script-src 'self'; style-src 'self'; " +
        "media-src blob:; connect-src 'self'; img-src 'self' data:; " +
        "frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      response.writeHead(405, {'Allow': 'GET, HEAD'});
      response.end();
      return;
    }
    let entry;
    try {
      const url = new URL(request.url, 'http://viewer.invalid');
      entry = resources.get(decodeURIComponent(url.pathname));
    } catch {
      response.writeHead(400);
      response.end();
      return;
    }
    if (!entry) {
      response.writeHead(404);
      response.end();
      return;
    }
    try {
      const data = await readFile(join(resourceDir, entry[0]));
      response.writeHead(200, {
        'Content-Type': entry[1],
        'Content-Length': data.length,
      });
      response.end(request.method === 'HEAD' ? undefined : data);
    } catch {
      response.writeHead(404);
      response.end();
    }
  };
}

export async function exportViewer(destination, resourceDir = defaultResourceDir) {
  // Refuse an existing directory so exporting cannot overwrite unrelated files.
  await mkdir(destination);
  for (const file of new Set([...resources.values()].map(entry => entry[0]))) {
    await copyFile(join(resourceDir, file), join(destination, file));
  }
  await copyFile(join(resourceDir, 'viewer.html'), join(destination, 'index.html'));
}

export function parseArgs(args) {
  const options = {bind: '127.0.0.1', port: 8787};
  const names = new Map([
    ['--bind', 'bind'], ['--port', 'port'], ['--cert', 'cert'],
    ['--key', 'key'], ['--export-dir', 'exportDir'],
  ]);
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--help') {
      options.help = true;
      continue;
    }
    const name = names.get(args[i]);
    if (!name || !args[i + 1] || args[i + 1].startsWith('--')) {
      throw new Error(`Unknown option or missing value: ${args[i]}`);
    }
    options[name] = args[++i];
  }
  options.port = Number(options.port);
  if (!Number.isInteger(options.port) || options.port < 1 || options.port > 65535) {
    throw new Error('--port must be an integer from 1 to 65535.');
  }
  if (Boolean(options.cert) !== Boolean(options.key)) {
    throw new Error('--cert and --key must be supplied together.');
  }
  return options;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) {
    process.stdout.write(
        'Serve the portable Xenon remote viewer (no dependencies).\n' +
        '  node serve_viewer.mjs [--bind 127.0.0.1] [--port 8787]\n' +
        '                        [--cert certificate.pem --key key.pem]\n' +
        '  node serve_viewer.mjs --export-dir <new-directory>\n' +
        'Use --bind 0.0.0.0 to allow other devices to open the viewer.\n' +
        'For HTTPS, use a certificate trusted by the receiving device.\n');
    return;
  }
  if (options.exportDir) {
    await exportViewer(resolve(options.exportDir));
    process.stdout.write(`Viewer exported to ${resolve(options.exportDir)}\n`);
    return;
  }
  // Fail before listening if the source checkout does not contain the viewer.
  for (const file of new Set([...resources.values()].map(entry => entry[0]))) {
    await readFile(join(defaultResourceDir, file));
  }
  const handler = createViewerHandler();
  const server = options.cert ? createHttpsServer({
    cert: await readFile(options.cert), key: await readFile(options.key),
  }, handler) : createHttpServer(handler);
  server.requestTimeout = 10_000;
  server.headersTimeout = 10_000;
  server.on('error', error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
  server.listen(options.port, options.bind, () => {
    const scheme = options.cert ? 'https' : 'http';
    const host = options.bind.includes(':') ? `[${options.bind}]` : options.bind;
    process.stdout.write(`Viewer: ${scheme}://${host}:${options.port}/\n`);
    process.stdout.write('On another device, use this computer\'s reachable address.\n');
  });
  for (const signal of ['SIGINT', 'SIGTERM']) {
    process.once(signal, () => {
      server.close();
      server.closeAllConnections();
    });
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}
