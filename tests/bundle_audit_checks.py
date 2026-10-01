#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise bundle dependency parsing without requiring real external libraries."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("bundle_audit", Path(__file__).parents[1] / "scripts/audit_bundle.py")
auditor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(auditor)


class BundleAuditChecks(unittest.TestCase):
    def test_universal_headers_are_not_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "Example.app"
            binary = bundle / "Contents/MacOS/Example"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(bytes.fromhex("cafebabe"))
            dependencies = (f"{binary} (architecture x86_64):\n"
                            "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
                            f"{binary} (architecture arm64):\n"
                            "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n")
            with patch.object(auditor.subprocess, "check_output",
                              side_effect=lambda args, **kwargs: dependencies if args[1] == "-L" else ""):
                report = auditor.audit(bundle)
            self.assertEqual(report["errors"], [])
            self.assertTrue(report["files"][0]["dependencies"])
            self.assertEqual(set(report["files"][0]["dependencies"]), {"/usr/lib/libSystem.B.dylib"})

    def test_external_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "Example.app"
            binary = bundle / "Contents/MacOS/Example"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(bytes.fromhex("cffaedfe"))
            for library in ("/opt/developer/libMissing.dylib",
                            "/usr/lib/../../opt/homebrew/lib/libX.dylib",
                            "/System/Library/../../../opt/homebrew/lib/libX.dylib"):
                with self.subTest(library=library):
                    dependencies = f"{binary}:\n\t{library} (compatibility version 1.0.0)\n"
                    with patch.object(auditor.subprocess, "check_output",
                                      side_effect=lambda args, **kwargs: dependencies if args[1] == "-L" else ""):
                        report = auditor.audit(bundle)
                    self.assertTrue(any("unresolved or external" in error for error in report["errors"]))

    def test_system_rpath_prefix_is_not_a_system_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "Example.app"
            binary = bundle / "Contents/MacOS/Example"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(bytes.fromhex("cffaedfe"))
            for root, allowed in (("/usr/lib-private", False), ("/usr/library", False),
                                  ("/usr/lib", True), ("/usr/lib/swift", True), ("/System/Library", True)):
                with self.subTest(root=root):
                    commands = f"cmd LC_RPATH\ncmdsize 32\npath {root} (offset 12)\n"
                    with patch.object(auditor.subprocess, "check_output",
                                      side_effect=lambda args, **kwargs: str(binary) + ":\n" if args[1] == "-L" else commands):
                        report = auditor.audit(bundle)
                    self.assertEqual(not report["errors"], allowed)


if __name__ == "__main__":
    unittest.main(verbosity=2)
