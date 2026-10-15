[CmdletBinding()]
param(
    [switch]$Offline,
    [switch]$ValidateOnly
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$lockPath = Join-Path $projectRoot 'dependencies.lock.json'
$cacheRoot = Join-Path $projectRoot '.deps-cache'
$archiveRoot = Join-Path $cacheRoot 'archives'
$stagingRoot = Join-Path $cacheRoot 'staging'
$depsRoot = Join-Path $projectRoot 'deps'
$markerName = '.airplay-dependency.json'
$utf8NoBom = New-Object Text.UTF8Encoding($false)

function Get-ContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$BasePath,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath)) {
        throw "$Description must be a non-empty relative path: $RelativePath"
    }

    $segments = @($RelativePath -split '[\\/]')
    if (@($segments | Where-Object {
        $_ -eq '' -or $_ -eq '.' -or $_ -eq '..'
    }).Count -gt 0) {
        throw "$Description contains an unsafe path segment: $RelativePath"
    }

    $baseFull = [IO.Path]::GetFullPath($BasePath)
    $prefix = $baseFull.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $candidate = [IO.Path]::GetFullPath((Join-Path $baseFull $RelativePath))
    if (-not $candidate.StartsWith(
        $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description escapes its root: $RelativePath"
    }
    return $candidate
}

function Assert-NoNestedReparsePoint {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RootPath
    )

    $rootFull = [IO.Path]::GetFullPath($RootPath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $pathFull = [IO.Path]::GetFullPath($Path)
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if ($pathFull -ne $rootFull -and -not $pathFull.StartsWith(
        $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the project root: $pathFull"
    }

    $relative = $pathFull.Substring($rootFull.Length).TrimStart(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $current = $rootFull
    foreach ($segment in @($relative -split '[\\/]' | Where-Object { $_ })) {
        $current = Join-Path $current $segment
        if (-not (Test-Path -LiteralPath $current)) {
            break
        }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Dependency path traverses a reparse point: $($item.FullName)"
        }
    }
}

function New-PrivateDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    Assert-NoNestedReparsePoint -Path $Path -RootPath $projectRoot
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) {
        throw "Expected a directory: $Path"
    }
    Assert-NoNestedReparsePoint -Path $Path -RootPath $projectRoot
}

function Remove-StagingDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $stagingFull = [IO.Path]::GetFullPath($stagingRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $pathFull = [IO.Path]::GetFullPath($Path)
    $prefix = $stagingFull + [IO.Path]::DirectorySeparatorChar
    if ($pathFull -eq $stagingFull -or -not $pathFull.StartsWith(
        $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a non-staging directory: $pathFull"
    }
    Assert-NoNestedReparsePoint -Path $pathFull -RootPath $projectRoot
    Remove-Item -LiteralPath $pathFull -Recurse -Force
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString(
            $sha.ComputeHash($stream)).Replace('-', '')
    } finally {
        try {
            $sha.Dispose()
        } finally {
            $stream.Dispose()
        }
    }
}

function Get-StringSha256 {
    param([Parameter(Mandatory = $true)][string[]]$Parts)

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes([string]::Join("`n", $Parts))
        return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}

function Assert-Sha256Text {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if ($Value -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "$Description is not a complete SHA-256 value"
    }
}

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Description
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or
        [string]::IsNullOrWhiteSpace([string]$property.Value)) {
        throw "$Description is missing property '$Name'"
    }
    return $property.Value
}

function Assert-LockedFile {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$ExpectedHash
    )
    Assert-Sha256Text -Value $ExpectedHash -Description "$RelativePath hash"
    $path = Get-ContainedPath -BasePath $projectRoot `
        -RelativePath $RelativePath -Description 'Locked repository file'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Locked repository file is missing: $RelativePath"
    }
    $actual = Get-Sha256 $path
    if ($actual -ne $ExpectedHash.ToUpperInvariant()) {
        throw "Locked repository file hash mismatch: $RelativePath`nExpected: $ExpectedHash`nActual:   $actual"
    }
    return $path
}

function Get-Dependency {
    param([Parameter(Mandatory = $true)][string]$Name)
    $property = $dependencyLock.dependencies.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Dependency lock is missing '$Name'"
    }
    return $property.Value
}

function Test-PreparedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][string]$Dependency,
        [Parameter(Mandatory = $true)][string]$RecipeHash,
        [Parameter(Mandatory = $true)][string[]]$RequiredFiles
    )

    if (-not (Test-Path -LiteralPath $Target)) {
        return $false
    }
    $targetItem = Get-Item -LiteralPath $Target -Force
    if (-not $targetItem.PSIsContainer) {
        throw "Prepared dependency target is not a directory: $Target"
    }
    Assert-NoNestedReparsePoint -Path $Target -RootPath $projectRoot
    $marker = Join-Path $Target $markerName
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        throw "Existing dependency is unmanaged; refusing to overwrite it: $Target"
    }
    try {
        $metadata = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    } catch {
        throw "Dependency marker is unreadable: $marker"
    }
    if ([string]$metadata.dependency -ne $Dependency -or
        [string]$metadata.recipeSha256 -ne $RecipeHash) {
        throw "Existing dependency was prepared from a different recipe: $Target"
    }
    foreach ($relative in $RequiredFiles) {
        $required = Get-ContainedPath -BasePath $Target `
            -RelativePath $relative -Description 'Prepared dependency file'
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Prepared dependency is incomplete: $required"
        }
    }
    Write-Host "Ready: $Dependency"
    return $true
}

function Write-PreparedMarker {
    param(
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][string]$Dependency,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$ArchiveSha256,
        [Parameter(Mandatory = $true)][string]$RecipeHash
    )
    $metadata = [ordered]@{
        schemaVersion = 1
        dependency = $Dependency
        version = $Version
        archiveSha256 = $ArchiveSha256.ToUpperInvariant()
        recipeSha256 = $RecipeHash
        preparedUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $json = $metadata | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $Target $markerName), $json, $utf8NoBom)
}

function Move-PreparedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Target
    )
    if (Test-Path -LiteralPath $Target) {
        throw "Refusing to replace an existing dependency: $Target"
    }
    $parent = Split-Path -Parent $Target
    New-PrivateDirectory $parent
    Move-Item -LiteralPath $Source -Destination $Target
}

function Get-CachedArchive {
    param([Parameter(Mandatory = $true)][object]$Dependency)

    $fileName = [string](Get-RequiredProperty $Dependency 'archiveFile' 'Dependency')
    if ([IO.Path]::GetFileName($fileName) -ne $fileName) {
        throw "Archive name must not contain a path: $fileName"
    }
    $expectedHash = [string](Get-RequiredProperty $Dependency 'sha256' $fileName)
    Assert-Sha256Text -Value $expectedHash -Description "$fileName archive hash"
    $archive = Get-ContainedPath -BasePath $archiveRoot `
        -RelativePath $fileName -Description 'Dependency archive'

    if (Test-Path -LiteralPath $archive -PathType Leaf) {
        $actual = Get-Sha256 $archive
        if ($actual -ne $expectedHash.ToUpperInvariant()) {
            throw "Cached archive hash mismatch; remove it manually before retrying: $archive`nExpected: $expectedHash`nActual:   $actual"
        }
        Write-Host "Cached: $fileName"
        return $archive
    }
    if ($Offline) {
        throw "Offline mode cannot find cached archive: $archive"
    }

    $uriText = [string](Get-RequiredProperty $Dependency 'url' $fileName)
    $uri = $null
    if (-not [Uri]::TryCreate($uriText, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -ne 'https') {
        throw "Dependency URL must be absolute HTTPS: $uriText"
    }

    $partial = "$archive.partial.$([Guid]::NewGuid().ToString('N'))"
    try {
        Write-Host "Downloading: $fileName"
        if ([Net.ServicePointManager]::SecurityProtocol -band
            [Net.SecurityProtocolType]::Tls12) {
            # TLS 1.2 is already enabled.
        } else {
            [Net.ServicePointManager]::SecurityProtocol =
                [Net.ServicePointManager]::SecurityProtocol -bor
                [Net.SecurityProtocolType]::Tls12
        }
        Invoke-WebRequest -Uri $uri.AbsoluteUri -OutFile $partial `
            -UseBasicParsing
        $actual = Get-Sha256 $partial
        if ($actual -ne $expectedHash.ToUpperInvariant()) {
            throw "Downloaded archive hash mismatch for $fileName`nExpected: $expectedHash`nActual:   $actual"
        }
        Move-Item -LiteralPath $partial -Destination $archive
        return $archive
    } finally {
        if (Test-Path -LiteralPath $partial) {
            Remove-Item -LiteralPath $partial -Force
        }
    }
}

function Expand-ZipSafely {
    param(
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path -LiteralPath $Destination) {
        throw "ZIP destination already exists: $Destination"
    }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    $seen = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    try {
        foreach ($entry in $zip.Entries) {
            $entryName = $entry.FullName
            if ([string]::IsNullOrEmpty($entryName) -or
                $entryName.Contains('\')) {
                throw "ZIP contains a non-canonical entry name: $entryName"
            }
            $trimmed = $entryName.TrimEnd('/')
            if ([string]::IsNullOrEmpty($trimmed)) {
                continue
            }
            $segments = @($trimmed -split '/')
            if (@($segments | Where-Object {
                $_ -eq '' -or $_ -eq '.' -or $_ -eq '..'
            }).Count -gt 0 -or [IO.Path]::IsPathRooted($trimmed)) {
                throw "ZIP contains an unsafe entry name: $entryName"
            }
            $relative = $trimmed -replace '/', [IO.Path]::DirectorySeparatorChar
            $output = Get-ContainedPath -BasePath $Destination `
                -RelativePath $relative -Description 'ZIP entry'
            if (-not $seen.Add($output)) {
                throw "ZIP contains a duplicate entry: $entryName"
            }
            if ($entryName.EndsWith('/')) {
                New-Item -ItemType Directory -Path $output -Force | Out-Null
                continue
            }
            $parent = Split-Path -Parent $output
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
            $inputStream = $entry.Open()
            $outputStream = $null
            try {
                $outputStream = [IO.File]::Open(
                    $output, [IO.FileMode]::CreateNew,
                    [IO.FileAccess]::Write, [IO.FileShare]::None)
                $inputStream.CopyTo($outputStream)
            } finally {
                try {
                    if ($null -ne $outputStream) {
                        $outputStream.Dispose()
                    }
                } finally {
                    $inputStream.Dispose()
                }
            }
        }
    } finally {
        $zip.Dispose()
    }
}

function Copy-ZipEntry {
    param(
        [Parameter(Mandatory = $true)][object]$Entry,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    $output = Get-ContainedPath -BasePath $DestinationRoot `
        -RelativePath $RelativePath -Description 'Selected ZIP entry'
    $parent = Split-Path -Parent $output
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $inputStream = $Entry.Open()
    $outputStream = $null
    try {
        $outputStream = [IO.File]::Open(
            $output, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $inputStream.CopyTo($outputStream)
    } finally {
        try {
            if ($null -ne $outputStream) {
                $outputStream.Dispose()
            }
        } finally {
            $inputStream.Dispose()
        }
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Native command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
        }
    } finally {
        Pop-Location
    }
}

function Test-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments 2>$null | Out-Null
        return ($LASTEXITCODE -eq 0)
    } finally {
        Pop-Location
    }
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )
    Push-Location $WorkingDirectory
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object {
            $_.ToString()
        })
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw "Native command failed ($exitCode): $FilePath $($Arguments -join ' ')"
    }
    return [string[]]$output
}

function Get-ToolPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        throw "Required build tool '$Name' is unavailable. Run this script from an x64 Visual Studio 2022 or 2026 Developer Command Prompt after installing the C++ and CMake components."
    }
    return $command.Source
}

function Install-OpenSsl {
    param([Parameter(Mandatory = $true)][object]$Dependency)

    $target = Join-Path $depsRoot 'openssl'
    $recipe = Get-StringSha256 @(
        'openssl-selective-v1', [string]$Dependency.sha256,
        [string]$Dependency.version)
    $required = @(
        'include/openssl/opensslv.h',
        'lib/libcrypto.lib',
        'bin/libcrypto-3-x64.dll',
        'LICENSE.txt')
    if (Test-PreparedDirectory $target 'openssl' $recipe $required) {
        return
    }
    $archive = Get-CachedArchive $Dependency
    $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $stage | Out-Null
        $source = Join-Path $stage 'openssl'
        New-Item -ItemType Directory -Path $source | Out-Null
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        $headerCount = 0
        try {
            foreach ($entry in $zip.Entries) {
                $name = $entry.FullName
                if ($name.StartsWith(
                    'include/openssl/', [StringComparison]::Ordinal) -and
                    -not $name.EndsWith('/') -and
                    -not $name.EndsWith('.in')) {
                    Copy-ZipEntry $entry $source $name
                    $headerCount++
                } elseif ($name -eq 'lib/win-x64/native/libcrypto.lib') {
                    Copy-ZipEntry $entry $source 'lib/libcrypto.lib'
                } elseif ($name -eq
                    'runtimes/win-x64/native/libcrypto-3-x64.dll') {
                    Copy-ZipEntry $entry $source 'bin/libcrypto-3-x64.dll'
                } elseif ($name -eq 'docs/license.txt') {
                    Copy-ZipEntry $entry $source 'LICENSE.txt'
                }
            }
        } finally {
            $zip.Dispose()
        }
        if ($headerCount -lt 20) {
            throw "OpenSSL package contained too few headers: $headerCount"
        }
        foreach ($relative in $required) {
            $path = Get-ContainedPath $source $relative 'OpenSSL output'
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "OpenSSL package layout is missing: $relative"
            }
        }
        Write-PreparedMarker $source 'openssl' ([string]$Dependency.version) `
            ([string]$Dependency.sha256) $recipe
        Move-PreparedDirectory $source $target
        Write-Host 'Prepared: openssl'
    } finally {
        Remove-StagingDirectory $stage
    }
}

function Install-ObsSdk {
    param(
        [Parameter(Mandatory = $true)][object]$Dependency,
        [Parameter(Mandatory = $true)][string]$ObsConfigPath,
        [Parameter(Mandatory = $true)][string]$ObsConfigHash
    )

    $target = Join-Path $depsRoot 'obs-sdk\obs-studio-30.0.0'
    $recipe = Get-StringSha256 @(
        'obs-sdk-v1', [string]$Dependency.sha256, $ObsConfigHash)
    $required = @(
        'libobs/obs.h',
        'libobs/obs-module.h',
        'libobs/obsconfig.h',
        'deps/w32-pthreads/pthread.h')
    if (Test-PreparedDirectory $target 'obs-sdk' $recipe $required) {
        return
    }
    $archive = Get-CachedArchive $Dependency
    $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
    try {
        Expand-ZipSafely $archive $stage
        $source = Get-ContainedPath $stage ([string]$Dependency.archiveRoot) `
            'OBS archive root'
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "OBS archive root is missing: $source"
        }
        Copy-Item -LiteralPath $ObsConfigPath `
            -Destination (Join-Path $source 'libobs\obsconfig.h')
        Write-PreparedMarker $source 'obs-sdk' ([string]$Dependency.version) `
            ([string]$Dependency.sha256) $recipe
        Move-PreparedDirectory $source $target
        Write-Host 'Prepared: obs-sdk'
    } finally {
        Remove-StagingDirectory $stage
    }
}

function Install-FfmpegSdk {
    param([Parameter(Mandatory = $true)][object]$Dependency)

    $target = Join-Path $depsRoot 'ffmpeg-sdk'
    $recipe = Get-StringSha256 @(
        'ffmpeg-sdk-v1', [string]$Dependency.sha256)
    $required = @(
        'include/libavcodec/avcodec.h',
        'lib/avcodec.lib',
        'lib/avutil.lib',
        'lib/swscale.lib',
        'lib/swresample.lib',
        'bin/avcodec-62.dll',
        'bin/avutil-60.dll',
        'bin/swscale-9.dll',
        'bin/swresample-6.dll',
        'bin/ffmpeg.exe',
        'ffmpeg-sdk-buildconf.txt')
    if (-not (Test-PreparedDirectory $target 'ffmpeg-sdk' $recipe $required)) {
        $archive = Get-CachedArchive $Dependency
        $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
        try {
            Expand-ZipSafely $archive $stage
            $source = Get-ContainedPath $stage ([string]$Dependency.archiveRoot) `
                'FFmpeg archive root'
            if (-not (Test-Path -LiteralPath $source -PathType Container)) {
                throw "FFmpeg archive root is missing: $source"
            }
            foreach ($relative in $required[0..8]) {
                $path = Get-ContainedPath $source $relative 'FFmpeg SDK file'
                if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                    throw "FFmpeg SDK layout is missing: $relative"
                }
            }
            $ffmpeg = Join-Path $source 'bin\ffmpeg.exe'
            $buildConfig = Invoke-NativeCapture $ffmpeg @('-buildconf') `
                (Split-Path -Parent $ffmpeg)
            $joinedConfig = $buildConfig -join "`n"
            if ($joinedConfig -match
                '--enable-(gpl|nonfree|libfdk_aac)(\s|$)') {
                throw 'Pinned FFmpeg archive unexpectedly enables a non-LGPL build option'
            }
            [IO.File]::WriteAllLines(
                (Join-Path $source 'ffmpeg-sdk-buildconf.txt'),
                [string[]]$buildConfig, $utf8NoBom)
            Write-PreparedMarker $source 'ffmpeg-sdk' `
                ([string]$Dependency.version) ([string]$Dependency.sha256) $recipe
            Move-PreparedDirectory $source $target
            Write-Host 'Prepared: ffmpeg-sdk'
        } finally {
            Remove-StagingDirectory $stage
        }
    }
    Copy-Item -LiteralPath (Join-Path $target 'ffmpeg-sdk-buildconf.txt') `
        -Destination (Join-Path $depsRoot 'ffmpeg-sdk-buildconf.txt') -Force
}

function Install-Libplist {
    param(
        [Parameter(Mandatory = $true)][object]$Dependency,
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string]$ConfigHash,
        [Parameter(Mandatory = $true)][string]$CmakePath,
        [Parameter(Mandatory = $true)][string]$CmakeHash,
        [Parameter(Mandatory = $true)][string]$CmakeExe
    )

    $target = Join-Path $depsRoot 'libplist-2.7.0'
    $recipe = Get-StringSha256 @(
        'libplist-v2', [string]$Dependency.sha256, $ConfigHash, $CmakeHash,
        $script:toolchainFingerprint)
    $required = @('include/plist/plist.h', 'build/plist.lib')
    if (Test-PreparedDirectory $target 'libplist' $recipe $required) {
        return
    }
    $archive = Get-CachedArchive $Dependency
    $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
    try {
        Expand-ZipSafely $archive $stage
        $source = Get-ContainedPath $stage ([string]$Dependency.archiveRoot) `
            'libplist archive root'
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "libplist archive root is missing: $source"
        }
        Copy-Item -LiteralPath $ConfigPath `
            -Destination (Join-Path $source 'config.h')
        Copy-Item -LiteralPath $CmakePath `
            -Destination (Join-Path $source 'CMakeLists.txt') -Force
        $build = Join-Path $stage 'libplist-build'
        Invoke-NativeCommand $CmakeExe @(
            '-S', $source,
            '-B', $build,
            '-G', 'NMake Makefiles',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_C_COMPILER=cl') $stage
        Invoke-NativeCommand $CmakeExe @('--build', $build) $stage
        $builtLibrary = Join-Path $build 'plist.lib'
        if (-not (Test-Path -LiteralPath $builtLibrary -PathType Leaf)) {
            throw "libplist build did not produce: $builtLibrary"
        }
        New-Item -ItemType Directory -Path (Join-Path $source 'build') | Out-Null
        Copy-Item -LiteralPath $builtLibrary `
            -Destination (Join-Path $source 'build\plist.lib')
        Write-PreparedMarker $source 'libplist' ([string]$Dependency.version) `
            ([string]$Dependency.sha256) $recipe
        Move-PreparedDirectory $source $target
        Write-Host 'Prepared: libplist'
    } finally {
        Remove-StagingDirectory $stage
    }
}

function Install-UxPlaySource {
    param(
        [Parameter(Mandatory = $true)][object]$Dependency,
        [Parameter(Mandatory = $true)][string]$PatchPath,
        [Parameter(Mandatory = $true)][string]$PatchHash,
        [Parameter(Mandatory = $true)][string]$GitExe
    )

    $target = Join-Path $depsRoot 'UxPlay'
    $recipe = Get-StringSha256 @(
        'uxplay-source-v1', [string]$Dependency.sha256,
        [string]$Dependency.commit, $PatchHash)
    $required = @(
        'lib/raop.h', 'lib/dnssd.h', 'lib/threads.h',
        'lib/playfair/playfair.c', 'lib/llhttp/llhttp.c')
    if (Test-PreparedDirectory $target 'uxplay' $recipe $required) {
        return
    }
    $archive = Get-CachedArchive $Dependency
    $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
    try {
        Expand-ZipSafely $archive $stage
        $source = Get-ContainedPath $stage ([string]$Dependency.archiveRoot) `
            'UxPlay archive root'
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "UxPlay archive root is missing: $source"
        }

        Invoke-NativeCommand $GitExe @('-C', $source, 'init', '--quiet') $stage
        $applyArgs = @('-C', $source, 'apply', '--check', $PatchPath)
        $reverseArgs = @('-C', $source, 'apply', '--reverse', '--check', $PatchPath)
        if (Test-NativeCommand $GitExe $applyArgs $stage) {
            Invoke-NativeCommand $GitExe @(
                '-C', $source, 'apply', $PatchPath) $stage
            if (-not (Test-NativeCommand $GitExe $reverseArgs $stage)) {
                throw 'UxPlay patch did not pass reverse verification after application'
            }
        } elseif (Test-NativeCommand $GitExe $reverseArgs $stage) {
            Write-Host 'UxPlay patch was already applied in staging.'
        } else {
            throw 'UxPlay patch is neither cleanly applicable nor already applied'
        }

        $gitDirectory = Join-Path $source '.git'
        if (Test-Path -LiteralPath $gitDirectory) {
            Remove-Item -LiteralPath $gitDirectory -Recurse -Force
        }
        Write-PreparedMarker $source 'uxplay' ([string]$Dependency.version) `
            ([string]$Dependency.sha256) $recipe
        Move-PreparedDirectory $source $target
        Write-Host 'Prepared: uxplay'
    } finally {
        Remove-StagingDirectory $stage
    }
}

function Install-ImportLibraries {
    param(
        [Parameter(Mandatory = $true)][string]$ObsDefPath,
        [Parameter(Mandatory = $true)][string]$ObsDefHash,
        [Parameter(Mandatory = $true)][string]$PthreadDefPath,
        [Parameter(Mandatory = $true)][string]$PthreadDefHash,
        [Parameter(Mandatory = $true)][string]$LibExe
    )

    $recipe = Get-StringSha256 @(
        'import-libs-v2', $ObsDefHash, $PthreadDefHash,
        $script:toolchainFingerprint)
    $marker = Join-Path $depsRoot '.airplay-import-libs.json'
    $obsLib = Join-Path $depsRoot 'obs.lib'
    $pthreadLib = Join-Path $depsRoot 'w32-pthreads.lib'
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
        $metadata = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
        if ([string]$metadata.recipeSha256 -ne $recipe) {
            throw 'Existing import libraries were prepared from a different recipe'
        }
        $expectedOutputs = @(
            [pscustomobject]@{
                Path = $obsLib
                Hash = [string]$metadata.obsSha256
            },
            [pscustomobject]@{
                Path = $pthreadLib
                Hash = [string]$metadata.pthreadSha256
            })
        foreach ($expected in $expectedOutputs) {
            if (-not (Test-Path -LiteralPath $expected.Path -PathType Leaf) -or
                (Get-Sha256 $expected.Path) -ne $expected.Hash) {
                throw "Prepared import library is missing or changed: $($expected.Path)"
            }
        }
        Write-Host 'Ready: import-libs'
        return
    }
    if ((Test-Path -LiteralPath $obsLib) -or
        (Test-Path -LiteralPath $pthreadLib)) {
        throw 'Unmanaged OBS import libraries already exist; refusing to overwrite them'
    }

    $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $stage | Out-Null
        $stagedObs = Join-Path $stage 'obs.lib'
        $stagedPthread = Join-Path $stage 'w32-pthreads.lib'
        Invoke-NativeCommand $LibExe @(
            "/def:$ObsDefPath", "/out:$stagedObs", '/machine:x64') $stage
        Invoke-NativeCommand $LibExe @(
            "/def:$PthreadDefPath", "/out:$stagedPthread", '/machine:x64') $stage
        if (-not (Test-Path -LiteralPath $stagedObs -PathType Leaf) -or
            -not (Test-Path -LiteralPath $stagedPthread -PathType Leaf)) {
            throw 'MSVC library manager did not produce both import libraries'
        }
        Move-Item -LiteralPath $stagedObs -Destination $obsLib
        Move-Item -LiteralPath $stagedPthread -Destination $pthreadLib
        $metadata = [ordered]@{
            schemaVersion = 1
            recipeSha256 = $recipe
            obsSha256 = Get-Sha256 $obsLib
            pthreadSha256 = Get-Sha256 $pthreadLib
            preparedUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        }
        [IO.File]::WriteAllText(
            $marker, ($metadata | ConvertTo-Json), $utf8NoBom)
        Write-Host 'Prepared: import-libs'
    } finally {
        Remove-StagingDirectory $stage
    }
}

function Install-UxPlayLibraries {
    param(
        [Parameter(Mandatory = $true)][string]$CmakeOverlayPath,
        [Parameter(Mandatory = $true)][string]$CmakeOverlayHash,
        [Parameter(Mandatory = $true)][string]$CmakeExe
    )

    $uxMarker = Get-Content -LiteralPath `
        (Join-Path $depsRoot "UxPlay\$markerName") -Raw | ConvertFrom-Json
    $plistMarker = Get-Content -LiteralPath `
        (Join-Path $depsRoot "libplist-2.7.0\$markerName") -Raw | ConvertFrom-Json
    $opensslMarker = Get-Content -LiteralPath `
        (Join-Path $depsRoot "openssl\$markerName") -Raw | ConvertFrom-Json
    $obsMarker = Get-Content -LiteralPath `
        (Join-Path $depsRoot "obs-sdk\obs-studio-30.0.0\$markerName") `
        -Raw | ConvertFrom-Json
    $recipe = Get-StringSha256 @(
        'uxplay-libraries-v2', $CmakeOverlayHash,
        [string]$uxMarker.recipeSha256,
        [string]$plistMarker.recipeSha256,
        [string]$opensslMarker.recipeSha256,
        [string]$obsMarker.recipeSha256,
        $script:toolchainFingerprint)
    $target = Join-Path $depsRoot 'uxplay-build'
    $required = @(
        'CMakeLists.txt', 'build/airplay.lib',
        'build/playfair.lib', 'build/llhttp.lib')
    if (Test-PreparedDirectory $target 'uxplay-libraries' $recipe $required) {
        return
    }

    $stage = Join-Path $stagingRoot ([Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $stage | Out-Null
        $source = Join-Path $stage 'uxplay-build'
        $build = Join-Path $stage 'uxplay-objects'
        New-Item -ItemType Directory -Path $source | Out-Null
        Copy-Item -LiteralPath $CmakeOverlayPath `
            -Destination (Join-Path $source 'CMakeLists.txt')
        Invoke-NativeCommand $CmakeExe @(
            '-S', $source,
            '-B', $build,
            '-G', 'NMake Makefiles',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_C_COMPILER=cl',
            "-DDEPS_ROOT=$depsRoot",
            "-DOPENSSL_DIR=$(Join-Path $depsRoot 'openssl')") $stage
        Invoke-NativeCommand $CmakeExe @('--build', $build) $stage
        $outputDirectory = Join-Path $source 'build'
        New-Item -ItemType Directory -Path $outputDirectory | Out-Null
        foreach ($name in @('airplay.lib', 'playfair.lib', 'llhttp.lib')) {
            $built = Join-Path $build $name
            if (-not (Test-Path -LiteralPath $built -PathType Leaf)) {
                throw "UxPlay dependency build did not produce: $built"
            }
            Copy-Item -LiteralPath $built -Destination (Join-Path $outputDirectory $name)
        }
        Write-PreparedMarker $source 'uxplay-libraries' '1.73.6' `
            ([string]$uxMarker.archiveSha256) $recipe
        Move-PreparedDirectory $source $target
        Write-Host 'Prepared: uxplay-libraries'
    } finally {
        Remove-StagingDirectory $stage
    }
}

if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
    throw "Dependency lock file is missing: $lockPath"
}
$dependencyLock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ([int]$dependencyLock.schemaVersion -ne 1) {
    throw "Unsupported dependency lock schema: $($dependencyLock.schemaVersion)"
}

foreach ($name in @('uxplay', 'obsSdk', 'ffmpegSdk', 'libplist', 'openssl')) {
    $dependency = Get-Dependency $name
    $null = Get-RequiredProperty $dependency 'version' $name
    $urlText = [string](Get-RequiredProperty $dependency 'url' $name)
    $uri = $null
    if (-not [Uri]::TryCreate($urlText, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -ne 'https') {
        throw "$name URL must be absolute HTTPS"
    }
    Assert-Sha256Text ([string](Get-RequiredProperty $dependency 'sha256' $name)) `
        "$name archive hash"
    $archiveFile = [string](Get-RequiredProperty $dependency 'archiveFile' $name)
    if ([IO.Path]::GetFileName($archiveFile) -ne $archiveFile) {
        throw "$name archive filename is unsafe: $archiveFile"
    }
    $lockedArchiveRoot = [string](
        Get-RequiredProperty $dependency 'archiveRoot' $name)
    if ([IO.Path]::GetFileName($lockedArchiveRoot) -ne $lockedArchiveRoot -or
        $lockedArchiveRoot -eq '.' -or $lockedArchiveRoot -eq '..') {
        throw "$name archive root is unsafe: $lockedArchiveRoot"
    }
}

$lockedFiles = $dependencyLock.repositoryFiles
$uxplayPatch = Assert-LockedFile `
    ([string]$lockedFiles.uxplayPatch.path) `
    ([string]$lockedFiles.uxplayPatch.sha256)
$obsConfig = Assert-LockedFile `
    ([string]$lockedFiles.obsConfig.path) `
    ([string]$lockedFiles.obsConfig.sha256)
$plistConfig = Assert-LockedFile `
    ([string]$lockedFiles.libplistConfig.path) `
    ([string]$lockedFiles.libplistConfig.sha256)
$plistCmake = Assert-LockedFile `
    ([string]$lockedFiles.libplistCmake.path) `
    ([string]$lockedFiles.libplistCmake.sha256)
$uxplayCmake = Assert-LockedFile `
    ([string]$lockedFiles.uxplayCmake.path) `
    ([string]$lockedFiles.uxplayCmake.sha256)
$obsDef = Assert-LockedFile `
    ([string]$lockedFiles.obsDef.path) `
    ([string]$lockedFiles.obsDef.sha256)
$pthreadDef = Assert-LockedFile `
    ([string]$lockedFiles.pthreadDef.path) `
    ([string]$lockedFiles.pthreadDef.sha256)

if ($ValidateOnly) {
    Write-Host 'Dependency recipe validation passed.'
    return
}

$gitExe = Get-ToolPath 'git.exe'
$cmakeExe = Get-ToolPath 'cmake.exe'
$clExe = Get-ToolPath 'cl.exe'
$nmakeExe = Get-ToolPath 'nmake.exe'
$libExe = Get-ToolPath 'lib.exe'
if ([string]$env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    throw "The dependency build requires an x64 Visual Studio Developer Command Prompt. Current VSCMD_ARG_TGT_ARCH: '$($env:VSCMD_ARG_TGT_ARCH)'"
}
$script:toolchainFingerprint = Get-StringSha256 @(
    'msvc-x64-v1',
    [string]$env:VSCMD_VER,
    [string]$env:VCToolsVersion,
    $clExe,
    (Get-Item -LiteralPath $clExe).VersionInfo.FileVersion,
    $nmakeExe,
    (Get-Item -LiteralPath $nmakeExe).VersionInfo.FileVersion,
    $libExe,
    (Get-Item -LiteralPath $libExe).VersionInfo.FileVersion,
    $cmakeExe,
    (Get-Item -LiteralPath $cmakeExe).VersionInfo.FileVersion)

New-PrivateDirectory $cacheRoot
New-PrivateDirectory $archiveRoot
New-PrivateDirectory $stagingRoot
New-PrivateDirectory $depsRoot

$runLockPath = Join-Path $cacheRoot 'prepare-deps.lock'
$runLock = $null
try {
    try {
        $runLock = [IO.File]::Open(
            $runLockPath, [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch {
        throw 'Another dependency preparation process appears to be running.'
    }

    $opensslDependency = Get-Dependency 'openssl'
    $obsDependency = Get-Dependency 'obsSdk'
    $ffmpegDependency = Get-Dependency 'ffmpegSdk'
    $plistDependency = Get-Dependency 'libplist'
    $uxplayDependency = Get-Dependency 'uxplay'

    Install-OpenSsl $opensslDependency
    Install-ObsSdk $obsDependency $obsConfig `
        ([string]$lockedFiles.obsConfig.sha256).ToUpperInvariant()
    Install-FfmpegSdk $ffmpegDependency
    Install-Libplist $plistDependency $plistConfig `
        ([string]$lockedFiles.libplistConfig.sha256).ToUpperInvariant() `
        $plistCmake `
        ([string]$lockedFiles.libplistCmake.sha256).ToUpperInvariant() `
        $cmakeExe
    Install-UxPlaySource $uxplayDependency $uxplayPatch `
        ([string]$lockedFiles.uxplayPatch.sha256).ToUpperInvariant() $gitExe
    Install-ImportLibraries $obsDef `
        ([string]$lockedFiles.obsDef.sha256).ToUpperInvariant() `
        $pthreadDef `
        ([string]$lockedFiles.pthreadDef.sha256).ToUpperInvariant() $libExe
    Install-UxPlayLibraries $uxplayCmake `
        ([string]$lockedFiles.uxplayCmake.sha256).ToUpperInvariant() $cmakeExe

    $summary = [ordered]@{
        schemaVersion = 1
        lockSha256 = Get-Sha256 $lockPath
        preparedUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        depsRoot = $depsRoot
        targetArchitecture = 'x64'
        toolchainSha256 = $script:toolchainFingerprint
        openSslDir = Join-Path $depsRoot 'openssl'
        ffmpegRoot = Join-Path $depsRoot 'ffmpeg-sdk'
    }
    [IO.File]::WriteAllText(
        (Join-Path $depsRoot 'prepared-dependencies.json'),
        ($summary | ConvertTo-Json), $utf8NoBom)
    Write-Host "Dependencies are ready under: $depsRoot"
} finally {
    if ($null -ne $runLock) {
        $runLock.Dispose()
    }
}
