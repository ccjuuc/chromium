'use strict';
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
let pendingNavigation = false;
const files = new Map([
  ['/index.html', ['index.html', 'text/html; charset=utf-8']],
  ['/page.js', ['page.js', 'text/javascript; charset=utf-8']],
  ['/worker.js', ['worker.js', 'text/javascript; charset=utf-8']],
]);
const server = http.createServer((request, response) => {
  if (request.method !== 'GET') { response.writeHead(405).end(); return; }
  if (request.url === '/health') {
    response.writeHead(200, {'content-type': 'text/plain'}).end('xenon-game-host-smoke');
    return;
  }
  if (request.url === '/pending-navigation') {
    response.writeHead(200, {'content-type': 'text/plain'}).end(String(pendingNavigation));
    return;
  }
  if (request.url === '/delayed-navigation') {
    pendingNavigation = true;
    const timer = setTimeout(() => response.writeHead(200,
        {'content-type': 'text/html'}).end('<!doctype html><title>Delayed navigation</title>'), 30000);
    response.on('close', () => { pendingNavigation = false; clearTimeout(timer); });
    return;
  }
  const file = files.get(request.url);
  if (!file) { response.writeHead(404).end(); return; }
  response.writeHead(200, {
    'content-type': file[1], 'cache-control': 'no-store',
    'service-worker-allowed': '/',
    'content-security-policy': "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self' xenonsmoke:; worker-src 'self'",
  });
  response.end(fs.readFileSync(path.join(__dirname, file[0])));
});
server.listen(9339, '127.0.0.1', () => console.log('XENON_GAME_SMOKE_SERVER_READY'));
server.on('error', () => { console.error('XENON_GAME_SMOKE_SERVER_FAILED'); process.exitCode = 1; });
