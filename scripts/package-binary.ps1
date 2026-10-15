[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifactPath = [IO.Path]::GetFullPath((Join-Path $root 'artifact'))
$expectedArtifactPath = [IO.Path]::GetFullPath((Join-Path $root 'artifact'))
$requiredHostFFmpeg = @(
    'avcodec-62.dll',
    'avutil-60.dll',
    'swscale-9.dll',
    'swresample-6.dll'
)
$ffmpegDllPattern = '^(?i:(avcodec|avdevice|avfilter|avformat|avresample|avutil|postproc|swresample|swscale)-[0-9]+|ffmpeg(-[0-9]+)?)\.dll$'

function Get-ContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$BasePath,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ([IO.Path]::IsPathRooted($RelativePath)) {
        throw "$Description must be relative: $RelativePath"
    }

    $segments = @($RelativePath -split '[\\/]')
    if ($segments.Count -eq 0 -or
        @($segments | Where-Object {
            $_ -eq '' -or $_ -eq '.' -or $_ -eq '..'
        }).Count -gt 0) {
        throw "$Description contains an unsafe path segment: $RelativePath"
    }

    $invalidNameChars = [IO.Path]::GetInvalidFileNameChars()
    foreach ($segment in $segments) {
        if ($segment.IndexOfAny($invalidNameChars) -ge 0 -or
            $segment.EndsWith(' ') -or $segment.EndsWith('.')) {
            throw "$Description contains a non-canonical Windows path segment: $RelativePath"
        }
    }

    $baseFull = [IO.Path]::GetFullPath($BasePath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $basePrefix = $baseFull + [IO.Path]::DirectorySeparatorChar
    $candidate = [IO.Path]::GetFullPath((Join-Path $baseFull $RelativePath))
    if (-not $candidate.StartsWith(
        $basePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description escapes its root: $RelativePath"
    }

    return $candidate
}

function Assert-NoReparsePoint {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RootPath
    )

    $rootFull = [IO.Path]::GetFullPath($RootPath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $currentPath = [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    while (-not $currentPath.Equals(
        $rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        $current = Get-Item -LiteralPath $currentPath -Force
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Path traverses a reparse point: $($current.FullName)"
        }
        $parent = [IO.Directory]::GetParent($currentPath)
        if ($null -eq $parent) {
            throw "Path ancestry did not reach its expected root: $Path"
        }
        $currentPath = $parent.FullName.TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar)
    }
}

function Assert-SafeDirectoryTree {
    param([Parameter(Mandatory = $true)][string]$Path)

    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $pending.Enqueue([IO.Path]::GetFullPath($Path))
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        $directoryItem = Get-Item -LiteralPath $directory -Force
        if (($directoryItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to operate on a reparse-point directory: $directory"
        }
        foreach ($child in Get-ChildItem -LiteralPath $directory -Force) {
            if (($child.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to operate on a tree containing a reparse point: $($child.FullName)"
            }
            if ($child.PSIsContainer) {
                $pending.Enqueue($child.FullName)
            }
        }
    }
}

function Remove-SafeChildDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedParent,
        [Parameter(Mandatory = $true)][string]$RequiredNamePattern
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Refusing to replace a non-directory path: $Path"
    }

    $full = [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $parent = [IO.Directory]::GetParent($full)
    $allowed = [IO.Path]::GetFullPath($AllowedParent).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if ($null -eq $parent -or -not $parent.FullName.Equals(
        $allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside the expected parent: $full"
    }
    $name = [IO.Path]::GetFileName($full)
    if ($name -notmatch $RequiredNamePattern) {
        throw "Refusing to remove an unexpected directory name: $full"
    }

    Assert-SafeDirectoryTree -Path $full
    Remove-Item -LiteralPath $full -Recurse -Force
}

function Get-OrdinalSortedStrings {
    param([object[]]$Values)

    [string[]]$sorted = @($Values | ForEach-Object { [string]$_ })
    [Array]::Sort($sorted, [StringComparer]::OrdinalIgnoreCase)
    return $sorted
}

function Read-UInt16At {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][long]$Offset,
        [Parameter(Mandatory = $true)][long]$FileLength
    )
    if ($Offset -lt 0 -or $Offset + 2 -gt $FileLength) {
        throw "PE read is outside the file at offset $Offset"
    }
    [void]$Reader.BaseStream.Seek($Offset, [IO.SeekOrigin]::Begin)
    return $Reader.ReadUInt16()
}

function Read-UInt32At {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][long]$Offset,
        [Parameter(Mandatory = $true)][long]$FileLength
    )
    if ($Offset -lt 0 -or $Offset + 4 -gt $FileLength) {
        throw "PE read is outside the file at offset $Offset"
    }
    [void]$Reader.BaseStream.Seek($Offset, [IO.SeekOrigin]::Begin)
    return $Reader.ReadUInt32()
}

function Convert-PeRvaToOffset {
    param(
        [Parameter(Mandatory = $true)][uint32]$Rva,
        [Parameter(Mandatory = $true)][object[]]$Sections,
        [Parameter(Mandatory = $true)][uint32]$SizeOfHeaders,
        [Parameter(Mandatory = $true)][long]$FileLength
    )

    if ([uint64]$Rva -lt [uint64]$SizeOfHeaders) {
        if ([uint64]$Rva -ge [uint64]$FileLength) {
            throw "PE RVA 0x$($Rva.ToString('x')) is outside the file"
        }
        return [long]$Rva
    }

    foreach ($section in $Sections) {
        $start = [uint64]$section.VirtualAddress
        $span = [Math]::Max(
            [uint64]$section.VirtualSize, [uint64]$section.SizeOfRawData)
        $value = [uint64]$Rva
        if ($value -ge $start -and $value -lt ($start + $span)) {
            $delta = $value - $start
            if ($delta -ge [uint64]$section.SizeOfRawData) {
                throw "PE RVA 0x$($Rva.ToString('x')) has no file-backed data"
            }
            $offset = [uint64]$section.PointerToRawData + $delta
            if ($offset -ge [uint64]$FileLength) {
                throw "PE RVA 0x$($Rva.ToString('x')) maps outside the file"
            }
            return [long]$offset
        }
    }

    throw "PE RVA 0x$($Rva.ToString('x')) is not mapped by any section"
}

function Read-AsciiZAt {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][long]$Offset,
        [Parameter(Mandatory = $true)][long]$FileLength
    )

    if ($Offset -lt 0 -or $Offset -ge $FileLength) {
        throw "PE string is outside the file at offset $Offset"
    }
    [void]$Reader.BaseStream.Seek($Offset, [IO.SeekOrigin]::Begin)
    $bytes = New-Object 'System.Collections.Generic.List[byte]'
    for ($index = 0; $index -lt 512; $index++) {
        if ($Reader.BaseStream.Position -ge $FileLength) {
            throw "PE string reaches the end of the file at offset $Offset"
        }
        $value = $Reader.ReadByte()
        if ($value -eq 0) {
            if ($bytes.Count -eq 0) {
                throw "PE import contains an empty DLL name at offset $Offset"
            }
            return [Text.Encoding]::ASCII.GetString($bytes.ToArray())
        }
        if ($value -lt 0x20 -or $value -gt 0x7e) {
            throw "PE import contains a non-ASCII DLL name at offset $Offset"
        }
        $bytes.Add($value)
    }
    throw "PE import DLL name is unreasonably long at offset $Offset"
}

function Get-PeImports {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = $null
    try {
        $reader = New-Object IO.BinaryReader($stream)
        $length = $stream.Length
        if ($length -lt 0x100 -or
            (Read-UInt16At -Reader $reader -Offset 0 -FileLength $length) -ne 0x5a4d) {
            throw "Not a valid PE file: $Path"
        }

        $peOffset = [long](Read-UInt32At -Reader $reader -Offset 0x3c `
            -FileLength $length)
        if ((Read-UInt32At -Reader $reader -Offset $peOffset `
                -FileLength $length) -ne 0x00004550) {
            throw "PE signature is missing: $Path"
        }

        $machine = Read-UInt16At -Reader $reader -Offset ($peOffset + 4) `
            -FileLength $length
        $sectionCount = Read-UInt16At -Reader $reader -Offset ($peOffset + 6) `
            -FileLength $length
        $optionalSize = Read-UInt16At -Reader $reader -Offset ($peOffset + 20) `
            -FileLength $length
        if ($sectionCount -eq 0 -or $sectionCount -gt 96) {
            throw "PE section count is invalid: $sectionCount"
        }

        $optionalOffset = $peOffset + 24
        $optionalMagic = Read-UInt16At -Reader $reader -Offset $optionalOffset `
            -FileLength $length
        if ($optionalMagic -eq 0x20b) {
            $directoryCountOffset = $optionalOffset + 108
            $directoryOffset = $optionalOffset + 112
        } elseif ($optionalMagic -eq 0x10b) {
            $directoryCountOffset = $optionalOffset + 92
            $directoryOffset = $optionalOffset + 96
        } else {
            throw "Unsupported PE optional-header magic: 0x$($optionalMagic.ToString('x'))"
        }
        if ($directoryOffset + 16 -gt $optionalOffset + $optionalSize) {
            throw 'PE optional header is too small for the import directory'
        }

        $directoryCount = Read-UInt32At -Reader $reader `
            -Offset $directoryCountOffset -FileLength $length
        if ($directoryCount -lt 2) {
            throw 'PE file has no import data directory'
        }
        $importRva = Read-UInt32At -Reader $reader `
            -Offset ($directoryOffset + 8) -FileLength $length
        $importSize = Read-UInt32At -Reader $reader `
            -Offset ($directoryOffset + 12) -FileLength $length
        if ($importRva -eq 0 -or $importSize -lt 20) {
            throw 'PE file has an empty import directory'
        }

        $sizeOfHeaders = Read-UInt32At -Reader $reader `
            -Offset ($optionalOffset + 60) -FileLength $length
        $sectionTable = $optionalOffset + $optionalSize
        $sections = @()
        for ($index = 0; $index -lt $sectionCount; $index++) {
            $sectionOffset = $sectionTable + (40 * $index)
            if ($sectionOffset + 40 -gt $length) {
                throw 'PE section table extends beyond the file'
            }
            $sections += [pscustomobject]@{
                VirtualSize = Read-UInt32At -Reader $reader `
                    -Offset ($sectionOffset + 8) -FileLength $length
                VirtualAddress = Read-UInt32At -Reader $reader `
                    -Offset ($sectionOffset + 12) -FileLength $length
                SizeOfRawData = Read-UInt32At -Reader $reader `
                    -Offset ($sectionOffset + 16) -FileLength $length
                PointerToRawData = Read-UInt32At -Reader $reader `
                    -Offset ($sectionOffset + 20) -FileLength $length
            }
        }

        $importOffset = Convert-PeRvaToOffset -Rva $importRva `
            -Sections $sections -SizeOfHeaders $sizeOfHeaders -FileLength $length
        $importFileLimit = [uint64]$length
        if ([uint64]$importRva -lt [uint64]$sizeOfHeaders) {
            $importFileLimit = [Math]::Min(
                [uint64]$sizeOfHeaders, [uint64]$length)
        } else {
            $mappedSection = $null
            foreach ($section in $sections) {
                $sectionStart = [uint64]$section.VirtualAddress
                $sectionSpan = [Math]::Max(
                    [uint64]$section.VirtualSize,
                    [uint64]$section.SizeOfRawData)
                if ([uint64]$importRva -ge $sectionStart -and
                    [uint64]$importRva -lt ($sectionStart + $sectionSpan)) {
                    $mappedSection = $section
                    break
                }
            }
            if ($null -eq $mappedSection) {
                throw 'PE import directory is not mapped by a section'
            }
            $importFileLimit = [uint64]$mappedSection.PointerToRawData +
                [uint64]$mappedSection.SizeOfRawData
        }
        $importEnd = [uint64]$importOffset + [uint64]$importSize
        if ($importEnd -gt $importFileLimit -or
            $importEnd -gt [uint64]$length) {
            throw 'PE import directory extends beyond its file-backed section'
        }

        $imports = [System.Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        $terminated = $false
        $maximumDescriptors = [int][Math]::Floor(
            [double]$importSize / 20.0)
        for ($index = 0; $index -lt $maximumDescriptors; $index++) {
            $descriptorOffset = $importOffset + (20 * $index)
            $originalFirstThunk = Read-UInt32At -Reader $reader `
                -Offset $descriptorOffset -FileLength $length
            $timeDateStamp = Read-UInt32At -Reader $reader `
                -Offset ($descriptorOffset + 4) -FileLength $length
            $forwarderChain = Read-UInt32At -Reader $reader `
                -Offset ($descriptorOffset + 8) -FileLength $length
            $nameRva = Read-UInt32At -Reader $reader `
                -Offset ($descriptorOffset + 12) -FileLength $length
            $firstThunk = Read-UInt32At -Reader $reader `
                -Offset ($descriptorOffset + 16) -FileLength $length
            if ($originalFirstThunk -eq 0 -and $timeDateStamp -eq 0 -and
                $forwarderChain -eq 0 -and $nameRva -eq 0 -and
                $firstThunk -eq 0) {
                $terminated = $true
                break
            }
            if ($nameRva -eq 0) {
                throw "PE import descriptor $index has no DLL name"
            }
            $nameOffset = Convert-PeRvaToOffset -Rva $nameRva `
                -Sections $sections -SizeOfHeaders $sizeOfHeaders `
                -FileLength $length
            $name = Read-AsciiZAt -Reader $reader -Offset $nameOffset `
                -FileLength $length
            [void]$imports.Add($name)
        }
        if (-not $terminated) {
            throw 'PE import descriptor list is not terminated'
        }

        return [pscustomobject]@{
            Machine = $machine
            Imports = @(Get-OrdinalSortedStrings -Values @($imports))
        }
    } finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        } else {
            $stream.Dispose()
        }
    }
}

function Assert-ExpectedFFmpegImports {
    param([Parameter(Mandatory = $true)][string[]]$Imports)

    $observed = @($Imports | Where-Object { $_ -match $ffmpegDllPattern })
    $observedSet = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $observed) {
        [void]$observedSet.Add($name)
    }
    if ($observedSet.Count -ne $requiredHostFFmpeg.Count) {
        throw "Plugin FFmpeg import set is not exact. Expected: $($requiredHostFFmpeg -join ', '); found: $($observed -join ', ')"
    }
    foreach ($name in $requiredHostFFmpeg) {
        if (-not $observedSet.Contains($name)) {
            throw "Plugin DLL does not import required OBS host runtime: $name"
        }
    }
}

function Copy-VerifiedFile {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRelative,
        [Parameter(Mandatory = $true)][string]$DestinationRelative,
        [Parameter(Mandatory = $true)][string]$StagingArtifact
    )

    $source = Get-ContainedPath -BasePath $root -RelativePath $SourceRelative `
        -Description 'Binary package input'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing binary package input: $SourceRelative"
    }
    Assert-NoReparsePoint -Path $source -RootPath $root

    $destination = Get-ContainedPath -BasePath $StagingArtifact `
        -RelativePath $DestinationRelative -Description 'Binary package output'
    $destinationParent = Split-Path -Parent $destination
    if (-not (Test-Path -LiteralPath $destinationParent)) {
        New-Item -ItemType Directory -Path $destinationParent | Out-Null
    }

    $sourceHashBefore = (Get-FileHash -LiteralPath $source `
        -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $source -Destination $destination
    $destinationHash = (Get-FileHash -LiteralPath $destination `
        -Algorithm SHA256).Hash
    $sourceHashAfter = (Get-FileHash -LiteralPath $source `
        -Algorithm SHA256).Hash
    if ($sourceHashBefore -ne $sourceHashAfter -or
        $destinationHash -ne $sourceHashAfter) {
        throw "Package input changed while it was copied: $SourceRelative"
    }
}

function Get-ArtifactRelativeFiles {
    param([Parameter(Mandatory = $true)][string]$ArtifactRoot)

    $prefix = [IO.Path]::GetFullPath($ArtifactRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $relative = @(
        Get-ChildItem -LiteralPath $ArtifactRoot -File -Recurse | ForEach-Object {
            $full = [IO.Path]::GetFullPath($_.FullName)
            if (-not $full.StartsWith(
                $prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Artifact enumeration escaped its root: $full"
            }
            $full.Substring($prefix.Length) -replace '\\', '/'
        }
    )
    return @(Get-OrdinalSortedStrings -Values $relative)
}

function Assert-ArtifactContents {
    param(
        [Parameter(Mandatory = $true)][string]$ArtifactRoot,
        [Parameter(Mandatory = $true)][string[]]$ExpectedFiles
    )

    Assert-SafeDirectoryTree -Path $ArtifactRoot
    [string[]]$actual = @(Get-ArtifactRelativeFiles -ArtifactRoot $ArtifactRoot)
    [string[]]$expected = @(Get-OrdinalSortedStrings -Values $ExpectedFiles)
    if (($actual -join "`n") -cne ($expected -join "`n")) {
        throw "Artifact file set mismatch.`nExpected: $($expected -join ', ')`nActual: $($actual -join ', ')"
    }

    $forbidden = @(Get-ChildItem -LiteralPath $ArtifactRoot -File -Recurse |
        Where-Object {
            $_.Name -match $ffmpegDllPattern
        })
    if ($forbidden.Count -gt 0) {
        throw "Artifact must not bundle FFmpeg runtime DLLs: $($forbidden.Name -join ', ')"
    }

    $dllPath = Join-Path $ArtifactRoot 'airplay-live-source.dll'
    $pe = Get-PeImports -Path $dllPath
    if ($pe.Machine -ne 0x8664) {
        throw "Plugin DLL is not x64 (PE machine 0x$($pe.Machine.ToString('x')))"
    }
    Assert-ExpectedFFmpegImports -Imports $pe.Imports

    $cryptoImports = @($pe.Imports | Where-Object {
        $_ -match '^(?i:libcrypto-3.*-x64\.dll)$'
    })
    if ($cryptoImports.Count -ne 1) {
        throw "Expected exactly one OpenSSL 3 x64 import; found: $($cryptoImports -join ', ')"
    }
    $cryptoPath = Join-Path $ArtifactRoot $cryptoImports[0]
    if (-not (Test-Path -LiteralPath $cryptoPath -PathType Leaf)) {
        throw "Artifact does not contain its imported OpenSSL runtime: $($cryptoImports[0])"
    }
    $cryptoPe = Get-PeImports -Path $cryptoPath
    if ($cryptoPe.Machine -ne 0x8664) {
        throw "OpenSSL runtime is not x64 (PE machine 0x$($cryptoPe.Machine.ToString('x')))"
    }

    $manifestPath = Join-Path $ArtifactRoot 'SHA256SUMS.txt'
    $manifestLines = @(Get-Content -LiteralPath $manifestPath)
    $expectedManifestFiles = @($expected | Where-Object {
        $_ -cne 'SHA256SUMS.txt'
    })
    if ($manifestLines.Count -ne $expectedManifestFiles.Count) {
        throw "SHA256 manifest entry count mismatch: $($manifestLines.Count)"
    }
    $manifestFiles = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($line in $manifestLines) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
            throw "Invalid SHA256 manifest line: $line"
        }
        $relative = $Matches[2]
        if (-not $manifestFiles.Add($relative)) {
            throw "Duplicate SHA256 manifest path: $relative"
        }
        $path = Get-ContainedPath -BasePath $ArtifactRoot `
            -RelativePath $relative -Description 'SHA256 manifest path'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "SHA256 manifest references a missing file: $relative"
        }
        $actualHash = (Get-FileHash -LiteralPath $path `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -cne $Matches[1]) {
            throw "SHA256 manifest mismatch: $relative"
        }
    }
    foreach ($relative in $expectedManifestFiles) {
        if (-not $manifestFiles.Contains($relative)) {
            throw "SHA256 manifest omits an artifact file: $relative"
        }
    }
}

if (-not $artifactPath.Equals(
    $expectedArtifactPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe artifact path: $artifactPath"
}

& (Join-Path $PSScriptRoot 'check-source.ps1')
if ($LASTEXITCODE -ne 0) {
    throw 'Source consistency checks failed.'
}

$sourceDll = Get-ContainedPath -BasePath $root `
    -RelativePath 'build-local/airplay-live-source.dll' `
    -Description 'Built plugin DLL'
if (-not (Test-Path -LiteralPath $sourceDll -PathType Leaf)) {
    throw 'Missing build-local/airplay-live-source.dll; run build.bat first.'
}
Assert-NoReparsePoint -Path $sourceDll -RootPath $root
$sourcePe = Get-PeImports -Path $sourceDll
if ($sourcePe.Machine -ne 0x8664) {
    throw "build-local/airplay-live-source.dll is not x64 (PE machine 0x$($sourcePe.Machine.ToString('x')))"
}

Assert-ExpectedFFmpegImports -Imports $sourcePe.Imports
$sourceCryptoImports = @($sourcePe.Imports | Where-Object {
    $_ -match '^(?i:libcrypto-3.*-x64\.dll)$'
})
if ($sourceCryptoImports.Count -ne 1) {
    throw "Expected exactly one OpenSSL 3 x64 import; found: $($sourceCryptoImports -join ', ')"
}
$cryptoName = $sourceCryptoImports[0]
$cryptoSourceRelative = 'deps/openssl/bin/' + $cryptoName

$ffmpegBuildConf = Get-ContainedPath -BasePath $root `
    -RelativePath 'deps/ffmpeg-sdk-buildconf.txt' `
    -Description 'FFmpeg build configuration'
if (-not (Test-Path -LiteralPath $ffmpegBuildConf -PathType Leaf)) {
    throw 'Missing binary package input: deps/ffmpeg-sdk-buildconf.txt'
}
$buildConfText = Get-Content -LiteralPath $ffmpegBuildConf -Raw
if ($buildConfText -match '--enable-(gpl|nonfree|libfdk_aac)(\s|$)') {
    throw 'Pinned FFmpeg build configuration contains a non-LGPL option.'
}

$stageName = '.airplay-binary-stage-' + [Guid]::NewGuid().ToString('N')
$stagingRoot = Get-ContainedPath -BasePath $root -RelativePath $stageName `
    -Description 'Binary package staging path'
$stagingArtifact = Join-Path $stagingRoot 'artifact'
$backupPath = $null
$published = $false

try {
    New-Item -ItemType Directory -Path $stagingArtifact | Out-Null

    $copies = @(
        @('build-local/airplay-live-source.dll', 'airplay-live-source.dll'),
        @($cryptoSourceRelative, $cryptoName),
        @('deps/ffmpeg-sdk-buildconf.txt', 'ffmpeg-sdk-buildconf.txt'),
        @('install.bat', 'install.bat'),
        @('uninstall.bat', 'uninstall.bat'),
        @('README.md', 'README.md'),
        @('data/locale/en-US.ini', 'data/locale/en-US.ini'),
        @('data/locale/zh-CN.ini', 'data/locale/zh-CN.ini'),
        @('LICENSES/aomkoyo-LGPL-2.1-or-later-NOTICE.txt', 'UPSTREAM-LGPL-NOTICE.txt'),
        @('LICENSES/GPL-3.0.txt', 'GPL-3.0.txt'),
        @('LICENSES/llhttp-MIT.txt', 'LLHTTP-MIT-NOTICE.txt'),
        @('LICENSES/LGPL-2.1-or-later.txt', 'LGPL-2.1-or-later.txt'),
        @('deps/openssl/LICENSE.txt', 'OPENSSL-LICENSE.txt'),
        @('NOTICE.md', 'NOTICE.md'),
        @('RELEASE-GATE.md', 'RELEASE-GATE.md'),
        @('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md')
    )
    foreach ($copy in $copies) {
        Copy-VerifiedFile -SourceRelative $copy[0] `
            -DestinationRelative $copy[1] -StagingArtifact $stagingArtifact
    }

    $stagedPe = Get-PeImports -Path (Join-Path $stagingArtifact `
        'airplay-live-source.dll')
    $dependencyLines = @(
        'AirPlay Live Source PE dependency audit',
        'Machine: AMD64 (0x8664)',
        'Policy: FFmpeg runtime DLLs are imported from the OBS 32.2.2 host and are not bundled.',
        '',
        'Imported DLLs:'
    ) + @($stagedPe.Imports | ForEach-Object { '  ' + $_ })
    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllLines(
        (Join-Path $stagingArtifact 'dependencies.txt'),
        [string[]]$dependencyLines, $utf8NoBom)

    $payloadFiles = @(Get-ArtifactRelativeFiles -ArtifactRoot $stagingArtifact)
    $manifestLines = foreach ($relative in $payloadFiles) {
        $path = Get-ContainedPath -BasePath $stagingArtifact `
            -RelativePath $relative -Description 'SHA256 manifest input'
        $hash = (Get-FileHash -LiteralPath $path `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $relative"
    }
    [IO.File]::WriteAllLines(
        (Join-Path $stagingArtifact 'SHA256SUMS.txt'),
        [string[]]$manifestLines, $utf8NoBom)

    $expectedFiles = @($copies | ForEach-Object { $_[1] }) + @(
        'dependencies.txt',
        'SHA256SUMS.txt'
    )
    Assert-ArtifactContents -ArtifactRoot $stagingArtifact `
        -ExpectedFiles $expectedFiles

    if (Test-Path -LiteralPath $artifactPath) {
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Container)) {
            throw "Refusing to replace a non-directory artifact path: $artifactPath"
        }
        Assert-SafeDirectoryTree -Path $artifactPath
        $backupName = '.airplay-artifact-backup-' +
            [Guid]::NewGuid().ToString('N')
        $backupPath = Get-ContainedPath -BasePath $root `
            -RelativePath $backupName -Description 'Artifact backup path'
        Move-Item -LiteralPath $artifactPath -Destination $backupPath
    }

    $newArtifactPlaced = $false
    try {
        Move-Item -LiteralPath $stagingArtifact -Destination $artifactPath
        $newArtifactPlaced = $true
        Assert-ArtifactContents -ArtifactRoot $artifactPath `
            -ExpectedFiles $expectedFiles
    } catch {
        if ($newArtifactPlaced -and
            (Test-Path -LiteralPath $artifactPath -PathType Container)) {
            Remove-SafeChildDirectory -Path $artifactPath -AllowedParent $root `
                -RequiredNamePattern '^artifact$'
        }
        if ($null -ne $backupPath -and
            (Test-Path -LiteralPath $backupPath -PathType Container) -and
            -not (Test-Path -LiteralPath $artifactPath)) {
            Move-Item -LiteralPath $backupPath -Destination $artifactPath
            $backupPath = $null
        }
        throw
    }

    if ($null -ne $backupPath) {
        Remove-SafeChildDirectory -Path $backupPath -AllowedParent $root `
            -RequiredNamePattern '^\.airplay-artifact-backup-[0-9a-f]{32}$'
        $backupPath = $null
    }
    $published = $true

    $manifestHash = (Get-FileHash -LiteralPath `
        (Join-Path $artifactPath 'SHA256SUMS.txt') -Algorithm SHA256).Hash
    Write-Host "Created and verified: $artifactPath"
    Write-Host "Files:   $($expectedFiles.Count)"
    Write-Host "Manifest SHA-256: $manifestHash"
} finally {
    if (-not $published -and $null -ne $backupPath -and
        (Test-Path -LiteralPath $backupPath -PathType Container) -and
        -not (Test-Path -LiteralPath $artifactPath)) {
        Move-Item -LiteralPath $backupPath -Destination $artifactPath
        $backupPath = $null
    }
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-SafeChildDirectory -Path $stagingRoot -AllowedParent $root `
            -RequiredNamePattern '^\.airplay-binary-stage-[0-9a-f]{32}$'
    }
}
