# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Build a pinned, static Tongsuo dependency under GN/Ninja (no networking).

The upstream Configure/build system owns source selection and assembly. GN owns
the archive, target configuration, toolchain and complete output boundary. An
action rerun starts from the verified archive, so changed SDK/tool/configuration
inputs cannot reuse incompatible objects from an earlier build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import stat
import subprocess
import sys
import zipfile


HERE = Path(__file__).resolve().parent
TARGETS = {
    ("win", "x64"): "chromium-VC-WIN64A",
    ("linux", "x64"): "linux-x86_64-clang",
    ("linux", "arm64"): "linux-aarch64",
    ("mac", "x64"): "darwin64-x86_64",
    ("mac", "arm64"): "darwin64-arm64",
}
FEATURES = ["no-shared", "enable-ntls", "enable-async", "no-apps", "no-tests",
            "no-legacy", "no-quic", "no-ml-kem", "no-ml-dsa", "no-slh-dsa"]


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def configure_arguments(config):
    try:
        target = TARGETS[(config["target_os"], config["target_cpu"])]
    except KeyError as error:
        raise ValueError("Unsupported Tongsuo OS/architecture") from error
    args = [target, *FEATURES, "--debug" if config["debug"] else "--release"]
    if config["target_os"] == "win":
        args += ["--config=" + str(HERE / "chromium.conf"), "/guard:cf"]
    else:
        if not config.get("sysroot"):
            raise ValueError("A target SDK/sysroot is required")
        args += ["-fPIC", "-fvisibility=hidden"]
        if config["target_os"] == "linux":
            triple = ("x86_64-unknown-linux-gnu" if config["target_cpu"] == "x64"
                      else "aarch64-unknown-linux-gnu")
            args += ["--target=" + triple,
                     "--sysroot=" + shlex.quote(config["sysroot"])]
        else:
            if not config.get("deployment_target"):
                raise ValueError("macOS deployment target is required")
            # Configure treats a bare following path as another target. Keep
            # the SDK flag together, with quoting for upstream make recipes.
            args += ["-isysroot" + shlex.quote(config["sysroot"]),
                     "-mmacosx-version-min=" + config["deployment_target"]]
    return args


def archive_members(archive, root):
    """Validate *every* entry before extraction, including Windows aliases."""
    seen = set()
    members = archive.infolist()
    for entry in members:
        name = entry.orig_filename
        parts = PurePosixPath(name).parts
        key = name.rstrip("/").casefold()
        if (name != entry.filename or "\0" in name
                or not parts or parts[0] != root or ".." in parts
                or "\\" in name or ":" in name or name.startswith("/")
                or any(part.endswith((".", " ")) for part in parts)
                or key in seen):
            raise ValueError("Archive contains an unsafe or duplicate path")
        seen.add(key)
        mode = entry.external_attr >> 16
        if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG,
                                                         stat.S_IFDIR)):
            raise ValueError("Archive contains a non-regular entry")
    return members


def empty_output_scaffolding(path):
    """Ninja creates declared output parents before running an action."""
    pending = [path]
    while pending:
        current = pending.pop()
        info = current.lstat()
        if (not stat.S_ISDIR(info.st_mode)
                or getattr(info, "st_file_attributes", 0) &
                getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)):
            return False
        for entry in current.iterdir():
            # Inspect every entry without following symlinks or junctions. Any
            # file, even an empty one, means this is not Ninja scaffolding.
            pending.append(entry)
    return True


def prepare_source(archive, output, spec):
    if digest(archive) != spec["sha256"]:
        raise ValueError("Source archive SHA-256 mismatch")
    source = output / spec["archive_root"]
    # Validate the full archive before modifying any prior build.
    with zipfile.ZipFile(archive) as bundle:
        members = archive_members(bundle, spec["archive_root"])
        output.mkdir(parents=True, exist_ok=True)
        marker = output / ".tongsuo-managed"
        if source.exists():
            managed = (marker.is_file() and
                       marker.read_text(encoding="utf-8").strip() == spec["archive_root"])
            info = source.lstat()
            if (source.is_symlink()
                    or getattr(info, "st_file_attributes", 0) &
                    getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
                    or source.resolve().parent != output.resolve()
                    or (not managed and not empty_output_scaffolding(source))):
                raise ValueError("Refusing to replace an unmanaged source directory")
            if managed:
                shutil.rmtree(source)
        marker.write_text(spec["archive_root"] + "\n", encoding="utf-8")
        bundle.extractall(output, members)
    return source


def read_environment(path):
    return dict(item.split("=", 1) for item in
                Path(path).read_text().rstrip("\0").split("\0"))


def resolve_tool(name, env):
    path_key = next(key for key in env if key.upper() == "PATH")
    path = shutil.which(name, path=env[path_key])
    if not path:
        raise ValueError("Missing build tool: " + name)
    return str(Path(path).resolve())


def compiler_environment(os_name, tools):
    if os_name == "win":
        # The upstream Windows template quotes CC/AR itself. Prequoting here
        # produces ""C:\Program Files\..."" and breaks archiver execution.
        return {"CC": tools["compiler"], "AR": tools["archiver"], "CL": "/FS"}
    return {"CC": shlex.quote(tools["compiler"]),
            "AR": shlex.quote(tools["archiver"]),
            "RANLIB": shlex.quote(tools["archiver"]) + " s"}


def collect_tree_dependencies(root, suffixes):
    if not root.is_dir():
        return set()
    return {p.resolve() for p in root.rglob("*")
            if p.is_file() and p.suffix.lower() in suffixes}


def write_depfile(path, target, dependencies):
    def quote(value):
        try:
            value = os.path.relpath(value)
        except ValueError:
            value = str(value)
        return (Path(value).as_posix().replace("$", "$$").replace("#", "\\#")
                .replace(" ", "\\ ").replace(":", "\\:"))
    path.parent.mkdir(parents=True, exist_ok=True)
    separator = " " + "\\" + "\n  "
    path.write_text(quote(target) + ": " + separator.join(
        sorted(quote(p) for p in dependencies)) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--depfile", type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    host_os = {"win32": "win", "darwin": "mac", "linux": "linux"}.get(sys.platform)
    if config["target_os"] != host_os:
        raise ValueError("Use a native runner for the target operating system")
    if config["jobs"] < 1:
        raise ValueError("Build jobs must be positive")
    configure = configure_arguments(config)
    spec = json.loads((HERE / "source.json").read_text())
    output = Path(config["output_dir"]).resolve()
    archive = Path(config["archive"]).resolve()
    compiler_dir = Path(config["compiler_dir"]).resolve()
    env = read_environment(config["environment"]) if host_os == "win" else os.environ.copy()
    # A caller's compiler/provider overrides must not change this build.
    for key in list(env):
        if key.upper() in {"CC", "CXX", "CPP", "AR", "AS", "LD", "RANLIB",
                           "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CL",
                           "_CL_", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH",
                           "OPENSSL_CONF", "OPENSSL_MODULES", "MAKEFLAGS"}:
            del env[key]
    path_key = next(key for key in env if key.upper() == "PATH")
    tools = {"perl": str(Path(config["perl"]).resolve())}
    path_dirs = [str(compiler_dir), str(Path(tools["perl"]).parent)]
    if host_os == "win":
        tools["nasm"] = str(Path(config["nasm"]).resolve())
        path_dirs.append(str(Path(tools["nasm"]).parent))
    env[path_key] = os.pathsep.join([*path_dirs, env[path_key]])
    env.update({"LC_ALL": "C", "LC_CTYPE": "C", "LANG": "C", "VSLANG": "1033"})
    tools["compiler"] = str(compiler_dir / ("clang-cl.exe" if host_os == "win" else "clang"))
    tools["make"] = (str(Path(config["make"]).resolve()) if config["make"]
                     else resolve_tool("nmake.exe" if host_os == "win" else "make", env))
    tools["archiver"] = (resolve_tool("lib.exe", env) if host_os == "win"
                         else str(compiler_dir / "llvm-ar"))
    for path in tools.values():
        if not Path(path).is_file():
            raise ValueError("Missing build tool: " + path)
    # Configure records these exact tools using its platform-specific quoting.
    env.update(compiler_environment(host_os, tools))
    dependencies = {Path(p) for p in tools.values()}
    dependencies.update({args.config.resolve(), archive, HERE / "source.json",
                         HERE / "chromium.conf", Path(__file__).resolve()})
    if host_os == "win":
        dependencies.add(Path(config["environment"]).resolve())
        include_key = next(key for key in env if key.upper() == "INCLUDE")
        include_roots = [Path(p) for p in env[include_key].split(";") if p]
        # The bundled Windows Perl's DLL and modules affect Configure/generation.
        perl_root = Path(tools["perl"]).parent.parent
        dependencies.update(collect_tree_dependencies(perl_root / "lib", {".pm", ".pl"}))
        dependencies.update(Path(tools["perl"]).parent.glob("perl*.dll"))
    else:
        sdk = Path(config["sysroot"])
        include_roots = [sdk / "usr/include"]
        if host_os == "mac":
            include_roots.append(sdk / "System/Library/Frameworks")
            dependencies.update(p for p in (sdk / "SDKSettings.json", sdk / "SDKSettings.plist") if p.is_file())
    include_roots.append(compiler_dir.parent / "lib/clang")
    for include_root in include_roots:
        dependencies.update(collect_tree_dependencies(include_root, {".h", ".inc"}))
    source = prepare_source(archive, output, spec)
    manifest_path = output / "build-manifest.json"
    manifest_path.unlink(missing_ok=True)
    with (output / "build.log").open("w", encoding="utf-8") as log:
        def run(command):
            log.write(subprocess.list2cmdline(command) + "\n")
            log.flush()
            subprocess.run(command, cwd=source, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        run([tools["perl"], "Configure", *configure])
        parallel = (["-j", str(config["jobs"])] if host_os != "win"
                    or Path(tools["make"]).stem.lower() == "jom" else [])
        run([tools["make"], *parallel, "build_libs"])
    library_names = (["libssl.lib", "libcrypto.lib"] if host_os == "win"
                     else ["libssl.a", "libcrypto.a"])
    expected = [source / name for name in library_names]
    expected += [source / "include" / name for name in config["public_headers"]]
    for path in expected:
        if not path.is_file():
            raise ValueError("Missing declared Tongsuo output: " + str(path))
    manifest = {
        "schema": 2, "version": spec["version"], "sha256": spec["sha256"],
        "source": str(source), "target_os": host_os,
        "target_cpu": config["target_cpu"], "debug": config["debug"],
        "configure": configure, "config_sha256": digest(args.config),
        "tools": {name: {"path": path, "sha256": digest(path)}
                  for name, path in tools.items()},
        "artifacts": {name: digest(source / name) for name in library_names},
        "public_headers_sha256": hashlib.sha256("".join(
            name + ":" + digest(source / "include" / name) + "\n"
            for name in config["public_headers"]).encode()).hexdigest(),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    # Keep GN's output spelling, particularly across a Windows junction.
    depfile_target = (Path(config["output_dir"]) / spec["archive_root"] /
                      library_names[0])
    write_depfile(args.depfile, depfile_target, dependencies)
    print("Built Tongsuo " + spec["version"] + " for " + host_os + "/" + config["target_cpu"])


if __name__ == "__main__":
    main()
