"""Pure-Python packaging guard tests; no Qt or macOS needed for these cases."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "package_macos", Path(__file__).resolve().parents[1] / "scripts/package_macos.py")
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class PackagingTests(unittest.TestCase):
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
