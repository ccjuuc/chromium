#!/usr/bin/env python3
# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Package Chromium's Xenon-branded Updater and root app registration.

This only creates artifacts. It never invokes installer, launchctl or an updater.
Use --include-app for initial installation; otherwise the app must already exist.
"""

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import sys
import tempfile
from xml.sax.saxutils import escape

from updater_packaging_common import (app_metadata, identifier, publish_file,
                                      run, sha256, verify_app)

UPDATER_ROOT = "/Library/Application Support/Xenon/XenonUpdater"
STAGING = UPDATER_ROOT + "/PkgStaging/XenonUpdater.app"
TARGET = "/Applications/xl153.app"
SUPPORTED_ARCHITECTURES = frozenset(("arm64", "x86_64"))


def is_macho(path):
    with path.open("rb") as source:
        header = source.read(8)
    if header[:4] in (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                      b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"):
        return True
    byte_order = {b"\xca\xfe\xba\xbe": "big", b"\xbe\xba\xfe\xca": "little",
                  b"\xca\xfe\xba\xbf": "big", b"\xbf\xba\xfe\xca": "little"}.get(header[:4])
    return byte_order is not None and len(header) == 8 and 0 < int.from_bytes(
        header[4:], byte_order) <= 64


def bundle_architectures(app, *, command=run):
    app = Path(app)
    info = app_metadata(app)
    main = app / "Contents/MacOS" / info["CFBundleExecutable"]
    if not is_macho(main):
        raise ValueError(f"Application main executable must be Mach-O: {main}")
    architectures = set(SUPPORTED_ARCHITECTURES)
    for current, directories, files in os.walk(app, followlinks=False):
        for name in files:
            binary = Path(current) / name
            if binary.is_symlink() or not is_macho(binary):
                continue
            architectures.intersection_update(command(
                ["/usr/bin/lipo", "-archs", binary]).decode("ascii").split())
            if not architectures:
                raise ValueError(f"Bundle has no common supported architecture: {app}")
    return architectures


def architecture_names(architectures):
    if not architectures or not set(architectures) <= SUPPORTED_ARCHITECTURES:
        raise ValueError("Expected non-empty supported package architectures")
    return sorted(architectures)


def architecture_preinstall(architectures):
    allowed = "|".join(architecture_names(architectures))
    return f'''#!/bin/bash -p
set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
unset BASH_ENV CDPATH ENV GLOBIGNORE IFS
HOST_ARCH="$(/usr/bin/uname -m)"
if [[ "$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]]; then
  HOST_ARCH=arm64
fi
case "$HOST_ARCH" in
  {allowed}) ;;
  *) echo "Unsupported host architecture: $HOST_ARCH" >&2; exit 1 ;;
esac
'''


def postinstall_script(app_id, app_version, bundle_id, keystone_name):
    """Use the same --install --system entrypoint as upstream postinstall.in."""
    ksadmin = (UPDATER_ROOT + "/Current/XenonUpdater.app/Contents/Helpers/" +
               keystone_name + ".bundle/Contents/Helpers/ksadmin")
    quote = shlex.quote
    return f'''#!/bin/bash -p
set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
unset BASH_ENV CDPATH ENV GLOBIGNORE IFS
[[ "$EUID" -eq 0 && "${{3:-/}}" = / ]] || exit 1
readonly STAGING={quote(STAGING)}
readonly TARGET={quote(TARGET)}
# Registration belongs to this root package transaction, never a user IPC call.
[[ -d "$TARGET" && ! -L "$TARGET" ]] || exit 1
[[ "$(/usr/bin/stat -f %u "$TARGET")" = 0 ]] || exit 1
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TARGET/Contents/Info.plist")" = {quote(bundle_id)} ]] || exit 1
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$TARGET/Contents/Info.plist")" = {quote(app_version)} ]] || exit 1
[[ -d "$STAGING" && ! -L "$STAGING" ]] || exit 1
"$STAGING/Contents/MacOS/XenonUpdater" --install --system
{quote(ksadmin)} -r -S -P {quote(app_id)} -v {quote(app_version)} -x "$TARGET" \\
  --version-path "$TARGET/Contents/Info.plist" --version-key CFBundleShortVersionString
# Exactly the fixed root-owned PkgStaging app used by the upstream packaging flow.
/bin/rm -rf "$STAGING"
'''


def distribution_xml(package_id, package_version, architectures, browser_id=None):
    must_close = (f'<must-close><app id="{escape(browser_id)}"/></must-close>'
                  if browser_id else "")
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<installer-gui-script minSpecVersion="2">
  <title>Xenon Updater</title>
  <options customize="never" require-scripts="true" hostArchitectures="{','.join(architecture_names(architectures))}"/>
  <domains enable_currentUserHome="false" enable_localSystem="true" enable_anywhere="false"/>
  <choices-outline><line choice="updater"/></choices-outline>
  <choice id="updater" visible="false"><pkg-ref id="{escape(package_id)}"/></choice>
  <pkg-ref id="{escape(package_id)}" version="{escape(package_version)}">component.pkg</pkg-ref>
  <pkg-ref id="{escape(package_id)}">{must_close}</pkg-ref>
</installer-gui-script>
'''


def build_package(args, *, command=run):
    updater = Path(args.updater_app).absolute()
    application = Path(args.app).absolute()
    output = Path(args.out).absolute()
    if updater.name != "XenonUpdater.app":
        raise ValueError("--updater-app must be the Xenon-branded XenonUpdater.app")
    app_id = identifier(args.app_id)
    updater_id = identifier(args.updater_id)
    package_id = identifier(args.identifier)
    keystone_name = identifier(args.keystone_name)
    if not args.development and not (args.installer_identity and args.notary_profile):
        raise ValueError("Release output requires --installer-identity and --notary-profile")
    if args.development and (args.installer_identity or args.notary_profile):
        raise ValueError("Development artifacts cannot use release signing/notarization options")
    for source in (updater, application):
        if output.resolve().is_relative_to(source.resolve()):
            raise ValueError("Output must be outside the input application bundles")
    updater_info = app_metadata(updater)
    app_info = app_metadata(application)
    if app_info["CFBundleIdentifier"] != app_id:
        raise ValueError("The registered app ID must match the application bundle ID")
    if updater_info["CFBundleIdentifier"] != updater_id or updater_info[
            "CFBundleExecutable"] != "XenonUpdater":
        raise ValueError("Updater identity or executable does not match Xenon branding")
    if application.name != "xl153.app":
        raise ValueError("The registered application must be xl153.app")
    updater_architectures = bundle_architectures(updater, command=command)
    app_architectures = bundle_architectures(application, command=command)
    architectures = updater_architectures & app_architectures
    if not architectures:
        raise ValueError("Browser and Updater have no common supported architecture")
    for source in (updater, application):
        verify_app(source, development=args.development, team_id=args.team_id,
                   command=command)
    with tempfile.TemporaryDirectory(prefix="xenon-system-pkg-") as temporary:
        stage = Path(temporary)
        payload = stage / "payload"
        if args.include_app:
            payload.mkdir()
            copied_updater = payload / STAGING.lstrip("/")
            copied_updater.parent.mkdir(parents=True)
            copied_app = payload / TARGET.lstrip("/")
            copied_app.parent.mkdir(parents=True)
            command(["/usr/bin/ditto", application, copied_app])
            if app_metadata(copied_app) != app_info:
                raise ValueError("Application metadata changed while copying")
            if bundle_architectures(copied_app, command=command) != app_architectures:
                raise ValueError("Application architectures changed while copying")
            verify_app(copied_app, development=args.development,
                       team_id=args.team_id, command=command)
        else:
            copied_updater = payload
        command(["/usr/bin/ditto", updater, copied_updater])
        # Validate the exact copied inputs, too: package a snapshot, not a live build.
        if app_metadata(copied_updater) != updater_info:
            raise ValueError("Updater metadata changed while copying")
        if bundle_architectures(copied_updater, command=command) != updater_architectures:
            raise ValueError("Updater architectures changed while copying")
        verify_app(copied_updater, development=args.development, team_id=args.team_id,
                   command=command)
        scripts = stage / "scripts"
        scripts.mkdir()
        (scripts / "postinstall").write_text(postinstall_script(
            app_id, app_info["CFBundleShortVersionString"],
            app_info["CFBundleIdentifier"], keystone_name))
        (scripts / "postinstall").chmod(0o755)
        preinstall = architecture_preinstall(architectures)
        if args.development:
            # These artifacts exercise packaging without making an ad-hoc root
            # daemon installable by accidentally opening the PKG in Installer.
            preinstall += 'echo "Development artifact: root installation is disabled." >&2\nexit 1\n'
        (scripts / "preinstall").write_text(preinstall)
        (scripts / "preinstall").chmod(0o755)
        component = stage / "component.pkg"
        components = stage / "components.plist"
        command(["/usr/bin/pkgbuild", "--analyze", "--root", payload, components])
        with components.open("rb") as source:
            component_properties = plistlib.load(source)
        for bundle in component_properties:
            bundle.update(BundleIsRelocatable=False, BundleIsVersionChecked=False,
                          BundleHasStrictIdentifier=False, BundleOverwriteAction="upgrade")
        components.write_bytes(plistlib.dumps(component_properties))
        command(["/usr/bin/pkgbuild", "--root", payload, "--component-plist", components,
                 "--install-location", "/" if args.include_app else STAGING,
                 "--identifier", package_id, "--version",
                 updater_info["CFBundleShortVersionString"], "--ownership", "recommended",
                 "--scripts", scripts, component])
        distribution = stage / "Distribution"
        distribution.write_text(distribution_xml(
            package_id, updater_info["CFBundleShortVersionString"],
            architectures,
            app_info["CFBundleIdentifier"] if args.include_app else None))
        package = stage / "XenonUpdater.pkg"
        product_args = ["/usr/bin/productbuild", "--distribution", distribution,
                        "--package-path", stage]
        if not args.development:
            product_args += ["--sign", args.installer_identity, "--timestamp"]
        command([*product_args, package])
        if not package.is_file() or package.stat().st_size == 0:
            raise ValueError("productbuild did not produce a package")
        if not args.development:
            signature = command(["/usr/sbin/pkgutil", "--check-signature", package]).decode()
            if not re.search(r"Developer ID Installer: .+\(" +
                             re.escape(args.team_id) + r"\)", signature):
                raise ValueError("PKG was not signed by the requested Developer ID Installer team")
            command(["/usr/bin/xcrun", "notarytool", "submit", package,
                     "--keychain-profile", args.notary_profile, "--wait"])
            command(["/usr/bin/xcrun", "stapler", "staple", package])
            command(["/usr/bin/xcrun", "stapler", "validate", package])
        metadata = {"artifact": str(output), "sha256": sha256(package),
                    "size": package.stat().st_size, "scope": "system",
                    "development": args.development, "app_id": app_id,
                    "includes_app": args.include_app,
                    "architectures": architecture_names(architectures),
                    "app_version": app_info["CFBundleShortVersionString"],
                    "updater_version": updater_info["CFBundleShortVersionString"],
                    "install_root": UPDATER_ROOT, "registered_path": TARGET}
        publish_file(package, output)
    return metadata


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--updater-app", required=True)
    result.add_argument("--app", required=True)
    result.add_argument("--app-id", required=True)
    result.add_argument("--updater-id", required=True)
    result.add_argument("--keystone-name", default="XenonSoftwareUpdate")
    result.add_argument("--identifier", default="com.xenon.updater.pkg")
    result.add_argument("--out", required=True)
    result.add_argument("--include-app", action="store_true",
                        help="Install xl153 and the Updater in one component (recommended for initial install)")
    result.add_argument("--development", action="store_true")
    result.add_argument("--team-id")
    result.add_argument("--installer-identity")
    result.add_argument("--notary-profile")
    return result


if __name__ == "__main__":
    try:
        print(json.dumps(build_package(parser().parse_args()), indent=2))
    except (ValueError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
