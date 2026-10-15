[CmdletBinding()]
param([string]$OutputDirectory)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
& (Join-Path $PSScriptRoot 'check-source.ps1')
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Split-Path -Parent $root) 'source-packages'
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$rootPrefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if ($outputRoot -eq $root -or $outputRoot.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Write source archives outside the source tree'
}
$paths = @(Get-Content -LiteralPath (Join-Path $root 'SOURCE-FILES.txt') | Where-Object { $_ -and -not $_.StartsWith('#') })
$manifest = Get-Content -LiteralPath (Join-Path $root 'SOURCE-MANIFEST.json') -Raw | ConvertFrom-Json
$manifestDigest = (Get-FileHash -LiteralPath (Join-Path $root 'SOURCE-MANIFEST.json') -Algorithm SHA256).Hash.ToLowerInvariant()
$packageName = 'FurryUP-AirPlay-Source-Preview-' + $manifest.snapshotDate + '-' + $manifestDigest.Substring(0,12)
[void][IO.Directory]::CreateDirectory($outputRoot)
$archivePath = Join-Path $outputRoot ($packageName + '.zip')
Add-Type -AssemblyName System.IO.Compression
$stream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
$zip = $null
try {
    $zip = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
    foreach ($relative in $paths) {
        $entry = $zip.CreateEntry($packageName + '/' + $relative, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]::Parse($manifest.snapshotDate + 'T00:00:00Z')
        $inputStream = [IO.File]::OpenRead((Join-Path $root $relative))
        $entryStream = $entry.Open()
        try { $inputStream.CopyTo($entryStream) }
        finally { $entryStream.Dispose(); $inputStream.Dispose() }
    }
} finally {
    if ($null -ne $zip) { $zip.Dispose() } else { $stream.Dispose() }
}
$stream = [IO.File]::OpenRead($archivePath)
$zip = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Read, $false)
try {
    if ($zip.Entries.Count -ne $paths.Count) { throw 'Source archive entry count mismatch' }
    $remaining = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($relative in $paths) { [void]$remaining.Add($packageName + '/' + $relative) }
    foreach ($entry in $zip.Entries) {
        if (-not $remaining.Remove($entry.FullName)) { throw 'Unexpected or duplicate source archive entry' }
        $relative = $entry.FullName.Substring($packageName.Length + 1)
        $entryStream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $digest = [BitConverter]::ToString($sha.ComputeHash($entryStream)).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose(); $entryStream.Dispose() }
        if ($digest -ne (Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw "Source archive hash mismatch: $relative"
        }
    }
    if ($remaining.Count -ne 0) { throw 'Source archive omitted selected files' }
} finally { $zip.Dispose() }
Write-Host ("Created and verified source-only archive: {0}" -f $archivePath)
Get-FileHash -LiteralPath $archivePath -Algorithm SHA256 | Select-Object Hash
