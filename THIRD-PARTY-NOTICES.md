# Third-party software

B4's original code is licensed under MPL-2.0. This does not change the licenses of its dependencies or the Noto Sans font.

The macOS alpha uses Qt 6.12.0 Core, Gui, Widgets, DBus and the Cocoa platform plugin under their **LGPL-3.0-only** option, with the additional upstream notices for code incorporated in those modules. The application uses dynamically linked Qt frameworks. Apple system frameworks are supplied by macOS and are not redistributed in this package.

Exact component, version, license and source metadata is recorded in [public-runtime.json](docs/third-party/public-runtime.json). [The notice collection](docs/third-party/notices/) retains upstream license, copyright and attribution files. Its Qt SDK inventory is a superset of the shipped module set; the actual application bundle inventory is recorded separately in release evidence.

The matching, unmodified QtBase 6.12.0 source archive is supplied alongside the alpha release:

- [QtBase corresponding source](https://github.com/edwin0912-00/b4/releases/download/v0.7.0-alpha.1/qtbase-everywhere-src-6.12.0.tar.xz)
- SHA-256: `a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307`
- [Original Qt download and publisher checksum](https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/qtbase-everywhere-src-6.12.0.tar.xz.mirrorlist)

The release also supplies B4's source and build instructions. Qt was obtained from the official SDK through a pinned installer; application deployment relocates library paths, strips deployment debug data, limits plugins to Cocoa and signs the result ad hoc. These deployment operations do not modify Qt source code.

Noto Sans Regular retains [SIL OFL 1.1](fixtures/font/OFL.txt) and its [source provenance](fixtures/font/PROVENANCE.txt). Original project examples have their own [provenance record](fixtures/PROVENANCE.md).

## Building and using modified Qt libraries

You may build an ABI-compatible Qt 6 installation from the provided sources, following its included build documentation and license conditions. To build B4 against it, set `CMAKE_PREFIX_PATH` to that installation when running CMake. Deploy with that Qt installation's `macdeployqt`, retain the Cocoa platform plugin, and run the bundle audit described in [BUILD.md](docs/public/BUILD.md).

The app is not locked to a vendor signing key. After replacing/redeploying frameworks in your own copied app bundle, an ad-hoc signature can be recreated with:

```sh
codesign --force --deep --sign - --timestamp=none "Before Effects.app"
codesign --verify --deep --strict "Before Effects.app"
```

Rebuild and run the tests after changing libraries. This alpha is not Developer ID signed or notarized; the license files and source are supplied to support users' modification and redistribution rights, subject to their respective terms.

## Scoped dependency-security review

The Qt SDK records PCRE2 10.48 and libpng 1.6.58. Their upstream advisories must not be confused with a blanket statement that every caller is exploitable. In the reviewed application:

- [CVE-2026-103111](https://github.com/PCRE2Project/pcre2/security/advisories/GHSA-r9hj-j2rw-4q3m) requires attacker-controlled regex patterns used with a growable JIT stack. B4's current five regex call sites use fixed literal patterns; project and UI strings are subjects, not patterns. This conclusion must be revisited before adding user-supplied regex or direct PCRE2 calls.
- [CVE-2026-46675](https://github.com/pnggroup/libpng/security/advisories/GHSA-qvg3-h654-xq3j) requires `png_read_end` before image-row reading is started. At the [pinned QtBase source](https://github.com/qt/qtbase/blob/025bdad181de241e81bf853c8a2d7bf3d19261a9/src/gui/image/qpnghandler.cpp), B4's successful PNG path updates image information, reads the image rows, then reads the end; error paths destroy the reader without that call. The affected ordering is not used in this path.

This records the assessed call paths at the alpha release. It is not a claim that all Qt code or all future B4 changes are free of vulnerabilities. Upstream updates remain part of maintenance.
