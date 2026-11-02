#!/usr/bin/env python3
"""HTTP integration tests for the account and same-account device directory.

Run: python cross_transfer/account_service/test_server.py
The service binds only ephemeral loopback ports and uses temporary databases.
"""

import base64
import concurrent.futures
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

    def transfer_pair(self):
        session = self.register()
        sender, receiver = secrets.token_hex(16), secrets.token_hex(16)
        self.put_device(session["token"], sender, self.device("发送设备"))
        self.put_device(session["token"], receiver, self.device("接收设备"))
        return session, sender, receiver

    def invite(self, token, sender, receiver, **overrides):
        data = {"senderDeviceId": sender, "receiverDeviceId": receiver,
                "kind": "text", "text": "你好，另一台设备 🌍", "fileName": "", "fileSize": 0}
        data.update(overrides)
        return self.api("POST", "/transfers", data, token=token)

    def act(self, token, identity, device, action):
        return self.api("POST", "/transfers/" + identity + "/" + action,
                        {"deviceId": device}, token=token)

    def test_text_invitation_requires_recipient_confirmation(self):
        session, sender, receiver = self.transfer_pair()
        token = session["token"]
        status, value = self.invite(token, sender, receiver)
        self.assertEqual(status, 201)
        invitation = value["transfer"]
        identity = invitation["id"]
        self.assertEqual(invitation["status"], "pending")
        self.assertEqual(invitation["senderName"], "发送设备")
        self.assertEqual(invitation["receiverName"], "接收设备")
        listing = self.api("GET", "/devices/" + receiver + "/transfers", token=token)
        self.assertEqual(listing, (200, {"transfers": [invitation]}))
        self.assert_error(self.act(token, identity, sender, "accept"), 403, "forbidden")
        self.assert_error(self.act(token, identity, receiver, "relay"), 409, "transfer_state")
        status, accepted = self.act(token, identity, receiver, "accept")
        self.assertEqual(status, 200)
        self.assertEqual(accepted["transfer"]["status"], "completed")
        self.assertEqual(accepted["transfer"]["text"], invitation["text"])
        self.assertEqual(self.act(token, identity, receiver, "accept"), (200, accepted))
        self.assert_error(self.act(token, identity, sender, "cancel"), 409, "transfer_state")
        serialized = json.dumps(accepted)
        self.assertNotIn(token, serialized)
        self.assertNotIn("sender_session", serialized)
        self.assertNotIn("bearerToken", serialized)

    def test_transfer_permission_checks_hide_other_accounts_and_devices(self):
        session, sender, receiver = self.transfer_pair()
        token = session["token"]
        identity = self.invite(token, sender, receiver)[1]["transfer"]["id"]
        bob = self.register("bob")
        other = secrets.token_hex(16)
        self.put_device(bob["token"], other)
        self.assert_error(self.invite(bob["token"], other, receiver), 404, "not_found")
        self.assert_error(self.api("GET", "/devices/" + receiver + "/transfers",
                                   token=bob["token"]), 404, "not_found")
        for action in ("accept", "reject", "cancel", "complete", "relay"):
            self.assert_error(self.act(bob["token"], identity, other, action), 404, "not_found")
            self.assert_error(self.act(None, identity, receiver, action), 401, "unauthorized")
        third = secrets.token_hex(16)
        self.put_device(token, third)
        self.assert_error(self.act(token, identity, third, "accept"), 404, "not_found")
        self.assertEqual(self.api("GET", "/devices/" + third + "/transfers", token=token),
                         (200, {"transfers": []}))

    def test_transfer_metadata_limits_and_file_names(self):
        session, sender, receiver = self.transfer_pair()
        token = session["token"]
        for bad in ({"senderDeviceId": receiver}, {"text": ""}, {"text": "x" * 16385},
                    {"text": "\0"}, {"text": 3}, {"fileSize": True}, {"kind": "unknown"},
                    {"kind": "file", "text": "", "fileName": "../secret", "fileSize": 1},
                    {"kind": "file", "text": "", "fileName": "C:\\a.txt", "fileSize": 1},
                    {"kind": "file", "text": "", "fileName": "CON.txt", "fileSize": 1},
                    {"kind": "file", "text": "", "fileName": "a.txt", "fileSize": -1},
                    {"kind": "file", "text": "", "fileName": "a.txt", "fileSize": 2**34}):
            with self.subTest(bad=bad.keys()):
                self.assert_error(self.invite(token, sender, receiver, **bad), 400, "invalid_request")
        self.assertEqual(self.invite(token, sender, receiver, text="中" * 5461)[0], 201)
        status, value = self.invite(token, sender, receiver, kind="file", text="",
                                    fileName="空文件.txt", fileSize=0)
        self.assertEqual(status, 201)
        # Standalone account HTTP tests intentionally have no relay attached.
        self.assert_error(self.act(token, value["transfer"]["id"], receiver, "accept"),
                          503, "unavailable")

    def test_text_limit_allows_worst_case_json_escaping(self):
        session, sender, receiver = self.transfer_pair()
        text = "<" * account_server.MAX_TEXT_BYTES
        payload = {"senderDeviceId": sender, "receiverDeviceId": receiver,
                   "kind": "text", "text": text, "fileName": "", "fileSize": 0}
        # Chromium escapes '<' to prevent accidental script interpretation.
        encoded = json.dumps(payload).replace("<", "\\u003C").encode("ascii")
        self.assertGreater(len(encoded), 64 * 1024)
        status, value = self.api("POST", "/transfers", raw_body=encoded,
                                 token=session["token"])
        self.assertEqual(status, 201)
        accepted = self.act(session["token"], value["transfer"]["id"], receiver, "accept")
        self.assertEqual(accepted[1]["transfer"]["text"], text)

    def test_rejection_cancellation_expiry_and_active_limit(self):
        session, sender, receiver = self.transfer_pair()
        token = session["token"]
        identity = self.invite(token, sender, receiver)[1]["transfer"]["id"]
        rejected = self.act(token, identity, receiver, "reject")
        self.assertEqual(rejected[1]["transfer"]["status"], "rejected")
        self.assertEqual(self.act(token, identity, receiver, "reject"), rejected)
        self.assert_error(self.act(token, identity, receiver, "accept"), 409, "transfer_state")
        cancelled = self.invite(token, sender, receiver)[1]["transfer"]["id"]
        self.assertEqual(self.act(token, cancelled, sender, "cancel")[1]["transfer"]["status"], "cancelled")
        self.assertEqual(self.act(token, cancelled, receiver, "cancel")[0], 200)
        expiring = self.invite(token, sender, receiver)[1]["transfer"]["id"]
        with self.server.application.store.connect() as connection:
            connection.execute("UPDATE transfers SET expires_at=? WHERE id=?", (time.time() - 1, expiring))
        self.assert_error(self.act(token, expiring, receiver, "accept"), 409, "transfer_state")
        for _ in range(account_server.MAX_TRANSFERS):
            self.assertEqual(self.invite(token, sender, receiver)[0], 201)
        self.assert_error(self.invite(token, sender, receiver), 429, "limit_exceeded")
        listing = self.api("GET", "/devices/" + receiver + "/transfers", token=token)[1]["transfers"]
        self.assertEqual(len(listing), 32)
        self.assertTrue(all(item["status"] == "pending" for item in listing))

    def test_device_revocation_and_logout_cancel_related_transfers(self):
        session, sender, receiver = self.transfer_pair()
        token = session["token"]
        identity = self.invite(token, sender, receiver)[1]["transfer"]["id"]
        self.assertEqual(self.api("DELETE", "/devices/" + receiver, token=token)[0], 200)
        listing = self.api("GET", "/devices/" + sender + "/transfers", token=token)[1]["transfers"]
        self.assertEqual(listing[0]["status"], "cancelled")
        self.put_device(token, receiver)
        self.assert_error(self.act(token, identity, receiver, "accept"), 409, "transfer_state")
        identity = self.invite(token, sender, receiver)[1]["transfer"]["id"]
        other_session = self.api("POST", "/auth/login", {"username": "alice", "password": PASSWORD})[1]
        self.assertEqual(self.api("POST", "/auth/logout", {}, token=token)[0], 200)
        listing = self.api("GET", "/devices/" + receiver + "/transfers",
                           token=other_session["token"])[1]["transfers"]
        self.assertEqual(next(item for item in listing if item["id"] == identity)["status"], "cancelled")

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
        session, sender, receiver = self.transfer_pair()
        other_session = self.api("POST", "/auth/login", {
            "username": "alice", "password": PASSWORD})[1]
        # Device-related invitations must be cancelled even if another session
        # created them, while that independent session remains usable.
        identity = self.invite(other_session["token"], sender, receiver)[1]["transfer"]["id"]
        self.assertEqual(self.api("POST", "/auth/logout", {"deviceId": sender},
                                  token=session["token"]), (200, {"ok": True}))
        self.assert_error(self.api("GET", "/auth/me", token=session["token"]),
                          401, "unauthorized")
        listing = self.api("GET", "/devices", token=other_session["token"])[1]["devices"]
        self.assertEqual([device["id"] for device in listing], [receiver])
        transfers = self.api("GET", "/devices/" + receiver + "/transfers",
                             token=other_session["token"])[1]["transfers"]
        self.assertEqual(next(item for item in transfers if item["id"] == identity)["status"],
                         "cancelled")

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
