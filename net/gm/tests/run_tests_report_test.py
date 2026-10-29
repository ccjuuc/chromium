# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Coverage-report regressions. Native subprocesses are always mocked."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "gm_test_runner", Path(__file__).resolve().parents[1] / "run_tests.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

UNIT_TESTS = [
    "GmPinDialogTest.SelectedCertificateAndPinAreReturnedOnce",
    "InvalidPin/GmPinDialogInvalidInputTest.InvalidInputDoesNotReturnAnAuthorizedPin/0",
]
NAVIGATION_TESTS = [
    "NationalTls/GmBrowserTest.NavigationAndReconnect/JointTls13SimulatedKey",
    "NationalTls/GmCancellationBrowserTest.ClosingPageDiscardsLatePin/JointTls13SimulatedKey",
    "NationalTls/GmAutomaticRejectionBrowserTest.GenericTlcpAlertStopsDiscovery/SimulatedKey",
    "GmPinDialogBrowserTest.PageModalAcceptReturnsSelectedPinOnce",
    "GmPinDialogBrowserTest.ClosingOwningTabCancelsPendingPinOnce",
    "GmPinDialogBrowserTest.ClosingOwningBrowserCancelsPendingPinOnce",
    "GmPinDialogBrowserTest.LockedNoticeIsPageModalAndClosesOnce",
]


def summary(names, executed=None, status="SUCCESS", disabled=()):
    return {
        "all_tests": names,
        "disabled_tests": list(disabled),
        "per_iteration_data": [{
            name: [{"status": status}]
            for name in (names if executed is None else executed)
        }],
    }


class CoverageReportTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.build = Path(self.directory.name)

    def read(self, data, pattern="*"):
        path = self.build / "summary.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return runner.read_launcher_summary(path, pattern)

    def flags(self, **layers):
        return runner.coverage_flags(
            [{"name": name, "exit_code": 0} for name in layers],
            {name: self.read(data) for name, data in layers.items()})

    def run_main(self, names, execute, environment=None):
        argv = ["run_tests.py", "--build-dir", str(self.build)]
        for name in names:
            argv.extend(["--only", name])
        with (mock.patch.object(runner.sys, "argv", argv),
              mock.patch.object(runner.sys, "platform", "win32"),
              mock.patch.object(runner.subprocess, "run", side_effect=execute),
              mock.patch.dict(runner.os.environ, environment or {}),
              contextlib.redirect_stdout(io.StringIO())):
            code = runner.main()
        return code, json.loads(
            (self.build / "gm-test-results.json").read_text(encoding="utf-8"))

    def test_unit_success_does_not_claim_page_modal_coverage(self):
        flags = self.flags(browser_unit=summary(UNIT_TESTS))
        self.assertTrue(flags["native_pin_dialog_unit_tested"])
        self.assertFalse(flags["page_modal_pin_dialog_tested"])
        self.assertFalse(flags["native_pin_dialog_tested"])
        self.assertFalse(flags["browser_navigation_end_to_end_tested"])

    def test_page_modal_success_does_not_claim_widget_unit_coverage(self):
        flags = self.flags(browser_navigation=summary(NAVIGATION_TESTS))
        self.assertTrue(flags["page_modal_pin_dialog_tested"])
        self.assertTrue(flags["browser_navigation_end_to_end_tested"])
        self.assertFalse(flags["native_pin_dialog_unit_tested"])
        self.assertFalse(flags["native_pin_dialog_tested"])

    def test_both_layers_are_required_for_complete_native_ui_coverage(self):
        flags = self.flags(browser_unit=summary(UNIT_TESTS),
                           browser_navigation=summary(NAVIGATION_TESTS))
        self.assertTrue(flags["native_pin_dialog_tested"])

    def test_missing_skipped_or_failed_target_test_prevents_coverage(self):
        variants = [summary(UNIT_TESTS, executed=UNIT_TESTS[:1]),
                    summary(UNIT_TESTS, status="SKIPPED"),
                    summary(UNIT_TESTS, status="FAILURE"),
                    summary(UNIT_TESTS[:1])]
        retried = summary(UNIT_TESTS)
        retried["per_iteration_data"][0][UNIT_TESTS[0]].insert(
            0, {"status": "FAILURE"})
        variants.append(retried)
        for data in variants:
            with self.subTest(data=data):
                self.assertFalse(self.flags(browser_unit=data)[
                    "native_pin_dialog_unit_tested"])

    def test_disabled_only_suite_cannot_prove_coverage(self):
        data = summary(UNIT_TESTS, disabled=UNIT_TESTS)
        self.assertFalse(self.flags(browser_unit=data)[
            "native_pin_dialog_unit_tested"])

    def test_every_iteration_requires_success(self):
        data = summary(UNIT_TESTS)
        data["per_iteration_data"].append({})
        self.assertFalse(self.flags(browser_unit=data)[
            "native_pin_dialog_unit_tested"])

    def test_content_filter_excludes_unrequested_registered_tests(self):
        target = "StoragePartitionImplGmCertificateTest.CancelsOnRequestDisconnect"
        data = summary([target, "OtherSuite.Unrelated"], executed=[target])
        counts, expected, successful = self.read(
            data, "StoragePartitionImplGmCertificateTest.*")
        self.assertEqual(counts["enabled_tests"], 1)
        self.assertEqual(expected, {target})
        self.assertEqual(expected, successful)

    def test_malformed_inventory_or_iterations_are_rejected(self):
        for data in ([], {}, {"all_tests": [1], "per_iteration_data": [{}]},
                     {"all_tests": UNIT_TESTS, "per_iteration_data": []},
                     {"all_tests": UNIT_TESTS, "per_iteration_data": [None]}):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.read(data)

    def test_old_summary_is_removed_and_missing_fresh_evidence_fails(self):
        path = self.build / "gm-browser-unit-tests.json"
        path.write_text(json.dumps(summary(UNIT_TESTS)), encoding="utf-8")
        settings = {name: "1" for name in runner.TEST_SCOPE_ENVIRONMENT}

        def execute(command, **kwargs):
            self.assertFalse(path.exists())
            for name in settings:
                self.assertNotIn(name, kwargs["env"])
            return subprocess.CompletedProcess(command, 0, "", "")

        code, report = self.run_main(["browser_unit"], execute, settings)
        self.assertEqual(code, 1)
        self.assertFalse(report["native_pin_dialog_unit_tested"])
        self.assertIn("summary_error", report["results"][0])
        self.assertEqual(set(report["ignored_test_environment_variables"]),
                         set(settings))

    def test_fresh_full_summaries_supply_counts_and_separate_coverage(self):
        def execute(command, **kwargs):
            path = runner.launcher_summary_path(command)
            self.assertFalse(path.exists())
            names = UNIT_TESTS if "unit" in path.name else NAVIGATION_TESTS
            path.write_text(json.dumps(summary(names)), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "", "")

        code, report = self.run_main(
            ["browser_unit", "browser_navigation"], execute)
        self.assertEqual(code, 0)
        self.assertTrue(report["native_pin_dialog_tested"])
        self.assertTrue(report["native_pin_dialog_unit_tested"])
        self.assertTrue(report["page_modal_pin_dialog_tested"])
        self.assertEqual(report["results"][0]["test_summary"]["executed_tests"],
                         len(UNIT_TESTS))

    def test_zero_exit_for_a_partial_run_is_not_a_layer_pass(self):
        def execute(command, **kwargs):
            runner.launcher_summary_path(command).write_text(json.dumps(
                summary(NAVIGATION_TESTS, executed=NAVIGATION_TESTS[3:])),
                encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "", "")

        code, report = self.run_main(["browser_navigation"], execute)
        self.assertEqual(code, 1)
        self.assertFalse(report["browser_navigation_end_to_end_tested"])
        self.assertFalse(report["page_modal_pin_dialog_tested"])
        self.assertEqual(len(report["results"][0]["test_summary"]["missing_tests"]), 3)

    def test_unrequested_old_summary_is_never_used(self):
        (self.build / "gm-browser-unit-tests.json").write_text(
            json.dumps(summary(UNIT_TESTS)), encoding="utf-8")

        def execute(command, **kwargs):
            return subprocess.CompletedProcess(command, 0, "", "")

        code, report = self.run_main(["connection_profile"], execute)
        self.assertEqual(code, 0)
        self.assertFalse(report["native_pin_dialog_unit_tested"])


if __name__ == "__main__":
    unittest.main()
