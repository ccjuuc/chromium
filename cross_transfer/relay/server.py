#!/usr/bin/env python3
"""Bounded, authenticated HTTPS frame relay for CrossTransfer.

Both peers connect outbound. This reference service terminates TLS and is a
trusted party, not an end-to-end encrypted relay. Provision room credentials
through a separate authenticated control plane; never put tokens in URLs.
"""

import argparse
import collections
import hmac
import importlib.util
import json
from pathlib import Path
import re
import secrets
import socket
import socketserver
import ssl
import threading
import time

MAX_PAYLOAD = 4 * 1024 * 1024
MAX_FRAME = MAX_PAYLOAD + 38
MAX_HEADERS = 8192
MAX_QUEUE_BYTES = 8 * 1024 * 1024 + 76
MAX_GLOBAL_BYTES = 32 * 1024 * 1024
MAX_QUEUE_FRAMES = 64
MAX_CONNECTIONS = 16
# Includes worst-case JSON escaping for a 16 KiB text invitation.
MAX_ACCOUNT_BODY = 128 * 1024
MAX_DYNAMIC_ROOMS = 64
ROOM = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
HEX32 = re.compile(r"[0-9a-fA-F]{32}\Z")
HEX64 = re.compile(r"[0-9a-fA-F]{64}\Z")
ROUTE = re.compile(r"/v1/rooms/([A-Za-z0-9_-]{1,64})/([0-9a-fA-F]{32})/(sender|receiver)/(open|frames)\Z")


class RoomState:
    def __init__(self, credentials, expires_at=None, fixed_connection_id=None):
        self.credentials = credentials
        self.expires_at = expires_at
        self.fixed_connection_id = fixed_connection_id
        self.finishing = False
        self.reset()

    def reset(self):
        self.connection_id = None
        self.leases = {}
        self.closed = set()
        self.queues = {role: collections.deque() for role in ("sender", "receiver")}
        self.sizes = {"sender": 0, "receiver": 0}
        self.next_sequence = {"sender": 1, "receiver": 1}
        self.acknowledged = {"sender": 0, "receiver": 0}
        self.last_activity = time.monotonic()


class RelayState:
    def __init__(self, credentials, idle_timeout=60):
        if not credentials or len(credentials) > 64:
            raise ValueError("configure between 1 and 64 rooms")
        for room, roles in credentials.items():
            if not ROOM.fullmatch(room) or set(roles) != {"sender_token", "receiver_token"}:
                raise ValueError("invalid room configuration")
            if any(not isinstance(token, str) or not HEX64.fullmatch(token) for token in roles.values()):
                raise ValueError("role tokens must be 256-bit hex strings")
            if hmac.compare_digest(roles["sender_token"], roles["receiver_token"]):
                raise ValueError("sender and receiver must have distinct credentials")
        self.rooms = {name: RoomState(roles) for name, roles in credentials.items()}
        self.condition = threading.Condition()
        self.idle_timeout = max(1, min(idle_timeout, 300))
        self.total_bytes = 0

    def expire(self):
        now = time.monotonic()
        expired = []
        for name, room in self.rooms.items():
            if room.expires_at is not None and time.time() >= room.expires_at:
                self.total_bytes -= sum(room.sizes.values())
                expired.append(name)
                continue
            if room.connection_id and now - room.last_activity >= self.idle_timeout:
                self.total_bytes -= sum(room.sizes.values())
                if room.expires_at is not None:
                    expired.append(name)
                else:
                    room.reset()
        for name in expired:
            del self.rooms[name]
        self.condition.notify_all()

    def authorized(self, name, role, authorization):
        with self.condition:
            self.expire()
            return self._authorized(name, role, authorization)

    def _authorized(self, name, role, authorization):
        room = self.rooms.get(name)
        # Responses never disclose whether a room exists.
        expected = "Bearer " + (room.credentials[role + "_token"] if room else "0" * 64)
        return room is not None and hmac.compare_digest(authorization, expected)

    def provision_dynamic(self, name, connection_id, expires_at):
        with self.condition:
            self.expire()
            existing = self.rooms.get(name)
            if existing:
                return (existing.expires_at is not None and not existing.finishing and
                        existing.fixed_connection_id == connection_id)
            if (sum(room.expires_at is not None for room in self.rooms.values()) >= MAX_DYNAMIC_ROOMS or
                    expires_at <= time.time()):
                return False
            self.rooms[name] = RoomState(
                {"sender_token": secrets.token_hex(32), "receiver_token": secrets.token_hex(32)},
                expires_at, connection_id)
            return True

    def get_dynamic(self, name, role):
        with self.condition:
            self.expire()
            room = self.rooms.get(name)
            if room is None or room.expires_at is None or room.finishing:
                return None
            return {"roomId": name, "connectionId": room.fixed_connection_id,
                    "bearerToken": room.credentials[role + "_token"], "expiresAt": room.expires_at}

    def revoke_dynamic(self, name):
        with self.condition:
            room = self.rooms.get(name)
            if room is not None and room.expires_at is not None:
                self.total_bytes -= sum(room.sizes.values())
                del self.rooms[name]
                self.condition.notify_all()

    def finish_dynamic(self, name):
        with self.condition:
            room = self.rooms.get(name)
            if room is not None and room.expires_at is not None:
                # The sender may still be reading the receiver's final ACK.
                # Permit only existing leases to drain, bounded to 15 seconds.
                room.finishing = True
                room.expires_at = min(room.expires_at, time.time() + 15)
                self.condition.notify_all()

    def operate(self, name, connection_id, role, lease, acknowledgement, method, operation, body,
                authorization=None):
        peer = "receiver" if role == "sender" else "sender"
        with self.condition:
            self.expire()
            # Re-check under the operation lock after the handler read the
            # body: cancellation may have revoked a room in that interval.
            if authorization is not None and not self._authorized(name, role, authorization):
                return 401, b""
            room = self.rooms.get(name)
            if room is None:
                return 410, b""
            if room.fixed_connection_id and room.fixed_connection_id != connection_id:
                return 409, b""
            if operation == "open" and method == "POST":
                if room.finishing:
                    return 410, b""
                if room.connection_id is None:
                    room.connection_id = connection_id
                if room.connection_id != connection_id or role in room.closed:
                    return 409, b""
                if role in room.leases and room.leases[role] != lease:
                    return 409, b""
                room.leases[role] = lease
                room.last_activity = time.monotonic()
                return 200, b""
            if room.connection_id != connection_id or room.leases.get(role) != lease or role in room.closed:
                return 410, b""
            room.last_activity = time.monotonic()
            if acknowledgement > room.acknowledged[role]:
                if not room.queues[role] or room.queues[role][0][0] != acknowledgement:
                    return 400, b""
                _, acknowledged_body = room.queues[role].popleft()
                room.sizes[role] -= len(acknowledged_body)
                self.total_bytes -= len(acknowledged_body)
                room.acknowledged[role] = acknowledgement
            if operation == "open" and method == "DELETE":
                room.closed.add(role)
                # Preserve queued final ACKs for the remaining reader.
                if len(room.closed) == 2:
                    self.total_bytes -= sum(room.sizes.values())
                    if room.expires_at is not None:
                        del self.rooms[name]
                    else:
                        room.reset()
                self.condition.notify_all()
                return 200, b""
            if operation == "frames" and method == "POST":
                if room.finishing:
                    return 410, b""
                if peer in room.closed:
                    return 410, b""
                if (len(room.queues[peer]) >= MAX_QUEUE_FRAMES or
                    room.sizes[peer] + len(body) > MAX_QUEUE_BYTES or
                    self.total_bytes + len(body) > MAX_GLOBAL_BYTES):
                    return 429, b""
                sequence = room.next_sequence[peer]
                room.next_sequence[peer] += 1
                room.queues[peer].append((sequence, body))
                room.sizes[peer] += len(body)
                self.total_bytes += len(body)
                self.condition.notify_all()
                return 200, b""
            if operation == "frames" and method == "GET":
                deadline = time.monotonic() + 0.1
                while True:
                    # The condition wait releases the lock: this room may have
                    # been reset and leased to a newer connection meanwhile.
                    if (self.rooms.get(name) is not room or
                        (room.expires_at is not None and time.time() >= room.expires_at) or
                        room.connection_id != connection_id or
                        room.leases.get(role) != lease or role in room.closed):
                        return 410, b""
                    if room.queues[role]:
                        break
                    if peer in room.closed:
                        return 410, b""
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return 204, b""
                    self.condition.wait(remaining)
                # A response can time out after this request selected a frame.
                # Retain it until the client explicitly acknowledges full receipt.
                sequence, data = room.queues[role][0]
                return 200, data, sequence
            return 405, b""


class RelayHandler(socketserver.BaseRequestHandler):
    def respond(self, status, body=b"", sequence=None,
                content_type="application/octet-stream"):
        reasons = {200: "OK", 201: "Created", 204: "No Content", 400: "Bad Request",
                   401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
                   405: "Method Not Allowed", 408: "Request Timeout", 409: "Conflict",
                   410: "Gone", 413: "Content Too Large", 429: "Too Many Requests",
                   500: "Internal Server Error", 502: "Bad Gateway",
                   503: "Service Unavailable"}
        sequence_header = f"X-Relay-Sequence: {sequence}\r\n" if sequence is not None else ""
        response = (f"HTTP/1.1 {status} {reasons.get(status, 'Error')}\r\n"
                    f"Content-Length: {len(body)}\r\n"
                    f"{sequence_header}"
                    f"Content-Type: {content_type}\r\n"
                    "Cache-Control: no-store\r\nConnection: close\r\n\r\n").encode("ascii")
        self.request.sendall(response + body)

    def handle(self):
        try:
            self.handle_request()
        except (OSError, ValueError, UnicodeError):
            # No request, URL, or credential logging. TLS/socket timeouts simply
            # abort this request; the bounded lease TTL cleans abandoned rooms.
            pass

    def handle_request(self):
        deadline = time.monotonic() + 10
        buffered = bytearray()

        def receive():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("request deadline")
            self.request.settimeout(remaining)
            data = self.request.recv(4096)
            if not data:
                raise ValueError("truncated request")
            buffered.extend(data)

        def read_line():
            while b"\r\n" not in buffered:
                if len(buffered) > MAX_HEADERS:
                    raise ValueError("invalid HTTP line")
                receive()
            end = buffered.index(b"\r\n") + 2
            if end > MAX_HEADERS:
                raise ValueError("invalid HTTP line")
            line = bytes(buffered[:end])
            del buffered[:end]
            return line

        first = read_line()
        parts = first[:-2].decode("ascii").split(" ")
        if len(parts) != 3 or parts[2] != "HTTP/1.1":
            self.respond(400)
            return
        method, path, _ = parts
        headers = {}
        total_headers = len(first)
        while True:
            line = read_line()
            total_headers += len(line)
            if total_headers > MAX_HEADERS:
                self.respond(400)
                return
            if line == b"\r\n":
                break
            key, separator, value = line[:-2].decode("ascii").partition(":")
            if not separator or not key or key[0].isspace() or key.lower() in headers:
                self.respond(400)
                return
            headers[key.lower()] = value.strip()
        if path.startswith("/api/v1/"):
            self.handle_account(method, path, headers, buffered, receive)
            return
        match = ROUTE.fullmatch(path)
        if not match or method not in ("GET", "POST", "DELETE"):
            self.respond(400)
            return
        name, connection_id, role, operation = match.groups()
        if not self.server.state.authorized(name, role, headers.get("authorization", "")):
            self.respond(401)
            return
        lease = headers.get("x-relay-lease", "")
        acknowledgement = headers.get("x-relay-ack", "0")
        length = headers.get("content-length", "")
        if (not HEX32.fullmatch(lease) or not length.isascii() or not length.isdecimal() or
            len(length) > 10 or not acknowledgement.isascii() or not acknowledgement.isdecimal() or
            len(acknowledgement) > 20 or int(acknowledgement) > 2**64 - 1 or
            "transfer-encoding" in headers or "expect" in headers):
            self.respond(400)
            return
        size = int(length)
        if size > MAX_FRAME:
            self.respond(413)
            return
        if (method != "POST" or operation != "frames") and size != 0:
            self.respond(400)
            return
        while len(buffered) < size:
            receive()
        if len(buffered) != size:
            self.respond(400)
            return
        body = bytes(buffered)
        if operation == "frames" and method == "POST":
            # Relay transports bounded opaque frames; the shared C++ codec
            # owns command/version/checksum semantics at the endpoints.
            if len(body) < 38 or int.from_bytes(body[30:34], "big") != len(body) - 38:
                self.respond(400)
                return
        result = self.server.state.operate(
            name, connection_id, role, lease, int(acknowledgement), method, operation, body,
            headers.get("authorization", ""))
        self.request.settimeout(10)
        self.respond(*result)

    def handle_account(self, method, path, headers, buffered, receive):
        """Dispatch accounts in this same HTTPS process, with bounded input."""
        def respond(status, value):
            self.respond(status, json.dumps(value, ensure_ascii=False).encode("utf-8"),
                         content_type="application/json; charset=utf-8")

        def reject(status, message):
            respond(status, {"error": {"code": "invalid_request", "message": message}})

        if self.server.account_application is None:
            respond(404, {"error": {"code": "not_found", "message": "Endpoint not found"}})
            return
        if method not in ("GET", "POST", "PUT", "DELETE"):
            reject(405, "Method not allowed")
            return
        length = headers.get("content-length", "0")
        if (not length.isascii() or not length.isdecimal() or len(length) > 10 or
                "transfer-encoding" in headers or "expect" in headers):
            reject(400, "Unsupported HTTP framing")
            return
        size = int(length)
        if size > MAX_ACCOUNT_BODY:
            reject(413, "Request body is too large")
            return
        while len(buffered) < size:
            receive()
        if len(buffered) != size:
            reject(400, "Unexpected bytes after request")
            return
        status, value = self.server.account_application.handle(
            method, path, headers, bytes(buffered), self.client_address[0])
        self.request.settimeout(10)
        respond(status, value)


class RelayServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, context, state, account_application=None):
        self.context = context
        self.state = state
        self.account_application = account_application
        self.slots = threading.BoundedSemaphore(MAX_CONNECTIONS)
        super().__init__(address, RelayHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            request.settimeout(5)
            with self.context.wrap_socket(request, server_side=True) as secured:
                self.finish_request(secured, client_address)
        except OSError:
            request.close()
        finally:
            self.slots.release()

    def service_actions(self):
        with self.state.condition:
            self.state.expire()


class IPv6RelayServer(RelayServer):
    address_family = socket.AF_INET6


def create_server(host, port, certificate, private_key, credentials, idle_timeout=60,
                  account_database=None):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(certificate, private_key)
    context.set_alpn_protocols(["http/1.1"])
    application = None
    state = RelayState(credentials, idle_timeout)
    if account_database is not None:
        # Keep account storage reusable by standalone tests without requiring
        # installed packages when this relay is launched directly as a script.
        module_path = Path(__file__).resolve().parents[1] / "account_service" / "server.py"
        spec = importlib.util.spec_from_file_location("cross_transfer_account_service", module_path)
        account_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(account_module)
        application = account_module.AccountApplication(account_database, relay=state)
    server_type = IPv6RelayServer if ":" in host else RelayServer
    return server_type((host, port), context, state, application)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--certificate", required=True)
    parser.add_argument("--private-key", required=True)
    parser.add_argument("--credentials", required=True,
                        help="JSON file: room ID -> sender_token and receiver_token (distinct 64 hex strings)")
    parser.add_argument("--idle-timeout", type=int, default=60)
    parser.add_argument("--account-database",
                        help="enable /api/v1/ account/device APIs in this HTTPS process")
    args = parser.parse_args()
    with open(args.credentials, encoding="utf-8") as f:
        credentials = json.load(f)
    server = create_server(args.host, args.port, args.certificate, args.private_key,
                           credentials, args.idle_timeout, args.account_database)
    print(json.dumps({"host": server.server_address[0], "port": server.server_address[1]}), flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
