#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Screen CAPTURE ONLY. No screen keyboard, pointer or text commands are sent,
// and no desktop image, screenshot, video recording or pixel array is saved.
import assert from 'node:assert/strict';
import {randomInt, randomUUID} from 'node:crypto';
import {mkdtemp, rm, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {basename, isAbsolute, join, relative, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {createViewerHandler} from './serve_viewer.mjs';
import {until, startBrowser, stopBrowser, newPage, ownedBrowsers,
  requestStop} from './local_pairing_test.mjs';
import {waitForOwnedWindowTitle, runPowerShellJson,
  findVideoMarker} from './local_window_test.mjs';

const usage = `Usage: node local_screen_readonly_test.mjs --browser <new browser executable>
                                          [--report <new JSON file>]

Windows native SCREEN CAPTURE ONLY, with a headed sharing host, a separate
owned fixture window and a headless portable viewer. Requires the new native
Xenon Remote build. All remote control stays disabled; no screen keyboard,
mouse or text input is attempted. No screenshots or desktop video are saved.

The real capture track label must match screen:<device-index>:<window-id>.
The ACTIVE EnumDisplayDevicesW device is matched by DeviceName to a physical
monitor. A per-monitor-DPI-aware helper moves only the verified owned fixture
HWND into that monitor. Unknown/ambiguous monitors fail instead of guessing.
Decoded video is inspected only within that fixture window's rectangle; its
locally changed color proves real capture updates. Output is JSON statistics.
All temporary profiles and owned browser processes are cleaned up on exit.`;

export function parseScreenLabel(label) {
  assert.equal(typeof label, 'string', 'Capture track has no screen label');
  const match = /^screen:(\d+):(-?\d+)$/.exec(label);
  assert(match, `Unsupported screen label: ${label}`);
  const index = BigInt(match[1]);
  const windowId = BigInt(match[2]);
  assert(index <= 0xffffffffn, 'Screen device index exceeds EnumDisplayDevicesW range');
  assert(windowId >= -(1n << 63n) && windowId < (1n << 63n), 'Screen window ID is out of range');
  return {deviceIndex: Number(index), windowId: match[2]};
}

export function fixtureRegion(monitor, fixture) {
  for (const rectangle of [monitor, fixture]) {
    assert(rectangle && ['left', 'top', 'right', 'bottom'].every(
      name => Number.isInteger(rectangle[name])), 'Invalid physical rectangle');
    assert(rectangle.right > rectangle.left && rectangle.bottom > rectangle.top,
      'Empty physical rectangle');
  }
  assert(fixture.left >= monitor.left && fixture.top >= monitor.top &&
    fixture.right <= monitor.right && fixture.bottom <= monitor.bottom,
  'Fixture is not completely inside the selected physical monitor');
  const width = monitor.right - monitor.left;
  const height = monitor.bottom - monitor.top;
  return {left: (fixture.left - monitor.left) / width,
    top: (fixture.top - monitor.top) / height,
    right: (fixture.right - monitor.left) / width,
    bottom: (fixture.bottom - monitor.top) / height};
}

async function mapScreenAndPlaceOwnedFixture(deviceIndex, pid, windowHandle, marker) {
  assert(Number.isInteger(deviceIndex) && deviceIndex >= 0 && deviceIndex <= 0xffffffff);
  assert(Number.isInteger(pid) && pid > 0);
  assert(Number.isSafeInteger(windowHandle) && windowHandle > 0);
  assert(/^[A-Za-z0-9-]+$/.test(marker));
  const script = `Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class XenonScreenReadOnlyPlacement {
  [StructLayout(LayoutKind.Sequential)] public struct Rect {
    public int left, top, right, bottom;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct DisplayDevice {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
    public uint StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct MonitorInfo {
    public int cbSize;
    public Rect rcMonitor, rcWork;
    public uint dwFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
  }
  public delegate bool MonitorProc(IntPtr monitor, IntPtr hdc, ref Rect rect, IntPtr data);
  [DllImport("user32.dll", EntryPoint="EnumDisplayDevicesW", ExactSpelling=true, CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice result, uint flags);
  [DllImport("user32.dll", SetLastError=true)]
  static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorProc callback, IntPtr data);
  [DllImport("user32.dll", EntryPoint="GetMonitorInfoW", ExactSpelling=true, CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
  [DllImport("user32.dll", SetLastError=true)]
  static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll", SetLastError=true)] static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
  [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
  [DllImport("user32.dll", SetLastError=true)]
  static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
  public class Result {
    public uint deviceIndex, stateFlags, fixturePid;
    public string deviceName;
    public long fixtureHandle;
    public Rect monitorBounds, workBounds, fixtureBounds;
  }
  static DisplayDevice GetActiveDevice(uint index) {
    var device = new DisplayDevice(); device.cb = Marshal.SizeOf(typeof(DisplayDevice));
    if (!EnumDisplayDevices(null, index, ref device, 0))
      throw new InvalidOperationException("Selected display device index does not exist");
    if ((device.StateFlags & 1) == 0 || String.IsNullOrEmpty(device.DeviceName))
      throw new InvalidOperationException("Selected display device is not ACTIVE");
    return device;
  }
  public static Result Place(uint index, uint ownedPid, long ownedHandle, string marker) {
    // -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. This thread obtains
    // physical rcMonitor/rcWork coordinates and uses them with SetWindowPos.
    // Never pass CDP's DIP window bounds as physical display coordinates.
    IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
    if (previous == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(),
      "Could not enter per-monitor DPI awareness");
    try {
      var device = GetActiveDevice(index);
      int count = 0; IntPtr selectedHandle = IntPtr.Zero;
      var selected = new MonitorInfo();
      bool enumerated = EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero,
        delegate(IntPtr monitor, IntPtr hdc, ref Rect rect, IntPtr data) {
          var info = new MonitorInfo(); info.cbSize = Marshal.SizeOf(typeof(MonitorInfo));
          if (GetMonitorInfo(monitor, ref info) && String.Equals(info.DeviceName,
              device.DeviceName, StringComparison.OrdinalIgnoreCase)) {
            selected = info; selectedHandle = monitor; ++count;
          }
          return true;
        }, IntPtr.Zero);
      if (!enumerated || count != 1)
        throw new InvalidOperationException("ACTIVE display DeviceName did not map to exactly one physical monitor");
      IntPtr hwnd = new IntPtr(ownedHandle); uint actualPid;
      GetWindowThreadProcessId(hwnd, out actualPid);
      if (actualPid != ownedPid || !IsWindowVisible(hwnd))
        throw new InvalidOperationException("Fixture HWND is not the visible owned browser window");
      var title = new StringBuilder(2048); GetWindowText(hwnd, title, title.Capacity);
      if (!title.ToString().Contains(marker))
        throw new InvalidOperationException("Fixture HWND no longer has this run's unique title");
      var area = selected.rcWork;
      if (area.left < selected.rcMonitor.left || area.top < selected.rcMonitor.top ||
          area.right > selected.rcMonitor.right || area.bottom > selected.rcMonitor.bottom)
        throw new InvalidOperationException("Physical work area is outside the selected monitor");
      int width = Math.Min(1000, area.right - area.left - 48);
      int height = Math.Min(700, area.bottom - area.top - 48);
      if (width < 480 || height < 360)
        throw new InvalidOperationException("Selected monitor is too small for the visible test fixture");
      // HWND_TOP, SWP_NOACTIVATE | SWP_SHOWWINDOW: move only our validated
      // fixture, with physical coordinates. No keyboard/mouse injection occurs.
      if (!SetWindowPos(hwnd, IntPtr.Zero, area.left + 24, area.top + 24,
          width, height, 0x0010 | 0x0040))
        throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not place owned fixture window");
      Rect actual;
      if (!GetWindowRect(hwnd, out actual)) throw new Win32Exception(Marshal.GetLastWin32Error());
      var verify = GetActiveDevice(index);
      if (!String.Equals(verify.DeviceName, device.DeviceName, StringComparison.OrdinalIgnoreCase) ||
          MonitorFromWindow(hwnd, 0) != selectedHandle ||
          actual.left < selected.rcMonitor.left || actual.top < selected.rcMonitor.top ||
          actual.right > selected.rcMonitor.right || actual.bottom > selected.rcMonitor.bottom)
        throw new InvalidOperationException("Display changed or fixture was not placed inside the selected monitor");
      return new Result {deviceIndex=index, stateFlags=device.StateFlags,
        deviceName=device.DeviceName, monitorBounds=selected.rcMonitor, workBounds=selected.rcWork,
        fixtureBounds=actual, fixtureHandle=ownedHandle, fixturePid=actualPid};
    } finally { SetThreadDpiAwarenessContext(previous); }
  }
}
'@
[XenonScreenReadOnlyPlacement]::Place(${deviceIndex}, ${pid}, ${windowHandle}, '${marker}') |
  ConvertTo-Json -Depth 4 -Compress`;
  return runPowerShellJson(script, 'Physical display mapping and owned fixture placement');
}

function parseArgs(args) {
  const options = {};
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--help') options.help = true;
    else if (args[i] === '--browser' || args[i] === '--report') {
      assert(args[i + 1] && !args[i + 1].startsWith('--'), `Missing ${args[i]} value`);
      options[args[i] === '--browser' ? 'browser' : 'report'] = resolve(args[++i]);
    } else throw new Error(`Unknown option: ${args[i]}`);
  }
  assert(options.help || options.browser, usage);
  return options;
}

async function assertViewingOnly(host, viewer) {
  assert.equal(await host.evaluate(`document.querySelector('#allow-control')?.checked`), false,
    'Screen control must remain disabled');
  assert.equal(await viewer.evaluate('globalThis.__localPairProbe.controlEverEnabled'), false,
    'A screen control grant was observed');
  const snapshot = await viewer.snapshot();
  if (snapshot.source) assert.equal(snapshot.source.control, false);
  assert.equal(snapshot.badge, '仅观看');
  assert.equal(snapshot.textDisabled, true);
}

async function decodedFrameCount(viewer) {
  return viewer.evaluate(`(async () => {
    const peer = globalThis.__localPairProbe.peers[0];
    if (!peer) return 0;
    const stats = await peer.getStats();
    return [...stats.values()].filter(item => item.type === 'inbound-rtp' &&
      item.kind === 'video').reduce((total, item) => total + (item.framesDecoded || 0), 0);
  })()`);
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) { console.log(usage); return; }
  assert.equal(process.platform, 'win32', 'Physical screen mapping requires Windows');
  assert.equal(typeof WebSocket, 'function', 'Node.js 22+ is required');
  const executableStat = await stat(options.browser);
  assert(executableStat.isFile(), 'Browser executable not found');
  const temporaryRoot = await mkdtemp(join(tmpdir(), 'xenon-screen-readonly-'));
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, requestStop);
  const titleMarker = `XenonScreenReadOnly-${randomUUID()}`;
  const colors = [[randomInt(190, 236), randomInt(20, 46), randomInt(150, 216)],
    [randomInt(20, 46), randomInt(175, 236), randomInt(80, 136)]];
  const viewerHandler = createViewerHandler();
  const server = createServer((request, response) => {
    if (request.url === '/fixture/screen') {
      response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8',
        'Cache-Control': 'no-store'}).end(`<!doctype html><meta charset="utf-8">
        <title>${titleMarker}</title><style>
        html,body{margin:0;height:100%;overflow:hidden;background:#151921;color:white;font:24px sans-serif}
        h1{text-align:center;font-size:22px;margin:24px}
        #marker{width:70vw;height:45vh;margin:32px auto;border:12px solid white;
          background:rgb(${colors[0].join(',')});box-sizing:border-box}
        </style><h1>Screen capture fixture — remote control disabled</h1><div id="marker"></div>`);
    } else void viewerHandler(request, response);
  });
  const report = {scope: 'native screen capture only', screenshotSaved: false,
    remoteInputAttempted: false, remoteControlEverEnabled: false,
    executable: options.browser, executableModified: executableStat.mtime.toISOString(),
    started: new Date().toISOString(), checks: [], passed: false};
  let host;
  let fixture;
  let viewer;
  let mapping;
  let screen;
  let ownedWindow;
  let region;
  let firstMarker;
  const run = async (name, action) => {
    const started = Date.now();
    await action();
    await assertViewingOnly(host, viewer);
    const result = {name, passed: true, elapsedMs: Date.now() - started};
    report.checks.push(result);
    console.log(JSON.stringify({event: 'check', ...result}));
  };
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    const baseUrl = `http://127.0.0.1:${server.address().port}`;
    const hostBrowser = await startBrowser('screen-host', options.browser,
      join(temporaryRoot, 'host-profile'), ['--auto-select-screen-capture-source'], true);
    const viewerBrowser = await startBrowser('screen-viewer', options.browser,
      join(temporaryRoot, 'viewer-profile'), [], false);
    assert.notEqual(hostBrowser.child.pid, viewerBrowser.child.pid);
    report.browsers = {host: hostBrowser.version.product, viewer: viewerBrowser.version.product,
      hostPid: hostBrowser.child.pid, viewerPid: viewerBrowser.child.pid};
    host = await newPage(hostBrowser, 'chrome://xenon-remote/', true);
    fixture = await newPage(hostBrowser, `${baseUrl}/fixture/screen`, false, {newWindow: true});
    viewer = await newPage(viewerBrowser, `${baseUrl}/`, true);
    viewer.focusBeforeInput = false;
    ownedWindow = await waitForOwnedWindowTitle(hostBrowser.child.pid, titleMarker);
    const controlWindow = await hostBrowser.cdp.send('Browser.getWindowForTarget', {targetId: host.targetId});
    const fixtureWindow = await hostBrowser.cdp.send('Browser.getWindowForTarget', {targetId: fixture.targetId});
    assert.notEqual(controlWindow.windowId, fixtureWindow.windowId);
    // Only the state is set through CDP; physical x/y/width/height come solely
    // from the per-monitor-DPI-aware Win32 helper after the real label is known.
    await hostBrowser.cdp.send('Browser.setWindowBounds', {
      windowId: fixtureWindow.windowId, bounds: {windowState: 'normal'},
    });

    await run('native screen capture yields a strict identifiable device label', async () => {
      await until('native screen host ready', async () => {
        const snapshot = await host.snapshot();
        if (snapshot.error) throw new Error(snapshot.error);
        return host.evaluate(`Boolean(document.querySelector('#choose-source') &&
          !document.querySelector('#choose-source').disabled)`);
      });
      await assertViewingOnly(host, viewer);
      await host.click('input[name="mode"][value="screen"]');
      await host.click('#choose-source');
      const label = await until('native screen capture track', async () => {
        const snapshot = await host.snapshot();
        if (snapshot.error) throw new Error(snapshot.error);
        return host.evaluate(`document.querySelector('#preview').srcObject?.getVideoTracks()[0]?.label || null`);
      });
      screen = parseScreenLabel(label);
      report.captureLabel = label;
    });

    await run('ACTIVE display maps by DeviceName and owned fixture moves in physical pixels', async () => {
      mapping = await mapScreenAndPlaceOwnedFixture(screen.deviceIndex, hostBrowser.child.pid,
        ownedWindow.handle, titleMarker);
      region = fixtureRegion(mapping.monitorBounds, mapping.fixtureBounds);
      report.display = mapping;
      report.inspectedRegion = region;
    });

    await run('real portable viewer receives only-viewing screen metadata and the fixture marker', async () => {
      await until('capture ready to pair', () => host.evaluate(
        `!document.querySelector('#create-offer').disabled`));
      await host.click('#create-offer');
      const offer = await until('screen host offer', () => host.evaluate(
        `document.querySelector('#offer-output').value || null`));
      await viewer.fill('#offer-input', offer);
      await viewer.click('#create-answer');
      const answer = await until('screen viewer answer', () => viewer.evaluate(
        `document.querySelector('#answer-output').value || null`));
      await host.fill('#answer-input', answer);
      await host.click('#accept-answer');
      await until('screen media connected', async () => {
        await assertViewingOnly(host, viewer);
        const snapshot = await viewer.snapshot();
        if (snapshot.error) throw new Error(snapshot.error);
        return snapshot.source?.active && snapshot.source.mode === 'screen' &&
          snapshot.videoWidth > 0 && snapshot.peers?.[0]?.connection === 'connected';
      });
      // The control UI may have covered our fixture while pairing. Activate
      // only that owned window, with no synthetic screen input.
      await fixture.send('Page.bringToFront');
      // Revalidate the exact ACTIVE device and HWND after asynchronous pairing.
      const verified = await mapScreenAndPlaceOwnedFixture(screen.deviceIndex,
        hostBrowser.child.pid, ownedWindow.handle, titleMarker);
      assert.equal(verified.deviceName, mapping.deviceName, 'Display topology changed during pairing');
      mapping = verified;
      region = fixtureRegion(mapping.monitorBounds, mapping.fixtureBounds);
      const bounds = mapping.monitorBounds;
      const aspect = (bounds.right - bounds.left) / (bounds.bottom - bounds.top);
      const decoded = await viewer.snapshot();
      assert(Math.abs(decoded.videoWidth / decoded.videoHeight / aspect - 1) < .03,
        'Decoded frame does not match the selected physical monitor aspect');
      firstMarker = await until('owned fixture marker decoded', async () => {
        await assertViewingOnly(host, viewer);
        return findVideoMarker(viewer, colors[0], region);
      });
      report.display = mapping;
      report.inspectedRegion = region;
      report.firstMarker = firstMarker;
      report.framesBeforeChange = await decodedFrameCount(viewer);
      assert(report.framesBeforeChange > 0, 'No real screen RTP frames decoded');
    });

    await run('local fixture color change arrives through the real read-only screen stream', async () => {
      // Change only local test page DOM through its own CDP session. This is
      // not sent through the viewer/DataChannel/native screen input path.
      await fixture.evaluate(`document.querySelector('#marker').style.backgroundColor =
        ${JSON.stringify(`rgb(${colors[1].join(',')})`)}`);
      const next = await until('updated fixture color decoded', async () => {
        await assertViewingOnly(host, viewer);
        const marker = await findVideoMarker(viewer, colors[1], region);
        const old = await findVideoMarker(viewer, colors[0], region);
        return marker && !old ? marker : null;
      });
      assert(Math.abs(next.x - firstMarker.x) < .015 && Math.abs(next.y - firstMarker.y) < .015,
        'Color update was not observed in the same owned fixture rectangle');
      report.secondMarker = next;
      report.framesAfterChange = await decodedFrameCount(viewer);
      assert(report.framesAfterChange > report.framesBeforeChange, 'Screen video did not advance');
    });

    await run('stop ends native screen tracks and transport with control always disabled', async () => {
      await host.evaluate(`globalThis.__screenTracks =
        document.querySelector('#preview').srcObject.getTracks()`);
      await host.click('#stop');
      await until('native screen tracks ended', () => host.evaluate(`
        document.querySelector('#preview').srcObject === null &&
        document.querySelector('#stop').disabled &&
        globalThis.__screenTracks.every(track => track.readyState === 'ended') &&
        globalThis.__localPairProbe.peers.every(peer => peer.connectionState === 'closed')`));
      await until('read-only viewer channel closed', () => viewer.evaluate(`
        globalThis.__localPairProbe.channels.length > 0 &&
        globalThis.__localPairProbe.channels.every(channel => channel.readyState === 'closed')`));
    });
    report.passed = true;
  } catch (error) {
    report.error = error.stack || String(error);
    // These snapshots contain only test UI/connection statistics, never
    // desktop content, screenshot data, full SDP, or other applications' titles.
    for (const [name, page] of [['host', host], ['viewer', viewer]]) {
      if (!page) continue;
      try { report[name] = await page.snapshot(); } catch { /* Closed page. */ }
    }
    throw error;
  } finally {
    report.finished = new Date().toISOString();
    if (viewer) {
      try {
        report.remoteControlEverEnabled = await viewer.evaluate(
          'globalThis.__localPairProbe.controlEverEnabled');
      } catch { report.remoteControlEverEnabled = null; }
    }
    await Promise.allSettled(ownedBrowsers.map(stopBrowser));
    await new Promise(resolve => { server.close(resolve); server.closeAllConnections(); });
    const owned = relative(resolve(tmpdir()), resolve(temporaryRoot));
    assert(owned && !isAbsolute(owned) && !owned.startsWith('..') &&
      basename(temporaryRoot).startsWith('xenon-screen-readonly-'), 'Unexpected temporary path');
    await rm(temporaryRoot, {recursive: true, force: true, maxRetries: 10, retryDelay: 200});
    for (const signal of ['SIGINT', 'SIGTERM']) process.removeListener(signal, requestStop);
    if (options.report) {
      await writeFile(options.report, JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
    }
    console.log(JSON.stringify(report));
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => {
    console.error(JSON.stringify({passed: false, error: error.message}));
    process.exitCode = 1;
  });
}
