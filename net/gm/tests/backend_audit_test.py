# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Regression checks for the exported ABI and native dependency audit."""

import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "backend_audit", Path(__file__).resolve().parents[1] / "build_backend.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class BackendAuditTest(unittest.TestCase):
    def test_pe_exports(self):
        exports = """
          ordinal hint RVA      name
                1    0 00012340 gm_backend_get_api
        """
        audit.audit_surface(audit.pe_symbols(exports),
                            ["CRYPT32.dll", "VCRUNTIME140.dll"])

    def test_elf_exports(self):
        audit.audit_surface(audit.posix_symbols("gm_backend_get_api T 1234 10\n"),
                            ["libstdc++.so.6", "libc.so.6"])

    def test_macho_exports(self):
        audit.audit_surface(
            audit.posix_symbols("_gm_backend_get_api T 00001234 0\n", mac=True),
            ["/usr/lib/libc++.1.dylib", "/usr/lib/libSystem.B.dylib"])

    def test_reject_leaked_crypto_or_cpp_symbols(self):
        for unexpected in ("SSL_new", "EVP_Digest", "_ZN3std6vector"):
            with self.subTest(symbol=unexpected), self.assertRaises(ValueError):
                audit.audit_surface(["gm_backend_get_api", unexpected], [])

    def test_reject_missing_or_duplicate_entry_point(self):
        for exports in ([], ["gm_backend_get_api", "gm_backend_get_api"]):
            with self.subTest(exports=exports), self.assertRaises(ValueError):
                audit.audit_surface(exports, [])

    def test_reject_shared_crypto_on_every_platform(self):
        for dependency in ("libssl-3-x64.dll", "libcrypto-3-x64.dll",
                           "LIBCRYPTO-3-X64.DLL", "libcrypto.so.3",
                           "libssl.so.3", "@rpath/libssl.3.dylib",
                           "/usr/local/lib/libcrypto.3.dylib"):
            with self.subTest(dependency=dependency), self.assertRaises(ValueError):
                audit.audit_surface(["gm_backend_get_api"], [dependency])


if __name__ == "__main__":
    unittest.main()
