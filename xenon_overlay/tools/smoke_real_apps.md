# Real TH / PL-E main startup diagnostics

This harness diagnoses main-script loading in a unit-test process. The packaged
TH / PL-E native SDKs are not validated for that process image; use the full
browser procedure below to validate application startup.

Build `ipc_main_container_unittests`, then run from the source root:

```powershell
node xenon_overlay/tools/smoke_real_apps.cjs --out out/Release_64 --app all
```

Options: `--app th|ple|all`, `--native <test-executable>`,
`--results <artifact-directory>`, and `--observe-ms 0..10000` (default 1000).
The two tests skip in the normal suite unless `--xenon-app-smoke-root` is set.

## What runs

Each application runs in a separate native test process, through the real
`XenonServiceImpl`, main V8 isolate, filesystem/CommonJS loader, and
`XenonNodeExecutor` addon hooks. No application source or addon is rewritten.

| App | Entry relative to build directory | Native runtime root |
| --- | --- | --- |
| TH | `thunder_2025/resources/app/out/main.js` | `thunder_2025` |
| PL-E | `xenon_player/main/main.js` | `xenon_player/main` |

The packaged `Thunder.exe` / `xmp.exe` supplies executable identity only.
`userData`, APPDATA, LOCALAPPDATA, TEMP, and TMP use per-run directories.
This is not a native-code sandbox: only run trusted application payloads.

## Boundaries and results

No browser observer is installed. BrowserWindow creation therefore fails
explicitly; no HWND, renderer page, login result, network response, or playback
success is fabricated. Native addon initialization and calls are real.
Main networking uses whatever the actual compatibility runtime implements.

The runner records `process.log`, `report.json`, and a combined `summary.json`.
Main evaluation failure, ready-listener exceptions, unhandled rejections,
process crashes, and timeout make the smoke fail. An evaluation pass means only
that main evaluation and the bounded observation period completed. Every report
sets `business_startup_verified: false`; UI and application startup still need
the browser test below. Full exception stacks remain in `process.log`.

## Native test-host limitation

The production `node.exe` / `node.dll` sidecars forward their N-API exports to
`xlbrowser.dll`. The commercial `thunder_helper.node`, `player_helper.node`, and
`xmp_helper.node` binaries delay-load `node.exe`. In contrast,
`ipc_main_container_unittests.exe` contains its own N-API/V8 implementation.
The repository's `test_addon` has a dedicated delay-load hook which redirects
`node.exe` imports to the current test EXE. That hook is not a guarantee made by
the packaged commercial SDKs.

Consequently, a commercial addon can resolve its imports to a different runtime
image from the one that created the test's `napi_env` and V8 isolate. Unit-test
addon success does not establish commercial SDK compatibility with this host.
Do not change production addon loading to accommodate the diagnostic EXE.

The 2026-09-16 packaged-payload runs in
`out/real-app-smoke-zlib-20260916` crashed during native initialization/calls:
TH exited with `0xC0000005`; PL-E exited with `0xC0000409`. Both are failed smoke
runs, with main evaluation incomplete. The runtime-image difference explains
why the test host is unsuitable as production startup evidence; the exact
native crash cause remains unconfirmed without a symbolized crash stack.
The runner requires a zero process exit code, completed evaluation and
observation, and no ready-phase errors before setting `smokePassed`. It never
sets `businessStartupVerified` to true.

## Full browser validation

The current build's Chrome target produces `xlb153.exe` and `xlbrowser.dll`.
Rebuild the entire target so the service, renderer transport, and resource pak
are from the same source revision:

```powershell
autoninja -C out/Release_64 chrome
Start-Process -FilePath (Resolve-Path out/Release_64/xlb153.exe) -WindowStyle Hidden -ArgumentList '--user-data-dir=H:\chromium_142\src\out\real-app-browser-profile','--remote-debugging-port=9222','--enable-logging','--log-level=0'
```

Use a fresh profile path for the run. Activate TH / PL-E through their normal
sidebar entry, which starts the configured Utility container and lets the real
main create its BrowserWindows. Navigating directly to either renderer WebUI
does not test this startup path. Check the service initialization log, renderer
window creation and console, then application functionality. CDP can inspect
the resulting pages on port 9222. The smoke runner never launches the browser.

### Existing PL-E WebUI activation

For browser diagnostics when sidebar automation is unavailable, the existing
`chrome://xenon-player-electron/` WebUI exposes the same player activation through
its Mojo `PreparePlayerHost` method. In that normal browser tab's main-world
DevTools context, run:

```js
await import('/require.js');
await globalThis.__xenonPageHandler__.preparePlayerHost();
```

The controller calls `ShowXenonPlayerElectron` and returns the expected
`errorMsg` of `Open the player from the sidebar, not as a browser tab` to the
diagnostic tab. The real app must still create its own BrowserWindow; this
response does not indicate application startup success. This method always
activates PL-E, including on the shared Thunder controller, so it must not be
used as a Thunder activation method. No dedicated Thunder startup switch or
WebUI method calling `ShowThunder2025` is currently present; its direct call
site is the normal sidebar shortcut.
