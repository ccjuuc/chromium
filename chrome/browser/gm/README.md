# Automatic and managed national TLS with USBKey authentication

This implementation targets Windows x64. Ordinary HTTPS, RFC 8998 and static-ECC
TLCP use Chromium's pinned BoringSSL with local national-crypto extensions.
GM builds enable bounded automatic discovery for static-ECC TLCP 1.1
(HTTP/1.1) or RFC 8998 TLS 1.3 (HTTP/1.1 and HTTP/2), using a browser-collected
Windows ROOT/CA/Disallowed snapshot. Exact managed origins take priority.
A strict `tlcp` or `rfc8998` rule never falls back to ordinary TLS. A `tls13_auto`
rule or an authenticated learned RFC 8998 route offers ordinary and national
TLS 1.3 algorithms in one ClientHello. Unknown ordinary sites retain the normal
GM-disabled ClientHello. SM2DHE and a webpage business-signing API are not
implemented. GM system trust is collected separately from ordinary Chrome root
verification; no root is downloaded or installed by this feature. See
`//net/gm/AUTO_ROUTING_DESIGN.md` for the retry boundaries and current validation
status. A generic server `handshake_failure` does not enable automatic retry;
such sites may still need an explicit rule.

## Deployment

Build with `target_cpu="x64"` and `enable_gm_tls=true` as documented in
`//net/gm/BUILDING.md`. The production GN targets link the national-crypto
implementation into the existing BoringSSL/`net` targets. There is no production
Tongsuo archive build, provider, backend DLL preload or `gm_backend/` package
payload. The optional independent Tongsuo peer and BoringSSL facade DLL are
test-only; simulator and test certificate files are also excluded from packaging.
Old files left in a build directory are not current package inputs. Network
Service remains sandboxed and does not load the vendor USB driver. Current
build and package-audit results are recorded in `//net/gm/BORINGSSL_TEST_RESULTS.md`.
The current installer passed 50 archive, direct-import and payload-identity
checks. The two preexisting bundled applications retain their own OpenSSL
libraries, matched to canonical application bytes by an exact path/hash manifest.
The entire installer is therefore not described as OpenSSL-free. The installer
has not been executed for installation/upgrade qualification.

Provision the vendor's signed **x64 SKF** SDK separately. The browser does not
download drivers and never guesses a DLL name. Feitian device/SDK/firmware
compatibility requires a real-device acceptance run. The production service
requires both a valid Authenticode signature and the administrator's exact
SHA-256. It runs in a dedicated utility process without a security sandbox,
because the trusted vendor driver may need native device, file and registry
access. Process separation protects browser availability from a hung/crashed
driver; it does not make malicious native code safe.

For explicit origin requirements or USBKey access, configure these machine-level
enterprise policies through the browser's normal Windows policy provider. They
are visible in `chrome://policy`:

- `GmTlsSiteRules`: a JSON array of exact-origin rules (maximum 256).
- `GmUsbKeyDriver`: an object with `path` (absolute local DLL path) and `sha256`
  (64 hexadecimal characters).

Site rules are not required for successful automatic server-authentication
discovery. A configured trusted driver is still required for USBKey use. The
browser requests an identity only after the server asks for client
authentication and its certificates pass the authorization gate. An optional
request with no usable Key can continue with an empty certificate; a server or
managed rule requiring mTLS still rejects it. The GM Key service is not
enumerated on the ordinary TLS path or for GM servers that do not request
client authentication; existing ordinary TLS client-certificate behavior is
unchanged.

User preferences and website JavaScript cannot enable these policies. Policy
refresh revokes existing Key grants and prompts. Per-profile managers, including
separate incognito managers, never share PINs or service control pipes.

Example rule (replace certificate data with base64-encoded DER):

```json
{
  "host": "portal.example.internal",
  "port": 443,
  "protocol": "tlcp",
  "trust_profile_id": "corporate-gm-ca",
  "trust_generation": 1,
  "identity_policy_id": "employee-feitian-key",
  "identity_generation": 1,
  "policy_revision": 1,
  "trust_anchors": ["BASE64_DER_CA_CERTIFICATE"],
  "crls": ["BASE64_DER_CURRENT_CRL"],
  "revocation": "chain",
  "enable_session_resumption": true
}
```

Use lowercase canonical DNS names or IP literals, without URL components,
wildcards or a trailing dot. Set `protocol` to `rfc8998` to require national
TLS 1.3 algorithms, or `tls13_auto` to offer ordinary TLS 1.3 and RFC 8998 in
the same ClientHello. Joint negotiation retains a separate connection profile:
it permits HTTP/2 but forbids QUIC and cross-origin connection reuse. Its GM
result uses the configured trust anchors; an ordinary TLS result uses Chromium's
normal certificate verifier. A required Key identity cannot be silently waived.
Omit both identity fields when no specific managed identity is required; a
server's optional client-auth request can still be handled after its handshake
request. The optional
`certificate_sha256` is the exact SHA-256 fingerprint of the client signing
certificate and restricts which identities appear in the trusted native prompt.
Otherwise the user chooses a certificate. The prompt shows the requesting
origin and a fingerprint prefix, and accepts a printable ASCII PIN of up to
64 bytes. It is modal to the initiating page and uses localized strings.
Invalid local input keeps the dialog open and is not sent to the Key. Wrong-PIN
retries always require another user action. A locked PIN produces a notice with
only a Close button, bounded by the prompt timeout; it does not retry the SDK.

`revocation` must explicitly be `disabled`, `leaf` or `chain`. The latter two
hard-fail for missing, stale, invalid or revoked CRLs. There is no implicit
network OCSP/CRL fetching. Maintain CRLs through managed configuration and use
positive, increasing trust/identity generations and policy revisions. The
NetworkContext additionally assigns a monotonic internal epoch, so reusing an
administrator revision cannot resurrect old connections. Invalid rule lists
produce a rejecting configuration; they do not silently enable ordinary TLS.

The BoringSSL facade verifies both TLCP server certificate roles through libpki,
including certificate purposes, expiry, explicit trust and revocation. The joint
TLS 1.3 socket selects its GM verifier only after negotiating an RFC 8998 suite;
an ordinary suite retains Chromium's ordinary CertVerifier/CT/OCSP path.
GM verification includes Chromium SAN/public-suffix name matching and applicable
pin checks. Its normal certificate-error override switch does
not bypass this managed verification. Standard security metadata displays
TLCP/SM2/SM4 accurately; static TLCP key exchange and CBC are still reported as
obsolete according to Chromium's security criteria.

## Request-owned certificate and PIN flow

The production path is `URLLoader` ->
`URLLoaderNetworkServiceObserver::OnGmCertificateRequested` ->
`StoragePartitionImpl` -> `ContentBrowserClient::SelectGmClientCertificate` ->
the current profile's `KeyManager`. The profile-wide identity broker cannot
start a fresh production authorization by itself.

The request token is created lazily, when a GM CertificateRequest needs a new
authorization and no matching live grant exists. Ordinary HTTPS and GM requests
that never request a client certificate do not register a prompt context. The
token identifies the UI owner; it is not added to connection, session or grant
partition keys. Pending requests coalesce only within the same live request
context, so one page cannot keep another page's PIN prompt alive.

StoragePartition resolves the requesting frame or pre-commit navigation; a
service worker must resolve its originating `window_id`. Missing pages, unknown
worker windows, prefetch and prerender requests cannot show PIN UI. Closing the
request's responder pipe, destroying the page or starting a new cross-document
main-frame navigation cancels the interaction. Chrome also rejects no-state
prefetch and selects the manager belonging to the actual BrowserContext,
including its separate incognito instance.

Preconnects have no live page request context. They may reuse an existing valid
grant, but cannot enumerate a Key or start a fresh PIN prompt. Without a grant,
optional client authentication proceeds without a certificate; required
authentication fails. Requests that disable client certificates
(`PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS`, including applicable credentialless
requests) cannot use even a cached GM identity. A managed rule requiring an
identity cannot override that restriction.

The browser completes the device's signing/encryption certificate chains from
the local Windows CA and ROOT stores before applying the server's CA list and
algorithm/role constraints. This work runs on a blocking worker, deduplicates
issuer candidates, omits only locally verified self-signed roots and has explicit
scan/chain limits. Same-name CA rollovers are retained. Signatures supported by
the current libpki, including SM2/SM3, can establish that a root is self-signed;
unsupported signature algorithms conservatively retain the candidate.
It does not call the Windows chain engine or fetch AIA. Discovered certificates
remain public, untrusted issuer candidates; the server validates the submitted
client chain. This allows a root-only CertificateRequest to match a Key whose
driver exposes only the leaf certificates.

## Lifetime and performance

An origin's repeated requests share an authorized signer and reuse eligible
connections. A browser authorization can issue additional signer capabilities
without retaining or replaying the PIN. Signatures execute on one fixed SDK
thread in the isolated service. Calls have bounded queues, a watchdog, monotonic
IDs and generation checks; cancellation rejects late replies.

The joint TLS 1.3 path parses cold GM roots/CRLs and verifies the server chain
on a background worker, using a bounded cache of immutable trust configurations.
Cancelling the request discards late results and cannot resume a handshake or
show a PIN prompt. Already-running worker calculations are not forcibly stopped.
The strict `tlcp`/`rfc8998` facade removes duplicate verification within the same
handshake but still performs one synchronous verification. This is not a claim
that every GM operation has left the network I/O sequence.

Fresh PIN authorization has prepare/commit semantics. `Authorize(request_id)`
can return a successful SDK result without binding a signer or adding reusable
authorization. The browser rechecks the live request, policy, generation and
selected certificate, then sends `CommitAuthorization(request_id)` on the same
ordered service pipe. Cancellation revokes queued, running or prepared requests;
even a successful SDK reply arriving after cancellation cannot create a usable
grant. A synchronous vendor SDK call is not assumed to be interruptible.

Automatic server trust has its own continuously rearmed Windows store monitor.
Monitoring must be ready before the first snapshot is loaded. Every change
invalidates GM trust/session state before asynchronous replacement, and loss of
monitoring disables automatic GM trust. Repeated changes are observed even when
the ordinary Chromium certificate verifier never accesses its stores. Snapshot
and notification work is independent of per-connection handshakes.

Private keys stay in the device. `SecurePin` wipes its owned browser/service/SDK
buffers on normal destruction, cancellation and late callbacks. No PIN enters
Network Service, preferences, logs or session tickets. This is not a guarantee
that every internal native text-widget or Mojo transport allocation is wiped;
password fields and edit history are cleared after submission/closure.

Device removal, locking, signer failure or service death revokes capabilities
and active connections. TLS session tickets remain memory-only, partitioned by
NetworkContext, network anonymization key, origin, proxy, privacy mode, policy,
trust, identity and offered ALPN. Expired/unusable tickets are discarded and a
fresh handshake is attempted with the same required protocol and verification.
Joint sessions also retain their full GM profile and identity generation. GM
invalidation uses its own cache generation so that Key removal or a GM trust
refresh does not discard ordinary TLS/HTTPS-proxy tickets or their late inserts.

GM-profile requests, including ordinary-suite results of joint negotiation,
bypass HTTP memory/disk response caches because a URL cache entry does not prove
current trust or live Key authorization. Origins without a GM profile retain
their existing caching behavior. Connection reuse, TLS resumption and RFC 8998
HTTP/2 remain enabled. This is an explicit tradeoff: GM static resources may
transfer more bytes; cached responses cannot bypass a new GM security policy.

## Testing without hardware

`//services/gm_key:gm_key_service_tests` uses a test-only SKF DLL with real SM2
keys generated and retained in a separate process. It covers signatures,
incorrect/locked PINs, removal, cancellation, capability limits, timeouts and
crashes. The network tests connect this service through the actual asynchronous
signer/provider interfaces to real loopback TLS servers. Only test targets can
load the unsigned simulator; production hash/signature enforcement is retained.

`//chrome/browser/gm:gm_browser_unittests` tests managed-policy parsing,
origin/identity authorization, queued prompts, cancellation before commit,
local issuer discovery, trust monitoring and native PIN UI. The focused Views
runner loads `gen/chrome/generated_resources_en-US.pak`; this file is declared
as runtime test data. Dialog tests cover invalid-input correction, cancellation,
`CloseNow()` and direct native-window destruction.
`//content/test:gm_content_unittests` uses the normal Content test harness for
page/request ownership, synchronous completion and cancellation. Select
`StoragePartitionImplGmCertificateTest.*` to run only the GM cases.
`//net/gm:gm_net_unittests` and `gm_network_service_unittests` cover routing,
pools, caches, normal TLS regression and actual TLS/Key I/O. Installer staging
is checked by `net/gm/tests/installer_packaging_test.py`.

These focused executables complement the standard suites. On Windows x64,
the browser GM unit sources are also dependencies of `//chrome/test:unit_tests`;
the StoragePartition cases remain in `//content/test:content_unittests`; net GM
sources also belong to `//net:net_unittests`; and Network Service coverage belongs
to `//services:services_unittests` through `//services/network:tests`. Tests that
need the linked GM implementation are conditional on its build configuration.

From the source root, use the existing `out/Release_64` directory and retain its
GN settings while enabling GM as described in `//net/gm/BUILDING.md`.
The browser and focused test executables have been linked and run in that
directory. Use the report below for exact passing layers and independent
interoperability failures; the commands reproduce coverage rather than certify
hardware or every peer:

```powershell
autoninja -C out/Release_64 gm_browser_unittests gm_content_unittests gm_net_unittests gm_network_service_unittests gm_browser_browsertests
python3 net/gm/run_tests.py --build-dir out/Release_64 --include-network --include-browser
python3 net/gm/run_tests.py --build-dir out/Release_64 --only content
```

`--include-browser` includes the Content ownership layer. Use `--only
browser_unit` for native PIN/KeyManager tests or `--only browser_navigation`
for real-browser navigation tests. These commands describe how to run coverage;
their inclusion here does not establish a passing result.

The actual production EXE separately passed a headless CDP check of startup,
a local data-page DOM marker and graceful exit. It does not prove TLS, visible
GUI or physical Key behavior. Earlier `--dump-dom` checks timed out twice at
60 seconds; their root cause is unresolved, and the passing CDP check neither
fixes that method nor excludes a connection to the GM changes. Full evidence
is retained in the current report.

See `//net/gm/BORINGSSL_TEST_RESULTS.md` for current executed results, measured
cost and explicit interoperability limits. `TEST_RESULTS.md` is the historical
dual-library record. Simulation validates software behavior, not Feitian
hardware, proprietary SDK compatibility or cryptographic product certification.
SSL configuration manager tests run in `gm_browser_unittests`, whose runner
initializes Mojo; the browser launcher contains only in-process browser tests.
