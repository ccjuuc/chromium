#!/usr/bin/env python3
"""Generate an isolated TLS fixture and exercise the real C++ HTTPS channel.

Usage: python3 tests/test_https_relay.py /absolute/path/to/test_https_relay
Only binds loopback. Requires openssl command for temporary test certificates.
"""

import http.client
import importlib.util
import json
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time


def command(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_certificates(directory):
    ca = directory / "ca.pem"
    key = directory / "ca.key"
    command("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=CrossTransfer Test CA", "-keyout", str(key), "-out", str(ca))
    config = directory / "server.ext"
    config.write_text("subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n", encoding="ascii")
    command("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost",
            "-keyout", str(directory / "server.key"), "-out", str(directory / "server.csr"))
    command("openssl", "x509", "-req", "-in", str(directory / "server.csr"), "-CA", str(ca),
            "-CAkey", str(key), "-CAcreateserial", "-days", "1", "-extfile", str(config),
            "-out", str(directory / "server.pem"))
    for role in ("sender", "receiver"):
        config.write_text(f"subjectAltName=DNS:{role}.test\nextendedKeyUsage=serverAuth,clientAuth\n", encoding="ascii")
        command("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={role}.test",
                "-keyout", str(directory / f"{role}.key"), "-out", str(directory / f"{role}.csr"))
        command("openssl", "x509", "-req", "-in", str(directory / f"{role}.csr"), "-CA", str(ca),
                "-CAkey", str(key), "-CAcreateserial", "-days", "1", "-extfile", str(config),
                "-out", str(directory / f"{role}.pem"))
    return ca


def main():
    module_path = pathlib.Path(__file__).resolve().parents[1] / "relay" / "server.py"
    spec = importlib.util.spec_from_file_location("cross_transfer_relay", module_path)
    relay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(relay)
    # GET is redelivered until explicitly acknowledged: a lost HTTP response
    # cannot silently consume a protocol frame.
    state = relay.RelayState({"unit": {"sender_token": "1" * 64, "receiver_token": "2" * 64}})
    generation = "a" * 32
    for role, lease in (("sender", "b" * 32), ("receiver", "c" * 32)):
        assert state.operate("unit", generation, role, lease, 0, "POST", "open", b"")[0] == 200
    frame = b"x" * 38
    assert state.operate("unit", generation, "sender", "b" * 32, 0, "POST", "frames", frame)[0] == 200
    first = state.operate("unit", generation, "receiver", "c" * 32, 0, "GET", "frames", b"")
    retry = state.operate("unit", generation, "receiver", "c" * 32, 0, "GET", "frames", b"")
    assert first == retry == (200, frame, 1)
    assert state.operate("unit", generation, "receiver", "c" * 32, 1, "GET", "frames", b"")[0] == 204
    for _ in range(relay.MAX_QUEUE_FRAMES):
        assert state.operate("unit", generation, "sender", "b" * 32, 0, "POST", "frames", frame)[0] == 200
    assert state.operate("unit", generation, "sender", "b" * 32, 0, "POST", "frames", frame)[0] == 429
    state.rooms["unit"].last_activity = time.monotonic() - 301
    with state.condition:
        state.expire()
    assert state.total_bytes == 0 and state.rooms["unit"].connection_id is None
    # A pending read must not cross into a newer connection generation when
    # reset/open/enqueue happen before its condition waiter runs again.
    for role, lease in (("sender", "b" * 32), ("receiver", "c" * 32)):
        assert state.operate("unit", generation, role, lease, 0, "POST", "open", b"")[0] == 200
    waiting = threading.Event()
    original_wait = state.condition.wait

    def tracked_wait(timeout=None):
        waiting.set()
        return original_wait(timeout)

    state.condition.wait = tracked_wait
    previous_read = []
    pending = threading.Thread(target=lambda: previous_read.append(
        state.operate("unit", generation, "receiver", "c" * 32, 0, "GET", "frames", b"")))
    pending.start()
    assert waiting.wait(1)
    with state.condition:
        for role, lease in (("sender", "b" * 32), ("receiver", "c" * 32)):
            assert state.operate("unit", generation, role, lease, 0, "DELETE", "open", b"")[0] == 200
        for role, lease in (("sender", "e" * 32), ("receiver", "f" * 32)):
            assert state.operate("unit", "d" * 32, role, lease, 0, "POST", "open", b"")[0] == 200
        assert state.operate("unit", "d" * 32, "sender", "e" * 32, 0, "POST", "frames", frame)[0] == 200
    pending.join(timeout=1)
    assert not pending.is_alive() and previous_read == [(410, b"")]
    state.condition.wait = original_wait
    with tempfile.TemporaryDirectory(prefix="cross_transfer_https_") as temp:
        directory = pathlib.Path(temp)
        ca = make_certificates(directory)
        credentials = {"integration": {"sender_token": "1" * 64, "receiver_token": "2" * 64},
                       "jobs": {"sender_token": "1" * 64, "receiver_token": "2" * 64},
                       "bounds": {"sender_token": "4" * 64, "receiver_token": "5" * 64}}
        for room in ("auto-unreachable", "auto-certificate", "auto-agreement", "auto-resume", "auto-policy"):
            credentials[room] = {"sender_token": "1" * 64, "receiver_token": "2" * 64}
        server = relay.create_server("127.0.0.1", 0, str(directory / "server.pem"),
                                     str(directory / "server.key"), credentials)
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
        thread.start()
        port = server.server_address[1]
        try:
            # The service rejects an oversized advertised frame before reading
            # any body, so this assertion does not allocate the advertised size.
            context = ssl.create_default_context(cafile=str(ca))
            connection = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=2)
            path = "/v1/rooms/bounds/" + "a" * 32 + "/sender/frames"
            connection.request("POST", path, body=b"", headers={
                "Authorization": "Bearer " + "4" * 64, "X-Relay-Lease": "a" * 32,
                "Content-Length": str(relay.MAX_FRAME + 1)})
            response = connection.getresponse()
            assert response.status == 413
            connection.close()
            subprocess.run([sys.argv[1], str(ca), str(port), str(directory)], check=True, timeout=45)
            if len(sys.argv) > 2:
                subprocess.run([sys.argv[2], str(directory)], check=True, timeout=20)
            if len(sys.argv) > 3:
                subprocess.run([sys.argv[3], str(ca), str(port), str(directory)],
                               check=True, timeout=45)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
    print("[PASS] HTTPS relay service integration")


if __name__ == "__main__":
    main()
