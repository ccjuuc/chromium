#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Validation shared by the offline macOS system-updater release tools."""

import hashlib
import base64
import os
from pathlib import Path
import plistlib
import re
import stat
import subprocess


def run(command, **kwargs):
    result = subprocess.run([str(arg) for arg in command], check=False,
                            capture_output=True, **kwargs)
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(f"{command[0]} failed ({result.returncode}): {detail}")
    return result.stdout


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,199}", value):
        raise ValueError(f"Invalid identifier: {value!r}")
    return value


def version(value):
    if not isinstance(value, str) or not re.fullmatch(
            r"(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){3}", value):
        raise ValueError(f"Expected a four-component version: {value!r}")
    if any(int(part) > 0xffffffff for part in value.split(".")):
        raise ValueError("Version component exceeds uint32")
    return value


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_sha256_pin(value):
    if re.fullmatch(r"[A-Fa-f0-9]{64}", value or ""):
        return bytes.fromhex(value)
    try:
        decoded = base64.b64decode(value, validate=True)
    except (ValueError, TypeError) as error:
        raise ValueError("Expected a SHA-256 pin in hexadecimal or base64") from error
    if len(decoded) != 32:
        raise ValueError("Expected a non-empty 32-byte SHA-256 pin")
    return decoded


def check_tree(root):
    """Refuse privileged payload hazards, while retaining framework symlinks."""
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"Expected a real directory: {root}")
    canonical = root.resolve(strict=True)
    for current, directories, files in os.walk(root, followlinks=False):
        for path in [Path(current), *(Path(current) / name
                                     for name in directories + files)]:
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                target = os.readlink(path)
                if os.path.isabs(target):
                    raise ValueError(f"Absolute payload symlink: {path}")
                try:
                    path.resolve(strict=True).relative_to(canonical)
                except (OSError, RuntimeError, ValueError) as error:
                    raise ValueError(f"Escaping or broken payload symlink: {path}") from error
            elif stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode):
                if info.st_mode & 0o7022:
                    raise ValueError(f"Unsafe payload permissions: {path}")
                if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
                    raise ValueError(f"Hard-linked payload file: {path}")
            else:
                raise ValueError(f"Special payload file: {path}")


def app_metadata(app):
    app = Path(app)
    check_tree(app)
    with (app / "Contents/Info.plist").open("rb") as source:
        info = plistlib.load(source)
    identifier(info.get("CFBundleIdentifier", ""))
    version(info.get("CFBundleShortVersionString"))
    executable = info.get("CFBundleExecutable", "")
    if not executable or executable in (".", "..") or any(
            part in executable for part in ("/", "\\", "\0")):
        raise ValueError("Invalid CFBundleExecutable")
    main = app / "Contents/MacOS" / executable
    if main.is_symlink() or not main.is_file() or not os.access(main, os.X_OK):
        raise ValueError(f"Missing executable: {main}")
    return info


def verify_app(app, *, development, team_id=None, command=run):
    arguments = ["/usr/bin/codesign", "--verify", "--deep", "--strict",
                 "--all-architectures"]
    if not development:
        if not team_id or not re.fullmatch(r"[A-Z0-9]{10}", team_id):
            raise ValueError("Release packages require a 10-character --team-id")
        arguments += ["--test-requirement",
                      'anchor apple generic and '
                      f'certificate leaf[subject.OU] = "{team_id}" and '
                      'certificate leaf[field.1.2.840.113635.100.6.1.13] exists']
    command([*arguments, app])
    if not development:
        command(["/usr/bin/xcrun", "stapler", "validate", app])


def publish_file(source, output):
    """Publish complete artifacts only; the temporary file stays on that volume."""
    import shutil
    import tempfile
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".xenon-publish-", dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as destination, open(source, "rb") as incoming:
            shutil.copyfileobj(incoming, destination, 1024 * 1024)
            destination.flush()
            os.fsync(destination.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, output)
    finally:
        if os.path.lexists(temporary):
            os.unlink(temporary)
