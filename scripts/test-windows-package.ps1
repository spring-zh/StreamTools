#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageDir, [Parameter(Mandatory)][string]$BuildDir)
. "$PSScriptRoot/windows-common.ps1"
if (-not $IsWindows) { throw 'Runtime smoke tests require Windows.' }
# Test from a different path, with no Qt/build tools on PATH. Keep evidence on failure.
$sandbox = New-BuildDirectory $BuildDir 'portable-test'
Copy-Item -Path "$PackageDir/*" -Destination $sandbox -Recurse
foreach ($test in @('BitScopeWindowChromeTest', 'BitScopeMacroblockLogTest', 'BitScopePreviewSmoke', 'BitScopeHevcCuSmoke', 'BitScopeAv1BlockSmoke')) {
    Copy-Item -LiteralPath (Require-File "$BuildDir/Release/$test.exe") -Destination "$sandbox/$test.exe"
}
$names = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH',
           'BITSCOPE_FFMPEG', 'BITSCOPE_HEVC_ANALYZER', 'BITSCOPE_AV1_INSPECT')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:BITSCOPE_FFMPEG = "$sandbox/ffmpeg.exe"
    $ffmpeg = "$sandbox/ffmpeg.exe"
    Invoke-Native "$sandbox/BitScope.exe" @('--version')
    Invoke-Native "$sandbox/BitScopeWindowChromeTest.exe" @()
    Invoke-Native "$sandbox/BitScopeMacroblockLogTest.exe" @()
    $samples = Join-Path $sandbox 'samples'
    New-Item -ItemType Directory -Path $samples | Out-Null
    # Tiny deterministic synthetic samples: no private user videos in CI.
    foreach ($spec in @(@('h264', 'libx264'), @('hevc', 'libx265'), @('mpeg4', 'mpeg4'), @('av1', 'libaom-av1'))) {
        $sample = Join-Path $samples ($spec[0] + '.mp4')
        $arguments = @('-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=128x96:rate=6',
            '-frames:v', '6', '-an', '-c:v', $spec[1], '-pix_fmt', 'yuv420p', '-threads', '1')
        if ($spec[0] -eq 'av1') { $arguments += @('-cpu-used', '8', '-crf', '40') }
        if ($spec[0] -eq 'hevc') { $arguments += @('-x265-params', 'pools=none:frame-threads=1:log-level=error') }
        if ($spec[0] -in @('h264', 'mpeg4')) { $arguments += @('-bf', '2') }
        Invoke-Native $ffmpeg ($arguments + @('-y', $sample))
        Invoke-Native "$sandbox/BitScope.exe" @('--analyze', $sample)
        $previewArguments = @($sample, '0', '5')
        if ($spec[0] -in @('h264', 'mpeg4')) { $previewArguments += '--verify-images' }
        Invoke-Native "$sandbox/BitScopePreviewSmoke.exe" $previewArguments
        if ($spec[0] -in @('h264', 'mpeg4')) {
            # Also validate the nonzero, single-frame request path and its QP data.
            Invoke-Native "$sandbox/BitScopePreviewSmoke.exe" @($sample, '5')
        }
    }
    Invoke-Native "$sandbox/BitScopeHevcCuSmoke.exe" @("$samples/hevc.mp4", "$sandbox/tools/HM_180.exe", '0', '5')
    Invoke-Native "$sandbox/BitScopeAv1BlockSmoke.exe" @("$samples/av1.mp4", "$sandbox/tools/av1inspect.exe")
    Invoke-Native "$sandbox/BitScope.exe" @("$samples/hevc.mp4", '--screenshot', "$sandbox/smoke-preview.png")
    Require-File "$sandbox/smoke-preview.png" | Out-Null
    Write-Host "Packaged startup, 4 codec previews, AVC/MPEG-4 macroblocks/QP, HEVC CU and AV1 block smoke tests passed: $sandbox"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
