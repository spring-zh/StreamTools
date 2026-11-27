#Requires -Version 7.0
# Cross-platform, no Pester dependency: syntax, guardrails and argument handling.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$files = @((Get-Item "$root/package-windows.ps1")) + @(Get-ChildItem "$root/scripts" -Filter '*.ps1') + @(Get-ChildItem "$root/tests" -Filter '*.ps1')
foreach ($file in $files) {
    $tokens = $null; $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count) { throw "$($file.FullName): $($parseErrors | Out-String)" }
}
. "$root/scripts/windows-common.ps1"
function Assert-True([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Throws([scriptblock]$Body) {
    $threw = $false
    try { & $Body } catch { $threw = $true }
    Assert-True $threw 'Expected an error, but operation succeeded.'
}
# Inspect the actual deployment call (not comments) without running Windows tools.
# Release builds may omit the Concurrent import even though the package requires it.
$packageAst = [Management.Automation.Language.Parser]::ParseFile("$root/package-windows.ps1", [ref]$null, [ref]$null)
$deployCalls = @($packageAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -eq 'Invoke-Native' -and
        $node.CommandElements.Count -ge 3 -and
        $node.CommandElements[1].Extent.Text -eq '$deploy'
}, $true))
Assert-True ($deployCalls.Count -eq 1) 'Expected one windeployqt invocation.'
$deployOptions = @($deployCalls[0].FindAll({
    param($node)
    $node -is [Management.Automation.Language.StringConstantExpressionAst]
}, $true) | ForEach-Object { $_.Value })
Assert-True ($deployOptions -contains '--concurrent') 'windeployqt must explicitly deploy the required Concurrent module.'
Assert-True ($deployOptions -notcontains '--ignore-library-errors') 'Qt deployment must not ignore missing libraries.'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('bitscope-ps-tests-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $pe = Join-Path $temp 'binary with spaces.exe'
    $stream = [IO.File]::Create($pe)
    $writer = [IO.BinaryWriter]::new($stream)
    $writer.Write([uint16]0x5A4D)
    $stream.Position = 0x3C; $writer.Write([int32]0x80)
    $stream.Position = 0x80; $writer.Write([uint32]0x4550); $writer.Write([uint16]0x8664)
    $writer.Dispose()
    Assert-X64PE $pe
    $stream = [IO.File]::OpenWrite($pe)
    $writer = [IO.BinaryWriter]::new($stream)
    $stream.Position = 0x84; $writer.Write([uint16]0xAA64); $writer.Dispose()
    Assert-Throws { Assert-X64PE $pe }
    Assert-Throws { Require-File "$temp/missing.exe" }
    $one = New-BuildDirectory $temp 'package'
    $two = New-BuildDirectory $temp 'package'
    Assert-True ($one -ne $two -and (Test-Path $one) -and (Test-Path $two)) 'Build output must not overwrite existing directories.'
    # Verify argument arrays and exit-code handling without running a compiler.
    # GitHub Actions checks LASTEXITCODE after the script returns. Mock failures
    # must not leak into that check, or a passing test is reported as failed.
    $previousExitCode = Get-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
    $hadExitCode = $null -ne $previousExitCode
    $savedExitCode = if ($hadExitCode) { $previousExitCode.Value } else { $null }
    try {
        function fake-native { $script:actualArgs = $args; $global:LASTEXITCODE = 0 }
        Invoke-Native fake-native @('-S', 'a folder/source', '-B', 'a folder/build')
        Assert-True ($script:actualArgs.Count -eq 4 -and $script:actualArgs[1] -eq 'a folder/source') 'Native arguments were split incorrectly.'
        function failed-native { $global:LASTEXITCODE = 9 }
        Assert-Throws { Invoke-Native failed-native @() }
        Assert-True ($global:LASTEXITCODE -eq 9) 'Failed-native mock did not set its exit code.'
    } finally {
        # Restore only mock state; do not catch assertions or force a successful exit.
        if ($hadExitCode) {
            $global:LASTEXITCODE = $savedExitCode
        } else {
            Remove-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
        }
    }
    $restoredExitCode = Get-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
    Assert-True (($null -ne $restoredExitCode) -eq $hadExitCode) 'Mock changed the presence of LASTEXITCODE.'
    if ($hadExitCode) {
        Assert-True ($restoredExitCode.Value -eq $savedExitCode) 'Mock leaked its exit code.'
    }
    Write-Host "PASS: $($files.Count) PowerShell files parsed; Qt deployment, x64 checks, missing files, unique outputs and native argument/exit-code checks."
} finally {
    # Only the unique test directory created above, never project/user data.
    Remove-Item -LiteralPath $temp -Recurse -Force
}
