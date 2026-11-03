#!/usr/bin/env python3
"""Account and own-device registry for CrossTransfer.

The default listener is private HTTP on loopback, intended to sit behind an
HTTPS reverse proxy. Public listeners require TLS certificate/key arguments.
No passwords, session tokens, request bodies, or authorization headers are logged.
Only accounts, sessions and devices are durable. Opaque messages and channels
are bounded, short-lived memory; transfer history belongs to clients.
"""

import argparse
import base64
import binascii
import collections
from contextlib import contextmanager
import hashlib
import hmac
import ipaddress
import json
import logging
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import socketserver
import sqlite3
import ssl
import stat
import threading
import time


MAX_HEADERS = 8192
# Bounds the full request independently of opaque payload byte limits.
MAX_BODY = 128 * 1024
MAX_DEVICES = 32
MAX_DEVICE_RECORDS = 256
MAX_SESSIONS = 32
MAX_MAILBOX_MESSAGES = 128
MAX_PENDING_SIGNALS = 4096
MAX_SIGNAL_RECEIPTS = 8192
MAX_CHANNELS = 128
MAX_ACCOUNT_CHANNELS = 32
MAX_SIGNAL_PAYLOAD = 16 * 1024
MAX_EXCHANGE_PAYLOAD = 112 * 1024
MAX_OPAQUE_BYTES = 32 * 1024 * 1024
MAX_PAYLOAD_DEPTH = 64
SIGNAL_TTL = 5 * 60
CHANNEL_TTL = 30 * 60
REQUEST_TIMEOUT = 10
USERNAME = re.compile(r"[A-Za-z0-9_.-]{3,64}\Z")
DEVICE_ID = re.compile(r"[0-9a-f]{32}\Z")
TOKEN = re.compile(r"[0-9a-f]{64}\Z")
PLATFORM = re.compile(r"[A-Za-z0-9_.-]{1,32}\Z")
HEADER_NAME = re.compile(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+\Z")
DEVICE_ROUTE = re.compile(r"/api/v1/devices/([0-9a-f]{32})(/heartbeat)?\Z")
DEVICE_SIGNALS_ROUTE = re.compile(r"/api/v1/devices/([0-9a-f]{32})/signals(/ack)?\Z")
CHANNEL_ROUTE = re.compile(r"/api/v1/channels/([0-9a-f]{32})(?:/(exchange|relay|release))?\Z")


class ApiError(Exception):
    def __init__(self, status, code, message):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


def invalid(message="Invalid request", status=400):
    return ApiError(status, "invalid_request", message)


def _strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def _invalid_constant(value):
    raise ValueError("Non-finite JSON numbers are not supported")


def _json_body(body, expected, optional=False, optional_fields=()):
    if not body and optional:
        return {}
    try:
        value = json.loads(body.decode("utf-8"), object_pairs_hook=_strict_object,
                           parse_constant=_invalid_constant)
    except (UnicodeError, ValueError, RecursionError):
        raise invalid("Expected a valid JSON object") from None
    if (not isinstance(value, dict) or not set(expected) <= set(value) or
            not set(value) <= set(expected) | set(optional_fields)):
        raise invalid("Unexpected or missing JSON fields")
    return value


def _credentials(body):
    data = _json_body(body, ("username", "password"))
    username, password = data["username"], data["password"]
    if not isinstance(username, str) or not USERNAME.fullmatch(username):
        raise invalid("Username must be 3-64 ASCII letters, digits, '.', '_' or '-'")
    if (not isinstance(password, str) or not 12 <= len(password) <= 256 or
            "\0" in password):
        raise invalid("Password must contain 12-256 characters")
    try:
        if len(password.encode("utf-8")) > 1024:
            raise invalid("Password is too long")
    except UnicodeError:
        raise invalid("Password must be valid UTF-8") from None
    return username, password


def _device_input(body):
    data = _json_body(body, ("displayName", "platform", "publicKey"))
    name, platform, key = data["displayName"], data["platform"], data["publicKey"]
    if (not isinstance(name, str) or not 1 <= len(name) <= 128 or
            name != name.strip() or any(ord(char) < 32 or ord(char) == 127 for char in name)):
        raise invalid("Invalid device display name")
    try:
        if len(name.encode("utf-8")) > 512:
            raise invalid("Device display name is too long")
    except UnicodeError:
        raise invalid("Device display name must be valid UTF-8") from None
    if not isinstance(platform, str) or not PLATFORM.fullmatch(platform):
        raise invalid("Invalid device platform")
    if not isinstance(key, str) or len(key) != 44:
        raise invalid("Expected a base64-encoded 32-byte Ed25519 public key")
    try:
        decoded = base64.b64decode(key, validate=True)
    except (ValueError, binascii.Error):
        raise invalid("Invalid device public key") from None
    if len(decoded) != 32 or base64.b64encode(decoded).decode("ascii") != key:
        raise invalid("Invalid device public key")
    return name, platform, key


def _opaque_payload(value, limit):
    if not isinstance(value, dict):
        raise invalid("Expected an opaque payload object")
    # Bound nesting independently of encoded size, without retaining another
    # object tree or making recursive copies of untrusted JSON.
    exhausted = object()
    stack = [iter((value,))]
    while stack:
        child = next(stack[-1], exhausted)
        if child is exhausted:
            stack.pop()
        elif isinstance(child, (dict, list)):
            if len(stack) > MAX_PAYLOAD_DEPTH:
                raise invalid("Payload nesting is too deep")
            stack.append(iter(child.values() if isinstance(child, dict) else child))
    try:
        encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                             sort_keys=True, allow_nan=False).encode("utf-8")
    except (UnicodeError, ValueError, RecursionError):
        raise invalid("Payload must be valid UTF-8 JSON") from None
    if len(encoded) > limit:
        raise invalid("Payload exceeds its byte limit", 413)
    return encoded


def _signal_input(body):
    data = _json_body(body, ("id", "fromDeviceId", "toDeviceId", "payload"))
    for key in ("id", "fromDeviceId", "toDeviceId"):
        if not isinstance(data[key], str) or not DEVICE_ID.fullmatch(data[key]):
            raise invalid("Expected a 32-character lowercase hexadecimal identity")
    if data["fromDeviceId"] == data["toDeviceId"]:
        raise invalid("Select a different registered device")
    data["payload"] = _opaque_payload(data["payload"], MAX_SIGNAL_PAYLOAD)
    return data


def _stun_urls(values):
    if not isinstance(values, (list, tuple)) or len(values) > 8:
        raise ValueError("At most eight STUN URLs may be configured")
    result = []
    for value in values:
        if not isinstance(value, str) or len(value) > 512:
            raise ValueError("Invalid STUN URL")
        match = re.fullmatch(r"stuns?:((?:[A-Za-z0-9.-]+)|(?:\[[0-9a-fA-F:]+\]))(?::([0-9]{1,5}))?", value)
        if not match or (match[2] is not None and not 1 <= int(match[2]) <= 65535):
            raise ValueError("Expected stun:host:port or stuns:host:port")
        host = match[1]
        if host.startswith("["):
            try:
                ipaddress.IPv6Address(host[1:-1])
            except ValueError:
                raise ValueError("Invalid IPv6 STUN host") from None
        elif (len(host) > 253 or any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
                                   for label in host.rstrip(".").split("."))):
            raise ValueError("Invalid STUN hostname")
        if value not in result:
            result.append(value)
    return tuple(result)



class RateLimiter:
    def __init__(self, login_limit, registration_limit, window):
        if login_limit <= 0 or registration_limit <= 0 or window <= 0:
            raise ValueError("rate limits and window must be positive")
        self.limits = {"login": login_limit, "register": registration_limit}
        self.window = window
        self.buckets = collections.OrderedDict()
        self.lock = threading.Lock()

    def check(self, operation, address):
        now = time.monotonic()
        key = (operation, address)
        with self.lock:
            while self.buckets and next(iter(self.buckets.values()))[-1] <= now - self.window:
                self.buckets.popitem(last=False)
            bucket = self.buckets.get(key)
            if bucket is None:
                # Refuse new sources when full instead of evicting active rate
                # limits, which would allow an attacker to recycle addresses.
                if len(self.buckets) >= 8192:
                    raise ApiError(429, "rate_limited", "Try again later")
                bucket = collections.deque()
                self.buckets[key] = bucket
            while bucket and bucket[0] <= now - self.window:
                bucket.popleft()
            self.buckets.move_to_end(key)
            if len(bucket) >= self.limits[operation]:
                raise ApiError(429, "rate_limited", "Try again later")
            bucket.append(now)


class AccountStore:
    def __init__(self, database, session_ttl, online_timeout):
        if session_ttl <= 0 or online_timeout <= 0:
            raise ValueError("session and presence lifetimes must be positive")
        self.database = Path(database).absolute()
        self.session_ttl = session_ttl
        self.online_timeout = online_timeout
        self.hash_slots = threading.BoundedSemaphore(2)
        directory = self.database.parent
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if directory.is_symlink():
            raise ValueError("database directory must not be a symlink")
        if os.name != "nt" and stat.S_IMODE(directory.stat().st_mode) & 0o077:
            raise ValueError("database requires a private directory with mode 0700")
        flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(self.database, flags, 0o600)
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise ValueError("database must be a regular file with one link")
            if os.name != "nt":
                os.fchmod(descriptor, 0o600)
        finally:
            os.close(descriptor)
        with self.connect() as connection:
            version = connection.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1, 2, 3, 4):
                raise ValueError("unsupported account database version")
            connection.execute("PRAGMA journal_mode=WAL")
            connection.executescript("""
                CREATE TABLE IF NOT EXISTS accounts (
                    id TEXT PRIMARY KEY,
                    username TEXT NOT NULL,
                    username_key TEXT NOT NULL UNIQUE,
                    salt BLOB NOT NULL,
                    password_hash BLOB NOT NULL,
                    created_at REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS sessions (
                    token_hash TEXT PRIMARY KEY,
                    account_id TEXT NOT NULL REFERENCES accounts(id),
                    created_at REAL NOT NULL,
                    expires_at REAL NOT NULL
                );
                CREATE INDEX IF NOT EXISTS sessions_account ON sessions(account_id);
                CREATE INDEX IF NOT EXISTS sessions_expiry ON sessions(expires_at);
                CREATE TABLE IF NOT EXISTS devices (
                    id TEXT PRIMARY KEY,
                    account_id TEXT NOT NULL REFERENCES accounts(id),
                    display_name TEXT NOT NULL,
                    platform TEXT NOT NULL,
                    public_key TEXT NOT NULL,
                    last_seen REAL NOT NULL,
                    revoked INTEGER NOT NULL DEFAULT 0
                );
                CREATE INDEX IF NOT EXISTS devices_account ON devices(account_id);
                DROP TABLE IF EXISTS transfers;
                PRAGMA user_version=4;
            """)
        self.dummy_salt = secrets.token_bytes(16)
        self.dummy_hash = self.password_hash("invalid-password-placeholder", self.dummy_salt)

    @contextmanager
    def connect(self):
        connection = sqlite3.connect(self.database, timeout=5)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys=ON")
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def password_hash(self, password, salt):
        with self.hash_slots:
            return hashlib.scrypt(password.encode("utf-8"), salt=salt, n=32768,
                                  r=8, p=1, maxmem=64 * 1024 * 1024, dklen=32)

    @staticmethod
    def account_value(row):
        return {"id": row["id"], "username": row["username"]}

    def new_session(self, connection, account, now):
        connection.execute("DELETE FROM sessions WHERE expires_at<=?", (now,))
        connection.execute("""DELETE FROM sessions WHERE token_hash IN (
            SELECT token_hash FROM sessions WHERE account_id=?
            ORDER BY created_at DESC, rowid DESC LIMIT -1 OFFSET ?)
            """, (account["id"], MAX_SESSIONS - 1))
        token = secrets.token_hex(32)
        expires = now + self.session_ttl
        connection.execute("INSERT INTO sessions VALUES(?,?,?,?)",
                           (hashlib.sha256(token.encode("ascii")).hexdigest(),
                            account["id"], now, expires))
        return {"account": self.account_value(account), "token": token, "expiresAt": expires}

    def register(self, username, password):
        salt = secrets.token_bytes(16)
        digest = self.password_hash(password, salt)
        now = time.time()
        account = {"id": secrets.token_hex(16), "username": username}
        try:
            with self.connect() as connection:
                connection.execute("INSERT INTO accounts VALUES(?,?,?,?,?,?)",
                                   (account["id"], username, username.lower(), salt, digest, now))
                return self.new_session(connection, account, now)
        except sqlite3.IntegrityError:
            raise ApiError(409, "username_taken", "Username is unavailable") from None

    def login(self, username, password):
        with self.connect() as connection:
            row = connection.execute("SELECT * FROM accounts WHERE username_key=?",
                                     (username.lower(),)).fetchone()
        digest = self.password_hash(password, row["salt"] if row else self.dummy_salt)
        if not hmac.compare_digest(digest, row["password_hash"] if row else self.dummy_hash) or row is None:
            raise ApiError(401, "invalid_credentials", "Invalid username or password")
        with self.connect() as connection:
            return self.new_session(connection, row, time.time())

    def authenticate(self, token):
        if not TOKEN.fullmatch(token):
            raise ApiError(401, "unauthorized", "Authentication required")
        digest = hashlib.sha256(token.encode("ascii")).hexdigest()
        with self.connect() as connection:
            row = connection.execute("""SELECT accounts.id, accounts.username FROM sessions
                JOIN accounts ON accounts.id=sessions.account_id
                WHERE sessions.token_hash=? AND sessions.expires_at>?""",
                                     (digest, time.time())).fetchone()
        if row is None:
            raise ApiError(401, "unauthorized", "Authentication required")
        return self.account_value(row)

    @staticmethod
    def require_session(connection, account_id, token_hash):
        row = connection.execute(
            "SELECT expires_at FROM sessions WHERE token_hash=? AND account_id=? AND expires_at>?",
            (token_hash, account_id, time.time())).fetchone()
        if row is None:
            raise ApiError(401, "unauthorized", "Authentication required")
        return row["expires_at"]

    def active_sessions(self, token_hashes, now):
        result = set()
        # Only inspect hashes referenced by the bounded in-memory resources;
        # do not scan all accounts' sessions or exceed SQLite's parameter cap.
        hashes = list(token_hashes)
        with self.connect() as connection:
            for start in range(0, len(hashes), 256):
                batch = hashes[start:start + 256]
                placeholders = ",".join("?" for _ in batch)
                result.update(row[0] for row in connection.execute(
                    "SELECT token_hash FROM sessions WHERE expires_at>? AND token_hash IN (" +
                    placeholders + ")", (now, *batch)))
        return result

    def logout(self, token, device_id=None):
        digest = hashlib.sha256(token.encode("ascii")).hexdigest()
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            session = connection.execute(
                "SELECT account_id FROM sessions WHERE token_hash=? AND expires_at>?",
                (digest, time.time())).fetchone()
            if session is None:
                raise ApiError(401, "unauthorized", "Authentication required")
            account_id = session["account_id"]
            if device_id is not None:
                device = connection.execute("SELECT account_id FROM devices WHERE id=?",
                                            (device_id,)).fetchone()
                if device is not None and device["account_id"] != account_id:
                    raise ApiError(404, "not_found", "Device not found")
                # An interrupted registration may never have created the device.
                # Revoke the session even when that device does not exist yet.
                connection.execute("UPDATE devices SET revoked=1 WHERE id=? AND account_id=?",
                                   (device_id, account_id))
            connection.execute("DELETE FROM sessions WHERE token_hash=?", (digest,))
        return account_id, digest

    def device_value(self, row, now=None):
        now = time.time() if now is None else now
        return {"id": row["id"], "displayName": row["display_name"],
                "platform": row["platform"], "publicKey": row["public_key"],
                "lastSeenAt": row["last_seen"],
                "online": now - row["last_seen"] <= self.online_timeout}

    def list_devices(self, account_id):
        with self.connect() as connection:
            rows = connection.execute("""SELECT * FROM devices WHERE account_id=? AND revoked=0
                ORDER BY display_name COLLATE NOCASE, id""", (account_id,)).fetchall()
        now = time.time()
        return [self.device_value(row, now) for row in rows]

    def put_device(self, account_id, device_id, name, platform, key):
        now = time.time()
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute("SELECT * FROM devices WHERE id=?", (device_id,)).fetchone()
            if row is not None and row["account_id"] != account_id:
                raise ApiError(409, "device_conflict", "Device identity is unavailable")
            if row is None or row["revoked"]:
                count = connection.execute(
                    "SELECT COUNT(*) FROM devices WHERE account_id=? AND revoked=0",
                    (account_id,)).fetchone()[0]
                if count >= MAX_DEVICES:
                    raise ApiError(429, "limit_exceeded", "Device limit reached")
            if row is None:
                count = connection.execute("SELECT COUNT(*) FROM devices WHERE account_id=?",
                                           (account_id,)).fetchone()[0]
                if count >= MAX_DEVICE_RECORDS:
                    raise ApiError(429, "limit_exceeded", "Device identity limit reached")
                connection.execute("INSERT INTO devices VALUES(?,?,?,?,?,?,0)",
                                   (device_id, account_id, name, platform, key, now))
            else:
                connection.execute("""UPDATE devices SET display_name=?, platform=?, public_key=?,
                    last_seen=?, revoked=0 WHERE id=? AND account_id=?""",
                                   (name, platform, key, now, device_id, account_id))
            row = connection.execute("SELECT * FROM devices WHERE id=?", (device_id,)).fetchone()
        return self.device_value(row, now)

    def heartbeat(self, account_id, device_id):
        now = time.time()
        with self.connect() as connection:
            changed = connection.execute("""UPDATE devices SET last_seen=?
                WHERE id=? AND account_id=? AND revoked=0""", (now, device_id, account_id)).rowcount
            if not changed:
                raise ApiError(404, "not_found", "Device not found")
            row = connection.execute("SELECT * FROM devices WHERE id=?", (device_id,)).fetchone()
        return self.device_value(row, now)

    def revoke(self, account_id, device_id):
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            changed = connection.execute("""UPDATE devices SET revoked=1
                WHERE id=? AND account_id=? AND revoked=0""", (device_id, account_id)).rowcount
            if not changed:
                raise ApiError(404, "not_found", "Device not found")

    @staticmethod
    def require_device(connection, account_id, device_id):
        row = connection.execute("SELECT * FROM devices WHERE id=? AND account_id=? AND revoked=0",
                                 (device_id, account_id)).fetchone()
        if row is None:
            raise ApiError(404, "not_found", "Device not found")
        return row


class AccountApplication:
    """Reusable account API; the caller owns HTTP framing and TLS transport."""

    def __init__(self, database, *, session_ttl=7 * 24 * 60 * 60,
                 rate_limit=10, rate_window=60, registration_limit=5,
                 online_timeout=90, trusted_proxies=(), relay=None, direct_min_bytes=0,
                 stun_urls=()):
        self.stun_urls = _stun_urls(stun_urls)
        if type(direct_min_bytes) is not int or not 0 <= direct_min_bytes <= 8 * 1024 * 1024 * 1024 + 1:
            raise ValueError("direct_min_bytes must be an integer between 0 and 8 GiB + 1")
        self.direct_min_bytes = direct_min_bytes
        self.store = AccountStore(database, session_ttl, online_timeout)
        self.limiter = RateLimiter(rate_limit, registration_limit, rate_window)
        self.trusted_proxies = tuple(ipaddress.ip_network(value) for value in trusted_proxies)
        self.relay = relay
        # Serialize mailbox delivery and channel ACL changes with relay
        # revocation. Relay operations never acquire this lock.
        self.signal_lock = threading.RLock()
        self.mailboxes = {}
        self.receipts = {}
        self.channels = {}
        self.opaque_bytes = 0

    @staticmethod
    def digest(data):
        return hashlib.sha256(data).hexdigest()

    @staticmethod
    def decode_envelope(value):
        return dict(value, payload=json.loads(value["payload"]))

    def check_payload_capacity(self, size, previous=0):
        if self.opaque_bytes - previous + size > MAX_OPAQUE_BYTES:
            raise ApiError(429, "limit_exceeded", "Opaque buffer capacity reached")

    def drop_signal(self, device_id, identity):
        mailbox = self.mailboxes.get(device_id)
        if mailbox is not None:
            message = mailbox.pop(identity, None)
            if message is not None:
                self.opaque_bytes -= len(message["payload"])
            if not mailbox:
                self.mailboxes.pop(device_id, None)

    def drop_channel(self, identity, *, drain=False):
        channel = self.channels.pop(identity, None)
        if channel is not None:
            self.opaque_bytes -= sum(len(slot["payload"]) for slot in channel["slots"].values())
        if self.relay is not None:
            if drain:
                self.relay.finish_dynamic("own_" + identity)
            else:
                self.relay.revoke_dynamic("own_" + identity)

    def expire(self, *, check_sessions=False):
        with self.signal_lock:
            now = time.time()
            active_rooms = None
            if self.relay is not None and any(channel.get("relayAllocated") for channel in self.channels.values()):
                active_rooms = self.relay.active_dynamic_rooms()
            valid_sessions = None
            if check_sessions:
                hashes = {digest for channel in self.channels.values()
                          for digest in channel["sessions"].values()}
                hashes.update(receipt["session"] for receipt in self.receipts.values())
                if hashes:
                    valid_sessions = self.store.active_sessions(hashes, now)
            for identity, channel in list(self.channels.items()):
                relay_closed = (channel.get("relayAllocated") and active_rooms is not None and
                                "own_" + identity not in active_rooms)
                if (channel["expiresAt"] <= now or
                        relay_closed or
                        (valid_sessions is not None and
                        any(digest not in valid_sessions for digest in channel["sessions"].values()))):
                    self.drop_channel(identity)
            for identity, receipt in list(self.receipts.items()):
                if (receipt["expiresAt"] <= now or (valid_sessions is not None and
                        receipt["session"] not in valid_sessions)):
                    self.receipts.pop(identity)
                    self.drop_signal(receipt["toDeviceId"], identity)

    def revoke_session_or_device(self, account_id, token_hash=None, device_id=None):
        for identity, channel in list(self.channels.items()):
            if channel["accountId"] == account_id and (
                    (token_hash is not None and token_hash in channel["sessions"].values()) or
                    (device_id is not None and device_id in
                     (channel["senderDeviceId"], channel["receiverDeviceId"]))):
                self.drop_channel(identity)
        for identity, receipt in list(self.receipts.items()):
            if receipt["accountId"] == account_id and (
                    receipt["session"] == token_hash or
                    (device_id is not None and device_id in
                     (receipt["fromDeviceId"], receipt["toDeviceId"]))):
                self.drop_signal(receipt["toDeviceId"], identity)
                self.receipts.pop(identity)

    def envelope(self, data, receipt):
        return {**data,
                **{key: receipt[key] for key in
                   ("fromName", "toName", "createdAt", "expiresAt")}}

    def send_signal(self, account_id, token, data):
        self.expire()
        token_hash = hashlib.sha256(token.encode("ascii")).hexdigest()
        with self.store.connect() as db:
            session_expiry = self.store.require_session(db, account_id, token_hash)
            sender = self.store.require_device(db, account_id, data["fromDeviceId"])
            receiver = self.store.require_device(db, account_id, data["toDeviceId"])
        digest = self.digest(b"".join(data[key].encode("ascii") for key in
                                     ("id", "fromDeviceId", "toDeviceId")) + data["payload"])
        previous = self.receipts.get(data["id"])
        if previous is not None:
            if previous["accountId"] != account_id:
                raise ApiError(404, "not_found", "Signal not found")
            if previous["digest"] != digest:
                raise ApiError(409, "signal_conflict", "Message identity is already used")
            return 200, {"signal": self.decode_envelope(self.envelope(data, previous))}
        mailbox = self.mailboxes.get(data["toDeviceId"], {})
        if (len(mailbox) >= MAX_MAILBOX_MESSAGES or
                sum(len(items) for items in self.mailboxes.values()) >= MAX_PENDING_SIGNALS or
                len(self.receipts) >= MAX_SIGNAL_RECEIPTS):
            raise ApiError(429, "limit_exceeded", "Signal mailbox capacity reached")
        self.check_payload_capacity(len(data["payload"]))
        now = time.time()
        receipt = {"accountId": account_id, "digest": digest,
                   "session": token_hash,
                   "fromDeviceId": data["fromDeviceId"], "toDeviceId": data["toDeviceId"],
                   "fromName": sender["display_name"], "toName": receiver["display_name"],
                   "createdAt": now, "expiresAt": min(now + SIGNAL_TTL, session_expiry)}
        self.receipts[data["id"]] = receipt
        self.mailboxes.setdefault(data["toDeviceId"], collections.OrderedDict())[data["id"]] = self.envelope(data, receipt)
        self.opaque_bytes += len(data["payload"])
        return 201, {"signal": self.decode_envelope(self.envelope(data, receipt))}

    def channel_for(self, account_id, token, identity, data, *, bind=True):
        self.expire()
        device, peer, role = (data[key] for key in ("deviceId", "peerDeviceId", "role"))
        if (not isinstance(device, str) or not DEVICE_ID.fullmatch(device) or
                not isinstance(peer, str) or not DEVICE_ID.fullmatch(peer) or
                device == peer or role not in ("sender", "receiver")):
            raise invalid("Invalid channel participants or role")
        token_hash = hashlib.sha256(token.encode("ascii")).hexdigest()
        with self.store.connect() as db:
            session_expiry = self.store.require_session(db, account_id, token_hash)
            self.store.require_device(db, account_id, device)
            self.store.require_device(db, account_id, peer)
        sender, receiver = (device, peer) if role == "sender" else (peer, device)
        channel = self.channels.get(identity)
        if channel is None:
            if not bind:
                raise ApiError(404, "not_found", "Channel not found")
            if (len(self.channels) >= MAX_CHANNELS or
                    sum(item["accountId"] == account_id for item in self.channels.values()) >= MAX_ACCOUNT_CHANNELS):
                raise ApiError(429, "limit_exceeded", "Channel capacity reached")
            channel = {"accountId": account_id, "senderDeviceId": sender,
                       "receiverDeviceId": receiver, "sessions": {}, "slots": {},
                       "expiresAt": min(time.time() + CHANNEL_TTL, session_expiry)}
        elif channel["accountId"] != account_id:
            raise ApiError(404, "not_found", "Channel not found")
        elif (channel["senderDeviceId"], channel["receiverDeviceId"]) != (sender, receiver):
            raise ApiError(409, "channel_conflict", "Channel participant binding cannot change")
        if role in channel["sessions"] and channel["sessions"][role] != token_hash:
            raise ApiError(403, "forbidden", "Channel role belongs to another session")
        if not bind:
            if role not in channel["sessions"]:
                raise ApiError(403, "forbidden", "Only a bound role may release this channel")
            return channel
        old = channel["slots"].get(role)
        if "payload" in data and (old is None or data["sequence"] > old["sequence"]):
            self.check_payload_capacity(len(data["payload"]), len(old["payload"]) if old else 0)
        channel["expiresAt"] = min(channel["expiresAt"], session_expiry)
        if self.relay is not None:
            self.relay.limit_dynamic_expiry("own_" + identity, channel["expiresAt"])
        channel["sessions"][role] = token_hash
        self.channels[identity] = channel
        return channel

    def trusted(self, address):
        return any(address in network for network in self.trusted_proxies)

    def client_ip(self, headers, client_address):
        remote = ipaddress.ip_address(client_address)
        if not self.trusted(remote):
            return str(remote)
        forwarded = headers.get("x-forwarded-for", "")
        if not forwarded:
            return str(remote)
        parts = forwarded.split(",")
        if len(parts) > 16:
            raise invalid("Invalid forwarded address")
        try:
            chain = [ipaddress.ip_address(part.strip()) for part in parts]
        except ValueError:
            raise invalid("Invalid forwarded address") from None
        for address in reversed(chain):
            remote = address
            if not self.trusted(address):
                break
        return str(remote)

    def handle(self, method, path, headers, body, client_address):
        """Return (HTTP status, JSON value), without exposing internal errors.

        client_address is the immediate peer's IP, never a forwarded value.
        The HTTP server must enforce MAX_BODY before reading an advertised body,
        an absolute read deadline, and unambiguous Content-Length framing.
        """
        try:
            headers = {key.lower(): value for key, value in headers.items()}
            if len(body) > MAX_BODY:
                raise invalid("Request body is too large", 413)
            if body and headers.get("content-type", "").split(";")[0].strip().lower() != "application/json":
                raise invalid("Content-Type must be application/json")
            if method in ("GET", "DELETE") and body:
                raise invalid("This request must not have a body")
            return self.dispatch(method, path, headers, body, client_address)
        except ApiError as error:
            return error.status, {"error": {"code": error.code, "message": error.message}}
        except sqlite3.Error:
            return 503, {"error": {"code": "unavailable", "message": "Service temporarily unavailable"}}
        except Exception as error:
            logging.error("Account request failed (%s)", type(error).__name__)
            return 500, {"error": {"code": "internal_error", "message": "Internal server error"}}

    def dispatch(self, method, path, headers, body, client_address):
        if path in ("/healthz", "/api/v1/healthz"):
            if method != "GET":
                raise ApiError(405, "method_not_allowed", "Method not allowed")
            return 200, {"status": "ok"}
        store = self.store
        if path in ("/api/v1/auth/register", "/api/v1/auth/login"):
            if method != "POST":
                raise ApiError(405, "method_not_allowed", "Method not allowed")
            operation = path.rsplit("/", 1)[1]
            self.limiter.check(operation, self.client_ip(headers, client_address))
            username, password = _credentials(body)
            result = store.register(username, password) if operation == "register" else store.login(username, password)
            # A new login may evict an older session, including one holding a
            # relay grant. Release those resources before returning the login.
            self.expire(check_sessions=True)
            return (201 if operation == "register" else 200), result
        match = DEVICE_ROUTE.fullmatch(path)
        signals = DEVICE_SIGNALS_ROUTE.fullmatch(path)
        channel = CHANNEL_ROUTE.fullmatch(path)
        if (path not in ("/api/v1/auth/me", "/api/v1/auth/logout", "/api/v1/devices", "/api/v1/signals", "/api/v1/transport-config")
                and not match and not signals and not channel):
            raise ApiError(404, "not_found", "Endpoint not found")
        authorization = headers.get("authorization", "")
        token = authorization[7:] if authorization.startswith("Bearer ") else ""
        account = store.authenticate(token)
        if path == "/api/v1/transport-config" and method == "GET":
            return 200, {"directMinBytes": self.direct_min_bytes,
                         "iceServers": [{"urls": list(self.stun_urls)}] if self.stun_urls else []}
        if path == "/api/v1/auth/me" and method == "GET":
            return 200, {"account": account}
        if path == "/api/v1/auth/logout" and method == "POST":
            data = _json_body(body, (), optional=True, optional_fields=("deviceId",))
            device_id = data.get("deviceId")
            if "deviceId" in data and (not isinstance(device_id, str) or not DEVICE_ID.fullmatch(device_id)):
                raise invalid("Invalid device id")
            with self.signal_lock:
                account_id, digest = store.logout(token, device_id)
                self.revoke_session_or_device(account_id, digest, device_id)
            return 200, {"ok": True}
        if path == "/api/v1/devices" and method == "GET":
            return 200, {"devices": store.list_devices(account["id"])}
        if match:
            device_id, heartbeat = match.groups()
            if not heartbeat and method == "PUT":
                fields = _device_input(body)
                with self.signal_lock:
                    account = store.authenticate(token)
                    return 200, {"device": store.put_device(account["id"], device_id, *fields)}
            if not heartbeat and method == "DELETE":
                with self.signal_lock:
                    account = store.authenticate(token)
                    store.revoke(account["id"], device_id)
                    self.revoke_session_or_device(account["id"], device_id=device_id)
                return 200, {"ok": True}
            if heartbeat and method == "POST":
                _json_body(body, (), optional=True)
                with self.signal_lock:
                    account = store.authenticate(token)
                    return 200, {"device": store.heartbeat(account["id"], device_id)}
        if path == "/api/v1/signals" and method == "POST":
            data = _signal_input(body)
            with self.signal_lock:
                account = store.authenticate(token)
                return self.send_signal(account["id"], token, data)
        if signals:
            device_id, acknowledgement = signals.groups()
            if (not acknowledgement and method == "GET") or (acknowledgement and method == "POST"):
                data = _json_body(body, ("messageIds",)) if acknowledgement else None
                if data is not None and (not isinstance(data["messageIds"], list) or
                        len(data["messageIds"]) > 64 or any(not isinstance(value, str) or
                        not DEVICE_ID.fullmatch(value) for value in data["messageIds"])):
                    raise invalid("Expected at most 64 message identities")
                with self.signal_lock:
                    account = store.authenticate(token)
                    self.expire()
                    with store.connect() as db:
                        store.require_device(db, account["id"], device_id)
                    mailbox = self.mailboxes.get(device_id, {})
                    if acknowledgement:
                        for identity in data["messageIds"]:
                            self.drop_signal(device_id, identity)
                        return 200, {"ok": True}
                    return 200, {"signals": [self.decode_envelope(value)
                                             for value in list(mailbox.values())[:64]]}
        if channel:
            identity, action = channel.groups()
            if action is None and method == "DELETE":
                with self.signal_lock:
                    account = store.authenticate(token)
                    self.expire()
                    resource = self.channels.get(identity)
                    if resource is None or resource["accountId"] != account["id"]:
                        raise ApiError(404, "not_found", "Channel not found")
                    digest = hashlib.sha256(token.encode("ascii")).hexdigest()
                    if digest not in resource["sessions"].values():
                        raise ApiError(403, "forbidden", "Only participating sessions may release this channel")
                    self.drop_channel(identity, drain=True)
                    return 200, {"ok": True}
            if action is not None and method == "POST":
                fields = ("deviceId", "peerDeviceId", "role")
                if action == "exchange":
                    fields += ("sequence", "payload")
                data = _json_body(body, fields)
                if action == "exchange":
                    if type(data["sequence"]) is not int or data["sequence"] < 0:
                        raise invalid("Sequence must be a nonnegative integer")
                    data["payload"] = _opaque_payload(data["payload"], MAX_EXCHANGE_PAYLOAD)
                with self.signal_lock:
                    account = store.authenticate(token)
                    resource = self.channel_for(account["id"], token, identity, data,
                                                bind=action != "release")
                    role = data["role"]
                    if action == "release":
                        # A participant releases only its own resource use.
                        # Keep snapshots available for the peer's last polls
                        # until both bound roles release, or the TTL expires.
                        released = resource.setdefault("releasedRoles", set())
                        released.add(role)
                        if released == {"sender", "receiver"}:
                            self.drop_channel(identity, drain=True)
                        return 200, {"ok": True}
                    if action == "exchange":
                        old = resource["slots"].get(role)
                        digest = self.digest(data["payload"])
                        if old is not None and old["sequence"] == data["sequence"] and old["digest"] != digest:
                            raise ApiError(409, "channel_conflict", "Sequence already has different payload")
                        if old is None or data["sequence"] > old["sequence"]:
                            self.opaque_bytes += len(data["payload"]) - (len(old["payload"]) if old else 0)
                            resource["slots"][role] = {"sequence": data["sequence"],
                                                       "payload": data["payload"], "digest": digest}
                        peer = resource["slots"].get("receiver" if role == "sender" else "sender")
                        return 200, {"peer": {"sequence": peer["sequence"],
                                              "payload": json.loads(peer["payload"])} if peer else None,
                                     "expiresAt": resource["expiresAt"]}
                    if self.relay is None:
                        raise ApiError(503, "unavailable", "Relay is unavailable")
                    if not self.relay.provision_dynamic("own_" + identity, identity, resource["expiresAt"]):
                        raise ApiError(429, "limit_exceeded", "Relay capacity reached")
                    # Track only allocated transport resources. When the room
                    # closes or expires, its exchange buffers can be released.
                    resource["relayAllocated"] = True
                    room = self.relay.get_dynamic("own_" + identity, role)
                    if room is None:
                        raise ApiError(410, "channel_expired", "Relay is no longer available")
                    return 200, {"relay": room}
        raise ApiError(405, "method_not_allowed", "Method not allowed")


class AccountHandler(socketserver.BaseRequestHandler):
    def respond(self, status, data):
        reasons = {200: "OK", 201: "Created", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
                   404: "Not Found", 405: "Method Not Allowed", 408: "Request Timeout",
                   409: "Conflict", 410: "Gone", 413: "Content Too Large", 429: "Too Many Requests",
                   500: "Internal Server Error", 503: "Service Unavailable"}
        body = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        header = (f"HTTP/1.1 {status} {reasons[status]}\r\n"
                  f"Content-Length: {len(body)}\r\n"
                  "Content-Type: application/json; charset=utf-8\r\n"
                  "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                  "Connection: close\r\n\r\n").encode("ascii")
        self.request.settimeout(2)
        self.request.sendall(header + body)

    def handle(self):
        try:
            method, path, headers, body = self.read_request()
            status, response = self.server.application.handle(
                method, path, headers, body, self.client_address[0])
            self.respond(status, response)
        except ApiError as error:
            self.respond(error.status, {"error": {"code": error.code, "message": error.message}})
        except TimeoutError:
            self.respond(408, {"error": {"code": "invalid_request", "message": "Request timed out"}})
        except (OSError, UnicodeError):
            pass
        except sqlite3.Error:
            self.respond(503, {"error": {"code": "unavailable", "message": "Service temporarily unavailable"}})
        except Exception as error:
            logging.error("Account request failed (%s)", type(error).__name__)
            self.respond(500, {"error": {"code": "internal_error", "message": "Internal server error"}})

    def read_request(self):
        deadline = time.monotonic() + REQUEST_TIMEOUT
        buffered = bytearray()

        def receive():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError()
            self.request.settimeout(remaining)
            data = self.request.recv(4096)
            if not data:
                raise invalid("Truncated request")
            buffered.extend(data)

        while b"\r\n\r\n" not in buffered:
            if len(buffered) > MAX_HEADERS:
                raise invalid("Headers are too large")
            receive()
        end = buffered.index(b"\r\n\r\n") + 4
        if end > MAX_HEADERS:
            raise invalid("Headers are too large")
        try:
            lines = bytes(buffered[:end - 4]).decode("ascii").split("\r\n")
        except UnicodeError:
            raise invalid("Invalid HTTP headers") from None
        del buffered[:end]
        parts = lines[0].split(" ")
        if len(parts) != 3 or parts[2] not in ("HTTP/1.0", "HTTP/1.1"):
            raise invalid("Invalid request line")
        method, path, _ = parts
        headers = {}
        for line in lines[1:]:
            key, separator, value = line.partition(":")
            if (not separator or not HEADER_NAME.fullmatch(key) or key.lower() in headers or
                    any(ord(char) < 32 and char != "\t" for char in value)):
                raise invalid("Invalid HTTP header")
            headers[key.lower()] = value.strip()
        if "transfer-encoding" in headers or "expect" in headers:
            raise invalid("Unsupported HTTP framing")
        length = headers.get("content-length", "0")
        if not length.isascii() or not length.isdecimal() or len(length) > 10:
            raise invalid("Invalid content length")
        size = int(length)
        if size > MAX_BODY:
            raise invalid("Request body is too large", 413)
        if size and headers.get("content-type", "").split(";")[0].strip().lower() != "application/json":
            raise invalid("Content-Type must be application/json")
        if method in ("GET", "DELETE") and size:
            raise invalid("This request must not have a body")
        while len(buffered) < size:
            receive()
        if len(buffered) != size:
            raise invalid("Unexpected bytes after request")
        return method, path, headers, bytes(buffered)


class AccountServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    # server_close joins the bounded in-flight requests before process exit.
    daemon_threads = False
    request_queue_size = 16

    def __init__(self, address, application, max_connections, context):
        self.application = application
        self.store = application.store
        self.limiter = application.limiter
        self.trusted_proxies = application.trusted_proxies
        self.slots = threading.BoundedSemaphore(max_connections)
        self.context = context
        super().__init__(address, AccountHandler)

    def service_actions(self):
        self.application.expire(check_sessions=True)

    def trusted(self, address):
        return self.application.trusted(address)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            request.close()
            raise

    def process_request_thread(self, request, client_address):
        try:
            request.settimeout(5)
            if self.context:
                request = self.context.wrap_socket(request, server_side=True)
            self.finish_request(request, client_address)
        except (OSError, ValueError):
            pass
        finally:
            self.shutdown_request(request)
            self.slots.release()


class IPv6AccountServer(AccountServer):
    address_family = socket.AF_INET6


def create_server(host, port, database, *, session_ttl=7 * 24 * 60 * 60,
                  rate_limit=10, rate_window=60, registration_limit=5,
                  online_timeout=90, trusted_proxies=(), max_connections=16,
                  certificate=None, private_key=None, direct_min_bytes=0, stun_urls=()):
    if not 1 <= max_connections <= 128:
        raise ValueError("max_connections must be between 1 and 128")
    if bool(certificate) != bool(private_key):
        raise ValueError("certificate and private key must be supplied together")
    context = None
    if certificate:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(certificate, private_key)
        context.set_alpn_protocols(["http/1.1"])
    elif not ipaddress.ip_address(host).is_loopback:
        raise ValueError("public HTTP listeners are not allowed; use TLS or a loopback HTTPS proxy")
    application = AccountApplication(database, session_ttl=session_ttl,
                                     online_timeout=online_timeout, rate_limit=rate_limit,
                                     registration_limit=registration_limit,
                                     rate_window=rate_window, trusted_proxies=trusted_proxies,
                                     direct_min_bytes=direct_min_bytes, stun_urls=stun_urls)
    server_type = IPv6AccountServer if ":" in host else AccountServer
    return server_type((host, port), application, max_connections, context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--database", required=True)
    parser.add_argument("--session-ttl", type=int, default=7 * 24 * 60 * 60)
    parser.add_argument("--direct-min-bytes", type=int, default=0,
                        help="stream payloads smaller than this byte count use relay directly; 0 always tries direct")
    parser.add_argument("--stun-url", action="append", default=[],
                        help="STUN URL advertised to authenticated WebRTC peers; repeatable")
    parser.add_argument("--trusted-proxy", action="append", default=[],
                        help="trusted immediate reverse proxy IP/CIDR; repeatable")
    parser.add_argument("--certificate")
    parser.add_argument("--private-key")
    args = parser.parse_args()
    server = create_server(args.host, args.port, args.database,
                           session_ttl=args.session_ttl, trusted_proxies=args.trusted_proxy,
                           certificate=args.certificate, private_key=args.private_key,
                           direct_min_bytes=args.direct_min_bytes, stun_urls=args.stun_url)
    stop = threading.Event()

    def request_stop(signum, frame):
        stop.set()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    print(json.dumps({"host": server.server_address[0], "port": server.server_address[1]}), flush=True)
    server.timeout = 0.25
    try:
        while not stop.is_set():
            server.handle_request()
            server.service_actions()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
