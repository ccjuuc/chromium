#!/usr/bin/env python3
"""Exercise account APIs and relay frames in one verified HTTPS listener."""
import http.client
import base64
import importlib.util
import json
from pathlib import Path
import secrets
import ssl
import tempfile
import threading
import time
import unittest

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
            {"legacy": {"sender_token": "1" * 64, "receiver_token": "2" * 64}},
            account_database=self.database_directory / "accounts.sqlite3")
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

    def file_invitation(self):
        status, session = self.api("POST", "/auth/register", {
            "username": "file_user", "password": "Combined file test password!"})
        self.assertEqual(status, 201)
        token = session["token"]
        sender, receiver = secrets.token_hex(16), secrets.token_hex(16)
        for device in (sender, receiver):
            self.assertEqual(self.api("PUT", "/devices/" + device, {
                "displayName": device[:6], "platform": "windows",
                "publicKey": base64.b64encode(secrets.token_bytes(32)).decode("ascii")}, token)[0], 200)
        status, value = self.api("POST", "/transfers", {
            "senderDeviceId": sender, "receiverDeviceId": receiver, "kind": "file",
            "text": "", "fileName": "test.txt", "fileSize": 12}, token)
        self.assertEqual(status, 201)
        return token, sender, receiver, value["transfer"]["id"]

    def transfer_action(self, token, identity, device, action):
        return self.api("POST", "/transfers/" + identity + "/" + action, {"deviceId": device}, token)

    def room_request(self, info, role, method, operation, body=None, token=None):
        return self.request(method, "/v1/rooms/" + info["roomId"] + "/" +
                            info["connectionId"] + "/" + role + "/" + operation, body,
                            {"Authorization": "Bearer " + (token or info["bearerToken"]),
                             "X-Relay-Lease": ("b" if role == "sender" else "c") * 32,
                             "Content-Length": str(len(body) if body else 0)})

    def test_accepted_file_uses_distinct_scoped_dynamic_credentials(self):
        token, sender, receiver, identity = self.file_invitation()
        self.assertEqual(self.transfer_action(token, identity, sender, "relay")[0], 409)
        self.assertEqual(self.transfer_action(token, identity, sender, "accept")[0], 403)
        status, accepted = self.transfer_action(token, identity, receiver, "accept")
        self.assertEqual(status, 200)
        self.assertEqual(accepted["transfer"]["status"], "accepted")
        self.assertEqual(self.transfer_action(token, identity, receiver, "accept"), (200, accepted))
        sending = self.transfer_action(token, identity, sender, "relay")[1]["relay"]
        receiving = self.transfer_action(token, identity, receiver, "relay")[1]["relay"]
        self.assertEqual(sending["roomId"], receiving["roomId"])
        self.assertEqual(sending["connectionId"], identity)
        self.assertNotEqual(sending["bearerToken"], receiving["bearerToken"])
        self.assertEqual(self.transfer_action(token, identity, sender, "relay")[1]["relay"], sending)
        listing = self.api("GET", "/devices/" + receiver + "/transfers", token=token)[1]
        self.assertNotIn(sending["bearerToken"], json.dumps(listing))
        self.assertEqual(self.room_request(sending, "sender", "POST", "open", token=token)[0], 401)
        self.assertEqual(self.room_request(sending, "receiver", "POST", "open")[0], 401)
        wrong_connection = dict(sending, connectionId="a" * 32)
        self.assertEqual(self.room_request(wrong_connection, "sender", "POST", "open")[0], 409)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 200)
        frame = bytes(38)
        self.assertEqual(self.room_request(sending, "sender", "POST", "frames", frame)[0], 200)
        status, body, _ = self.room_request(receiving, "receiver", "GET", "frames")
        self.assertEqual((status, body), (200, frame))

    def test_completion_drains_final_ack_but_prevents_new_data_or_leases(self):
        token, sender, receiver, identity = self.file_invitation()
        self.transfer_action(token, identity, receiver, "accept")
        sending = self.transfer_action(token, identity, sender, "relay")[1]["relay"]
        receiving = self.transfer_action(token, identity, receiver, "relay")[1]["relay"]
        self.room_request(sending, "sender", "POST", "open")
        self.room_request(receiving, "receiver", "POST", "open")
        ack = bytes(38)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", ack)[0], 200)
        self.assertEqual(self.transfer_action(token, identity, sender, "complete")[0], 403)
        completed = self.transfer_action(token, identity, receiver, "complete")
        self.assertEqual(completed[1]["transfer"]["status"], "completed")
        self.assertEqual(self.transfer_action(token, identity, receiver, "complete"), completed)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 410)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "frames", ack)[0], 410)
        status, body, _ = self.room_request(sending, "sender", "GET", "frames")
        self.assertEqual((status, body), (200, ack))
        self.assertEqual(self.transfer_action(token, identity, sender, "relay")[0], 409)
        self.assertLessEqual(self.server.state.rooms[sending["roomId"]].expires_at, time.time() + 15)

    def test_cancellation_revocation_and_logout_invalidate_live_room_tokens(self):
        token, sender, receiver, identity = self.file_invitation()
        self.transfer_action(token, identity, receiver, "accept")
        sending = self.transfer_action(token, identity, sender, "relay")[1]["relay"]
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 200)
        self.assertEqual(self.transfer_action(token, identity, sender, "cancel")[0], 200)
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)
        self.assertEqual(self.server.state.total_bytes, 0)
        for operation in ("revoke", "logout"):
            status, value = self.api("POST", "/transfers", {
                "senderDeviceId": sender, "receiverDeviceId": receiver, "kind": "file",
                "text": "", "fileName": "test.txt", "fileSize": 12}, token)
            self.assertEqual(status, 201)
            identity = value["transfer"]["id"]
            self.transfer_action(token, identity, receiver, "accept")
            sending = self.transfer_action(token, identity, sender, "relay")[1]["relay"]
            if operation == "revoke":
                self.assertEqual(self.api("DELETE", "/devices/" + receiver, token=token)[0], 200)
                self.api("PUT", "/devices/" + receiver, {
                    "displayName": "returned", "platform": "windows",
                    "publicKey": base64.b64encode(secrets.token_bytes(32)).decode("ascii")}, token)
            else:
                self.assertEqual(self.api("POST", "/auth/logout", {}, token)[0], 200)
            self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)

    def test_logout_with_device_revokes_device_and_its_other_session_room(self):
        token, sender, receiver, identity = self.file_invitation()
        self.transfer_action(token, identity, receiver, "accept")
        receiving = self.transfer_action(token, identity, receiver, "relay")[1]["relay"]
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 200)
        status, other = self.api("POST", "/auth/login", {
            "username": "file_user", "password": "Combined file test password!"})
        self.assertEqual(status, 200)
        self.assertEqual(self.api("POST", "/auth/logout", {"deviceId": receiver}, other["token"])[0], 200)
        self.assertEqual(self.api("GET", "/auth/me", token=other["token"])[0], 401)
        self.assertEqual(self.room_request(receiving, "receiver", "POST", "open")[0], 401)
        status, devices = self.api("GET", "/devices", token=token)
        self.assertEqual(status, 200)
        self.assertEqual([item["id"] for item in devices["devices"]], [sender])
        listing = self.api("GET", "/devices/" + sender + "/transfers", token=token)[1]["transfers"]
        self.assertEqual(listing[0]["status"], "cancelled")

    def test_text_limit_survives_worst_case_escaping_in_shared_https_listener(self):
        token, sender, receiver, _ = self.file_invitation()
        text = "<" * 16384
        payload = {"senderDeviceId": sender, "receiverDeviceId": receiver,
                   "kind": "text", "text": text, "fileName": "", "fileSize": 0}
        encoded = json.dumps(payload).replace("<", "\\u003C").encode("ascii")
        self.assertGreater(len(encoded), 64 * 1024)
        status, body, _ = self.request("POST", "/api/v1/transfers", encoded,
                                      {"Content-Type": "application/json", "Authorization": "Bearer " + token})
        self.assertEqual(status, 201)
        invitation = json.loads(body)["transfer"]
        status, accepted = self.transfer_action(token, invitation["id"], receiver, "accept")
        self.assertEqual(status, 200)
        self.assertEqual(accepted["transfer"]["text"], text)

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

    def test_restart_fails_active_file_but_preserves_pending_invitation(self):
        token, sender, receiver, identity = self.file_invitation()
        self.transfer_action(token, identity, receiver, "accept")
        sending = self.transfer_action(token, identity, sender, "relay")[1]["relay"]
        pending = self.api("POST", "/transfers", {
            "senderDeviceId": sender, "receiverDeviceId": receiver, "kind": "text",
            "text": "still pending", "fileName": "", "fileSize": 0}, token)[1]["transfer"]["id"]
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)
        self.server = relay.create_server(
            "127.0.0.1", 0, str(self.directory / "server.pem"), str(self.directory / "server.key"),
            {"legacy": {"sender_token": "1" * 64, "receiver_token": "2" * 64}},
            account_database=self.database_directory / "accounts.sqlite3")
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()
        listing = self.api("GET", "/devices/" + receiver + "/transfers", token=token)[1]["transfers"]
        states = {item["id"]: item["status"] for item in listing}
        self.assertEqual(states[identity], "failed")
        self.assertEqual(states[pending], "pending")
        self.assertEqual(self.room_request(sending, "sender", "POST", "open")[0], 401)

    def test_accounts_and_legacy_frames_share_verified_listener(self):
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
        root = "/v1/rooms/legacy/" + "a" * 32
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
