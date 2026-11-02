#!/usr/bin/env python3
"""Account and own-device registry for CrossTransfer.

The default listener is private HTTP on loopback, intended to sit behind an
HTTPS reverse proxy. Public listeners require TLS certificate/key arguments.
No passwords, session tokens, request bodies, or authorization headers are logged.
Same-account invitations use an optional in-process relay for accepted files.
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
# A 16 KiB text may occupy six bytes per character after JSON escaping.
MAX_BODY = 128 * 1024
MAX_DEVICES = 32
MAX_DEVICE_RECORDS = 256
MAX_SESSIONS = 32
MAX_TRANSFERS = 32
MAX_TEXT_BYTES = 16 * 1024
MAX_FILE_SIZE = 8 * 1024 * 1024 * 1024
INVITATION_TTL = 5 * 60
FILE_TRANSFER_TTL = 30 * 60
REQUEST_TIMEOUT = 10
USERNAME = re.compile(r"[A-Za-z0-9_.-]{3,64}\Z")
DEVICE_ID = re.compile(r"[0-9a-f]{32}\Z")
TOKEN = re.compile(r"[0-9a-f]{64}\Z")
PLATFORM = re.compile(r"[A-Za-z0-9_.-]{1,32}\Z")
HEADER_NAME = re.compile(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+\Z")
DEVICE_ROUTE = re.compile(r"/api/v1/devices/([0-9a-f]{32})(/heartbeat)?\Z")
DEVICE_TRANSFERS_ROUTE = re.compile(r"/api/v1/devices/([0-9a-f]{32})/transfers\Z")
TRANSFER_ROUTE = re.compile(r"/api/v1/transfers/([0-9a-f]{32})/(accept|reject|cancel|complete|relay)\Z")


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


def _transfer_input(body):
    data = _json_body(body, ("senderDeviceId", "receiverDeviceId", "kind",
                             "text", "fileName", "fileSize"))
    sender, receiver = data["senderDeviceId"], data["receiverDeviceId"]
    if (not isinstance(sender, str) or not DEVICE_ID.fullmatch(sender) or
            not isinstance(receiver, str) or not DEVICE_ID.fullmatch(receiver) or sender == receiver):
        raise invalid("Select a different registered device")
    kind, text, name, size = (data[key] for key in ("kind", "text", "fileName", "fileSize"))
    if not isinstance(text, str) or not isinstance(name, str) or type(size) is not int:
        raise invalid("Invalid transfer metadata")
    try:
        text_bytes, name_bytes = len(text.encode("utf-8")), len(name.encode("utf-8"))
    except UnicodeError:
        raise invalid("Transfer metadata must be valid UTF-8") from None
    if kind == "text":
        if not 1 <= text_bytes <= MAX_TEXT_BYTES or "\0" in text or name or size:
            raise invalid("Text must contain 1 to 16384 UTF-8 bytes")
    elif kind == "file":
        if (text or not 1 <= name_bytes <= 255 or not 0 <= size <= MAX_FILE_SIZE or
                name in (".", "..") or name != name.strip() or name.endswith(".") or
                any(char in '/\\<>:"|?*' or ord(char) < 32 or ord(char) == 127 for char in name)):
            raise invalid("Invalid file name or size")
        if name.split(".", 1)[0].upper() in (
                {"CON", "PRN", "AUX", "NUL"} |
                {prefix + str(number) for prefix in ("COM", "LPT") for number in range(1, 10)}):
            raise invalid("Reserved file name")
    else:
        raise invalid("Unsupported transfer kind")
    return data


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
            if version not in (0, 1, 2):
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
                CREATE TABLE IF NOT EXISTS transfers (
                    id TEXT PRIMARY KEY,
                    account_id TEXT NOT NULL REFERENCES accounts(id),
                    sender_device TEXT NOT NULL REFERENCES devices(id),
                    receiver_device TEXT NOT NULL REFERENCES devices(id),
                    sender_name TEXT NOT NULL,
                    receiver_name TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    text TEXT NOT NULL,
                    file_name TEXT NOT NULL,
                    file_size INTEGER NOT NULL,
                    status TEXT NOT NULL,
                    created_at REAL NOT NULL,
                    expires_at REAL NOT NULL,
                    sender_session TEXT NOT NULL,
                    receiver_session TEXT NOT NULL DEFAULT ''
                );
                CREATE INDEX IF NOT EXISTS transfers_account ON transfers(account_id, created_at);
                CREATE INDEX IF NOT EXISTS transfers_sender ON transfers(sender_device, status);
                CREATE INDEX IF NOT EXISTS transfers_receiver ON transfers(receiver_device, status);
                PRAGMA user_version=2;
            """)
            # Relay leases and their secrets are deliberately process-local.
            # Pending invitations survive restarts; active file jobs must retry
            # with a new invitation instead of reusing stale room credentials.
            connection.execute("UPDATE transfers SET status='failed' WHERE status='accepted'")
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
            participants = (digest, digest, account_id, device_id, device_id)
            rows = connection.execute("""SELECT id FROM transfers
                WHERE sender_session=? OR receiver_session=? OR
                (account_id=? AND (sender_device=? OR receiver_device=?))""",
                                      participants).fetchall()
            connection.execute("""UPDATE transfers SET status='cancelled'
                WHERE status IN ('pending','accepted') AND
                (sender_session=? OR receiver_session=? OR
                 (account_id=? AND (sender_device=? OR receiver_device=?)))""", participants)
            connection.execute("DELETE FROM sessions WHERE token_hash=?",
                               (digest,))
        return [row["id"] for row in rows]

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
            rows = connection.execute("""SELECT id FROM transfers WHERE account_id=?
                AND (sender_device=? OR receiver_device=?)""",
                                      (account_id, device_id, device_id)).fetchall()
            connection.execute("""UPDATE transfers SET status='cancelled' WHERE account_id=?
                AND (sender_device=? OR receiver_device=?) AND status IN ('pending','accepted')""",
                               (account_id, device_id, device_id))
        return [row["id"] for row in rows]

    @staticmethod
    def transfer_value(row):
        return {"id": row["id"], "senderDeviceId": row["sender_device"],
                "receiverDeviceId": row["receiver_device"], "senderName": row["sender_name"],
                "receiverName": row["receiver_name"], "kind": row["kind"], "text": row["text"],
                "fileName": row["file_name"], "fileSize": row["file_size"],
                "status": row["status"], "createdAt": row["created_at"], "expiresAt": row["expires_at"]}

    @staticmethod
    def require_device(connection, account_id, device_id):
        row = connection.execute("SELECT * FROM devices WHERE id=? AND account_id=? AND revoked=0",
                                 (device_id, account_id)).fetchone()
        if row is None:
            raise ApiError(404, "not_found", "Device not found")
        return row

    @staticmethod
    def expire_transfers(connection):
        connection.execute("""UPDATE transfers SET status='expired'
            WHERE status IN ('pending','accepted') AND expires_at<=?""", (time.time(),))

    def create_transfer(self, account_id, token, data):
        now = time.time()
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self.expire_transfers(connection)
            sender = self.require_device(connection, account_id, data["senderDeviceId"])
            receiver = self.require_device(connection, account_id, data["receiverDeviceId"])
            if now - sender["last_seen"] > self.online_timeout or now - receiver["last_seen"] > self.online_timeout:
                raise ApiError(409, "device_offline", "Both devices must be online")
            count = connection.execute("""SELECT COUNT(*) FROM transfers WHERE account_id=?
                AND status IN ('pending','accepted')""", (account_id,)).fetchone()[0]
            if count >= MAX_TRANSFERS:
                raise ApiError(429, "limit_exceeded", "Too many pending transfers")
            # Bound retained text/history per account as well as active jobs.
            connection.execute("""DELETE FROM transfers WHERE id IN (
                SELECT id FROM transfers WHERE account_id=? AND status NOT IN ('pending','accepted')
                ORDER BY created_at DESC, rowid DESC LIMIT -1 OFFSET 64)""", (account_id,))
            identity = secrets.token_hex(16)
            connection.execute("""INSERT INTO transfers VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                               (identity, account_id, sender["id"], receiver["id"],
                                sender["display_name"], receiver["display_name"], data["kind"],
                                data["text"], data["fileName"], data["fileSize"], "pending", now,
                                now + INVITATION_TTL, hashlib.sha256(token.encode("ascii")).hexdigest(), ""))
            row = connection.execute("SELECT * FROM transfers WHERE id=?", (identity,)).fetchone()
        return self.transfer_value(row)

    def list_transfers(self, account_id, device_id):
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self.require_device(connection, account_id, device_id)
            self.expire_transfers(connection)
            rows = connection.execute("""SELECT * FROM transfers WHERE account_id=?
                AND (sender_device=? OR receiver_device=?)
                ORDER BY (status IN ('pending','accepted')) DESC, created_at DESC, rowid DESC LIMIT 32""",
                                      (account_id, device_id, device_id)).fetchall()
        return [self.transfer_value(row) for row in rows]

    def get_transfer(self, account_id, transfer_id, device_id):
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self.expire_transfers(connection)
            self.require_device(connection, account_id, device_id)
            row = connection.execute("""SELECT * FROM transfers WHERE id=? AND account_id=?
                AND (sender_device=? OR receiver_device=?)""",
                                     (transfer_id, account_id, device_id, device_id)).fetchone()
        if row is None:
            raise ApiError(404, "not_found", "Transfer not found")
        return self.transfer_value(row)

    def act_on_transfer(self, account_id, token, transfer_id, device_id, action):
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            self.expire_transfers(connection)
            self.require_device(connection, account_id, device_id)
            row = connection.execute("""SELECT * FROM transfers WHERE id=? AND account_id=?
                AND (sender_device=? OR receiver_device=?)""",
                                     (transfer_id, account_id, device_id, device_id)).fetchone()
            if row is None:
                raise ApiError(404, "not_found", "Transfer not found")
            if action in ("accept", "reject", "complete") and device_id != row["receiver_device"]:
                raise ApiError(403, "forbidden", "Only the receiving device may perform this action")
            target = {"accept": "completed" if row["kind"] == "text" else "accepted",
                      "reject": "rejected", "cancel": "cancelled", "complete": "completed"}[action]
            if row["status"] == target:
                return self.transfer_value(row)
            allowed = row["status"] == "pending" if action in ("accept", "reject") else (
                row["status"] in ("pending", "accepted") if action == "cancel" else
                row["status"] == "accepted" and row["kind"] == "file")
            if not allowed:
                raise ApiError(409, "transfer_state", "Transfer is no longer available for this action")
            expiry = time.time() + FILE_TRANSFER_TTL if target == "accepted" else row["expires_at"]
            receiver_session = (hashlib.sha256(token.encode("ascii")).hexdigest()
                                if action == "accept" else row["receiver_session"])
            connection.execute("UPDATE transfers SET status=?, expires_at=?, receiver_session=? WHERE id=?",
                               (target, expiry, receiver_session, transfer_id))
            row = connection.execute("SELECT * FROM transfers WHERE id=?", (transfer_id,)).fetchone()
        return self.transfer_value(row)


class AccountApplication:
    """Reusable account API; the caller owns HTTP framing and TLS transport."""

    def __init__(self, database, *, session_ttl=7 * 24 * 60 * 60,
                 rate_limit=10, rate_window=60, registration_limit=5,
                 online_timeout=90, trusted_proxies=(), relay=None):
        self.store = AccountStore(database, session_ttl, online_timeout)
        self.limiter = RateLimiter(rate_limit, registration_limit, rate_window)
        self.trusted_proxies = tuple(ipaddress.ip_network(value) for value in trusted_proxies)
        self.relay = relay
        # Serialize device/session changes and invitation transitions with relay
        # provisioning/revocation. Reauthenticate mutations while holding it.
        # Relay operations never acquire this lock, avoiding reverse lock order.
        self.transfer_lock = threading.RLock()

    def revoke_rooms(self, identities):
        if self.relay is not None:
            for identity in identities:
                self.relay.revoke_dynamic("own_" + identity)

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
            if operation == "register":
                return 201, store.register(username, password)
            return 200, store.login(username, password)
        match = DEVICE_ROUTE.fullmatch(path)
        listing = DEVICE_TRANSFERS_ROUTE.fullmatch(path)
        transfer = TRANSFER_ROUTE.fullmatch(path)
        if (path not in ("/api/v1/auth/me", "/api/v1/auth/logout", "/api/v1/devices", "/api/v1/transfers")
                and not match and not listing and not transfer):
            raise ApiError(404, "not_found", "Endpoint not found")
        authorization = headers.get("authorization", "")
        token = authorization[7:] if authorization.startswith("Bearer ") else ""
        account = store.authenticate(token)
        if path == "/api/v1/auth/me" and method == "GET":
            return 200, {"account": account}
        if path == "/api/v1/auth/logout" and method == "POST":
            data = _json_body(body, (), optional=True, optional_fields=("deviceId",))
            device_id = data.get("deviceId")
            if "deviceId" in data and (not isinstance(device_id, str) or
                                       not DEVICE_ID.fullmatch(device_id)):
                raise invalid("Invalid device id")
            with self.transfer_lock:
                self.revoke_rooms(store.logout(token, device_id))
            return 200, {"ok": True}
        if path == "/api/v1/devices" and method == "GET":
            return 200, {"devices": store.list_devices(account["id"])}
        if match:
            device_id, heartbeat = match.groups()
            if not heartbeat and method == "PUT":
                fields = _device_input(body)
                with self.transfer_lock:
                    account = store.authenticate(token)
                    return 200, {"device": store.put_device(account["id"], device_id, *fields)}
            if not heartbeat and method == "DELETE":
                with self.transfer_lock:
                    account = store.authenticate(token)
                    self.revoke_rooms(store.revoke(account["id"], device_id))
                return 200, {"ok": True}
            if heartbeat and method == "POST":
                _json_body(body, (), optional=True)
                with self.transfer_lock:
                    account = store.authenticate(token)
                    return 200, {"device": store.heartbeat(account["id"], device_id)}
        if path == "/api/v1/transfers" and method == "POST":
            data = _transfer_input(body)
            with self.transfer_lock:
                account = store.authenticate(token)
                return 201, {"transfer": store.create_transfer(account["id"], token, data)}
        if listing and method == "GET":
            with self.transfer_lock:
                account = store.authenticate(token)
                return 200, {"transfers": store.list_transfers(account["id"], listing[1])}
        if transfer and method == "POST":
            data = _json_body(body, ("deviceId",))
            device_id = data["deviceId"]
            if not isinstance(device_id, str) or not DEVICE_ID.fullmatch(device_id):
                raise invalid("Invalid device ID")
            identity, action = transfer.groups()
            with self.transfer_lock:
                account = store.authenticate(token)
                current = store.get_transfer(account["id"], identity, device_id)
                if action == "relay":
                    if current["kind"] != "file" or current["status"] != "accepted":
                        raise ApiError(409, "transfer_state", "Transfer has not been accepted")
                    role = "sender" if device_id == current["senderDeviceId"] else "receiver"
                    room = self.relay.get_dynamic("own_" + identity, role) if self.relay else None
                    if room is None:
                        raise ApiError(410, "transfer_expired", "Transfer relay is no longer available")
                    return 200, {"relay": room}
                if action == "accept" and current["kind"] == "file" and self.relay is None:
                    raise ApiError(503, "unavailable", "File relay is unavailable")
                updated = store.act_on_transfer(account["id"], token, identity, device_id, action)
                if action == "accept" and updated["status"] == "accepted":
                    if not self.relay.provision_dynamic("own_" + identity, identity, updated["expiresAt"]):
                        # A bounded relay cannot reserve another room. Cancel
                        # this attempt instead of advertising a usable session.
                        store.act_on_transfer(account["id"], token, identity, device_id, "cancel")
                        raise ApiError(429, "limit_exceeded", "File relay capacity reached; send a new invitation")
                elif updated["status"] in ("cancelled", "rejected"):
                    self.revoke_rooms([identity])
                elif updated["status"] == "completed" and self.relay:
                    self.relay.finish_dynamic("own_" + identity)
                return 200, {"transfer": updated}
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
                  certificate=None, private_key=None):
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
                                     rate_window=rate_window, trusted_proxies=trusted_proxies)
    server_type = IPv6AccountServer if ":" in host else AccountServer
    return server_type((host, port), application, max_connections, context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--database", required=True)
    parser.add_argument("--session-ttl", type=int, default=7 * 24 * 60 * 60)
    parser.add_argument("--trusted-proxy", action="append", default=[],
                        help="trusted immediate reverse proxy IP/CIDR; repeatable")
    parser.add_argument("--certificate")
    parser.add_argument("--private-key")
    args = parser.parse_args()
    server = create_server(args.host, args.port, args.database,
                           session_ttl=args.session_ttl, trusted_proxies=args.trusted_proxy,
                           certificate=args.certificate, private_key=args.private_key)
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
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
