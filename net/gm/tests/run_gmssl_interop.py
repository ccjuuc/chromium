# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Build a pinned independent GmSSL 3 peer and test the backend over TCP.

Windows x64 test only. Downloads/builds live under --work-dir and are never
Chromium product dependencies. The fixture directory contains generated test
private keys; it must never be populated with production certificates or Keys.
"""

import argparse
import hashlib
import json
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import tarfile
import threading
import urllib.request


VERSION = "3.2.0"
COMMIT = "7c9f02904ef33e59c87b4f16621cc8fd434e7579"
SHA256 = "68163f2098b1c996f4aa3c4ff7d439f9782e1b65f6343eeeb9b3863d73c5e391"
URL = f"https://codeload.github.com/guanzhi/GmSSL/tar.gz/{COMMIT}"
HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log):
    result = subprocess.run([str(value) for value in command], text=True,
                            encoding="utf-8", errors="replace",
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=600)
    log.write(subprocess.list2cmdline([str(value) for value in command]) + "\n")
    log.write(result.stdout + "\n")
    log.flush()
    if result.returncode:
        print(result.stdout[-6000:])
        result.check_returncode()
    return result.stdout


def source_tree(work):
    archive = work / f"gmssl-{VERSION}.tar.gz"
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=90) as response:
            archive.write_bytes(response.read(32 * 1024 * 1024))
    if digest(archive) != SHA256:
        raise ValueError("GmSSL archive checksum mismatch")
    source = work / f"GmSSL-{COMMIT}"
    with tarfile.open(archive) as bundle:
        if not source.exists():
            bundle.extractall(work, filter="data")
        # Also check reused sources: an old local patch must not silently turn
        # this into a test against a different independent implementation.
        for member in bundle.getmembers():
            if member.isfile():
                path = work / member.name
                archived = bundle.extractfile(member).read()
                if not path.is_file() or path.read_bytes() != archived:
                    raise ValueError("Modified GmSSL source: " + str(path))
    return source


def cmake_path(path):
    return '"' + str(path).replace("\\", "/").replace('"', '\\"') + '"'


def build_peer(work, source, cmake, log):
    project = work / "interop-project"
    project.mkdir(exist_ok=True)
    build = work / "interop-build"
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(GmInterop C)\n"
        "set(ENABLE_SKF OFF CACHE BOOL \"\" FORCE)\n"
        "set(ENABLE_SDF OFF CACHE BOOL \"\" FORCE)\n"
        f"add_subdirectory({cmake_path(source)} gmssl)\n"
        "get_directory_property(gm_definitions DIRECTORY "
        f"{cmake_path(source)} COMPILE_DEFINITIONS)\n"
        "add_executable(gmssl_interop_peer "
        f"{cmake_path(HERE / 'gmssl_interop_peer.c')})\n"
        "target_compile_definitions(gmssl_interop_peer PRIVATE ${gm_definitions})\n"
        "target_compile_options(gmssl_interop_peer PRIVATE /utf-8)\n"
        "target_include_directories(gmssl_interop_peer PRIVATE "
        f"{cmake_path(source / 'include')})\n"
        "target_link_libraries(gmssl_interop_peer PRIVATE gmssl ws2_32)\n"
        "add_custom_command(TARGET gmssl_interop_peer POST_BUILD\n"
        " COMMAND ${CMAKE_COMMAND} -E copy_if_different\n"
        " $<TARGET_FILE:gmssl> $<TARGET_FILE_DIR:gmssl_interop_peer>)\n",
        encoding="utf-8")
    run([cmake, "-S", project, "-B", build, "-G", "Visual Studio 17 2022",
         "-A", "x64"], log)
    run([cmake, "--build", build, "--config", "Release", "--target",
         "gmssl_interop_peer", "--parallel", "4"], log)
    return build / "Release/gmssl_interop_peer.exe"


def test_case(peer, smoke, backend, fixture, protocol, mutual, log):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    chain = fixture / ("tlcp-chain.pem" if protocol == "tlcp" else "server-sign.pem")
    keys = fixture / ("tlcp-keys.pem" if protocol == "tlcp" else "server-sign-gmssl.pem")
    command = [str(peer), str(port), protocol, str(chain), str(keys),
               "mutual" if mutual else "server-auth"]
    if mutual:
        command += [str(fixture / "server-root.pem")]
    server = subprocess.Popen(command, text=True, encoding="utf-8", errors="replace",
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = []
    ready = queue.Queue()

    def read_output():
        for line in server.stdout:
            output.append(line)
            if line.startswith("READY "):
                ready.put(True)
        ready.put(False)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    try:
        if not ready.get(timeout=30):
            raise RuntimeError("Independent peer failed before listening")
        client = [smoke, "--fetch", backend, port, protocol, fixture / "server-root.der"]
        if mutual:
            client += [fixture]
        result = run(client, log)
        if server.wait(timeout=30) != 0:
            raise RuntimeError("Independent peer rejected the connection")
        print(result.strip())
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        reader.join(timeout=10)
        log.write("".join(output))
        log.flush()
        if server.returncode:
            print("".join(output)[-6000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--test-peer", type=Path, required=True)
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # A failed bootstrap/build must not leave a previous run's report looking
    # like evidence for the newly requested executable.
    (work / "interop-report.json").unlink(missing_ok=True)
    if not args.cmake:
        raise ValueError("CMake with Visual Studio 2022 x64 is required")
    backend, smoke = args.backend.resolve(), args.test_peer.resolve()
    source = source_tree(work)
    with (work / "interop.log").open("w", encoding="utf-8") as log:
        peer = build_peer(work, source, args.cmake, log)
        fixture = work / "fixtures"
        run([smoke, "--export-interop-fixtures", fixture], log)
        for role in ("sign", "enc"):
            run([peer, "--convert", fixture / f"server-{role}-key.pem",
                 fixture / f"server-{role}-gmssl.pem"], log)
        (fixture / "tlcp-chain.pem").write_bytes(
            (fixture / "server-sign.pem").read_bytes() +
            (fixture / "server-enc.pem").read_bytes())
        (fixture / "tlcp-keys.pem").write_bytes(
            (fixture / "server-sign-gmssl.pem").read_bytes() +
            (fixture / "server-enc-gmssl.pem").read_bytes())
        results = []
        for protocol in ("tlcp", "rfc8998"):
            for mutual in (False, True):
                result = {"protocol": protocol, "mutual": mutual}
                try:
                    test_case(peer, smoke, backend, fixture, protocol, mutual, log)
                    result["passed"] = True
                except (subprocess.SubprocessError, RuntimeError, queue.Empty) as exc:
                    result.update(passed=False, error=str(exc))
                results.append(result)
    report = {"gmssl_version": VERSION, "gmssl_commit": COMMIT,
              "archive_sha256": SHA256, "backend_sha256": digest(backend),
              "peer_sha256": digest(peer), "tests": results,
              "fixture_profile": "Four-octet serials; optional end-entity BasicConstraints omitted",
              "tlcp_client_profile": "Signing and encryption certificates both transmitted"}
    (work / "interop-report.json").write_text(json.dumps(report, indent=2) + "\n")
    passed = sum(result["passed"] for result in results)
    print(f"Independent GmSSL 3 TCP interoperability: {passed}/4 passed; see interop-report.json and interop.log")
    return 0 if passed == 4 else 1


if __name__ == "__main__":
    sys.exit(main())
