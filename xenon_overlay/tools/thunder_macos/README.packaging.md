# Mac TH resource assembly for 153

`package_adapter.cjs` is a resource-only stage in the Xenon packaging wrapper.
Its result belongs to the **153 compatibility build**, not an unmodified official
TH distribution. The source path is used only to resolve the installed official
`@electron/asar` library. This stage does not rebuild TH, edit source files,
rewrite JavaScript, replace ASAR archives, or change source maps.

```sh
node xenon_overlay/tools/thunder_macos/package_adapter.cjs \
  --source /absolute/installed/thunder-source \
  --bundle /absolute/staging/Thunder.app \
  --report /absolute/staging/packaging-adapter.json
```

The source and bundle must already exist. The bundle must be an unpublished
staging copy outside the source checkout. Use the Node version and dependencies
selected for the official TH workflow; there are no extra dependencies. The
optional report must be a new file outside both the source checkout and app.

Original Mac bundles contain `thunder-pan-plugin.asar/static/`, while their
JavaScript requests assets through `../../plugins/thunder-pan-plugin/`.
The adapter uses the official ASAR reader to copy the original `static/` files
into the sibling `plugins/thunder-pan-plugin/static/` directory. Every copied
file is compared byte for byte with its archive entry. The archive itself,
including `index.js`, retains its original bytes.

Only missing resource files are created. Existing expected files must already
match the archive. Unknown entries inside the target `static/` tree, conflicting
contents, unsafe archive paths and symbolic links cause an explicit failure
before installation. Files outside the target `static/` tree are preserved.
Repeated execution reuses matching files without rewriting them.

The stage also adds `Contents/Resources/xenon-native-player.json`. This declaration
requires the matching `xunlei.mac-player` adapter supplied by the wrapper's
separate native/runtime compatibility stages; the declaration does not add an
adapter itself. Other compatibility changes made by that wrapper must be listed
in the overall build report.

The JSON report records the unchanged plugin archive hash, copied/reused counts,
and hashes for every extracted resource and the declaration. **The wrapper must
sign the completed staging app and verify its signature before publication.**
This stage does not deploy, launch, modify user profiles, notarize, or upload.

Run the resource assembly checks with the official ASAR dependency:

```sh
THUNDER_MAC_TEST_SOURCE=/absolute/installed/thunder-source \
  node --test xenon_overlay/tools/thunder_macos/package_adapter_test.cjs
```

The tests build only tiny ASAR fixtures. They check byte preservation,
idempotency, unknown-file protection and path/link rejection; they do not compile
or export TH. Without the environment variable, only argument and archive-path
validation tests run.

## Browser runtime instead of Electron

After resource assembly, `strip_electron_runtime.py` removes Electron from the
unpublished staging copy:

```sh
python3 xenon_overlay/tools/thunder_macos/strip_electron_runtime.py \
  --bundle /absolute/work/artifacts/Thunder.app \
  --staging-root /absolute/work/artifacts \
  --report /absolute/work/runtime-pruning.json
```

`--check` performs the same identity and Mach-O dependency inspection without
changing the app. The staging root must contain the bundle. The tool identifies
the Electron launcher and helper processes by their bundle identities and
`otool` dependencies, and removes Electron Framework, Squirrel, Mantle and
ReactiveObjC only after verifying that retained native code does not need them.
The Electron icon and obsolete root signature/PkgInfo are also removed.

TH's native modules, MacPlayer, MachXPC, Sparkle, WCDBSwift, sqlcipher, sqliterk,
extensions and their existing signatures remain byte-for-byte unchanged.
The report records every removed path and retained Mach-O dependency/hash.
Removal is reversible until final validation succeeds; failures restore the
staging bundle. Neither the input source nor the running application is used as
a mutation target.

The resulting `Thunder.app` is a **BNDL resource bundle without an executable**.
Its identifier, name, version and icons remain in Info.plist; the obsolete
`CFBundleExecutable` and `AtomApplication` principal class are removed. The empty
`Contents/MacOS` directory remains for the browser's execution context. The
deployment descriptor supplies `application` and omits `executable`, so 153
provides the runtime. This bundle cannot be launched independently.

Sign only the completed resource bundle, preserving nested native signatures:

```sh
codesign --force --sign - /absolute/work/artifacts/Thunder.app
codesign --verify --deep --strict /absolute/work/artifacts/Thunder.app
```

Do not use `--deep` when signing: it would modify the retained native modules and
extensions. Signing a BNDL with an empty MacOS directory has been verified with
both a minimal fixture and a complete pruned TH copy. The complete-copy check
removed 209,045,213 bytes and preserved all 20 retained Mach-O hashes and
`out.asar` bytes; strict deep verification passed.

Run the isolated pruning tests with:

```sh
python3 -m unittest discover -s xenon_overlay/tools/thunder_macos \
  -p strip_electron_runtime_test.py -v
```

## Local runtime archive

`build_thunder_macos.py` signs and verifies the resource bundle, then creates a
PAX `tar.xz` archive with only `Thunder.app/` and `thunder_2025.xenon.json` at its
root. The archive keeps ordinary modes, relative Framework symlinks and signed
file bytes. Host ownership, timestamps and extended attributes are omitted.
The writer rejects hardlinks, special files, special permissions, and absolute,
escaping or dangling symlinks, then reads back every archived file to verify it.

The output directory, archive and standard SHA256 file are published together;
an installation error restores all previous outputs. The standalone
`archive_thunder_macos.py` entry point can archive an existing resource directory
and publishes its archive/checksum pair with the same rollback behavior. An
incomplete rollback keeps recovery files and reports an error. Both entry points
share the default output's build lock.

GN consumes the archive and checksum, verifies their compressed bytes, validates
the member paths and extracts into temporary staging before updating the browser
resources. The unpacked build directory is not a GN input. Logs, build reports
and source maps next to the bundle are excluded from the archive.

The resource directory, `.tar.xz` and `.sha256` are local, Git-ignored build
outputs. **Do not commit TH resources or ASAR keys.** Commit the tools, patches,
host code and documentation; prepare authorized build inputs separately. Without
the optional local archive/checksum, a fresh checkout builds the host by default.

Run the archive and builder publication checks on small isolated fixtures:

```sh
python3 -m unittest discover -s xenon_overlay/tools \
  -p archive_thunder_macos_unittest.py -v
python3 -m unittest discover -s xenon_overlay/tools \
  -p build_thunder_macos_unittest.py -v
```
