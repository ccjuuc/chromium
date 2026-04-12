#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Build xenon_ai_inferencer_burn via Cargo and copy the binary to Chromium out dir."""

import argparse
import os
import shutil
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--crate-root",
        required=True,
        help="Absolute or src-relative path to xenon_ai_inferencer_burn",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Chromium $root_out_dir (absolute path from GN rebase)",
    )
    args = parser.parse_args()

    crate_root = os.path.normpath(os.path.abspath(args.crate_root))
    out_dir = os.path.normpath(os.path.abspath(args.out_dir))
    target_dir = os.path.join(out_dir, "obj", "xenon_ai_inferencer_burn_cargo")

    cargo = shutil.which("cargo")
    if not cargo:
        print(
            "ERROR: cargo not found on PATH. Install a Rust toolchain (rustup) "
            "to build xenon_ai_inferencer_burn.",
            file=sys.stderr,
        )
        return 1

    manifest = os.path.join(crate_root, "Cargo.toml")
    if not os.path.isfile(manifest):
        print(f"ERROR: missing {manifest}", file=sys.stderr)
        return 1

    cmd = [
        cargo,
        "build",
        "--release",
        f"--manifest-path={manifest}",
        f"--target-dir={target_dir}",
    ]
    print("Running:", " ".join(cmd), flush=True)
    try:
        subprocess.check_call(cmd, cwd=crate_root, env=os.environ.copy())
    except subprocess.CalledProcessError as e:
        print(f"ERROR: cargo failed with exit code {e.returncode}", file=sys.stderr)
        return e.returncode

    is_win = sys.platform == "win32"
    bin_name = "xenon_ai_inferencer_burn.exe" if is_win else "xenon_ai_inferencer_burn"
    src = os.path.join(target_dir, "release", bin_name)
    if not os.path.isfile(src):
        print(f"ERROR: expected binary not found: {src}", file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, bin_name)
    shutil.copy2(src, dst)
    print(f"OK: {dst}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
