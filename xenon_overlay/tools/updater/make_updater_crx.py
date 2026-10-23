#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Wrap a system application PKG in Chromium Updater's signed CRX3 format.

The native //components/crx_file:crx3_build_action creates the CRX. This tool
independently checks its pinned RSA proof before publishing anything. It never
executes the payload installer or starts an updater service.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import struct
import sys
import tempfile
import zipfile

from updater_packaging_common import (app_metadata, parse_sha256_pin, publish_file, run, sha256,
                                      verify_app, version)

TARGET = "/Applications/xl153.app"


def version_guard_script():
    return r'''version_allows_install() {
  local candidate component index
  local -a parts installed_parts target_parts
  for candidate in "$1" "$2"; do
    [[ "$candidate" =~ ^(0|[1-9][0-9]*)([.](0|[1-9][0-9]*)){3}$ ]] || return 2
    IFS=. read -r -a parts <<< "$candidate"
    for component in "${parts[@]}"; do
      [[ "${#component}" -le 10 ]] || return 2
      [[ "$((10#$component))" -le 4294967295 ]] || return 2
    done
  done
  IFS=. read -r -a installed_parts <<< "$1"
  IFS=. read -r -a target_parts <<< "$2"
  for index in 0 1 2 3; do
    if ((10#${installed_parts[index]} > 10#${target_parts[index]})); then
      return 1
    elif ((10#${installed_parts[index]} < 10#${target_parts[index]})); then
      return 0
    fi
  done
  return 0
}
'''


def installer_script(info, package_hash, *, wait_seconds=180):
    """A fixed system-only payload, following installer_api_mac.md's .install ABI."""
    if not 1 <= wait_seconds <= 600:
        raise ValueError("--wait-seconds must be between 1 and 600")
    quote = shlex.quote
    target_version = version(info["CFBundleShortVersionString"])
    executable = info["CFBundleExecutable"]
    # pgrep's exact process-name match is intentionally conservative: another
    # xl153 installation also delays this update. It checks all users as root.
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", executable):
        raise ValueError("The process name must fit macOS's 15-character comm field")
    return f'''#!/bin/bash -p
set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
unset BASH_ENV CDPATH ENV GLOBIGNORE IFS
{version_guard_script()}
[[ "$EUID" -eq 0 && "${{UPDATE_IS_MACHINE:-}}" = 1 ]] || exit 1
readonly TARGET={quote(TARGET)}
[[ "${{KS_TICKET_XC_PATH:-}}" = "$TARGET" ]] || exit 1
[[ -d "$TARGET" && ! -L "$TARGET" ]] || exit 1
[[ "$(/usr/bin/stat -f %u "$TARGET")" = 0 ]] || exit 1
readonly PAYLOAD="$(cd -P -- "$(/usr/bin/dirname -- "${{BASH_SOURCE[0]}}")" && pwd)"
readonly PACKAGE="$PAYLOAD/application.pkg"
[[ -f "$PACKAGE" && ! -L "$PACKAGE" ]] || exit 1
[[ "$(/usr/bin/shasum -a 256 "$PACKAGE" | /usr/bin/cut -d ' ' -f 1)" = {quote(package_hash)} ]] || exit 1
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TARGET/Contents/Info.plist")" = {quote(info['CFBundleIdentifier'])} ]] || exit 1
# Never overwrite the framework of a running browser, and never kill a user process.
echo 'Waiting for xl153 to exit before installing the update.' >&2
readonly DEADLINE=$((SECONDS + {wait_seconds}))
while true; do
  status=0
  /usr/bin/pgrep -x {quote(executable)} >/dev/null || status=$?
  [[ "$status" -eq 1 ]] && break
  [[ "$status" -eq 0 ]] || exit 1
  [[ "$SECONDS" -lt "$DEADLINE" ]] || {{ echo 'xl153 is still running; retry after quitting it.' >&2; exit 1; }}
  /bin/sleep 1
done
# Recheck the installation boundary after a potentially long wait.
[[ -d "$TARGET" && ! -L "$TARGET" ]] || exit 1
[[ "$(/usr/bin/stat -f %u "$TARGET")" = 0 ]] || exit 1
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TARGET/Contents/Info.plist")" = {quote(info['CFBundleIdentifier'])} ]] || exit 1
# The root Updater supplies privilege; this is not a browser-side elevation command.
INSTALLED_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$TARGET/Contents/Info.plist")"
if ! version_allows_install "$INSTALLED_VERSION" {quote(target_version)}; then
  echo "Refusing update: installed product version '$INSTALLED_VERSION' is invalid or newer than {target_version}." >&2
  exit 1
fi
/usr/sbin/installer -pkg "$PACKAGE" -target /
[[ -d "$TARGET" && ! -L "$TARGET" ]] || exit 1
[[ "$(/usr/bin/stat -f %u "$TARGET")" = 0 ]] || exit 1
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TARGET/Contents/Info.plist")" = {quote(info['CFBundleIdentifier'])} ]] || exit 1
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$TARGET/Contents/Info.plist")" = {quote(info['CFBundleShortVersionString'])} ]] || exit 1
/usr/bin/codesign --verify --deep --strict "$TARGET"
'''


def public_key(key, expected_hash, *, command=run, openssl="/usr/bin/openssl"):
    path = Path(key)
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
            getattr(info, "st_file_attributes", 0) & 0x400):
        raise ValueError("CRX signing key must be a regular file with no links")
    if os.name == "nt":
        from updater_host_util import check_private_key_acl
        check_private_key_acl(path)
    elif info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("CRX signing key must be a private, owned 0600 regular file")
    pin = parse_sha256_pin(expected_hash)
    command([openssl, "rsa", "-inform", "DER", "-in", key,
             "-passin", "pass:", "-check", "-noout"])
    # The native creator dereferences FromPrivateKeyInfo's result. Traditional
    # PKCS#1 RSA keys also pass `openssl rsa`, but are not valid inputs to it.
    command([openssl, "pkcs8", "-inform", "DER", "-in", key,
             "-nocrypt", "-out", os.devnull])
    public = command([openssl, "pkey", "-inform", "DER", "-in", key,
                      "-pubout", "-outform", "DER"])
    description = command([openssl, "pkey", "-pubin", "-inform", "DER",
                           "-text_pub", "-noout"], input=public).decode()
    bits = re.search(r"Public-Key:\s*\((\d+) bit\)", description)
    if not bits or int(bits.group(1)) < 2048:
        raise ValueError("CRX signing requires an RSA key of at least 2048 bits")
    if hashlib.sha256(public).digest() != pin:
        raise ValueError("Signing key does not match the pinned CRX public key")
    return public


def protobuf_bytes(data):
    """Read the length-delimited fields used by components/crx_file/crx3.proto."""
    fields = {}
    offset = 0

    def varint():
        nonlocal offset
        value = 0
        for shift in range(0, 64, 7):
            if offset == len(data):
                raise ValueError("Truncated CRX header")
            octet = data[offset]
            offset += 1
            value |= (octet & 127) << shift
            if not octet & 128:
                return value
        raise ValueError("Invalid CRX varint")

    while offset < len(data):
        tag = varint()
        if tag & 7 != 2 or not tag >> 3:
            raise ValueError("Unexpected CRX protobuf field")
        length = varint()
        if length > len(data) - offset:
            raise ValueError("Truncated CRX field")
        fields.setdefault(tag >> 3, []).append(data[offset:offset + length])
        offset += length
    return fields


def single_field(fields, number):
    values = fields.get(number, [])
    if len(values) != 1:
        raise ValueError(f"CRX field {number} must occur exactly once")
    return values[0]


def verify_crx(crx, archive, expected_public, *, command=run, openssl="/usr/bin/openssl"):
    with open(crx, "rb") as source:
        prefix = source.read(12)
        if len(prefix) != 12 or prefix[:4] != b"Cr24":
            raise ValueError("Not a CRX3 file")
        file_version, length = struct.unpack("<II", prefix[4:])
        if file_version != 3 or not 0 < length <= 1024 * 1024:
            raise ValueError("Unsupported CRX header")
        fields = protobuf_bytes(source.read(length))
        signed_header = single_field(fields, 10000)
        proof = protobuf_bytes(single_field(fields, 2))
        if single_field(proof, 1) != expected_public:
            raise ValueError("CRX proof uses an unexpected public key")
        crx_id = single_field(protobuf_bytes(signed_header), 1)
        if crx_id != hashlib.sha256(expected_public).digest()[:16]:
            raise ValueError("CRX ID does not match the pinned key")
        with tempfile.TemporaryDirectory(prefix="xenon-crx-verify-") as temporary:
            stage = Path(temporary)
            (stage / "signature").write_bytes(single_field(proof, 2))
            public_pem = command([openssl, "pkey", "-pubin", "-inform",
                                  "DER", "-outform", "PEM"], input=expected_public)
            (stage / "public.pem").write_bytes(public_pem)
            payload_hash = hashlib.sha256()
            with (stage / "signed-data").open("wb") as signed:
                signed.write(b"CRX3 SignedData\0")
                signed.write(struct.pack("<I", len(signed_header)))
                signed.write(signed_header)
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    payload_hash.update(block)
                    signed.write(block)
            if payload_hash.hexdigest() != sha256(archive):
                raise ValueError("CRX archive differs from the verified input ZIP")
            command([openssl, "dgst", "-sha256", "-verify", stage / "public.pem",
                     "-signature", stage / "signature", stage / "signed-data"])


def inventory(root):
    result = {}
    for current, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            path = Path(current) / name
            info = path.lstat()
            value = (stat.S_IFMT(info.st_mode), stat.S_IMODE(info.st_mode))
            if path.is_symlink():
                value += (os.readlink(path),)
            elif path.is_file():
                value += (sha256(path),)
            result[path.relative_to(root).as_posix()] = value
    return result


def verify_package_payload(package, app, stage, *, command=run):
    """Require the PKG to contain exactly the supplied app and no install scripts."""
    expanded = stage / "expanded"
    command(["/usr/sbin/pkgutil", "--expand-full", package, expanded])
    payloads = list(expanded.rglob("Payload"))
    if len(payloads) != 1 or not payloads[0].is_dir():
        raise ValueError("Application PKG must contain exactly one expanded payload")
    payload = payloads[0]
    # make_mac_pkg.js uses an /Applications install-location, with xl153.app
    # immediately below Payload. Inspect PackageInfo too, not only contents.
    import xml.etree.ElementTree as ET
    package_infos = list(expanded.rglob("PackageInfo"))
    if len(package_infos) != 1 or list(expanded.rglob("Scripts")):
        raise ValueError("Application PKG must have one component and no install scripts")
    package_info = ET.parse(payload.parent / "PackageInfo").getroot()
    if package_info.get("install-location") != "/Applications":
        raise ValueError("Application PKG must target /Applications")
    if (payload.parent / "Scripts").exists() or package_info.find("scripts") is not None:
        raise ValueError("Application update PKG must not include arbitrary install scripts")
    distribution = expanded / "Distribution"
    if distribution.exists():
        root = ET.parse(distribution).getroot()
        if root.find("script") is not None or root.find("installation-check") is not None:
            raise ValueError("Application PKG must not contain executable distribution scripts")
        references = [ref.text.strip() for ref in root.findall("pkg-ref") if ref.text and ref.text.strip()]
        if len(references) != 1 or references[0] not in (payload.parent.name, "#" + payload.parent.name):
            raise ValueError("Application PKG must reference only its embedded application component")
    if sorted(path.name for path in payload.iterdir()) != ["xl153.app"]:
        raise ValueError("Application PKG must install only xl153.app")
    extracted = payload / "xl153.app"
    if app_metadata(extracted) != app_metadata(app) or inventory(extracted) != inventory(app):
        raise ValueError("Application PKG does not match the supplied signed app")


def build_crx(args, *, command=run):
    app = Path(args.app).absolute()
    package = Path(args.pkg).absolute()
    output = Path(args.out).absolute()
    if app.name != "xl153.app":
        raise ValueError("The system update target must be xl153.app")
    if (output.resolve().is_relative_to(app.resolve()) or
            output.resolve() in (package.resolve(), Path(args.key).resolve())):
        raise ValueError("Output must not replace a packaging input")
    if package.is_symlink() or not package.is_file():
        raise ValueError("--pkg must be a regular flat macOS PKG")
    with package.open("rb") as source:
        if source.read(4) != b"xar!":
            raise ValueError("--pkg must be a regular flat macOS PKG")
    info = app_metadata(app)
    verify_app(app, development=args.development, team_id=args.team_id, command=command)
    public = public_key(args.key, args.expected_key_sha256, command=command)
    with tempfile.TemporaryDirectory(prefix="xenon-updater-crx-") as temporary:
        stage = Path(temporary)
        copied_key = stage / "signing-key.pk8"
        shutil.copyfile(args.key, copied_key)
        copied_key.chmod(0o600)
        if public_key(copied_key, args.expected_key_sha256, command=command) != public:
            raise ValueError("CRX signing key changed while copying")
        # Freeze the package before inspecting it, hashing it and signing it.
        copied_package = stage / "application.pkg"
        shutil.copyfile(package, copied_package)
        if not args.development:
            signature = command(["/usr/sbin/pkgutil", "--check-signature", copied_package]).decode()
            if not re.search(r"Developer ID Installer: .+\(" + re.escape(args.team_id) + r"\)", signature):
                raise ValueError("Application PKG is not signed by the configured installer team")
            command(["/usr/bin/xcrun", "stapler", "validate", copied_package])
        verify_package_payload(copied_package, app, stage, command=command)
        package_hash = sha256(copied_package)
        archive = stage / "payload.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED,
                             allowZip64=True) as payload:
            script = zipfile.ZipInfo(".install", date_time=(2020, 1, 1, 0, 0, 0))
            script.create_system = 3
            script.external_attr = (stat.S_IFREG | 0o755) << 16
            payload.writestr(script, installer_script(info, package_hash,
                                                    wait_seconds=args.wait_seconds))
            package_entry = zipfile.ZipInfo("application.pkg", date_time=(2020, 1, 1, 0, 0, 0))
            package_entry.create_system = 3
            package_entry.external_attr = (stat.S_IFREG | 0o644) << 16
            with payload.open(package_entry, "w", force_zip64=True) as dest, copied_package.open("rb") as source:
                shutil.copyfileobj(source, dest, 1024 * 1024)
        crx = stage / "update.crx3"
        command([args.crx_tool, crx, archive, copied_key])
        verify_crx(crx, archive, public, command=command)
        metadata = {"artifact": str(output), "sha256": sha256(crx),
                    "size": crx.stat().st_size, "version": info["CFBundleShortVersionString"],
                    "app_id": info["CFBundleIdentifier"], "format": "crx3",
                    "public_key_sha256": hashlib.sha256(public).hexdigest(),
                    "public_key_sha256_base64": base64.b64encode(hashlib.sha256(public).digest()).decode(),
                    "package_sha256": package_hash, "development": args.development,
                    "installer": ".install", "target": TARGET}
        publish_file(crx, output)
    return metadata


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--app", required=True)
    result.add_argument("--pkg", required=True)
    result.add_argument("--key", required=True, help="RSA private key, DER PKCS#8, mode 0600")
    result.add_argument("--expected-key-sha256", required=True,
                        help="Pinned SHA-256 of public SPKI DER, hexadecimal or base64 (same as GN)")
    result.add_argument("--crx-tool", required=True, help="Built crx3_build_action executable")
    result.add_argument("--out", required=True)
    result.add_argument("--wait-seconds", type=int, default=180)
    result.add_argument("--development", action="store_true")
    result.add_argument("--team-id")
    return result


if __name__ == "__main__":
    try:
        print(json.dumps(build_crx(parser().parse_args()), indent=2))
    except (ValueError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
