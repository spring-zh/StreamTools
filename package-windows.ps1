#Requires -Version 7.0
<#
.SYNOPSIS
Build and package BitScope for Windows x64. Missing video tools are built/downloaded.
.EXAMPLE
pwsh -File .\package-windows.ps1 -QtPrefix C:\Qt\6.8.3\msvc2022_64
#>
[CmdletBinding()]
param(
    [string]$QtPrefix = $env:QT_ROOT_DIR,
    [string]$FFmpeg = $env:BITSCOPE_FFMPEG,
    [string]$HevcAnalyzer = $env:BITSCOPE_HEVC_ANALYZER,
    [string]$Av1Inspect = $env:BITSCOPE_AV1_INSPECT,
    [string]$VcRedist,
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build-windows-x64'),
    [string]$OutputDir = (Join-Path $PSScriptRoot 'dist'),
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [switch]$SkipTests
)
. "$PSScriptRoot/scripts/windows-common.ps1"
if (-not $IsWindows) { throw 'This script builds Windows binaries; run it on Windows or GitHub Actions.' }
Get-Command cmake -ErrorAction Stop | Out-Null
if (-not $QtPrefix) { throw 'Specify -QtPrefix C:\Qt\6.8.3\msvc2022_64 (or QT_ROOT_DIR).' }
$QtPrefix = (Resolve-Path -LiteralPath $QtPrefix).Path
$deploy = Require-File "$QtPrefix/bin/windeployqt.exe"
Assert-X64PE "$QtPrefix/bin/Qt6Core.dll"
# Also select this Qt for configure/deployment, not another kit on PATH.
$env:PATH = "$QtPrefix/bin;$env:PATH"
$dependencyInfo = $null
if (-not $FFmpeg -or -not $HevcAnalyzer -or -not $Av1Inspect) {
    $dependencyInfo = & "$PSScriptRoot/scripts/build-windows-deps.ps1" -Jobs $Jobs
    if (-not $FFmpeg) { $FFmpeg = $dependencyInfo.FFmpeg }
    if (-not $HevcAnalyzer) { $HevcAnalyzer = $dependencyInfo.HevcAnalyzer }
    if (-not $Av1Inspect) { $Av1Inspect = $dependencyInfo.Av1Inspect }
}
$FFmpeg = Require-File $FFmpeg
$HevcAnalyzer = Require-File $HevcAnalyzer
$Av1Inspect = Require-File $Av1Inspect
foreach ($binary in @($FFmpeg, $HevcAnalyzer, $Av1Inspect)) { Assert-X64PE $binary }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if ($BuildDir -eq $PSScriptRoot) { throw 'Build directory must not be the source root.' }
Write-Host '[1/5] Building BitScope Release (Windows x64)...'
$testsOption = if ($SkipTests) { 'OFF' } else { 'ON' }
Invoke-Native cmake @('-S', $PSScriptRoot, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$QtPrefix", "-DQt6_DIR=$QtPrefix/lib/cmake/Qt6",
    "-DBITSCOPE_HEVC_ANALYZER=$HevcAnalyzer", "-DBITSCOPE_AV1_INSPECT_EXECUTABLE=$Av1Inspect",
    "-DBITSCOPE_BUILD_TESTS=$testsOption")
Invoke-Native cmake @('--build', $BuildDir, '--config', 'Release', '--parallel', "$Jobs")
$builtExe = Require-File "$BuildDir/Release/BitScope.exe"
$release = New-BuildDirectory $OutputDir 'BitScope-windows-x64'
$marker = Join-Path $release 'INCOMPLETE.txt'
'Packaging incomplete. Do not distribute while this file exists.' | Set-Content $marker
$package = Join-Path $release 'BitScope'
New-Item -ItemType Directory -Path "$package/tools", "$package/licenses", "$package/prerequisites" -Force | Out-Null
Copy-Item -LiteralPath $builtExe -Destination "$package/BitScope.exe"
Copy-Item -LiteralPath $FFmpeg -Destination "$package/ffmpeg.exe"
Copy-Item -LiteralPath $HevcAnalyzer -Destination "$package/tools/HM_180.exe"
Copy-Item -LiteralPath $Av1Inspect -Destination "$package/tools/av1inspect.exe"
# For explicitly supplied shared builds, preserve helper-adjacent DLLs too.
foreach ($entry in @(@($FFmpeg, $package), @($HevcAnalyzer, "$package/tools"), @($Av1Inspect, "$package/tools"))) {
    foreach ($dll in Get-ChildItem -LiteralPath (Split-Path $entry[0]) -Filter '*.dll' -File) {
        $destination = Join-Path $entry[1] $dll.Name
        if ((Test-Path $destination) -and (Get-FileHash $destination).Hash -ne (Get-FileHash $dll.FullName).Hash) {
            throw "Conflicting helper DLL: $($dll.Name)"
        }
        Copy-Item -LiteralPath $dll.FullName -Destination $destination
    }
}
Copy-Item -LiteralPath "$PSScriptRoot/third_party/hevc-cu-analyzer", "$PSScriptRoot/third_party/aom-inspection" -Destination "$package/licenses" -Recurse
if ($dependencyInfo) {
    Copy-Item -LiteralPath $dependencyInfo.Manifest -Destination "$package/licenses/dependencies.json"
    New-Item -ItemType Directory -Path "$package/licenses/ffmpeg" | Out-Null
    Get-ChildItem -LiteralPath $dependencyInfo.FFmpegRoot -File | Where-Object {
        $_.Name -match '^(LICENSE|COPYING|NOTICE|README)'
    } | Copy-Item -Destination "$package/licenses/ffmpeg"
}
if (Test-Path "$QtPrefix/licenses") { Copy-Item "$QtPrefix/licenses" "$package/licenses/qt" -Recurse }
Write-Host '[2/5] Deploying Qt libraries and plugins...'
# QtConcurrent::run is template-based; Release imports may only reference Core.
# Deploy Concurrent explicitly to match our linked modules and package contract,
# including the AV1 smoke executable, rather than relying on EXE imports alone.
Invoke-Native $deploy @('--release', '--concurrent', '--no-translations', '--no-compiler-runtime', '--dir', $package, "$package/BitScope.exe")
"[Paths]`nPrefix=.`nPlugins=.`n" | Set-Content -Encoding utf8 "$package/qt.conf"
foreach ($required in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Concurrent.dll', 'platforms/qwindows.dll', 'imageformats/qjpeg.dll')) {
    Require-File "$package/$required" | Out-Null
}
Get-ChildItem $package -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object { Assert-X64PE $_.FullName }
Write-Host '[3/5] Including the official Microsoft runtime installer...'
$runtime = Join-Path $package 'prerequisites/VC_redist.x64.exe'
if ($VcRedist) { Copy-Item -LiteralPath (Require-File $VcRedist) -Destination $runtime }
else { Get-CheckedDownload 'https://aka.ms/vs/17/release/vc_redist.x64.exe' $runtime '' | Out-Null }
$signature = Get-AuthenticodeSignature -LiteralPath $runtime
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw 'The Visual C++ runtime installer does not have a valid Microsoft signature.'
}
@'
BitScope for Windows x64 (Windows 10/11, Intel/AMD)

1. Extract the entire folder. Do not run the exe from inside a ZIP.
2. On a clean PC, install prerequisites\VC_redist.x64.exe once if the
   Microsoft Visual C++ 2015-2022 x64 runtime is not already installed.
3. Open BitScope.exe. Qt, FFmpeg, HEVC CU and AV1 block analyzers are included.

This is not a Windows ARM64 or 32-bit build. Keep all DLLs and subfolders.
The application is unsigned; Windows may show a SmartScreen warning.
Only allow software whose origin you trust; do not disable Windows security.

Third-party notices and patches are under licenses. Before public/commercial
distribution, review the actual Qt/FFmpeg/helper licenses and supply any
required source and license materials. Automatic collection is not a full
license compliance review. FFmpeg 8.0.1 essentials is a third-party GPL build.
'@ | Set-Content -Encoding utf8 "$package/README.txt"
Write-Host '[4/5] Checking the packaged program and video analyzers...'
if (-not $SkipTests) {
    & "$PSScriptRoot/scripts/test-windows-package.ps1" -PackageDir $package -BuildDir $BuildDir
} else { Write-Warning 'Smoke tests explicitly skipped; this package has not passed runtime checks.' }
$revision = & git -C $PSScriptRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { $revision = 'unknown' }
$manifest = @{ architecture = 'x64'; qt = (& "$QtPrefix/bin/qmake.exe" -query QT_VERSION);
    source_commit = "$revision"; smoke_tests_passed = (-not $SkipTests);
    runtime_sha256 = (Get-FileHash $runtime).Hash; files = @(
        Get-ChildItem $package -File -Recurse | ForEach-Object {
            @{ path = [IO.Path]::GetRelativePath($package, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash }
        }) }
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$release/build-manifest.json"
Write-Host '[5/5] Creating ZIP...'
$zip = Join-Path $release 'BitScope-windows-x64.zip'
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -LiteralPath $marker # Only our own completion marker, never user files.
Write-Host "Package ready: $package`nZIP: $zip"
if ($env:GITHUB_OUTPUT) {
    "package-dir=$package" | Add-Content -Encoding utf8 $env:GITHUB_OUTPUT
    "zip-path=$zip" | Add-Content -Encoding utf8 $env:GITHUB_OUTPUT
    "manifest-path=$release/build-manifest.json" | Add-Content -Encoding utf8 $env:GITHUB_OUTPUT
}
