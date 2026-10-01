#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Copy a verified macOS app and reviewed release inputs into a fresh directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile


MACHO_MAGICS = {bytes.fromhex(value) for value in ("cffaedfe", "feedfacf", "cafebabe", "bebafeca")}
NOTICE_PREFIX = PurePosixPath("docs/third-party/notices")
PROVENANCE_FIELDS = (
    "source_commit", "source_tree", "source_clean", "compiler", "sdk_version",
    "cmake_version", "cmake_flags", "qt_version", "executable_path", "executable_sha256",
)


def _required_file(value, label):
    path = Path(value).expanduser().resolve(strict=True)
    if not path.is_file():
        raise ValueError(f"{label} must be a regular file: {path}")
    return path


def _required_tree(value, label):
    path = Path(value).expanduser().resolve(strict=True)
    if not path.is_dir():
        raise ValueError(f"{label} must be a directory: {path}")
    return path


def _validate_tree(root, label):
    entries = list(root.rglob("*"))
    if not entries:
        raise ValueError(f"{label} must not be empty: {root}")
    for path in entries:
        if path.is_symlink():
            link = os.readlink(path)
            if Path(link).is_absolute():
                raise ValueError(f"{label} contains an absolute symlink: {path.relative_to(root)}")
            try:
                target = path.resolve(strict=True)
            except (FileNotFoundError, RuntimeError) as exc:
                raise ValueError(f"{label} contains a broken or cyclic symlink: {path.relative_to(root)}") from exc
            if not target.is_relative_to(root):
                raise ValueError(f"{label} symlink escapes its input tree: {path.relative_to(root)}")
        elif not path.is_dir() and not path.is_file():
            raise ValueError(f"{label} contains a special file: {path.relative_to(root)}")


def _json_object(path, label):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} must contain valid UTF-8 JSON: {path}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object: {path}")
    return value


def _hash_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def _checkout_state(root):
    commit = _git(root, "rev-parse", "HEAD")
    tree = _git(root, "rev-parse", "HEAD^{tree}")
    status = _git(root, "status", "--porcelain", "--untracked-files=all")
    if status:
        raise ValueError("source checkout must have a clean tracked tree and no untracked public files")
    return commit, tree


def _tracked_inputs(root, paths):
    root = root.resolve(strict=True)
    for source in paths:
        source = Path(source).resolve(strict=True)
        try:
            relative = source.relative_to(root).as_posix()
        except ValueError as exc:
            raise ValueError(f"package input is outside the public source checkout: {source}") from exc
        tracked = set(filter(None, _git(root, "ls-tree", "-r", "-z", "--name-only", "HEAD", "--", relative).split("\0")))
        actual = ({relative} if source.is_file() else {
            child.relative_to(root).as_posix()
            for child in source.rglob("*")
            if child.is_file() or child.is_symlink()
        })
        if actual != tracked:
            raise ValueError(f"package input does not exactly match tracked HEAD content: {relative}")


def _macho(path):
    try:
        with path.open("rb") as stream:
            return stream.read(4) in MACHO_MAGICS
    except OSError:
        return False


def _audit_bundle(bundle, script):
    with tempfile.TemporaryDirectory(prefix="b4-bundle-audit-") as temp:
        report_path = Path(temp) / "audit.json"
        subprocess.run([sys.executable, str(script), str(bundle), str(report_path)], check=True)
        report = _json_object(report_path, "bundle audit report")
    if report.get("errors"):
        raise ValueError("bundle dependency-closure audit failed")
    files = report.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("bundle audit report contains no Mach-O files")
    return report


def _safe_relative(value, label):
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        raise ValueError(f"{label} must be a nonempty POSIX relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError(f"{label} is not a safe relative path: {value!r}")
    return path


def _notice_map(runtime, notices):
    declared = runtime.get("notice_files")
    if not isinstance(declared, dict) or not declared:
        raise ValueError("runtime manifest has no reviewed notice_files mapping")
    expected = {}
    for name, digest in declared.items():
        path = _safe_relative(name, "notice path")
        try:
            relative = path.relative_to(NOTICE_PREFIX)
        except ValueError as exc:
            raise ValueError(f"notice path is outside {NOTICE_PREFIX}: {name}") from exc
        if not relative.parts or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"notice path or SHA-256 is invalid: {name}")
        expected[relative.as_posix()] = digest
    actual = {
        path.relative_to(notices).as_posix()
        for path in notices.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    if actual != set(expected):
        raise ValueError("runtime notice files do not exactly match the manifest path list")
    for relative, digest in expected.items():
        path = notices / relative
        if path.is_symlink() or not path.is_file() or _hash_file(path) != digest:
            raise ValueError(f"runtime notice SHA-256 mismatch: {relative}")
    return expected


def _framework_version(app, relative):
    parts = PurePosixPath(relative).parts
    index = next((i for i, part in enumerate(parts) if part.endswith(".framework")), None)
    if index is None:
        return None
    framework = app.joinpath(*parts[:index + 1])
    for info_path in (framework / "Resources/Info.plist", framework / "Versions/A/Resources/Info.plist"):
        if info_path.is_file():
            info = plistlib.loads(info_path.read_bytes())
            return str(info.get("CFBundleVersion", info.get("CFBundleShortVersionString", "")))
    raise ValueError(f"framework has no versioned Info.plist: {relative}")


def _component_version(app, path, relative):
    version = _framework_version(app, relative)
    if version is not None:
        return version
    listing = subprocess.check_output(["otool", "-L", str(path)], text=True)
    versions = set()
    for line in listing.splitlines():
        if ".framework/" in line and re.search(r"\bQt[^/\s]*\.framework/", line):
            match = re.search(r"current version ([0-9]+(?:\.[0-9]+){1,2})", line)
            if match:
                versions.add(match.group(1))
    if not versions:
        raise ValueError(f"runtime plug-in has no versioned Qt framework dependency: {relative}")
    if len(versions) != 1:
        raise ValueError(f"runtime plug-in has conflicting Qt framework versions: {relative}")
    return versions.pop()


def _validate_runtime(runtime, app, executable_relative, audit_report, build_info, notice_files, notices):
    app = Path(app).resolve(strict=True)
    if runtime.get("schema") != 1:
        raise ValueError("unsupported runtime manifest schema")
    qt_version = runtime.get("qt_version")
    if not isinstance(qt_version, str) or not qt_version or qt_version != build_info.get("qt_version"):
        raise ValueError("runtime manifest Qt version does not match build provenance")
    components = runtime.get("components")
    if not isinstance(components, list) or not components:
        raise ValueError("runtime manifest has no component inventory")

    audited = {record.get("file") for record in audit_report["files"] if isinstance(record, dict)}
    expected_components = audited - {executable_relative}
    declared_components = set()
    for component in components:
        if not isinstance(component, dict):
            raise ValueError("runtime component record must be an object")
        relative_path = _safe_relative(component.get("bundle_path"), "component bundle_path").as_posix()
        if relative_path in declared_components:
            raise ValueError(f"duplicate runtime component path: {relative_path}")
        declared_components.add(relative_path)
        component_path = app / relative_path
        if (not component_path.is_file() or component_path.is_symlink() or
                not component_path.resolve().is_relative_to(app.resolve()) or not _macho(component_path)):
            raise ValueError(f"runtime component is not an in-bundle regular Mach-O: {relative_path}")
        if component.get("version") != qt_version or _component_version(app, component_path, relative_path) != component["version"]:
            raise ValueError(f"runtime component version mismatch: {relative_path}")
    if declared_components != expected_components:
        missing = sorted(expected_components - declared_components)
        extra = sorted(declared_components - expected_components)
        raise ValueError(f"runtime component coverage mismatch; missing={missing}, extra={extra}")
    copied_notices = _notice_map(runtime, notices)
    if copied_notices != notice_files:
        raise ValueError("runtime notice inventory changed while preparing the package")


def _validate_build_provenance(info, commit, tree, qt_version, executable_relative, executable):
    if info.get("schema") != 1:
        raise ValueError("unsupported build provenance schema")
    missing = [field for field in PROVENANCE_FIELDS if field not in info]
    if missing:
        raise ValueError(f"build provenance is missing fields: {', '.join(missing)}")
    if info["source_commit"] != commit or info["source_tree"] != tree:
        raise ValueError("build provenance does not match the checked-out Git HEAD and tree")
    if info["source_clean"] is not True:
        raise ValueError("build provenance was not recorded from a clean source checkout")
    compiler = info["compiler"]
    if not isinstance(compiler, dict) or not all(
            isinstance(compiler.get(key), str) and compiler[key] for key in ("id", "version")):
        raise ValueError("build provenance compiler id/version are missing")
    for field in ("sdk_version", "cmake_version"):
        if not isinstance(info[field], str) or not info[field]:
            raise ValueError(f"build provenance {field} is missing")
    flags = info["cmake_flags"]
    required_flags = ("build_type", "architectures", "deployment_target", "sdk", "qt_prefix")
    if not isinstance(flags, dict) or not all(
            isinstance(flags.get(key), str) and flags[key] for key in required_flags):
        raise ValueError("build provenance is missing actual CMake build flags")
    if (flags["build_type"], flags["architectures"], flags["deployment_target"]) != ("Release", "arm64", "26.0"):
        raise ValueError("build provenance flags do not match the supported Release/arm64/macOS 26 target")
    if info["qt_version"] != qt_version:
        raise ValueError("build provenance Qt version does not match runtime manifest")
    if info["executable_path"] != executable_relative or not re.fullmatch(
            r"[0-9a-f]{64}", str(info["executable_sha256"])):
        raise ValueError("build provenance final executable path/hash is invalid")
    if _hash_file(executable) != info["executable_sha256"]:
        raise ValueError("build provenance final executable hash does not match supplied app")


def _cmake_cache(build_dir):
    cache = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line or ":" not in line:
            continue
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
    return cache


def _cmake_compiler(build_dir):
    files = list((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(files) != 1:
        raise ValueError("expected one configured CMake C++ compiler record")
    source = files[0].read_text(encoding="utf-8")
    values = {}
    for key in ("ID", "VERSION"):
        match = re.search(rf'set\(CMAKE_CXX_COMPILER_{key}\s+"([^"]+)"\)', source)
        if match:
            values[key.lower()] = match.group(1)
    if set(values) != {"id", "version"}:
        raise ValueError("configured CMake compiler record is missing id/version")
    return values


def record_build(args, project_root=None):
    root = Path(project_root or Path(__file__).resolve().parents[1]).resolve(strict=True)
    app = _required_tree(args.app, "app bundle")
    build_dir = _required_tree(args.build_dir, "CMake build directory")
    qt_root = _required_tree(args.qt_root, "Qt SDK root")
    commit, tree = _checkout_state(root)
    if args.source_commit and args.source_commit.lower() != commit:
        raise ValueError("--source-commit does not match checkout HEAD")

    cache = _cmake_cache(build_dir)
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve(strict=True) != root:
        raise ValueError("CMake build directory was configured for a different source checkout")
    compiler = _cmake_compiler(build_dir)
    flags = {
        "build_type": cache.get("CMAKE_BUILD_TYPE", ""),
        "architectures": cache.get("CMAKE_OSX_ARCHITECTURES", ""),
        "deployment_target": cache.get("CMAKE_OSX_DEPLOYMENT_TARGET", ""),
        "sdk": cache.get("CMAKE_OSX_SYSROOT", ""),
        "qt_prefix": "$QT_DIR",
    }
    if not all(flags[key] for key in ("build_type", "architectures", "deployment_target", "sdk")):
        raise ValueError("CMakeCache.txt is missing release build flags")
    if flags["build_type"] != "Release" or flags["architectures"] != "arm64" or flags["deployment_target"] != "26.0":
        raise ValueError("CMake build flags do not match the supported Release/arm64/macOS 26 target")
    qt_core_dir = Path(cache.get("Qt6Core_DIR", "")).resolve(strict=True)
    if not qt_core_dir.is_relative_to(qt_root.resolve()):
        raise ValueError("CMake did not use the supplied Qt SDK")
    qt_info = plistlib.loads((qt_root / "lib/QtCore.framework/Resources/Info.plist").read_bytes())
    qt_version = str(qt_info.get("CFBundleVersion", ""))
    if not qt_version or (args.qt_version and args.qt_version != qt_version):
        raise ValueError("supplied Qt version does not match the installed SDK")

    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    executable_name = info.get("CFBundleExecutable")
    if not isinstance(executable_name, str) or Path(executable_name).name != executable_name:
        raise ValueError("app bundle Info.plist has no safe CFBundleExecutable")
    executable = app / "Contents/MacOS" / executable_name
    if not executable.is_file() or executable.is_symlink():
        raise ValueError("app bundle executable is missing")
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)

    output = Path(args.output).expanduser().absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"build provenance output already exists: {output}")
    if output.resolve(strict=False).is_relative_to(root):
        raise ValueError("build provenance output must be outside the source checkout")
    record = {
        "schema": 1,
        "source_commit": commit,
        "source_tree": tree,
        "source_clean": True,
        "compiler": compiler,
        "sdk_version": subprocess.check_output(
            ["xcrun", "--sdk", flags["sdk"], "--show-sdk-version"], text=True
        ).strip(),
        "cmake_version": subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0],
        "cmake_flags": flags,
        "qt_version": qt_version,
        "executable_path": f"Contents/MacOS/{executable_name}",
        "executable_sha256": _hash_file(executable),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(f"Recorded signed build provenance: {output}")


def _payload_records(root):
    records = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            records.append({"path": relative, "type": "symlink", "target": os.readlink(path)})
        elif path.is_file():
            records.append({
                "path": relative,
                "type": "file",
                "bytes": path.stat().st_size,
                "sha256": _hash_file(path),
            })
    return records


def package(args, project_root=None):
    project_root = Path(project_root or Path(__file__).resolve().parents[1]).resolve(strict=True)
    app = _required_tree(args.app, "app bundle")
    if app.suffix != ".app":
        raise ValueError(f"app input must be a .app bundle: {app}")
    build_manifest_path = _required_file(args.build_provenance, "build provenance")
    runtime_manifest_path = _required_file(args.runtime_manifest, "runtime manifest")
    project_license = _required_file(args.project_license, "project license")
    notices = _required_tree(args.notices_dir, "runtime notices")
    examples = _required_tree(args.examples_dir, "editable examples")
    for tree, label in ((app, "app bundle"), (notices, "runtime notices"), (examples, "editable examples")):
        _validate_tree(tree, label)

    build_info = _json_object(build_manifest_path, "build provenance")
    runtime = _json_object(runtime_manifest_path, "runtime manifest")
    commit = args.source_commit.lower()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("--source-commit must be a 40-character Git commit SHA")
    head, tree = _checkout_state(project_root)
    if commit != head:
        raise ValueError("--source-commit does not match the clean public checkout HEAD")

    build_doc = _required_file(project_root / "docs/public/BUILD.md", "public build guide")
    release_doc = _required_file(project_root / "docs/public/RELEASE.md", "public release notes")
    features_doc = _required_file(project_root / "docs/public/FEATURES.md", "public feature list")
    audit_script = _required_file(project_root / "scripts/audit_bundle.py", "bundle auditor")
    package_script = _required_file(project_root / "scripts/package_macos.py", "package script")
    notices_index = _required_file(project_root / _safe_relative(runtime.get("notices_index"), "notices_index"),
                                    "third-party notice summary")
    _tracked_inputs(project_root, (runtime_manifest_path, notices, project_license, examples, build_doc,
                                   release_doc, features_doc, audit_script, package_script, notices_index))

    notice_files = _notice_map(runtime, notices)

    raw_output = Path(args.output).expanduser()
    if raw_output.exists() or raw_output.is_symlink():
        raise FileExistsError(f"output already exists: {raw_output}")
    output = raw_output.absolute()
    resolved_output = output.resolve(strict=False)
    sources = (app, build_manifest_path, runtime_manifest_path, project_license, notices, examples,
               notices_index, build_doc, release_doc, features_doc)
    for source in sources:
        if resolved_output.is_relative_to(source) or source.is_relative_to(resolved_output):
            raise ValueError("output must be separate from every package input")

    app_info_path = app / "Contents/Info.plist"
    try:
        app_info = plistlib.loads(app_info_path.read_bytes())
    except (OSError, plistlib.InvalidFileException) as exc:
        raise ValueError(f"app bundle has no readable Contents/Info.plist: {app}") from exc
    executable_name = app_info.get("CFBundleExecutable")
    if (not isinstance(executable_name, str) or not executable_name or
            Path(executable_name).name != executable_name or executable_name in (".", "..")):
        raise ValueError("app bundle Info.plist has no CFBundleExecutable")
    executable = app / "Contents/MacOS" / executable_name
    if not executable.is_file() or executable.is_symlink():
        raise ValueError("app bundle executable is missing or is a symlink")
    executable_relative = f"Contents/MacOS/{executable_name}"
    _validate_build_provenance(build_info, commit, tree, runtime.get("qt_version"),
                               executable_relative, executable)
    audit_report = _audit_bundle(app, audit_script)
    _validate_runtime(runtime, app, executable_relative, audit_report, build_info, notice_files, notices)
    embedded = app / "Contents/Resources/Third-party"
    _notice_map(runtime, embedded / "Notices")
    embedded_license = app / "Contents/Resources/LICENSE"
    if not embedded_license.is_file() or _hash_file(embedded_license) != _hash_file(project_license):
        raise ValueError("embedded project license does not match tracked source")
    if _hash_file(embedded / notices_index.name) != _hash_file(notices_index):
        raise ValueError("embedded third-party notice summary does not match tracked source")
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)

    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir(exist_ok=False)
    try:
        app_copy = output / "Before Effects.app"
        shutil.copytree(app, app_copy, symlinks=True)
        documentation = output / "Documentation"
        documentation.mkdir()
        shutil.copy2(build_doc, documentation / "BUILD.md")
        shutil.copy2(release_doc, documentation / "RELEASE.md")
        shutil.copy2(features_doc, documentation / "FEATURES.md")
        shutil.copy2(project_license, documentation / "PROJECT-LICENSE.txt")
        shutil.copy2(build_manifest_path, documentation / "Build-Provenance.json")
        third_party = output / "Third-party"
        third_party.mkdir()
        shutil.copy2(runtime_manifest_path, third_party / "Runtime-Provenance.json")
        shutil.copy2(notices_index, third_party / notices_index.name)
        shutil.copytree(notices, third_party / "Notices", symlinks=True)
        shutil.copytree(examples, output / "Examples", symlinks=True)

        copied_audit = _audit_bundle(app_copy, audit_script)
        _validate_runtime(runtime, app_copy, executable_relative, copied_audit, build_info, notice_files,
                          third_party / "Notices")
        if _hash_file(third_party / notices_index.name) != _hash_file(notices_index):
            raise ValueError("packaged third-party notice summary does not match tracked source")
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app_copy)], check=True)

        records = _payload_records(output)
        manifest = {
            "format": "before-effects-macos-package-v1",
            "source_commit": commit,
            "source_tree": tree,
            "release_tag": args.release_tag,
            "application": {
                "bundle_name": app_copy.name,
                "bundle_identifier": app_info.get("CFBundleIdentifier"),
                "version": app_info.get("CFBundleShortVersionString"),
                "build": app_info.get("CFBundleVersion"),
                "executable": f"Before Effects.app/Contents/MacOS/{executable_name}",
                "executable_sha256": _hash_file(app_copy / "Contents/MacOS" / executable_name),
            },
            "final_executable_sha256": build_info["executable_sha256"],
            "qt_version": build_info.get("qt_version"),
            "runtime_manifest_sha256": _hash_file(runtime_manifest_path),
            "build_provenance_sha256": _hash_file(build_manifest_path),
            "files": records,
        }
        (output / "PACKAGE-MANIFEST.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except BaseException:
        shutil.rmtree(output)
        raise
    print(f"Packaged {len(records)} files and symlinks into {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record-build", action="store_true",
                        help="record clean checkout, toolchain, Qt SDK, and final signed executable inputs")
    parser.add_argument("--app", required=True, help="verified .app bundle from this source commit")
    parser.add_argument("--output", required=True, help="new package directory, or provenance JSON with --record-build")
    parser.add_argument("--source-commit", help="40-character source Git commit (derived in --record-build mode)")
    parser.add_argument("--release-tag", default="", help="release tag, when packaging a tagged build")
    parser.add_argument("--build-provenance", help="JSON build record for the same commit")
    parser.add_argument("--runtime-manifest", help="reviewed runtime provenance JSON")
    parser.add_argument("--notices-dir", help="reviewed third-party notice files")
    parser.add_argument("--project-license", help="selected project LICENSE file")
    parser.add_argument("--examples-dir", help="reviewed editable example projects and assets")
    parser.add_argument("--build-dir", help="CMake build directory for --record-build")
    parser.add_argument("--qt-root", help="Qt SDK root used by the build")
    parser.add_argument("--qt-version", help="expected Qt version for --record-build")
    args = parser.parse_args()
    try:
        if args.record_build:
            if not args.build_dir or not args.qt_root or not args.qt_version:
                parser.error("--record-build requires --build-dir, --qt-root, and --qt-version")
            record_build(args)
        else:
            required = ("source_commit", "build_provenance", "runtime_manifest", "notices_dir",
                        "project_license", "examples_dir")
            missing = [name.replace("_", "-") for name in required if not getattr(args, name)]
            if missing:
                parser.error(f"package mode requires: {', '.join('--' + name for name in missing)}")
            package(args)
    except (OSError, ValueError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"package_macos.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
