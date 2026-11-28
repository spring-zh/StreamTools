"""Pure-Python packaging guard tests; no Qt or macOS needed for these cases."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "package_macos", Path(__file__).resolve().parents[1] / "scripts/package_macos.py")
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class PackagingTests(unittest.TestCase):
    def test_qt68_help_exit_one_does_not_add_unsupported_signing_option(self):
        help_text = "Usage: macdeployqt app-bundle [options]\n   -codesign=<ident> : Run codesign\n"
        with patch.object(package.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, help_text)) as run:
            self.assertEqual(package.macdeployqt_signing_options(Path("/Qt 6.8/bin/macdeployqt")), [])
        self.assertEqual(run.call_args.args[0], ["/Qt 6.8/bin/macdeployqt", "-h"])
        self.assertEqual(run.call_args.kwargs["stderr"], subprocess.STDOUT)

    def test_newer_qt_explicitly_defers_signing_until_after_relocation(self):
        help_text = "Usage: macdeployqt app-bundle [options]\n   -no-codesign : Disable code signing\n"
        with patch.object(package.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, help_text)):
            self.assertEqual(package.macdeployqt_signing_options(Path("macdeployqt")), ["-no-codesign"])

    def test_broken_help_probe_is_not_mistaken_for_old_qt(self):
        for code, output in ((1, "dyld: Library not loaded"), (-9, ""), (2, "Usage: macdeployqt")):
            with self.subTest(code=code), patch.object(package.subprocess, "run", return_value=subprocess.CompletedProcess([], code, output)):
                with self.assertRaisesRegex(RuntimeError, "无法读取 macdeployqt"):
                    package.macdeployqt_signing_options(Path("macdeployqt"))

    def test_deployment_preserves_spaces_helpers_and_plugins(self):
        for options in ([], ["-no-codesign"]):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory:
                app = Path(directory) / "folder with spaces/BitScope.app"
                executables = [app / "Contents/MacOS/ffmpeg", app / "Contents/Resources/tools/HM_180",
                               app / "Contents/Resources/tools/av1inspect", app / "Contents/PlugIns/platforms/libqcocoa.dylib"]
                log_path = Path(directory) / "deployment.log"
                with patch.object(package, "macdeployqt_signing_options", return_value=options), \
                     patch.object(package.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
                    package.deploy_qt(Path("/Qt SDK/bin/macdeployqt"), app, Path("/Qt SDK"), executables, log_path)
                self.assertEqual(run.call_args.args[0], ["/Qt SDK/bin/macdeployqt", str(app), *options,
                                 "-no-plugins", "-verbose=2", "-libpath=/Qt SDK/lib",
                                 *[f"-executable={p}" for p in executables]])
                self.assertIn("Command:", log_path.read_text())

    def test_deployment_failure_surfaces_log_and_does_not_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "deployment.log"

            def fail(args, **kwargs):
                kwargs["stdout"].write("first diagnostic\n" + "progress\n" * 150 + "ERROR: Missing framework\n")
                return subprocess.CompletedProcess(args, 1)

            with patch.object(package, "macdeployqt_signing_options", return_value=[]), \
                 patch.object(package.subprocess, "run", side_effect=fail) as run:
                with self.assertRaises(RuntimeError) as raised:
                    package.deploy_qt(Path("macdeployqt"), Path("app"), Path("qt"), [], log_path)
            self.assertIn("ERROR: Missing framework", str(raised.exception))
            self.assertIn(str(log_path), str(raised.exception))
            self.assertNotIn("first diagnostic", str(raised.exception))
            self.assertIn("first diagnostic", log_path.read_text())
            self.assertEqual(run.call_count, 1)

    def test_github_outputs_preserve_spaces_and_existing_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "outputs"
            output.write_text("previous=ok\n")
            with patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}):
                package.github_outputs({"zip-path": Path("/a folder/BitScope.zip")})
            self.assertEqual(output.read_text(), "previous=ok\nzip-path=/a folder/BitScope.zip\n")

    def test_github_outputs_reject_newline_injection(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "outputs"
            with patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}):
                with self.assertRaisesRegex(RuntimeError, "single-line"):
                    package.github_outputs({"path": "/tmp/file\nother=bad"})
            self.assertFalse(output.exists())

    def test_github_outputs_are_optional_for_local_builds(self):
        with patch.dict(os.environ, {}, clear=True):
            package.github_outputs({"zip-path": "/tmp/BitScope.zip"})

    def test_otool_parser_keeps_paths_with_spaces_and_excludes_id(self):
        library = "/a folder/libexample.dylib"
        outputs = [f"{library}:\n{library}\n",
                   f"{library}:\n\t{library} (compatibility version 1.0.0, current version 1.0.0)\n"
                   "\t@rpath/a folder/libfoo.dylib (compatibility version 1.0.0, current version 1.0.0)\n"]
        with patch.object(package, "run", side_effect=outputs):
            self.assertEqual(package.dependencies(Path(library)), ["@rpath/a folder/libfoo.dylib"])

    def test_rpath_parser(self):
        with patch.object(package, "run", return_value="Load command 12\n cmd LC_RPATH\n cmdsize 48\n path @loader_path/../Frameworks (offset 12)\n"):
            self.assertEqual(package.rpaths(Path("binary")), ["@loader_path/../Frameworks"])

    def test_dependency_resolves_inside_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "folder with spaces/BitScope.app"
            library = app / "Contents/Frameworks/libfoo.dylib"
            library.parent.mkdir(parents=True)
            library.touch()
            binary = app / "Contents/MacOS/ffmpeg"
            binary.parent.mkdir()
            with patch.object(package, "rpaths", return_value=[]):
                self.assertEqual(package.resolve_dependency("@rpath/libfoo.dylib", binary, app), library.resolve())
            self.assertEqual(package.resolve_dependency("@loader_path/../Frameworks/libfoo.dylib", binary, app), library.resolve())

    def test_symlink_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "BitScope.app"
            app.mkdir()
            external = Path(directory) / "outside.dylib"
            external.touch()
            (app / "escape.dylib").symlink_to(external)
            self.assertFalse(package.inside(app / "escape.dylib", app))
            with patch.object(package, "macho_files", return_value=[]):
                with self.assertRaisesRegex(RuntimeError, "符号链接"):
                    package.relocate_and_audit(app, "arm64")

    def test_wrong_architecture_fails(self):
        with patch.object(package, "macho_files", return_value=[Path("BitScope")]), \
             patch.object(package, "run", return_value="x86_64"):
            with self.assertRaisesRegex(RuntimeError, "架构不匹配"):
                package.relocate_and_audit(Path("BitScope.app"), "arm64")

    def test_external_library_fails_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "BitScope.app"
            binary = app / "Contents/MacOS/BitScope"
            external = Path(directory) / "libexternal.dylib"
            external.touch()
            with patch.object(package, "macho_files", return_value=[binary]), \
                 patch.object(package, "run", return_value="arm64"), \
                 patch.object(package, "dependencies", return_value=[str(external)]):
                with self.assertRaisesRegex(RuntimeError, "仍依赖本机"):
                    package.relocate_and_audit(app, "arm64")

    def test_minimum_version_is_maximum_of_all_binaries(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory)
            binaries = [app / "BitScope", app / "ffmpeg"]

            def fake_run(args, **kwargs):
                if args[0] == "lipo":
                    return "arm64"
                return "cmd LC_VERSION_MIN_MACOSX\ncmdsize 16\nversion 12.0\n" if args[-1] == binaries[0] else "cmd LC_BUILD_VERSION\nminos 15.0\n"

            with patch.object(package, "macho_files", return_value=binaries), \
                 patch.object(package, "run", side_effect=fake_run), \
                 patch.object(package, "dependencies", return_value=[]), \
                 patch.object(package, "rpaths", return_value=[]):
                self.assertEqual(package.relocate_and_audit(app, "arm64")[1], "15.0")

    def test_explicit_missing_helper_does_not_silently_fallback(self):
        with patch.object(package, "executable", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "不存在或不可执行"):
                package.choose_helper("/missing", "HELPER", "HELPER", "helper", Path("build"))

    def test_signed_inode_refresh_preserves_bytes_and_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "signed binary"
            binary.write_bytes(b"signed Mach-O contents")
            binary.chmod(0o755)
            before = binary.stat()
            package.refresh_signed_inodes([binary])
            self.assertEqual(binary.read_bytes(), b"signed Mach-O contents")
            self.assertEqual(binary.stat().st_mode, before.st_mode)
            self.assertNotEqual(binary.stat().st_ino, before.st_ino)
            self.assertEqual(list(Path(directory).iterdir()), [binary])


if __name__ == "__main__":
    unittest.main()
