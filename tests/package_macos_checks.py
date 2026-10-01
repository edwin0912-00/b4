#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise release packaging gates with tiny synthetic bundles and temporary Git trees."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location("package_macos", ROOT / "scripts/package_macos.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PackageFixture:
    def __init__(self, base, runtime_change=None):
        self.base = Path(base)
        self.root = self.base / "repo"
        self.root.mkdir()
        self.app = self.base / "Before Effects.app"
        self.exe = self.app / "Contents/MacOS/Before Effects"
        core = self.app / "Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
        plugin = self.app / "Contents/PlugIns/platforms/libqcocoa.dylib"
        self.exe.parent.mkdir(parents=True)
        core.parent.mkdir(parents=True)
        plugin.parent.mkdir(parents=True)
        self.exe.write_bytes(b"app-executable")
        core.write_bytes(bytes.fromhex("cffaedfe") + b"core")
        plugin.write_bytes(bytes.fromhex("cffaedfe") + b"plugin")
        info = self.app / "Contents/Info.plist"
        info.write_bytes(plistlib.dumps({
            "CFBundleExecutable": "Before Effects",
            "CFBundleIdentifier": "org.independent.beforeeffects",
            "CFBundleShortVersionString": "0.7.0",
        }))
        framework_info = core.parents[2] / "Resources/Info.plist"
        framework_info.parent.mkdir(parents=True)
        framework_info.write_bytes(plistlib.dumps({"CFBundleVersion": "6.12.0"}))

        self.notices = self.root / "docs/third-party/notices"
        self.notices.mkdir(parents=True)
        notice_bytes = b"reviewed license notice\n"
        (self.notices / "notice.txt").write_bytes(notice_bytes)
        self.notice_map = {"docs/third-party/notices/notice.txt": sha(notice_bytes)}
        self.summary = b"Third-party software summary\n"
        self._write("THIRD-PARTY-NOTICES.md", self.summary)
        self.license = self._write("LICENSE", b"MPL-2.0\n")
        self.examples = self.root / "fixtures"
        self.examples.mkdir()
        self._write("fixtures/example.json", b"{}\n")
        self._write("docs/public/BUILD.md", b"build docs\n")
        self._write("docs/public/RELEASE.md", b"release docs\n")
        self._write("docs/public/FEATURES.md", b"features\n")
        self._write("scripts/audit_bundle.py", b"auditor\n")
        self._write("scripts/package_macos.py", b"packager\n")

        self.runtime = {
            "schema": 1,
            "qt_version": "6.12.0",
            "notices_index": "THIRD-PARTY-NOTICES.md",
            "notice_files": self.notice_map,
            "components": [
                {"bundle_path": "Contents/Frameworks/QtCore.framework/Versions/A/QtCore", "version": "6.12.0"},
                {"bundle_path": "Contents/PlugIns/platforms/libqcocoa.dylib", "version": "6.12.0"},
            ],
        }
        if runtime_change:
            runtime_change(self.runtime)
        self._write("docs/third-party/public-runtime.json", json.dumps(self.runtime, indent=2).encode())
        embedded = self.app / "Contents/Resources/Third-party"
        (embedded / "Notices").mkdir(parents=True)
        (embedded.parent / "LICENSE").write_bytes(self.license.read_bytes())
        (embedded / "Notices/notice.txt").write_bytes(notice_bytes)
        (embedded / "THIRD-PARTY-NOTICES.md").write_bytes(self.summary)

        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(self.root), "config", "user.name", "Package Test"], check=True)
        subprocess.run(["git", "-C", str(self.root), "config", "user.email", "package@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.root), "commit", "-q", "-m", "fixture"], check=True)
        self.commit, self.tree = packager._checkout_state(self.root)
        self.build_info_path = self.base / "build-provenance.json"
        self.build_info = {
            "schema": 1,
            "source_commit": self.commit,
            "source_tree": self.tree,
            "source_clean": True,
            "compiler": {"id": "AppleClang", "version": "21.0"},
            "sdk_version": "26.2",
            "cmake_version": "cmake version 3.31.1",
            "cmake_flags": {
                "build_type": "Release", "architectures": "arm64", "deployment_target": "26.0",
                "sdk": "macosx26.2", "qt_prefix": "$QT_DIR",
            },
            "qt_version": "6.12.0",
            "executable_path": "Contents/MacOS/Before Effects",
            "executable_sha256": sha(self.exe.read_bytes()),
        }
        self.write_build()

    def _write(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def write_build(self):
        self.build_info_path.write_text(json.dumps(self.build_info, indent=2), encoding="utf-8")

    def args(self, output):
        from argparse import Namespace
        return Namespace(
            app=str(self.app), output=str(output), source_commit=self.commit, release_tag="v0.7.0-alpha.1",
            build_provenance=str(self.build_info_path), runtime_manifest=str(self.root / "docs/third-party/public-runtime.json"),
            notices_dir=str(self.notices), project_license=str(self.license), examples_dir=str(self.examples),
        )


def mock_audit(_bundle, _script):
    return {"errors": [], "files": [
        {"file": "Contents/MacOS/Before Effects"},
        {"file": "Contents/Frameworks/QtCore.framework/Versions/A/QtCore"},
        {"file": "Contents/PlugIns/platforms/libqcocoa.dylib"},
    ]}


def tool_output(args, **kwargs):
    if args[0] == "otool":
        return "@rpath/QtCore.framework/Versions/A/QtCore (compatibility version 6.0.0, current version 6.12.0)\n"
    return original_check_output(args, **kwargs)


original_check_output = subprocess.check_output
original_run = subprocess.run


def mock_run(args, *positional, **kwargs):
    if args[0] == "codesign":
        return subprocess.CompletedProcess(args, 0)
    return original_run(args, *positional, **kwargs)


class PackageMacOSChecks(unittest.TestCase):
    def _package(self, fixture, output):
        with patch.object(packager, "_audit_bundle", side_effect=mock_audit), \
             patch.object(packager.subprocess, "run", side_effect=mock_run), \
             patch.object(packager.subprocess, "check_output", side_effect=tool_output):
            packager.package(fixture.args(output), fixture.root)

    def test_clean_tracked_package_is_bound_and_copied(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = PackageFixture(tmp)
            output = Path(tmp) / "package"
            self._package(fixture, output)
            manifest = json.loads((output / "PACKAGE-MANIFEST.json").read_text())
            self.assertEqual(manifest["source_commit"], fixture.commit)
            self.assertEqual(manifest["source_tree"], fixture.tree)
            self.assertEqual(manifest["final_executable_sha256"], fixture.build_info["executable_sha256"])
            self.assertTrue((output / "Third-party/Notices/notice.txt").is_file())
            self.assertEqual((output / "Third-party/THIRD-PARTY-NOTICES.md").read_bytes(), fixture.summary)

    def test_runtime_inventory_failures_leave_no_package(self):
        changes = (
            lambda runtime: runtime.update(schema=2),
            lambda runtime: runtime.update(components=[]),
            lambda runtime: runtime["components"].pop(),
            lambda runtime: runtime["components"][0].update(version="6.11.0"),
            lambda runtime: runtime["components"][0].update(bundle_path="Contents/Frameworks/Missing.framework/Missing"),
            lambda runtime: runtime.update(qt_version="6.11.0"),
            lambda runtime: runtime["notice_files"].update({"docs/third-party/notices/notice.txt": "0" * 64}),
            lambda runtime: runtime["notice_files"].update({"docs/third-party/notices/missing.txt": "0" * 64}),
        )
        for index, change in enumerate(changes):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as tmp:
                fixture = PackageFixture(tmp, change)
                output = Path(tmp) / "package"
                with self.assertRaises(ValueError):
                    self._package(fixture, output)
                self.assertFalse(output.exists())

    def test_build_provenance_mismatch_missing_fields_and_stale_executable_fail_before_output(self):
        changes = (
            lambda info: info.update(source_commit="0" * 40),
            lambda info: info.update(source_tree="0" * 40),
            lambda info: info.update(source_clean=False),
            lambda info: info.pop("sdk_version"),
            lambda info: info.update(executable_sha256="0" * 64),
        )
        for index, change in enumerate(changes):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as tmp:
                fixture = PackageFixture(tmp)
                change(fixture.build_info)
                fixture.write_build()
                output = Path(tmp) / "package"
                with self.assertRaises(ValueError):
                    self._package(fixture, output)
                self.assertFalse(output.exists())

    def test_dirty_or_untracked_source_and_untracked_examples_are_rejected(self):
        for mode in ("tracked", "example-untracked", "untracked"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                fixture = PackageFixture(tmp)
                if mode == "tracked":
                    (fixture.root / "fixtures/example.json").write_bytes(b"changed\n")
                elif mode == "example-untracked":
                    (fixture.examples / "private.json").write_text("{}\n", encoding="utf-8")
                else:
                    (fixture.root / "extra.txt").write_text("untracked\n", encoding="utf-8")
                output = Path(tmp) / "package"
                with self.assertRaises(ValueError):
                    self._package(fixture, output)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
