# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Prepare a pinned x64 Tongsuo build without changing the Chromium toolchain.

Run explicitly before GN generation; GN/Ninja never downloads dependencies.
An existing Chromium output directory supplies environment.x64. The runtime
backend links *_static.lib into one DLL. Shared libraries are built only for
the separate test peer and are not backend runtime dependencies.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import urllib.request
import zipfile


HERE = Path(__file__).resolve().parent
CONFIGURE_ARGS = [
    "VC-WIN64A", "shared", "enable-ntls", "no-apps", "no-tests",
    "no-legacy", "no-quic", "no-ml-kem", "no-ml-dsa", "no-slh-dsa",
]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_environment(path):
    # Same Windows environment-block format as build/toolchain/win/tool_wrapper.
    return dict(item.split("=", 1)
                for item in path.read_text().rstrip("\0").split("\0"))


def extract_archive(archive, destination, root):
    with zipfile.ZipFile(archive) as bundle:
        for entry in bundle.infolist():
            parts = PurePosixPath(entry.filename).parts
            if (not parts or parts[0] != root or ".." in parts
                    or "\\" in entry.filename or ":" in entry.filename
                    or entry.filename.startswith("/")):
                raise ValueError("Archive contains an unexpected path")
            if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("Archive contains a symbolic link")
        bundle.extractall(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--environment", required=True, type=Path)
    parser.add_argument("--perl", required=True, type=Path)
    parser.add_argument("--nasm", required=True, type=Path)
    parser.add_argument("--make", type=Path, help="Optional jom.exe; defaults to nmake")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--archive", type=Path, help="Use a local pinned ZIP")
    parser.add_argument("--download", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32" or args.jobs < 1:
        parser.error("This bootstrap requires Windows and a positive job count")
    if bool(args.archive) == args.download:
        parser.error("Specify exactly one of --archive or --download")

    spec = json.loads((HERE / "source.json").read_text())
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = args.archive.resolve() if args.archive else output / "source.zip"
    if args.download:
        staging = archive.with_suffix(".download")
        urllib.request.urlretrieve(spec["url"], staging)
        if sha256(staging) != spec["sha256"]:
            raise ValueError("Downloaded source SHA-256 mismatch")
        staging.replace(archive)
    if sha256(archive) != spec["sha256"]:
        raise ValueError("Source archive SHA-256 mismatch")

    source = output / spec["archive_root"]
    marker = output / "source-verified.json"
    expected = {"sha256": spec["sha256"], "configure": CONFIGURE_ARGS}
    if source.exists():
        if not marker.exists() or json.loads(marker.read_text()) != expected:
            raise ValueError("Existing build directory has no matching provenance; "
                             "choose a new output directory")
    else:
        extract_archive(archive, output, spec["archive_root"])
        marker.write_text(json.dumps(expected, indent=2) + "\n")

    env = read_environment(args.environment.resolve())
    path_key = next(key for key in env if key.upper() == "PATH")
    env[path_key] = os.pathsep.join([
        str(args.perl.resolve().parent), str(args.nasm.resolve().parent), env[path_key]])
    env.update({"CL": "/FS", "LC_ALL": "C", "LC_CTYPE": "C", "LANG": "C"})
    make = (str(args.make.resolve()) if args.make
            else shutil.which("nmake.exe", path=env[path_key]))
    if not make:
        raise ValueError("nmake.exe is absent from the x64 build environment")
    with (output / "build.log").open("w", encoding="utf-8") as log:
        def run(command):
            log.write(subprocess.list2cmdline(command) + "\n")
            log.flush()
            subprocess.run(command, cwd=source, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        run([str(args.perl.resolve()), "Configure", *CONFIGURE_ARGS,
             "--prefix=" + str(output / "install")])
        parallel = ["-j", str(args.jobs)] if Path(make).stem.lower() == "jom" else []
        run([make, *parallel,
             "LDFLAGS=/nologo /INCREMENTAL:NO /OPT:REF /OPT:ICF", "build_libs"])

    outputs = ["libcrypto_static.lib", "libssl_static.lib",
               "libcrypto-3-x64.dll", "libssl-3-x64.dll"]
    manifest = dict(expected, version=spec["version"], source=str(source),
                    artifacts={name: sha256(source / name) for name in outputs})
    (output / "build-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Tongsuo x64 build verified: " + str(source))
    print("Manifest: " + str(output / "build-manifest.json"))


if __name__ == "__main__":
    main()
