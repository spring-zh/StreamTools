#!/usr/bin/env python3
"""Build a relocatable macOS folder; only the packaging machine needs Python/Qt."""
import argparse
from collections import deque
import datetime
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
MACH_MAGICS = {bytes.fromhex(x) for x in (
    "feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca",
    "cafebabf", "bfbafeca")}


def run(args, capture=False, env=None, cwd=None):
    result = subprocess.run([str(x) for x in args], check=True, text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None, env=env, cwd=cwd)
    return result.stdout if capture else ""


def macdeployqt_signing_options(deploy):
    # Qt 6.8 defaults to no signing and rejects -no-codesign. Newer Qt
    # defaults to ad-hoc signing. Detect the option instead of assuming a
    # version; we sign the whole bundle ourselves after rewriting its paths.
    result = subprocess.run([str(deploy), "-h"], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False, timeout=30)
    help_text = result.stdout or ""
    # macdeployqt prints help to stderr and returns 1, including on Qt 6.8.
    if result.returncode not in (0, 1) or "Usage: macdeployqt" not in help_text:
        raise RuntimeError(f"无法读取 macdeployqt 支持的参数（退出码 {result.returncode}）：\n{help_text}")
    return ["-no-codesign"] if re.search(r"(?m)^\s*-no-codesign\s*:", help_text) else []


def deploy_qt(deploy, app, qt_prefix, executables, log_path):
    args = [str(deploy), str(app), *macdeployqt_signing_options(deploy),
            "-no-plugins", "-verbose=2", f"-libpath={qt_prefix / 'lib'}",
            *[f"-executable={p}" for p in executables]]
    with log_path.open("w", encoding="utf-8") as log:
        log.write("Command: " + json.dumps(args, ensure_ascii=False) + "\n")
        log.flush()
        result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, check=False)
    if result.returncode != 0:
        # Keep the full artifact, but also expose the real error in the Actions
        # step output. A generic CalledProcessError hides all Qt diagnostics.
        with log_path.open(encoding="utf-8", errors="replace") as log:
            tail = "".join(deque(log, maxlen=120))
        raise RuntimeError(f"macdeployqt 部署失败（退出码 {result.returncode}）。"
                           f"完整日志：{log_path}\n最后 120 行：\n{tail}")


def github_outputs(values):
    """Expose exact successful build paths to CI, never select stale artifacts."""
    destination = os.environ.get("GITHUB_OUTPUT")
    if not destination:
        return
    lines = []
    for key, value in values.items():
        value = str(value)
        if any(char in key + value for char in "\r\n"):
            raise RuntimeError("GitHub output names and paths must be single-line")
        lines.append(f"{key}={value}\n")
    with open(destination, "a", encoding="utf-8") as stream:
        stream.writelines(lines)


def cache_value(cache, key):
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith(key + ":"):
                return line.split("=", 1)[1]
    return ""


def executable(value):
    if not value:
        return None
    candidate = Path(value).expanduser()
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return candidate.resolve()
    found = shutil.which(value)
    return Path(found).resolve() if found else None


def choose_helper(explicit, variable, cache_key, name, build_dir):
    override = explicit or os.environ.get(variable)
    if override:
        found = executable(override)
        if not found:
            raise RuntimeError(f"{variable} 指定的程序不存在或不可执行：{override}")
        return found
    for directory in (ROOT / "build-qt", ROOT / "build", build_dir):
        for candidate in (
            cache_value(directory / "CMakeCache.txt", cache_key),
            str(directory / "BitScope.app/Contents/Resources/tools" / name),
        ):
            found = executable(candidate)
            if found:
                return found
    found = executable(name)
    if not found:
        raise RuntimeError(f"缺少 {name}。请用 {variable} 或命令行参数指定带本项目补丁的 macOS 程序；"
                           "不会生成缺少详细块分析功能的发布包。")
    return found


def is_macho(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in MACH_MAGICS


def macho_files(app):
    return sorted(p for p in app.rglob("*") if is_macho(p))


def dependencies(path):
    # Includes LC_ID_DYLIB, excluded by comparing it to otool -D below.
    ids = run(["otool", "-D", path], capture=True).splitlines()[1:]
    ids = {line.strip() for line in ids if line and not line.endswith(":")}
    return list(dict.fromkeys(
        match.group(1) for line in run(["otool", "-L", path], capture=True).splitlines()
        if (match := re.match(r"\s+(.+?) \(compatibility version", line))
        and match.group(1) not in ids))


def rpaths(path):
    return list(dict.fromkeys(re.findall(
        r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset",
        run(["otool", "-l", path], capture=True))))


def inside(path, directory):
    try:
        path.resolve().relative_to(directory.resolve())
        return True
    except ValueError:
        return False


def resolve_dependency(dep, binary, app, search_roots=()):
    main_dir = app / "Contents/MacOS"
    executable_dir = binary.parent if binary.parent.name in ("MacOS", "tools", "Resources") else main_dir

    def expand(value):
        return Path(value.replace("@loader_path", str(binary.parent))
                    .replace("@executable_path", str(executable_dir)))

    if dep.startswith("@rpath/"):
        suffix = dep[len("@rpath/"):]
        roots = [expand(p) for p in rpaths(binary)]
        roots += [app / "Contents/Frameworks", *search_roots]
        candidates = [root / suffix for root in roots]
    else:
        candidates = [expand(dep)]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise RuntimeError(f"无法定位动态库：{binary}\n  {dep}")


def collect_sources(seeds, app, qt_prefix):
    """Record dependency origins before deployment rewrites install names."""
    seen, pending = set(), list(seeds)
    while pending:
        binary = pending.pop().resolve()
        if binary in seen:
            continue
        seen.add(binary)
        for dep in dependencies(binary):
            if not dep.startswith(SYSTEM_PREFIXES):
                pending.append(resolve_dependency(dep, binary, app, [qt_prefix / "lib"]))
    return seen


def relocate_and_audit(app, arch):
    files = macho_files(app)
    minimum = (0, 0)
    records = []
    for binary in files:
        architectures = run(["lipo", "-archs", binary], capture=True).strip().split()
        if arch not in architectures:
            raise RuntimeError(f"架构不匹配（需要 {arch}）：{binary}: {architectures}")
        # Resolve every dependency strictly inside the bundle, then eliminate
        # dependence on the working directory, inherited rpaths or Homebrew.
        changes = []
        for dep in dependencies(binary):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            resolved = resolve_dependency(dep, binary, app)
            if not inside(resolved, app):
                raise RuntimeError(f"部署后仍依赖本机文件：{binary}\n  {dep} -> {resolved}")
            replacement = "@loader_path/" + os.path.relpath(resolved, binary.parent)
            if replacement != dep:
                changes += ["-change", dep, replacement]
        # All non-system dependencies now use loader-relative names.
        for path in rpaths(binary):
            changes += ["-delete_rpath", path]
        if changes:
            run(["install_name_tool", *changes, binary], capture=True)
        load_commands = run(["otool", "-arch", arch, "-l", binary], capture=True)
        versions = re.findall(r"\bminos (\d+(?:\.\d+)+)", load_commands)
        versions += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version (\d+(?:\.\d+)+)", load_commands)
        if not versions:
            raise RuntimeError(f"无法确定最低 macOS 版本：{binary}")
        version = max(tuple(map(int, v.split("."))) for v in versions)
        minimum = max(minimum, version)
        records.append({"file": str(binary.relative_to(app)), "architectures": architectures,
                        "minimum_macos": ".".join(map(str, version))})
    # Independently check the rewritten load commands, including symlinks.
    for path in app.rglob("*"):
        if path.is_symlink() and (not path.exists() or not inside(path, app)):
            raise RuntimeError(f"发布包内存在失效或指向外部的符号链接：{path}")
    for binary in files:
        for dep in dependencies(binary):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            if not dep.startswith("@loader_path/") or not inside(resolve_dependency(dep, binary, app), app):
                raise RuntimeError(f"动态库审计未通过：{binary}: {dep}")
    return files, ".".join(map(str, minimum)), records


def collect_notices(sources, destination):
    destination.mkdir(parents=True, exist_ok=True)
    packages = set()
    for source in sources:
        for parent in source.parents:
            if (parent / "INSTALL_RECEIPT.json").is_file():
                packages.add(parent)
                break
    copied = []
    for package in sorted(packages):
        target = destination / (package.parent.name + "-" + package.name)
        target.mkdir(exist_ok=True)
        for path in package.iterdir():
            if path.is_file() and (path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT"))
                                   or path.name in ("INSTALL_RECEIPT.json", "sbom.spdx.json")):
                shutil.copy2(path, target / path.name)
                copied.append(str((target / path.name).relative_to(destination)))
        for subdir in ("LICENSES", "licenses", "share/licenses"):
            if (package / subdir).is_dir():
                shutil.copytree(package / subdir, target / subdir, dirs_exist_ok=True)
    return copied


def refresh_signed_inodes(files):
    # macOS may retain the old signature of binaries modified in place by
    # install_name_tool/codesign. Keep the signed bytes and metadata unchanged,
    # but replace the staged files with fresh inodes before execution.
    for binary in files:
        fd, name = tempfile.mkstemp(prefix=".bitscope-signed-", dir=binary.parent)
        os.close(fd)
        temporary = Path(name)
        try:
            shutil.copy2(binary, temporary)
            os.replace(temporary, binary)
        finally:
            if temporary.exists():
                temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description="编译并打包自包含的 macOS BitScope 文件夹（默认完整功能）。")
    parser.add_argument("--qt-prefix", help="Qt 安装根目录，例如 /opt/homebrew/opt/qt")
    parser.add_argument("--ffmpeg", default=os.environ.get("BITSCOPE_FFMPEG"), help="macOS ffmpeg 可执行文件")
    parser.add_argument("--hevc-analyzer", help="带 BitScope 补丁的 HM_180")
    parser.add_argument("--av1-inspect", help="带 BitScope 补丁的 libaom inspect")
    parser.add_argument("--avc-motion", help="H.264 运动矢量提取器 bitscope-avc-motion")
    parser.add_argument("--dependency-notices", type=Path, help="额外依赖许可目录（CI 源码构建使用）")
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default=platform.machine())
    parser.add_argument("--build-dir", type=Path, help="专用构建目录")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist", help="发布文件夹的父目录（不覆盖已有包）")
    parser.add_argument("--sign-identity", default="-", help="默认 ad-hoc；可指定 Developer ID Application 证书")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise RuntimeError("此脚本需要在 macOS 上运行。")
    if args.jobs < 1:
        raise RuntimeError("--jobs 必须大于 0")
    for tool in ("cmake", "xcrun", "otool", "lipo", "install_name_tool", "codesign", "ditto"):
        if not shutil.which(tool):
            raise RuntimeError(f"缺少 {tool}，请安装 CMake / Xcode Command Line Tools。")
    if args.qt_prefix:
        qt_prefix = Path(args.qt_prefix).expanduser().resolve()
        deploy = executable(str(qt_prefix / "bin/macdeployqt"))
    else:
        deploy = executable("macdeployqt")
        # Preserve the public Qt prefix: Homebrew's macdeployqt resolves into qtbase.
        qt_prefix = Path(shutil.which("macdeployqt")).parent.parent if deploy else None
    if not deploy:
        raise RuntimeError("找不到 macdeployqt；请用 --qt-prefix 指定 Qt 6 安装目录。")
    qtpaths = executable(str(qt_prefix / "bin/qtpaths"))
    if not qtpaths:
        raise RuntimeError("Qt 安装中缺少 qtpaths，无法确定匹配的插件目录。")
    plugin_root = Path(run([qtpaths, "--query", "QT_INSTALL_PLUGINS"], capture=True).strip())
    build_dir = (args.build_dir or ROOT / ("build-macos-package-" + args.arch)).expanduser().resolve()
    if build_dir == ROOT:
        raise RuntimeError("构建目录不能是源码根目录。")
    ffmpeg = executable(args.ffmpeg or "ffmpeg")
    if not ffmpeg:
        raise RuntimeError("找不到 FFmpeg，请安装或用 --ffmpeg 指定。")
    hevc = choose_helper(args.hevc_analyzer, "BITSCOPE_HEVC_ANALYZER", "BITSCOPE_HEVC_ANALYZER", "HM_180", build_dir)
    av1 = choose_helper(args.av1_inspect, "BITSCOPE_AV1_INSPECT", "BITSCOPE_AV1_INSPECT_EXECUTABLE", "av1inspect", build_dir)
    motion = choose_helper(args.avc_motion, "BITSCOPE_AVC_MOTION", "BITSCOPE_AVC_MOTION_EXECUTABLE", "bitscope-avc-motion", build_dir)
    for helper in (ffmpeg, hevc, av1, motion):
        if args.arch not in run(["lipo", "-archs", helper], capture=True).split():
            raise RuntimeError(f"辅助程序架构与 {args.arch} 不符：{helper}")
    print("[1/6] 编译 Release 主程序…", flush=True)
    run(["cmake", "-S", ROOT, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_PREFIX_PATH={qt_prefix}", f"-DCMAKE_OSX_ARCHITECTURES={args.arch}",
         f"-DBITSCOPE_HEVC_ANALYZER={hevc}", f"-DBITSCOPE_AV1_INSPECT_EXECUTABLE={av1}",
         f"-DBITSCOPE_AVC_MOTION_EXECUTABLE={motion}",
         "-DBITSCOPE_BUILD_TESTS=OFF"])
    run(["cmake", "--build", build_dir, "--config", "Release", "--target", "BitScopeQt", "--parallel", args.jobs])
    source_app = build_dir / "BitScope.app"
    if not source_app.is_dir():
        source_app = build_dir / "Release/BitScope.app"
    args.output_dir = args.output_dir.expanduser().resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = Path(tempfile.mkdtemp(prefix=f"BitScope-macOS-{args.arch}-{stamp}-", dir=args.output_dir))
    incomplete = output / "INCOMPLETE.txt"
    incomplete.write_text("打包尚未完成。只有此文件消失后才能分发。\n", encoding="utf-8")
    print(f"发布目录：{output}", flush=True)
    app = output / "BitScope.app"
    shutil.copytree(source_app, app, symlinks=True)
    bundled_ffmpeg = app / "Contents/MacOS/ffmpeg"
    shutil.copy2(ffmpeg, bundled_ffmpeg)
    helpers = [bundled_ffmpeg, app / "Contents/Resources/tools/HM_180", app / "Contents/Resources/tools/av1inspect"]
    helpers.append(app / "Contents/Resources/tools/bitscope-avc-motion")
    # This Widgets app uses Fusion style and JPEG thumbnails, not SVG/PDF/QML
    # or virtual keyboards. Avoid deploying unrelated installed Qt plugins.
    plugins = []
    plugin_sources = []
    for name in ("platforms/libqcocoa.dylib", "imageformats/libqjpeg.dylib"):
        source = plugin_root / name
        if not source.is_file():
            raise RuntimeError(f"缺少必要 Qt 插件：{source}")
        target = app / "Contents/PlugIns" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        plugins.append(target)
        plugin_sources.append(source)
    sources = collect_sources([app / "Contents/MacOS/BitScope", ffmpeg, hevc, av1, motion, *plugin_sources], app, qt_prefix)
    print("[2/6] 收集 Qt、FFmpeg 及分析器的动态库…", flush=True)
    deploy_qt(deploy, app, qt_prefix, helpers + plugins, output / "deployment.log")
    (app / "Contents/Resources/qt.conf").write_text("[Paths]\nPlugins = PlugIns\n", encoding="utf-8")
    print("[3/6] 修正路径，检查全部动态库和目标架构…", flush=True)
    files, minimum, records = relocate_and_audit(app, args.arch)
    plist_path = app / "Contents/Info.plist"
    with plist_path.open("rb") as stream:
        info = plistlib.load(stream)
    info["LSMinimumSystemVersion"] = minimum
    # CMake may generate a read-only plist; change only the staged copy.
    plist_path.chmod(plist_path.stat().st_mode | stat.S_IWUSR)
    with plist_path.open("wb") as stream:
        plistlib.dump(info, stream)
    notices = collect_notices(sources, app / "Contents/Resources/licenses/dependencies")
    if args.dependency_notices:
        shutil.copytree(args.dependency_notices, app / "Contents/Resources/licenses/ci-sources", symlinks=False)
    manifest = {"architecture": args.arch, "minimum_macos": minimum,
                "signing": "ad-hoc" if args.sign_identity == "-" else args.sign_identity,
                "notarized": False, "binaries": records, "dependency_sources": sorted(map(str, sources)),
                "collected_notices": notices}
    (output / "build-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"[4/6] 签名（{manifest['signing']}）…", flush=True)
    sign = ["codesign", "--force", "--sign", args.sign_identity]
    if args.sign_identity != "-":
        sign += ["--options", "runtime", "--timestamp"]
    # Sign from the inside out; never modify any binaries after signing.
    for binary in files:
        run([*sign, binary], capture=True)
    for framework in sorted(app.rglob("*.framework"), key=lambda p: len(p.parts), reverse=True):
        run([*sign, framework], capture=True)
    run([*sign, app], capture=True)
    refresh_signed_inodes(files)
    run(["codesign", "--verify", "--deep", "--strict", app], capture=True)
    print("[5/6] 在精简环境中测试打包后的程序…", flush=True)
    clean_env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "QT_", "BITSCOPE_"))}
    clean_env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    run([app / "Contents/MacOS/BitScope", "--version"], env=clean_env)
    run([bundled_ffmpeg, "-version"], capture=True, env=clean_env)
    (output / "使用说明.txt").write_text(
        f"BitScope macOS 独立运行版\n\n架构：{args.arch}\n最低 macOS：{minimum}\n"
        "将 BitScope.app 拷到应用程序文件夹后双击，无需安装 Qt、Homebrew、FFmpeg 或 Python。\n"
        "已包含 FFmpeg、HEVC CU 分析器和 AV1 inspection 分析器。\n"
        "arm64 版本面向 Apple Silicon；x86_64 版本面向 Intel，不是通用二进制。\n\n"
        f"签名：{manifest['signing']}；未进行 Apple 公证。\n"
        "从网络下载后可能被 Gatekeeper 拦截；确认来源可信后，在系统设置的隐私与安全性中允许打开。\n"
        "面向公众分发建议使用 Developer ID 签名并完成 Apple 公证，不要关闭系统安全保护。\n\n"
        "许可证、补丁和本机可找到的第三方声明保存在 app 的 Contents/Resources/licenses。\n"
        "自动收集不代表已完成发行许可审查；公开或商业分发前需核对 Qt、FFmpeg 构建配置及依赖许可，"
        "并补齐相应源码/许可材料。build-manifest.json 记录了构建机器上的依赖来源，仅供审计。\n"
        "已做本机依赖、架构和签名检查；仍建议在对应系统版本的干净 Mac 上做真实播放和块分析测试。\n",
        encoding="utf-8")
    print("[6/6] 生成便于传输、保留应用结构的 ZIP…", flush=True)
    # Archive only user-facing deliverables, not paths/logs from the build host.
    run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, output / "BitScope.zip"])
    incomplete.unlink()
    github_outputs({"package-dir": output, "zip-path": output / "BitScope.zip",
                    "manifest-path": output / "build-manifest.json",
                    "instructions-path": output / "使用说明.txt"})
    print(f"\n打包完成：{output}\n架构：{args.arch}；最低 macOS：{minimum}\n"
          "可复制 BitScope.app，或发送 BitScope.zip（未公证）。", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"\n打包失败：{error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
