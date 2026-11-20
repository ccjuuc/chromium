#!/usr/bin/env node
// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');

const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

function call(url, method, params = {}) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url);
    let settled = false;
    const finish = (error, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.close();
      if (error) reject(error);
      else resolve(result);
    };
    const timer = setTimeout(() => finish(new Error('CDP timeout: ' + method)), 5000);
    socket.onopen = () => socket.send(JSON.stringify({id: 1, method, params}));
    socket.onmessage = ({data}) => {
      const response = JSON.parse(data);
      if (response.id !== 1) return;
      finish(response.error && new Error(response.error.message), response.result);
    };
    socket.onerror = () => finish(new Error('CDP connection failed: ' + method));
    socket.onclose = () => finish(method === 'Browser.close' ? null :
        new Error('CDP connection closed: ' + method));
  });
}

async function run() {
  const options = {};
  for (let index = 2; index < process.argv.length; index += 2) {
    const key = process.argv[index];
    if (!['--chrome', '--output'].includes(key) || !process.argv[index + 1]) {
      throw new Error('Usage: node run.cjs --chrome <executable> [--output <new directory>]');
    }
    options[key] = process.argv[index + 1];
  }
  if (!options['--chrome']) throw new Error('--chrome is required');
  const executable = path.resolve(options['--chrome']);
  fs.accessSync(executable);
  const output = options['--output'] ? path.resolve(options['--output']) :
      fs.mkdtempSync(path.join(os.tmpdir(), 'xenon-microtask-order-'));
  if (options['--output']) fs.mkdirSync(output);
  const profile = path.join(output, 'profile');
  const log = fs.openSync(path.join(output, 'browser.log'), 'wx');
  console.log('Results: ' + output);
  const child = spawn(executable, [
    '--headless=new', '--remote-debugging-port=0', '--user-data-dir=' + profile,
    '--no-first-run', '--no-default-browser-check', '--disable-gpu',
    '--enable-logging=stderr', '--xenon-electron-app=' + __dirname,
  ], {stdio: ['ignore', log, log]});
  let launchError;
  child.on('error', error => { launchError = error; });
  let endpoint;
  let result = {ok: false, pages: []};
  try {
    const deadline = Date.now() + 45000;
    while (Date.now() < deadline) {
      if (launchError) throw launchError;
      if (child.exitCode !== null || child.signalCode !== null) {
        throw new Error('Browser exited before completing the fixture');
      }
      const portFile = path.join(profile, 'DevToolsActivePort');
      if (!fs.existsSync(portFile)) {
        await delay(100);
        continue;
      }
      const port = Number(fs.readFileSync(portFile, 'utf8').split('\n')[0]);
      if (!Number.isInteger(port) || port <= 0) {
        await delay(100);
        continue;
      }
      endpoint = 'http://127.0.0.1:' + port;
      const targets = await (await fetch(endpoint + '/json/list', {
        signal: AbortSignal.timeout(5000),
      })).json();
      const pages = targets.filter(target => target.type === 'page' && URL.canParse(target.url) &&
          new URL(target.url).pathname.endsWith('/electron_microtask_order/index.html'));
      const snapshots = [];
      for (const page of pages) {
        const reply = await call(page.webSocketDebuggerUrl, 'Runtime.evaluate', {
          expression: '({done: globalThis.__xenonMicrotaskDone === true, result: globalThis.__xenonMicrotaskResult})',
          returnByValue: true,
        });
        if (reply.exceptionDetails) throw new Error('Unable to inspect fixture results');
        snapshots.push(reply.result.value);
      }
      if (snapshots.length === 2 && snapshots.every(snapshot => snapshot.done)) {
        const results = snapshots.map(snapshot => snapshot.result);
        result = {ok: results.every(page => page?.ok === true) &&
            new Set(results.map(page => page.contextIsolation)).size === 2, pages: results};
        break;
      }
      await delay(100);
    }
    if (result.pages.length !== 2) throw new Error('Fixture timed out before both pages completed');
  } catch (error) {
    result.error = error.message;
  } finally {
    if (endpoint && child.exitCode === null && child.signalCode === null) {
      try {
        const version = await (await fetch(endpoint + '/json/version', {
          signal: AbortSignal.timeout(2000),
        })).json();
        await call(version.webSocketDebuggerUrl, 'Browser.close');
      } catch {}
    }
    for (let attempt = 0; attempt < 20 && child.exitCode === null &&
        child.signalCode === null && !launchError; ++attempt) await delay(100);
    if (child.exitCode === null && child.signalCode === null && !launchError) {
      child.kill('SIGTERM');
      for (let attempt = 0; attempt < 20 && child.exitCode === null &&
          child.signalCode === null; ++attempt) await delay(100);
      if (child.exitCode === null && child.signalCode === null) {
        child.kill('SIGKILL');
        await new Promise(resolve => child.once('exit', resolve));
      }
    }
    fs.closeSync(log);
  }
  fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result, null, 2));
  if (!result.ok) process.exitCode = 1;
}

run().catch(error => { console.error(error.message); process.exitCode = 1; });
