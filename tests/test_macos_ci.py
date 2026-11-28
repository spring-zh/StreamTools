"""CI helper guard tests; no network, compiler or macOS runtime required."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


with patch.dict(sys.modules, {"package_macos": load("package_macos")}):
    deps = load("build_macos_deps")
    smoke = load("test_macos_package")


class MacCiTests(unittest.TestCase):
    def test_all_committed_synthetic_fixtures_have_valid_checksums(self):
        fixtures = json.loads((SCRIPTS.parent / "tests/fixtures/codec-smoke.json").read_text())["files"]
        self.assertEqual(set(fixtures), {"h264", "hevc", "mpeg4", "av1"})
        for record in fixtures.values():
            self.assertIn(b"ftyp", smoke.decode_fixture(record)[:32])

    def test_corrupt_fixture_fails(self):
        with self.assertRaisesRegex(RuntimeError, "checksum"):
            smoke.decode_fixture({"base64": "YWJj", "sha256": "wrong"})

    def test_bad_ffmpeg_archive_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "source.tar.xz"
            archive.write_bytes(b"corrupted source")
            with self.assertRaisesRegex(RuntimeError, "checksum"):
                deps.verify_archive(archive)

    def test_wrong_revision_fails(self):
        with patch.object(deps, "run", return_value="unexpected\n"):
            with self.assertRaisesRegex(RuntimeError, "revision"):
                deps.verify_commit(Path("source"), deps.AOM_COMMIT)

    def test_expected_revision_accepts_trailing_newline(self):
        with patch.object(deps, "run", return_value=deps.HM_COMMIT + "\n"):
            deps.verify_commit(Path("source"), deps.HM_COMMIT)

    def test_patch_is_checked_before_application(self):
        with patch.object(deps, "run") as run:
            deps.apply_patch(Path("source"), Path("fix.patch"))
        calls = [call.args[0] for call in run.call_args_list]
        self.assertIn("--check", calls[0])
        self.assertNotIn("--check", calls[1])
        self.assertEqual(calls[0][-1], Path("fix.patch"))

    def test_bad_patch_stops_before_application(self):
        with patch.object(deps, "run", side_effect=RuntimeError("bad patch")) as run:
            with self.assertRaisesRegex(RuntimeError, "bad patch"):
                deps.apply_patch(Path("source"), Path("fix.patch"))
        self.assertEqual(run.call_count, 1)

    def test_cross_architecture_rejected_before_build(self):
        with patch.object(sys, "argv", ["build_macos_deps.py", "--arch", "x86_64"]), \
             patch.object(sys, "platform", "darwin"), \
             patch.object(deps.platform, "machine", return_value="arm64"), \
             patch.object(deps, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "matching --arch"):
                deps.main()
        run.assert_not_called()

    def test_invalid_jobs_rejected_before_build(self):
        with patch.object(sys, "argv", ["build_macos_deps.py", "--arch", "arm64", "--jobs", "0"]), \
             patch.object(sys, "platform", "darwin"), \
             patch.object(deps.platform, "machine", return_value="arm64"), \
             patch.object(deps, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "positive"):
                deps.main()
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
