# HTTPS relay

`server.py` is a bounded reference relay for peers that cannot establish a direct
connection. Both peers initiate outbound HTTPS connections. The C++
`HttpsRelayChannel` uses the same validated frames and `SenderSession` /
`ReceiverSession` protocol as the mutually authenticated TLS direct channel.

The relay terminates TLS and can read frame contents. It is a trusted service;
this implementation does not claim end-to-end encryption through the relay.
Direct `TlsTransportChannel` connections authenticate both peer certificates and
their expected SAN identities. HTTPS clients always verify the service's CA and
hostname; there is no insecure TLS switch.

The browser can also negotiate a native WebRTC DataChannel through the account
API. ICE tests local IPv4/IPv6 and server-reflexive candidates collected using
STUN, so peers behind compatible NATs can establish a direct connection. DTLS
protects the direct DataChannel. Unsupported NAT/firewall paths fall back to the
same HTTPS relay; this service does not implement TURN.
The browser's local STUN setting is disabled by default and saved per Profile.
Enabling it uses only server-advertised STUN URLs. A deployment without STUN
still supports interface-address ICE checks and HTTPS fallback.

## Local integration test

The normal `../build_and_test.sh` compiles and runs the C++ integration clients,
then invokes `../tests/test_https_relay.py`. The runner creates a temporary CA,
server and peer certificates, binds only loopback, verifies actual file transfers
through both channels, and removes the fixture afterward. It also checks bad
credentials, hostname mismatch, lease conflicts, frame limits, cancellation,
deadlines, mailbox redelivery and queue cleanup.

## Service configuration

Provision a JSON file with one room per transfer rendezvous. Each room has
distinct, cryptographically random 256-bit hex credentials for its two roles:

```json
{
  "room-from-pairing-service": {
    "sender_token": "<64 hex characters>",
    "receiver_token": "<different 64 hex characters>"
  }
}
```

Protect this file and the TLS private key with owner-only permissions. Deliver
each token only to its authorized role through the application's authenticated
pairing/control plane. Tokens are authorization capabilities, not user-facing
short pairing codes. They must not appear in URLs, logs or analytics. Rotate or
remove room credentials when the application revokes a transfer invitation.
Static rooms load at startup. An optional
[account, device and forwarding API](../account_service/README.md) runs in the
same HTTPS process when `--account-database` is supplied. It provisions temporary
rooms only when a client requests a relay channel, with two fixed same-account
devices and separate role credentials. Account tokens cannot be used directly
on relay frame routes. No peer IP, certificate path or role token needs to be
entered in the browser UI.

At most 64 dynamic rooms coexist with configured static rooms. Dynamic
credentials and leases expire within 30 minutes; logout or device revocation
invalidates associated resources. Explicit channel release permits existing
leases to drain for at most 15 seconds. The server does not interpret invitation
acceptance, cancellation or completion, and it does not maintain a task list.
Service restart discards temporary messages and channels while preserving
accounts, sessions and devices. Browsers own task history and results.
Tests in `../tests/test_account_relay.py` cover role separation, expiry,
revocation during polling, final-frame draining and restart behavior.

```sh
python3 server.py --host 127.0.0.1 --port 8443 \
  --certificate /protected/relay-chain.pem \
  --private-key /protected/relay-key.pem \
  --credentials /protected/relay-rooms.json
```

For the combined production service, `--stun-port 443` adds a UDP Binding
listener to this same process alongside HTTPS on TCP 443. Announce its public
address with `--stun-url stun:115.29.16.37:443` (repeatable for additional STUN
servers). The UDP listener is disabled by default; advertised URLs support
`stun:` and `stuns:` only. The built-in listener implements UDP STUN, so a
`stuns:` URL must refer to a separately available TLS STUN server. Allow both
TCP 443 and UDP 443 through the server firewall/security group. STUN reveals
the source address and port to the requesting device; it carries no payload.

The Binding listener accepts at most 1200-byte RFC 5389 requests with the
correct cookie, exact aligned lengths and valid attribute boundaries. It
returns only a minimal XOR-MAPPED-ADDRESS (32 bytes for IPv4, 44 for IPv6),
verifies an optional FINGERPRINT, and silently ignores unsupported mandatory
attributes or invalid packets. A source IP is limited to 20 replies/second
with a burst of 40; the process allows 200 replies/second with a burst of 400.
Source limiter state is capped at 4096 entries. One daemon thread services UDP;
server shutdown closes the socket and joins the thread. Neither addresses nor
request data are logged. Run `python3 ../tests/test_stun.py` to verify the wire
format, input bounds, rate limits, and actual UDP socket lifetime.

To operate on different networks, deploy the service on a reachable HTTPS
endpoint with a certificate for that hostname, configure firewall access, and
give both clients its hostname, port and trust configuration. The example above
intentionally binds loopback. No public deployment is performed by the build or
tests. Outbound HTTP proxy discovery/authentication is not implemented by this
standalone adapter; using port 443 alone does not guarantee proxy traversal.

## Rendezvous and mailbox semantics

The control plane supplies both peers the same `room_id` and random 128-bit hex
`connection_id` for each connection attempt. This connection ID is independent
of the file task ID persisted by `TransferSession`. Client-generated leases
prevent concurrent consumers for one role. A different generation cannot join
an active room. After both peers close, or the room's idle TTL expires, the
service discards its queued frames and releases the leases. File progress is
durable on the receiving device, so a new rendezvous can negotiate recovery.

Requests use `/v1/rooms/{room}/{connection_id}/{sender|receiver}/{operation}`,
`Authorization: Bearer ...` and `X-Relay-Lease` headers:

| Method / operation | Meaning |
| --- | --- |
| `POST open` | Acquire the role's lease. |
| `POST frames` | Queue one bounded serialized protocol frame for the peer. |
| `GET frames` | Peek one incoming frame, or return `204` after a short wait. |
| `DELETE open` | Release the role after acknowledging its last received frame. |

Each delivered frame carries `X-Relay-Sequence`. The client acknowledges a fully
received and validated frame in `X-Relay-Ack` on its next request. The service
retains a frame until that acknowledgement, so an expired long poll or lost
HTTP response cannot consume a later protocol frame. This mailbox receipt is
separate from the file protocol's durable chunk acknowledgement.

Only explicit `204` and `429` responses may be retried inside an operation;
ambiguous network writes fail the channel and require a new connection attempt.
All client operations have an overall deadline and check cancellation. Close
attempts lease release for at most 250 ms; a cancelled or unreachable client is
reclaimed by the service TTL.

## Resource limits

The service accepts at most 16 concurrent connections and 64 configured rooms.
TLS handshakes have a 5 second timeout; request headers and body share a 10
second absolute deadline. Headers are limited to 8 KiB. Each frame is at most
4 MiB plus its 38 byte wire header. Each destination queue has at most 64 frames
and about 8 MiB of data; all queues share a 32 MiB budget. Idle rooms expire after
60 seconds by default (`--idle-timeout`, bounded to 1–300 seconds). These bounds
are intentional resource controls, not throughput or availability guarantees.

One HTTP/TLS request is used per mailbox operation. This favors a simple,
testable ownership model. Future persistent HTTP or WebSocket adapters can
optimize handshake overhead without duplicating the file session or storage
protocol. Run channel work on a dedicated worker sequence, never the UI or a
network-library callback thread.
