#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Local Omaha 4.0 development endpoint. Never creates or executes packages.
import { createHash } from 'node:crypto';
import { constants } from 'node:fs';
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { fileURLToPath } from 'node:url';

export const RESPONSE_PREFIX = ")]}'\n";
const ENDPOINT = '/service/update2/json';
const SHA256 = /^[0-9a-f]{64}$/;
const MAX_BODY = 1024 * 1024;
const MAX_MANIFEST = 64 * 1024;
const BASE_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]{0,199}$/;
const plain = value => value !== null && typeof value === 'object' && !Array.isArray(value);
function requireValue(condition, message) { if (!condition) throw new Error(message); }
function keys(value, allowed, label) {
  requireValue(plain(value), `${label} must be an object`);
  requireValue(Object.keys(value).every(key => allowed.includes(key)), `${label}: unsupported field`);
}
function version(value) {
  requireValue(typeof value === 'string' && value.length <= 43 &&
    /^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){0,3}$/.test(value), 'Invalid version');
  const parts = value.split('.').map(Number);
  requireValue(parts.every(n => n <= 0xffffffff), 'Version component exceeds uint32');
  return [...parts, ...Array(4 - parts.length).fill(0)];
}
export function compareVersions(a, b) {
  const aa = version(a), bb = version(b);
  for (let i = 0; i < 4; ++i) if (aa[i] !== bb[i]) return aa[i] < bb[i] ? -1 : 1;
  return 0;
}
const architecture = value => ({ x64: 'x86_64', x86_64: 'x86_64', arm64: 'arm64' })[value];
const platform = value => ({ mac: 'mac', 'Mac OS X': 'mac' })[value];
const sameFile = (a, b) => ['dev', 'ino', 'size', 'mtimeNs', 'ctimeNs'].every(key => a[key] === b[key]);
function deepFreeze(value) {
  for (const item of Object.values(value)) if (item && typeof item === 'object') deepFreeze(item);
  return Object.freeze(value);
}

// Files are opened once with O_NOFOLLOW and retained. Requests never resolve a
// caller-supplied filesystem path; replacing a directory entry cannot redirect
// a download to an unlisted file. In-place modifications fail the stat check.
export async function loadCatalog(manifestPath) {
  const manifestHandle = await fs.open(path.resolve(manifestPath), constants.O_RDONLY | constants.O_NOFOLLOW);
  let manifest;
  try {
    const stat = await manifestHandle.stat();
    requireValue(stat.isFile() && stat.size <= MAX_MANIFEST, 'Manifest must be a small regular file');
    manifest = JSON.parse(await manifestHandle.readFile('utf8'));
  } finally { await manifestHandle.close(); }
  requireValue(manifest.schema_version === 1, 'Unsupported manifest schema');
  const multiple = Object.hasOwn(manifest, 'apps');
  if (multiple) {
    keys(manifest, ['schema_version', 'apps'], 'manifest');
    requireValue(Array.isArray(manifest.apps) && manifest.apps.length > 0 && manifest.apps.length <= 32, 'Invalid manifest apps');
  }
  const releases = multiple ? manifest.apps : [manifest];
  const ids = new Set();
  for (const release of releases) {
    keys(release, [...(multiple ? [] : ['schema_version']), 'appid', 'version', 'minimum_version', 'platform', 'arch', 'full', 'differential'], 'release');
    requireValue(typeof release.appid === 'string' && /^[\x21-\x7e]{1,128}$/.test(release.appid), 'Invalid appid');
    requireValue(!ids.has(release.appid.toLowerCase()), 'Duplicate manifest appid');
    ids.add(release.appid.toLowerCase());
    version(release.version);
    version(release.minimum_version ?? '0');
    requireValue(compareVersions(release.minimum_version ?? '0', release.version) < 0, 'minimum_version must be below target');
    requireValue(release.platform === 'mac', 'This installer contract currently supports mac only');
    requireValue(['arm64', 'x86_64'].includes(release.arch), 'Unsupported manifest architecture');
    requireValue(release.differential === undefined || (Array.isArray(release.differential) && release.differential.length <= 32), 'Invalid differential list');
  }
  const root = await fs.realpath(path.dirname(path.resolve(manifestPath)));
  const artifacts = new Map();
  try {
    const entries = [];
    for (const release of releases) {
      keys(release.full, ['file', 'size', 'sha256'], 'full');
      entries.push({ ...release.full, kind: 'crx3' });
      const bases = new Set();
      for (const diff of release.differential ?? []) {
        keys(diff, ['file', 'size', 'sha256', 'type', 'from_version', 'previous_sha256'], 'differential');
        requireValue(['puff', 'zucc'].includes(diff.type), 'Only Puffin or Zucchini deltas are supported');
        requireValue(compareVersions(diff.from_version, release.version) < 0, 'Differential must upgrade its base version');
        requireValue(SHA256.test(diff.previous_sha256), 'Invalid previous_sha256');
        const baseKey = `${version(diff.from_version).join('.')}:${diff.previous_sha256}`;
        requireValue(!bases.has(baseKey), 'Duplicate differential base');
        bases.add(baseKey);
        entries.push({ ...diff, kind: diff.type });
      }
    }
    for (const entry of entries) {
      requireValue(typeof entry.file === 'string' && BASE_NAME.test(entry.file), 'Artifact file must be a basename in the manifest directory');
      requireValue(!artifacts.has(entry.file), 'Duplicate artifact filename');
      requireValue(Number.isSafeInteger(entry.size) && entry.size > 0, 'Artifact size must be a positive safe integer');
      requireValue(typeof entry.sha256 === 'string' && SHA256.test(entry.sha256), 'Artifact SHA256 must be 64 lowercase hex digits');
      const handle = await fs.open(path.join(root, entry.file), constants.O_RDONLY | constants.O_NOFOLLOW);
      // Track immediately so a subsequent validation failure closes this fd.
      artifacts.set(entry.file, { handle });
      const stat = await handle.stat({ bigint: true });
      requireValue(stat.isFile() && stat.nlink === 1n && stat.size === BigInt(entry.size), `Artifact size/type mismatch: ${entry.file}`);
      const header = Buffer.alloc(12);
      await handle.read(header, 0, header.length, 0);
      if (entry.kind === 'crx3') {
        requireValue(header.subarray(0, 4).toString() === 'Cr24' && header.readUInt32LE(4) === 3 &&
          header.readUInt32LE(8) > 0 && header.readUInt32LE(8) <= 16 * 1024 * 1024 &&
          header.readUInt32LE(8) + 12 < entry.size, 'Full artifact is not a CRX3 container');
      } else {
        requireValue(header.subarray(0, 4).toString() === (entry.kind === 'puff' ? 'PUF1' : 'Zucc'), 'Differential container does not match its declared format');
      }
      const hash = createHash('sha256');
      for await (const chunk of handle.createReadStream({ start: 0, autoClose: false })) hash.update(chunk);
      requireValue(hash.digest('hex') === entry.sha256, `Artifact SHA256 mismatch: ${entry.file}`);
      requireValue(sameFile(stat, await handle.stat({ bigint: true })), 'Artifact changed during verification');
      artifacts.set(entry.file, { handle, stat, ...entry });
    }
  } catch (error) {
    await Promise.all([...artifacts.values()].map(({ handle }) => handle.close()));
    throw error;
  }
  return { manifest: deepFreeze(manifest), releases: Object.freeze(releases), artifacts,
    close: () => Promise.all([...artifacts.values()].map(({ handle }) => handle.close())) };
}

export function buildResponse(catalog, body, origin, now = Date.now()) {
  requireValue(plain(body) && plain(body.request), 'Missing request object');
  const request = body.request;
  requireValue(request.protocol === '4.0', 'Only Omaha protocol 4.0 is supported');
  requireValue(Array.isArray(request.apps) && request.apps.length <= 32, 'Invalid apps list');
  const seen = new Set();
  const formats = new Set(typeof request.acceptformat === 'string' ? request.acceptformat.split(',') : []);
  const responseApps = request.apps.map(app => {
    requireValue(plain(app) && typeof app.appid === 'string' && /^[\x21-\x7e]{1,128}$/.test(app.appid), 'Invalid request appid');
    const id = app.appid.toLowerCase();
    requireValue(!seen.has(id), 'Duplicate request appid');
    seen.add(id);
    const manifest = catalog.releases.find(release => release.appid.toLowerCase() === id);
    const reply = { appid: app.appid, status: manifest ? 'ok' : 'error-unknownApplication' };
    if (reply.status !== 'ok' || app.updatecheck === undefined) return reply; // Ping acknowledgment.
    requireValue(plain(app.updatecheck), 'Invalid updatecheck');
    const no = (status, info) => ({ ...reply, updatecheck: { status, info } });
    const reportedPlatform = request.os?.platform ?? request['@os'];
    if (platform(reportedPlatform) !== manifest.platform ||
        (request['@os'] !== undefined && platform(request['@os']) !== manifest.platform))
      return no('error-osnotsupported', 'Release platform mismatch');
    if (architecture(request.arch ?? request.os?.arch) !== manifest.arch)
      return no('error-hwnotsupported', 'Release architecture mismatch');
    const current = app.version ?? '0';
    version(current);
    if (app.updatecheck.updatedisabled === true) return no('noupdate', 'Updates disabled by client');
    if (compareVersions(current, manifest.version) >= 0) return no('noupdate', 'Installed version is current or newer');
    if (compareVersions(current, manifest.minimum_version ?? '0') < 0) return no('noupdate', 'Installed version is below the release minimum');
    const prefix = app.updatecheck.targetversionprefix;
    if (prefix !== undefined) {
      requireValue(typeof prefix === 'string', 'Invalid targetversionprefix');
      if (prefix && !manifest.version.startsWith(prefix)) return no('noupdate', 'Target version prefix excludes this release');
    }
    if (!formats.has('download') || !formats.has('crx3')) return no('error-inexpressible', 'Client must support download and crx3');
    requireValue(app.cached_items === undefined || (Array.isArray(app.cached_items) && app.cached_items.length <= 128 &&
      app.cached_items.every(item => plain(item) && typeof item.sha256 === 'string' && SHA256.test(item.sha256))), 'Invalid cached_items');
    // Legacy fingerprint/fp is deliberately not a substitute for cached bytes.
    const cached = new Set((app.cached_items ?? []).map(item => item.sha256));
    const download = artifact => ({ type: 'download', size: artifact.size, out: { sha256: artifact.sha256 },
      urls: [{ url: `${origin}/artifacts/${artifact.file}` }] });
    const install = () => ({ type: 'crx3', in: { sha256: manifest.full.sha256 }, size: manifest.full.size, path: '.', arguments: '' });
    const pipelines = [];
    const diff = (manifest.differential ?? []).find(item => compareVersions(current, item.from_version) === 0 &&
      cached.has(item.previous_sha256) && formats.has(item.type) && item.size < manifest.full.size);
    if (diff) pipelines.push({ pipeline_id: `diff-${diff.type}-${diff.sha256}`, operations: [download(diff),
      { type: diff.type, previous: { sha256: diff.previous_sha256 }, out: { sha256: manifest.full.sha256 } }, install()] });
    pipelines.push({ pipeline_id: `full-${manifest.full.sha256}`, operations: [download(manifest.full), install()] });
    return { ...reply, updatecheck: { status: 'ok', nextversion: manifest.version, pipelines } };
  });
  return { response: { protocol: '4.0', server: 'xenon-local-development',
    daystart: { elapsed_days: Math.floor((now - Date.UTC(2007, 0, 1)) / 86400000) }, apps: responseApps } };
}

function rangeFor(header, size) {
  if (header === undefined) return { start: 0, end: size - 1, status: 200 };
  const match = /^bytes=(\d*)-(\d*)$/.exec(header);
  requireValue(match && (match[1] || match[2]), 'Invalid range');
  let start = match[1] ? Number(match[1]) : Math.max(0, size - Number(match[2]));
  let end = match[1] && match[2] ? Math.min(size - 1, Number(match[2])) : size - 1;
  requireValue(Number.isSafeInteger(start) && Number.isSafeInteger(end) && start >= 0 && start <= end && start < size, 'Invalid range');
  return { start, end, status: 206 };
}

export function createOmahaServer(catalog, { logger = () => {}, publicOrigin } = {}) {
  if (publicOrigin !== undefined) {
    const origin = new URL(publicOrigin);
    requireValue(origin.protocol === 'https:' && origin.origin === publicOrigin && !origin.username && !origin.password,
      'public-origin must be an HTTPS origin without credentials, path, query, or fragment');
  }
  const server = http.createServer(async (req, res) => {
    const localOrigin = `http://127.0.0.1:${server.address().port}`;
    const origin = publicOrigin ?? localOrigin;
    const respond = (status, data) => {
      res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
      res.end(JSON.stringify(data));
    };
    const record = { time: new Date().toISOString(), method: req.method, route: 'unknown' };
    res.on('finish', () => logger({ ...record, status: res.statusCode }));
    try {
      // No CORS or browser-origin access, no DNS-rebinding hostnames, and URLs
      // are formed from the actual local listener rather than the Host header.
      if (!['127.0.0.1', '::ffff:127.0.0.1'].includes(req.socket.remoteAddress) ||
          ![`127.0.0.1:${server.address().port}`, `localhost:${server.address().port}`].includes(req.headers.host) || req.headers.origin)
        return respond(403, { error: 'Loopback native clients only' });
      const url = new URL(req.url, localOrigin);
      if (url.pathname === ENDPOINT) {
        record.route = ENDPOINT;
        if (req.method !== 'POST') return respond(405, { error: 'POST required' });
        if (url.searchParams.has('cup2key') || url.searchParams.has('cup2hreq'))
          return respond(501, { error: 'CUP signing is not configured. This local development endpoint cannot authenticate a response to the built-in Google CUP keys.' });
        if (!/^application\/json(?:\s*;|$)/i.test(req.headers['content-type'] ?? '') || req.headers['content-encoding'])
          return respond(415, { error: 'Uncompressed application/json required' });
        if (Number(req.headers['content-length'] ?? 0) > MAX_BODY) return respond(413, { error: 'Request too large' });
        const chunks = [];
        let size = 0;
        for await (const chunk of req) {
          size += chunk.length;
          if (size > MAX_BODY) { respond(413, { error: 'Request too large' }); req.resume(); return; }
          chunks.push(chunk);
        }
        const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        const reply = buildResponse(catalog, body, origin);
        record.apps = body.request.apps.map(app => ({ appid: app.appid, version: app.version, updatecheck: app.updatecheck !== undefined }));
        res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
        res.end(RESPONSE_PREFIX + JSON.stringify(reply));
        return;
      }
      // Exact route comparison also rejects encoded slashes and path traversal.
      const artifact = [...catalog.artifacts.values()].find(item => req.url === `/artifacts/${item.file}`);
      if (!artifact) return respond(404, { error: 'Not found' });
      record.route = `/artifacts/${artifact.file}`;
      if (!['GET', 'HEAD'].includes(req.method)) return respond(405, { error: 'GET or HEAD required' });
      if (!sameFile(artifact.stat, await artifact.handle.stat({ bigint: true })))
        return respond(409, { error: 'Prepared artifact changed; restart with a newly verified manifest' });
      let range;
      try { range = rangeFor(req.headers.range, artifact.size); } catch {
        res.writeHead(416, { 'Content-Range': `bytes */${artifact.size}` }); res.end(); return;
      }
      const headers = { 'Content-Type': 'application/octet-stream', 'Content-Length': range.end - range.start + 1,
        'Accept-Ranges': 'bytes', 'ETag': `"${artifact.sha256}"`, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' };
      if (range.status === 206) headers['Content-Range'] = `bytes ${range.start}-${range.end}/${artifact.size}`;
      res.writeHead(range.status, headers);
      if (req.method === 'HEAD') { res.end(); return; }
      // FileHandle ReadStream.destroy() closes the shared descriptor even with
      // autoClose=false on some Node versions. A cancelled request must not
      // break subsequent range requests or retries against the verified file.
      const chunks = async function* () {
        let position = range.start;
        while (position <= range.end) {
          const buffer = Buffer.alloc(Math.min(64 * 1024, range.end - position + 1));
          const { bytesRead } = await artifact.handle.read(buffer, 0, buffer.length, position);
          requireValue(bytesRead > 0, 'Artifact truncated during download');
          position += bytesRead;
          yield buffer.subarray(0, bytesRead);
        }
      };
      await pipeline(Readable.from(chunks()), res);
    } catch (error) {
      if (res.headersSent) res.destroy();
      else respond(400, { error: error instanceof SyntaxError ? 'Invalid JSON' : error.message });
    }
  });
  server.requestTimeout = 30000;
  server.headersTimeout = 15000;
  return server;
}

async function main(args) {
  const options = {};
  for (let i = 0; i < args.length; ++i) {
    requireValue(['--manifest', '--port', '--public-origin'].includes(args[i]) && args[i + 1] !== undefined && options[args[i]] === undefined,
      'Usage: node omaha_update_server.js --manifest /absolute/path/release.json [--port 9001] [--public-origin https://updates.example.com]');
    options[args[i]] = args[++i];
  }
  requireValue(options['--manifest'], '--manifest is required; no artifacts are generated');
  const port = Number(options['--port'] ?? 9001);
  requireValue(Number.isInteger(port) && port >= 1 && port <= 65535, 'Invalid port');
  const catalog = await loadCatalog(options['--manifest']);
  let server;
  try {
    server = createOmahaServer(catalog, { publicOrigin: options['--public-origin'], logger: event => process.stdout.write(JSON.stringify(event) + '\n') });
  } catch (error) { await catalog.close(); throw error; }
  server.on('error', async error => { console.error(error.message); await catalog.close(); process.exitCode = 1; });
  server.listen(port, '127.0.0.1', () => console.log(JSON.stringify({ listening: `http://127.0.0.1:${port}${ENDPOINT}`,
    apps: catalog.releases.map(({ appid, version }) => ({ appid, version })), cup: 'unsupported-development-only' })));
  const stop = () => { server.close(async () => { await catalog.close(); }); server.closeIdleConnections(); };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).catch(error => { console.error(error.message); process.exitCode = 1; });
}
