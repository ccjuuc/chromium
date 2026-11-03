#!/usr/bin/env python3
"""HTTP integration tests for the account and same-account device directory.

Run: python cross_transfer/account_service/test_server.py
The service binds only ephemeral loopback ports and uses temporary databases.
"""

import base64
import concurrent.futures
import copy
from contextlib import closing
import http.client
import importlib.util
import json
from pathlib import Path
import secrets
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock


_SPEC = importlib.util.spec_from_file_location(
    "cross_transfer_account_server", Path(__file__).with_name("server.py"))
account_server = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = account_server
_SPEC.loader.exec_module(account_server)

PASSWORD = "Account integration test password 2026!"
_UNSET = object()


class AccountServiceIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="cross-transfer-account-")
        self.database = Path(self.directory.name) / "accounts.sqlite3"
        self.server = None
        self.thread = None
        self.start_server()

    def tearDown(self):
        self.stop_server()
        self.directory.cleanup()

    def start_server(self, **options):
        self.server = account_server.create_server(
            "127.0.0.1", 0, self.database, **options)
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()

    def stop_server(self):
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=2)
            self.assertFalse(self.thread.is_alive(), "server did not shut down")
            self.server = None
            self.thread = None

    def restart_server(self, **options):
        self.stop_server()
        self.start_server(**options)

    def api(self, method, path, payload=_UNSET, *, token=None,
            headers=None, raw_body=None):
        if path != "/healthz":
            path = "/api/v1" + path
        request_headers = {"Content-Type": "application/json"}
        if token is not None:
            request_headers["Authorization"] = "Bearer " + token
        request_headers.update(headers or {})
        body = raw_body
        if payload is not _UNSET:
            self.assertIsNone(raw_body)
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        connection = http.client.HTTPConnection(
            *self.server.server_address, timeout=3)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            status = response.status
            response_headers = dict(response.getheaders())
            encoded = response.read()
        finally:
            connection.close()
        self.assertIn("application/json", response_headers.get("Content-Type", ""))
        self.assertEqual(response_headers.get("Cache-Control"), "no-store")
        return status, json.loads(encoded.decode("utf-8"))

    def register(self, username="alice", password=PASSWORD):
        status, result = self.api("POST", "/auth/register", {
            "username": username, "password": password,
        })
        self.assertIn(status, (200, 201))
        self.assertRegex(result["token"], r"^[0-9a-f]{64}$")
        self.assertIsInstance(result["account"]["id"], str)
        self.assertTrue(result["account"]["id"])
        self.assertGreater(result["expiresAt"], time.time())
        self.assertNotIn("password", result["account"])
        return result

    def assert_error(self, response, status, code):
        actual_status, result = response
        self.assertEqual(actual_status, status)
        self.assertEqual(result["error"]["code"], code)
        self.assertIsInstance(result["error"]["message"], str)
        return result

    def device(self, name="Test laptop"):
        return {
            "displayName": name,
            "platform": "windows",
            "publicKey": base64.b64encode(secrets.token_bytes(32)).decode("ascii"),
        }

    def put_device(self, token, identity, value=None):
        status, result = self.api(
            "PUT", "/devices/" + identity, value or self.device(), token=token)
        self.assertIn(status, (200, 201))
        self.assertEqual(result["device"]["id"], identity)
        return result["device"]

    def device_pair(self):
        session = self.register()
        sender, receiver = secrets.token_hex(16), secrets.token_hex(16)
        self.put_device(session["token"], sender, self.device("sender"))
        self.put_device(session["token"], receiver, self.device("receiver"))
        return session, sender, receiver

    def signal(self, sender, receiver, payload=None):
        return {"id": secrets.token_hex(16), "fromDeviceId": sender,
                "toDeviceId": receiver, "payload": payload if payload is not None else {"hello": "world"}}

    def exchange(self, token, identity, device, peer, role, sequence=0, payload=None):
        return self.api("POST", "/channels/" + identity + "/exchange", {
            "deviceId": device, "peerDeviceId": peer, "role": role, "sequence": sequence,
            "payload": payload if payload is not None else {}}, token=token)

    def release(self, token, identity, device, peer, role):
        return self.api("POST", "/channels/" + identity + "/release", {
            "deviceId": device, "peerDeviceId": peer, "role": role}, token=token)

    def test_opaque_mailbox_repeated_read_ack_and_send_retry(self):
        session, sender, receiver = self.device_pair()
        token = session["token"]
        signal = self.signal(sender, receiver, {"anything": {"type": "unrecognized", "fileName": "private-name"}})
        status, sent = self.api("POST", "/signals", signal, token=token)
        self.assertEqual(status, 201)
        envelope = sent["signal"]
        self.assertEqual(set(envelope), set(signal) | {"fromName", "toName", "createdAt", "expiresAt"})
        self.assertEqual(envelope["payload"], signal["payload"])
        self.assertEqual(self.server.application.channels, {})
        self.assertEqual((envelope["fromName"], envelope["toName"]), ("sender", "receiver"))
        self.assertAlmostEqual(envelope["expiresAt"] - envelope["createdAt"], 300)
        path = "/devices/" + receiver + "/signals"
        for _ in range(2):
            self.assertEqual(self.api("GET", path, token=token), (200, {"signals": [envelope]}))
        self.assertEqual(self.api("POST", "/signals", signal, token=token), (200, sent))
        # A different mailbox cannot acknowledge this message.
        self.api("POST", "/devices/" + sender + "/signals/ack", {"messageIds": [signal["id"]]}, token=token)
        self.assertEqual(len(self.api("GET", path, token=token)[1]["signals"]), 1)
        self.assertEqual(self.api("POST", path + "/ack", {"messageIds": [signal["id"]]}, token=token),
                         (200, {"ok": True}))
        self.assertEqual(self.api("GET", path, token=token), (200, {"signals": []}))
        self.assertEqual(self.api("POST", "/signals", signal, token=token), (200, sent))
        self.assertEqual(self.api("GET", path, token=token), (200, {"signals": []}))
        receipt = self.server.application.receipts[signal["id"]]
        self.assertNotIn("payload", receipt)
        self.assertNotIn("private-name", json.dumps(receipt))
        signal["payload"] = {"changed": True}
        self.assert_error(self.api("POST", "/signals", signal, token=token), 409, "signal_conflict")

    def test_signal_permissions_shape_and_payload_bounds(self):
        session, sender, receiver = self.device_pair()
        token = session["token"]
        other = self.register("other")
        signal = self.signal(sender, receiver)
        self.assert_error(self.api("POST", "/signals", signal, token=other["token"]), 404, "not_found")
        self.assert_error(self.api("GET", "/devices/" + receiver + "/signals", token=other["token"]), 404, "not_found")
        self.assert_error(self.api("POST", "/devices/" + receiver + "/signals/ack",
                                   {"messageIds": []}, token=other["token"]), 404, "not_found")
        for fields in ({"id": "bad"}, {"toDeviceId": sender}, {"payload": []},
                       {"taskId": "a" * 32}, {"type": "offer"}):
            self.assert_error(self.api("POST", "/signals", {**signal, **fields}, token=token), 400, "invalid_request")
        for ids in (["bad"], [signal["id"]] * 65, "not-a-list"):
            self.assert_error(self.api("POST", "/devices/" + receiver + "/signals/ack",
                                       {"messageIds": ids}, token=token), 400, "invalid_request")
        signal["payload"] = {"x": "x" * (account_server.MAX_SIGNAL_PAYLOAD - 8)}
        self.assertEqual(self.api("POST", "/signals", signal, token=token)[0], 201)
        signal["id"] = secrets.token_hex(16)
        signal["payload"]["x"] += "x"
        self.assert_error(self.api("POST", "/signals", signal, token=token), 413, "invalid_request")

    def test_wide_payloads_are_retained_as_bounded_serialized_bytes(self):
        session, sender, receiver = self.device_pair()
        token, app = session["token"], self.server.application
        payload = {"nodes": [{} for _ in range(5457)]}
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.assertEqual(len(encoded), 16382)
        signal = self.signal(sender, receiver, payload)
        self.assertEqual(self.api("POST", "/signals", signal, token=token)[1]["signal"]["payload"], payload)
        retained = app.mailboxes[receiver][signal["id"]]["payload"]
        self.assertIsInstance(retained, bytes)
        self.assertEqual(retained, encoded)
        # The former parsed tree occupied almost 400 KiB for this 16 KiB JSON.
        self.assertLess(sys.getsizeof(retained), len(encoded) + 64)
        self.assertEqual(self.api("GET", "/devices/" + receiver + "/signals", token=token)[1]
                         ["signals"][0]["payload"], payload)
        identity = secrets.token_hex(16)
        wide = {"nodes": [{} for _ in range(38225)]}
        body = {"deviceId": sender, "peerDeviceId": receiver, "role": "sender",
                "sequence": 0, "payload": wide}
        self.assertEqual(self.api("POST", "/channels/" + identity + "/exchange", token=token,
                                 raw_body=json.dumps(body, separators=(",", ":")).encode("utf-8"))[0], 200)
        retained = app.channels[identity]["slots"]["sender"]["payload"]
        self.assertIsInstance(retained, bytes)
        self.assertEqual(len(retained), 114686)
        self.assertLess(sys.getsizeof(retained), len(retained) + 64)
        self.assertEqual(self.exchange(token, identity, receiver, sender, "receiver")[1]["peer"]["payload"], wide)
        self.assertEqual(app.opaque_bytes, 16382 + 114686 + 2)

    def test_payload_budget_retries_ack_replacement_release_and_expiry(self):
        session, sender, receiver = self.device_pair()
        token, app = session["token"], self.server.application
        identity = secrets.token_hex(16)
        signal = self.signal(sender, receiver, {"a": 1, "b": 2})
        size = len(b'{"a":1,"b":2}')
        with mock.patch.object(account_server, "MAX_OPAQUE_BYTES", size * 2):
            self.assertEqual(self.api("POST", "/signals", signal, token=token)[0], 201)
            self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", payload=signal["payload"])[0], 200)
            self.assertEqual(app.opaque_bytes, size * 2)
            # Canonically identical retries do not consume another reservation.
            signal["payload"] = {"b": 2, "a": 1}
            self.assertEqual(self.api("POST", "/signals", signal, token=token)[0], 200)
            self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", payload=signal["payload"])[0], 200)
            self.assertEqual(app.opaque_bytes, size * 2)
            self.assert_error(self.api("POST", "/signals", self.signal(sender, receiver), token=token), 429, "limit_exceeded")
            rejected = secrets.token_hex(16)
            self.assert_error(self.exchange(token, rejected, sender, receiver, "sender"), 429, "limit_exceeded")
            self.assertNotIn(rejected, app.channels)
            self.assert_error(self.exchange(token, identity, sender, receiver, "sender", 1, {"x": "x" * 20}),
                              429, "limit_exceeded")
            self.assertEqual(app.channels[identity]["slots"]["sender"]["sequence"], 0)
            ack = "/devices/" + receiver + "/signals/ack"
            self.assertEqual(self.api("POST", ack, {"messageIds": [signal["id"]] * 2}, token=token)[0], 200)
            self.assertEqual(app.opaque_bytes, size)
            self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", 1, {"x": "x" * 15})[0], 200)
            self.assertEqual(app.opaque_bytes, 23)
            self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", 2, {})[0], 200)
            self.assertEqual(app.opaque_bytes, 2)
            self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", 1, {"x": "x" * 40})[0], 200)
            self.assertEqual(app.opaque_bytes, 2)
            signal = self.signal(sender, receiver, {"a": 1, "b": 2})
            self.assertEqual(self.api("POST", "/signals", signal, token=token)[0], 201)
            self.assertEqual(self.api("DELETE", "/channels/" + identity, token=token)[0], 200)
            self.assertEqual(app.opaque_bytes, size)
            with app.signal_lock:
                app.receipts[signal["id"]]["expiresAt"] = time.time() - 1
            self.server.service_actions()
            self.assertEqual(app.opaque_bytes, 0)

    def test_payload_depth_and_non_json_values_are_rejected_at_http_boundary(self):
        session, sender, receiver = self.device_pair()
        token = session["token"]
        nested = {}
        for _ in range(account_server.MAX_PAYLOAD_DEPTH + 1):
            nested = {"child": nested}
        # A null sibling cannot bypass the depth walk.
        self.assert_error(self.api("POST", "/signals", self.signal(sender, receiver, {"null": None, "deep": nested}),
                                   token=token), 400, "invalid_request")
        self.assert_error(self.exchange(token, secrets.token_hex(16), sender, receiver, "sender", payload=nested),
                          400, "invalid_request")
        envelope = self.signal(sender, receiver)
        prefix = json.dumps({key: value for key, value in envelope.items() if key != "payload"})[:-1] + ',"payload":'
        for payload in ('{"x":NaN}', '{"x":1e400}', '{"x":"\\ud800"}', '{"x":1,"x":2}',
                        '{"x":' + '[' * 1500 + '0' + ']' * 1500 + '}'):
            self.assert_error(self.api("POST", "/signals", token=token,
                                       raw_body=(prefix + payload + '}').encode("ascii")), 400, "invalid_request")
        self.assertEqual(self.server.application.opaque_bytes, 0)

    def test_mailbox_limits_pagination_and_tombstone_expiry(self):
        session, sender, receiver = self.device_pair()
        token = session["token"]
        app = self.server.application
        signals = [self.signal(sender, receiver) for _ in range(65)]
        for signal in signals:
            self.assertEqual(self.api("POST", "/signals", signal, token=token)[0], 201)
        path = "/devices/" + receiver + "/signals"
        first = self.api("GET", path, token=token)[1]["signals"]
        self.assertEqual([item["id"] for item in first], [item["id"] for item in signals[:64]])
        self.api("POST", path + "/ack", {"messageIds": [item["id"] for item in first]}, token=token)
        self.assertEqual([item["id"] for item in self.api("GET", path, token=token)[1]["signals"]],
                         [signals[-1]["id"]])
        for limit, value in (("MAX_MAILBOX_MESSAGES", 1), ("MAX_PENDING_SIGNALS", 1), ("MAX_SIGNAL_RECEIPTS", 65)):
            with mock.patch.object(account_server, limit, value):
                self.assert_error(self.api("POST", "/signals", self.signal(sender, receiver), token=token),
                                  429, "limit_exceeded")
                self.assertEqual(self.api("POST", "/signals", signals[-1], token=token)[0], 200)
        with app.signal_lock:
            for receipt in app.receipts.values():
                receipt["expiresAt"] = time.time() - 1
        self.server.service_actions()
        self.assertEqual((app.mailboxes, app.receipts), ({}, {}))

    def test_opaque_channel_sequence_retries_stale_updates_and_role_binding(self):
        session, sender, receiver = self.device_pair()
        token, identity = session["token"], secrets.token_hex(16)
        first = {"type": "accept", "route": "arbitrary", "sdp": ["opaque"]}
        self.assertIsNone(self.exchange(token, identity, sender, receiver, "sender", 2, first)[1]["peer"])
        reply = self.exchange(token, identity, receiver, sender, "receiver", 0, {"answer": 1})[1]
        self.assertEqual(reply["peer"], {"sequence": 2, "payload": first})
        self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", 2, first)[1]["peer"],
                         {"sequence": 0, "payload": {"answer": 1}})
        self.assert_error(self.exchange(token, identity, sender, receiver, "sender", 2, {"changed": 1}),
                          409, "channel_conflict")
        self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", 1, {"stale": 1})[0], 200)
        self.assertEqual(self.exchange(token, identity, receiver, sender, "receiver", 0, {"answer": 1})[1]["peer"],
                         {"sequence": 2, "payload": first})
        self.assert_error(self.exchange(token, identity, receiver, sender, "sender"), 409, "channel_conflict")
        third = secrets.token_hex(16)
        self.put_device(token, third)
        self.assert_error(self.exchange(token, identity, sender, third, "sender"), 409, "channel_conflict")
        self.assertEqual(set(self.server.application.channels[identity]),
                         {"accountId", "senderDeviceId", "receiverDeviceId", "sessions", "slots", "expiresAt"})

    def test_channel_session_permissions_limits_and_release(self):
        session, sender, receiver = self.device_pair()
        token, identity = session["token"], secrets.token_hex(16)
        self.exchange(token, identity, sender, receiver, "sender")
        second = self.api("POST", "/auth/login", {"username": "alice", "password": PASSWORD})[1]["token"]
        self.assert_error(self.exchange(second, identity, sender, receiver, "sender"), 403, "forbidden")
        self.assert_error(self.api("DELETE", "/channels/" + identity, token=second), 403, "forbidden")
        self.assertEqual(self.exchange(second, identity, receiver, sender, "receiver")[0], 200)
        other = self.register("other")
        self.assert_error(self.api("DELETE", "/channels/" + identity, token=other["token"]), 404, "not_found")
        for limit in ("MAX_CHANNELS", "MAX_ACCOUNT_CHANNELS"):
            with mock.patch.object(account_server, limit, 1):
                self.assert_error(self.exchange(token, secrets.token_hex(16), sender, receiver, "sender"),
                                  429, "limit_exceeded")
        for sequence in (-1, True, "1"):
            self.assert_error(self.exchange(token, identity, sender, receiver, "sender", sequence), 400, "invalid_request")
        self.assertEqual(self.api("DELETE", "/channels/" + identity, token=second), (200, {"ok": True}))
        self.assertEqual(self.server.application.channels, {})

    def test_channel_payload_bounds_and_no_business_lifecycle(self):
        session, sender, receiver = self.device_pair()
        token, identity = session["token"], secrets.token_hex(16)
        payload = {"x": "x" * (account_server.MAX_EXCHANGE_PAYLOAD - 8)}
        self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", 0, payload)[0], 200)
        payload["x"] += "x"
        self.assert_error(self.exchange(token, identity, sender, receiver, "sender", 1, payload), 413, "invalid_request")
        # Arbitrary signals do not create, accept, cancel or delete channels.
        before = copy.deepcopy(self.server.application.channels)
        for value in ("offer", "accept", "reject", "cancel", "complete", "unknown"):
            signal = self.signal(sender, receiver, {"type": value, "taskId": identity})
            self.assertEqual(self.api("POST", "/signals", signal, token=token)[0], 201)
            self.assertEqual(self.server.application.channels, before)
        with self.server.application.signal_lock:
            self.server.application.channels[identity]["expiresAt"] = time.time() - 1
        self.server.service_actions()
        self.assertEqual(self.server.application.channels, {})

    def test_channel_role_release_retains_peer_snapshots_and_is_idempotent(self):
        session, sender, receiver = self.device_pair()
        token, identity, app = session["token"], secrets.token_hex(16), self.server.application
        payload = {"opaque": "sender snapshot"}
        self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", payload=payload)[0], 200)
        bytes_before = app.opaque_bytes
        for _ in range(2):
            self.assertEqual(self.release(token, identity, sender, receiver, "sender"), (200, {"ok": True}))
        self.assertEqual(app.opaque_bytes, bytes_before)
        self.assertEqual(set(app.channels[identity]["sessions"]), {"sender"})
        self.server.service_actions()
        self.assertIn(identity, app.channels)
        # The late peer must see the original snapshot, including when the
        # first role finished before this role ever joined.
        self.assertEqual(self.exchange(token, identity, receiver, sender, "receiver")[1]["peer"],
                         {"sequence": 0, "payload": payload})
        self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", payload=payload)[0], 200)
        self.assertEqual(self.release(token, identity, receiver, sender, "receiver"), (200, {"ok": True}))
        self.assertNotIn(identity, app.channels)
        self.assertEqual(app.opaque_bytes, 0)
        # No release tombstone or new channel is needed once both roles left.
        self.assert_error(self.release(token, identity, receiver, sender, "receiver"), 404, "not_found")
        self.assertEqual(app.channels, {})

    def test_channel_role_release_requires_existing_pair_role_and_session(self):
        session, sender, receiver = self.device_pair()
        token, identity, app = session["token"], secrets.token_hex(16), self.server.application
        self.assert_error(self.release(token, identity, sender, receiver, "sender"), 404, "not_found")
        self.assertEqual(app.channels, {})
        self.exchange(token, identity, sender, receiver, "sender", payload={"opaque": 1})
        other_session = self.api("POST", "/auth/login", {"username": "alice", "password": PASSWORD})[1]["token"]
        other_account = self.register("other")
        third = secrets.token_hex(16)
        self.put_device(token, third)
        before = copy.deepcopy(app.channels)
        for value, device, peer, role, status, code in (
                (other_account["token"], sender, receiver, "sender", 404, "not_found"),
                (other_session, sender, receiver, "sender", 403, "forbidden"),
                (token, receiver, sender, "receiver", 403, "forbidden"),
                (token, sender, receiver, "receiver", 409, "channel_conflict"),
                (token, sender, third, "sender", 409, "channel_conflict"),
                (token, sender, receiver, "unknown", 400, "invalid_request")):
            self.assert_error(self.release(value, identity, device, peer, role), status, code)
        self.assertEqual(app.channels, before)
        self.exchange(other_session, identity, receiver, sender, "receiver")
        self.assert_error(self.release(token, identity, receiver, sender, "receiver"), 403, "forbidden")
        self.assertEqual(self.release(token, identity, sender, receiver, "sender")[0], 200)
        self.assertEqual(self.release(other_session, identity, receiver, sender, "receiver")[0], 200)
        self.assertEqual((app.channels, app.opaque_bytes), ({}, 0))

    def test_direct_channel_role_release_reuses_account_capacity(self):
        session, sender, receiver = self.device_pair()
        token, app = session["token"], self.server.application
        # Exceed the real account limit without waiting for the 30-minute TTL.
        for _ in range(account_server.MAX_ACCOUNT_CHANNELS + 3):
            identity = secrets.token_hex(16)
            self.assertEqual(self.exchange(token, identity, sender, receiver, "sender", payload={"route": "direct"})[0], 200)
            self.assertEqual(self.exchange(token, identity, receiver, sender, "receiver", payload={"route": "direct"})[0], 200)
            self.assertEqual(self.release(token, identity, sender, receiver, "sender")[0], 200)
            self.assertEqual(self.release(token, identity, receiver, sender, "receiver")[0], 200)
            self.assertEqual((app.channels, app.opaque_bytes), ({}, 0))

    def test_logout_and_device_revocation_clean_ephemeral_resources(self):
        session, sender, receiver = self.device_pair()
        token = session["token"]
        self.api("POST", "/signals", self.signal(sender, receiver), token=token)
        self.exchange(token, secrets.token_hex(16), sender, receiver, "sender")
        self.assertEqual(self.api("DELETE", "/devices/" + receiver, token=token)[0], 200)
        app = self.server.application
        self.assertEqual((app.channels, app.mailboxes, app.receipts), ({}, {}, {}))
        self.assertEqual(app.opaque_bytes, 0)
        self.put_device(token, receiver)
        self.api("POST", "/signals", self.signal(sender, receiver), token=token)
        self.exchange(token, secrets.token_hex(16), sender, receiver, "sender")
        self.assertEqual(self.api("POST", "/auth/logout", {}, token=token)[0], 200)
        self.assertEqual((app.channels, app.mailboxes, app.receipts), ({}, {}, {}))
        self.assertEqual(app.opaque_bytes, 0)

    def test_schema_v4_drops_transfers_preserves_identity_and_forgets_transport(self):
        session, sender, receiver = self.device_pair()
        token = session["token"]
        self.api("POST", "/signals", self.signal(sender, receiver), token=token)
        self.exchange(token, secrets.token_hex(16), sender, receiver, "sender")
        self.stop_server()
        with closing(account_server.sqlite3.connect(self.database)) as db:
            db.executescript("""
                CREATE TABLE transfers (
                    id TEXT PRIMARY KEY,
                    account_id TEXT NOT NULL REFERENCES accounts(id),
                    sender_device TEXT NOT NULL REFERENCES devices(id),
                    receiver_device TEXT NOT NULL REFERENCES devices(id),
                    sender_name TEXT NOT NULL, receiver_name TEXT NOT NULL,
                    kind TEXT NOT NULL, text TEXT NOT NULL,
                    file_name TEXT NOT NULL, file_size INTEGER NOT NULL,
                    status TEXT NOT NULL, created_at REAL NOT NULL,
                    expires_at REAL NOT NULL, sender_session TEXT NOT NULL,
                    receiver_session TEXT NOT NULL DEFAULT '',
                    payload_mode TEXT NOT NULL DEFAULT 'inline');
                CREATE INDEX transfers_account ON transfers(account_id, created_at);
                CREATE INDEX transfers_sender ON transfers(sender_device, status);
                CREATE INDEX transfers_receiver ON transfers(receiver_device, status);
                PRAGMA user_version=3;
            """)
            with db:
                db.execute("INSERT INTO transfers VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                           (secrets.token_hex(16), session["account"]["id"], sender, receiver,
                            "sender", "receiver", "file", "", "discard-history.txt", 12,
                            "accepted", time.time(), time.time() + 1800,
                            account_server.hashlib.sha256(token.encode("ascii")).hexdigest(), "", "stream"))
        self.start_server()
        self.assertEqual(self.api("GET", "/auth/me", token=token)[1]["account"], session["account"])
        self.assertEqual(len(self.api("GET", "/devices", token=token)[1]["devices"]), 2)
        self.assertEqual(self.api("GET", "/devices/" + receiver + "/signals", token=token)[1], {"signals": []})
        self.assertEqual(self.server.application.channels, {})
        with self.server.store.connect() as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 4)
            self.assertEqual({row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")},
                             {"accounts", "sessions", "devices"})
        for path in ("/transfers", "/devices/" + receiver + "/transfers",
                     "/transfers/" + "a" * 32 + "/complete", "/connections/" + "a" * 32 + "/webrtc"):
            self.assert_error(self.api("GET", path, token=token), 404, "not_found")

    def test_transport_configuration_is_read_only_and_validated(self):
        for value in (-1, True, 1.5, "1024", 8 * 1024 * 1024 * 1024 + 2):
            with self.assertRaises(ValueError):
                account_server.AccountApplication(self.database, direct_min_bytes=value)
        self.restart_server(direct_min_bytes=1024, stun_urls=["stun:example.com:443"])
        token = self.register()["token"]
        self.assertEqual(self.api("GET", "/transport-config", token=token),
                         (200, {"directMinBytes": 1024, "iceServers": [{"urls": ["stun:example.com:443"]}]}))
        self.assert_error(self.api("GET", "/transport-config"), 401, "unauthorized")

    def test_registration_login_logout_and_independent_sessions(self):
        initial = self.register("Alice")
        status, current = self.api("GET", "/auth/me", token=initial["token"])
        self.assertEqual(status, 200)
        self.assertEqual(current["account"], initial["account"])

        status, login = self.api("POST", "/auth/login", {
            "username": "ALICE", "password": PASSWORD,
        })
        self.assertEqual(status, 200)
        self.assertEqual(login["account"]["id"], initial["account"]["id"])
        self.assertNotEqual(login["token"], initial["token"])

        status, result = self.api("POST", "/auth/logout", {}, token=initial["token"])
        self.assertEqual((status, result), (200, {"ok": True}))
        self.assert_error(self.api("GET", "/auth/me", token=initial["token"]),
                          401, "unauthorized")
        self.assertEqual(self.api("GET", "/auth/me", token=login["token"])[0], 200)

    def test_logout_revokes_device_and_session_together(self):
        session, sender, receiver = self.device_pair()
        other_session = self.api("POST", "/auth/login", {
            "username": "alice", "password": PASSWORD})[1]
        # Revoke resources bound by another session while leaving that session usable.
        identity = secrets.token_hex(16)
        self.exchange(other_session["token"], identity, sender, receiver, "sender")
        self.api("POST", "/signals", self.signal(sender, receiver), token=other_session["token"])
        self.assertEqual(self.api("POST", "/auth/logout", {"deviceId": sender},
                                  token=session["token"]), (200, {"ok": True}))
        self.assert_error(self.api("GET", "/auth/me", token=session["token"]),
                          401, "unauthorized")
        listing = self.api("GET", "/devices", token=other_session["token"])[1]["devices"]
        self.assertEqual([device["id"] for device in listing], [receiver])
        self.assertEqual(self.server.application.channels, {})
        self.assertEqual(self.api("GET", "/devices/" + receiver + "/signals",
                                  token=other_session["token"])[1], {"signals": []})

    def test_logout_device_validation_does_not_revoke_other_accounts(self):
        alice = self.register()
        bob = self.register("bob")
        device = secrets.token_hex(16)
        self.put_device(alice["token"], device)
        self.assert_error(self.api("POST", "/auth/logout", {"deviceId": device},
                                   token=bob["token"]), 404, "not_found")
        self.assertEqual(self.api("GET", "/auth/me", token=bob["token"])[0], 200)
        self.assertEqual(len(self.api("GET", "/devices", token=alice["token"])[1]["devices"]), 1)
        for payload in ({"deviceId": None}, {"deviceId": "invalid"}, {"extra": True}):
            self.assert_error(self.api("POST", "/auth/logout", payload, token=bob["token"]),
                              400, "invalid_request")
        # Registration can be interrupted before the server creates a device.
        self.assertEqual(self.api("POST", "/auth/logout", {"deviceId": secrets.token_hex(16)},
                                  token=bob["token"])[0], 200)
        self.assert_error(self.api("GET", "/auth/me", token=bob["token"]), 401, "unauthorized")

    def test_logout_rejects_device_mutations_authenticated_before_revocation(self):
        observer = self.register()
        store = self.server.application.store
        for method in ("PUT", "POST", "DELETE"):
            with self.subTest(method=method):
                session = self.api("POST", "/auth/login", {
                    "username": "alice", "password": PASSWORD})[1]
                device = secrets.token_hex(16)
                self.put_device(session["token"], device)
                path = "/devices/" + device + ("/heartbeat" if method == "POST" else "")
                payload = self.device() if method == "PUT" else {} if method == "POST" else _UNSET
                authenticated, release = threading.Event(), threading.Event()
                original = store.authenticate

                def pause_first_authentication(token):
                    account = original(token)
                    if not authenticated.is_set():
                        authenticated.set()
                        if not release.wait(3):
                            raise AssertionError("test did not release device request")
                    return account

                with mock.patch.object(store, "authenticate", pause_first_authentication):
                    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                        pending = executor.submit(self.api, method, path, payload, token=session["token"])
                        try:
                            self.assertTrue(authenticated.wait(2))
                            self.assertEqual(self.api("POST", "/auth/logout", {"deviceId": device},
                                                      token=session["token"])[0], 200)
                        finally:
                            release.set()
                        self.assert_error(pending.result(timeout=3), 401, "unauthorized")
                self.assertEqual(self.api("GET", "/devices", token=observer["token"]),
                                 (200, {"devices": []}))

    def test_credentials_and_devices_survive_restart_without_plaintext_secrets(self):
        initial = self.register()
        identity = secrets.token_hex(16)
        registered = self.put_device(initial["token"], identity)
        self.restart_server()

        self.assertEqual(self.api("GET", "/auth/me", token=initial["token"])[0], 200)
        status, listing = self.api("GET", "/devices", token=initial["token"])
        self.assertEqual(status, 200)
        self.assertEqual(listing["devices"][0]["id"], registered["id"])
        status, login = self.api("POST", "/auth/login", {
            "username": "alice", "password": PASSWORD,
        })
        self.assertEqual(status, 200)
        self.assertEqual(login["account"]["id"], initial["account"]["id"])
        for path in self.database.parent.glob(self.database.name + "*"):
            if path.is_file():
                stored = path.read_bytes()
                self.assertNotIn(PASSWORD.encode(), stored)
                self.assertNotIn(initial["token"].encode(), stored)
                self.assertNotIn(login["token"].encode(), stored)

    def test_bad_login_is_uniform_and_username_uniqueness_is_case_insensitive(self):
        self.register("Alice")
        known = self.assert_error(self.api("POST", "/auth/login", {
            "username": "alice", "password": "wrong password long enough",
        }), 401, "invalid_credentials")
        unknown = self.assert_error(self.api("POST", "/auth/login", {
            "username": "nobody", "password": "wrong password long enough",
        }), 401, "invalid_credentials")
        self.assertEqual(known, unknown)
        self.assert_error(self.api("POST", "/auth/register", {
            "username": "ALICE", "password": PASSWORD,
        }), 409, "username_taken")

    def test_expired_session_cannot_read_or_mutate_devices(self):
        self.restart_server(session_ttl=0.15)
        session = self.register()
        identity = secrets.token_hex(16)
        self.put_device(session["token"], identity)
        time.sleep(0.18)
        for method, path, payload in (
                ("GET", "/auth/me", _UNSET),
                ("GET", "/devices", _UNSET),
                ("POST", "/devices/" + identity + "/heartbeat", {}),
                ("DELETE", "/devices/" + identity, _UNSET)):
            with self.subTest(method=method, path=path):
                self.assert_error(self.api(method, path, payload, token=session["token"]),
                                  401, "unauthorized")

    def test_device_directory_update_presence_and_revocation(self):
        self.restart_server(online_timeout=0.1)
        session = self.register()
        token = session["token"]
        identity = secrets.token_hex(16)
        original = self.device("工作电脑")
        registered = self.put_device(token, identity, original)
        self.assertEqual(registered["publicKey"], original["publicKey"])
        self.assertEqual(registered["displayName"], "工作电脑")
        self.assertTrue(registered["online"])

        time.sleep(0.13)
        status, listing = self.api("GET", "/devices", token=token)
        self.assertEqual(status, 200)
        self.assertFalse(listing["devices"][0]["online"])
        status, renewed = self.api(
            "POST", "/devices/" + identity + "/heartbeat", {}, token=token)
        self.assertEqual(status, 200)
        self.assertTrue(renewed["device"]["online"])
        self.assertGreater(renewed["device"]["lastSeenAt"], registered["lastSeenAt"])

        updated = self.device("Renamed laptop")
        self.put_device(token, identity, updated)
        status, listing = self.api("GET", "/devices", token=token)
        self.assertEqual(status, 200)
        self.assertEqual(len(listing["devices"]), 1)
        self.assertEqual(listing["devices"][0]["publicKey"], updated["publicKey"])
        status, revoked = self.api("DELETE", "/devices/" + identity, token=token)
        self.assertEqual((status, revoked), (200, {"ok": True}))
        self.assertEqual(self.api("GET", "/devices", token=token)[1], {"devices": []})
        self.assert_error(self.api("POST", "/devices/" + identity + "/heartbeat", {},
                                   token=token), 404, "not_found")
        self.put_device(token, identity, original)

    def test_other_account_cannot_discover_mutate_or_claim_device(self):
        alice = self.register("alice")
        bob = self.register("bob")
        identity = secrets.token_hex(16)
        self.put_device(alice["token"], identity)
        status, listing = self.api("GET", "/devices", token=bob["token"])
        self.assertEqual((status, listing), (200, {"devices": []}))
        self.assert_error(self.api("PUT", "/devices/" + identity, self.device(),
                                   token=bob["token"]), 409, "device_conflict")
        self.assert_error(self.api("POST", "/devices/" + identity + "/heartbeat", {},
                                   token=bob["token"]), 404, "not_found")
        self.assert_error(self.api("DELETE", "/devices/" + identity,
                                   token=bob["token"]), 404, "not_found")
        self.api("DELETE", "/devices/" + identity, token=alice["token"])
        self.assert_error(self.api("PUT", "/devices/" + identity, self.device(),
                                   token=bob["token"]), 409, "device_conflict")

    def test_invalid_credentials_and_device_fields_are_rejected(self):
        for credentials in (
                {"username": "x' OR '1'='1", "password": PASSWORD},
                {"username": "alice", "password": "too short"},
                {"username": "alice", "password": "x" * 257},
                {"username": "alice", "password": PASSWORD, "administrator": True}):
            with self.subTest(credentials=list(credentials)):
                self.assert_error(self.api("POST", "/auth/register", credentials),
                                  400, "invalid_request")
        session = self.register()
        identity = secrets.token_hex(16)
        for updates in (
                {"displayName": ""}, {"platform": ""},
                {"publicKey": "not base64"},
                {"publicKey": base64.b64encode(b"short").decode()},
                {"ownerId": "another-account"}):
            value = self.device()
            value.update(updates)
            with self.subTest(fields=list(updates)):
                self.assert_error(self.api("PUT", "/devices/" + identity, value,
                                           token=session["token"]), 400, "invalid_request")

    def test_http_json_limits_and_untrusted_authentication(self):
        self.assertEqual(self.api("GET", "/healthz"), (200, {"status": "ok"}))
        for authorization in ("", "Basic abc", "Bearer short", "Bearer " + "0" * 64):
            with self.subTest(authorization=authorization[:20]):
                self.assert_error(self.api("GET", "/auth/me", headers={
                    "Authorization": authorization,
                }), 401, "unauthorized")
        for body in (b"not-json", b"[]", b'{"username":"\xff"}',
                     b'{"username":"alice","username":"bob","password":"long password value"}'):
            with self.subTest(body=body[:20]):
                self.assert_error(self.api("POST", "/auth/register", raw_body=body),
                                  400, "invalid_request")
        self.assert_error(self.api("POST", "/auth/register", raw_body=b"{}",
                                   headers={"Content-Length": str(account_server.MAX_BODY + 1)}),
                          413, "invalid_request")

    def test_login_rate_limit_cannot_be_bypassed_with_forwarded_headers(self):
        self.register()
        self.restart_server(rate_limit=2)
        for index in range(3):
            response = self.api("POST", "/auth/login", {
                "username": "alice", "password": "incorrect but long password",
            }, headers={"X-Forwarded-For": "192.0.2." + str(index + 1)})
            self.assert_error(response, 401 if index < 2 else 429,
                              "invalid_credentials" if index < 2 else "rate_limited")

    def test_registration_rate_limit_bounds_account_creation(self):
        self.restart_server(registration_limit=2)
        self.register("alice")
        self.register("bob")
        self.assert_error(self.api("POST", "/auth/register", {
            "username": "carol", "password": PASSWORD,
        }), 429, "rate_limited")

    def test_device_limit_preserves_existing_devices_and_allows_updates(self):
        session = self.register()
        identities = [secrets.token_hex(16) for _ in range(32)]
        for identity in identities:
            self.put_device(session["token"], identity)
        self.assert_error(self.api("PUT", "/devices/" + secrets.token_hex(16),
                                   self.device(), token=session["token"]),
                          429, "limit_exceeded")
        self.put_device(session["token"], identities[0], self.device("Renamed"))
        status, listing = self.api("GET", "/devices", token=session["token"])
        self.assertEqual(status, 200)
        self.assertEqual({device["id"] for device in listing["devices"]}, set(identities))


if __name__ == "__main__":
    unittest.main(verbosity=2)
