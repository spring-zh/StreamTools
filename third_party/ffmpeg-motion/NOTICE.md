# FFmpeg motion-vector extraction

Source: https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz

SHA-256: `05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41`

The independent `bitscope-avc-motion` process uses libavcodec / libavutil.
The minimal build uses FFmpeg's LGPL-2.1-or-later configuration, without GPL or
nonfree options. The Windows preview executable is a separate distribution with
its own license; this notice does not cover it.

`mpeg4-b-mvs.patch` records decoded / derived MPEG-4 Part 2 B-picture vectors in
the existing per-picture motion tables when `AV_CODEC_EXPORT_DATA_MVS` is enabled.
Upstream 8.0.1 does not populate these tables for B pictures. The patch does not
change motion compensation and is inactive for normal preview decoding.
Field-vector mapping is not supported by the helper and is rejected explicitly.

Build source and instructions are in `scripts/avc-motion-helper/main.cpp` and
`scripts/build-motion-helper.sh` in the BitScope source tree. The build outputs
include FFmpeg's license, this patch, helper source and the build script.
Distributors must retain the notices and satisfy LGPL source/relinking obligations.
