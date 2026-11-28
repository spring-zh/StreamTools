# Synthetic video fixtures

`mpeg4-open-gop.mp4` is generated from FFmpeg's `testsrc2`, not user video.
It contains 36 frames (128×96, 12 fps), I-VOPs at coded indices 0/10/22/34,
and two leading B-VOPs after each later I-VOP. It exercises the first-GOP,
ordinary random-access, preceding-reference, and image/MB mapping paths.

Generation command (FFmpeg 8.0.1):

```sh
ffmpeg -f lavfi -i testsrc2=size=128x96:rate=12 -frames:v 36 -an \
  -c:v mpeg4 -bf 2 -g 12 -sc_threshold 1000000000 -q:v 4 \
  -threads 1 -pix_fmt yuv420p mpeg4-open-gop.mp4
```

`BitScopePreviewSmoke ... 10 18 --verify-images` crosses an open GOP boundary.
`... 23 24 --verify-images` requests only leading B frames and must start at
I-VOP 10, not at the beginning of the file or at I-VOP 22.
`... 28 28 --verify-images` must retain the fast path starting at I-VOP 22.
The smoke test compares JPEG pixels with ordinary sequential FFmpeg decoding
and checks macroblock addresses, QPs, types, and completeness.

These open-GOP checks are manual developer regressions, not part of the macOS
or Windows packaging scripts. Packaging retains its original small four-codec
smoke tests.

`codec-smoke.json` contains the pre-existing four-codec fixtures, their
generation parameters and SHA-256 hashes.
