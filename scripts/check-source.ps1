[CmdletBinding()]
param()
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$prefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$paths = @(Get-Content -LiteralPath (Join-Path $root 'SOURCE-FILES.txt') | Where-Object { $_ -and -not $_.StartsWith('#') })
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$utf8 = New-Object Text.UTF8Encoding($false, $true)
foreach ($relative in $paths) {
    if ($relative -match '\\|(^|/)\.\.?(/|$)|(^|/)(collector|curriculum|training|courseware)(/|$)' -or
        [IO.Path]::IsPathRooted($relative) -or -not $seen.Add($relative)) {
        throw "Unsafe or duplicate source path: $relative"
    }
    $absolute = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source path escapes root: $relative"
    }
    $item = Get-Item -LiteralPath $absolute -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Source input is not a regular file: $relative"
    }
    $ancestor = $item.Directory
    while ($ancestor.FullName -ne $root) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked source ancestor: $relative" }
        $ancestor = $ancestor.Parent
        if ($null -eq $ancestor) { throw "Invalid source ancestry: $relative" }
    }
    if ($relative -match '\.(exe|dll|obj|lib|pdb|zip|7z|pfx|pem|key|p12)$') {
        throw "Non-source input: $relative"
    }
    $bytes = [IO.File]::ReadAllBytes($absolute)
    if ([Array]::IndexOf($bytes, [byte]0) -ge 0) { throw "Binary input: $relative" }
    $text = $utf8.GetString($bytes)
    if ($text -match 'C:[/\\]Users[/\\]|-----BEGIN [A-Z ]*PRIVATE KEY|gh[pousr]_[A-Za-z0-9]{20}|github_pat_[A-Za-z0-9_]{20}') {
        throw "Credential or local user-path match: $relative"
    }
}
# Ignore only local build/cache directories. All other files must be selected.
function Get-SourceCandidates([string]$directory, [string]$relativeDirectory) {
    foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
        $relative = if ($relativeDirectory) { "$relativeDirectory/$($item.Name)" } else { $item.Name }
        if ($item.PSIsContainer) {
            if (-not $relativeDirectory -and $item.Name -match '^(\.git|\.deps-cache|deps|build[^/]*|artifact[^/]*|outputs|\.vs|\.vscode)$') { continue }
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked source directory: $relative" }
            Get-SourceCandidates $item.FullName $relative
        } else { $relative }
    }
}
foreach ($relative in @(Get-SourceCandidates $root '')) {
    if (-not $seen.Contains($relative)) { throw "Unlisted source file: $relative" }
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'SOURCE-MANIFEST.json') -Raw | ConvertFrom-Json
$manifestSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
foreach ($row in $manifest.files) {
    if (-not $seen.Contains([string]$row.path) -or -not $manifestSet.Add([string]$row.path)) { throw 'Invalid source manifest entry' }
    $absolute = Join-Path $root $row.path
    if ((Get-Item -LiteralPath $absolute).Length -ne $row.bytes -or
        (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant() -ne $row.sha256) {
        throw "Source snapshot hash mismatch: $($row.path)"
    }
}
if ($manifestSet.Count -ne $paths.Count - 1 -or $manifestSet.Contains('SOURCE-MANIFEST.json')) {
    throw 'Manifest must cover every selected file except itself'
}
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
foreach ($match in [regex]::Matches($cmake, '(?m)^\s*(src/[A-Za-z0-9_-]+\.c)\s*$')) {
    if (-not $seen.Contains($match.Groups[1].Value)) { throw 'CMake source omitted from snapshot' }
}
if ($cmake -notmatch 'add_library\(airplay-live-source MODULE' -or $cmake -match 'furryup-studio|live-coach|collector') {
    throw 'Native build boundary is unexpected'
}
$license = Get-Content -LiteralPath (Join-Path $root 'LICENSE') -Raw
if ($license -notmatch 'Version 3, 29 June 2007') { throw 'GPLv3 root license missing' }
& (Join-Path $PSScriptRoot 'prepare-deps.ps1') -ValidateOnly
Write-Host ("Source preview checks passed: {0} allowlisted text files; manifest, dependency recipes, scope and secret checks passed." -f $paths.Count)
