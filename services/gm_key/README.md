# Windows x64 SKF USBKey service

`GmKeyService` is registered in the Chromium utility service factory on Windows
x64. Each instance must own a dedicated utility process. The browser owns its
control pipe and configures a provisioned driver; Network Service receives only
an authorized `KeySigner` capability. No private key, SDK handle, driver path,
or PIN is exposed by that signer interface.

## Production boundary

- The minimal GM/T 0016 ABI accepts only AMD64 PE DLLs. No vendor SDK, firmware,
  private keys, or driver binaries are included. There is no 32-bit fallback.
- Driver loading requires an explicit local absolute path, Authenticode and a
  provisioned SHA-256. Hash verification uses the same locked file handle as
  signature/PE checks. DLL dependency lookup is limited to its directory and
  System32; relative/UNC paths, alternate streams and network redirects fail.
- Driver configuration is attempted once per process. Browser provisioning must
  enforce the intended Feitian signer/hash and administrator-controlled directory
  ACLs, including dependencies. Authenticode alone is not a vendor allowlist.
  Online revocation and driver updates belong to provisioning.
- Third-party SDK access uses `kNoSandbox`: this is fault isolation, not a
  security sandbox for malicious native code. It must not share a process with
  another service. Control-pipe disconnect ends that process. Shutdown does not
  call possibly blocking SKF close/unload functions.
- All SDK calls use one dedicated OS thread. Mojo remains responsive. At most 64
  operations and 64 combined pending/committed signer capabilities are
  outstanding; each queued operation
  has a 30-second process watchdog. A hung SDK ends only this dedicated process,
  including hangs during load, enumeration, PIN verification or signing.
- Device/application/container enumeration is bounded. Identity IDs are opaque,
  per-instance random values; generations change on invalidation. Inventory is
  cached to avoid opening/closing application handles on every TLS connection.
  Windows USB device-interface notifications advance a generation independently
  of the SDK thread; removal/arrival revokes grants even when successive SKF
  name lists are identical. Since SKF exposes no standard mapping from its
  names to PnP paths, any physical USB arrival/removal conservatively revokes
  this configured Key service's grants. Failure to register notifications fails
  driver configuration. The service is only started for GM Key access.
  Device names are also polled each second. Application/container lists and
  certificate DER are refreshed every five seconds and before returning a
  cached inventory. Refresh uses existing session handles to preserve PIN state.
  A change revokes grants and yields new identities; software certificates are
  never used as a fallback. Actual PnP delivery and vendor behavior still need
  physical hardware acceptance.
- Signing and encryption certificates are exported separately. Signing-only
  identities can serve RFC 8998; TLCP callers must require their encryption
  certificate. SM2 private keys are never exported.
- The browser sends a move-only `SecurePin`; its owned buffers and decoded
  service/SDK buffers are wiped before destruction. Mojo's internal serialized
  transport buffers are not covered by that guarantee. PINs are never persisted
  or automatically retried. Wrong PIN and
  locked PIN return the driver's retry count. Browser UI owns its own wiping.
- `Authorize` takes a nonzero monotonically increasing control-pipe request ID
  and only prepares a result. The browser calls `CommitAuthorization` while
  that request is still current before handing out the signer; until then it
  cannot sign or be reused. `CancelAuthorization` cancels queued, running and
  prepared results, without revoking other committed authorizations. An early
  cancellation consumes its request ID. Prepared results expire after 30 seconds
  without commit, releasing their slot without ending the service. A PIN
  failure still revokes older authorization because the SDK may share login
  state, while a newer queued PIN attempt remains eligible to succeed. Lock,
  device changes and service failure cancel every pending authorization.
  Signature request IDs must strictly increase per signer, so cancelled IDs
  cannot accept an old signature.
- Browser-only `GrantAuthorized` issues independent signer capabilities after
  one successful authorization, allowing multiple NetworkContexts to share a
  PIN prompt without storing or replaying the PIN. It caches public identity
  metadata only and returns `kPinRequired` after any revocation. The browser
  owns user/session policy; `Lock` is the service operation for ending that
  authorization explicitly.
- `Sign` takes exactly the protocol's precomputed `SM3(ZA || message)` digest,
  validates SKF scalar padding/ranges, and emits canonical DER. The adapter never
  hashes the TLS transcript again. No PIN, device name or signature is logged.

SKF authentication may be shared by multiple application handles. Lock resets
all cached sessions in that application and revokes all capabilities. Actual
Feitian session semantics remain a hardware acceptance requirement. SM2 key
agreement, private-key decryption and vendor extensions are not exposed here.

## Simulation without hardware

Two separate test DLLs serve different purposes:

1. `gm_fake_skf.dll` is a synthetic fault injector for low-level ABI/loader tests.
   It deliberately does not implement cryptography.
2. `gm_sm2_simulator.dll` uses Chromium's current BoringSSL SM2 implementation
   for real SM2 signatures.
   It generates an ephemeral CA and distinct signing/encryption certificates
   inside `gm_key_service_test_helper.exe`. Private keys stay in that process.
   `gm_sm2_verify.exe` verifies signatures in another process with that same
   BoringSSL implementation. Standard vectors and an external TLS peer provide
   independent interoperability checks; these two processes alone do not.

The simulator's PIN is `123456`, with three wrong attempts before lockout.
Test controls model removal/reinsertion, PIN/signing delay, locked PIN and driver
crash. They exist only in the test helper. Production neither registers test
controls nor accepts unsigned simulator drivers. No automatic software fallback
is present, and a test result is not evidence of hardware performance.

`gm_key_service_tests.exe` launches a fresh helper for each case and exercises
real Mojo IPC, hash pinning, SM2 signature verification/tampering, PIN retry and
lockout, cancellation, identity generations, removal/reinsertion, authorization
races, capability limits, grants without PIN replay, watchdog isolation and
driver crash isolation.

## Build and run

With `enable_gm_tls_backend = true` (see `//net/gm/BUILDING.md`):

```bat
third_party\ninja\ninja.exe -C out\gm_x64 gm_key_tests services/gm_key:service
out\gm_x64\gm_key_service_tests.exe
out\gm_x64\gm_skf_test.exe H:\chromium_142\src\out\gm_x64\gm_fake_skf.dll
```

The simulator DLL and verifier link Chromium's BoringSSL through GN. They do not
load OpenSSL providers or a separate cryptographic library. The production SKF
service continues to call the vendor's SKF ABI for private-key operations.

For the standalone low-level checks, from an x64 Native Tools prompt:

```bat
services\gm_key\build_tests.cmd out\gm-key-tests
```

Historical validation of the previous backend on 2026-10-27: the subprocess
suite passed **13/13** (6.412 seconds),
and the low-level checks passed **100/100** with both Chromium Clang and MSVC
x64. The production service source set, test helper, SecurePin typemap,
simulator and independent verifier all compiled successfully. The subprocess
log is `out/gm_x64/gm-key-service-test-results.log`.

The low-level checks cover malformed MULTI_SZ/lengths, thread affinity, partial-open
cleanup, PIN failures, certificate roles, digest lengths, scalar padding/ranges,
DER encoding, removal, lifetime ordering, unsigned/incorrect-machine DLLs and
missing exports. These are service/adapter checks; full TLS integration is
validated separately by the network tests.

The read-only diagnostic probe takes an explicit approved signed driver path,
counts devices/applications/containers, and never receives a PIN or signs:

```bat
out\gm_x64\gm_skf_probe.exe "C:\Program Files\Vendor\approved-skf.dll"
```

## Remaining hardware acceptance

No physical Feitian Key, model, firmware or vendor DLL is available in this
workspace. Acceptance must verify the signed x64 SDK, device naming/encoding,
certificate roles, PIN/session lifecycle, signature digest semantics, external
verification, rapid hotplug, actual connection throughput/latency and recovery.
Browser PIN UI and end-to-end TLS checks live in their respective components.
