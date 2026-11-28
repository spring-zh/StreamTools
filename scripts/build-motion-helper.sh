#!/usr/bin/env bash
# Builds a private, minimal static FFmpeg for motion extraction. No Qt ABI
# boundary is crossed; the helper communicates with the UI over text records.
set -euo pipefail
if [[ $# != 2 ]]; then
  echo 'usage: build-motion-helper.sh NEW_WORK_DIRECTORY JOBS' >&2
  exit 2
fi
motion_work="$1"
motion_jobs="$2"
[[ "$motion_jobs" =~ ^[1-9][0-9]*$ ]] || exit 2
motion_root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir "$motion_work"
motion_work="$(cd "$motion_work" && pwd)"
curl --fail --location --retry 3 --max-time 180 https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz -o "$motion_work/ffmpeg.tar.xz"
motion_hash=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
if command -v sha256sum >/dev/null; then
  motion_actual="$(sha256sum "$motion_work/ffmpeg.tar.xz")"
else
  motion_actual="$(shasum -a 256 "$motion_work/ffmpeg.tar.xz")"
fi
[[ "${motion_actual%% *}" == "$motion_hash" ]] || { echo 'FFmpeg checksum mismatch' >&2; exit 1; }
mkdir "$motion_work/source" "$motion_work/build"
tar -xf "$motion_work/ffmpeg.tar.xz" --strip-components=1 -C "$motion_work/source"
patch -d "$motion_work/source" -p1 < "$motion_root/third_party/ffmpeg-motion/mpeg4-b-mvs.patch"
motion_cxx="${CXX:-c++}"
motion_link=()
motion_suffix=''
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  motion_suffix='.exe'
  motion_link=(-static -municode)
fi
cd "$motion_work/build"
mkdir tmp
export TMPDIR="$motion_work/build/tmp"
"$motion_work/source/configure" --prefix="$motion_work/install" \
  --disable-autodetect --disable-shared --enable-static --disable-network \
  --disable-doc --disable-debug --disable-programs --disable-everything \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale \
  --enable-decoder=h264,mpeg4 --enable-parser=h264,mpeg4video
make -j "$motion_jobs"
make install
export PKG_CONFIG_LIBDIR="$motion_work/install/lib/pkgconfig"
export PKG_CONFIG_PATH=''
# pkg-config emits shell-escaped flags; a Bash array preserves paths with spaces.
eval "motion_flags=($(pkg-config --static --cflags --libs libavcodec libavutil))"
"$motion_cxx" -std=c++17 -O2 -DBITSCOPE_PATCHED_MPEG4_MVS=1 \
  "$motion_root/scripts/avc-motion-helper/main.cpp" \
  -o "$motion_work/bitscope-avc-motion$motion_suffix" "${motion_link[@]}" "${motion_flags[@]}"
mkdir "$motion_work/licenses"
cp "$motion_work/source/COPYING.LGPLv2.1" "$motion_work/licenses/"
cp "$motion_root/third_party/ffmpeg-motion/mpeg4-b-mvs.patch" "$motion_work/licenses/"
cp "$motion_root/scripts/avc-motion-helper/main.cpp" "$motion_work/licenses/bitscope-motion-main.cpp"
cp "$motion_root/scripts/build-motion-helper.sh" "$motion_work/licenses/"
echo "Patched motion helper: $motion_work/bitscope-avc-motion$motion_suffix"
