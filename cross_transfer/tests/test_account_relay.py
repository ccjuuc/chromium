#!/usr/bin/env python3
"""Exercise account APIs and relay frames in one verified HTTPS listener."""
import http.client
import base64
import concurrent.futures
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import socket
import ssl
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
relay = load("combined_relay", ROOT / "relay/server.py")
fixtures = load("combined_fixtures", ROOT / "tests/test_https_relay.py")

class CombinedHttpsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="cross-transfer-combined-")
        cls.directory = Path(cls.temp.name)
        fixtures.make_certificates(cls.directory)
        cls.context = ssl.create_default_context(cafile=str(cls.directory / "ca.pem"))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.database_directory = self.directory / secrets.token_hex(8)
        self.database_directory.mkdir(mode=0o700)
        self.server = relay.create_server(
            "127.0.0.1", 0, str(self.directory / "server.pem"),
            str(self.directory / "server.key"),
            {"static": {"sender_token": "1" * 64, "receiver_token": "2" * 64}},
            account_database=self.database_directory / "accounts.sqlite3",
            stun_urls=["stun:115.29.16.37:443"])
        self.thread = threading.Thread(target=self.server.serve_forever,
                                      kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPSConnection(
            *self.server.server_address, context=self.context, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            connection.close()

    def api(self, method, path, payload=None, token=None):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        status, body, _ = self.request(method, "/api/v1" + path,
                                       json.dumps(payload) if payload is not None else None, headers)
        return status, json.loads(body)

    def device_pair(self):
        status, session = self.api("POST", "/auth/register", {
            "username": "file_user", "password": "Combined file test password!"})
        self.assertEqual(status, 201)
        token = session["token"]
        sender, receiver = secrets.token_hex(16), secrets.token_hex(16)
        for device in (sender, receiver):
            self.assertEqual(self.api("PUT", "/devices/" + device, {
                "displayName": device[:6], "platform": "windows",
                "publicKey": base64.b64encode(secrets.token_bytes(32)).decode("ascii")}, token)[0], 200)
        return token, sender, receiver

    def channel(self, token, identity, device, peer, role, action="exchange", **fields):
        body = {"deviceId": device, "peerDeviceId": peer, "role": role}
        if action == "exchange":
            body.update(sequence=0, payload={})
        body.update(fields)
        return self.api("POST", "/channels/" + identity + "/" + action, body, token)

    def room_request(self, info, role, method, operation, body=None, token=None):
        return self.request(method, "/v1/rooms/" + info["roomId"] + "/" +
                            info["connectionId"] + "/" + role + "/" + operation, body,
                            {"Authorization": "Bearer " + (token or info["bearerToken"]),
                             "X-Relay-Lease": ("b" if role == "sender" else "c") * 32,
                             "Content-Length": str(len(body) if body else 0)})

    def test_exchange_and_lazy_relay_do_not_require_mailbox_messages(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        payload = {"sdp": "opaque", "route": "direct", "applicationStatus": "unknown"}
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", payload=payload)[0], 200)
        self.assertNotIn("own_" + identity, self.server.state.rooms)
        receiving_slot = self.channel(token, identity, receiver, sender, "receiver")[1]
        self.assertEqual(receiving_slot["peer"], {"sequence": 0, "payload": payload})
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        receiving = self.channel(token, identity, receiver, sender, "receiver", "relay")[1]["relay"]
        self.assertEqual(sending["roomId"], "own_" + identity)
        self.assertEqual(sending["connectionId"], identity)
        self.assertNotEqual(sending["bearerToken"], receiving["bearerToken"])
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"], sending)
        # Obtaining relay resources never rewrites the opaque exchange slot.
        self.assertEqual(self.channel(token, identity, receiver, sender, "receiver")[1]["peer"],
                         {"sequence": 0, "payload": payload})
        self.assertEqual(self.server.account_application.mailboxes, {})
        self.assertEqual(self.room_request(sending, "sender", "POST", "open", token=token)[0], 401)
        self.assertEqual(self.room_request(sending, "receiver", "POST", "open")[0], 401)
        wrong = dict(sending, connectionId="a" * 32)
        self.assertEqual(self.room_request(wrong, "sender", "POST", "open")[0], 409)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 200)
        frame = bytes(38)
        self.assertEqual(self.room_request(sending, "sender", "POST", "frames", frame)[0], 200)
        self.assertEqual(self.room_request(receiving, "receiver", "GET", "frames")[:2], (200, frame))

    def test_relay_can_create_resource_before_any_exchange(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        receiving = self.channel(token, identity, receiver, sender, "receiver", "relay")
        self.assertEqual(receiving[0], 200)
        self.assertEqual(self.server.account_application.channels[identity]["slots"], {})
        self.assertEqual(self.channel(token, identity, sender, receiver, "receiver", "relay")[0], 409)
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")
        self.assertEqual(sending[0], 200)
        self.assertNotEqual(sending[1]["relay"]["bearerToken"], receiving[1]["relay"]["bearerToken"])

    def test_closed_relay_releases_channel_capacity_and_payload_but_keeps_direct_exchange(self):
        token, sender, receiver = self.device_pair()
        app = self.server.account_application
        direct = secrets.token_hex(16)
        direct_payload = {"sdp": "opaque direct exchange"}
        self.assertEqual(self.channel(token, direct, sender, receiver, "sender", payload=direct_payload)[0], 200)
        direct_bytes = len(json.dumps(direct_payload, separators=(",", ":")).encode("utf-8"))
        # One ongoing direct exchange leaves capacity for just one relay at a
        # time. Repeated completed transports must not consume that capacity.
        with mock.patch.dict(app.channel_for.__func__.__globals__, MAX_ACCOUNT_CHANNELS=2):
            for _ in range(3):
                identity = secrets.token_hex(16)
                self.assertEqual(self.channel(token, identity, sender, receiver, "sender", payload={"offer": "opaque"})[0], 200)
                self.assertEqual(self.channel(token, identity, receiver, sender, "receiver", payload={"answer": "opaque"})[0], 200)
                sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
                receiving = self.channel(token, identity, receiver, sender, "receiver", "relay")[1]["relay"]
                self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
                self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 200)
                frame = bytes(38)
                self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", frame)[0], 200)
                self.assertEqual(self.room_request(receiving, "receiver", "DELETE", "open")[0], 200)
                self.server.service_actions()
                # Closing one role preserves the remaining reader's data.
                self.assertIn(identity, app.channels)
                self.assertEqual(self.room_request(sending, "sender", "GET", "frames")[:2], (200, frame))
                self.assertEqual(self.room_request(sending, "sender", "DELETE", "open")[0], 200)
                self.server.service_actions()
                self.assertNotIn(sending["roomId"], self.server.state.rooms)
                self.assertEqual(set(app.channels), {direct})
                self.assertEqual(app.opaque_bytes, direct_bytes)
                self.assertEqual(self.server.state.total_bytes, 0)
            identity = secrets.token_hex(16)
            self.assertEqual(self.channel(token, identity, sender, receiver, "sender", payload={"offer": "opaque"})[0], 200)
            sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
            self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
            with self.server.state.condition:
                self.server.state.rooms[sending["roomId"]].last_activity = time.monotonic() - self.server.state.idle_timeout - 1
            self.server.service_actions()
            self.assertEqual(set(app.channels), {direct})
            self.assertEqual(app.opaque_bytes, direct_bytes)
        reply = self.channel(token, direct, receiver, sender, "receiver")[1]["peer"]
        self.assertEqual(reply, {"sequence": 0, "payload": direct_payload})

    def test_release_drains_last_frames_without_recording_a_result(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        receiving = self.channel(token, identity, receiver, sender, "receiver", "relay")[1]["relay"]
        self.room_request(sending, "sender", "POST", "open")
        self.room_request(receiving, "receiver", "POST", "open")
        ack = bytes(38)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", ack)[0], 200)
        self.assertEqual(self.api("DELETE", "/channels/" + identity, token=token), (200, {"ok": True}))
        self.assertNotIn(identity, self.server.account_application.channels)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 410)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", ack)[0], 410)
        self.assertEqual(self.room_request(sending, "sender", "GET", "frames")[:2], (200, ack))
        self.assertLessEqual(self.server.state.rooms[sending["roomId"]].expires_at, time.time() + 15)
        with self.server.account_application.store.connect() as db:
            self.assertEqual({row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")},
                             {"accounts", "sessions", "devices"})

    def test_both_role_releases_drain_relay_without_cutting_off_peer(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        app = self.server.account_application
        self.channel(token, identity, sender, receiver, "sender", payload={"opaque": "exchange"})
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        receiving = self.channel(token, identity, receiver, sender, "receiver", "relay")[1]["relay"]
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 200)
        self.assertEqual(self.channel(token, identity, receiver, sender, "receiver", "release"), (200, {"ok": True}))
        # A release is a local resource hint, not permission to interrupt the
        # other role. Pending transport and signaling requests can still drain.
        self.assertFalse(self.server.state.rooms[sending["roomId"]].finishing)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", payload={"opaque": "exchange"})[0], 200)
        frame = bytes(38)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", frame)[0], 200)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", "release"), (200, {"ok": True}))
        self.assertNotIn(identity, app.channels)
        self.assertEqual(app.opaque_bytes, 0)
        self.assertTrue(self.server.state.rooms[sending["roomId"]].finishing)
        self.assertEqual(self.room_request(sending, "sender", "GET", "frames")[:2], (200, frame))
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", frame)[0], 410)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", "release")[0], 404)
        self.assertEqual(app.channels, {})

    def test_revoke_and_logout_invalidate_bound_transport_resources(self):
        token, sender, receiver = self.device_pair()
        for operation in ("revoke", "logout"):
            identity = secrets.token_hex(16)
            sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
            self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
            if operation == "revoke":
                self.assertEqual(self.api("DELETE", "/devices/" + receiver, token=token)[0], 200)
                self.api("PUT", "/devices/" + receiver, {
                    "displayName": "returned", "platform": "windows",
                    "publicKey": base64.b64encode(secrets.token_bytes(32)).decode("ascii")}, token)
            else:
                self.assertEqual(self.api("POST", "/auth/logout", {}, token)[0], 200)
            self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)
            self.assertNotIn(identity, self.server.account_application.channels)

    def test_late_exchange_cannot_retrieve_released_room_credentials(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        previous = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        self.room_request(previous, "sender", "POST", "open")
        self.assertEqual(self.api("DELETE", "/channels/" + identity, token=token)[0], 200)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender")[0], 200)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", "relay")[0], 429)
        with self.server.state.condition:
            self.server.state.rooms[previous["roomId"]].expires_at = time.time() - 1
        renewed = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        self.assertNotEqual(previous["bearerToken"], renewed["bearerToken"])
        self.assertEqual(self.room_request(previous, "sender", "POST", "open")[0], 401)
        self.assertEqual(self.room_request(renewed, "sender", "POST", "open")[0], 200)

    def test_opaque_signals_never_change_existing_relay_resources(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        transport = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        app = self.server.account_application
        before = copy.deepcopy(app.channels)
        for kind in ("offer", "accept", "reject", "cancel", "complete", "anything-else"):
            message = {"id": secrets.token_hex(16), "fromDeviceId": sender,
                       "toDeviceId": receiver, "payload": {"type": kind, "taskId": identity}}
            self.assertEqual(self.api("POST", "/signals", message, token)[1]["signal"]["payload"], message["payload"])
            self.assertEqual(app.channels, before)
            self.assertEqual(self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"], transport)

    def test_logout_device_cleans_channels_bound_to_another_session(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        receiving = self.channel(token, identity, receiver, sender, "receiver", "relay")[1]["relay"]
        other = self.api("POST", "/auth/login", {
            "username": "file_user", "password": "Combined file test password!"})[1]["token"]
        self.assertEqual(self.api("POST", "/auth/logout", {"deviceId": receiver}, other)[0], 200)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 401)
        self.assertEqual(self.server.account_application.channels, {})

    def test_channel_expiry_runs_from_shared_listener_maintenance(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        app = self.server.account_application
        with app.signal_lock:
            app.channels[identity]["expiresAt"] = time.time() - 1
        self.server.service_actions()
        self.assertNotIn(identity, app.channels)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)

    def test_expired_account_session_releases_mailbox_channel_and_relay(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        self.channel(token, identity, sender, receiver, "sender", payload={"data": "opaque"})
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
        self.api("POST", "/signals", {"id": secrets.token_hex(16), "fromDeviceId": sender,
                                      "toDeviceId": receiver, "payload": {"message": "opaque"}}, token)
        app = self.server.account_application
        with app.store.connect() as db:
            db.execute("UPDATE sessions SET expires_at=? WHERE token_hash=?",
                       (time.time() - 1, hashlib.sha256(token.encode("ascii")).hexdigest()))
        self.server.service_actions()
        self.assertEqual(self.api("GET", "/auth/me", token=token)[0], 401)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)
        self.assertEqual((app.mailboxes, app.receipts, app.channels, app.opaque_bytes), ({}, {}, {}, 0))

    def test_new_login_evicts_old_session_transport_before_returning(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        self.channel(token, identity, sender, receiver, "sender", payload={"data": "opaque"})
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
        self.api("POST", "/signals", {"id": secrets.token_hex(16), "fromDeviceId": sender,
                                      "toDeviceId": receiver, "payload": {"message": "opaque"}}, token)
        app = self.server.account_application
        with mock.patch.dict(app.store.new_session.__func__.__globals__, MAX_SESSIONS=1):
            status, session = self.api("POST", "/auth/login", {
                "username": "file_user", "password": "Combined file test password!"})
        self.assertEqual(status, 200)
        self.assertEqual(self.api("GET", "/auth/me", token=session["token"])[0], 200)
        self.assertEqual(self.api("GET", "/auth/me", token=token)[0], 401)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)
        self.assertEqual((app.mailboxes, app.receipts, app.channels, app.opaque_bytes), ({}, {}, {}, 0))

    def test_channel_and_relay_expiry_follow_both_participating_sessions(self):
        token, sender, receiver = self.device_pair()
        second = self.api("POST", "/auth/login", {
            "username": "file_user", "password": "Combined file test password!"})[1]["token"]
        first_expiry, second_expiry = time.time() + 60, time.time() + 20
        app = self.server.account_application
        with app.store.connect() as db:
            for value, expires in ((token, first_expiry), (second, second_expiry)):
                db.execute("UPDATE sessions SET expires_at=? WHERE token_hash=?",
                           (expires, hashlib.sha256(value.encode("ascii")).hexdigest()))
        identity = secrets.token_hex(16)
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        self.assertEqual(sending["expiresAt"], first_expiry)
        reply = self.channel(second, identity, receiver, sender, "receiver")
        self.assertEqual(reply[0], 200)
        self.assertEqual(reply[1]["expiresAt"], second_expiry)
        self.assertEqual(app.channels[identity]["expiresAt"], second_expiry)
        self.assertEqual(self.server.state.rooms[sending["roomId"]].expires_at, second_expiry)
        # Expiry is enforced by relay access itself even between maintenance ticks.
        with mock.patch.object(time, "time", return_value=second_expiry + 1):
            self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)
        self.assertEqual(self.api("GET", "/auth/me", token=token)[0], 200)

    def test_channel_sequence_race_has_one_winner(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        self.channel(token, identity, sender, receiver, "sender")
        barrier = threading.Barrier(2)
        def publish(value):
            barrier.wait(3)
            return self.channel(token, identity, sender, receiver, "sender", sequence=1, payload={"value": value})
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            first = executor.submit(publish, 1)
            second = executor.submit(publish, 2)
            self.assertEqual(sorted((first.result(timeout=5)[0], second.result(timeout=5)[0])), [200, 409])
        peer = self.channel(token, identity, receiver, sender, "receiver")[1]["peer"]
        self.assertEqual(peer["sequence"], 1)
        self.assertIn(peer["payload"], ({"value": 1}, {"value": 2}))

    def test_opaque_payload_limits_and_transport_configuration(self):
        token, sender, receiver = self.device_pair()
        configuration = self.api("GET", "/transport-config", token=token)[1]
        self.assertEqual(configuration, {"directMinBytes": 0, "iceServers": [{"urls": ["stun:115.29.16.37:443"]}]})
        # UTF-8 bounds are measured on the JSON value, independent of escaping.
        message = {"id": secrets.token_hex(16), "fromDeviceId": sender, "toDeviceId": receiver,
                   "payload": {"x": "中" * 5000}}
        self.assertEqual(self.api("POST", "/signals", message, token)[0], 201)
        message["id"] = secrets.token_hex(16)
        message["payload"]["x"] = "中" * 6000
        self.assertEqual(self.api("POST", "/signals", message, token)[0], 413)
        identity = secrets.token_hex(16)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender",
                                      payload={"x": "x" * (112 * 1024 - 8)})[0], 200)
        self.assertEqual(self.channel(token, identity, sender, receiver, "sender", sequence=1,
                                      payload={"x": "x" * (112 * 1024 - 7)})[0], 413)

    def test_restart_forgets_mailboxes_and_channels_preserving_accounts(self):
        token, sender, receiver = self.device_pair()
        identity = secrets.token_hex(16)
        sending = self.channel(token, identity, sender, receiver, "sender", "relay")[1]["relay"]
        self.api("POST", "/signals", {"id": secrets.token_hex(16), "fromDeviceId": sender,
                                      "toDeviceId": receiver, "payload": {"opaque": 1}}, token)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)
        self.server = relay.create_server(
            "127.0.0.1", 0, str(self.directory / "server.pem"), str(self.directory / "server.key"),
            {"static": {"sender_token": "1" * 64, "receiver_token": "2" * 64}},
            account_database=self.database_directory / "accounts.sqlite3")
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()
        self.assertEqual(self.api("GET", "/auth/me", token=token)[0], 200)
        self.assertEqual(len(self.api("GET", "/devices", token=token)[1]["devices"]), 2)
        self.assertEqual(self.api("GET", "/devices/" + receiver + "/signals", token=token)[1], {"signals": []})
        self.assertEqual(self.server.account_application.channels, {})
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)
        for path in ("/transfers", "/devices/" + receiver + "/transfers", "/connections/" + identity + "/webrtc"):
            self.assertEqual(self.api("GET", path, token=token)[0], 404)

    def test_stun_configuration_and_listener_share_server_lifecycle(self):
        for value in (-1, 65536, True, "443"):
            with self.assertRaises(ValueError):
                relay.create_server("127.0.0.1", 0, str(self.directory / "server.pem"),
                                    str(self.directory / "server.key"), {}, stun_port=value)
        app_type = type(self.server.account_application)
        for urls in (["turn:relay:443"], ["stun:host:0"], ["stun:host:65536"],
                     ["stun:user@host:443"], ["stun:host\r\n"], ["stun:[:::]:443"]):
            with self.assertRaises(ValueError):
                app_type(self.database_directory / "invalid.sqlite3", stun_urls=urls)
        # Bind a UDP port first so both actual listeners can use the same number.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server = relay.create_server("127.0.0.1", port, str(self.directory / "server.pem"),
                                    str(self.directory / "server.key"),
                                    {"static": {"sender_token": "1" * 64, "receiver_token": "2" * 64}},
                                    stun_port=port)
        listener = server.stun
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(2)
                request = bytes.fromhex("000100002112a442010203040506070809101112")
                client.sendto(request, listener.server_address)
                self.assertEqual(client.recvfrom(1200)[0][8:20], request[8:20])
        finally:
            server.server_close()
        self.assertFalse(listener.thread.is_alive())

    def test_relay_rejects_invalid_size_policy_configuration(self):
        for value in (-1, 8 * 1024 * 1024 * 1024 + 2, True, 1.5, "1024"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    relay.create_server("127.0.0.1", 0, str(self.directory / "server.pem"),
                                        str(self.directory / "server.key"),
                                        {"static": {"sender_token": "1" * 64, "receiver_token": "2" * 64}},
                                        direct_min_bytes=value)

    def test_dynamic_expiry_capacity_and_revocation_during_poll(self):
        state = self.server.state
        self.assertTrue(state.provision_dynamic("own_pending", "a" * 32, time.time() + 20))
        info = state.get_dynamic("own_pending", "receiver")
        self.assertEqual(self.room_request(info, "receiver", "POST", "open")[0], 200)
        result = []
        waiter = threading.Thread(target=lambda: result.append(
            state.operate(info["roomId"], info["connectionId"], "receiver", "c" * 32,
                          0, "GET", "frames", b"", "Bearer " + info["bearerToken"])))
        waiter.start()
        time.sleep(0.02)
        state.revoke_dynamic(info["roomId"])
        waiter.join(2)
        self.assertFalse(waiter.is_alive())
        self.assertIn(result[0][0], (401, 410))
        self.assertEqual(state.operate(info["roomId"], info["connectionId"], "receiver", "c" * 32,
                                       0, "GET", "frames", b"", "Bearer " + info["bearerToken"])[0], 401)
        for number in range(relay.MAX_DYNAMIC_ROOMS):
            self.assertTrue(state.provision_dynamic("own_" + str(number), "a" * 32, time.time() + 30))
        self.assertFalse(state.provision_dynamic("own_overflow", "a" * 32, time.time() + 30))
        with state.condition:
            state.rooms["own_0"].expires_at = time.time() - 1
        self.assertIsNone(state.get_dynamic("own_0", "sender"))
        self.assertTrue(state.provision_dynamic("own_new", "a" * 32, time.time() + 30))

    def test_accounts_and_static_frames_share_verified_listener(self):
        status, body, _ = self.request("GET", "/api/v1/healthz")
        self.assertEqual((status, json.loads(body)), (200, {"status": "ok"}))
        status, body, headers = self.request("POST", "/api/v1/auth/register",
            json.dumps({"username": "combined_user", "password": "Combined HTTPS test password!"}),
            {"Content-Type": "application/json"})
        self.assertEqual(status, 201)
        self.assertEqual(headers["Cache-Control"], "no-store")
        token = json.loads(body)["token"]
        status, body, _ = self.request("GET", "/api/v1/auth/me", headers={"Authorization": "Bearer " + token})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["account"]["username"], "combined_user")
        root = "/v1/rooms/static/" + "a" * 32
        def role_headers(role):
            return {"Authorization": "Bearer " + ("1" if role == "sender" else "2") * 64,
                    "X-Relay-Lease": ("b" if role == "sender" else "c") * 32,
                    "Content-Length": "0"}
        self.assertEqual(self.request("POST", root + "/sender/open", headers=role_headers("sender"))[0], 200)
        self.assertEqual(self.request("POST", root + "/receiver/open", headers=role_headers("receiver"))[0], 200)
        frame = bytes(38)
        headers = role_headers("sender")
        headers["Content-Length"] = str(len(frame))
        self.assertEqual(self.request("POST", root + "/sender/frames", frame, headers)[0], 200)
        status, received, headers = self.request("GET", root + "/receiver/frames", headers=role_headers("receiver"))
        self.assertEqual((status, received), (200, frame))
        self.assertEqual(headers["X-Relay-Sequence"], "1")

    def test_account_framing_and_forwarded_ip_cannot_bypass_limits(self):
        self.assertEqual(self.request("POST", "/api/v1/auth/register", headers={
            "Content-Length": str(relay.MAX_ACCOUNT_BODY + 1), "Content-Type": "application/json"})[0], 413)
        self.assertEqual(self.request("GET", "/api/v1/auth/me")[0], 401)
        for attempt in range(11):
            status, _, _ = self.request("POST", "/api/v1/auth/login",
                json.dumps({"username": "missing", "password": "Incorrect password test!"}),
                {"Content-Type": "application/json", "X-Forwarded-For": "192.0.2." + str(attempt + 1)})
            self.assertEqual(status, 401 if attempt < 10 else 429)
        self.assertEqual(self.request("POST", "/api/v1/auth/login", "{}", {
            "Content-Type": "text/plain"})[0], 400)

if __name__ == "__main__":
    unittest.main()
