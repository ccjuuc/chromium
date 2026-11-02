#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Native end-to-end test. Unlike webrtc_smoke_test.mjs, this REQUIRES a browser
// built with Xenon Remote. It never substitutes a mock host or synthetic media.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {mkdtemp, mkdir, readFile, rm, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {basename, isAbsolute, join, relative, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {createViewerHandler} from './serve_viewer.mjs';
import {Cdp} from './webrtc_smoke_test.mjs';

const usage = `Usage: node local_pairing_test.mjs --browser <new browser executable>
                                  [--headed] [--builtin-viewer] [--artifacts-dir <new directory>]

Requires a Chromium build containing chrome://xenon-remote/ and its native
capture/input implementation. Launches TWO independent browser processes with
temporary profiles, plus one loopback HTTP server for a portable viewer and
test-only capture pages. No external ICE servers or npm packages are used.

The real host/viewer buttons perform capture, manual offer/answer pairing and
permission changes. Chromium's auto-select-tab-capture-source-by-title test
switch selects only this run's uniquely titled local fixture. CDP operates the
test UI and observes results; it does not replace capture or native input.
All owned browser processes and temporary profiles are removed on completion.
--headed is available if the platform's native picker needs a visible window.
--builtin-viewer uses the packaged chrome://xenon-remote/viewer.html receiver.
--artifacts-dir preserves host/viewer screenshots and a JSON result report.`;

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let interrupted = false;
const ownedBrowsers = [];
const stop = () => { interrupted = true; };

function parseArgs(args) {
  const options = {headed: false};
  for (let i = 0; i < args.length; ++i) {
    const argument = args[i];
    if (argument === '--help') options.help = true;
    else if (argument === '--headed') options.headed = true;
    else if (argument === '--builtin-viewer') options.builtinViewer = true;
    else if (argument === '--browser' || argument === '--artifacts-dir') {
      assert(args[i + 1] && !args[i + 1].startsWith('--'), `Missing ${argument} value`);
      options[argument === '--browser' ? 'browser' : 'artifacts'] = resolve(args[++i]);
    } else throw new Error(`Unknown option: ${argument}`);
  }
  assert(options.help || options.browser, usage);
  return options;
}

async function until(description, predicate, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    assert(!interrupted, 'Local pairing test interrupted');
    for (const browser of ownedBrowsers) {
      if (browser.spawnError) throw browser.spawnError;
      assert(browser.child.exitCode === null && browser.child.signalCode === null,
        `${browser.name} browser exited (${browser.child.exitCode ?? browser.child.signalCode})`);
    }
    const value = await predicate();
    if (value) return value;
    await delay(100);
  }
  throw new Error(`Timed out: ${description}`);
}

async function startBrowser(name, executable, profile, extraArgs, headed,
  {disableGpu = true} = {}) {
  const child = spawn(executable, [
    ...(!headed ? ['--headless=new'] : []),
    '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
    '--no-proxy-server', '--disable-background-networking',
    '--disable-component-update', '--disable-extensions', '--disable-sync',
    ...(disableGpu ? ['--disable-gpu'] : []),
    '--window-size=1400,1000', '--force-device-scale-factor=1',
    '--disable-renderer-backgrounding', '--disable-background-timer-throttling',
    '--disable-backgrounding-occluded-windows',
    '--autoplay-policy=no-user-gesture-required', ...extraArgs, 'about:blank',
  ], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
  const browser = {name, child, log: '', cdp: null};
  ownedBrowsers.push(browser);
  browser.exited = new Promise(resolve => child.once('close', resolve));
  child.once('error', error => { browser.spawnError = error; });
  child.stderr.on('data', data => { browser.log = (browser.log + data).slice(-16000); });
  const endpoint = await until(`${name} debugging endpoint`, async () => {
    let data;
    try { data = await readFile(join(profile, 'DevToolsActivePort'), 'utf8'); }
    catch (error) {
      if (['ENOENT', 'EBUSY'].includes(error.code)) return null;
      throw error;
    }
    const [port, path] = data.trim().split(/\r?\n/);
    if (!/^\d+$/.test(port) || !path?.startsWith('/devtools/browser/')) return null;
    return `ws://127.0.0.1:${port}${path}`;
  });
  browser.cdp = await Cdp.connect(endpoint);
  browser.version = await browser.cdp.send('Browser.getVersion');
  return browser;
}

async function stopBrowser(browser) {
  if (browser.cdp) {
    await browser.cdp.send('Browser.close').catch(() => {});
    browser.cdp.socket.close();
  }
  const child = browser.child;
  if (child.exitCode !== null || child.signalCode !== null) return;
  await Promise.race([browser.exited, delay(3000)]);
  if (child.exitCode !== null || child.signalCode !== null || !child.pid) return;
  if (process.platform === 'win32') {
    await new Promise(resolve => {
      const killer = spawn('taskkill.exe', ['/PID', String(child.pid), '/T', '/F'],
        {windowsHide: true, stdio: 'ignore'});
      killer.once('close', resolve);
      killer.once('error', resolve);
    });
  } else child.kill('SIGKILL');
  await Promise.race([browser.exited, delay(3000)]);
}

// Observe native RTCPeerConnection objects and incoming metadata. The proxy
// delegates construction unchanged; no SDP, capture, tracks or input methods
// are replaced. Recording the client channel also lets the test bypass UI
// checks and prove the host rejects unauthorized/stale protocol messages.
function installProbe() {
  const probe = globalThis.__localPairProbe = {
    peers: [], channels: [], source: null, controlEverEnabled: false,
  };
  globalThis.RTCPeerConnection = new Proxy(globalThis.RTCPeerConnection, {
    construct(target, args, newTarget) {
      const peer = Reflect.construct(target, args, newTarget);
      probe.peers.push(peer);
      peer.addEventListener('datachannel', event => {
        if (event.channel.label !== 'xenon-remote') return;
        probe.channels.push(event.channel);
        event.channel.addEventListener('message', message => {
          try {
            const value = JSON.parse(message.data);
            if (value.protocol === 'xenon-remote' && value.type === 'source') {
              probe.source = value.source;
              if (value.source.control) probe.controlEverEnabled = true;
            }
          } catch { /* Invalid protocol messages are intentionally tested. */ }
        });
      });
      return peer;
    },
  });
}

class Page {
  constructor(browser, targetId, sessionId) {
    this.browser = browser;
    this.targetId = targetId;
    this.sessionId = sessionId;
    this.focusBeforeInput = true;
  }

  send(method, params = {}) {
    return this.browser.cdp.send(method, params, this.sessionId);
  }

  async evaluate(expression) {
    const response = await this.send('Runtime.evaluate', {
      expression, awaitPromise: true, returnByValue: true, userGesture: true,
    });
    if (response.exceptionDetails) {
      throw new Error(response.exceptionDetails.exception?.description ||
        response.exceptionDetails.text || 'Page evaluation failed');
    }
    return response.result?.value;
  }

  async point(selector) {
    return this.evaluate(`(() => {
      const element = document.querySelector(${JSON.stringify(selector)});
      if (!element || element.disabled) throw new Error('UI element unavailable: ' +
        ${JSON.stringify(selector)});
      element.scrollIntoView({block: 'center', inline: 'center'});
      const rect = element.getBoundingClientRect();
      if (!rect.width || !rect.height) throw new Error('UI element is hidden');
      return {x: rect.x + rect.width / 2, y: rect.y + rect.height / 2};
    })()`);
  }

  async clickPoint(point, modifiers = 0) {
    if (this.focusBeforeInput) await this.send('Page.bringToFront');
    await this.send('Input.dispatchMouseEvent', {type: 'mouseMoved', ...point, modifiers});
    await this.send('Input.dispatchMouseEvent', {
      type: 'mousePressed', ...point, button: 'left', buttons: 1, clickCount: 1, modifiers,
    });
    await this.send('Input.dispatchMouseEvent', {
      type: 'mouseReleased', ...point, button: 'left', buttons: 0, clickCount: 1, modifiers,
    });
  }

  async click(selector) { await this.clickPoint(await this.point(selector)); }

  async fill(selector, text) {
    await this.click(selector);
    await this.evaluate(`document.querySelector(${JSON.stringify(selector)}).select()`);
    await this.send('Input.insertText', {text});
  }

  async key(code, key, windowsVirtualKeyCode) {
    await this.send('Input.dispatchKeyEvent', {
      type: 'keyDown', code, key, windowsVirtualKeyCode,
    });
    await this.send('Input.dispatchKeyEvent', {
      type: 'keyUp', code, key, windowsVirtualKeyCode,
    });
  }

  async snapshot() {
    return this.evaluate(`(() => {
      const element = id => document.getElementById(id);
      const video = element('remote-video') || element('preview');
      return {url: location.href, title: document.title,
        status: element('status-title')?.textContent,
        error: element('error')?.hidden === false ? element('error').textContent : '',
        control: element('allow-control')?.checked,
        controlDisabled: element('allow-control')?.disabled,
        badge: element('control-badge')?.textContent,
        textDisabled: element('text-input')?.disabled,
        videoWidth: video?.videoWidth, videoHeight: video?.videoHeight,
        readyState: video?.readyState,
        source: globalThis.__localPairProbe?.source,
        peers: globalThis.__localPairProbe?.peers.map(peer => ({
          connection: peer.connectionState, signaling: peer.signalingState})),
        fixture: globalThis.fixtureState || null};
    })()`);
  }
}

async function newPage(browser, url, probe = false, createOptions = {}) {
  const {targetId} = await browser.cdp.send('Target.createTarget', {
    url: 'about:blank', ...createOptions,
  });
  const {sessionId} = await browser.cdp.send('Target.attachToTarget', {targetId, flatten: true});
  const page = new Page(browser, targetId, sessionId);
  await page.send('Page.enable');
  if (probe) {
    await page.send('Page.addScriptToEvaluateOnNewDocument', {
      source: `(${installProbe.toString()})();`,
    });
  }
  await page.send('Page.navigate', {url});
  await until(`page loaded: ${url}`, () => page.evaluate(
    `location.href === ${JSON.stringify(url)} && document.readyState === 'complete'`));
  return page;
}

function fixtureHtml(id, title) {
  const background = id === 'a' ? '#144682' : '#782d14';
  return `<!doctype html><meta charset="utf-8"><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;
      background:${background};color:white;font:24px sans-serif}
      h1{position:fixed;left:10%;top:3%;font-size:30px}
      button{position:fixed;left:10%;top:24%;width:240px;height:64px;font-size:24px}
      input{position:fixed;left:10%;top:45%;width:65%;height:56px;font-size:26px}
      output{position:fixed;left:10%;top:65%}
      #ticker{position:fixed;bottom:0;left:0;width:20px;height:20px;background:white}
    </style><h1>Local capture fixture ${id.toUpperCase()}</h1>
    <button id="target-button">Remote click target</button>
    <input id="target-input" autocomplete="off" aria-label="Remote text target">
    <output id="result">No remote input</output><div id="ticker"></div>
    <script src="/fixture.js"></script>`;
}

const fixtureScript = `(() => {
  const state = globalThis.fixtureState = {clicks: 0, text: '', keys: [],
    lastClick: null, inputTrusted: false};
  const render = () => { document.querySelector('#result').textContent =
    'Clicks: ' + state.clicks + ' / Text: ' + state.text; };
  document.querySelector('#target-button').addEventListener('click', event => {
    ++state.clicks;
    state.lastClick = {trusted: event.isTrusted, shift: event.shiftKey,
      control: event.ctrlKey};
    render();
  });
  document.querySelector('#target-input').addEventListener('input', event => {
    state.text = event.target.value;
    state.inputTrusted = event.isTrusted;
    render();
  });
  document.querySelector('#target-input').addEventListener('keydown', event => {
    state.keys.push({key: event.key, code: event.code, trusted: event.isTrusted});
  });
  let tick = 0;
  setInterval(() => { document.querySelector('#ticker').style.left =
    (tick++ % 100) + 'px'; }, 100);
})();`;

async function targetPoint(page, selector) {
  return page.evaluate(`(() => {
    const rect = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
    return {x: (rect.x + rect.width / 2) / innerWidth,
      y: (rect.y + rect.height / 2) / innerHeight};
  })()`);
}

async function remoteClick(viewer, target, selector, modifiers = 0) {
  const normalized = await targetPoint(target, selector);
  const point = await viewer.evaluate(`(() => {
    const video = document.querySelector('#remote-video');
    video.scrollIntoView({block: 'center'});
    const rect = video.getBoundingClientRect();
    const scale = Math.min(rect.width / video.videoWidth, rect.height / video.videoHeight);
    const width = video.videoWidth * scale, height = video.videoHeight * scale;
    return {x: rect.x + (rect.width - width) / 2 + width * ${normalized.x},
      y: rect.y + (rect.height - height) / 2 + height * ${normalized.y}};
  })()`);
  await viewer.clickPoint(point, modifiers);
  return normalized;
}

async function forgedClick(viewer, point, generation) {
  await viewer.evaluate(`(() => {
    const probe = globalThis.__localPairProbe;
    const channel = probe.channels.find(channel => channel.readyState === 'open');
    if (!channel) throw new Error('No real viewer DataChannel');
    for (const down of [true, false]) channel.send(JSON.stringify({
      protocol: 'xenon-remote', version: 1, type: 'input', input: {
        generation: ${generation}, type: 'button', x: ${point.x}, y: ${point.y},
        button: 0, down, modifiers: 0}}));
  })()`);
}

async function remainsUnchanged(target, expected, duration = 1000) {
  const deadline = Date.now() + duration;
  do {
    const actual = await target.evaluate('globalThis.fixtureState');
    assert.equal(actual.clicks, expected.clicks, 'Unauthorized remote click reached the target');
    assert.equal(actual.text, expected.text, 'Unauthorized remote text reached the target');
    await delay(100);
  } while (Date.now() < deadline);
}

async function decodedColor(viewer, expected) {
  return viewer.evaluate(`(() => {
    const video = document.querySelector('#remote-video');
    if (!video || video.readyState < 2 || !video.videoWidth) return false;
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1;
    const context = canvas.getContext('2d');
    context.drawImage(video, video.videoWidth * .85, video.videoHeight * .15,
      1, 1, 0, 0, 1, 1);
    const rgba = Array.from(context.getImageData(0, 0, 1, 1).data);
    return ${JSON.stringify(expected)}.every((value, i) => Math.abs(value - rgba[i]) < 25);
  })()`);
}

async function screenshot(page, path) {
  const {data} = await page.send('Page.captureScreenshot', {format: 'png'});
  await writeFile(path, Buffer.from(data, 'base64'));
}

async function setViewerControls(viewer, expanded) {
  const toggle = await viewer.evaluate(`(() => {
    const menu = document.getElementById('viewer-menu');
    return menu && !menu.hidden &&
      (menu.getAttribute('aria-expanded') === 'true') !== ${expanded};
  })()`);
  if (toggle) await viewer.click('#viewer-menu');
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) { console.log(usage); return; }
  assert.equal(typeof WebSocket, 'function', 'Node.js 22+ is required');
  const executableStat = await stat(options.browser);
  assert(executableStat.isFile(), 'Browser executable not found');
  if (options.artifacts) await mkdir(options.artifacts);
  const temporaryRoot = await mkdtemp(join(tmpdir(), 'xenon-local-pairing-'));
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, stop);
  const titleMarker = `XenonLocalPair-${randomUUID()}`;
  const viewerHandler = createViewerHandler();
  const server = createServer((request, response) => {
    let fixture;
    if (request.url === '/fixture/a') fixture = fixtureHtml('a', titleMarker);
    else if (request.url === '/fixture/b') fixture = fixtureHtml('b', 'Second capture fixture');
    else if (request.url === '/fixture.js') {
      response.writeHead(200, {'Content-Type': 'text/javascript; charset=utf-8',
        'Cache-Control': 'no-store'}).end(fixtureScript);
      return;
    }
    if (fixture !== undefined) {
      response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8',
        'Cache-Control': 'no-store'}).end(fixture);
      return;
    }
    void viewerHandler(request, response);
  });
  const report = {scope: 'native tab capture and portable viewer in separate processes',
    executable: options.browser, executableModified: executableStat.mtime.toISOString(),
    started: new Date().toISOString(), checks: [], passed: false};
  let host;
  let viewer;
  let targetA;
  let targetB;
  let firstGeneration;
  const run = async (name, action) => {
    console.log(`RUN  ${name}`);
    const start = Date.now();
    await action();
    const check = {name, passed: true, elapsedMs: Date.now() - start};
    report.checks.push(check);
    console.log(`PASS ${name} (${check.elapsedMs} ms)`);
  };
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    const baseUrl = `http://127.0.0.1:${server.address().port}`;
    const hostBrowser = await startBrowser('host', options.browser,
      join(temporaryRoot, 'host-profile'),
      [`--auto-select-tab-capture-source-by-title=${titleMarker}`], options.headed);
    const viewerBrowser = await startBrowser('viewer', options.browser,
      join(temporaryRoot, 'viewer-profile'), [], options.headed);
    assert.notEqual(hostBrowser.child.pid, viewerBrowser.child.pid);
    report.browsers = {host: hostBrowser.version.product, viewer: viewerBrowser.version.product,
      hostPid: hostBrowser.child.pid, viewerPid: viewerBrowser.child.pid};
    console.log(`Native pairing test: host PID ${hostBrowser.child.pid}, ` +
      `viewer PID ${viewerBrowser.child.pid}; ${hostBrowser.version.product}`);
    targetA = await newPage(hostBrowser, `${baseUrl}/fixture/a`);
    targetB = await newPage(hostBrowser, `${baseUrl}/fixture/b`);
    host = await newPage(hostBrowser, 'chrome://xenon-remote/', true);
    viewer = await newPage(viewerBrowser, options.builtinViewer ?
      'chrome://xenon-remote/viewer.html' : `${baseUrl}/`, true);

    await run('native host selects the real local tab and starts capture', async () => {
      await until('native Xenon Remote WebUI ready', async () => {
        const snapshot = await host.snapshot();
        if (snapshot.error) throw new Error(`Native host: ${snapshot.error}`);
        return host.evaluate(`Boolean(document.querySelector('#choose-source') &&
          !document.querySelector('#choose-source').disabled)`);
      });
      await host.click('#choose-source');
      await until('native capture stream and local preview', async () => {
        const snapshot = await host.snapshot();
        if (snapshot.error) throw new Error(`Native capture: ${snapshot.error}`);
        return host.evaluate(`Boolean(document.querySelector('#preview').srcObject &&
          document.querySelector('#preview').readyState >= 2 &&
          !document.querySelector('#create-offer').disabled)`);
      });
      assert.equal(await host.evaluate(`document.querySelector('#allow-control').checked`), false);
    });

    await run('real host/viewer UI exchanges pairing and decodes captured pixels', async () => {
      await host.click('#create-offer');
      const offer = await until('host offer', () => host.evaluate(
        `document.querySelector('#offer-output').value || null`));
      await viewer.fill('#offer-input', offer);
      await viewer.click('#create-answer');
      const answer = await until('viewer answer', () => viewer.evaluate(
        `document.querySelector('#answer-output').value || null`));
      await host.fill('#answer-input', answer);
      await host.click('#accept-answer');
      await until('native video decoded in the other process', async () => {
        const snapshot = await viewer.snapshot();
        if (snapshot.error) throw new Error(`Viewer: ${snapshot.error}`);
        return snapshot.source?.active && snapshot.peers?.[0]?.connection === 'connected' &&
          await decodedColor(viewer, [20, 70, 130]);
      });
      assert(await viewer.evaluate(`(async () => {
        const stats = await globalThis.__localPairProbe.peers[0].getStats();
        return [...stats.values()].some(item => item.type === 'inbound-rtp' &&
          item.kind === 'video' && item.framesDecoded > 0);
      })()`), 'No real RTP video was decoded');
      firstGeneration = (await viewer.snapshot()).source.generation;
      await host.evaluate(`globalThis.__firstPeer = globalThis.__localPairProbe.peers[0]`);
    });

    await run('default view-only blocks UI clicks and forged DataChannel input', async () => {
      const before = await targetA.evaluate('globalThis.fixtureState');
      const snapshot = await viewer.snapshot();
      assert.equal(snapshot.badge, '仅观看');
      assert.equal(snapshot.source.control, false);
      assert.equal(snapshot.textDisabled, true);
      const point = await remoteClick(viewer, targetA, '#target-button');
      await forgedClick(viewer, point, firstGeneration);
      await remainsUnchanged(targetA, before);
    });

    await run('local grant allows trusted remote click, modifiers, keyboard and Chinese text', async () => {
      await host.click('#allow-control');
      await until('local permission reflected in viewer', async () => {
        const local = await host.snapshot();
        const remote = await viewer.snapshot();
        return local.control && remote.source?.control && remote.badge === '可以操作';
      });
      await remoteClick(viewer, targetA, '#target-button', 10);
      await until('native trusted remote button click', async () => {
        const state = await targetA.evaluate('globalThis.fixtureState');
        return state.clicks === 1 && state.lastClick?.trusted &&
          state.lastClick.shift && state.lastClick.control;
      });
      await remoteClick(viewer, targetA, '#target-input');
      await viewer.key('KeyA', 'a', 65);
      await until('native physical-key input', async () => {
        const state = await targetA.evaluate('globalThis.fixtureState');
        return state.text === 'a' && state.keys.some(key => key.code === 'KeyA' && key.trusted);
      });
      await setViewerControls(viewer, true);
      await viewer.fill('#text-input', '中文远程输入');
      await viewer.click('#send-text');
      await until('native committed text input without duplicates', async () => {
        const state = await targetA.evaluate('globalThis.fixtureState');
        return state.text === 'a中文远程输入' && state.inputTrusted;
      });
      await setViewerControls(viewer, false);
      if (options.artifacts) {
        await screenshot(host, join(options.artifacts, 'host-connected.png'));
        await screenshot(viewer, join(options.artifacts, 'viewer-controlling.png'));
        await screenshot(targetA, join(options.artifacts, 'target-a-input.png'));
      }
    });

    await run('local revocation immediately blocks UI and forged channel input', async () => {
      await host.click('#allow-control');
      await until('permission revoked at both endpoints', async () =>
        !(await host.snapshot()).control && !(await viewer.snapshot()).source.control);
      const before = await targetA.evaluate('globalThis.fixtureState');
      const point = await remoteClick(viewer, targetA, '#target-button');
      await forgedClick(viewer, point, firstGeneration);
      await remainsUnchanged(targetA, before);
    });

    await run('switching actual captured tabs revokes control and preserves the connection', async () => {
      await host.click('#allow-control');
      await until('control re-enabled before switching', async () =>
        (await viewer.snapshot()).source.control);
      await targetA.evaluate(`document.title = 'Previous capture fixture'`);
      await targetB.evaluate(`document.title = ${JSON.stringify(titleMarker)}`);
      await host.click('#choose-source');
      await until('second native capture displayed', async () => {
        const local = await host.snapshot();
        if (local.error) throw new Error(`Native source switch: ${local.error}`);
        const remote = await viewer.snapshot();
        return remote.source?.generation > firstGeneration && remote.source.active &&
          !remote.source.control && !local.control && await decodedColor(viewer, [120, 45, 20]);
      });
      assert(await host.evaluate(`globalThis.__localPairProbe.peers.length === 1 &&
        globalThis.__localPairProbe.peers[0] === globalThis.__firstPeer &&
        globalThis.__firstPeer.connectionState === 'connected'`), 'Switch replaced the connection');
      const before = await targetB.evaluate('globalThis.fixtureState');
      const point = await remoteClick(viewer, targetB, '#target-button');
      await forgedClick(viewer, point, (await viewer.snapshot()).source.generation);
      await remainsUnchanged(targetB, before);
    });

    await run('new grant targets only the new tab and rejects old generation input', async () => {
      await host.click('#allow-control');
      await until('new target control grant', async () => (await viewer.snapshot()).source.control);
      const beforeA = await targetA.evaluate('globalThis.fixtureState');
      const beforeB = await targetB.evaluate('globalThis.fixtureState');
      await forgedClick(viewer, await targetPoint(targetB, '#target-button'), firstGeneration);
      await remainsUnchanged(targetA, beforeA, 500);
      await remainsUnchanged(targetB, beforeB, 500);
      await remoteClick(viewer, targetB, '#target-button');
      await until('new target native click', async () => {
        const state = await targetB.evaluate('globalThis.fixtureState');
        return state.clicks === 1 && state.lastClick?.trusted;
      });
      assert.equal((await targetA.evaluate('globalThis.fixtureState')).clicks, beforeA.clicks);
    });

    await run('stop ends native capture tracks, transport and remote control', async () => {
      await host.evaluate(`globalThis.__captureTracks =
        document.querySelector('#preview').srcObject.getTracks()`);
      const before = await targetB.evaluate('globalThis.fixtureState');
      await host.click('#stop');
      await until('local capture and connection closed', () => host.evaluate(`
        document.querySelector('#preview').srcObject === null &&
        document.querySelector('#stop').disabled &&
        !document.querySelector('#allow-control').checked &&
        globalThis.__captureTracks.every(track => track.readyState === 'ended') &&
        globalThis.__firstPeer.connectionState === 'closed'`));
      await until('viewer input disabled after stop', async () => {
        const remote = await viewer.snapshot();
        return remote.badge === '仅观看' && remote.textDisabled && await viewer.evaluate(
          `globalThis.__localPairProbe.channels.every(channel => channel.readyState === 'closed')`);
      });
      await remainsUnchanged(targetB, before, 500);
    });
    report.passed = true;
    console.log(`${report.checks.length}/${report.checks.length} native local pairing checks passed.`);
  } catch (error) {
    report.error = error.stack || String(error);
    for (const [name, page] of [['host', host], ['viewer', viewer], ['targetA', targetA], ['targetB', targetB]]) {
      if (!page) continue;
      try {
        report[name] = await page.snapshot();
        console.error(`${name} state: ${JSON.stringify(report[name])}`);
        if (options.artifacts) await screenshot(page, join(options.artifacts, `${name}-failure.png`));
      } catch { /* A crashed page may no longer answer debugging commands. */ }
    }
    for (const browser of ownedBrowsers) {
      if (browser.log) console.error(`${browser.name} browser diagnostic tail:\n${browser.log}`);
    }
    throw error;
  } finally {
    report.finished = new Date().toISOString();
    await Promise.allSettled(ownedBrowsers.map(stopBrowser));
    await new Promise(resolve => {
      server.close(resolve);
      server.closeAllConnections();
    });
    const owned = relative(resolve(tmpdir()), resolve(temporaryRoot));
    assert(owned && !isAbsolute(owned) && !owned.startsWith('..') &&
      basename(temporaryRoot).startsWith('xenon-local-pairing-'), 'Unexpected temporary path');
    await rm(temporaryRoot, {recursive: true, force: true, maxRetries: 10, retryDelay: 200});
    for (const signal of ['SIGINT', 'SIGTERM']) process.removeListener(signal, stop);
    if (options.artifacts) {
      await writeFile(join(options.artifacts, 'result.json'), JSON.stringify(report, null, 2) + '\n');
      console.log(`Test artifacts: ${options.artifacts}`);
    }
  }
}

export {until, startBrowser, stopBrowser, newPage, fixtureHtml, fixtureScript, setViewerControls,
  forgedClick, remainsUnchanged, screenshot, ownedBrowsers, stop as requestStop};

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => {
    console.error(error.stack || String(error));
    process.exitCode = 1;
  });
}
