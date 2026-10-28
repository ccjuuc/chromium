# Electron host integration smoke

This fixture exercises the actual browser, Utility, renderer and Service Worker
without loading TH or modifying its source. It requires a built Windows host
with Xenon enabled and Node.js 22 or newer for the CDP client.

From the Chromium source root:

```powershell
./xenon_overlay/tools/testdata/electron_host_smoke/run.ps1 `
  -ChromePath ./out/Release_64/xlb153.exe `
  -NodePath node
```

The runner uses loopback ports 9223/9339 and refuses to start if either is busy.
It creates a separate profile and writes `result.json`, `chrome.log`, and server
logs under `out/electron-host-smoke`. Use `-OutputDirectory` to place these
artifacts elsewhere. It closes only the processes it started, including on
failure. Run after linking the browser, without another Ninja writing that
output directory.

The test requires all of the following to pass:

- Stable browser-authored WebContents/mainFrame identity and the expected Session.
- A Session preload in an isolated context; Node and native IPC absent from the page.
- A custom protocol response with all 11 binary bytes preserved.
- Hidden rendering with throttling disabled, restoration of hidden state when
  enabled, and visibility after showing the native window.
- A Service Worker preload in a separate realm, with trusted Worker IPC identity,
  working timers/microtasks, callback ordering, and cancellation.
- An ordinary browser tab's Service Worker initializes and answers a message
  without receiving the app preload or native transport. Its registration and
  tab are removed after the probe.
- Clean shutdown while hidden background rendering is enabled and a cross-site
  navigation is pending, without new dumps. The server holds the response so
  Chromium's speculative frame remains uncommitted during teardown.

The runner exits nonzero on any failed check or a 45-second timeout. Page checks
and the native `suite-complete` result must both succeed. A missing preload or
worker is a failure, never a skipped check. The manual `cdp.cjs` commands
`inspect`, `native`, `screenshot`, and `close` are available while a fixture
browser is running; screenshots go to the current working directory.
