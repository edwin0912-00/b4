# Effects, viewing and motion in Before Effects 0.6

These controls operate in the native CPU renderer. Their parameter policies and sampling kernel are original implementations; they have not been measured for pixel equivalence with After Effects.

## Add and animate an effect

Select a visual layer, then double-click an effect in **Effects Presets**, or select its type in **Effect Controls** and press **+ Add effect**. Effects run in their displayed order. Each effect has its own enable switch; move and remove actions participate in Undo. Numeric controls use the same keyframe, easing, copy/paste and graph system as transform properties. Saving and reopening retains effect IDs, order, switches and animation.

| Effect | Parameters and defaults | Result |
|---|---|---|
| Linear Color | Existing Gain, Bias and Amount | Linear gain and offset on premultiplied RGB; alpha is retained. |
| RGB Split | Red/Green/Blue Offset: X/Y, default 0 px, range −4096…4096; Amount: 100%, range 0…100% | Bilinear channel offsets in layer coordinates. Output alpha is the largest of the three shifted alphas; Amount mixes the complete RGBA result with the original. |
| Exposure | Stops: 0, range −20…20; Amount: 100%, range 0…100% | Multiplies linear RGB by `2^Stops`; alpha is retained. |
| Gaussian Blur | Sigma: 0 px, range 0…128; Amount: 100%, range 0…100% | A normalized separable Gaussian with radius `ceil(3 × Sigma)`, applied to premultiplied RGBA. |

RGB Split and Gaussian Blur keep the layer canvas size. Sampling beyond its edges is transparent, so effects near an edge may clip. Neutral settings return the original pixels exactly. Large sigma values cost more CPU time; there is no GPU effect backend in this build.

## Inspect an image without changing it

The controls below the viewer select **RGB**, **Red**, **Green**, **Blue**, **Alpha**, or **Luminance**. **Gray** chooses grayscale or isolated-color presentation for individual RGB channels. Viewing exposure is expressed in stops and is independent of the Exposure effect.

RGB preserves transparency for the existing checkerboard control. Individual channels and luminance are opaque diagnostic views; RGB values are unpremultiplied with a zero-alpha guard. Luminance uses linear-sRGB coefficients 0.2126, 0.7152 and 0.0722 before display encoding. Alpha displays coverage directly and disables viewing exposure; changing back restores the retained exposure value.

Channel, viewing exposure, viewer zoom and preview resolution change the view only. They do not dirty the project, create Undo entries, change effect values or alter exported pixels. Audio waveform views disable image-only controls. Preview cache keys include channel, grayscale, exposure and render size; viewer zoom and checkerboard are applied when painting the image.

## Enable transform motion blur

1. Enable the three-ring switch in the layer's **Motion Blur** column.
2. Enable the three-ring composition switch above the Timeline.
3. Open Composition Settings to set the exposure.

| Composition setting | Default | Range |
|---|---:|---:|
| Motion Blur | Off | Off / On |
| Shutter Angle | 180° | 0…720° |
| Shutter Phase | −90° | −360…360° |
| Samples | 16 | 1…64 |

At 30 fps, 180° covers 1/60 second. A −90° phase centers that interval around the requested frame. Samples are equally weighted midpoints; one sample evaluates only the exposure midpoint. A zero angle or disabled gates uses the original direct renderer exactly. Increasing Samples improves temporal integration at a corresponding CPU cost.

The **Render Queue** has three independent override choices:

| Override | Export behavior |
|---|---|
| Current settings | Requires the layer switch and every composition master in its nesting path. |
| On checked layers | Ignores composition masters and honors each layer switch. |
| Off | Disables transform motion blur for every layer in the render. |

The render request captures its settings when submitted. Editing or switching view controls later cannot change an in-progress export. The Render Queue controls are disabled while the job runs; **Cancel export** remains available in the status bar.

## What is integrated across time

Each sample evaluates eligible layers' complete world transforms, including animated parent transforms. The renderer composites the whole scene at that sample, then averages premultiplied linear frames. This retains correct sample-by-sample occlusion between moving layers.

Layer visibility and cuts, source/video frame, opacity, masks, effect values, text and source colors stay at the requested nominal time. A layer visible at the nominal frame remains present throughout its exposure; transform keyframes hold their endpoints outside their keyed interval. This prevents static content fading at composition boundaries.

Nested compositions receive paired nominal and sample times through their layer time mapping. One root shutter interval drives the tree. A nested composition's own shutter angle, phase and sample count apply when it is rendered directly. This avoids multiplying sample counts at each nesting level.

This implementation does **not** yet blur motion inside footage pixels, animated masks or animated effect parameters. Adaptive sampling, optical flow, frame blending, mask-local blur switches and exact AE nested-render semantics remain unfinished. It is transform motion blur, not a Pixel Motion Blur replacement.

## Project compatibility

New projects save as native schema 4. Schemas 1–3 migrate with motion blur off and existing curves/media unchanged; old project files are not rewritten merely by opening them. Use Save As to preserve an older file. The new effect types and motion-blur fields are not understood by older Before Effects versions.

General `.aep` interchange and native AE-plugin hosting are separate unfinished tracks. The isolated host experiment is not loaded by the application.
