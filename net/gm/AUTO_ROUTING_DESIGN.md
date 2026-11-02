# Automatic national TLS with an ordinary TLS fast path

The current implementation uses Chromium's pinned BoringSSL for ordinary TLS
and its local SM2/SM3/SM4, RFC 8998 and TLCP client extensions. It implements
bounded automatic discovery, joint TLS 1.3 negotiation and handshake-driven
USBKey identity selection on Windows x64 GM builds. Tongsuo remains an independent
test peer and source reference, not a production TLS library or DLL.

This document describes the implementation and its acceptance boundaries.
Executed automatic-routing, trust-snapshot, identity-selection and browser
results are recorded in `BORINGSSL_TEST_RESULTS.md`, including strict discovery
and independent interoperability limits. Physical Feitian acceptance is separate.
`TEST_RESULTS.md` preserves historical results from the removed dual-library
implementation and must not be used as evidence for this migration.

## Protocol discovery and certificate selection are separate

A server chooses its certificate during the selected protocol's handshake. A
client chooses a USBKey identity only after the server requests client
authentication. Neither operation requires a per-site certificate-selection
rule for every ordinary connection.

Ordinary TLS 1.3 and RFC 8998 share a state machine and can advertise their
algorithms in one ClientHello. An explicit `tls13_auto` rule or a previously
authenticated RFC 8998 route enables this joint mode in `SSLClientSocketImpl`.
Ordinary unknown sites keep GM disabled, preserving their existing key shares.
Only an actual C6/C7 national suite selects the GM verifier and SM2 identity
bridge; an ordinary result retains Chromium's CertVerifier, CT and ordinary
client-certificate path. GM trust does not become ordinary TLS trust.

TLCP still uses a different handshake/record protocol. A client cannot wait for
a certificate before choosing which ClientHello/state-machine branch to use.
A single library does not change a deployed pure-TLCP server's first-packet
requirements. Unknown destinations therefore retain normal TLS first, narrowly
classified protocol retries and a bounded cache of successful discoveries.
Identity selection suspends and resumes the same SSL/TCP connection after
`CertificateRequest` and the server-identity gate; it does not reconnect for PIN.

## Connection decision

1. An explicit managed origin rule takes priority and selects its configured
   protocol and trust. Invalid managed configuration still rejects connections;
   automatic discovery cannot override it. Strict `tlcp`/`rfc8998` rules require
   those protocols. `tls13_auto` permits either TLS 1.3 family in one handshake,
   but a required GM identity cannot silently be waived by an ordinary result.
2. With no managed rule, an unexpired capability hint selects the previously
   verified GM route for that partition. Authenticated TLCP is retained as TLCP;
   authenticated RFC 8998 promotes subsequent connections to `tls13_auto`,
   retaining the same system-trust snapshot and current DNS ECH settings.
3. Otherwise use Chromium's ordinary BoringSSL connection path. Successful
   ordinary HTTPS adds no GM handshake, SM2 key share or GM USBKey enumeration.
4. If fresh DNS provides ECH that is enabled for the destination, an automatic
   strict GM candidate stops before creating its socket or sending its
   ClientHello. A dedicated internal signal selects ordinary TLS again above
   both pools, using the ordinary profile and pool key. It clears the learned
   hint and does not turn a generic cancellation into a protocol retry. If
   ordinary TLS was already attempted, the request fails rather than trying
   another GM protocol without ECH. A joint `tls13_auto` candidate instead
   preserves Chromium's ECH/ALPS/TAI configuration in its existing TLS 1.3 path;
   it does not take the strict-GM reselection branch.
5. Once a TLS handshake starts, only an explicitly classified
   protocol/version/cipher incompatibility,
   before certificate or authentication activity, allows another TCP attempt.
   Discovery selects each protocol at most once for that request. After the
   initial ordinary TLS attempt, it tries TLCP and then RFC 8998. If a learned
   GM route fails with the same precise protocol evidence, it can first retry
   ordinary TLS if that protocol has not yet been attempted. Retries re-enter
   above both connection-pool implementations before pool lookup. An explicit
   ordinary TLS retry skips capability-hint lookup, so a concurrent request's
   newly learned hint cannot change its selected protocol. A failed joint
   attempt already counts as offering RFC 8998, so it cannot repeat a strict
   RFC 8998 attempt. It can still try ordinary TLS once for a peer which now
   supports only TLS 1.2, subject to the same retry guards.
6. Only a verified actual GM result records or refreshes a capability hint.
   A successful ordinary result in joint mode neither creates nor extends GM
   capability evidence. Failure does not establish trust.

The classifier does not treat all `ERR_SSL_PROTOCOL_ERROR` or
`ERR_SSL_VERSION_OR_CIPHER_MISMATCH` results alike. It requires the socket's
specific retry permission. BoringSSL's allowlist includes unsupported protocol,
explicit `protocol_version`, and no shared cipher, with
certificate/client-auth/ECH guards. Its generic `WRONG_VERSION_NUMBER` error
allows discovery only when the first seven received bytes are exactly the
complete TLCP fatal `protocol_version` alert `15 01 01 00 02 02 46` (hex).
The socket captures this bounded prefix only with automatic discovery enabled,
for destination TLS and without ECH. It does not issue an additional read or
wait to complete the alert. If BoringSSL rejects the five-byte record header
before the alert body arrives, the evidence is insufficient and discovery is
not retried; such a server may need an explicit site rule. The GM backend
similarly allows explicit protocol/cipher incompatibility before authentication
begins.

Generic `handshake_failure`, EOF, resets, timeouts, malformed records,
certificate errors, PIN failures and device errors are not evidence that a
different national TLS protocol is needed. In particular, a server returning
only generic `handshake_failure` may still require an explicit site rule. This
is a compatibility boundary, not proof that the server lacks national TLS.
Application bytes have not been sent when a protocol-discovery retry occurs.

Proxy TLS remains independent of destination TLS. Proxy connection errors must
not enable discovery for the destination. Managed/learned GM profiles disable
QUIC/Alt-Svc bypass and cross-origin HTTP/2 coalescing; ordinary profiles keep
their existing behavior.

An automatic capability hint does not authorize disabling ECH. Automatic routes
retain ordinary SVCB/HTTPS endpoint restrictions, including disabling authority
A/AAAA fallback when all alternative endpoints offer ECH. DNS selection must
allow an HTTP/2-only ECH endpoint to reach the ordinary-TLS reselection check
even when the remembered protocol is HTTP/1.1-only TLCP. Joint TLS 1.3 keeps ECH;
strict explicit GM rules retain their protocol-specific behavior. The per-host
ECH-disabled setting is respected. Reselection can incur another TCP connection, but the
abandoned GM candidate has performed no TLS handshake or USBKey operation.

## System trust snapshot

`SSLConfigServiceManager` enables automatic discovery by default only for a
Windows x64 GM build. A blocking worker reads a snapshot after the dedicated
Windows store monitor is ready and after trust notifications. The monitor is
continuously rearmed; loss of monitoring disables automatic GM trust. Its
operation does not depend on the ordinary verifier first opening the stores.
Certificate stores are not enumerated for each connection.

The snapshot reads local registry-backed stores in the current-user,
local-machine, group-policy and enterprise locations:

| Store/data | Use in the GM backend |
| --- | --- |
| Effective `ROOT` certificates | Explicit trust anchors |
| Effective `CA` certificates | Untrusted issuer candidates, never anchors |
| `Disallowed` certificates | Reject a match anywhere in either verified role chain |
| Locally stored CRLs | Explicit CRL input; no online acquisition |

ROOT/CA candidates must be currently valid and permit server authentication.
`CertGetEnhancedKeyUsage(..., 0, ...)` applies the intersection of the Windows
EKU property and the signed EKU extension. A missing property and extension
permit all purposes; an explicitly empty property permits none. Usable copies
of the same DER are combined; explicit `Disallowed` always takes precedence.
A certificate restricted from server authentication is retained as denied
when no usable local copy exists, so a server-supplied issuer cannot bypass its
Windows usage property. Disallowed entries remain effective regardless of
their own validity period or EKU.

The collector does not read `AuthRoot`, download roots, add an SM2 CA to Windows,
or search OpenSSL configuration/environment paths. Ordinary Chrome certificate
verification remains separate. This snapshot does not add TrustedPeople leaf
trust or implement a Windows chain-engine call inside the sandboxed backend.

Empty roots are a valid automatic configuration: protocol negotiation can
proceed, but server verification must return an authority error. Empty roots
do not mean accept-all or fall back to process/system defaults. Managed rules
continue to require explicit nonempty trust anchors.

Limits apply during enumeration and configuration: up to 4096 certificates in
each list, 128 CRLs, 256 KiB per DER object and 64 MiB in an automatic snapshot.
An incomplete/oversized snapshot publishes empty trust. Automatic mode does
not require a complete CRL set or perform online OCSP/CRL retrieval; deployments
requiring hard-fail leaf/chain CRL checking use the existing managed revocation
settings. Explicit Disallowed checks always apply.

On a trust notification the browser first publishes an empty snapshot with a
new source epoch, then collects the replacement asynchronously. It discards
results started before a newer notification. The NetworkContext assigns its
own monotonically increasing effective epoch, including disable/remove/re-add
transitions. GM contexts, sessions, capability hints and pooled GM connections
cannot survive under the old trust generation. Automatic-config-only changes
preserve ordinary BoringSSL sessions and pools.

## USBKey selection on the existing connection

The strict socket's in-process facade retains ABI 3 `GM_WANT_IDENTITY`, bounded
identity-request metadata and completion, plus untrusted-issuer and Disallowed
inputs. The function table does not load a separate library; all cryptography
resolves to the linked BoringSSL. Joint TLS 1.3 uses `gm_tls13_client` and
`GmSSLPrivateKey` from the ordinary socket. Neither path sends PINs or private-key
bytes to Network Service.

1. Connect without prompting or acquiring a new identity. A still-authorized
   cached identity may be used to offer an eligible authenticated session.
2. On `CertificateRequest`, validate the server's role chains, purpose and
   revocation requirements, then apply Chromium's hostname/public-suffix and
   pin checks before authorizing a prompt. This is a server-certificate gate;
   the handshake's Finished confirmation may still be pending.
3. Copy the bounded request: CA distinguished names, signature algorithms,
   certificate types, protocol and required roles. Suspend the same SSL/TCP
   while the browser processes this request asynchronously.
4. Filter candidates for SM2 curve, validity, clientAuth, key usage and the
   requested issuers/algorithms. TLCP requires separate signing and encryption
   certificates for the same subject/issuer with distinct keys. Issuer-name
   matching follows reachable CA candidates and cannot be satisfied by an
   unrelated certificate included in the device's bundle.
5. The browser independently enforces origin/driver/identity policy and obtains
   user authorization. It returns opaque key capabilities and public
   certificates. Install them and resume the existing handshake.
6. Private-key signing can suspend again through BoringSSL's
   `SSL_PRIVATE_KEY_METHOD` retry callback. The SKF service performs the
   operation; its result resumes the existing handshake. There is no OpenSSL
   ASYNC job or provider in the production path.

No Key, no eligible identity, or an allowed optional decline continues with an
empty client certificate. The server decides whether optional authentication
is sufficient. A managed requirement for client authentication remains strict;
errors such as incorrect PIN, failed hardware signing and service failure do
not silently become an anonymous connection or trigger protocol fallback.

Candidate filtering selects an identity; it is not a replacement for full
client-chain verification or proof of private-key possession. The TLS backend
and server perform those handshake operations. Session resumption may omit
CertificateRequest, so current identity authorization and trust/identity epochs
must be checked before offering or reusing an authenticated session.

In joint mode, cold root/CRL parsing and chain verification run on a worker;
request cancellation or destruction discards late verification replies and
cannot start a PIN prompt. Each task retains its own immutable trust snapshot.
Already-running computation is not forcibly interrupted. The strict facade
deduplicates verification within the same handshake but still verifies once
synchronously. These changes do not imply that every GM operation has moved
off the network I/O sequence.

## Reuse and ordinary-path cost

Capability hints are memory-only, limited to 256 entries and 64 MiB of retained
trust/issuer/Disallowed/CRL DER per NetworkContext. They expire after 10 minutes
and successful verified GM use renews that expiry. They are partitioned by
normalized secure origin, NetworkAnonymizationKey, privacy mode and proxy chain.
`wss` uses the corresponding HTTPS capability. A hint stores the verified TLCP
route or the joint TLS 1.3 entry learned from RFC 8998, with its trust epoch;
it never overrides a certificate or authorizes a Key. Expiry,
eviction or a failed learned route also invalidates the associated GM context,
sessions and reusable connections through a dedicated GM-only notification
scoped to the destination. It preserves ordinary HTTPS connections to the same
host and ordinary connections through a same-named HTTPS proxy. All reuse paths
retain ordinary origin and partition checks.

Joint session keys retain the full GM connection profile, identity ID and
identity generation. GM-only invalidation has a separate insertion generation,
so it does not flush ordinary TLS/HTTPS-proxy tickets or reject their late
inserts. If a server rejects an offered identity-bound GM ticket and negotiates
ordinary TLS, the cached GM identity is cleared before ordinary verification.

When either budget is exceeded, the earliest-expiring hints are evicted. A
single rule exceeding the byte budget is not cached. Large Windows stores thus
reduce the number of sites whose routes can be retained; they do not relax
trust. Ordinary HTTPS checks an empty hint table in O(1), or performs an
O(log N) lookup when hints exist. It does not scan all hints or copy the full
trust snapshot for every ordinary request.

Managed or learned GM-profile traffic, including an ordinary result of joint
negotiation, bypasses the HTTP response cache so stale responses cannot
stand in for current trust or Key authorization. Discovery restores caller
headers/URL rather than carrying ordinary-cache-generated validators into the
new GM request. Responses without a GM profile keep their existing cache behavior. A first
unrecognized origin can therefore still use a valid ordinary cache entry until
normal network validation is required; automatic discovery does not globally
disable caching or probe every URL.

Normal successful HTTPS without a GM profile has no additional transport
handshake, SM2 key generation or GM SDK work. There is no production Tongsuo DLL
preload or provider initialization. Joint mode intentionally adds national
algorithms and an SM2 key share only for the selected routes, requires TLS 1.3
and disables early data. These have a cost even when the server chooses an
ordinary suite; joint mode is not enabled for every website.
No claim of zero ordinary-page overhead follows from code-path isolation or
one backend microbenchmark. Measure ordinary-path overhead, first discovery,
learned routes, resumption and actual Feitian signing separately; current
measurements and their cross-run variation are in `BORINGSSL_TEST_RESULTS.md`.

## Validation boundaries

Focused coverage is in `auto_routing_unittest.cc`,
`client_cert_constraints_unittest.cc`, `tls_policy_unittest.cc`,
`ssl_config_service_mojo_unittest.cc`, `gm_system_trust_win_unittest.cc` and the
socket/browser suites. The Windows trust tests use memory stores. Browser
tests can inject a `CHECK_IS_TEST`-protected snapshot through the production
CertDatabase/async/Mojo path without installing a root on the machine.

The automatic TLCP navigation success fixtures use the test peer's explicit
`protocol_version` rejection of a standard TLS ClientHello. Separate browser
negative cases use the default Tongsuo TLCP peer, whose response is the generic
`15 01 01 00 02 02 28` fatal `handshake_failure` alert. They check the original
protocol error, zero Key operations and peer-side counts proving that no TLCP
ClientHello was sent. Such a server needs a managed site rule under the strict
discovery policy; success against the explicit-alert fixture is not a claim of
universal automatic TLCP compatibility.

Required acceptance includes ordinary TLS/pool/cache regressions, both pool
implementations, allowed and rejected retry classifications, trust/identity
changes during connection, hint partitioning/expiry, optional and required
mTLS, and cancellation/hotplug/service failures. Executed results and exact
binary/artifact records are in `BORINGSSL_TEST_RESULTS.md`; rerun affected checks
for future changes. Physical Feitian hardware/SDK interoperability and latency
remain separate acceptance items.
