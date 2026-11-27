#Requires -Version 7.0
# Run against the pristine, pinned Gitl checkout before applying our patch.
param([Parameter(Mandatory)][string]$GitlSource)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/windows-common.ps1"
$patch = (Resolve-Path "$PSScriptRoot/../third_party/hevc-cu-analyzer/bitscope-output-frames.patch").Path
$relative = 'appgitlhevcdecoder/HM_18.0/TLibSysuAnalyzer'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('bitscope-hevc-patch-tests-' + [guid]::NewGuid())
function Assert-Patch([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "HEVC patch regression: $Message" }
}
try {
    foreach ($ending in @('LF', 'CRLF')) {
        $fixture = Join-Path $temp $ending
        $directory = Join-Path $fixture $relative
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Invoke-Native git @('init', '--quiet', $fixture)
        $newline = if ($ending -eq 'CRLF') { "`r`n" } else { "`n" }
        foreach ($name in @('TSysuAnalyzerOutput.cpp', 'TSysuAnalyzerOutput.h')) {
            # git show deliberately bypasses any already-patched working tree.
            $lines = @(& git -C $GitlSource show "HEAD:$relative/$name")
            if ($LASTEXITCODE -ne 0) { throw "Cannot read pristine $name" }
            [IO.File]::WriteAllText((Join-Path $directory $name),
                ($lines -join $newline) + $newline, [Text.UTF8Encoding]::new($false))
        }
        Invoke-Native git @('-C', $fixture, 'apply', '--check', '--ignore-space-change', $patch)
        Invoke-Native git @('-C', $fixture, 'apply', '--ignore-space-change', $patch)
        $cpp = [IO.File]::ReadAllText((Join-Path $directory 'TSysuAnalyzerOutput.cpp')).Replace("`r`n", "`n")
        $header = [IO.File]::ReadAllText((Join-Path $directory 'TSysuAnalyzerOutput.h'))
        $cu = [regex]::Match($cpp, '(?ms)^void TSysuAnalyzerOutput::writeOutCUInfo\s*\([^\n]*\)\n\{.*?^\}').Value
        $leaf = [regex]::Match($cpp, '(?ms)^void TSysuAnalyzerOutput::xWriteOutCUInfo\s*\([^\n]*\)\n\{.*?^\}').Value
        Assert-Patch ($cu.Length -gt 0 -and $leaf.Length -gt 0) 'CU output functions not found.'
        $qpHeader = $cu.IndexOf('m_cQPOutput << "<"')
        $recurse = $cu.IndexOf('xWriteOutCUInfo  (')
        $qpEnd = $cu.IndexOf('m_cQPOutput << endl;')
        Assert-Patch ($qpHeader -ge 0 -and $qpHeader -lt $recurse -and $qpEnd -gt $recurse) 'QP header/newline must surround leaf output INSIDE writeOutCUInfo.'
        Assert-Patch ($cu.IndexOf('if( !shouldWriteFrame( pcCU ) ) return;') -lt $qpHeader -and $cu.Contains('shouldWriteFrame( pcCU )')) 'Frame filter must precede output.'
        Assert-Patch ($leaf.Contains('m_cQPOutput << (puhPartSize[iOffset] == NUMBER_OF_PART_SIZES ? -1 : pcCU->getQP(iOffset))')) 'Leaf QP export missing.'
        Assert-Patch ([regex]::Matches($cpp, 'm_cQPOutput').Count -eq 4) 'Missing or stray QP statements.'
        Assert-Patch ($header.Contains('std::ofstream m_cQPOutput;') -and $header.Contains('std::set<Int>') -and $header.Contains('Bool shouldWriteFrame')) 'Analyzer declarations missing.'
        Write-Host "PASS: HEVC patch applies to pristine $ending sources; QP record order, function scope and frame filter verified."
    }
} finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
