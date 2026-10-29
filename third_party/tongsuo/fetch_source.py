# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Provision the pinned source archive before GN; never invoked by Ninja."""

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    spec = json.loads(Path(__file__).with_name("source.json").read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.is_file():
        if hashlib.sha256(args.output.read_bytes()).hexdigest() == spec["sha256"]:
            return
        raise ValueError("Existing source archive has an unexpected hash")
    staging = args.output.with_suffix(args.output.suffix + ".download")
    try:
        urllib.request.urlretrieve(spec["url"], staging)
        if hashlib.sha256(staging.read_bytes()).hexdigest() != spec["sha256"]:
            raise ValueError("Downloaded source SHA-256 mismatch")
        staging.replace(args.output)
    finally:
        staging.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
