#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import os
import sys


def _to_gn_path(path):
    return path.replace("\\", "/")


def main():
    if len(sys.argv) < 4:
        print(
            "Usage: generate_assets_grdp.py <dir_to_scan> <output_json> <base_dir>"
        )
        return 1

    scan_dir = os.path.normpath(sys.argv[1])
    out_file = os.path.normpath(sys.argv[2])
    base_dir = os.path.normpath(sys.argv[3])

    files = []
    dep_paths = []

    if os.path.isdir(scan_dir):
        dep_paths.append(_to_gn_path(os.path.relpath(scan_dir, os.getcwd())))
        for dirpath, _, filenames in os.walk(scan_dir):
            for filename in filenames:
                abs_path = os.path.join(dirpath, filename)
                dep_paths.append(_to_gn_path(os.path.relpath(abs_path, os.getcwd())))

                if filename.endswith(".map"):
                    continue

                rel_path = os.path.relpath(abs_path, base_dir)
                files.append(_to_gn_path(rel_path))

    manifest = {
        "base_dir": _to_gn_path(os.path.relpath(base_dir, os.getcwd())),
        "files": sorted(files),
    }

    os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)
    with open(out_file, "w", encoding="utf-8") as manifest_file:
        json.dump(manifest, manifest_file, indent=2, ensure_ascii=False)

    script_rel = _to_gn_path(os.path.relpath(os.path.abspath(__file__), os.getcwd()))
    out_rel = _to_gn_path(os.path.relpath(out_file, os.getcwd()))
    deps = sorted(set(dep_paths + [script_rel]))
    with open(out_rel + ".d", "w", encoding="utf-8") as depfile:
        depfile.write(out_rel + ": " + " ".join(deps) + "\n")

    print(f"Generated manifest {out_file} with {len(files)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
