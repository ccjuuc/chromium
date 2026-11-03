#!/usr/bin/env python3
"""Validate the UDP Binding wire format, bounded parsing, and listener lifetime."""

import importlib.util
from pathlib import Path
import socket
import struct
import unittest
import zlib


spec = importlib.util.spec_from_file_location("cross_transfer_stun",
                                            Path(__file__).resolve().parents[1] / "relay/stun.py")
stun = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stun)


class StunTest(unittest.TestCase):
    transaction = bytes.fromhex("b7e7a701bc34d686fa87dfae")

    def request(self, attributes=b"", kind=0x0001, cookie=0x2112A442):
        return struct.pack("!HHI", kind, len(attributes), cookie) + self.transaction + attributes

    def test_ipv4_xor_mapping_and_transaction_echo(self):
        response = stun.binding_response(self.request(), ("192.0.2.1", 32853))
        # RFC 5769's address/port encodings, with the minimal response header.
        self.assertEqual(response, bytes.fromhex(
            "0101000c2112a442b7e7a701bc34d686fa87dfae002000080001a147e112a643"))
        self.assertEqual(stun.binding_response(self.request(), ("::ffff:192.0.2.1", 32853)), response)

    def test_ipv6_xor_mapping_uses_cookie_and_transaction(self):
        response = stun.binding_response(self.request(), ("2001:db8:1234:5678:11:2233:4455:6677", 32853))
        self.assertEqual(response, bytes.fromhex(
            "010100182112a442b7e7a701bc34d686fa87dfae002000140002a147"
            "0113a9faa5d3f179bc25f4b5bed2b9d9"))

    def test_bad_types_lengths_cookie_and_attributes_are_silent(self):
        packet = self.request()
        malformed = [b"", packet[:19], packet + b"\0", self.request(kind=0x0101),
                     self.request(kind=0x0011), self.request(cookie=0),
                     self.request(bytes(1200)), self.request(b"\x80\x22\0\x08abcd"),
                     self.request(b"\x80\x22\0\x01a"),
                     self.request(b"\x00\x06\0\x04user")]
        for value in malformed:
            with self.subTest(packet=value[:24].hex()):
                self.assertIsNone(stun.binding_response(value, ("192.0.2.1", 32853)))
        self.assertIsNone(stun.binding_response(packet, ("0.0.0.0", 10)))
        self.assertIsNone(stun.binding_response(packet, ("224.0.0.1", 10)))
        self.assertIsNone(stun.binding_response(packet, ("192.0.2.1", 0)))

    def test_optional_attributes_padding_and_fingerprint(self):
        software = b"\x80\x22\0\x03abc\0"
        prefix = self.request(software + b"\x80\x28\0\x04" + bytes(4))[:-8]
        packet = prefix + b"\x80\x28\0\x04" + struct.pack("!I", zlib.crc32(prefix) ^ 0x5354554E)
        self.assertIsNotNone(stun.binding_response(packet, ("192.0.2.1", 1234)))
        self.assertIsNotNone(stun.binding_response(self.request(software), ("192.0.2.1", 1234)))
        self.assertIsNone(stun.binding_response(packet[:-1] + bytes([packet[-1] ^ 1]),
                                               ("192.0.2.1", 1234)))
        self.assertIsNone(stun.binding_response(self.request(b"\x80\x28\0\0"), ("192.0.2.1", 1234)))

    def test_per_source_global_rate_and_memory_bounds(self):
        limiter = stun.RateLimiter(now=0)
        self.assertTrue(all(limiter.allow("one", now=0) for _ in range(40)))
        self.assertFalse(limiter.allow("one", now=0))
        self.assertTrue(limiter.allow("one", now=0.05))
        self.assertFalse(limiter.allow("one", now=0.05))
        limiter = stun.RateLimiter(now=0)
        self.assertTrue(all(limiter.allow(str(index), now=0) for index in range(400)))
        self.assertFalse(limiter.allow("extra", now=0))
        self.assertTrue(limiter.allow("extra", now=0.01))
        for index in range(2 * stun.MAX_SOURCES):
            self.assertTrue(limiter.allow(str(index), now=index + 1))
        self.assertEqual(len(limiter.sources), stun.MAX_SOURCES)

    def test_actual_udp_mapping_and_clean_close(self):
        listener = stun.StunListener("127.0.0.1", 0)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.bind(("127.0.0.1", 0))
                client.settimeout(2)
                client.sendto(self.request(), listener.server_address)
                response, source = client.recvfrom(1200)
                self.assertEqual(source, listener.server_address)
                self.assertEqual(response, stun.binding_response(self.request(), client.getsockname()))
                client.sendto(self.request(kind=0x0101), listener.server_address)
                client.settimeout(0.1)
                with self.assertRaises(socket.timeout):
                    client.recvfrom(1200)
        finally:
            listener.close()
        self.assertFalse(listener.thread.is_alive())
        listener.close()


if __name__ == "__main__":
    unittest.main()
