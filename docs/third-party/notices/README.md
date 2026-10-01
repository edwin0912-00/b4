# Third-party notices: Qt 6.12.0

This directory contains source-derived QtBase notice material copied verbatim from the official QtBase 6.12.0 archive, plus the exact Qt SDK `qtbase-6.12.0.spdx.json` file. See [`qtbase-source-notice-manifest.json`](qtbase-source-notice-manifest.json) for the archive receipt, selection rule, file paths, sizes, and SHA-256 values.

The source archive's package-root prefix `qtbase-everywhere-src-6.12.0/` was removed; all remaining archive-relative paths are preserved under [`qtbase/`](qtbase/). The collection contains 160 files (38 under `LICENSES/`, 78 `qt_attribution.json` files, and 44 other named license/copyright files), totaling 533,697 bytes. No source, SDK, or application binaries are included here.

## What the current app bundle uses

The curated app bundle contains five Qt binary components: **Qt Core, Qt Gui, Qt Widgets, Qt D-Bus, and the macOS Cocoa platform plugin**. The selected Qt open-source option for those Qt components is **LGPL-3.0-only**. The Cocoa platform plugin also carries a separate BSD-3-Clause attribution for Apple-authored Cocoa integration code.

For example, the matching QtBase SDK SBOM records Qt Core's bundled PCRE2 10.48 plus SLJIT (BSD-3-Clause with PCRE2 binary-package exception; SLJIT BSD-2-Clause), Qt Gui's bundled libpng 1.6.58 (Libpng License and PNG Reference Library License v2), and Qt D-Bus's libdbus-1 headers 1.13.12 (AFL-2.1 or GPL-2.0-or-later). The full component list comes from the app-specific SBOM, not this abbreviated index. Source attributions and license copies are included under `qtbase/src/3rdparty/` and `qtbase/src/dbus/`; see the paths in the machine-readable manifest. Keep these component terms separate from Qt's LGPL-3.0-only module selection.

## Scope of this collection

The extracted `LICENSES/` and all QtBase `qt_attribution.json` files form a source-tree attribution superset. They cover optional QtBase platform code, tools, and components that are not necessarily in the curated app bundle. The copied `qtbase-sdk.spdx.json` is from the universal `x86_64;arm64` macOS SDK and is also broader than the app's five Qt binary components. It is evidence for source and build metadata, not a claim that every listed component is shipped. Use the app-specific runtime inventory/SBOM to select the attributions applicable to its exact binaries.

The verbatim SDK inventory includes five `/Users/qt/...` build paths in upstream comments. These describe Qt's published build environment, not a B4 developer's machine or a user's project.

The release supplies matching QtBase source, license copies and a Qt-use notice. [The main notice](../../../THIRD-PARTY-NOTICES.md) links the source archive and explains how to rebuild or replace the dynamically linked Qt libraries. Verification exercised replacement with the same-version SDK, redeployment, ad-hoc signing and a native launch; it does not establish that every modified Qt build will work.
