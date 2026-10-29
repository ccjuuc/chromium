// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {test} from 'node:test';
import {createViewerHandler, exportViewer, parseArgs} from './serve_viewer.mjs';

test('static server serves only portable viewer resources', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'xenon-remote-viewer-'));
  t.after(() => rm(dir, {recursive: true, force: true}));
  for (const file of ['viewer.html', 'viewer.js', 'session.js', 'index.css']) {
    await writeFile(join(dir, file), `fixture:${file}`);
  }
  await writeFile(join(dir, 'index.html'), 'host UI must not be served');
  await writeFile(join(dir, 'secret.txt'), 'not a viewer resource');
  const server = createServer(createViewerHandler(dir));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => {
    server.closeAllConnections();
    return new Promise(resolve => server.close(resolve));
  });
  const base = `http://127.0.0.1:${server.address().port}`;
  const root = await fetch(base);
  assert.equal(root.status, 200);
  assert.equal(await root.text(), 'fixture:viewer.html');
  assert.equal(root.headers.get('cache-control'), 'no-store');
  assert.match(root.headers.get('content-security-policy'), /frame-ancestors 'none'/);
  const script = await fetch(`${base}/session.js`);
  assert.match(script.headers.get('content-type'), /^text\/javascript/);
  const head = await fetch(`${base}/viewer.html`, {method: 'HEAD'});
  assert.equal(head.status, 200);
  assert.equal(await head.text(), '');
  for (const path of ['/index.html', '/secret.txt', '/%2e%2e%2fsecret.txt', '/subdir/']) {
    assert.equal((await fetch(base + path)).status, 404, path);
  }
  assert.equal((await fetch(`${base}/%XX`)).status, 400);
  assert.equal((await fetch(base, {method: 'POST', body: 'offer'})).status, 405);
});

test('export only includes the portable receiver and refuses overwrite', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'xenon-remote-export-'));
  t.after(() => rm(dir, {recursive: true, force: true}));
  for (const file of ['viewer.html', 'viewer.js', 'session.js', 'index.css']) {
    await writeFile(join(dir, file), `fixture:${file}`);
  }
  const output = join(dir, 'export');
  await exportViewer(output, dir);
  assert.equal(await readFile(join(output, 'index.html'), 'utf8'), 'fixture:viewer.html');
  await assert.rejects(exportViewer(output, dir), {code: 'EEXIST'});
});

test('CLI rejects invalid ports, unpaired TLS arguments and unknown options', () => {
  assert.deepEqual(parseArgs([]), {bind: '127.0.0.1', port: 8787});
  assert.deepEqual(parseArgs(['--bind', '0.0.0.0', '--port', '9000']),
      {bind: '0.0.0.0', port: 9000});
  for (const args of [['--port', '0'], ['--port', 'NaN'], ['--port', '65536'],
    ['--port', '2.5'], ['--cert', 'certificate.pem'], ['--unknown', 'value'], ['--bind']]) {
    assert.throws(() => parseArgs(args));
  }
});
