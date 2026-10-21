#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Generate and independently round-trip a Chromium Updater Puffin delta.

The .puff file is published only after native puffpatch reconstructs the exact
new CRX. `recommended` is false if the delta is not smaller than the full CRX;
release tooling must keep the full payload as its default in that case.
"""

import argparse
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from updater_packaging_common import publish_file, run, sha256, version


def check_crx(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Delta inputs must be regular CRX3 files")
    with path.open("rb") as source:
        prefix = source.read(12)
    if (len(prefix) != 12 or prefix[:4] != b"Cr24" or
            struct.unpack("<I", prefix[4:8])[0] != 3):
        raise ValueError("Delta inputs must use the CRX3 format")
    header_size = struct.unpack("<I", prefix[8:])[0]
    if not 0 < header_size <= 1024 * 1024 or 12 + header_size >= path.stat().st_size:
        raise ValueError("Invalid CRX3 input header length")


def files_equal(left, right):
    with left.open("rb") as first, right.open("rb") as second:
        while True:
            a, b = first.read(1024 * 1024), second.read(1024 * 1024)
            if a != b:
                return False
            if not a:
                return True


def build_delta(args, *, command=run):
    old = Path(args.old_crx).absolute()
    new = Path(args.new_crx).absolute()
    output = Path(args.out).absolute()
    tool = Path(args.puffin).absolute()
    previous_version = version(args.from_version)
    target_version = version(args.to_version)
    if tuple(map(int, target_version.split("."))) <= tuple(map(int, previous_version.split("."))):
        raise ValueError("Delta target version must be newer than its baseline")
    if output.resolve() in (old.resolve(), new.resolve()):
        raise ValueError("Output must not replace an input CRX")
    metadata_output = Path(args.metadata).absolute() if args.metadata else None
    if metadata_output and metadata_output.resolve() in (old.resolve(), new.resolve(), output.resolve()):
        raise ValueError("Metadata output must not replace a CRX or patch")
    check_crx(old)
    check_crx(new)
    with tempfile.TemporaryDirectory(prefix="xenon-updater-delta-") as temporary:
        stage = Path(temporary)
        # Puffin infers compressed input formats from these .crx3 extensions.
        baseline, target = stage / "old.crx3", stage / "new.crx3"
        shutil.copyfile(old, baseline)
        shutil.copyfile(new, target)
        check_crx(baseline)
        check_crx(target)
        old_hash, new_hash = sha256(baseline), sha256(target)
        patch, restored = stage / "update.puff", stage / "restored.crx3"
        command([tool, "-puffdiff", baseline, target, patch], cwd=stage)
        if not patch.is_file() or patch.stat().st_size == 0:
            raise ValueError("Puffin did not produce a patch")
        command([tool, "-puffpatch", baseline, restored, patch], cwd=stage)
        if (not restored.is_file() or sha256(restored) != new_hash or
                not files_equal(restored, target)):
            raise ValueError("Puffin round-trip did not reproduce the target CRX byte for byte")
        # Also reject an input rebuilt concurrently; metadata must describe the
        # release artifacts the caller supplied, not an obsolete snapshot.
        if sha256(old) != old_hash or sha256(new) != new_hash:
            raise ValueError("An input CRX changed during delta generation")
        smaller = patch.stat().st_size < target.stat().st_size
        metadata = {"from_version": previous_version, "to_version": target_version,
                    "previous_sha256": old_hash, "type": "puff", "file": output.name,
                    "size": patch.stat().st_size, "sha256": sha256(patch),
                    "full_file": new.name, "full_size": target.stat().st_size,
                    "full_sha256": new_hash, "roundtrip_verified": True,
                    "recommended": smaller, "default_payload": "delta" if smaller else "full"}
        publish_file(patch, output)
        if metadata_output:
            staged_metadata = stage / "metadata.json"
            staged_metadata.write_text(json.dumps(metadata, indent=2) + "\n")
            publish_file(staged_metadata, metadata_output)
    return metadata


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--old-crx", required=True)
    result.add_argument("--new-crx", required=True)
    result.add_argument("--from-version", required=True)
    result.add_argument("--to-version", required=True)
    result.add_argument("--puffin", required=True, help="Built //third_party/puffin:puffin tool")
    result.add_argument("--out", required=True)
    result.add_argument("--metadata", help="Optional JSON sidecar")
    return result


if __name__ == "__main__":
    try:
        print(json.dumps(build_delta(parser().parse_args()), indent=2))
    except (ValueError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
