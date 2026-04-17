#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Build xenon_ai_inferencer_burn using Chromium's Rust toolchain.

Thin driver over tools/crates/run_cargo.py: that script already resolves
cargo/rustc from the given --rust-sysroot and sets Windows toolchain flags.
This script only adds what is specific to this sidecar: invoking cargo with
our manifest/target-dir and copying the release binary into $root_out_dir.

GN `action` must pass --run-cargo and --rust-sysroot via rebase_path(...).
See README.md for manual invocation. Follows docs/rust.md ("Using cargo").
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--run-cargo",
        required=True,
        help="Absolute path to tools/crates/run_cargo.py (GN: rebase_path).",
    )
    parser.add_argument(
        "--rust-sysroot",
        required=True,
        help="Absolute path to //third_party/rust-toolchain (GN: rebase_path).",
    )
    parser.add_argument(
        "--crate-root",
        required=True,
        help="Absolute path to xenon_ai_inferencer_burn (GN: rebase_path).",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Chromium $root_out_dir (GN: rebase_path).",
    )
    parser.add_argument(
        "--no-default-features",
        action="store_true",
        help="Pass --no-default-features to cargo (e.g. use backend_ndarray only).",
    )
    parser.add_argument(
        "--features",
        default="",
        help="Comma-separated list passed to cargo --features (optional).",
    )
    args = parser.parse_args()

    run_cargo_py = Path(args.run_cargo).resolve()
    rust_sysroot = Path(args.rust_sysroot).resolve()
    crate_root = Path(args.crate_root).resolve()
    out_dir = Path(args.out_dir).resolve()
    target_dir = out_dir / "obj" / "xenon_ai_inferencer_burn_cargo"

    manifest = crate_root / "Cargo.toml"
    if not manifest.is_file():
        print(f"ERROR: missing {manifest}", file=sys.stderr)
        return 1

    # Let run_cargo.py locate cargo/rustc under --rust-sysroot and set
    # Windows toolchain env (CARGO_ENCODED_RUSTFLAGS etc.).
    cmd = [
        sys.executable,
        str(run_cargo_py),
        "--rust-sysroot",
        str(rust_sysroot),
        "build",
        "--release",
        f"--manifest-path={manifest}",
        f"--target-dir={target_dir}",
    ]
    if args.no_default_features:
        cmd.append("--no-default-features")
    if args.features:
        cmd.extend(["--features", args.features])
    print("Running:", " ".join(str(x) for x in cmd), flush=True)

    try:
        subprocess.check_call(cmd, cwd=str(crate_root), env=os.environ.copy())
    except subprocess.CalledProcessError as e:
        print(
            f"ERROR: cargo (via run_cargo.py) failed with exit code {e.returncode}",
            file=sys.stderr,
        )
        return e.returncode

    # cargo writes "<name>.exe" on Windows and "<name>" elsewhere; copy it
    # next to chrome.exe in $root_out_dir.
    bin_name = "xenon_ai_inferencer_burn"
    if sys.platform == "win32":
        bin_name += ".exe"
    src_bin = target_dir / "release" / bin_name
    if not src_bin.is_file():
        print(f"ERROR: expected binary not found: {src_bin}", file=sys.stderr)
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    dst = out_dir / bin_name
    shutil.copy2(src_bin, dst)
    print(f"OK: {dst}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
