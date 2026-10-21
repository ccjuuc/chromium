#!/usr/bin/env python3
"""Build macOS application PKG, signed update CRX3 and Omaha release metadata.

Optionally include a browser + system Updater installer. Inputs must already be
signed; this script only packages artifacts and never installs or starts them.
The output directory must be new and appears only after all checks succeed.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

import make_system_updater_pkg as system_pkg
import make_updater_crx as crx
from updater_packaging_common import app_metadata, parse_sha256_pin, run, sha256, verify_app

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[2]


def executable(value):
    found = shutil.which(str(value))
    if not found:
        raise ValueError(f"Required executable not found: {value}")
    return Path(found).resolve()


def build_packages(args, *, command=run):
    if sys.platform != "darwin":
        raise ValueError("macOS packaging requires macOS")
    if args.development:
        if args.team_id or args.installer_identity or args.notary_profile:
            raise ValueError("Development mode cannot use release signing options")
    elif not all((args.team_id, args.installer_identity, args.notary_profile)):
        raise ValueError("Release mode requires --team-id, --installer-identity and --notary-profile")
    if not 1 <= args.wait_seconds <= 600:
        raise ValueError("--wait-seconds must be between 1 and 600")
    output = Path(args.out_dir).absolute()
    if os.path.lexists(output):
        raise ValueError("--out-dir must not already exist; preserve previously published CRXs")
    app = Path(args.app).absolute()
    updater = Path(args.updater_app).absolute() if args.updater_app else None
    if app.name != "xl153.app":
        raise ValueError("--app must name xl153.app")
    if updater and updater.name != "XenonUpdater.app":
        raise ValueError("--updater-app must name XenonUpdater.app")
    for source in (app, updater):
        if source and output.resolve().is_relative_to(source.resolve()):
            raise ValueError("Output must be outside the input application bundles")
    tools_dir = Path(args.tools_dir).absolute()
    crx_tool = executable(tools_dir / "crx3_build_action")
    verifier = executable(tools_dir / "xenon_updater_verify")
    node = executable(args.node)
    pin = parse_sha256_pin(args.expected_key_sha256).hex()
    key = Path(args.key).absolute()
    crx.public_key(key, pin, command=command)
    info = app_metadata(app)
    if info["CFBundleExecutable"] != "xl153":
        raise ValueError("Browser executable must be xl153")
    for source in (app, updater):
        if source:
            app_metadata(source)
            verify_app(source, development=args.development, team_id=args.team_id,
                       command=command)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".xenon-package-", dir=output.parent) as temporary:
        stage = Path(temporary)
        snapshots = stage / "inputs"
        snapshots.mkdir()
        for source in (app, updater):
            if source:
                destination = snapshots / source.name
                command(["/usr/bin/ditto", source, destination])
                if app_metadata(destination) != app_metadata(source):
                    raise ValueError("Application metadata changed while copying")
                verify_app(destination, development=args.development, team_id=args.team_id,
                           command=command)
        app = snapshots / app.name
        if app_metadata(app) != info:
            raise ValueError("Browser metadata changed while copying")
        architectures = system_pkg.bundle_architectures(app, command=command)
        if updater:
            updater = snapshots / updater.name
            updater_info = app_metadata(updater)
            if (updater_info["CFBundleIdentifier"] != "com.xenon.XenonUpdater" or
                    updater_info["CFBundleExecutable"] != "XenonUpdater"):
                raise ValueError("Updater identity or executable does not match Xenon branding")
            architectures &= system_pkg.bundle_architectures(updater, command=command)
        architecture = args.arch
        if not architecture and len(architectures) == 1:
            architecture = next(iter(architectures))
        if not architecture or architecture not in architectures:
            raise ValueError("Select --arch from the architectures supported by every input: " +
                             ", ".join(sorted(architectures)))
        artifacts = stage / "artifacts"
        artifacts.mkdir()
        product_version = info["CFBundleShortVersionString"]
        package = artifacts / f"application-{product_version}.pkg"
        update = artifacts / f"update-{product_version}.crx3"
        print("Building application PKG...", file=sys.stderr)
        package_command = [node, SCRIPT_DIR / "make_mac_pkg.js", "--app", app,
                           "--out", package]
        if not args.development:
            package_command += ["--sign", args.installer_identity]
        command(package_command)
        if not args.development:
            print("Notarizing application PKG...", file=sys.stderr)
            command(["/usr/bin/xcrun", "notarytool", "submit", package,
                     "--keychain-profile", args.notary_profile, "--wait"])
            command(["/usr/bin/xcrun", "stapler", "staple", package])
        print("Building and verifying signed CRX3...", file=sys.stderr)
        crx_metadata = crx.build_crx(argparse.Namespace(
            app=app, pkg=package, key=key, expected_key_sha256=pin, crx_tool=crx_tool,
            out=update, wait_seconds=args.wait_seconds, development=args.development,
            team_id=args.team_id), command=command)
        command([verifier, f"--crx={update}", f"--key-sha256={pin}",
                 f"--file-sha256={crx_metadata['sha256']}"])
        initial_metadata = None
        if updater:
            print("Building combined browser and system Updater PKG...", file=sys.stderr)
            initial_metadata = system_pkg.build_package(argparse.Namespace(
                app=app, updater_app=updater, app_id=info["CFBundleIdentifier"],
                updater_id="com.xenon.XenonUpdater", keystone_name="XenonSoftwareUpdate",
                identifier="com.xenon.updater.pkg", include_app=True,
                out=artifacts / f"xl153-with-updater-{product_version}.pkg",
                development=args.development, team_id=args.team_id,
                installer_identity=args.installer_identity,
                notary_profile=args.notary_profile), command=command)
            initial_metadata["artifact"] = Path(initial_metadata["artifact"]).name
        crx_metadata["artifact"] = update.name
        manifest = {"schema_version": 1, "appid": info["CFBundleIdentifier"],
                    "version": product_version, "platform": "mac", "arch": architecture,
                    "full": {"file": update.name, "size": crx_metadata["size"],
                             "sha256": crx_metadata["sha256"]}}
        metadata = {"schema_version": 1, "development": args.development,
                    "application_pkg": {"file": package.name, "sha256": sha256(package),
                                        "size": package.stat().st_size},
                    "crx": crx_metadata, "system_installer": initial_metadata}
        for name, contents in (("release.json", manifest), ("artifacts.json", metadata)):
            (artifacts / name).write_text(json.dumps(contents, indent=2) + "\n")
        if os.path.lexists(output):
            raise ValueError("Output directory appeared during packaging; refusing to replace it")
        artifacts.rename(output)
    return {"output": str(output), "version": product_version,
            "development": args.development, "manifest": str(output / "release.json"),
            "metadata": str(output / "artifacts.json")}


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--app", required=True, help="Already signed xl153.app")
    result.add_argument("--updater-app", help="Also build an initial installer with XenonUpdater.app")
    result.add_argument("--key", required=True, help="Private RSA key, DER PKCS#8, mode 0600")
    result.add_argument("--expected-key-sha256", required=True,
                        help="Public SPKI SHA-256, hex or base64; must match the client's GN pin")
    result.add_argument("--out-dir", required=True, help="New output directory; never overwritten")
    result.add_argument("--tools-dir", default=str(ROOT / "out/release_arm64"),
                        help="Directory containing crx3_build_action and xenon_updater_verify")
    result.add_argument("--node", default="node", help="Node.js executable")
    result.add_argument("--arch", choices=("arm64", "x86_64"),
                        help="Omaha target architecture; inferred for single-architecture bundles")
    result.add_argument("--wait-seconds", type=int, default=180)
    result.add_argument("--development", action="store_true",
                        help="Ad-hoc artifacts; the combined system installer blocks installation")
    result.add_argument("--team-id")
    result.add_argument("--installer-identity")
    result.add_argument("--notary-profile")
    return result


if __name__ == "__main__":
    try:
        print(json.dumps(build_packages(parser().parse_args()), indent=2))
    except (ValueError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
