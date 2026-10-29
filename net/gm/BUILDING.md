# Building national TLS with Chromium BoringSSL

The production build uses the Chromium GN/Ninja toolchain and its existing
BoringSSL target. No Tongsuo Configure, make, jom, provider or runtime DLL is required.
Use the existing `out/Release_64` output directory requested for this workspace.
Preserve its existing `args.gn` settings and enable national TLS for x64:

```gn
target_cpu = "x64"
enable_gm_tls = true
```

`enable_gm_tls` is the single source of truth for the `XENON_GM` code macro.
Chromium integration code includes `net/gm/gm_buildflags.h` and uses
`#if BUILDFLAG(XENON_GM)`. BoringSSL is independently buildable and uses a private
`XENON_GM` compiler define from the same GN argument; that raw define must not
be propagated to Chromium consumers or mixed with its generated buildflag.
Do not add a second, independently configurable `-DXENON_GM` switch.

Set `enable_gm_tls = false` and regenerate the same output directory to disable
GM transport, algorithm registration, policy/USBKey activation and GM test-page
presets. Public constants, data types and Mojo interfaces remain stable; their
presence does not enable the feature. Disabled builds reject GM-specific entry
points and retain ordinary HTTPS and the general certificate diagnostic page.
The focused network/service/browser test targets support both configurations;
GM-only tests are replaced with disabled-mode assertions where appropriate.
After testing a disabled configuration, restore the intended argument and
rebuild before distributing its executables. Tests and package evidence apply
only to the configuration and artifact hashes recorded for that run.

Then run `gn gen out/Release_64` and `autoninja -C out/Release_64 chrome`.
The local output is now an ordinary directory on H:. Reusable build artifacts
were migrated, GN was regenerated with the retained configuration, and the
browser and test executables were linked and tested there. The installer was
also built, its 50 archive/import/payload-identity checks passed, and four paired
performance rounds completed. Exact evidence and remaining interoperability
and hardware limits are in the current test report.
The same production EXE also passed a headless CDP startup/local-data-page/
graceful-exit check. This does not establish TLS, visible GUI or hardware
acceptance. Two earlier `--dump-dom` timeouts remain unresolved and recorded;
the passing CDP check required no product-code change.

The browser has no external GM-library payload. Existing bundled applications
retain their own OpenSSL dependencies, audited by exact path and canonical-source
hash; the single-library browser design does not mean the whole installer is
free of OpenSSL. Package auditing does not execute the installer or qualify an
installation/upgrade.

Earlier migration checks ran in `out/gm_release`, then a junction to
`C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/Release_gm`.
That junction has been removed. Historical evidence keeps its original paths,
including the earlier C: location. Subsequent work uses
`out/Release_64` only. Current Release_64 results have their own retained test
summaries in `out/gm_boringssl_tools/release64-test-evidence/`.

SM source files are registered in BoringSSL's `build.json` and generated source
lists. Maintainers regenerate those lists with the upstream `util/pregenerate`
Go tool after changing them; ordinary consumer builds use the checked-in generated
files. This does not introduce Go as a new browser runtime dependency.

The nested BoringSSL checkout is a separate Git repository. Preserve its local
changes using [the checked-in patch package](../../third_party/boringssl/gm_patch/README.md).
Its explicit file list, baseline and replay check prevent a parent repository
commit from silently losing the algorithm and protocol changes. Export/check
is a maintenance operation, not a prerequisite of ordinary GN builds.

## Library and integration checks

```sh
autoninja -C out/Release_64 boringssl_crypto_tests boringssl_ssl_tests boringssl_pki_tests
```

Run all three executables with `--test-launcher-retry-limit=0`. Exercise both
normal CPU dispatch and the SM4 portable fallback. Standard TLS tests remain
part of the same suite; GM-only results are insufficient to release the browser.

The Windows integration tests use a separately provisioned independent Tongsuo
peer. Point this test-only argument to a directory containing `gm_backend_smoke.exe`,
`libssl-3-x64.dll`, `libcrypto-3-x64.dll` and the peer's two VC runtime DLLs:

```gn
gm_interop_peer_dir = "C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/facade-verification-peer/build/Release"
```

This is the current local test-peer directory; use an equivalent absolute path
on another machine. It is not a production link dependency. Its harness is
built from the current `net/gm/tests/backend_smoke.cc` and linked to the pinned
independent Tongsuo 8.5.0 libraries. The bundle's `facade-verification-peer/manifest.json`
records source/executable/library hashes and reproduction commands. Manifest
creation itself is not a test pass; see the current result log.

`out/gm_interop_reference` retains the old harness and its hash manifest as a
comparison baseline. Do not use that old executable to claim coverage of newly
added assertions. Source-archive metadata remains in `third_party/tongsuo`.
Neither peer directory is shipped with the production browser.

```sh
autoninja -C out/Release_64 gm_integration_tests gm_boringssl_test_backend gm_crypto_benchmark
python net/gm/run_tests.py --build-dir out/Release_64 --include-network --include-content --include-browser
```

The test-only DLL exposes the in-process facade to the independent peer. It must
never be packaged with Chrome. Browser and network tests execute the same linked
facade directly. Use `run_tests.py` only after checking its available layers match
the current migration; historical result JSON and old executables do not validate
new sources.

The focused network target also runs Chromium's ordinary SSL socket and session
cache tests. `--include-benchmark` additionally records primitive throughput and
the independent peer's software-key handshake measurements. Run measurements
with compilation stopped. The memory-BIO benchmark excludes network round trips,
USB, browser IPC and user PIN entry; its test DLL size is not browser size growth.
Current execution records are in [BORINGSSL_TEST_RESULTS.md](BORINGSSL_TEST_RESULTS.md).
The older [TEST_RESULTS.md](TEST_RESULTS.md) belongs to the removed production
Tongsuo integration and does not validate the single-library migration.

For native macOS/Linux qualification, build the actual target platform with its
Chromium compiler and SDK/sysroot. The Windows external-peer and SKF tests are
not cross-platform binaries. Physical Feitian SDK/PIN/hotplug and latency tests
remain required when hardware becomes available.
The existing vendor-Key loader, utility service and native PIN integration are
Windows x64 implementations. macOS/Linux USBKey use requires a corresponding
service/driver integration in addition to native crypto/TLS qualification.
