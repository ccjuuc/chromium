#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Windows native WINDOW capture/input test. Only the two browser windows
// created by this run are eligible; screen capture/control is never selected.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {mkdtemp, mkdir, rm, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {basename, isAbsolute, join, relative, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {createViewerHandler} from './serve_viewer.mjs';
import {until, startBrowser, stopBrowser, newPage, fixtureHtml, fixtureScript, setViewerControls,
  forgedClick, remainsUnchanged, screenshot, ownedBrowsers, requestStop} from './local_pairing_test.mjs';

const usage = `Usage: node local_window_test.mjs --browser <new browser executable>
                                 [--artifacts-dir <new directory>]

Requires Windows with an interactive desktop and a newly built Xenon Remote
browser. Creates a headed host with two OWN browser windows (sharing controls
and local capture fixture) and a separate headless viewer process/profile.
The viewer never calls Page.bringToFront. The native picker auto-selects only
this run's unique window title; input stays inside that selected window.

Tests real window pixels including Chrome's frame, native click with an actual
Shift key press, Unicode text, physical Backspace, revocation and stop.
Clicks use unique color blocks located in the decoded window video so toolbar
height, borders, crop and display DPI do not need guessed coordinates.
No screen capture or screen-wide input is exercised. No external ICE is used.
Owned browser processes and temporary profiles are cleaned up after the run.`;

const markerColors = {button: [232, 32, 216], input: [28, 206, 84]};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

function parseArgs(args) {
  const options = {};
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--help') options.help = true;
    else if (args[i] === '--browser' || args[i] === '--artifacts-dir') {
      assert(args[i + 1] && !args[i + 1].startsWith('--'), `Missing ${args[i]} value`);
      options[args[i] === '--browser' ? 'browser' : 'artifacts'] = resolve(args[++i]);
    } else throw new Error(`Unknown option: ${args[i]}`);
  }
  assert(options.help || options.browser, usage);
  return options;
}

// Read-only Win32 enumeration is restricted to our spawned browser PID BEFORE
// reading a caption. Waiting for its native caption (not merely document.title)
// avoids the picker enumerating an initially untitled window and missing the
// auto-selection, which is checked when a source is first added.
async function waitForOwnedWindowTitle(pid, marker) {
  assert(Number.isSafeInteger(pid) && pid > 0);
  assert(/^[A-Za-z0-9-]+$/.test(marker));
  const script = `Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class XenonOwnWindowProbe {
  public delegate bool EnumProc(IntPtr hwnd, IntPtr parameter);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
  public class Result { public long handle; public string title; public Rect bounds; }
  public static Result[] Find(uint ownedPid, string marker) {
    var found = new List<Result>();
    EnumWindows((hwnd, unused) => {
      uint pid; GetWindowThreadProcessId(hwnd, out pid);
      if (pid != ownedPid || !IsWindowVisible(hwnd)) return true;
      var title = new StringBuilder(2048); GetWindowText(hwnd, title, title.Capacity);
      if (!title.ToString().Contains(marker)) return true;
      Rect rect; if (GetWindowRect(hwnd, out rect)) found.Add(new Result {
        handle=hwnd.ToInt64(), title=title.ToString(), bounds=rect});
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }
}
'@
$deadline = [DateTime]::UtcNow.AddSeconds(12)
do {
  $probeWindows = [XenonOwnWindowProbe]::Find(${pid}, '${marker}')
  if ($probeWindows.Length -eq 1) { $probeWindows[0] | ConvertTo-Json -Compress; exit 0 }
  if ($probeWindows.Length -gt 1) { Write-Error 'Ambiguous owned test window title'; exit 2 }
  Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)
Write-Error 'Native title did not appear on the owned test browser window'; exit 3`;
  return runPowerShellJson(script, 'Owned window caption probe');
}

async function runPowerShellJson(script, description, timeoutMs = 20000) {
  return new Promise((resolve, reject) => {
    const child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script],
      {windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
    let output = '';
    let error = '';
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`${description} timed out`));
    }, timeoutMs);
    child.stdout.on('data', chunk => { output = (output + chunk).slice(-16000); });
    child.stderr.on('data', chunk => { error = (error + chunk).slice(-8000); });
    child.once('error', reason => { clearTimeout(timer); reject(reason); });
    child.once('close', code => {
      clearTimeout(timer);
      if (code !== 0) { reject(new Error(error || `${description} exited ${code}`)); return; }
      try { resolve(JSON.parse(output.trim())); } catch (reason) { reject(reason); }
    });
  });
}

async function findVideoMarker(viewer, color, region = null) {
  if (region) {
    assert(Object.values(region).every(Number.isFinite) && region.left >= 0 &&
      region.top >= 0 && region.right <= 1 && region.bottom <= 1 &&
      region.right > region.left && region.bottom > region.top, 'Invalid marker scan region');
  }
  return viewer.evaluate(`(() => {
    const video = document.querySelector('#remote-video');
    if (!video || video.readyState < 2 || !video.videoWidth) return null;
    const canvas = document.createElement('canvas');
    const region = ${JSON.stringify(region)} || {left: 0, top: 0, right: 1, bottom: 1};
    const offsetX = Math.floor(region.left * video.videoWidth);
    const offsetY = Math.floor(region.top * video.videoHeight);
    canvas.width = Math.floor(region.right * video.videoWidth) - offsetX;
    canvas.height = Math.floor(region.bottom * video.videoHeight) - offsetY;
    const context = canvas.getContext('2d', {willReadFrequently: true});
    // Screen tests pass only their own fixture window's bounds. Never copy
    // other desktop pixels into the inspection canvas in that mode.
    context.drawImage(video, offsetX, offsetY, canvas.width, canvas.height,
      0, 0, canvas.width, canvas.height);
    const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
    const color = ${JSON.stringify(color)};
    let left = canvas.width, top = canvas.height, right = -1, bottom = -1, count = 0;
    for (let y = 0; y < canvas.height; y += 2) {
      for (let x = 0; x < canvas.width; x += 2) {
        const offset = (y * canvas.width + x) * 4;
        // Native window/display capture and YUV video conversion can shift a
        // saturated channel by more than 24 levels (observed: 32 -> 63).
        // Keep the filled-rectangle and minimum-area checks below so this
        // tolerance cannot turn a few incidental pixels into a target.
        if (!color.every((value, channel) => Math.abs(value - pixels[offset + channel]) < 40)) continue;
        left = Math.min(left, x); right = Math.max(right, x);
        top = Math.min(top, y); bottom = Math.max(bottom, y); ++count;
      }
    }
    if (count < 300 || right - left < 60 || bottom - top < 20) return null;
    // Require a filled rectangle, not scattered matches in browser chrome.
    if (count * 4 < (right - left) * (bottom - top) * .6) return null;
    return {x: (offsetX + (left + right) / 2) / video.videoWidth,
      y: (offsetY + (top + bottom) / 2) / video.videoHeight,
      pixelBounds: {left: left + offsetX, top: top + offsetY,
        right: right + offsetX, bottom: bottom + offsetY},
      videoWidth: video.videoWidth, videoHeight: video.videoHeight};
  })()`);
}

async function clickVideoMarker(viewer, color) {
  const marker = await until('unique fixture marker in decoded video', () => findVideoMarker(viewer, color));
  const point = await viewer.evaluate(`(() => {
    const video = document.querySelector('#remote-video');
    video.scrollIntoView({block: 'center'});
    const rect = video.getBoundingClientRect();
    const scale = Math.min(rect.width / video.videoWidth, rect.height / video.videoHeight);
    const width = video.videoWidth * scale, height = video.videoHeight * scale;
    return {x: rect.x + (rect.width - width) / 2 + ${marker.x} * width,
      y: rect.y + (rect.height - height) / 2 + ${marker.y} * height};
  })()`);
  await viewer.clickPoint(point);
  return marker;
}

async function saveVideoFrame(viewer, path) {
  const png = await viewer.evaluate(`(() => {
    const video = document.querySelector('#remote-video');
    const canvas = document.createElement('canvas');
    canvas.width = video.videoWidth; canvas.height = video.videoHeight;
    canvas.getContext('2d').drawImage(video, 0, 0);
    return canvas.toDataURL('image/png').split(',')[1];
  })()`);
  await writeFile(path, Buffer.from(png, 'base64'));
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) { console.log(usage); return; }
  assert.equal(process.platform, 'win32', 'Window input integration currently requires Windows');
  assert.equal(typeof WebSocket, 'function', 'Node.js 22+ is required');
  const executableStat = await stat(options.browser);
  assert(executableStat.isFile(), 'Browser executable not found');
  if (options.artifacts) await mkdir(options.artifacts);
  const temporaryRoot = await mkdtemp(join(tmpdir(), 'xenon-window-pairing-'));
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, requestStop);
  const titleMarker = `XenonWindowPair-${randomUUID()}`;
  const viewerHandler = createViewerHandler();
  const server = createServer((request, response) => {
    if (request.url === '/fixture/window') {
      const html = fixtureHtml('a', titleMarker).replace('</style>',
        '#target-button{background:#e820d8;color:black;border:2px solid #111}' +
        '#target-input{background:#1cce54;color:black;border:2px solid #111}</style>');
      response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8',
        'Cache-Control': 'no-store'}).end(html);
    } else if (request.url === '/fixture.js') {
      response.writeHead(200, {'Content-Type': 'text/javascript; charset=utf-8',
        'Cache-Control': 'no-store'}).end(fixtureScript);
    } else void viewerHandler(request, response);
  });
  const report = {scope: 'native browser-window capture and input; no screen input',
    executable: options.browser, executableModified: executableStat.mtime.toISOString(),
    started: new Date().toISOString(), checks: [], passed: false};
  let host;
  let target;
  let viewer;
  const run = async (name, action) => {
    console.log(`RUN  ${name}`);
    const started = Date.now();
    await action();
    const item = {name, passed: true, elapsedMs: Date.now() - started};
    report.checks.push(item);
    console.log(`PASS ${name} (${item.elapsedMs} ms)`);
  };
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    const baseUrl = `http://127.0.0.1:${server.address().port}`;
    const hostBrowser = await startBrowser('window-host', options.browser,
      join(temporaryRoot, 'host-profile'),
      [`--auto-select-window-capture-source-by-title=${titleMarker}`, '--window-position=30,30'], true);
    const viewerBrowser = await startBrowser('window-viewer', options.browser,
      join(temporaryRoot, 'viewer-profile'), [], false);
    assert.notEqual(hostBrowser.child.pid, viewerBrowser.child.pid);
    report.browsers = {host: hostBrowser.version.product, viewer: viewerBrowser.version.product,
      hostPid: hostBrowser.child.pid, viewerPid: viewerBrowser.child.pid};
    console.log(`Native window test: headed host PID ${hostBrowser.child.pid}, ` +
      `headless viewer PID ${viewerBrowser.child.pid}`);
    host = await newPage(hostBrowser, 'chrome://xenon-remote/', true);
    target = await newPage(hostBrowser, `${baseUrl}/fixture/window`, false, {newWindow: true});
    viewer = await newPage(viewerBrowser, `${baseUrl}/`, true);
    // Native window input checks the selected HWND is foreground. Headless
    // viewer UI dispatch must never activate any other native window.
    viewer.focusBeforeInput = false;
    const controlWindow = await hostBrowser.cdp.send('Browser.getWindowForTarget', {targetId: host.targetId});
    const captureWindow = await hostBrowser.cdp.send('Browser.getWindowForTarget', {targetId: target.targetId});
    assert.notEqual(controlWindow.windowId, captureWindow.windowId,
      'Controls and capture target must be separate owned browser windows');
    const display = await target.evaluate('({width: screen.availWidth, height: screen.availHeight})');
    await hostBrowser.cdp.send('Browser.setWindowBounds', {
      windowId: captureWindow.windowId, bounds: {windowState: 'normal'},
    });
    await hostBrowser.cdp.send('Browser.setWindowBounds', {
      windowId: captureWindow.windowId, bounds: {
        left: 60, top: 60, width: Math.min(1100, display.width - 100),
        height: Math.min(850, display.height - 100),
      },
    });
    report.ownedNativeWindow = await waitForOwnedWindowTitle(hostBrowser.child.pid, titleMarker);

    await run('real native picker captures only the uniquely titled owned browser window', async () => {
      await until('native window host ready', async () => {
        const snapshot = await host.snapshot();
        if (snapshot.error) throw new Error(snapshot.error);
        return host.evaluate(`Boolean(document.querySelector('#choose-source') &&
          !document.querySelector('#choose-source').disabled)`);
      });
      await host.click('input[name="mode"][value="window"]');
      await host.click('#choose-source');
      await until('native window capture preview', async () => {
        const snapshot = await host.snapshot();
        if (snapshot.error) throw new Error(snapshot.error);
        return snapshot.videoWidth > 0 && snapshot.readyState >= 2 && await host.evaluate(
          `!document.querySelector('#create-offer').disabled`);
      });
      assert.equal(await host.evaluate(`document.querySelector('#allow-control').checked`), false);
    });

    await run('portable viewer decodes the actual fixture plus browser frame', async () => {
      await host.click('#create-offer');
      const offer = await until('window host offer', () => host.evaluate(
        `document.querySelector('#offer-output').value || null`));
      await viewer.fill('#offer-input', offer);
      await viewer.click('#create-answer');
      const answer = await until('window viewer answer', () => viewer.evaluate(
        `document.querySelector('#answer-output').value || null`));
      await host.fill('#answer-input', answer);
      await host.click('#accept-answer');
      const marker = await until('native window marker decoded', () => findVideoMarker(viewer, markerColors.button));
      const geometry = await target.evaluate(`({outerWidth, outerHeight, innerWidth, innerHeight,
        devicePixelRatio, screenX, screenY, button: (() => {
          const rect = document.querySelector('#target-button').getBoundingClientRect();
          return {x: rect.x, y: rect.y, width: rect.width, height: rect.height};
        })()})`);
      const scale = marker.videoWidth / geometry.outerWidth;
      assert(geometry.outerHeight > geometry.innerHeight, 'Test window has no browser frame');
      assert(marker.videoHeight / scale - geometry.innerHeight > 30,
        'Captured video appears to omit the browser frame');
      assert(Math.abs(marker.videoHeight / scale - geometry.outerHeight) < 60,
        'Captured frame does not match the selected window geometry');
      report.geometry = {window: geometry, marker};
      const snapshot = await viewer.snapshot();
      assert.equal(snapshot.source.mode, 'window');
      assert.equal(snapshot.source.control, false);
      assert.equal(snapshot.badge, '仅观看');
      const before = await target.evaluate('globalThis.fixtureState');
      await clickVideoMarker(viewer, markerColors.button);
      await forgedClick(viewer, marker, snapshot.source.generation);
      await remainsUnchanged(target, before);
      if (options.artifacts) await saveVideoFrame(viewer, join(options.artifacts, 'captured-browser-window.png'));
    });

    await run('local grant enables trusted window click with a real Shift key press', async () => {
      await host.click('#allow-control');
      await until('native window permission granted', async () => {
        const local = await host.snapshot();
        const remote = await viewer.snapshot();
        if (local.error) throw new Error(local.error);
        return local.control && remote.source?.control && remote.badge === '可以操作';
      });
      await viewer.evaluate(`document.querySelector('#remote-video').focus()`);
      // Windows derives modifiers from actual injected key state. A CDP mouse
      // modifiers bit alone does not press the Shift key in the target process.
      await viewer.send('Input.dispatchKeyEvent', {
        type: 'keyDown', code: 'ShiftLeft', key: 'Shift', windowsVirtualKeyCode: 16, modifiers: 8,
      });
      try {
        await clickVideoMarker(viewer, markerColors.button);
        await until('trusted native window click with held Shift', async () => {
          const state = await target.evaluate('globalThis.fixtureState');
          return state.clicks === 1 && state.lastClick?.trusted && state.lastClick.shift;
        });
      } finally {
        await viewer.send('Input.dispatchKeyEvent', {
          type: 'keyUp', code: 'ShiftLeft', key: 'Shift', windowsVirtualKeyCode: 16, modifiers: 0,
        }).catch(() => {});
      }
    });

    await run('Unicode text and physical Backspace reach only the selected window', async () => {
      await clickVideoMarker(viewer, markerColors.input);
      await setViewerControls(viewer, true);
      await viewer.fill('#text-input', '窗口输入XY');
      await viewer.click('#send-text');
      await until('native Unicode window text', async () => {
        const state = await target.evaluate('globalThis.fixtureState');
        return state.text === '窗口输入XY' && state.inputTrusted;
      });
      await setViewerControls(viewer, false);
      // Refocus the viewer's keyboard receiver without clicking the remote text
      // field again, so the native caret remains at the end of the committed text.
      await viewer.evaluate(`document.querySelector('#remote-video').focus()`);
      await viewer.key('Backspace', 'Backspace', 8);
      await until('native physical Backspace', async () => {
        const state = await target.evaluate('globalThis.fixtureState');
        return state.text === '窗口输入X' && state.keys.some(
          event => event.code === 'Backspace' && event.trusted);
      });
      if (options.artifacts) {
        await screenshot(viewer, join(options.artifacts, 'viewer-window-control.png'));
        await saveVideoFrame(viewer, join(options.artifacts, 'window-input-result.png'));
      }
    });

    await run('local UI revocation rejects further window input', async () => {
      // Click the actual UI checkbox handler without activating the controls
      // window first. Activating it would independently revoke on focus loss
      // and could race the click into re-enabling an already-cleared checkbox.
      assert(await host.evaluate(`(() => {
        const checkbox = document.querySelector('#allow-control');
        if (!checkbox.checked || checkbox.disabled) return false;
        checkbox.click(); return true;
      })()`), 'Window control was already revoked before the explicit UI action');
      await until('window permission revoked at both ends', async () =>
        !(await host.snapshot()).control && !(await viewer.snapshot()).source.control);
      const before = await target.evaluate('globalThis.fixtureState');
      const marker = await clickVideoMarker(viewer, markerColors.button);
      await forgedClick(viewer, marker, (await viewer.snapshot()).source.generation);
      await remainsUnchanged(target, before);
    });

    await run('leaving the selected window revokes control without another remote input', async () => {
      await host.click('#allow-control');
      await until('window permission re-enabled', async () =>
        (await host.snapshot()).control && (await viewer.snapshot()).source.control);
      // Move focus only between this run's two windows. Observe revocation
      // before sending anything, covering the router's asynchronous notification.
      await host.send('Page.bringToFront');
      await until('foreground loss automatically revokes both UI states', async () =>
        !(await host.snapshot()).control && !(await viewer.snapshot()).source.control);
      assert.equal((await viewer.snapshot()).badge, '仅观看');
    });

    await run('stop ends native window capture and its connection', async () => {
      await host.evaluate(`globalThis.__windowTracks =
        document.querySelector('#preview').srcObject.getTracks()`);
      const before = await target.evaluate('globalThis.fixtureState');
      await host.click('#stop');
      await until('native window capture stopped', () => host.evaluate(`
        document.querySelector('#preview').srcObject === null &&
        document.querySelector('#stop').disabled &&
        globalThis.__windowTracks.every(track => track.readyState === 'ended') &&
        globalThis.__localPairProbe.peers.every(peer => peer.connectionState === 'closed')`));
      await until('window viewer channel closed', async () => {
        const snapshot = await viewer.snapshot();
        return snapshot.badge === '仅观看' && snapshot.textDisabled && await viewer.evaluate(
          `globalThis.__localPairProbe.channels.every(channel => channel.readyState === 'closed')`);
      });
      await remainsUnchanged(target, before, 500);
    });
    report.passed = true;
    console.log(`${report.checks.length}/${report.checks.length} native window checks passed.`);
  } catch (error) {
    report.error = error.stack || String(error);
    for (const [name, page] of [['host', host], ['viewer', viewer], ['target', target]]) {
      if (!page) continue;
      try {
        report[name] = await page.snapshot();
        console.error(`${name} state: ${JSON.stringify(report[name])}`);
        if (options.artifacts) await screenshot(page, join(options.artifacts, `${name}-failure.png`));
      } catch { /* A closed/crashed page cannot supply diagnostics. */ }
    }
    for (const browser of ownedBrowsers) {
      if (browser.log) console.error(`${browser.name} diagnostic tail:\n${browser.log}`);
    }
    throw error;
  } finally {
    report.finished = new Date().toISOString();
    await Promise.allSettled(ownedBrowsers.map(stopBrowser));
    await new Promise(resolve => { server.close(resolve); server.closeAllConnections(); });
    const owned = relative(resolve(tmpdir()), resolve(temporaryRoot));
    assert(owned && !isAbsolute(owned) && !owned.startsWith('..') &&
      basename(temporaryRoot).startsWith('xenon-window-pairing-'), 'Unexpected temporary path');
    await rm(temporaryRoot, {recursive: true, force: true, maxRetries: 10, retryDelay: 200});
    for (const signal of ['SIGINT', 'SIGTERM']) process.removeListener(signal, requestStop);
    if (options.artifacts) {
      await writeFile(join(options.artifacts, 'result.json'), JSON.stringify(report, null, 2) + '\n');
      console.log(`Test artifacts: ${options.artifacts}`);
    }
  }
}

export {waitForOwnedWindowTitle, runPowerShellJson, findVideoMarker};

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => {
    console.error(error.stack || String(error));
    process.exitCode = 1;
  });
}
