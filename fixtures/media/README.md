# Original synthetic media fixtures

Created locally for MotionProof QA using the installed FFmpeg7.1.1 and Apple's H.264 encoder; no third-party footage or audio. FFmpeg is a developer verification tool and is not included in the app bundle.

- `source.mp4`: FFmpeg testsrc2 with burned-in time/frame annotation,640×360,120 frames at30000/1001 (4.004s),48kHz mono AAC tone from1.0 to1.1s. Created during the native-media preflight and copied unchanged into the fixture directory. Exact original CLI was not retained; the committed file/hash is the fixed oracle.
- `quadrants.mp4`:64×32,30fps,1s; red top-left, lime top-right, blue bottom-left, white bottom-right. Independent orientation/channel oracle.
- `variable.mp4`: testsrc2 at10fps, retain frame numbers0,2,7 with VFR output; PTS0,.2,.7s. No invented constant media FPS is stored.
- `tone.wav`: source.mp4 audio decoded with `ffmpeg -i source.mp4 -vn -c:a pcm_s16le tone.wav` for audio-only import checks.

Regeneration is not a substitute for preserving these committed fixture bytes. tests/media_checks.cpp checks exact timestamps, known colors and independently specified stereo gains. The delivery evidence includes external ffprobe and decoded-PCM results, not merely the application's own reader.
