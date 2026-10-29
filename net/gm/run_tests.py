# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Run native GM checks and retain exact per-layer results.

The default includes BoringSSL, facade interoperability and simulated device-process tests. Chromium
network/browser tests, measurements and independent GmSSL interoperability are
explicit options. Software simulation never claims physical USBKey acceptance.
Selected layers run in full; inherited Google Test filtering, sharding and
repetition settings are ignored and recorded in the report.
"""

import argparse
import fnmatch
import json
import os
from pathlib import Path
import subprocess
import sys
import time


# This runner selects complete layers through its CLI. Inherited Google Test
# settings must not silently turn one layer into a filtered, sharded or empty run.
TEST_SCOPE_ENVIRONMENT = (
    "GTEST_FILTER", "GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX",
    "GTEST_SHARD_STATUS_FILE", "GTEST_REPEAT", "GTEST_LIST_TESTS",
)


def launcher_summary_path(command):
    prefix = "--test-launcher-summary-output="
    for argument in command:
        if str(argument).startswith(prefix):
            return Path(str(argument)[len(prefix):])
    return None


def read_launcher_summary(path, pattern="*"):
    """Return current-run counts and the registered/successful enabled tests."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Test summary must be an object")
    registered = data.get("all_tests")
    disabled = data.get("disabled_tests", [])
    iterations = data.get("per_iteration_data")
    if (not isinstance(registered, list) or
            not all(isinstance(name, str) for name in registered) or
            not isinstance(disabled, list) or
            not all(isinstance(name, str) for name in disabled) or
            not isinstance(iterations, list) or not iterations or
            not all(isinstance(run, dict) for run in iterations)):
        raise ValueError("Test summary is missing its test inventory or results")
    expected = {name for name in set(registered) - set(disabled)
                if fnmatch.fnmatchcase(name, pattern)}
    executed = set()
    successful = set(expected)
    for run in iterations:
        for name in expected:
            attempts = run.get(name)
            if isinstance(attempts, list) and attempts:
                executed.add(name)
            if (not isinstance(attempts, list) or not attempts or
                    not all(isinstance(attempt, dict) and
                            attempt.get("status") == "SUCCESS"
                            for attempt in attempts)):
                successful.discard(name)
    counts = {"enabled_tests": len(expected),
              "executed_tests": len(executed),
              "successful_tests": len(successful),
              "iterations": len(iterations),
              "missing_tests": sorted(expected - executed),
              "unsuccessful_tests": sorted(executed - successful)}
    return counts, expected, successful


def coverage_flags(results, summaries):
    """Require evidence for every enabled test in each named target suite."""
    passed_layers = {result["name"] for result in results
                     if result["exit_code"] == 0}

    def suites_passed(layer, suites):
        if layer not in passed_layers or layer not in summaries:
            return False
        _, expected, successful = summaries[layer]
        for suite in suites:
            tests = {name for name in expected
                     if name.split(".", 1)[0] == suite}
            if not tests or not tests.issubset(successful):
                return False
        return True

    native_unit = suites_passed(
        "browser_unit", ("GmPinDialogTest",
                         "InvalidPin/GmPinDialogInvalidInputTest"))
    page_modal = suites_passed("browser_navigation", ("GmPinDialogBrowserTest",))
    return {
        "content_request_lifecycle_tested": suites_passed(
            "content", ("StoragePartitionImplGmCertificateTest",)),
        "browser_navigation_end_to_end_tested": suites_passed(
            "browser_navigation", ("NationalTls/GmBrowserTest",
                                   "NationalTls/GmCancellationBrowserTest",
                                   "NationalTls/GmAutomaticRejectionBrowserTest")),
        "native_pin_dialog_unit_tested": native_unit,
        "page_modal_pin_dialog_tested": page_modal,
        # Preserve the old field conservatively: complete native UI acceptance
        # requires both widget behavior and its real WebContents ownership.
        "native_pin_dialog_tested": native_unit and page_modal,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--include-network", action="store_true")
    parser.add_argument("--include-content", action="store_true")
    parser.add_argument("--include-browser", action="store_true")
    parser.add_argument("--include-benchmark", action="store_true")
    parser.add_argument("--interop-work-dir", type=Path)
    parser.add_argument("--cmake")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--only", action="append", choices=[
        "connection_profile", "skf_adapter", "key_service", "tls_backend",
        "crypto", "ssl", "pki", "network", "network_service", "content", "browser_unit",
        "browser_navigation", "benchmark", "crypto_benchmark",
        "independent_interop"])
    args = parser.parse_args()
    if sys.platform not in ("win32", "darwin", "linux"):
        parser.error("GM checks require Windows, macOS, or Linux")
    windows = sys.platform == "win32"
    executable_suffix = ".exe" if windows else ""
    build = args.build_dir.resolve()
    backend = build / "gm_interop_peer"
    backend_library = build / "gm_boringssl_test_backend.dll"
    if not build.is_dir() or args.timeout <= 0:
        parser.error("An existing build directory and positive timeout are required")
    backend.mkdir(exist_ok=True)
    root = backend / "test-root.der"
    # Neither wrapper initialization nor provider loading may consult these.
    config = backend / "must-not-load-openssl.cnf"
    config.write_text("[This configuration must never be loaded\n", encoding="ascii")
    environment = os.environ.copy()
    ignored_test_environment = [name for name in TEST_SCOPE_ENVIRONMENT
                                if environment.pop(name, None) is not None]
    environment["OPENSSL_CONF"] = str(config)
    environment["OPENSSL_MODULES"] = str(backend / "missing-provider-directory")
    commands = {
        "connection_profile": [build / ("gm_connection_profile_test" + executable_suffix)],
        "skf_adapter": [build / "gm_skf_test.exe", build / "gm_fake_skf.dll"],
        "key_service": [build / "gm_key_service_tests.exe", "--gtest_output=xml:" + str(build / "gm-key-service-tests.xml")],
        "tls_backend": [backend / ("gm_backend_smoke" + executable_suffix), backend_library, root],
        **{part: [build / ("boringssl_" + part + "_tests" + executable_suffix),
                  "--test-launcher-retry-limit=0",
                  "--test-launcher-summary-output=" + str(build / ("gm-boringssl-" + part + ".json"))]
           for part in ("crypto", "ssl", "pki")},
        "network": [build / ("gm_net_unittests" + executable_suffix), "--test-launcher-retry-limit=0",
                    "--test-launcher-summary-output=" + str(build / "gm-network-tests.json")],
        "network_service": [build / ("gm_network_service_unittests" + executable_suffix), "--test-launcher-retry-limit=0",
                            "--test-launcher-summary-output=" + str(build / "gm-network-service-tests.json")],
        "content": [build / "gm_content_unittests.exe", "--test-launcher-retry-limit=0",
                    "--test-launcher-jobs=1",
                    "--gtest_filter=StoragePartitionImplGmCertificateTest.*",
                    "--test-launcher-summary-output=" + str(build / "gm-content-tests.json")],
        "browser_unit": [build / "gm_browser_unittests.exe", "--test-launcher-retry-limit=0",
                         "--test-launcher-jobs=1",
                         "--test-launcher-summary-output=" + str(build / "gm-browser-unit-tests.json")],
        "browser_navigation": [build / "gm_browser_browsertests.exe", "--test-launcher-retry-limit=0",
                               "--test-launcher-jobs=1",
                               "--test-launcher-summary-output=" + str(build / "gm-browser-navigation-tests.json")],
        "benchmark": [backend / ("gm_backend_smoke" + executable_suffix), "--benchmark", backend_library, "200"],
        "crypto_benchmark": [build / ("gm_crypto_benchmark" + executable_suffix)],
    }
    names = ["connection_profile", "crypto", "ssl", "pki"]
    if windows:
        names += ["skf_adapter", "key_service", "tls_backend"]
    if args.include_network:
        names += ["network", "network_service"]
    if args.include_content or args.include_browser:
        names += ["content"]
    if args.include_browser:
        names += ["browser_unit", "browser_navigation"]
    if args.include_benchmark:
        names += ["crypto_benchmark"]
        if windows:
            names += ["benchmark"]
    if args.interop_work_dir:
        commands["independent_interop"] = [sys.executable, Path(__file__).parent / "tests/run_gmssl_interop.py",
                                           "--work-dir", args.interop_work_dir.resolve(),
                                           "--backend", backend_library,
                                           "--test-peer", backend / "gm_backend_smoke.exe"]
        if args.cmake:
            commands["independent_interop"] += ["--cmake", args.cmake]
        names += ["independent_interop"]
    if args.only:
        names = list(dict.fromkeys(args.only))
    windows_only = {"tls_backend", "benchmark", "skf_adapter", "key_service", "content", "browser_unit",
                    "browser_navigation", "independent_interop"}
    if not windows and windows_only.intersection(names):
        parser.error("These integration layers currently require Windows: " +
                     ", ".join(sorted(windows_only.intersection(names))))
    if "independent_interop" in names and not args.interop_work_dir:
        parser.error("independent_interop requires --interop-work-dir")
    (build / "gm-test-results.json").unlink(missing_ok=True)
    results = []
    summaries = {}
    for name in names:
        command = commands[name]
        summary = launcher_summary_path(command)
        summary_cleared = False
        timeout = max(args.timeout, 600) if name in ("independent_interop", "browser_navigation") else args.timeout
        start = time.monotonic()
        try:
            if summary is not None:
                summary.unlink(missing_ok=True)
                summary_cleared = True
            completed = subprocess.run([str(value) for value in command], cwd=build,
                                       env=environment, capture_output=True,
                                       text=True, encoding="utf-8", errors="replace", timeout=timeout)
            result = {"name": name, "exit_code": completed.returncode,
                      "stdout": completed.stdout, "stderr": completed.stderr}
        except subprocess.TimeoutExpired as error:
            def output_text(value):
                if isinstance(value, bytes):
                    return value.decode("utf-8", errors="replace")
                return value or ""

            result = {"name": name, "exit_code": -1,
                      "error": str(timeout) + " second timeout",
                      "stdout": output_text(error.stdout),
                      "stderr": output_text(error.stderr)}
        except OSError as error:
            result = {"name": name, "exit_code": -1, "error": str(error)}
        if summary_cleared:
            try:
                pattern = ("StoragePartitionImplGmCertificateTest.*"
                           if name == "content" else "*")
                evidence = read_launcher_summary(summary, pattern)
                summaries[name] = evidence
                result["test_summary"] = evidence[0]
                _, expected, successful = evidence
                if not expected or expected != successful:
                    raise ValueError("Not every enabled test completed successfully")
            except (OSError, ValueError) as error:
                result["summary_error"] = str(error)
                if result["exit_code"] == 0:
                    result["exit_code"] = -1
                    result["error"] = "Missing, invalid or incomplete current-run test summary"
        result["elapsed_seconds"] = round(time.monotonic() - start, 3)
        results.append(result)
        if name in ("benchmark", "crypto_benchmark") and result["exit_code"] == 0:
            try:
                measurements = json.loads(result["stdout"])
                report_name = "gm-" + name.replace("_", "-") + ".json"
                (build / report_name).write_text(json.dumps(measurements, indent=2) + "\n")
            except ValueError:
                result.update(exit_code=-1, error="Benchmark did not return valid measurements")
        print(name + ": " + ("PASS" if result["exit_code"] == 0 else "FAIL"))
        if result["exit_code"]:
            print(result.get("stderr", "") + result.get("stdout", "") + result.get("error", ""))
    report = {"architecture": "single-boringssl",
              "platform": sys.platform,
              "physical_usbkey_tested": False,
              **coverage_flags(results, summaries),
              # This describes the navigation authentication fixture. The
              # separate page-modal tests exercise the actual native dialog.
              "browser_navigation_uses_injected_pin_callback": True,
              "ignored_test_environment_variables": ignored_test_environment,
              "requested_layers": names,
              "invalid_openssl_environment_supplied": True, "results": results}
    if args.interop_work_dir and "independent_interop" in names:
        interop = args.interop_work_dir.resolve() / "interop-report.json"
        if interop.is_file():
            report["independent_interop"] = json.loads(interop.read_text(encoding="utf-8"))
    (build / "gm-test-results.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 1 if any(result["exit_code"] for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
