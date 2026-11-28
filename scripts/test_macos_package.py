#!/usr/bin/env python3
"""Decode synthetic samples using helpers extracted from the distributable ZIP."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import tempfile

from package_macos import ROOT, run


def decode_fixture(record):
    data = base64.b64decode(record["base64"], validate=True)
    if hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise RuntimeError("Synthetic video fixture checksum mismatch")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", required=True, type=Path)
    parser.add_argument("--test-build", required=True, type=Path)
    args = parser.parse_args()
    package = args.package_dir.resolve()
    build = args.test_build.resolve()
    if (package / "INCOMPLETE.txt").exists():
        raise RuntimeError("Refusing to test an incomplete package")
    # Preserve failure evidence; extracted paths deliberately contain spaces.
    sandbox = Path(tempfile.mkdtemp(prefix="portable test ", dir=build))
    run(["ditto", "-x", "-k", package / "BitScope.zip", sandbox])
    app = sandbox / "BitScope.app"
    ffmpeg = app / "Contents/MacOS/ffmpeg"
    hevc = app / "Contents/Resources/tools/HM_180"
    av1 = app / "Contents/Resources/tools/av1inspect"
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "QT_", "BITSCOPE_"))}
    env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    env["BITSCOPE_FFMPEG"] = str(ffmpeg)
    run(["codesign", "--verify", "--deep", "--strict", app], env=env)
    run([app / "Contents/MacOS/BitScope", "--version"], env=env)
    fixtures = json.loads((ROOT / "tests/fixtures/codec-smoke.json").read_text(encoding="utf-8"))["files"]
    for codec in ("h264", "hevc", "mpeg4", "av1"):
        sample = sandbox / f"{codec}.mp4"
        sample.write_bytes(decode_fixture(fixtures[codec]))
        summary = json.loads(run([app / "Contents/MacOS/BitScope", "--analyze", sample], capture=True, env=env))
        if summary.get("error") or summary.get("frames", 0) < 6:
            raise RuntimeError(f"Packaged app failed to parse {codec}: {summary}")
        verify = ["--verify-images"] if codec in ("h264", "mpeg4") else []
        run([build / "BitScopePreviewSmoke", sample, "0", "5", *verify], env=env)
        if codec in ("h264", "mpeg4"):
            run([build / "BitScopePreviewSmoke", sample, "5"], env=env)
    run([build / "BitScopeHevcCuSmoke", sandbox / "hevc.mp4", hevc, "0", "5"], env=env)
    run([build / "BitScopeAv1BlockSmoke", sandbox / "av1.mp4", av1], env=env)
    (package / "smoke-tests.json").write_text(json.dumps({
        "passed": True, "zip_extracted_and_signature_verified": True,
        "packaged_codecs": ["h264", "hevc", "mpeg4", "av1"],
        "h264_mpeg4_macroblocks_and_image_mapping": True, "hevc_cu": True, "av1_blocks": True,
        "note": "Test drivers use build-machine Qt; application and decoder helpers use extracted bundle.",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: packaged app, four codec previews and detailed blocks; evidence: {sandbox}")


if __name__ == "__main__":
    main()
