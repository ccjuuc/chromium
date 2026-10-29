# Windows hosted TH and PLE runtime packaging

For the build/repackage/verify entry point, see [Windows packaging](windows_packaging.md).

The Windows `thunder_2025` copy action opts into
`copy_runtime_tree.py --strip-windows-electron-runtime`. The original imported
release under `xenon_overlay/resources/thunder_2025` is left intact. Its build
output is a projection with the redundant Electron engine and its root assets
removed. `xenon_player` (PLE) opts into the same action with
`--windows-electron-profile=ple`. Both canonical imported releases remain intact.
Other applications and macOS packaging retain their own workflows.

`windows_electron_runtime.py` is the shared policy for copying and installer
verification. It removes the root XDASKernel/Electron engine, Chromium pak and
locale assets, ICU data, V8 snapshots, FFmpeg, ANGLE, and software Vulkan runtime.
It does not remove same-named DLLs in the player's private directory. All
retained PE executables, DLLs and addons are checked for both ordinary and delay
imports of removed DLLs. A new dependency aborts copying before changing the
previous output. These checks cannot discover arbitrary computed LoadLibrary
paths; actual application startup remains part of release verification.

PLE's profile also removes the packaged `debug.log`, but preserves the **root**
`vulkan-1.dll`: its root `libplacebo-274.dll` directly imports that copy. The
private `player/` copy serves the other libplacebo and is retained too. PLE's
root EGL/GLES DLLs are preserved as well: libplacebo can load them dynamically
for its OpenGL backend, which a static PE import scan alone would miss. Do not
apply TH's root Vulkan removal rule to PLE or deduplicate these business
libraries without checking their loading paths and playback behavior.

Retained intentionally:

- `Thunder.exe`: the descriptor uses it for executable identity and file version
  metadata. Xenon hosts the JavaScript itself; this launcher is not an independent
  usable Electron application after removing XDASKernel.
- The application's signed/encrypted ASARs, `.node` addons and business helpers.
- The player's own codecs, Vulkan DLL, and D3D compiler dependencies.
- License and attribution files, including `LICENSES.chromium.html`.
- The browser's `node.dll` / `node.exe` compatibility forwarders, which are Xenon
  shims rather than the original standalone Electron runtime.

The installer selects `--xenon-windows-hosted-payload=thunder_2025` alongside its
canonical source. With `--xenon-stage-hosted-sources`, it generates its own clean
projection directly from that source; a running application's build-directory
logs and caches are not read or modified. It requires exact hashes for every
retained file and rejects runtime files reintroduced into staging. Without that
flag it also requires the build cache to match the projection. Generic application
imports remain byte-for-byte copies. Re-importing a complete upstream TH release
is safe: the next copy action reapplies this policy and checks dependencies.
PLE uses `--xenon-windows-hosted-payload=xenon_player=ple` so installer verification
uses the same explicit profile as the copy action. Its `xmp.exe` identity file,
addons, media libraries and signed ASARs remain unchanged.

Run the focused regression suites from the source root:

```powershell
python -m unittest discover -s xenon_overlay/tools -p '*runtime*unittest.py'
python -m unittest discover -s xenon_overlay/tools -p 'create_xenon_installer_archive_unittest.py'
python -m unittest discover -s xenon_overlay/tools -p 'import_electron_app_unittest.py'
```

The packaging entry point records the current staging manifest and verifies
the archive hashes and embedded payload automatically. Startup checks
must use the production browser and its matching N-API forwarders; the standalone
IPC test EXE is not a substitute for a production commercial addon host.
