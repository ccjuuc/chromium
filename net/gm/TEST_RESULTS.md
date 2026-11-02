> 历史结果：以下记录来自撤销铜锁前的双库实现，不代表当前 BoringSSL 移植已通过。

# GM integration validation record

## Review repairs validated on 2026-10-28

Chromium **153.0.8004.0**, Windows x64 Release, backend ABI 3, Tongsuo 8.5.0.
The production browser and focused test targets build successfully. The final
incremental dry-run reports no work to do. All ten software layers pass with
automatic test retries disabled:

| Layer | Current result |
| --- | --- |
| Network, both connection pools, cache and routing | 2256/2256 PASS |
| Network Service and real SM2 process TLS | 62/62 PASS |
| Content request/page lifecycle | 8/8 PASS |
| Browser unit tests, trust, certificate chains and native PIN | 81/81 PASS |
| Real browser navigation, cancellation and page-modal PIN | 27/27 PASS |
| Isolated Key service, revocation and watchdog | 25/25 PASS |
| Connection profile | 113 checks PASS |
| SKF adapter | 100 checks PASS |
| Standalone backend | 54/54 PASS |
| BoringSSL/backend coexistence | PASS |

The repair coverage includes credentialless requests; continuously rearmed
Windows trust notifications; local client-chain completion and same-name CA key
rollover; lazy request-owned interaction; preconnect adoption; independent
connection-pool cancellation; provisional PIN authorization followed by commit
or cancellation; and USB/certificate generation invalidation. The 27 browser
cases include four native page-modal dialog tests and eight page-close/navigation
tests that discard late PIN or SDK replies. The Network Service sandbox remains
enabled. The browser tests use Chromium's standard Windows delayed-import config.

Current run: `20261028T054100Z-251db071`. Raw results and before/after binary
hashes are retained under `out/gm_release/gm-fix-evidence/` for this run, with
`gm-fix-test-binding.json` as the index. Failed intermediate runs have separate
directories and are excluded from the passing counts. The final browser suite
took 31.266 seconds; this is test runtime, not a browser performance benchmark.

Build and run:

```powershell
third_party/ninja/ninja.exe -C out/gm_release -j16 gm_integration_tests chrome
python3 net/gm/run_tests.py --build-dir out/gm_release --include-network --include-browser
```

GN header checks, scoped checkdeps, and diff whitespace checks pass. Component
export compilation was checked, but a full component Chromium link was not run.
No physical Feitian Key or signed vendor SDK is available. Software simulation
uses real SM2 cryptography but does not qualify hardware PIN/PnP behavior.
The independent GmSSL interoperability and performance measurements below were
not rerun for this repair; their dates, failures and hashes remain historical.
Successful ordinary HTTPS tests add no GM handshake or Key work; there is no
claim of measured zero CPU or memory overhead.

Current backend: **4,468,224 bytes**; SHA-256
`cfb7958e7d65efedb1379870fe4ec84cb1b54d6a558c6587b9c174a8f9d59cc7`.

Current installer generation and audits: **PASS**. The installer is
`out/gm_release/xlb153_installer_1.0.0.1.exe`, **450,658,304 bytes**; SHA-256
`3aaaeaba748b7e824e026d58900fc0fbbe1856d5574b0b7d04cb42b449e84da0`.
All **797 payload files** match staging sizes and CRCs. The packed archive
contains the exact audited `chrome.7z`, and installer embedded-resource audits
pass. Production browser, browser DLL, chrome_elf and GM backend hashes are
bound to this test run. The GM directory contains only the backend, its two
VC runtimes and license/notice files; no GM test tools, simulator, test roots or
private-key PEM content are packaged. Evidence is indexed by
`out/gm_release/gm-fix-package-binding.json`, with raw audits under
`out/gm_release/gm-fix-evidence/20261028T054100Z-251db071/package-e78e4135/`.
The installer was not executed; installation and in-place upgrade are untested.

## Historical validation from 2026-10-27

The remaining sections preserve the prior validation record. Their counts,
measurements, installer claims and binary hashes do not identify this repair.

Recorded 2026-10-27 for Chromium **153.0.8004.0**, Windows x64, GM backend
**ABI 3** and pinned Tongsuo **8.5.0**. The checkout directory name contains
`142`; the compiled Chromium version is 153.

The production browser build, all local software suites below and regenerated
installer audits have passed. The independent GmSSL interoperability failure
and unavailable physical Key remain explicit limits.

## Completed software checks

| Layer | Result |
| --- | --- |
| Chromium network, pools, cache and automatic routing | **1800/1800 PASS**, retries disabled |
| Network Service configuration, identity provider and real SM2 process TLS | **45/45 PASS**, retries disabled |
| Browser policy, KeyManager, native PIN dialog and SSL configuration manager | **66/66 PASS**, retries disabled |
| Standalone GM backend | **54/54 PASS**, ABI 3 |
| BoringSSL/backend coexistence | PASS |
| GM network and browser target GN header checks | PASS, including generated headers |
| Browser navigation, sandbox/process checks and discovery boundaries | **15/15 PASS**, retries disabled |
| Connection profile | **113 checks PASS** |
| SKF adapter and loader faults | **100 checks PASS** |
| Isolated Key service / real SM2 / revocation / watchdog | **13/13 PASS** |
| Production installer archive, embedded resources and tested-binary binding | PASS |
| Real Feitian USBKey, signed vendor SDK and firmware | Not available; not hardware-qualified |
| Installer execution and in-place upgrade | Not run against the existing installation |

The final combined run used the rebuilt test peer and all completed fixes. Raw
results are under
`out/gm_release/`:

- `gm-auto-all-final-report.json` and `gm-auto-all-final-tests.log` for all nine layers;
- `gm-auto-net-final-results.json`, `gm-auto-network-service-final-results.json`
  and `gm-auto-browser-unit-final-results.json`;
- `gm-auto-browser-final-results.json` for browser navigation/discovery;
- `gm-auto-final-binaries.json`, recording production and test executable hashes.

The separate profile/SKF/Key-service rerun is recorded in
`gm-auto-core-final-report.json`, `gm-auto-core-final-tests.log` and
`gm-auto-key-service-final-results.xml`. It includes a hung driver terminating
only its dedicated process and a crashed driver closing its signing capability.

The expanded 66-case browser unit suite is recorded in
the combined report and `gm-auto-browser-unit-harness-report.json`. SSL configuration manager tests run
in the unit runner with Mojo initialized; they are not plain unit fixtures mixed
into the in-process browser-test launcher.

Failed intermediate attempts are retained separately; they are not included in
the passing counts above. Earlier validation text is retained in
`gm-auto-previous-validation-record.md` in the build directory.

## What these checks establish

Ordinary successful HTTPS uses one standard TLS handshake. Tests cover both
connection pools, precise allowed/rejected protocol failures, the complete
seven-byte TLCP alert classifier, bounded attempts, stale/concurrent hints,
actual proxy routes, NAK/privacy partitions, hint expiry and resource bounds.
GM-only invalidation preserves ordinary connections and tickets, including
ordinary HTTPS proxies sharing a name with a GM destination.

Fresh DNS ECH information takes precedence over automatic GM hints. Tests check
reselection into the standard pool before creating a GM SSL socket, preservation
of SVCB restrictions, non-ECH ALPN filtering, exhausted attempt budgets and
unchanged explicit managed behavior. Certificate/client-auth errors clear the
matching hint without protocol retry or accessing unavailable proxy callback
data; other proxy/NAK/privacy hints remain usable.

The candidate tests cover real public SM2 fixtures, CA-name matching through a
reachable issuer chain, distinct TLCP role keys, validity, KU/EKU, signature
schemes and bounded inputs. TLCP requires explicit role KeyUsage; RFC 8998
permits an absent extension. This selection filter does not replace chain
verification or proof of private-key possession.

Identity-provider tests cover request/profile isolation, live authorization,
optional empty selection, revocation and expiry. A valid identical identity
retains the same grant/key IDs across compatible CertificateRequest changes,
so this change does not repeatedly invalidate its session ticket. Different
identity contents or revoked/expired grants are not reused.

The 54 backend cases include TLCP/RFC 8998, same-handshake asynchronous identity
selection, asynchronous signing, cancellation, exact retry classification,
independent signing/encryption role verification, hostname/CRL failures,
resumption, authenticated close, real TCP H1 and RFC 8998 H2 multiplexing. The
invalid OpenSSL configuration/provider environment remains part of the runner.

The 15 browser cases comprise four managed TLCP/RFC 8998 navigation/reconnect
cases, two automatic TLCP navigation/reconnect cases, six ordinary HTTPS cases
with zero Key work, unsigned production-driver rejection, and two generic-alert
rejection cases. The real Network Service sandbox remains enabled and process
integrity is checked. Software SM2 signing runs in a separate test Key process;
navigation uses an injected PIN callback. Actual PIN UI behavior is covered by
the separate native-dialog unit tests.

The automatic positive peer explicitly returns TLCP `protocol_version` for a
non-TLCP ClientHello. The unmodified default Tongsuo peer instead returns
`handshake_failure`; its two negative cases require the original protocol error,
zero Key operations and no TLCP ClientHello retry. Such a generic-alert server
needs an explicit managed protocol rule under this strict discovery policy.
Replay of a real BoringSSL ClientHello verified both wire alerts (2/2), recorded
in `gm-auto-peer-alert-report.json`. The positive test is not a claim that default
Tongsuo or every pure TLCP deployment can be discovered automatically.

## Independent interoperability

The independent peer is GmSSL **3.2.0**, commit
`7c9f02904ef33e59c87b4f16621cc8fd434e7579`. Its cached source/archive and peer hash
were checked, and fresh certificate/key fixtures were generated for the ABI 3
rerun. Neither upstream library was modified to make the test pass.

| Protocol | Client authentication | Result |
| --- | --- | --- |
| TLCP | None | PASS |
| TLCP | Signing and encryption certificates | **FAIL**: GmSSL treats the second client leaf as a CA |
| RFC 8998 | None | PASS |
| RFC 8998 | External SM2 signing | PASS |

This is **3/4**, not a universal interoperability pass. Verification was not
weakened, and the encryption leaf was not marked as a CA. A server needing a
single-client-certificate profile remains outside the currently tested dual
certificate profile. The runner records failure and returns nonzero.

Evidence: `gm-auto-interop-report.json`, `gm-auto-interop-tests.log` and
`gm-auto-interop-rerun.py`. The tested backend SHA-256 is
`3fe93b8bc1c552b99830d3be269e3ab9e5f62f9fe82ef17cb637666f5f845a82`.

## Ordinary HTTPS routing measurement

Both configurations below use the same GM-enabled binary. This compares
automatic discovery off/on; it is not a comparison with a separate unmodified
Chromium build. Compilation and the other test suites were idle during the
measurement. Each observation executes 200 sequential simulated transactions;
one warmup run is excluded and five runs are summarized. Every observation
asserts 200 standard TLS handshakes for 200 requests.

| Pool | Auto off | Auto on, empty hints | Auto on, 256 hints |
| --- | ---: | ---: | ---: |
| Legacy | 19.980 us/request | 21.240 us/request | 21.410 us/request |
| Stream | 19.630 us/request | 19.485 us/request | 20.775 us/request |

Values are medians of the five per-run averages. The full hint-table difference
is approximately 1.1--1.5 microseconds per simulated request. Individual ranges
overlap; the lower empty-table stream result is not evidence of a speedup.
These mock sockets exclude real TLS cryptography, DNS, WAN RTT, USB, PIN and
browser page rendering. There is no timing pass threshold and no claim of
exactly zero real-world performance impact. The stronger functional result is
that successful ordinary HTTPS adds no GM handshake or Key work.

Reproduction and raw observations: `gm-auto-benchmark-routing.py`,
`gm-auto-routing-benchmark.json` and the six corresponding per-run logs/JSONs.

## Backend size and local measurements

Current backend DLL: **4,468,224 bytes (4.26 MiB)**. Two x64 CRT DLLs add
**228,728 bytes**; the three DLLs total **4,696,952 bytes (4.48 MiB)**. License
and notice add 10,297 bytes. This excludes the Feitian SDK, test executables,
simulator and final compressed-installer overhead. The audit confirms one C ABI
export and no runtime libssl/libcrypto DLL import.

Measurements used the current ABI 3 backend on the Intel Core i9-13900HX host,
with builds/tests idle, 200 handshakes per sequential mode, a 32-connection batch,
and 64 MiB aggregate bidirectional payload using 16 KiB records. Both endpoints
run in-process over memory BIOs with real software SM2 external signing. Trust
checks remain enabled. USB, IPC, PIN and network latency are excluded.

| Measurement | TLCP | RFC 8998 |
| --- | ---: | ---: |
| New context, full mTLS, median / p95 | 6.32 / 6.98 ms | 5.44 / 6.22 ms |
| Reused context, full mTLS, median / p95 | 5.41 / 5.97 ms | 4.54 / 5.11 ms |
| Reused context, resumed mTLS, median / p95 | 2.28 / 2.73 ms | 2.74 / 3.21 ms |
| Reused context, server auth, median / p95 | 3.80 / 4.20 ms | 3.23 / 3.87 ms |
| 32 concurrent full mTLS, whole batch | 172.73 ms | 157.05 ms |
| 32 concurrent resumed mTLS, whole batch | 74.18 ms | 80.98 ms |
| Aggregate bidirectional payload throughput | 86.61 MiB/s | 85.36 MiB/s |

The two endpoints share CPU resources. Concurrent connections advance on one
thread; this is not 32 physical Keys or workers. Throughput includes both
endpoints and harness copying/validation, not browser download speed. DLL/API
load observation was 0.47 ms with OS file caching uncontrolled, not a cold-start
measurement. These results do not compare Tongsuo against GmSSL, OpenHiTLS or a
BoringSSL fork.

Evidence: `gm-auto-backend-benchmark.json` and
`gm-auto-backend-benchmark-report.json`. The previous ABI 2 benchmark is retained
as `gm-benchmark-abi2.json` and is not substituted for current results.

## Installer artifact verification

The rebuilt Windows x64 `xlb153_installer_1.0.0.1.exe` is **330,522,624 bytes**,
SHA-256 `bafa6049e420cff411fbae5b07e79199bb67970dba67edc4d759cb00da232377`.
It contains 534 staged files, including the five intended GM runtime/license
files totaling 4,707,249 uncompressed bytes. Simulator DLLs, test peers/programs,
test roots and private-key PEM files are excluded by the audited staging rules.

Both `chrome.7z` and `chrome.packed.7z` passed 7-Zip integrity checks. Streaming
extraction verified that the packed archive contains the exact current inner
archive. The installer embedded-resource hashes match the packed archive and
setup payload. The staged launcher, browser DLL and GM backend hashes match
the final tested binary record.

Evidence: `gm-auto-package-audit.json`, `gm-auto-installer-audit.json`,
`gm-auto-package-binding.json`, `gm-auto-chrome-archive-crc.log` and
`gm-auto-packed-archive-crc.log`. Local reproduction is retained in
`gm-package-reproduce.py`, `gm-package-audit.py`,
`gm-package-installer-audit.py` and `gm-auto-bind-package.py`.

The archive action used the verified physical C: output path; the H: build
directory is a junction rejected by the unchanged staging safety check.
The local reproduction script explicitly changes to that physical directory
before invoking preserved resource/link commands, as required for Windows
relative executable resolution. The installer was generated and inspected, not
executed on the existing installation.

## Remaining deployment boundaries

Software simulation performs real SM2 and tests process/authorization behavior;
it does not qualify a physical Feitian model. Hardware acceptance still needs
the signed x64 SKF SDK, actual certificates/firmware, PIN behavior, removal and
reinsertion, cancellation, driver hangs, concurrent signing and target-server
PKI. Production rejects the unsigned simulator rather than using it as a Key
fallback. SM2DHE, Key-side decryption/key agreement and TLCP exporter support
remain outside this implementation.

The public `test1.gmssl.cn` demo rule uses explicitly configured demo trust. Its
older successful navigation does not prove an arbitrary public GM site will be
trusted automatically. Automatic mode uses the effective local trust snapshot;
no public/test CA was installed into the operating-system trust store.
