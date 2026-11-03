#!/usr/bin/env python3
"""Bounded RFC 5389 Binding service for ICE server-reflexive candidates.

This is a UDP address discovery service, not TURN or a data relay. It sends only
minimal successful Binding responses and never logs client addresses or data.
"""

import collections
import ipaddress
import socket
import struct
import threading
import time
import zlib


MAGIC_COOKIE = 0x2112A442
MAX_PACKET = 1200
MAX_SOURCES = 4096


def binding_response(packet, address):
    if not 20 <= len(packet) <= MAX_PACKET:
        return None
    kind, length, cookie = struct.unpack_from("!HHI", packet)
    if kind != 0x0001 or cookie != MAGIC_COOKIE or length % 4 or length != len(packet) - 20:
        return None
    offset = 20
    while offset < len(packet):
        if len(packet) - offset < 4:
            return None
        attribute, size = struct.unpack_from("!HH", packet, offset)
        end = offset + 4 + size
        padded_end = offset + 4 + ((size + 3) & ~3)
        if padded_end > len(packet):
            return None
        # No credentials are provisioned for this public Binding service. Do
        # not pretend to validate MESSAGE-INTEGRITY or unsupported mandatory
        # extensions. Optional extensions such as SOFTWARE can be ignored.
        if attribute < 0x8000:
            return None
        if attribute == 0x8028:
            if size != 4 or end != len(packet):
                return None
            fingerprint = struct.unpack_from("!I", packet, offset + 4)[0]
            if fingerprint != (zlib.crc32(packet[:offset]) ^ 0x5354554E):
                return None
        offset = padded_end
    try:
        source = ipaddress.ip_address(address[0])
    except ValueError:
        return None
    if source.is_unspecified or source.is_multicast or not 1 <= address[1] <= 65535:
        return None
    if isinstance(source, ipaddress.IPv6Address) and source.ipv4_mapped:
        source = source.ipv4_mapped
    transaction = packet[8:20]
    mask = struct.pack("!I", MAGIC_COOKIE) + transaction
    mapped = bytes(value ^ mask[index] for index, value in enumerate(source.packed))
    value = struct.pack("!BBH", 0, 1 if source.version == 4 else 2,
                        address[1] ^ (MAGIC_COOKIE >> 16)) + mapped
    attribute = struct.pack("!HH", 0x0020, len(value)) + value
    return struct.pack("!HHI", 0x0101, len(attribute), MAGIC_COOKIE) + transaction + attribute


class RateLimiter:
    """Single-thread token buckets with bounded source memory."""

    def __init__(self, now=None):
        self.last = time.monotonic() if now is None else now
        self.tokens = 400.0
        self.sources = collections.OrderedDict()

    def allow(self, source, now=None):
        now = time.monotonic() if now is None else now
        self.tokens = min(400.0, self.tokens + max(0, now - self.last) * 200)
        self.last = now
        if self.tokens < 1:
            return False
        tokens, last = self.sources.pop(source, (40.0, now))
        tokens = min(40.0, tokens + max(0, now - last) * 20)
        allowed = tokens >= 1
        self.sources[source] = (tokens - 1 if allowed else tokens, now)
        if len(self.sources) > MAX_SOURCES:
            self.sources.popitem(last=False)
        if allowed:
            self.tokens -= 1
        return allowed


class StunListener:
    def __init__(self, host, port):
        self.stop = threading.Event()
        self.socket = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET,
                                    socket.SOCK_DGRAM)
        try:
            self.socket.bind((host, port))
            self.socket.settimeout(0.2)
        except BaseException:
            self.socket.close()
            raise
        self.server_address = self.socket.getsockname()
        self.limiter = RateLimiter()
        self.thread = threading.Thread(target=self.run, name="cross-transfer-stun", daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                packet, address = self.socket.recvfrom(MAX_PACKET + 1)
                response = binding_response(packet, address)
                if response is not None and self.limiter.allow(address[0]):
                    self.socket.sendto(response, address)
            except socket.timeout:
                continue
            except OSError:
                # Oversized UDP datagrams and transient network errors are
                # dropped. Closing the socket also wakes this loop on Windows.
                continue

    def close(self):
        self.stop.set()
        self.socket.close()
        self.thread.join(timeout=1)
