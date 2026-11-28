#Requires -Version 7.0
[CmdletBinding()]
param([string]$WorkRoot = (Join-Path $PSScriptRoot '../build-windows-deps'),
      [ValidateRange(1, 64)][int]$Jobs = 4)
. "$PSScriptRoot/windows-common.ps1"
if (-not $IsWindows) { throw 'Run this script on Windows (or a Windows GitHub Actions runner).' }
$root = (Resolve-Path "$PSScriptRoot/..").Path
foreach ($tool in @('git', 'cmake')) { Get-Command $tool -ErrorAction Stop | Out-Null }
$msysRoot = if ($env:BITSCOPE_MSYS2_ROOT) { $env:BITSCOPE_MSYS2_ROOT } else { 'C:\msys64' }
$motionBash = Require-File "$msysRoot/usr/bin/bash.exe"
if (-not (Get-Command nasm -ErrorAction SilentlyContinue)) {
    $nasmDirectory = Join-Path $env:ProgramFiles 'NASM'
    if (Test-Path "$nasmDirectory/nasm.exe") { $env:PATH = "$nasmDirectory;$env:PATH" }
    else { throw 'Install NASM and add it to PATH (CI installs it automatically).' }
}
$nasmExe = (Get-Command nasm -CommandType Application -ErrorAction Stop).Source
Write-Host "NASM executable: $nasmExe"
Invoke-Native $nasmExe @('-v')
$work = New-BuildDirectory $WorkRoot 'deps-x64'
$gitlCommit = 'f3997d7a76779921a7ae15b02ef382440c8fb376'
$aomCommit = 'd772e334cc724105040382a977ebb10dfd393293' # libaom v3.13.1
$ffmpegUrl = 'https://github.com/GyanD/codexffmpeg/releases/download/8.0.1/ffmpeg-8.0.1-essentials_build.zip'
$ffmpegHash = 'e2aaeaa0fdbc397d4794828086424d4aaa2102cef1fb6874f6ffd29c0b88b673'

function Get-PinnedSource {
    param([string]$Directory, [string]$Url, [string]$Commit)
    Invoke-Native git @('init', $Directory)
    Invoke-Native git @('-C', $Directory, 'config', 'core.autocrlf', 'false')
    Invoke-Native git @('-C', $Directory, 'remote', 'add', 'origin', $Url)
    Invoke-Native git @('-C', $Directory, 'fetch', '--depth=1', 'origin', $Commit)
    Invoke-Native git @('-C', $Directory, 'checkout', '--detach', 'FETCH_HEAD')
    $actual = (& git -C $Directory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Commit) { throw "Unexpected source revision: $Directory" }
}

Write-Host '[Dependencies 1/3] Building patched HEVC analyzer...'
$gitl = Join-Path $work 'gitl'
Get-PinnedSource $gitl 'https://github.com/lheric/GitlHEVCAnalyzer.git' $gitlCommit
Invoke-Native git @('-C', $gitl, 'submodule', 'update', '--init', '--depth=1', '--', 'appgitlhevcdecoder/HM_18.0/HM')
# Verify the patch against pristine sources with both common line endings.
& "$root/tests/hevc-patch.Tests.ps1" -GitlSource $gitl
Invoke-Native git @('-C', $gitl, 'apply', '--check', '--ignore-space-change', "$root/third_party/hevc-cu-analyzer/bitscope-output-frames.patch")
Invoke-Native git @('-C', $gitl, 'apply', '--ignore-space-change', "$root/third_party/hevc-cu-analyzer/bitscope-output-frames.patch")
$hm = Join-Path $gitl 'appgitlhevcdecoder/HM_18.0/HM'
Invoke-Native git @('-C', $hm, 'apply', '--ignore-space-change', "$gitl/appgitlhevcdecoder/HM_18.0/sysu-analyzer.patch")
$hmCommit = (& git -C $hm rev-parse HEAD).Trim()
$hevcBuild = Join-Path $work 'hevc-build'
Invoke-Native cmake @('-S', "$PSScriptRoot/hevc-helper", '-B', $hevcBuild,
    '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DGITL_SOURCE=$gitl")
Invoke-Native cmake @('--build', $hevcBuild, '--config', 'Release', '--parallel', "$Jobs")
$hevcExe = Require-File "$hevcBuild/Release/HM_180.exe"

Write-Host '[Dependencies 2/3] Building libaom inspection...'
$aom = Join-Path $work 'aom'
Get-PinnedSource $aom 'https://aomedia.googlesource.com/aom' $aomCommit
Invoke-Native git @('-C', $aom, 'apply', "$root/third_party/aom-inspection/inspect-output-start.patch")
Invoke-Native git @('-C', $aom, 'apply', '--check', "$root/third_party/aom-inspection/nasm-help-compat.patch")
Invoke-Native git @('-C', $aom, 'apply', "$root/third_party/aom-inspection/nasm-help-compat.patch")
Invoke-Native cmake @("-DAOM_ROOT=$aom", "-DCMAKE_ASM_NASM_COMPILER=$nasmExe",
    "-DOUTPUT_DIR=$work/nasm-smoke", '-P', "$root/tests/aom-nasm-smoke.cmake")
$aomBuild = Join-Path $work 'aom-build'
Invoke-Native cmake @('-S', $aom, '-B', $aomBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', '-DAOM_TARGET_CPU=x86_64',
    '-DENABLE_NASM=ON', "-DCMAKE_ASM_NASM_COMPILER=$nasmExe", '-DCONFIG_INSPECTION=1', '-DCONFIG_AV1_ENCODER=0',
    '-DENABLE_EXAMPLES=1', '-DENABLE_TESTS=0', '-DENABLE_DOCS=0', '-DENABLE_TOOLS=0', '-DBUILD_SHARED_LIBS=OFF')
Invoke-Native cmake @('--build', $aomBuild, '--config', 'Release', '--target', 'inspect', '--parallel', "$Jobs")
$inspectCandidates = @(Get-ChildItem $aomBuild -Filter inspect.exe -Recurse -File)
if ($inspectCandidates.Count -ne 1) { throw 'Could not uniquely locate the built inspect.exe.' }
$av1Exe = $inspectCandidates[0].FullName

Write-Host '[Dependencies 3/3] Downloading checksum-pinned FFmpeg...'
$archive = Join-Path $work 'ffmpeg.zip'
Get-CheckedDownload $ffmpegUrl $archive $ffmpegHash | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath "$work/ffmpeg"
$ffmpegCandidates = @(Get-ChildItem "$work/ffmpeg" -Filter ffmpeg.exe -Recurse -File)
if ($ffmpegCandidates.Count -ne 1) { throw 'Could not uniquely locate ffmpeg.exe.' }
$ffmpegExe = $ffmpegCandidates[0].FullName
$motionBuild = Join-Path $work 'motion-patched'
$previousMsystem = $env:MSYSTEM
try {
    $env:MSYSTEM = 'UCRT64'
    Invoke-Native $motionBash @('-lc', 'exec bash "$(cygpath -u "$1")" "$(cygpath -u "$2")" "$3"',
        '--', "$PSScriptRoot/build-motion-helper.sh", $motionBuild, "$Jobs")
} finally { $env:MSYSTEM = $previousMsystem }
$motionExe = Require-File "$motionBuild/bitscope-avc-motion.exe"
foreach ($binary in @($hevcExe, $av1Exe, $ffmpegExe, $motionExe)) { Assert-X64PE $binary }
$manifest = Join-Path $work 'dependencies.json'
@{ gitl_commit = $gitlCommit; hm_commit = $hmCommit; aom_commit = $aomCommit;
   ffmpeg_url = $ffmpegUrl; ffmpeg_sha256 = $ffmpegHash;
   hevc_patch_sha256 = (Get-FileHash "$root/third_party/hevc-cu-analyzer/bitscope-output-frames.patch").Hash;
   av1_patch_sha256 = (Get-FileHash "$root/third_party/aom-inspection/inspect-output-start.patch").Hash
   nasm_compat_patch_sha256 = (Get-FileHash "$root/third_party/aom-inspection/nasm-help-compat.patch").Hash
   motion_patch_sha256 = (Get-FileHash "$root/third_party/ffmpeg-motion/mpeg4-b-mvs.patch").Hash
} | ConvertTo-Json | Set-Content -Encoding utf8 $manifest
# Native build output goes to the host, leaving exactly one object in the pipeline.
[pscustomobject]@{ FFmpeg = $ffmpegExe; HevcAnalyzer = $hevcExe; Av1Inspect = $av1Exe;
    AvcMotion = $motionExe;
    MotionNotices = "$motionBuild/licenses";
    FFmpegRoot = $ffmpegCandidates[0].Directory.Parent.FullName; Manifest = $manifest }
