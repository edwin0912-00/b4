# Build Before Effects on macOS

This guide covers the native 0.7.0 alpha source build. The current supported target is Apple Silicon on macOS 26.0 or newer. Intel Macs and other operating systems are not supported. The existing app binary has a macOS 26.0 deployment target; support for an older macOS release has not been verified.

## Requirements

- Xcode 26.2 and its macOS SDK
- AppleClang with C++20 support
- CMake 3.24 or newer
- Python 3.12 or newer
- Qt 6 for macOS, modules Core, Gui, and Widgets

The release uses Qt 6.12.0 from Qt's official macOS SDK repository, installed with `aqtinstall` 3.3.0. The workflow's `QT_VERSION` value is the single version setting used for its install, build, and provenance record. The release evidence records the required test, render and packaged-app checks.

## Install the CI Qt SDK

From the repository root, create a private Python environment and install the same pinned Qt installer used by CI:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install aqtinstall==3.3.0
export B4_QT_VERSION=6.12.0
.venv/bin/aqt install-qt mac desktop "$B4_QT_VERSION" clang_64 --outputdir "$PWD/.qt"
export B4_QT_DIR="$PWD/.qt/$B4_QT_VERSION/macos"
```

Qt is placed in the local `.qt` directory. `clang_64` is Qt's universal macOS kit; the application build explicitly targets Apple Silicon. Qt and its embedded components retain their licenses in the [third-party notices](../../THIRD-PARTY-NOTICES.md).

## Configure, build, and test

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$B4_QT_DIR" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

CTest runs all 14 registered checks. Do not remove, weaken, or silently skip a test to accommodate a runner. Most UI checks use Qt's offscreen plugin. The packaged application smoke test must use the Cocoa platform plugin; the standalone bundle does not include the offscreen plugin.

## Deploy and inspect a local app bundle

After a successful build, deploy Qt into the new app bundle and check its dependency closure:

```sh
"$B4_QT_DIR/bin/macdeployqt" "build/Before Effects.app" -always-overwrite
python3 - <<'PY'
from pathlib import Path
import shutil
app = Path("build/Before Effects.app/Contents")
# The alpha uses Fusion widgets and PNG; only the Cocoa plugin is required.
for relative in ("PlugIns/styles", "PlugIns/iconengines", "PlugIns/imageformats", "Frameworks/QtSvg.framework"):
    path = app / relative
    if path.exists():
        shutil.rmtree(path)
(app / "Resources/qt.conf").write_text("[Paths]\nPlugins=PlugIns\n")
PY
python3 -B tests/bundle_audit_checks.py
python3 scripts/audit_bundle.py "build/Before Effects.app"
mkdir -p "build/Before Effects.app/Contents/Resources/Third-party/Notices"
cp LICENSE "build/Before Effects.app/Contents/Resources/LICENSE"
cp THIRD-PARTY-NOTICES.md "build/Before Effects.app/Contents/Resources/Third-party/"
cp -R docs/third-party/notices/. "build/Before Effects.app/Contents/Resources/Third-party/Notices/"
codesign --force --deep --sign - --timestamp=none "build/Before Effects.app"
codesign --verify --deep --strict "build/Before Effects.app"
```

Launch the application with Cocoa, for example:

```sh
open "build/Before Effects.app"
```

Release CI also launches the bundle with Cocoa, authors a synthetic scene, and captures a real window screenshot. After signing, record final build provenance and package only from the same clean, tracked source checkout:

```sh
stage="$(mktemp -d "${TMPDIR:-/tmp}/b4-release.XXXXXX")"
python3 scripts/package_macos.py --record-build \
  --app "build/Before Effects.app" \
  --build-dir build \
  --qt-root "$B4_QT_DIR" \
  --qt-version "$B4_QT_VERSION" \
  --output "$stage/build-provenance.json"
python3 scripts/package_macos.py \
  --app "build/Before Effects.app" \
  --output "$stage/package" \
  --source-commit "$(git rev-parse HEAD)" \
  --release-tag v0.7.0-alpha.1 \
  --build-provenance "$stage/build-provenance.json" \
  --runtime-manifest docs/third-party/public-runtime.json \
  --notices-dir docs/third-party/notices \
  --project-license LICENSE \
  --examples-dir fixtures
```

The provenance record binds the current Git commit and tree, clean tracked state, actual CMake/compiler/SDK/Qt inputs, and final signed executable hash. The packager verifies its checkout, tracked package inputs, runtime manifest coverage and notice hashes; it refuses an existing output directory and never changes the supplied app. It does not install, download, build, or publish anything.

The release app is ad-hoc signed and is not notarized. Its packaging and runtime notices are included with each candidate. Read the [release notes](RELEASE.md) for the supported scope and remaining product limits.
