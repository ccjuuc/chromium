# CrossTransfer

A transport-independent file-transfer core for authenticated LAN connections and
outbound HTTPS relay connections across different networks. The same session,
resume protocol and transactional store are used on both paths.

## Boundaries and ownership

- `ConnectionManager`: applies a finite direct-connection budget, then tries the
  injected relay connector within the remaining overall budget. Secure channels
  are required by default; a failed TLS connection never falls back to plaintext.
- `ITransportChannel`: bounded framed I/O on one dedicated worker sequence. It
  exposes cancellation, deadlines and typed transport errors. Keep it off the UI
  and networking event loops. `Cancel()` changes an atomic flag; do not destroy
  the session/channel until its worker has returned.
- `TlsTransportChannel`: TLS 1.2+ LAN transport with mutual certificate validation
  and verification of the expected peer SAN. `LanTcpClient` supports IPv4, IPv6,
  hostnames and candidate racing. Raw `LanTcpConnection` is a low-level test and
  TLS-adapter building block, not the default production security policy.
- `HttpsRelayChannel`: authenticated HTTPS frame mailboxes. Both peers connect
  outbound; no inbound NAT mapping is required. Role-specific bearer credentials
  and a fresh connection-attempt ID are supplied by the pairing layer.
- `SenderSession` / `ReceiverSession`: one persistent file task, shared wire
  validation, durable acknowledgements, bounded send window and resume handshake.
  File preparation/verification runs on a joined disk worker. An overall phase
  deadline is independent of the timeout for ordinary network I/O.
- `TransferStore`: owns safe destination paths, partial files, checkpoint identity,
  durable progress and verify-before-atomic-commit. Network code never renames or
  truncates destination files.

The core provides client/library components, with a browser test page described
below. A product still supplies device discovery, trusted certificates/credentials,
receiver admission decisions and persistent job records. Local pairing/discovery
must also work locally if offline WLAN operation is a product requirement.

## Connection and transfer flow

1. The trusted pairing layer exchanges candidate endpoints and provisions peer
   identity (LAN) or a relay room with separate sender/receiver credentials.
2. `ConnectionManager` invokes a TLS `DirectConnector`; on failure it invokes an
   HTTPS `RelayConnector`. Both factories must obey the passed deadline/cancel
   token. A relay room is usable only if both devices can reach the service.
3. `SenderSession` chooses a random 128-bit task ID (or uses the caller's persisted
   ID). Persist `sender.task_id()` with the file job so a new process can resume.
4. The receiver requires `TransferOptions::authorize_receive(peer, metadata)`.
   Device-name metadata is untrusted display information; authentication comes
   from the verified channel and pairing layer.
5. Peers exchange metadata and a bounded persisted bitmap. The sender transmits
   only missing chunks. A chunk ACK means both data and its checkpoint are durable.
6. The store hashes the temporary file, atomically replaces the chosen destination
   only on success, and sends completion. If the completion ACK is lost, reconnect
   using the same task ID and metadata to confirm the committed file.

Peers require `finish-confirm-v1` in both handshake messages. After the
receiver's durable completion reply, the sender confirms that it received the
reply. A receiver that loses this confirmation can join the same-task recovery;
the completed checkpoint avoids retransmitting file contents. A peer with a
different session protocol is rejected during the handshake. Local commit and delivery of the
completion reply are tracked separately: a broken confirmation connection must
not discard a saved file or received text.

A connection attempt has a new relay `connection_id`, shared by the two peers.
It is separate from the persistent file `task_id`. After a network change, obtain
another channel and rerun the same file task; the application coordinates the new
connection attempt. Channel selection does not create another file task.

## TLS and relay configuration

LAN peers use `TlsConfig{ca_file, certificate_file, private_key_file, peer_name}`.
`peer_name` is the expected certificate SAN, not an unverified device-name string.
The listener accepts a TCP connection and passes it to `TlsTransportChannel::Accept`.
The client calls `TlsTransportChannel::Connect`. Both peers must present trusted
certificates. The test runner generates temporary fixtures; those credentials
must never be used outside tests.

`HttpsRelayOptions` contains `host`, `port`, `ca_file`, `room_id`, `connection_id`,
`role`, `bearer_token` and `timeout_ms`. Certificates are always verified, including
hostname/IP identity. Supply an explicit CA bundle when the TLS library's default
trust store is not configured. There is no skip-verification switch.

`relay/server.py` is a bounded reference HTTPS service. It limits connections,
headers, frames, per-role queues, global queued bytes and idle room lifetimes.
Reads use explicit delivery sequence acknowledgements so a timed-out HTTP read
cannot silently discard a transfer frame. Ambiguous writes are not retried within
an attempt; establish a fresh attempt and use file-level resume.

Provision a private JSON file with this schema (replace values with distinct,
random 256-bit hexadecimal credentials generated by a CSPRNG):

```json
{
  "paired-device-room": {
    "sender_token": "<64 hex characters>",
    "receiver_token": "<different 64 hex characters>"
  }
}
```

Run the service with a certificate for its actual hostname:

```sh
python3 cross_transfer/relay/server.py \
  --host 127.0.0.1 --port 8443 \
  --certificate /secure/relay-chain.pem \
  --private-key /secure/relay-key.pem \
  --credentials /secure/relay-rooms.json
```

The default listener is loopback. Internet use requires an explicitly configured
reachable service, appropriate TLS termination/routing and protected credential
provisioning. No public service is deployed by this repository.

**Trust model:** the HTTPS relay terminates TLS and can read transferred data. It
must be trusted. This is not end-to-end encryption through an untrusted relay.
LAN mutual TLS is encrypted directly between the paired peers. The reserved
`public_key_hex` handshake field does not implement E2EE.

The browser embeds Chromium's native libwebrtc for ICE and a reliable ordered
DTLS/SCTP data channel, keeping the same session/store contracts. Account HTTPS
signaling authenticates the SDP fingerprints and ICE candidates. It requires no
page JavaScript connection and continues after the page closes. There is no TURN
dependency: failed ICE uses the existing HTTPS relay. The reference HTTPS
adapter currently opens one bounded TLS connection per HTTP request. It is
intended to establish functional correctness; connection pooling and production
network-stack integration should precede high-throughput deployment. Automatic
HTTP proxy discovery/authentication is not implemented, and port 443 alone does
not guarantee reachability through enterprise proxies.

## Wire and recovery rules

The wire version is **2**; version 1 peers are rejected. Frame headers are encoded
explicitly as 38 bytes in network byte order; C++ struct layout is not the wire
format. Shared validation applies before allocation on every transport.

Limits: chunks/frames at most 4 MiB, at most 8,388,608 chunks, file size at most
8 TiB, filename at most 255 bytes, bitmap at most 1 MiB plus its 16-byte header.
Files are identified by full SHA-256, size, chunk size, filename and task ID.
SHA-256 uses the same EVP implementation on every platform. Empty files have zero
chunks. Filenames are UTF-8 single components with portable naming restrictions.

`io_timeout_ms` limits ordinary network operations, while `stage_timeout_ms`
bounds source preparation, destination verification and waiting for the peer's
phase response. Configure finite stage deadlines for the supported file sizes
and disk performance. The sender prepares its source before establishing the
test page's connection; processing heartbeats are not currently emitted.

Partial files and checksummed checkpoints live under `.cross-transfer/` in the
destination. Successful commit checkpoints are retained to recover a lost final
ACK. The embedding application owns completed-job retention/cleanup, after both
peers have recorded completion. Do not remove an active task's state.

There is currently one active receiving transaction per output directory. A
cross-process lock prevents concurrent case-folded/Unicode-alias targets from
corrupting each other. Hash failure resets durable progress for a clean retry;
cancellation preserves completed chunks. Replacement of an existing destination
must be approved by the application's receiver admission policy and enabled via
`TransferOptions::allow_overwrite`. Otherwise the final atomic rename also
rejects a target that appears after admission, preserving it and resumable data.

## Build and verification

### Browser test page

After rebuilding the browser, open `chrome://cross-transfer-test/` on each
device, or click **CT — 跨端传输测试** in the sidebar to open a new tab.
Register an account, then sign in with the same account on another device to
see both devices and their online state. The browser restores its server token
at startup and maintains device heartbeats without leaving this page open.
Tokens are stored directly in the local Profile; passwords are not saved.
See [account service](account_service/README.md) for the API and account tests.

Select **发送** beside an online device, then enter text or choose a file using
the native file picker. The other device accepts or rejects the invitation in
this page. Accepted text can be copied; accepted files show progress and their
saved location. The browser chooses the route using the advertised size threshold when a new
connection is needed. By default, payloads below 1024 bytes go straight to the HTTPS relay;
payloads of 1024 bytes or more first try an authenticated WebRTC data channel
between the devices. If ICE cannot connect, both endpoints agree to use
the HTTPS relay automatically. The page shows **设备直连** or
**中继传输** for the active or most recent payload job. Text invitations contain
metadata only; text content travels through the same payload channel as files.
Peer addresses, DTLS fingerprints and relay credentials are supplied
automatically. Account transfers and invitation polling belong to
the Profile and continue when this page is closed or reloaded; signing out or
ending the Profile cancels its local jobs.

The browser owns the complete task list and recent history, scoped to the
account and device. Invitations, acceptance, progress and terminal results are
handled locally. The server only forwards opaque messages and supplies temporary
transport resources; it has no task list, task-state transitions or completion
report endpoint. Message delivery receipts do not mean a payload succeeded.
Local cancellation releases the task slot even if its notification is delayed.
Invitations keep their original sender deadline across retries; either endpoint
notifies cancellation on expiry. Active payload jobs use their own I/O and stage
timeouts, so the invitation deadline cannot interrupt a progressing transfer.

Completion recovery uses the peer protocol and the receiver's checked local
checkpoint. Discovery, invitations and fresh ICE negotiation still use the
server for forwarding. Direct payloads and completion receipts travel between
the devices. If a direct connection breaks, the HTTPS relay may carry recovery
traffic; this does not provide offline direct reconnection after both browsers
have lost their connection. Recent local history survives a Profile restart.

After a successful direct transfer, each browser keeps the authenticated channel
in its Profile for up to ten idle minutes (at most eight devices). The next
accepted invitation first tries this channel, including small messages and
reverse-direction sends, without repeating server connection negotiation.
A fresh invitation-bound handshake authenticates the new task over that channel.
Failed reuse closes the old connection before normal connection/relay fallback.
Signing out, changing the STUN setting, or closing the browser clears this local
cache. A separate local record keeps up to 32 successful direct peers per
account/device. Once the cached channel expires or is cleared, those peers still
try a fresh direct connection before the small-payload relay preference.

Each worker releases its own temporary signaling resource asynchronously after
use. Both roles must release before the server removes the shared snapshots;
this resource cleanup carries no task state and cannot block local completion.

The account service provides device discovery and generic message forwarding
over TCP 443. Browsers interpret invitations and coordinate their own routes. ICE gathers IPv4/IPv6 interface candidates and
tests candidate pairs in both directions. The local **使用 STUN** setting is
disabled by default, saved per Profile, and affects the next transfer. Enabling
it uses only STUN URLs advertised by the account server to discover NAT mappings
and improve UDP hole punching. An absent or unreachable STUN service does not
prevent interface-address checks or HTTPS fallback. The combined server can
optionally serve STUN on UDP 443 in the same process; deployments without STUN
omit `--stun-port` and `--stun-url`. STUN does not carry payload data, and symmetric
NATs or restrictive firewalls may still require the relay. Only one text or file
payload job runs per Profile at a time.

Expand **高级传输诊断** for the existing manual file transfer controls below.
These diagnostic controls call the native transfer core on a worker and poll
its real status. Closing or reloading the page cancels the diagnostic job;
acknowledged file progress remains available for resuming.

- Choose **接收文件** first, enter an absolute destination path, and start it
  after preparing the sender's configuration. **发送文件** takes the absolute
  path of one existing local file.
- **局域网直连** requires each device's own certificate/private key and the
  expected peer certificate name. The CA path is optional when the TLS default
  trust store already trusts the peer. The receiver defaults to IPv4 address
  `0.0.0.0`, port `53317`; the sender uses the receiver's reachable address.
- **服务器中继** takes a reachable HTTPS host/port, provisioned room ID, a fresh
  shared connection ID and that device's role-specific token. It does not
  require client certificate/private-key fields. The optional CA path is only
  needed when the relay certificate is outside the TLS default trust store.
- Retain the displayed task ID to retry the same source file and destination.
  Use a fresh shared connection ID for each relay attempt. Clear the task ID
  before testing a different file. Existing target files are not replaced unless
  **允许覆盖** is explicitly selected; confirming an already committed matching
  task does not replace it.

After cancellation or an interrupted relay connection, the room can remain
leased until the server's idle timeout (60 seconds by default). Stop both peers
before retrying, allow the idle lease to expire, and use a new connection ID.

The manual diagnostics do not deploy a relay, change certificates, persist
relay credentials, or simulate transfer progress. All paths refer to the local device. A static HTML
preview disables transfers because it has no native WebUI bridge.

The standalone `test_test_job` verifies the job used by this page; the TLS
fixture runner also exercises real direct and relay transfers, cancellation and
resumption through that job. Run `node cross_transfer/tests/webui_smoke_test.mjs`
to verify page interactions with an isolated test bridge in headless Chrome
(Node.js 18+; set `CHROME_BINARY` if Chrome is not detected). This checks UI
behavior; it does not exercise the browser's native message handler.

Build `chrome xenon_account_unittests` and set `CHROME_BINARY` to the built
browser for account transfer verification:

```sh
out/Release_64/xenon_account_unittests.exe --gtest_filter=XenonTransferServiceTest.*
node cross_transfer/tests/account_webui_smoke_test.mjs
node cross_transfer/tests/account_browser_integration.mjs --with-transfers
node cross_transfer/tests/account_browser_integration.mjs --expect-transport=direct
node cross_transfer/tests/account_browser_integration.mjs --expect-transport=direct --expect-small-transport=relay
node cross_transfer/tests/account_browser_integration.mjs --expect-transport=direct --expect-small-transport=relay --fresh-direct-runs=35
node cross_transfer/tests/account_browser_integration.mjs --with-file
```

The live tests use two temporary Profiles and a generated server account.
`--with-transfers` checks Unicode text reception, rejection and cancellation.
The text check confirms that the invitation has no text content and that both
native jobs report the same actual route. `--expect-transport=direct` or
`--expect-transport=relay` additionally requires that route and enables the text
checks. Use a network where the requested route is available; these flags do
not alter the browser's connection policy. The direct check generates at least
1024 bytes of Unicode text to clear the default small-payload threshold.
`--expect-small-transport=relay` additionally sends a separate text below 1024
bytes and verifies that both endpoints used relay. This check is opt-in so the
ordinary smoke test does not assume a fixed server threshold; its route is
recorded separately as `small_text` in the result.
`--fresh-direct-runs=N` (1–64, with `--expect-transport=direct`) clears both
connection caches before each small-text transfer and records every result.
Use 35 rounds to exercise channel cleanup beyond the per-account limit of 32
simultaneous signaling channels. The local successful-peer preference remains
available, so each round must negotiate a new direct connection.
`--with-file` also opens a visible native file picker: select the generated
source named in `out/own_transfer_file_selection.json` when the script prints
`WAIT_FILE_SELECTION`. It verifies a 1 MiB transfer and SHA-256 under an isolated
download directory and checks the file route too. Both modes retain the account restoration and logout
checks. Temporary files and Profiles are removed automatically; remove the
generated server account using the identity in
`out/account_browser_integration_result.json` after testing.

Two Profiles on one machine exercise real browser and server code, but do not
verify connectivity between separate devices or across NATs. A small payload
using the server's size policy also does not test fallback after ICE failure.

### Native targets and tests

Chromium GN target: `//cross_transfer:cross_transfer_core`; dependency on bundled
BoringSSL, public include configuration, platform sources and Windows socket
libraries are declared in `BUILD.gn`. Test executables are declared there too.

Standalone tests:

```sh
bash cross_transfer/build_and_test.sh
# Optional sanitizer build in a separate output directory:
CROSS_TRANSFER_SANITIZE=1 CROSS_TRANSFER_BUILD_DIR=/tmp/cross-transfer-asan \
  bash cross_transfer/build_and_test.sh
```

The script uses `OPENSSL_ROOT_DIR`, an existing local Chromium BoringSSL build,
or `pkg-config openssl`, in that order. It does not install dependencies. TLS tests
need Python 3, the `openssl` command and permission to bind loopback TCP sockets.

Regression coverage includes malformed/oversized frames, arithmetic bounds,
SHA-256 vectors, partial-file reopening, true disconnect/restart resume, hash
failure preserving existing files, commit-ACK recovery, empty files, cancellation,
TLS peer verification, HTTPS authorization, queue bounds and connection fallback.

Local tests cannot certify all network/platform combinations. Release acceptance
still needs two real devices on WLAN, separate NATs, Wi-Fi/cellular, AP isolation,
network switching and native Windows/Android/iOS builds. The library does not
supply mobile background execution permissions or lifecycle integration.
