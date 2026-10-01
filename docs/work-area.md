# Work Area in Before Effects 0.7

The Work Area is a saved composition range. Its gray band and blue handles sit below the Timeline ruler, independently of the current-time indicator (CTI) and the Time Navigator above it. Layer rows retain their 17 px spacing.

Open `fixtures/work-area-demo.json` for the editable example: a three-second composition with Work Area frames 30–74 at 30 fps. Exporting that range produces 45 frames / 1.5 seconds.

## Edit the range

- Press **B** to make the CTI's containing frame the first frame.
- Press **N** to include the CTI's containing frame as the last frame.
- Drag either blue handle to change its boundary, or drag the center to move the range while preserving its frame count. Edits snap to legal frame boundaries and clamp to the composition.
- Double-click the Work Area to restore the full composition.
- Undo/Redo applies to each completed edit. Escape cancels a drag.

B/N retain normal typing behavior in text and number editors. Work Area edits preserve the CTI, visible Timeline range, layers and animation. The range always includes at least one output frame, including a composition shorter than one frame or a partial final frame.

## Preview and export

The Timeline's **Preview** choice selects **Full composition** or **Work Area**; the default is Full composition. In Work Area mode, playback begins at the CTI when it is inside the range, otherwise at the range start. Audio and video loop over the same interval. Changing the range or composition timing rebuilds the playback snapshot.

The Render Queue has an independent **Time span** choice with the same options and default. The queued render retains the submitted first frame and frame count even if the project is edited afterward. PNG sequence filenames retain absolute source-frame numbers. A still export always uses the CTI, including when it is outside the Work Area.

## Persistence and timing

Native JSON schema 5 stores a custom range as exact rational `start` and exclusive `end` values. `workArea: null` means the full current composition. Older schemas 1–4 open with a full range. Saving writes schema 5; preserve a copy for older application versions.

Changing the frame rate snaps boundary times to the nearest legal frame, with half-frame ties choosing the later boundary. The exact composition end remains legal for a partial final frame. Full ranges follow duration changes; custom ranges clamp to the duration and remain nonempty. A newly created precomposition starts with its own full range.

MP4 export uses exact common movie/video timebases for the selected duration and audio samples. It rejects a timebase beyond AVFoundation’s 32-bit limit rather than silently rounding a very short range to zero.

These are the approved Before Effects policies. This block does not establish exact equivalence to unmeasured After Effects boundary, shortcut-profile or audio-loop behavior. It adds no AE launch or new runtime dependency.
