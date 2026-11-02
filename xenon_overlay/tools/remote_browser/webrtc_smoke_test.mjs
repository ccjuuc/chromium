#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Web-layer integration test only: the browser need not contain the new native
// WebUI or input router. Exercises the shipped session.js with real WebRTC.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile, mkdtemp, rm, stat} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {basename, dirname, isAbsolute, join, relative, resolve} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const sourceRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const defaultBrowser = join(sourceRoot, 'out', 'Release_64',
    process.platform === 'win32' ? 'xlb153.exe' : 'chrome');
const usage = `Usage: node webrtc_smoke_test.mjs [--browser <executable>]

Runs the shipped session.js in an isolated headless Chromium with temporary
user data and a loopback HTTP fixture. Requires Node.js 22+; no npm packages.
Tests real canvas video, optional audio, source switching, DataChannel input
authorization, stale generations and channel interruption without any STUN/TURN.
This tests the WEB LAYER ONLY, not native picking, capture or input injection.
Default browser: ${defaultBrowser}`;

// This function is serialized into the local fixture, not run in Node.
async function runBrowserTests() {
  const state = globalThis.__smoke = {done: false, results: []};
  const {RemoteSession, PROTOCOL, VERSION, modifiersFor} = globalThis.XenonRemote;
  const inputs = [];
  const interruptions = [];
  const sourceUpdates = [];
  const streams = [];
  const painters = [];
  let audioContext;
  const video = document.querySelector('video');
  const sample = document.createElement('canvas');
  sample.width = sample.height = 1;
  const sampleContext = sample.getContext('2d', {willReadFrequently: true});
  const check = (condition, message) => {
    if (!condition) throw new Error(message);
  };
  const wait = async (description, predicate, timeout = 12000) => {
    const deadline = performance.now() + timeout;
    while (performance.now() < deadline) {
      if (await predicate()) return;
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    throw new Error(`Timed out: ${description}`);
  };
  const run = async (name, action) => {
    const start = performance.now();
    await action();
    state.results.push({name, passed: true,
      elapsedMs: Math.round(performance.now() - start)});
  };
  const wire = (type, value, version = VERSION) => JSON.stringify({
    protocol: PROTOCOL, version, type, [type]: value,
  });
  const key = generation => ({type: 'key', generation, code: 'KeyA',
    key: 'a', down: true, modifiers: 0});
  const host = new RemoteSession('host', {
    onInput: input => inputs.push(input),
    onInterrupted: reason => interruptions.push(reason),
  });
  const viewer = new RemoteSession('viewer', {
    onSource: source => sourceUpdates.push(source),
    onStream: stream => {
      video.srcObject = stream;
      void video.play().catch(() => {});
    },
  });
  const source = (generation, control = false, active = true) =>
    ({generation, mode: 'tab', control, active});
  function canvasStream(color, width, height) {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d');
    let frame = 0;
    const paint = () => {
      context.fillStyle = color;
      context.fillRect(0, 0, width, height);
      // Force changing frames while keeping the sampled center a solid color.
      context.fillStyle = 'white';
      context.fillRect(frame++ % 40, 0, 8, 8);
    };
    paint();
    painters.push(setInterval(paint, 40));
    const stream = canvas.captureStream(25);
    streams.push(stream);
    return stream;
  }
  function hasColor(channel) {
    if (video.readyState < 2 || !video.videoWidth) return false;
    sampleContext.drawImage(video, video.videoWidth / 2, video.videoHeight / 2,
      1, 1, 0, 0, 1, 1);
    const pixel = sampleContext.getImageData(0, 0, 1, 1).data;
    return pixel[channel] > 180 && pixel[(channel + 1) % 3] < 70 &&
        pixel[(channel + 2) % 3] < 70;
  }
  // A release is valid in view-only mode and acts as an ordered channel barrier.
  async function drainHost(generation) {
    const count = inputs.length;
    viewer.channel.send(wire('input', {type: 'release', generation}));
    await wait('ordered input barrier', () =>
      inputs.length > count && inputs.at(-1).type === 'release');
    check(inputs.at(-1).type === 'release', 'Input barrier was not delivered');
  }
  try {
    let firstStream;
    await run('real offer/answer connects and decodes canvas video', async () => {
      firstStream = canvasStream('#ff0000', 640, 360);
      const offer = await host.createOffer(firstStream);
      const answer = await viewer.createAnswer(offer);
      await host.acceptAnswer(answer);
      await wait('peer and data channels connected', () =>
        host.peer.connectionState === 'connected' &&
        viewer.peer.connectionState === 'connected' &&
        host.channel?.readyState === 'open' &&
        viewer.channel?.readyState === 'open');
      host.setSource(source(1));
      await wait('initial view-only metadata', () => viewer.source?.generation === 1);
      check(!viewer.source.control, 'Initial source unexpectedly allows input');
      await wait('decoded red canvas frame', () => hasColor(0));
      const stats = [...(await viewer.peer.getStats()).values()];
      check(stats.some(item => item.type === 'inbound-rtp' &&
        item.kind === 'video' && item.framesDecoded > 0), 'No decoded RTP frames');
    });

    await run('view-only blocks viewer input and forged channel input', async () => {
      check(!viewer.sendInput(key(1)), 'Viewer sent input without a grant');
      viewer.channel.send(wire('input', key(1)));
      await drainHost(1);
      check(inputs.length === 1 && inputs[0].type === 'release',
        'Host accepted input without a local grant');
    });

    await run('grant forwards pointer modifiers and rejects stale/protocol input', async () => {
      host.setSource(source(1, true));
      await wait('control grant', () => viewer.source?.control);
      const start = inputs.length;
      const modifiers = modifiersFor({shiftKey: true, ctrlKey: true});
      check(viewer.sendInput({type: 'button', x: .25, y: .75,
        button: 0, down: true, modifiers}), 'Granted pointer input was rejected');
      viewer.channel.send(wire('input', key(2)));
      viewer.channel.send(wire('input', key(1), VERSION + 1));
      await drainHost(1);
      check(inputs.length === start + 2, 'Host accepted stale or incompatible input');
      check(inputs[start].type === 'button' && inputs[start].modifiers === 10 &&
        inputs[start].generation === 1, 'Pointer modifiers changed in transport');
      host.setSource(source(1));
      await wait('control revoked', () => !viewer.source.control);
      const revoked = inputs.length;
      viewer.channel.send(wire('input', key(1)));
      await drainHost(1);
      check(inputs.length === revoked + 1, 'Host accepted input after revocation');
    });

    await run('source switch reuses peer/senders and starts previously absent audio', async () => {
      const peer = host.peer;
      const videoSender = host.senders.video;
      const audioSender = host.senders.audio;
      check(!audioSender.track, 'Initial capture unexpectedly contained audio');
      host.setSource(source(1, false, false));
      await wait('inactive source metadata', () => !viewer.source.active);
      await host.replaceStream(null);
      firstStream.getTracks().forEach(track => track.stop());
      const next = canvasStream('#0000ff', 480, 270);
      audioContext = new AudioContext();
      await audioContext.resume();
      const audio = audioContext.createMediaStreamDestination();
      const tone = audioContext.createOscillator();
      tone.frequency.value = 440;
      tone.connect(audio);
      tone.start();
      next.addTrack(audio.stream.getAudioTracks()[0]);
      await host.replaceStream(next);
      host.setSource(source(2));
      await wait('new source metadata and blue video', () =>
        viewer.source.generation === 2 && !viewer.source.control && hasColor(2));
      check(host.peer === peer && host.senders.video === videoSender &&
        host.senders.audio === audioSender, 'Source change replaced the connection');
      check(host.peer.signalingState === 'stable', 'Source change needed renegotiation');
      await wait('received audio RTP after source switch', async () =>
        [...(await viewer.peer.getStats()).values()].some(item =>
          item.type === 'inbound-rtp' && item.kind === 'audio' && item.packetsReceived > 0));
    });

    await run('new source rejects old grants and old-generation control', async () => {
      const updates = sourceUpdates.length;
      host.channel.send(wire('source', {...source(1, true), revision: 999}));
      host.setSource(source(2));
      const revision = host.source.revision;
      await wait('source metadata barrier', () => viewer.source.revision === revision);
      check(sourceUpdates.length === updates + 1 && !viewer.source.control,
        'Viewer accepted an old source control grant');
      host.setSource(source(2, true));
      await wait('new source control grant', () => viewer.source.control);
      const start = inputs.length;
      viewer.channel.send(wire('input', key(1)));
      check(viewer.sendInput({type: 'text', text: '远程输入'}), 'New source rejected text');
      await drainHost(2);
      check(inputs.length === start + 2 && inputs[start].type === 'text' &&
        inputs[start].generation === 2, 'Stale source input reached the host');
    });

    await run('data channel interruption revokes session control', async () => {
      viewer.channel.close();
      await wait('host channel interruption', () =>
        interruptions.includes('channel-closed') && host.source.control === false);
      check(!viewer.source.control, 'Viewer retained control after channel close');
    });
  } catch (error) {
    state.error = error.stack || String(error);
  } finally {
    host.close();
    viewer.close();
    for (const timer of painters) clearInterval(timer);
    for (const stream of streams) stream.getTracks().forEach(track => track.stop());
    if (audioContext) await audioContext.close();
    video.srcObject = null;
    state.done = true;
  }
}

export class Cdp {
  constructor(socket) {
    this.socket = socket;
    this.nextId = 0;
    this.pending = new Map();
    socket.addEventListener('message', event => {
      const message = JSON.parse(event.data);
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      clearTimeout(pending.timer);
      if (message.error) pending.reject(new Error(JSON.stringify(message.error)));
      else pending.resolve(message.result);
    });
    socket.addEventListener('close', () => {
      for (const pending of this.pending.values()) {
        clearTimeout(pending.timer);
        pending.reject(new Error('Browser debugging connection closed'));
      }
      this.pending.clear();
    });
  }

  static async connect(url) {
    const socket = new WebSocket(url);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        socket.close();
        reject(new Error('Browser debugging connection timed out'));
      }, 10000);
      socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, {once: true});
      socket.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error('Browser debugging connection failed'));
      }, {once: true});
    });
    return new Cdp(socket);
  }

  send(method, params = {}, sessionId) {
    const id = ++this.nextId;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Browser debugging request timed out: ${method}`));
      }, 10000);
      this.pending.set(id, {resolve, reject, timer});
      try {
        this.socket.send(JSON.stringify({id, method, params, sessionId}));
      } catch (error) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(error);
      }
    });
  }
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length === 1 && args[0] === '--help') {
    console.log(usage);
    return;
  }
  assert(args.length === 0 || (args.length === 2 && args[0] === '--browser'), usage);
  const executable = resolve(args[1] || defaultBrowser);
  assert((await stat(executable)).isFile(), 'Browser executable does not exist');
  assert.equal(typeof WebSocket, 'function', 'Node.js 22+ with WebSocket is required');
  const sessionSource = await readFile(join(sourceRoot,
    'xenon_overlay/resources/webui/xenon_remote/session.js'));
  const temporaryRoot = await mkdtemp(join(tmpdir(), 'xenon-webrtc-smoke-'));
  const profile = join(temporaryRoot, 'profile');
  let browser;
  let browserExited;
  let spawnError;
  let cdp;
  let browserLog = '';
  let interrupted = false;
  const stop = () => { interrupted = true; };
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, stop);
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  const until = async (description, predicate, timeout = 30000) => {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      if (interrupted) throw new Error('Smoke test interrupted');
      if (spawnError) throw spawnError;
      if (browser && (browser.exitCode !== null || browser.signalCode !== null)) {
        throw new Error(`Test browser exited (${browser.exitCode ?? browser.signalCode})`);
      }
      const result = await predicate();
      if (result) return result;
      await delay(100);
    }
    throw new Error(`Timed out: ${description}`);
  };
  const server = createServer((request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    const routes = {
      '/': ['text/html; charset=utf-8', '<!doctype html><meta charset="utf-8">' +
        '<title>Xenon WebRTC smoke test</title><video muted autoplay playsinline></video>' +
        '<script src="/session.js"></script><script src="/fixture.js"></script>'],
      '/session.js': ['text/javascript; charset=utf-8', sessionSource],
      '/fixture.js': ['text/javascript; charset=utf-8', `(${runBrowserTests.toString()})();`],
    };
    const route = Object.hasOwn(routes, request.url) && routes[request.url];
    if (request.method !== 'GET' || !route) {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, {'Content-Type': route[0]}).end(route[1]);
  });
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    browser = spawn(executable, [
      '--headless=new', '--remote-debugging-address=127.0.0.1',
      '--remote-debugging-port=0', `--user-data-dir=${profile}`,
      '--no-first-run', '--no-default-browser-check', '--no-proxy-server',
      '--disable-background-networking', '--disable-component-update',
      '--disable-extensions', '--disable-sync', '--disable-gpu',
      '--disable-renderer-backgrounding', '--disable-background-timer-throttling',
      '--autoplay-policy=no-user-gesture-required', 'about:blank',
    ], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
    browserExited = new Promise(resolve => browser.once('close', resolve));
    browser.once('error', error => { spawnError = error; });
    browser.stderr.on('data', data => { browserLog = (browserLog + data).slice(-12000); });
    const endpoint = await until('isolated browser debugging endpoint', async () => {
      let text;
      try { text = await readFile(join(profile, 'DevToolsActivePort'), 'utf8'); }
      catch (error) { if (error.code === 'ENOENT') return null; throw error; }
      const [port, path] = text.trim().split(/\r?\n/);
      if (!/^\d+$/.test(port) || !path?.startsWith('/devtools/browser/')) return null;
      return `ws://127.0.0.1:${port}${path}`;
    });
    cdp = await Cdp.connect(endpoint);
    const version = await cdp.send('Browser.getVersion');
    console.log(`Web-layer only: ${version.product}; native capture/input are not tested.`);
    const {targetId} = await cdp.send('Target.createTarget', {
      url: `http://127.0.0.1:${server.address().port}/`,
    });
    const {sessionId} = await cdp.send('Target.attachToTarget', {targetId, flatten: true});
    const result = await until('WebRTC smoke results', async () => {
      const response = await cdp.send('Runtime.evaluate', {
        expression: 'globalThis.__smoke', returnByValue: true,
      }, sessionId);
      if (response.exceptionDetails) throw new Error('Could not inspect the smoke fixture');
      const snapshot = response.result?.value;
      return snapshot?.done ? snapshot : null;
    }, 90000);
    for (const item of result.results) {
      console.log(`PASS ${item.name} (${item.elapsedMs} ms)`);
    }
    assert(!result.error, result.error);
    assert.equal(result.results.length, 6, 'Not all browser checks completed');
    console.log('6/6 real WebRTC checks passed. No external ICE servers were used.');
  } catch (error) {
    if (browserLog) console.error(`Test browser diagnostic tail:\n${browserLog}`);
    throw error;
  } finally {
    // Close only the browser we launched with the unique profile. Never attach
    // to, enumerate, or kill an existing user browser process.
    if (cdp) {
      await cdp.send('Browser.close').catch(() => {});
      cdp.socket.close();
    }
    if (browser && browser.exitCode === null && browser.signalCode === null) {
      await Promise.race([browserExited, delay(3000)]);
      if (browser.exitCode === null && browser.signalCode === null && browser.pid) {
        if (process.platform === 'win32') {
          await new Promise(resolve => {
            const killer = spawn('taskkill.exe', ['/PID', String(browser.pid), '/T', '/F'],
              {windowsHide: true, stdio: 'ignore'});
            killer.once('close', resolve);
            killer.once('error', resolve);
          });
        } else {
          browser.kill('SIGKILL');
        }
        await Promise.race([browserExited, delay(3000)]);
      }
    }
    await new Promise(resolve => {
      server.close(resolve);
      server.closeAllConnections();
    });
    // Validate the owned temporary root before recursive removal on Windows.
    const owned = relative(resolve(tmpdir()), resolve(temporaryRoot));
    assert(owned && !isAbsolute(owned) && !owned.startsWith('..') &&
      basename(temporaryRoot).startsWith('xenon-webrtc-smoke-'), 'Unexpected temporary path');
    await rm(temporaryRoot, {recursive: true, force: true, maxRetries: 10, retryDelay: 200});
    for (const signal of ['SIGINT', 'SIGTERM']) process.removeListener(signal, stop);
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => {
    console.error(error.stack || String(error));
    process.exitCode = 1;
  });
}
