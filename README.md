# BitScope · H.264 / H.265 / MPEG-4 / AV1 码流分析器

一个本地运行的 MP4、H.264 / AVC、H.265 / HEVC、MPEG-4 Part 2 Visual 与 AV1 码流分析工具，界面参考专业码流分析器的多窗格工作方式。现在提供跨平台 Qt 6 桌面版；原来的浏览器版仍然保留。

## Qt 桌面版（推荐）

需要 Qt 6.5 或更高版本、CMake 3.21 或更高版本，以及用于生成帧预览的 FFmpeg。

### macOS / Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix dist
```

若要启用 H.265 真实 CTU/CU 四叉树，配置时指定修改后的 Gitl/HM-18 解析器：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBITSCOPE_HEVC_ANALYZER=/path/to/HM_180
```

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

在安装 Qt 6 的开发者命令行中执行：

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```

预览功能会按顺序查找：`BITSCOPE_FFMPEG` 环境变量、程序同目录的 `ffmpeg` / `ffmpeg.exe`、macOS Homebrew/MacPorts 常用目录、Linux 常用目录以及系统 `PATH`。因此从 Finder 双击启动也能识别 `/opt/homebrew/bin/ffmpeg`。没有 FFmpeg 时，码流和语法分析仍可正常使用，画面区域会显示明确的安装提示。

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
- 帧列表按可见区域分批调用本机 FFmpeg 生成缩略图；从最近的随机访问点 / Recovery Point 解码并缓存
- 缩略图使用 180 帧上限的 LRU 缓存，长时间滚动大码流时不会无限增加内存占用
- 每帧画面预览以及 H.264 宏块、H.265 CTU/CU、AV1 64×64 或 128×128 Superblock 根网格、地址、当前可见尺寸和像素坐标；边缘不完整单元按原图像素比例绘制，支持工具栏或滚轮缩放、放大后拖动和双击恢复适合窗口
- H.264 软件解码后的逐宏块实际 QP、预测类型码、QP 热力图，以及当前帧 QP 平均值、中位数、最小/最大值、标准差和宏块类型占比
- MPEG-4 Part 2 Visual 解析 Visual Object Sequence、VOL、GOV 和 VOP，显示 I/P/B/S-VOP、`vop_time_increment`、`vop_quant`、f-code、宏块实际 QP/预测类型、参考关系与 QP 热力图
- AV1 解析 OBU header、Sequence Header、Frame Header / Frame OBU、Tile Group 与 Metadata OBU，显示 profile/level、位深、色度采样、Superblock 尺寸、Order Hint、reference slots 和 `refresh_frame_flags`
- H.265 通过参考解码器按需导出 CTU 内部四叉树；独立的“CTU / CU”页面显示每个叶子 CU 的编码尺寸、深度、分区模式、预测类别、QP 和近似 CABAC 字节数；大分辨率码流在展开 CTU 时才加载明细
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

> AV1 当前显示 Sequence/Frame Header、参考槽与 Superblock 根网格。AV1 内部编码块的递归分区和块级 QIndex 位于熵编码数据中，未使用解码器导出时不会伪造为“实际块信息”。

> H.265 没有 H.264 的固定宏块概念。工具从 SPS 确定最小 CU 到最大 CTU 的合法尺寸，再由随应用打包的 HM-18 解析器实际解码 CABAC 语法，因此画面中的叶子 CU 边界不是根据画面或码率估算的。未打包该帮助程序时，仍可正常查看 CTU 根网格和其他码流信息。

## 第三方组件与隐私

Qt 桌面版只在本机读取和分析文件，帧预览通过本机 FFmpeg 子进程生成，不会上传码流。H.265 CU 树使用修改后的 [Gitl HEVC Analyzer](https://github.com/lheric/GitlHEVCAnalyzer) / HM-18 参考解码器，修改说明和完整许可见 `third_party/hevc-cu-analyzer/NOTICE.md`。浏览器版的无 IDR 预览回退使用本地打包的 `@ffmpeg/ffmpeg 0.12.15` 与 `@ffmpeg/core 0.12.10`，许可与来源见 `vendor/ffmpeg/NOTICE.md`。
