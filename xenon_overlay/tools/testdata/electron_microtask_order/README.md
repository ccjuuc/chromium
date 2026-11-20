# Renderer IPC microtask ordering fixture

This local Electron application exercises real Xenon Browser-to-Renderer IPC.
It requires no network server or third-party application payload. Run it with
Node.js 22 or newer:

```text
node xenon_overlay/tools/testdata/electron_microtask_order/run.cjs --chrome <path-to-host-binary> --output <optional-results-directory>
```

The runner launches this directory with the host's `--xenon-electron-app`
option and inspects both page targets through its DevTools connection. Each
run creates a separate profile and uses an automatically assigned debugging
port. Omit `--output` to create a temporary results directory; an explicit
output directory must not already exist. The runner preserves `browser.log`
and `result.json` and closes only the browser it started.

The fixture opens two hidden windows with background throttling disabled:
one with context isolation enabled and one without it. Both use `sandbox: false`
and `nodeIntegration: false`; this suite checks IPC event ordering rather than
sandbox policy. The isolated window invokes the page callback through
`contextBridge`; the other invokes it directly from the preload.

Each window requests two native replies, using `webContents.send()` and
`webContents.postMessage()` respectively. In each synchronous reply callback,
the page schedules a Promise reaction to clear a Map, parses a complete HTML
document with `DOMParser`, and then writes to the Map. The Map must remain
available until the callback returns. The required order is:

```text
callback, after-dom, cache-written, callback-end, microtask
```

An early microtask checkpoint clears the Map during HTML parsing, producing
the same `null.set` failure as an application using a per-turn HTML cache.
The test checks both the successful Map write and the full event order; it
does not check the choice of native scope implementation.

The runner requires both isolation modes and all four passing cases, and exits
nonzero on failure or timeout.
Read `globalThis.__xenonMicrotaskResult` from each page's main world. Its
`done` and `ok` fields report completion and success, `checks` contains the
two transport results, and `errors` contains setup failures.
`globalThis.__xenonMicrotaskDone` also marks completion. Missing replies fail
after 15 seconds; absent page targets or a missing result require a runner
timeout as well. Console messages are diagnostic only and are not the
acceptance signal. The app keeps both windows alive for inspection; the
runner owns host shutdown.
