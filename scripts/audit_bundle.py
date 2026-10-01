#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Reject app bundles that still need developer libraries or missing Mach-O dependencies."""
import json
import configparser
from pathlib import Path
import subprocess
import sys


def audit(bundle):
    bundle = Path(bundle).resolve()
    frameworks = bundle / "Contents/Frameworks"
    executable_directory = bundle / "Contents/MacOS"
    records, errors = [], []
    if (frameworks / "QtCore.framework").exists():
        config = configparser.ConfigParser()
        config.read(bundle / "Contents/Resources/qt.conf")
        if config.get("Paths", "Plugins", fallback="") != "PlugIns" or config.has_option("Paths", "Prefix"):
            errors.append("Qt plugin lookup must be restricted by Resources/qt.conf to bundled PlugIns")
    for path in bundle.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            magic = stream.read(4)
        if magic not in [bytes.fromhex(value) for value in ("cffaedfe", "feedfacf", "cafebabe", "bebafeca")]:
            continue
        dependencies = subprocess.check_output(["otool", "-L", str(path)], text=True).splitlines()[1:]
        # Universal Mach-O output repeats an unindented filename header for each slice.
        dependencies = [line.strip().split(" (")[0] for line in dependencies if line[:1].isspace()]
        commands = subprocess.check_output(["otool", "-l", str(path)], text=True).splitlines()
        rpaths = [commands[i + 2].strip().split("path ", 1)[1].split(" (offset")[0]
                  for i, line in enumerate(commands) if "cmd LC_RPATH" in line]

        def expand(value):
            value = value.replace("@loader_path", str(path.parent))
            return Path(value.replace("@executable_path", str(executable_directory))).resolve()

        for value in rpaths:
            target = expand(value)
            if not any(target.is_relative_to(root) for root in (bundle, Path("/System"), Path("/usr/lib"))):
                errors.append(f"{path.relative_to(bundle)}: external search path {value}")
        for value in dependencies:
            if value.startswith("/") and any(Path(value).resolve().is_relative_to(root)
                                              for root in (Path("/System"), Path("/usr/lib"))):
                continue
            candidates = ([frameworks / value.removeprefix("@rpath/")] +
                          [expand(root) / value.removeprefix("@rpath/") for root in rpaths]
                          if value.startswith("@rpath/") else [expand(value)])
            if not any(candidate.exists() and candidate.resolve().is_relative_to(bundle) for candidate in candidates):
                errors.append(f"{path.relative_to(bundle)}: unresolved or external dependency {value}")
        records.append({"file": str(path.relative_to(bundle)), "dependencies": dependencies, "rpaths": rpaths})
    if not records:
        errors.append("No Mach-O files found; an empty bundle cannot pass")
    return {"bundle": str(bundle), "mach_o_count": len(records), "errors": errors, "files": records}


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: audit_bundle.py 'Before Effects.app' [report.json]")
    report = audit(sys.argv[1])
    if len(sys.argv) == 3:
        Path(sys.argv[2]).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("mach_o_count", "errors")}, indent=2))
    raise SystemExit(bool(report["errors"]))
