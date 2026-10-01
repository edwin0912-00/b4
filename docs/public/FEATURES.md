# Features in the 0.7.0 alpha

## Available

The current alpha supports editable 2D compositions, text and solid layers, PNG images, native video/audio layers, transform and opacity animation, eased keyframes, scalar graphs, parenting, nested compositions, rectangular and elliptical masks, and Undo/Redo.

It includes four native effects in an ordered, animatable stack: Linear Color, RGB Split, Exposure, and Gaussian Blur. The viewer can inspect RGB, individual channels, alpha, and luminance with independent viewing exposure. Transform-only motion blur can be enabled per layer and composition and configured in Composition Settings. Work Area boundaries persist in native schema 5 and can independently constrain preview and queued video/audio export; still output remains at the current frame.

PNG image sequences and H.264/AAC MP4 are available on the supported macOS target. The included projects are `fixtures/original-scene.json`, `fixtures/effects-motion-demo.json`, and `fixtures/work-area-demo.json`.

## Not implemented or supported

- General After Effects `.aep` import/export. A small parser test target is not a project importer.
- Loading or executing third-party native After Effects plug-ins.
- The complete After Effects feature and parameter set or confirmed pixel/interaction parity.
- `.b4p` code-to-Timeline interchange or Remotion/Motion Canvas/Tesseract source-engine adapters.
- Windows, Intel Mac, or Linux builds.
- HDR/OCIO color management, full 3D, tracking, rotoscoping, expressions, adaptive pixel motion blur, or frame blending.

The full AE-like product target remains future work. These items are explicit gaps, not implied alpha support.
