# Shared by the build, packaging and verification scripts. PowerShell 7+.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

function Require-File {
    param([string]$Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-X64PE {
    param([string]$Path)
    $stream = [IO.File]::OpenRead((Require-File $Path))
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows executable: $Path" }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset + 6 -gt $stream.Length) { throw "Invalid PE header: $Path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "Expected Windows x64 (not ARM64/x86): $Path"
        }
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function New-BuildDirectory {
    param([string]$Parent, [string]$Prefix)
    New-Item -ItemType Directory -Force -Path $Parent | Out-Null
    $path = Join-Path $Parent ($Prefix + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $path | Out-Null
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-CheckedDownload {
    param([string]$Url, [string]$Destination, [string]$Sha256)
    Invoke-WebRequest -Uri $Url -OutFile $Destination -MaximumRetryCount 3 -RetryIntervalSec 3
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash
    if ($Sha256 -and $actual -ne $Sha256) { throw "SHA256 mismatch for $Url (got $actual)" }
    return $actual
}
