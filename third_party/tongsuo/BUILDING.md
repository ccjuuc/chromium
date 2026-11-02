# Tongsuo dependency build

The normal entry point is GN/Ninja. The upstream build system continues to own
source selection, configuration headers and assembly generation. The build is
static-only: the browser's GM module and the separate test peer each link the
archives into their own binary. No upstream libssl/libcrypto shared library is
a production dependency.

Provision the pinned ZIP once, before GN generation:

```sh
python3 third_party/tongsuo/fetch_source.py --output out/gm_deps/tongsuo-8.5.0/source.zip
```

An existing copy with the pinned SHA-256 can be placed at the same path instead.
This command is the only new entry point that performs a download. Ninja does
not use the network. The legacy `build_tongsuo.py` bootstrap is not used by GN;
its precompiled libraries are not inputs to this build.

Add these to the chosen output directory's existing `args.gn`:

```gn
enable_gm_tls_backend = true
gm_tongsuo_archive = "//out/gm_deps/tongsuo-8.5.0/source.zip"
```

Then run the normal `gn gen` and `autoninja -C <output> chrome` commands. The
dependency can be built alone with `autoninja -C <output> third_party/tongsuo:build`.
Setting the deprecated `gm_tongsuo_root` only enables the migration path; it no
longer accepts an existing library directory. Remove it when adopting the two
arguments above.

On Windows, the action uses the generated `environment.x64`, Chromium clang-cl,
the selected Visual Studio archiver, bundled Perl and GN's host NASM target.
The dynamic CRT follows `is_debug`: `/MD` or `/MDd`. Optional
`gm_tongsuo_make = "//path/to/jom.exe"` enables parallel upstream compilation;
otherwise the action uses nmake from the selected SDK environment. The chosen
make binary is tracked and hashed. `gm_tongsuo_jobs` defaults to 8.

On Linux and macOS, build on a native runner. The action uses Chromium clang and
llvm-ar, the GN-selected sysroot/SDK, and make/Perl. Linux selects the GNU target
triple for x64 or arm64, builds PIC and hides archive symbols. macOS propagates
the deployment target and SDK; x64 and arm64 use separate output directories.
These build recipes do not establish runtime qualification. In particular, the
ASYNC signing flow needs target libc/SDK, sandbox and native execution tests.

All outputs live under `<root_gen_dir>/third_party/tongsuo/upstream/`, isolated
from other configurations. The archive, generated build configuration, scripts,
compiler and host NASM target are explicit dependencies. A depfile tracks the
selected build tools, Windows environment, Perl modules and SDK/compiler headers.
An action rerun extracts a fresh verified source tree so a changed compiler, SDK,
architecture or configuration cannot reuse stale upstream objects. An unchanged
Ninja build does not invoke upstream make. `build-manifest.json` records the
source hash, target, configuration, exact tools and archive/header hashes.

For a version update, change `source.json`, regenerate `public_headers.gni` from
the ZIP's `include/openssl/*.h` and `*.h.in` members, and run the dependency and
backend/coexistence suites on each supported target. Preserve upstream LICENSE
and NOTICE. Do not patch algorithms simply to make an interoperability test pass.

Offline boundary tests:

```sh
python3 third_party/tongsuo/build_dependency_test.py
```
