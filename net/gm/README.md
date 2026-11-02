# Chromium 153 national TLS

The current implementation uses Chromium's pinned BoringSSL for ordinary TLS
and the local SM2/SM3/SM4, RFC 8998 and TLCP client extensions. The migration is
in progress; primitive or library tests do not establish browser release readiness.
See [SINGLE_LIBRARY_DESIGN.md](SINGLE_LIBRARY_DESIGN.md) and
[BUILDING.md](BUILDING.md) for the current design and build inputs, and
[BORINGSSL_TEST_RESULTS.md](BORINGSSL_TEST_RESULTS.md) for current results and
qualification gaps.

There is no production Tongsuo DLL, provider, ASYNC job or separate Configure/make
build. `gm_backend.cc` is an internal adapter linked into `//net`; the retained
function table preserves the existing socket, authorization and session contracts.
All crypto/protocol calls resolve to Chromium's BoringSSL. The adapter's indirect
calls participate in the normal compiler CFI checks.

The current Feitian SKF service and native PIN integration target Windows x64.
The service remains isolated in its utility process. Private keys
stay in the device. The adapter uses `SSL_PRIVATE_KEY_METHOD` to suspend a
handshake while the browser selects an identity and obtains PIN authorization.
The network process receives a signature, not the PIN or private key. Cancellation,
page teardown, policy/device generations and cross-origin reuse restrictions
continue to apply. No physical Feitian Key has been tested in this environment.

Both TLCP server leaves are checked independently through libpki, including
explicit roots, untrusted issuer candidates, disallowed certificates, validity,
SAN/Chromium name matching, role-specific key usage and configured CRLs. Root
certificates are not inferred from arbitrary peer-supplied certificates.

Ordinary BoringSSL TLS is GM-disabled by default. A `tls13_auto` site rule, or
an authenticated learned RFC 8998 route, enables joint negotiation in Chromium's
existing `SSLClientSocketImpl`. One ClientHello retains the ordinary suites and
key shares and adds SM4/SM2. A standard result uses the ordinary CertVerifier,
CT and client-certificate path; an RFC 8998 result uses the configured GM trust
and asynchronous SM2 Key bridge. GM trust anchors never become ordinary public
roots merely because both protocols share a socket. Strict `rfc8998` and `tlcp`
rules retain their meanings. A standard result cannot create or refresh GM
capability evidence. Unknown ordinary sites incur no extra SM2 key generation
or unconditional GM attempt.

Joint sessions retain the full trust/policy profile and authorized identity
generation. GM invalidation uses a separate cache generation, so Key removal
does not flush ordinary TLS or HTTPS proxy tickets. Immutable GM trust stores
are parsed once per cached rule; certificate expiry and CRLs are still checked
on each handshake, including resumption.

TLCP requires an explicit wire-protocol entry and currently
supports static ECC with SM4-GCM/CBC; it does not claim SM2DHE or Key-side decryption.
QUIC remains on its standard algorithms. Browser routing and capability caches
must retain their protocol/trust/identity boundaries even though the library is shared.
For an unknown destination, the existing bounded discovery may still need a new
connection after protocol incompatibility. Joint TLS does not make a pure TLCP
server accept a standard TLS ClientHello.

Tongsuo 8.5.0 is a pinned source and independent test reference. The optional
external peer and `gm_boringssl_test_backend` shim are test-only and excluded from
browser packages. Legacy DLLs left in an output directory are not current outputs.
The pre-migration results in [TEST_RESULTS.md](TEST_RESULTS.md) validate the old
implementation only; current runs are recorded in
[BORINGSSL_TEST_RESULTS.md](BORINGSSL_TEST_RESULTS.md).

Windows x64 is the local build/test platform. No 32-bit support is included.
macOS/Linux source paths require native builds and qualification. A software SKF
token exercises real IPC and PIN state transitions but cannot validate vendor
SDK loading, physical hotplug behavior or hardware latency.
The macOS/Linux vendor-Key and PIN service is not wired up; portable crypto/TLS
sources do not establish USBKey support on those systems.
