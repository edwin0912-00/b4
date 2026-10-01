# Before Effects 0.7.0 Alpha 1

Release label: `v0.7.0-alpha.1`

Application version: `0.7.0`

Before Effects is an independent motion-design and compositing editor. This alpha focuses on a native 2D timeline, editable compositions, keyframed transforms, basic effects, media layers, and project export. It keeps its own implementation and visual identity.

## Platform and package

The initial package targets Apple Silicon Macs running macOS 26.0 or newer. Windows, Intel Mac, and Linux builds are not included. The app is ad-hoc signed and not notarized; macOS may show its normal first-open approval prompt.

The package contains `Before Effects.app`, editable example projects and their fixture assets, project build instructions, the selected project license, runtime provenance and notices, and an integrity manifest. `Build-Provenance.json` records the source commit and toolchain/runtime inputs. `PACKAGE-MANIFEST.json` records the package files and their hashes. Review the included notices before redistributing the application.

The release gate is complete only after a clean source commit has passed all 14 registered tests, the original 360-frame render regression, the dependency-closure audit, code-signature verification, and a Cocoa launch/screenshot smoke test. A tag or candidate artifact alone does not prove those checks passed.

## Included behavior

- Editable 2D compositions with text, solid, PNG image, video, and audio layers.
- Transform and opacity animation, eased keyframes, scalar graphs, parenting, nested compositions, and rectangular/elliptical masks.
- Linear Color, RGB Split, Exposure, and Gaussian Blur effects in an ordered, animatable stack.
- Viewer channel inspection, independent viewer exposure, transform-only motion blur, and saved Work Area ranges for preview and queued export.
- PNG sequence and H.264/AAC MP4 output on the supported macOS target.

The examples are native editable JSON projects. General After Effects `.aep` import/export, third-party After Effects plug-ins, the complete After Effects feature set, and confirmed pixel or interaction parity are not included. `.b4p`, HDR/color management, full 3D, tracking, rotoscoping, Windows, Intel Mac, and Linux remain unsupported or future work. See [Features](FEATURES.md) for the full list.
