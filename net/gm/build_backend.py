# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Audit the GN-linked backend's provenance and isolated native ABI.

Compilation and linking belong to normal GN targets. This post-link action
rejects leaked upstream symbols and shared libssl/libcrypto dependencies.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_depfile(path, target, dependencies):
    def quote(value):
        try:
            value = os.path.relpath(value)
        except ValueError:
            value = str(value)
        return (Path(value).as_posix().replace("$", "$$")
                .replace("#", "\\#").replace(" ", "\\ ").replace(":", "\\:"))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(quote(target) + ": " + " ".join(
        sorted(quote(value) for value in dependencies)) + "\n", encoding="utf-8")


def pe_symbols(output):
    return re.findall(r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)",
                      output, re.MULTILINE)


def posix_symbols(output, mac=False):
    # POSIX nm format: name type [value [size]]. Undefined symbols have already
    # been excluded by the command. The Mach-O C name has one leading '_'.
    names = [line.split()[0] for line in output.splitlines() if line.strip()]
    return [name[1:] if mac and name.startswith("_") else name for name in names]


def audit_surface(symbols, dependencies):
    if sorted(symbols) != ["gm_backend_get_api"]:
        raise ValueError("Backend export boundary changed: " + repr(symbols))
    if any(re.search(r"(?:^|[/\\])(?:lib)?(?:ssl|crypto)(?:[-.\d]|$)",
                     name, re.IGNORECASE) for name in dependencies):
        raise ValueError("Backend unexpectedly imports a shared crypto library")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target-os", choices=("win", "mac", "linux"), required=True)
    parser.add_argument("--environment", type=Path)
    parser.add_argument("--depfile", required=True, type=Path)
    args = parser.parse_args()
    spec_path = ROOT / "third_party/tongsuo/source.json"
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest["sha256"] != spec["sha256"] or manifest["version"] != spec["version"]:
        raise ValueError("Dependency provenance does not match pinned Tongsuo")
    source = Path(manifest["source"])
    dependencies = {spec_path, args.manifest.resolve(), args.library.resolve()}
    for name, expected in manifest["artifacts"].items():
        library = source / name
        if digest(library) != expected:
            raise ValueError("Dependency changed after build: " + name)
        dependencies.add(library)

    env = os.environ.copy()
    if args.environment:
        env = dict(pair.split("=", 1) for pair in
                   args.environment.read_text().rstrip("\0").split("\0"))
        dependencies.add(args.environment.resolve())
    path_key = next(key for key in env if key.upper() == "PATH")
    env.update({"LC_ALL": "C", "LANG": "C", "VSLANG": "1033"})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.with_suffix(".log").open("w", encoding="utf-8") as log:
        def run(tool, *options):
            executable = shutil.which(tool, path=env[path_key])
            if not executable:
                raise ValueError("Missing native audit tool: " + tool)
            dependencies.add(Path(executable))
            command = [executable, *options, str(args.library.resolve())]
            result = subprocess.run(command, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    check=True)
            log.write(subprocess.list2cmdline(command) + "\n" + result.stdout)
            return result.stdout

        if args.target_os == "win":
            symbols = pe_symbols(run("dumpbin.exe", "/nologo", "/exports"))
            imports = run("dumpbin.exe", "/nologo", "/dependents")
            imported_libraries = sorted(set(re.findall(
                r"^\s+([\w.-]+\.dll)\s*$", imports, re.MULTILINE | re.IGNORECASE)))
        elif args.target_os == "mac":
            symbols = posix_symbols(run("nm", "-gU", "-P"), mac=True)
            imports = run("otool", "-L")
            imported_libraries = [line.strip().split(" (", 1)[0]
                                  for line in imports.splitlines()[1:] if line.strip()]
            # otool also lists the dylib's own install name.
            imported_libraries = [name for name in imported_libraries
                                  if Path(name).name != args.library.name]
        else:
            symbols = posix_symbols(run("nm", "-D", "--defined-only", "--format=posix"))
            imports = run("readelf", "-d")
            imported_libraries = re.findall(r"\(NEEDED\).*?\[(.+?)\]", imports)
        audit_surface(symbols, imported_libraries)
        audit = {"abi": 3, "tongsuo": spec["version"], "exports": symbols,
                 "build_system": "GN native shared_library",
                 "target_os": args.target_os,
                 "size": args.library.stat().st_size,
                 "sha256": digest(args.library),
                 "dependency_manifest_sha256": digest(args.manifest),
                 "dependencies": sorted(imported_libraries)}
        args.output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    write_depfile(args.depfile, args.output, dependencies)


if __name__ == "__main__":
    main()
