# ffmpeg.wasm components

This directory contains browser runtime files from:

- `@ffmpeg/ffmpeg` 0.12.15 — MIT License — https://github.com/ffmpegwasm/ffmpeg.wasm
- `@ffmpeg/core` 0.12.10 — MIT License — https://github.com/ffmpegwasm/ffmpeg.wasm

The WebAssembly core bundles FFmpeg and its enabled libraries. FFmpeg is licensed under LGPL/GPL terms depending on the configured build. Source and build scripts are available from the upstream project above and https://ffmpeg.org/.

These files are loaded locally and only when the browser's native decoder cannot produce a requested frame preview.
