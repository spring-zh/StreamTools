# BitScope · H.264 / H.265 / MPEG-4 / AV1 码流分析器

一个本地运行的 MP4、H.264 / AVC、H.265 / HEVC、MPEG-4 Part 2 Visual 与 AV1 码流分析工具，界面参考专业码流分析器的多窗格工作方式。现在提供跨平台 Qt 6 桌面版；原来的浏览器版仍然保留。

## Qt 桌面版（推荐）

需要 Qt 6.5 或更高版本、CMake 3.21 或更高版本，以及用于生成帧预览的 FFmpeg。

### macOS 一键编译、独立打包

在项目根目录执行（也可在 Finder 双击 `package-macos.command`）：

```bash
./package-macos.command
```

打包电脑需要 Xcode Command Line Tools、CMake、Python 3.9+、Qt 6.5+（含 `macdeployqt`）、FFmpeg，
以及带本项目补丁的 HEVC `HM_180`、AV1 `inspect` 和 H.264 运动矢量提取器 `bitscope-avc-motion`。目标电脑不需要这些开发环境。
脚本会优先读取显式参数/环境变量，其次从已有的 `build-qt`、`build` 和专用打包构建目录查找分析器；
最后查询 PATH。全新源码克隆不包含分析器二进制，需要先按下方说明编译分析器。

需要指定依赖时：

```bash
./package-macos.command \
  --qt-prefix /opt/homebrew/opt/qt \
  --ffmpeg /opt/homebrew/bin/ffmpeg \
  --hevc-analyzer /path/to/HM_180 \
  --av1-inspect /path/to/inspect \
  --avc-motion /path/to/bitscope-avc-motion
```

也支持 `BITSCOPE_FFMPEG`、`BITSCOPE_HEVC_ANALYZER`、`BITSCOPE_AV1_INSPECT`、`BITSCOPE_AVC_MOTION` 环境变量。
使用 `--help` 查看全部参数。

每次生成一个新的 `dist/BitScope-macOS-架构-时间-随机后缀/`，不覆盖旧包：

- `BitScope.app`：独立运行应用，包含 Qt、FFmpeg 及其动态库、详细块分析器和 H.264 运动矢量提取器。
- `BitScope.zip`：可传输的应用压缩包，保留应用结构和符号链接。
- `使用说明.txt`：适用架构、最低 macOS 版本、签名和使用说明。
- `build-manifest.json`、`deployment.log`：依赖审计和部署日志，包含构建机路径，不必发给最终用户。
- 如仍有 `INCOMPLETE.txt`，说明打包失败或尚未结束，**不要分发**。

脚本在 `build-macos-package-arm64` 或 `build-macos-package-x86_64` 单独构建，
递归部署依赖，将加载路径改为应用内相对路径，检查所有 Mach-O 文件的架构和最低系统版本，
执行签名校验，并在不含 Homebrew 的 PATH 下进行启动检查。
最低系统要求取主程序、Qt、FFmpeg 和所有动态库的最大值，不能只降低主程序编译选项来支持旧系统。

默认只生成当前架构版本：Apple Silicon 对应 `arm64`，Intel 对应 `x86_64`，不是通用二进制。
要构建另一架构，使用 `--arch x86_64` 或 `--arch arm64`，同时提供对应架构的 Qt 和全部辅助程序/动态库；
建议在相应架构的 Mac 上分别构建和测试。

默认采用 ad-hoc 签名，**不等于 Apple 开发者签名或公证**，网络下载后仍可能被 Gatekeeper 拦截。
只对确认来源可信的应用，通过“系统设置 → 隐私与安全性”允许打开；不要关闭系统安全保护。
有 Developer ID Application 证书时可运行：

```bash
./package-macos.command --sign-identity 'Developer ID Application: Your Name (TEAMID)'
```

此参数启用 hardened runtime 和时间戳，但不会自动上传应用公证。
面向公众分发应另行完成 Apple 公证、staple，再重新生成 ZIP。
参考 [Qt macOS 部署文档](https://doc.qt.io/qt-6/macos-deployment.html) 和
[Apple 公证文档](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)。

脚本收集项目自带的第三方声明/补丁和本机依赖包可找到的许可证、安装清单；这并非完整的发行许可审查。
公开或商业发行前，需按实际 Qt/FFmpeg 构建配置及依赖许可证补齐相关许可/源码材料。
发布前还应在没有 Qt/Homebrew 的干净 Mac 上，实际验证各格式预览和块分析。

#### GitHub Actions 手动编译 Mac 版本

已提供 `.github/workflows/macos-build.yml`，**仅手动触发**，推送代码或标签不会自动构建。

1. 将工作流及 `scripts`、`tests`、`third_party` 提交并推送；首次使用需将工作流合入默认分支。
2. 打开 **Actions → macOS build and package → Run workflow**，选择分支及 `architecture`：
   - `arm64`（默认）：Apple Silicon / M 系列，使用 `macos-15` runner。
   - `x86_64`：Intel Mac，使用 `macos-15-intel` runner。
   - `both`：分别构建上述两个版本，不是合并的 Universal App。
3. 成功后在运行页面的 **Artifacts** 下载 `BitScope-macOS-架构-运行编号`。
4. 解压下载文件，再解压内部的 `BitScope.zip`，将 `BitScope.app` 拷入应用程序文件夹。

不需要配置额外 Secret。工作流安装官方 Qt 6.8.3 macOS 二进制，自动从固定提交构建
带补丁的 HEVC / AV1 分析器，从 SHA-256 校验过的 FFmpeg 8.0.1 源码构建预览解码器，
再调用现有 `package-macos.command`。不依赖 Homebrew 的 Qt/FFmpeg 运行库或 Intel 预编译包。
缺少 CMake、pkg-config、NASM 时才通过 Homebrew 安装构建工具。
`homebrew-versions.txt`、`dependencies.json` 和 `build-manifest.json` 记录构建环境、源码提交、补丁哈希及最低系统要求。
成品包含运行依赖，使用者不需要安装 Homebrew、Qt 或 FFmpeg。
CI 版 FFmpeg 仅包含本工具所需的四种视频解码、MP4/裸流输入、缩放和 JPEG 输出，不是通用转码工具。

测试包括：脚本保护检查、界面控件回归、宏块日志解析、ZIP 解压及签名校验、
打包主程序解析四种合成码流、四种编码的预览，以及 H.264/MPEG-4 宏块、HEVC CU、AV1 Block 提取。
`tests/fixtures/codec-smoke.json` 保存可复现的合成测试视频及生成参数和校验值，不包含用户视频，
因此 CI 不需要额外的视频编码器。
测试从带空格的新路径执行，并移除 PATH 中的 Homebrew；测试驱动自身仍链接构建机 Qt，
不能代替真实干净 Mac 上的验收。测试失败只上传诊断，不上传成功包；成品默认保存 14 天。

默认 ad-hoc 签名，**未做 Apple 公证**，首次运行可能需要在“系统设置 → 隐私与安全性”允许打开。
最低 macOS 版本以包内 `使用说明.txt` 为准，不保证支持构建环境之前的所有系统。
工作流不发布 GitHub Release，也不修改仓库。私有仓库运行可能消耗 Actions 配额。

本地全新克隆如需编译分析器，可安装依赖后执行：

```bash
brew install cmake qtbase pkgconf nasm
python3 scripts/build_macos_deps.py --arch arm64 --jobs 3
```

Intel Mac 将 `arm64` 改为 `x86_64`。完成后根据输出的 `dependencies.json` 中路径，
将 `hevc_analyzer`、`av1_inspect`、`ffmpeg`、`avc_motion` 分别传给打包脚本的
`--hevc-analyzer`、`--av1-inspect`、`--ffmpeg`、`--avc-motion`，并通过 `--dependency-notices` 传入该构建目录的 `licenses`。

### macOS / Linux

四种编码都支持悬停气泡，鼠标移开即隐藏；单击不锁定块，放大拖拽用于平移。
H.264 / MPEG-4 显示宏块，H.265 显示真实 CU / 所属 CTU，AV1 显示 Coding Block / 所属 Superblock。
尺寸、QP / QIndex、预测模式、变换和运动信息按各解码器实际导出能力显示；缺失数据不填零。
顶部编码卡显示熵编码类型；H.264 根据各 Slice 的活动 PPS 区分 CABAC / CAVLC / 混合，
MPEG-4 的 VLC 类别暂不细分 RVLC。原“学习 / 对比”标签已移除，当前帧统计保留。
帧表的类型与结构合并显示为 `P（完整帧）`、`INTER（Frame OBU）` 等；POC / Order Hint 位于 PTS 后，偏移列位于大小列后。
H.265 POC 从活动 SPS/PPS 和 Slice 头按解码顺序推导，处理 LSB 回绕、TemporalId、IDR/BLA/CRA、负 POC 和依赖分片；语法树保留原始 LSB 和推导值。
中途截取且缺少随机访问上下文时，POC 提示为片段基准；缺失头信息、分片 POC 冲突或不支持的多层 HEVC 不伪造数值。
运动矢量的位移表示当前块到参考采样位置，不是物体运动轨迹或编码器的搜索过程。
H.264 显示 L0 / L1；MPEG-4 显示前向 / 后向参考；H.265 按 PU 显示 L0 / L1、参考 POC 和 1/4 像素位移；
AV1 按 MV0 / MV1 显示参考槽名称与 1/8 像素换算（全局 / 仿射运动另有提示）。
H.265 直接复用已有 CU 分析的 `decoder_mv.txt`，不增加一次解码；其中 POC 属于分析片段，不等同于界面帧编号。
MV 按需异步提取，帧间重排使用帧标识绑定，最近四帧结果缓存；不随缩略图批量生成。
H.264 / MPEG-4 当前导出接口不提供精确参考索引 / MVP / MVD，8×8 内更小子分区未细分；场编码和非零左上裁剪暂不提供 MV，以免坐标误导。

本地构建运动提取器（需要 C/C++ 编译器、make、curl、patch、pkg-config；工作目录必须尚不存在）：

```bash
bash scripts/build-motion-helper.sh "$PWD/build-motion-local" 4
cmake -S . -B build -DBITSCOPE_AVC_MOTION_EXECUTABLE="$PWD/build-motion-local/bitscope-avc-motion"
```

脚本使用校验过的 FFmpeg 8.0.1 源码，补齐 MPEG-4 B-VOP 的真实运动表导出，静态链接最小解码库。
直接用系统 FFmpeg 开发库编译的旧提取器不能保证 MPEG-4 B 帧矢量正确，新版未打补丁的构建会明确拒绝这类帧，避免把未填充数据显示成零运动。
完整 CI 依赖脚本会自动构建带补丁的提取器。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix dist
```

若要启用 H.265 真实 CTU/CU 四叉树，配置时指定修改后的 Gitl/HM-18 解析器：

```bash
git -C /path/to/GitlHEVCAnalyzer apply --ignore-space-change \
  /path/to/BitScope/third_party/hevc-cu-analyzer/bitscope-output-frames.patch
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBITSCOPE_HEVC_ANALYZER=/path/to/HM_180
```

若要启用 AV1 真实 Coding Block、块级 QIndex、预测/变换模式、参考帧和运动矢量，先将官方 libaom 以 inspection 模式构建，再把 `inspect` 帮助程序打包进应用：

```bash
git -C /path/to/aom apply \
  /path/to/BitScope/third_party/aom-inspection/inspect-output-start.patch
cmake -S /path/to/aom -B /path/to/aom-build -DCMAKE_BUILD_TYPE=Release \
  -DCONFIG_INSPECTION=1 -DENABLE_EXAMPLES=1 -DENABLE_TESTS=0
cmake --build /path/to/aom-build --target inspect --parallel

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBITSCOPE_AV1_INSPECT_EXECUTABLE=/path/to/aom-build/examples/inspect
```

运行时也可以通过 `BITSCOPE_AV1_INSPECT` 指向该帮助程序。Windows 下对应文件会在应用内命名为 `av1inspect.exe`。

macOS 可运行 `build/BitScope.app`，Linux 通常运行 `build/BitScope`。也可以把裸码流路径作为启动参数：

```bash
./build/BitScope /path/to/input.h264
# 或 MP4 / MOV 容器
./build/BitScope /path/to/input.mp4
# MPEG-4 Part 2 裸码流
./build/BitScope /path/to/input.m4v
# AV1 low-overhead OBU 或 IVF
./build/BitScope /path/to/input.obu
```

### Windows

#### GitHub Actions 自动编译、打包（Mac 上也能操作）

已提供 `.github/workflows/windows-build.yml`，使用 GitHub 的 `windows-2022` x64 环境，
安装 Qt 6.8.3 MSVC x64 和 NASM，并调用与本地相同的 `package-windows.ps1`。
无需在 Mac 安装 Windows，也不需要配置额外的仓库 Secret 或个人访问令牌。

1. 将本项目源码（包括 `.github`、`scripts`、`third_party`、`tests`）提交并推送到自己的 GitHub 仓库。
   不要上传 `build*`、`dist` 或测试视频。
2. 打开仓库的 **Actions → Windows x64 build and package → Run workflow**。
   首次使用时，工作流文件要先合入默认分支，网页才会显示手动运行按钮。
3. 选择分支并运行。成功后打开该次运行页面，在 **Artifacts** 下载 `BitScope-windows-x64-运行编号`。
4. 解压下载的 artifact，再解压其中的 `BitScope-windows-x64.zip`，保留完整 `BitScope` 文件夹。
5. 在 Windows 10/11 x64 上运行 `BitScope.exe`。干净电脑可能需要先运行
   `prerequisites/VC_redist.x64.exe` 安装微软 C++ x64 运行库。

工作流仅手动触发，代码变更和标签推送不会自动运行。
只上传构建产物，不创建 GitHub Release、不推送代码；权限仅 `contents: read`。
仓库或组织如限制 Actions，需要允许工作流引用的 `actions/*` 和 `jurplel/install-qt-action`。
Actions 引用固定提交 SHA，两个分析器源码固定提交，FFmpeg 固定版本并校验 SHA-256。
测试失败不会上传“成功包”，仅保留构建诊断；默认 artifact 保存 14 天。

自动检查包括：PowerShell 语法/参数与 x64 格式检查、主程序启动、四种编码的合成视频预览、
H.265 CU 和 AV1 Coding Block 提取、界面截图。测试文件在构建时生成，不使用个人视频。
运行检查把程序复制到新路径，并从 PATH 移除 Qt/编译工具；这不能代替真实干净 Windows 电脑的验收。

#### Windows 本地一键打包

安装 Visual Studio 2022（C++ 桌面开发和 Windows SDK）、Qt 的 `msvc2022_64` 组件、
CMake 3.21+、Git、PowerShell 7 和 NASM。确保 `cmake`、`git`、`nasm` 在 PATH 中。
运动提取器还需要 MSYS2 UCRT64，安装 `make patch tar curl nasm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf`。
默认使用 `C:\msys64`，其他位置通过 `BITSCOPE_MSYS2_ROOT` 指定；主程序仍使用 MSVC，二者通过独立进程通信。
在 **PowerShell 7** 中执行（替换 Qt 路径）：

```powershell
pwsh -NoProfile -File .\package-windows.ps1 -QtPrefix C:\Qt\6.8.3\msvc2022_64
```

脚本默认自动下载/构建缺失的视频分析工具：

- `scripts/build-windows-deps.ps1`：获取固定提交的 Gitl/HM、libaom，应用项目补丁并编译，
  下载校验过的 FFmpeg 8.0.1 essentials Windows x64 版本，并独立编译带补丁的 H.264 / MPEG-4 运动矢量提取器。
- `scripts/hevc-helper/CMakeLists.txt`：只构建 HEVC 参考解码/分析器，不构建上游 Qt GUI。
- `package-windows.ps1`：编译主程序，通过 `windeployqt` 部署 Qt，复制辅助工具和声明，
  下载并验证微软签名的 VC++ 运行库安装器，测试后输出文件夹和 ZIP。
- `scripts/test-windows-package.ps1`：运行实际视频/块解析测试。

每次输出到新的 `dist/BitScope-windows-x64-时间-随机后缀/`，不覆盖旧包。
里面的 `BitScope/` 是完整应用目录，`BitScope-windows-x64.zip` 是分发包，
`build-manifest.json` 记录源提交、文件哈希和测试状态。
如仍有 `INCOMPLETE.txt`，说明打包中断或失败，不能当作成品分发。

已有 Windows x64 分析工具时，可以全部显式指定，跳过下载/构建分析器：

```powershell
pwsh -File .\package-windows.ps1 `
  -QtPrefix C:\Qt\6.8.3\msvc2022_64 `
  -FFmpeg C:\tools\ffmpeg\bin\ffmpeg.exe `
  -HevcAnalyzer C:\tools\HM_180.exe `
  -Av1Inspect C:\tools\av1inspect.exe `
  -AvcMotion C:\tools\bitscope-avc-motion.exe `
  -VcRedist C:\tools\VC_redist.x64.exe
```

分析器必须包含本项目补丁；脚本不接受 macOS、ARM64 或 x86 二进制。
显式提供的共享库构建会复制工具同目录 DLL，遇到同名不同内容的 DLL 会报错。
主程序和 Qt 库仍需 MSVC 2022 x64 兼容，不能混用 MinGW 库。
`-SkipTests` 仅用于排查，生成清单会标记未测试；Actions 不使用此选项。

首次准备依赖需要访问 GitHub、Qt 下载站、AOM、HM 上游和微软下载站。
依赖构建保留在 `build-windows-deps`，不会改动用户已有源码目录。
程序默认未做 Windows 代码签名；公开分发前请按实际依赖的许可证补齐相关许可与源码材料，
并在 Windows 实机上验证。本项目随附的 FFmpeg 包是第三方 GPL 构建。

参考：[Qt Windows 部署](https://doc.qt.io/qt-6/windows-deployment.html)、
[GitHub 手动运行工作流](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)、
[FFmpeg Windows 构建来源](https://www.gyan.dev/ffmpeg/builds/)。

预览功能会按顺序查找：`BITSCOPE_FFMPEG` 环境变量、程序同目录的 `ffmpeg` / `ffmpeg.exe`、macOS Homebrew/MacPorts 常用目录、Linux 常用目录以及系统 `PATH`。因此从 Finder 双击启动也能识别 `/opt/homebrew/bin/ffmpeg`。没有 FFmpeg 时，码流和语法分析仍可正常使用，画面区域会显示明确的安装提示。

当前帧预览失败时，底部状态栏会持续显示红色错误原因，不会被宏块加载完成等后台消息覆盖。点击右侧“错误详情”可查看并复制帧号、解码范围、FFmpeg 路径与参数、图片路径、关键错误和末尾日志（为避免宏块调试日志过大，保留最后 8 条关键日志及最后 12 行，每行最多 1000 字符）。重新选中失败帧可重试；成功生成预览后错误自动清除。

MPEG-4 开放 GOP 预览保留最近 I-VOP 的按需解码，只在请求跨 GOP 的前导 B-VOP 时回退到前一个 I-VOP。解码起点处因缺少旧参考而被跳过的 B-VOP 不参与图片和宏块的输出编号，避免尾帧“加载失败”或后续帧错位；若实际输出数量仍与预期不符，则报告错误而不是绑定错误画面。

对于帧内刷新码流，Recovery Point 不会被当成 IDR 使用。预览器会结合 `recovery_frame_cnt` 从上一个已经完成恢复的点连续解码，避免每逢普通 I Slice / Recovery Point 就丢失历史参考而出现绿屏。文件开头若本身缺少前置参考帧，则恢复计数完成前的画面仍无法无损重建。

### 快速验证解析器

```bash
./build/BitScope --analyze /path/to/input.h264
```

该命令不打开主界面，会输出编码、尺寸、帧数、码流单元数量和首帧结构的 JSON 摘要。

## 浏览器版（保留）

直接双击 `index.html`，或在本目录运行：

```bash
python3 -m http.server 8765
```

然后访问 `http://127.0.0.1:8765`。点击“打开码流”，也可以把文件直接拖到页面中。文件只在本机浏览器内解析，不会上传。

建议使用上面的本地服务方式打开。常规 IDR 码流可直接使用浏览器硬件解码生成预览；像测试文件这样只有 Recovery Point、没有 IDR 的码流会自动按需加载本地软件解码组件。浏览器安全策略不允许 `file://` 页面启动该组件。

## Qt 桌面版功能

- 自动识别 MP4 / MOV 中的 `avc1` / `hvc1` / `hev1` / `mp4v` / `av01`，以及 H.264、H.265、MPEG-4 Part 2 `.m4v`、AV1 `.obu/.av1` low-overhead OBU 与 `.ivf`
- 码流单元列表：H.264/H.265 NAL、MPEG-4 Visual 起始码或 AV1 OBU 类型、偏移、大小与时间层
- 按帧聚合：I / P / B / S 或 AV1 KEY / INTER / INTRA_ONLY / SWITCH / SHOW_EXISTING，PTS、POC / Order Hint、大小与帧头数量
- 后台解析大文件，主界面保持响应
- 帧列表按可见区域分批调用本机 FFmpeg 生成缩略图；当前选中帧优先于其他缩略图，从最近的随机访问点 / Recovery Point 解码并缓存
- 缩略图使用 180 帧上限的 LRU 缓存，长时间滚动大码流时不会无限增加内存占用
- 每帧画面预览以及 H.264 宏块、H.265 CTU/CU、AV1 64×64 或 128×128 Superblock 与实际 Coding Block 边界、地址、当前可见尺寸和像素坐标；边缘不完整单元按原图像素比例绘制，支持工具栏或滚轮缩放、放大后拖动和双击恢复适合窗口
- H.264 软件解码后的逐宏块实际 QP、预测类型码、QP 热力图，以及当前帧 QP 平均值、中位数、最小/最大值、标准差和宏块类型占比
- MPEG-4 Part 2 Visual 解析 Visual Object Sequence、VOL、GOV 和 VOP，显示 I/P/B/S-VOP、`vop_time_increment`、`vop_quant`、f-code、宏块实际 QP/预测类型、参考关系与 QP 热力图
- AV1 解析 OBU header、Sequence Header、Frame Header / Frame OBU、Tile Group 与 Metadata OBU，显示 profile/level、位深、色度采样、Superblock 尺寸、Order Hint、reference slots 和 `refresh_frame_flags`
- AV1 通过 libaom inspection 按需导出真实 Coding Block；支持 4×4 到 128×128 及 4×8、8×4、16×64、64×16 等非方形块，显示 Y/UV 预测模式、变换尺寸/类型、块级 QIndex、Segment、Skip、IntraBC、参考帧和运动矢量，并提供 QIndex 热力图与当前帧统计
- AV1 与 H.265 块分析采用“当前帧优先”：根网格和 Slice 范围立即显示；选中帧画面一生成就以正常优先级单独补充叶子块，完成后再低优先级预取可见帧；切换帧会取消旧预取。H.265 只输出请求帧的 CU 文本，AV1 只序列化目标范围的 inspection JSON
- H.265 预览区分 IDR/BLA、CRA 与前导 RASL：从 CRA 按需解码时会校正被抑制图像造成的编号偏移，并按 MP4 PTS 将显示顺序画面对回解码顺序帧；真正失败的缩略图会显示错误状态，重新选中可重试
- H.265 通过参考解码器按需导出 CTU 内部四叉树；独立的“CTU / CU”页面显示每个叶子 CU 的编码尺寸、深度、分区模式、预测类别、QP 和近似 CABAC 字节数
- MP4 / MOV 中的 H.264、H.265、MPEG-4 Part 2 和 AV1 视频轨道，支持解析采样表、长度前缀 NAL、`mp4v/esds` 与 `av01/av1C`；“文件视图”按层级显示 ISO BMFF Box、偏移、大小、关键字段和 Hex
- 独立区分 IDR、普通 I、I/P 混合 Slice，以及完整帧、多 Slice 分片帧和起始分片缺失
- 每帧 Slice 清单及 Slice Header 字段级解析
- H.264/H.265 SEI 类型识别；Recovery Point 展开为 `recovery_frame_cnt`、`exact_match_flag`、`broken_link_flag` 等字段
- 逐帧参考关系、文件外缺失参考和反向被引用帧
- 时间线参考箭头、Recovery Point 标记和选中帧依赖摘要
- H.264 SPS / PPS、VUI、Slice、POC 和 QP 基础信息
- H.265 SPS / PPS、CTB、Slice Segment、Slice Type、POC 字段
- RBSP 防竞争字节移除、Exp-Golomb `ue(v)` / `se(v)` 解码
- 语法树、十六进制、摘要、筛选、搜索、时间线与键盘快捷键

## 快捷键

- `Ctrl / ⌘ + O`：打开码流
- `↑ / ↓`：选择上一帧或下一帧
- `Ctrl / ⌘ + 1`：帧视图
- `Ctrl / ⌘ + 2`：码流单元视图
- `Ctrl / ⌘ + 3`：MP4 文件 / Box 视图（仅容器文件）
- `/`：聚焦搜索框

> Qt 桌面版支持 MP4 / MOV 中的 H.264、H.265、MPEG-4 Part 2 `mp4v` 和 AV1 `av01` 视频轨道；浏览器版目前仍面向 H.264/H.265 裸视频流。MKV、TS、WebM 等其他容器暂未直接支持。

> AV1 没有 H.264 的固定宏块概念。工具把 libaom inspection 导出的 4×4 MI 网格按 `blockSize` 合并为真实 Coding Block，因此画面中的方形/非方形块边界和 QIndex 均来自熵解码结果，不是根据画面估算。未打包 `av1inspect` 时仍会显示 Sequence/Frame Header、参考槽和 Superblock 根网格，并明确提示块级数据不可用。

> H.265 没有 H.264 的固定宏块概念。工具从 SPS 确定最小 CU 到最大 CTU 的合法尺寸，再由随应用打包的 HM-18 解析器实际解码 CABAC 语法，因此画面中的叶子 CU 边界不是根据画面或码率估算的。未打包该帮助程序时，仍可正常查看 CTU 根网格和其他码流信息。

## 第三方组件与隐私

Qt 桌面版只在本机读取和分析文件，帧预览通过本机 FFmpeg 子进程生成，不会上传码流。H.265 CU 树使用修改后的 [Gitl HEVC Analyzer](https://github.com/lheric/GitlHEVCAnalyzer) / HM-18 参考解码器，修改说明和完整许可见 `third_party/hevc-cu-analyzer/NOTICE.md`。AV1 块数据使用官方 [libaom](https://aomedia.googlesource.com/aom/) 的 inspection 示例，许可见 `third_party/aom-inspection/`。浏览器版的无 IDR 预览回退使用本地打包的 `@ffmpeg/ffmpeg 0.12.15` 与 `@ffmpeg/core 0.12.10`，许可与来源见 `vendor/ffmpeg/NOTICE.md`。
