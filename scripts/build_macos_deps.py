#!/usr/bin/env python3
"""Build pinned HEVC / AV1 analyzers and a minimal FFmpeg on a native Mac runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import tempfile

from package_macos import ROOT, github_outputs, run

GITL_COMMIT = "f3997d7a76779921a7ae15b02ef382440c8fb376"
HM_COMMIT = "fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce"
AOM_COMMIT = "d772e334cc724105040382a977ebb10dfd393293"  # v3.13.1
FFMPEG_URL = "https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz"
FFMPEG_SHA256 = "05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41"


def verify_archive(archive):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != FFMPEG_SHA256:
        raise RuntimeError("FFmpeg source checksum mismatch; refusing to extract")


def build_ffmpeg(work, aom_prefix, jobs, archive=None):
    if archive is None:
        archive = work / "ffmpeg-8.0.1.tar.xz"
        run(["curl", "--fail", "--location", "--retry", "3", FFMPEG_URL, "--output", archive])
    verify_archive(archive)
    source = work / "ffmpeg-source"
    source.mkdir()
    run(["tar", "-xf", archive, "--strip-components=1", "-C", source])
    build = work / "ffmpeg-build"
    build.mkdir()
    temporary = build / "tmp"
    temporary.mkdir()
    env = dict(os.environ, PKG_CONFIG_LIBDIR=str(aom_prefix / "lib/pkgconfig"), PKG_CONFIG_PATH="",
               TMPDIR=str(temporary))
    # Only the functionality used by BitScope: all four decoders, MP4/raw input
    # and scaled JPEG output. No Homebrew runtime or optional system codec libs.
    options = ["--disable-autodetect", "--disable-shared", "--enable-static", "--disable-network",
               "--disable-doc", "--disable-debug", "--disable-ffplay", "--disable-ffprobe",
               "--disable-everything", "--enable-libaom", "--pkg-config-flags=--static",
               "--enable-decoder=h264,hevc,mpeg4,libaom_av1",
               "--enable-parser=h264,hevc,mpeg4video,av1",
               "--enable-demuxer=mov,h264,hevc,m4v,ivf,av1,obu",
               "--enable-muxer=image2", "--enable-encoder=mjpeg",
               "--enable-filter=scale,format,null,buffer,buffersink,select", "--enable-protocol=file,pipe",
               "--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,av1_frame_merge,av1_frame_split"]
    run([source / "configure", *options], env=env, cwd=build)
    run(["make", "-j", jobs, "ffmpeg"], env=env, cwd=build)
    notices = work / "licenses/ffmpeg"
    notices.mkdir(parents=True)
    for name in ("COPYING.LGPLv2.1", "COPYING.LGPLv3", "LICENSE.md"):
        shutil.copy2(source / name, notices / name)
    (notices / "SOURCE.txt").write_text(
        f"Source: {FFMPEG_URL}\nSHA256: {FFMPEG_SHA256}\nNo source patches.\n"
        + "Configure options:\n" + "\n".join(options) + "\n", encoding="utf-8")
    return build / "ffmpeg"


def checkout(directory, url, commit):
    run(["git", "init", directory])
    run(["git", "-C", directory, "config", "core.autocrlf", "false"])
    run(["git", "-C", directory, "remote", "add", "origin", url])
    run(["git", "-C", directory, "fetch", "--depth=1", "origin", commit])
    run(["git", "-C", directory, "checkout", "--detach", "FETCH_HEAD"])
    verify_commit(directory, commit)


def verify_commit(directory, expected):
    if run(["git", "-C", directory, "rev-parse", "HEAD"], capture=True).strip() != expected:
        raise RuntimeError(f"Unexpected source revision: {directory}")


def apply_patch(directory, patch):
    run(["git", "-C", directory, "apply", "--check", "--ignore-space-change", patch])
    run(["git", "-C", directory, "apply", "--ignore-space-change", patch])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default=platform.machine())
    parser.add_argument("--work-root", type=Path, default=ROOT / "build-macos-deps")
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    if sys.platform != "darwin" or args.arch != platform.machine():
        raise RuntimeError("Build on a Mac matching --arch; cross-architecture dependencies are not supported.")
    if args.jobs < 1:
        raise RuntimeError("--jobs must be positive")
    for tool in ("git", "cmake", "lipo", "clang++", "pkg-config", "curl", "tar", "make"):
        if not shutil.which(tool):
            raise RuntimeError(f"Missing build tool: {tool}")
    if args.arch == "x86_64" and not shutil.which("nasm"):
        raise RuntimeError("Intel AV1 assembly requires NASM (brew install nasm).")
    args.work_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f"deps-{args.arch}-", dir=args.work_root.resolve()))
    print(f"Dependency build: {work}", flush=True)
    gitl, aom = work / "gitl", work / "aom"
    hevc_build, aom_build = work / "hevc-build", work / "aom-build"
    hevc_patch = ROOT / "third_party/hevc-cu-analyzer/bitscope-output-frames.patch"
    av1_patch = ROOT / "third_party/aom-inspection/inspect-output-start.patch"
    nasm_patch = ROOT / "third_party/aom-inspection/nasm-help-compat.patch"
    print("[1/3] Build HEVC CU analyzer", flush=True)
    checkout(gitl, "https://github.com/lheric/GitlHEVCAnalyzer.git", GITL_COMMIT)
    run(["git", "-C", gitl, "submodule", "update", "--init", "--depth=1", "--",
         "appgitlhevcdecoder/HM_18.0/HM"])
    hm = gitl / "appgitlhevcdecoder/HM_18.0/HM"
    verify_commit(hm, HM_COMMIT)
    apply_patch(gitl, hevc_patch)
    apply_patch(hm, gitl / "appgitlhevcdecoder/HM_18.0/sysu-analyzer.patch")
    common = ["-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_OSX_ARCHITECTURES={args.arch}"]
    run(["cmake", "-S", ROOT / "scripts/hevc-helper", "-B", hevc_build,
         f"-DGITL_SOURCE={gitl}", *common])
    run(["cmake", "--build", hevc_build, "--parallel", args.jobs])
    print("[2/3] Build AV1 inspection", flush=True)
    checkout(aom, "https://aomedia.googlesource.com/aom", AOM_COMMIT)
    apply_patch(aom, av1_patch)
    apply_patch(aom, nasm_patch)
    cpu = "arm64" if args.arch == "arm64" else "x86_64"
    aom_prefix = work / "aom-install"
    run(["cmake", "-S", aom, "-B", aom_build, *common,
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", f"-DAOM_TARGET_CPU={cpu}",
         f"-DCMAKE_INSTALL_PREFIX={aom_prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DCONFIG_INSPECTION=1", "-DCONFIG_AV1_ENCODER=0", "-DENABLE_EXAMPLES=1",
         "-DENABLE_TESTS=0", "-DENABLE_DOCS=0", "-DENABLE_TOOLS=0", "-DBUILD_SHARED_LIBS=OFF",
         *(["-DENABLE_NASM=ON", f"-DCMAKE_ASM_NASM_COMPILER={shutil.which('nasm')}"]
           if args.arch == "x86_64" else [])])
    run(["cmake", "--build", aom_build, "--target", "inspect", "aomdec", "aom_pc", "--parallel", args.jobs])
    run(["cmake", "--install", aom_build])
    hevc = hevc_build / "HM_180"
    candidates = [p for p in aom_build.rglob("inspect") if p.is_file()]
    if len(candidates) != 1:
        raise RuntimeError("Could not uniquely locate the AV1 inspection executable")
    av1 = candidates[0]
    print("[3/3] Build checksum-pinned FFmpeg", flush=True)
    ffmpeg = build_ffmpeg(work, aom_prefix, args.jobs)
    for binary in (hevc, av1, ffmpeg):
        if args.arch not in run(["lipo", "-archs", binary], capture=True).split():
            raise RuntimeError(f"Wrong helper architecture: {binary}")
    manifest = work / "dependencies.json"
    manifest.write_text(json.dumps({
        "architecture": args.arch, "gitl_commit": GITL_COMMIT, "hm_commit": HM_COMMIT,
        "aom_commit": AOM_COMMIT, "hevc_analyzer": str(hevc), "av1_inspect": str(av1),
        "ffmpeg": str(ffmpeg), "ffmpeg_url": FFMPEG_URL, "ffmpeg_sha256": FFMPEG_SHA256,
        "patches_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (hevc_patch, av1_patch, nasm_patch)},
    }, indent=2) + "\n", encoding="utf-8")
    github_outputs({"hevc-analyzer": hevc, "av1-inspect": av1, "ffmpeg": ffmpeg,
                    "notices-dir": work / "licenses", "manifest-path": manifest})
    print(f"Dependencies ready: {manifest}", flush=True)


if __name__ == "__main__":
    main()
