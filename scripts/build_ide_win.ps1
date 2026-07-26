#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/build_ide_win.ps1
# Purpose: Build Zanna Studio as a standalone native Windows binary.
# Key invariants:
#   - Host and target Zanna trees remain distinct for cross-architecture builds.
#   - Existing CMake trees are built without mutating their generator platform.
#   - Build metadata describes the exact output and source state.
#   - The compatibility copy is skipped only when explicitly disabled.
# Ownership/Lifetime: The requested binary, metadata, and compatibility copy
#                     are owned by this invocation.
# Links: src/zannastudio/zanna.project, scripts/build_ide.sh
# Cross-platform touchpoints: Architecture aliases and metadata match the Unix
#                             driver; Windows CMake generators select x64/ARM64.
#
#===----------------------------------------------------------------------===#

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Get-EnvironmentValue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Default
    )
    $value = [Environment]::GetEnvironmentVariable($Name, "Process")
    if ([string]::IsNullOrEmpty($value)) {
        return $Default
    }
    return $value
}

function ConvertFrom-NativeArgumentString {
    param([AllowEmptyString()][string]$Value)

    $result = [Collections.Generic.List[string]]::new()
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $result.ToArray()
    }
    $current = [Text.StringBuilder]::new()
    [char]$quote = [char]0
    for ($index = 0; $index -lt $Value.Length; ++$index) {
        $character = $Value[$index]
        if ($quote -ne [char]0) {
            if ($character -eq $quote) {
                $quote = [char]0
            } elseif ($character -eq '\' -and $index + 1 -lt $Value.Length -and
                      $Value[$index + 1] -eq $quote) {
                [void]$current.Append($quote)
                ++$index
            } else {
                [void]$current.Append($character)
            }
        } elseif ($character -eq '"' -or $character -eq "'") {
            $quote = $character
        } elseif ([char]::IsWhiteSpace($character)) {
            if ($current.Length -gt 0) {
                $result.Add($current.ToString())
                [void]$current.Clear()
            }
        } else {
            [void]$current.Append($character)
        }
    }
    if ($quote -ne [char]0) {
        throw "ZANNA_EXTRA_CMAKE_ARGS contains an unterminated quoted argument."
    }
    if ($current.Length -gt 0) {
        $result.Add($current.ToString())
    }
    return $result.ToArray()
}

function Get-FullPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Base
    )
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $Base $Path))
}

function Test-PathsEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )

    return [string]::Equals(
        [IO.Path]::GetFullPath($Left).TrimEnd('\', '/'),
        [IO.Path]::GetFullPath($Right).TrimEnd('\', '/'),
        [StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeAutomationPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        $Path.IndexOfAny([char[]](0..31)) -ge 0) {
        throw "$Description contains an empty or control-character path."
    }
}

function Assert-ArtifactDestination {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    Assert-SafeAutomationPath -Path $Path -Description $Description
    if ((Test-Path -LiteralPath $Path) -and
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is not a regular file destination: $Path"
    }
    $current = $Path
    $isLeaf = $true
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description traverses a reparse point: $current"
            }
            $linkTypeProperty = $item.PSObject.Properties["LinkType"]
            if ($isLeaf -and $linkTypeProperty -and
                $linkTypeProperty.Value -eq "HardLink") {
                throw "$Description must not replace a hard-link alias: $current"
            }
        }
        $parent = Split-Path -Parent $current
        if ([string]::IsNullOrWhiteSpace($parent) -or
            [string]::Equals($parent, $current, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent
        $isLeaf = $false
    }
}

function Assert-NoReparseTree {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "$Description is missing or is not a directory: $Root"
    }
    $rootItem = Get-Item -LiteralPath $Root -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description is a reparse point: $Root"
    }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootItem.FullName)
    $visitedEntries = 0
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
            ++$visitedEntries
            if ($visitedEntries -gt 250000) {
                throw "$Description exceeds the bounded tree-entry limit: $Root"
            }
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) {
                $pending.Push($entry.FullName)
            }
        }
    }
}

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit $LASTEXITCODE)"
    }
}

function Resolve-ZannaExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Tree,
        [Parameter(Mandatory = $true)][string]$Configuration
    )

    $configured = Join-Path $Tree "src\tools\zanna\$Configuration\zanna.exe"
    $singleConfig = Join-Path $Tree "src\tools\zanna\zanna.exe"
    if (Test-Path -LiteralPath $configured -PathType Leaf) {
        return $configured
    }
    if (Test-Path -LiteralPath $singleConfig -PathType Leaf) {
        return $singleConfig
    }
    return $configured
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)][string]$Cache,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $prefix = "${Name}:"
    foreach ($line in @(Read-BoundedUtf8Lines -Path $Cache `
            -Description "CMake cache" -MaximumBytes 8388608)) {
        if ($line.StartsWith($prefix, [StringComparison]::Ordinal)) {
            $separator = $line.IndexOf('=')
            if ($separator -ge 0) {
                return $line.Substring($separator + 1).Trim()
            }
        }
    }
    return ""
}

function Read-BoundedUtf8Lines {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][int64]$MaximumBytes
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing or is not an ordinary file: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.Length -lt 0 -or $item.Length -gt $MaximumBytes) {
        throw "$Description has an unsafe shape or size: $Path"
    }
    $stream = [IO.File]::Open(
        $item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -gt $MaximumBytes) {
            throw "$Description changed to an oversized file: $Path"
        }
        $reader = [IO.StreamReader]::new(
            $stream, [Text.UTF8Encoding]::new($false, $true), $true, 4096, $true)
        try {
            $lines = [Collections.Generic.List[string]]::new()
            while (-not $reader.EndOfStream) {
                $line = $reader.ReadLine()
                if ($null -eq $line -or $line.Length -gt 65536 -or
                    $line.IndexOf([char]0) -ge 0) {
                    throw "$Description contains an invalid or oversized line: $Path"
                }
                $lines.Add($line)
            }
            return $lines.ToArray()
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-CMakeGeneratedSystemProcessor {
    param([Parameter(Mandatory = $true)][string]$Cache)

    $tree = Split-Path -Parent ([IO.Path]::GetFullPath($Cache))
    $cmakeFiles = Join-Path $tree "CMakeFiles"
    if (-not (Test-Path -LiteralPath $cmakeFiles -PathType Container)) {
        return ""
    }
    $rootItem = Get-Item -LiteralPath $cmakeFiles -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "CMake metadata root is a reparse point: $cmakeFiles"
    }
    $reported = @{}
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($rootItem.FullName)
    $visitedEntries = 0
    $systemFileCount = 0
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
            ++$visitedEntries
            if ($visitedEntries -gt 250000) {
                throw "CMake metadata tree exceeds the bounded entry limit: $tree"
            }
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "CMake metadata tree contains a reparse point: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) {
                $pending.Push($entry.FullName)
                continue
            }
            if (-not [string]::Equals(
                    $entry.Name, "CMakeSystem.cmake", [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            ++$systemFileCount
            if ($systemFileCount -gt 16) {
                throw "CMake tree has too many generated system descriptions: $tree"
            }
            foreach ($line in @(Read-BoundedUtf8Lines -Path $entry.FullName `
                    -Description "CMake generated system description" -MaximumBytes 65536)) {
                if ($line -match '^\s*set\(CMAKE_SYSTEM_PROCESSOR\s+"?([^"\s\)]+)"?\s*\)\s*$') {
                    $reported[$Matches[1].ToLowerInvariant()] = $Matches[1]
                }
            }
        }
    }
    if ($reported.Count -gt 1) {
        throw "CMake tree contains conflicting generated system architectures: $tree"
    }
    if ($reported.Count -eq 1) {
        return [string]($reported.Values | Select-Object -First 1)
    }
    return ""
}

function Assert-CMakeTreeArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$Cache,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $reported = Get-CMakeCacheValue -Cache $Cache -Name "CMAKE_GENERATOR_PLATFORM"
    if ([string]::IsNullOrWhiteSpace($reported)) {
        $reported = Get-CMakeCacheValue -Cache $Cache -Name "CMAKE_SYSTEM_PROCESSOR"
    }
    if ([string]::IsNullOrWhiteSpace($reported)) {
        $reported = Get-CMakeGeneratedSystemProcessor -Cache $Cache
    }
    if ([string]::IsNullOrWhiteSpace($reported)) {
        throw "$Description CMake tree cannot prove its target architecture: $Cache"
    }
    $normalized = switch ($reported.ToLowerInvariant()) {
        "arm64" { "arm64" }
        "aarch64" { "arm64" }
        "x64" { "x64" }
        "amd64" { "x64" }
        "x86_64" { "x64" }
        default { "" }
    }
    if ([string]::IsNullOrWhiteSpace($normalized)) {
        throw "$Description CMake tree reports unsupported architecture '$reported': $Cache"
    }
    if ($normalized -ne $Architecture) {
        throw "$Description CMake tree targets $reported, not requested architecture $Architecture`: $Cache"
    }
}

function Assert-PortableExecutableArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)][ValidateSet("arm64", "x64")][string]$Architecture
    )

    Assert-ZannaPeImageArchitecture -Binary $Binary -Architecture $Architecture
}

function Show-Usage {
    Write-Host "Usage: build_ide_win.ps1 [--clean] [--arch arm64|x64] [--output PATH]"
    Write-Host "  --clean        Remove the existing Zanna Studio binary before building"
    Write-Host "  --arch         Target architecture (default: host, or ZANNA_IDE_ARCH)"
    Write-Host "  --output PATH  Write the binary to PATH (default: src\zannastudio\bin\zannastudio.exe)"
    Write-Host "  Compatibility copy: build\zannastudio\zannastudio.exe unless ZANNA_IDE_SKIP_COMPAT_COPY=1"
}

$invocationRoot = (Get-Location).Path
$clean = $false
$requestedArch = [Environment]::GetEnvironmentVariable("ZANNA_IDE_ARCH", "Process")
if ([string]::IsNullOrWhiteSpace($requestedArch)) {
    $requestedArch = [Environment]::GetEnvironmentVariable("ZANNA_DEMO_ARCH", "Process")
}
$outputOverride = [Environment]::GetEnvironmentVariable("ZANNA_IDE_OUTPUT", "Process")
$argumentIndex = 0
while ($argumentIndex -lt $args.Count) {
    $argument = [string]$args[$argumentIndex]
    switch -CaseSensitive ($argument) {
        "--clean" {
            $clean = $true
            ++$argumentIndex
        }
        "--arch" {
            if ($argumentIndex + 1 -ge $args.Count) {
                Write-Error "--arch requires arm64 or x64"
                exit 1
            }
            $requestedArch = [string]$args[$argumentIndex + 1]
            $argumentIndex += 2
        }
        "--output" {
            if ($argumentIndex + 1 -ge $args.Count) {
                Write-Error "--output requires a path"
                exit 1
            }
            $outputOverride = [string]$args[$argumentIndex + 1]
            $argumentIndex += 2
        }
        { $_ -eq "-h" -or $_ -eq "--help" } {
            Show-Usage
            exit 0
        }
        default {
            Write-Error "Unknown argument: $argument"
            Show-Usage
            exit 1
        }
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot ".."))
. (Join-Path $scriptRoot "windows_pe_validation.ps1")
$ideDir = Join-Path $repoRoot "src\zannastudio"

$buildDirSetting = [Environment]::GetEnvironmentVariable("ZANNA_IDE_BUILD_DIR", "Process")
$buildDirExplicit = -not [string]::IsNullOrWhiteSpace($buildDirSetting)
if (-not $buildDirExplicit) {
    $buildDirSetting = [Environment]::GetEnvironmentVariable("ZANNA_BUILD_DIR", "Process")
    $buildDirExplicit = -not [string]::IsNullOrWhiteSpace($buildDirSetting)
}
if (-not $buildDirExplicit) {
    $buildDirSetting = Join-Path $repoRoot "build"
}

$toolBuildDirSetting = [Environment]::GetEnvironmentVariable("ZANNA_IDE_TOOL_BUILD_DIR", "Process")
$toolBuildDirExplicit = -not [string]::IsNullOrWhiteSpace($toolBuildDirSetting)
if (-not $toolBuildDirExplicit) {
    $toolBuildDirSetting = Join-Path $repoRoot "build"
}

$buildType = Get-EnvironmentValue -Name "ZANNA_BUILD_TYPE" -Default "Release"
$buildTypeInput = $buildType
switch ($buildType.ToLowerInvariant()) {
    "debug" { $buildType = "Debug" }
    "release" { $buildType = "Release" }
    "relwithdebinfo" { $buildType = "RelWithDebInfo" }
    "minsizerel" { $buildType = "MinSizeRel" }
    default {
        throw "ZANNA_BUILD_TYPE must be Debug, Release, RelWithDebInfo, or MinSizeRel; received '$buildTypeInput'."
    }
}
$jobsValue = Get-EnvironmentValue -Name "JOBS" `
    -Default (Get-EnvironmentValue -Name "NUMBER_OF_PROCESSORS" -Default "8")
$jobs = 0
if (-not [int]::TryParse($jobsValue, [ref]$jobs) -or $jobs -lt 1 -or $jobs -gt 1024) {
    throw "JOBS must be an integer from 1 through 1024; received '$jobsValue'."
}

$nativeArchitecture = [Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITEW6432", "Process")
if ([string]::IsNullOrWhiteSpace($nativeArchitecture)) {
    $nativeArchitecture = [Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE", "Process")
}
if ([string]::IsNullOrWhiteSpace($nativeArchitecture)) {
    throw "Cannot determine the native Windows host architecture."
}
$hostArch = switch ($nativeArchitecture.ToLowerInvariant()) {
    "arm64" { "arm64" }
    "amd64" { "x64" }
    "x86_64" { "x64" }
    default {
        throw "Unsupported native Windows host architecture '$nativeArchitecture'."
    }
}
if ([string]::IsNullOrWhiteSpace($requestedArch)) {
    $requestedArch = $hostArch
}
switch ($requestedArch.ToLowerInvariant()) {
    "aarch64" { $ideArch = "arm64" }
    "arm64" { $ideArch = "arm64" }
    "amd64" { $ideArch = "x64" }
    "x86_64" { $ideArch = "x64" }
    "x64" { $ideArch = "x64" }
    default {
        Write-Error "Invalid IDE architecture '$requestedArch'; expected arm64 or x64."
        exit 1
    }
}

if (-not $buildDirExplicit -and $ideArch -ne $hostArch) {
    $buildDirSetting = Join-Path $repoRoot "build-$ideArch"
}
if (-not $toolBuildDirExplicit -and $ideArch -eq $hostArch) {
    $toolBuildDirSetting = $buildDirSetting
}

$buildDir = Get-FullPath -Path $buildDirSetting -Base $repoRoot
$toolBuildDir = Get-FullPath -Path $toolBuildDirSetting -Base $repoRoot
$ideBinDirSetting = Get-EnvironmentValue -Name "ZANNA_IDE_OUT_DIR" `
    -Default (Join-Path $ideDir "bin")
$ideBinDir = Get-FullPath -Path $ideBinDirSetting -Base $invocationRoot
if ([string]::IsNullOrWhiteSpace($outputOverride)) {
    $outputFile = Join-Path $ideBinDir "zannastudio.exe"
} else {
    $outputFile = Get-FullPath -Path $outputOverride -Base $invocationRoot
}

$compatSetting = [Environment]::GetEnvironmentVariable("ZANNA_IDE_COMPAT_OUTPUT", "Process")
if ([string]::IsNullOrWhiteSpace($compatSetting)) {
    $compatOutput = Join-Path $buildDir "zannastudio\zannastudio.exe"
} else {
    $compatOutput = Get-FullPath -Path $compatSetting -Base $invocationRoot
}
$skipCompatCopy = Get-EnvironmentValue -Name "ZANNA_IDE_SKIP_COMPAT_COPY" -Default "0"
if ($skipCompatCopy -notin @("0", "1")) {
    throw "ZANNA_IDE_SKIP_COMPAT_COPY must be 0 or 1; received '$skipCompatCopy'."
}
$zanna = Resolve-ZannaExecutable -Tree $toolBuildDir -Configuration $buildType
$targetZanna = Resolve-ZannaExecutable -Tree $buildDir -Configuration $buildType
if ($ideArch -ne $hostArch -and (Test-PathsEqual -Left $toolBuildDir -Right $buildDir)) {
    throw "Cross-architecture Studio builds require distinct host-tool and target-runtime CMake trees."
}

function Ensure-ZannaBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Tree,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][bool]$TreeIsExplicit,
        [Parameter(Mandatory = $true)][string]$Description
    )

    Write-Host "$Description not found in $Tree"
    $cache = Join-Path $Tree "CMakeCache.txt"
    if (Test-Path -LiteralPath $cache -PathType Leaf) {
        Assert-CMakeTreeArchitecture -Cache $cache -Architecture $Architecture `
            -Description $Description
        Write-Host "Reusing the existing CMake configuration for $Description."
    } else {
        Write-Host "Configuring $Description..."
        $configureArguments = @("-S", $repoRoot, "-B", $Tree)
        $generator = [Environment]::GetEnvironmentVariable("ZANNA_CMAKE_GENERATOR", "Process")
        if (-not [string]::IsNullOrWhiteSpace($generator)) {
            $configureArguments += @("-G", $generator)
            if ($generator.StartsWith("Visual Studio ", [StringComparison]::OrdinalIgnoreCase)) {
                $cmakeArch = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
                $configureArguments += @("-A", $cmakeArch)
            }
        } else {
            $cmakeArch = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
            $configureArguments += @("-A", $cmakeArch)
        }
        $configureArguments += "-DCMAKE_BUILD_TYPE=$buildType"
        $configureArguments += @(ConvertFrom-NativeArgumentString -Value `
            ([Environment]::GetEnvironmentVariable("ZANNA_EXTRA_CMAKE_ARGS", "Process")))
        Invoke-CheckedNative -FilePath "cmake" -Arguments $configureArguments `
            -FailureMessage "CMake configuration failed for $Description"
        if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
            throw "CMake did not produce an expected cache for $Description`: $cache"
        }
        Assert-CMakeTreeArchitecture -Cache $cache -Architecture $Architecture `
            -Description $Description
    }
    Write-Host "Building $Description..."
    Invoke-CheckedNative -FilePath "cmake" `
        -Arguments @("--build", $Tree, "--config", $buildType, "--target", "zanna", "-j", [string]$jobs) `
        -FailureMessage "$Description build failed"
    Assert-CMakeTreeArchitecture -Cache $cache -Architecture $Architecture `
        -Description $Description
    $resolvedExecutable =
        Resolve-ZannaExecutable -Tree $Tree -Configuration $buildType
    if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
        throw "$Description still not found in $Tree"
    }
    Assert-PortableExecutableArchitecture -Binary $resolvedExecutable `
        -Architecture $Architecture
}

function Write-StagedBuildInfo {
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)][string]$PublishedBinary,
        [Parameter(Mandatory = $true)][string]$MetadataPath,
        [Parameter(Mandatory = $true)][ValidateSet("arm64", "x64")][string]$Architecture,
        [Parameter(Mandatory = $true)][string]$ZannaExecutable
    )

    $versionPath = Join-Path $repoRoot "src\buildmeta\VERSION"
    $version = if (Test-Path -LiteralPath $versionPath -PathType Leaf) {
        $versionLines = @(Read-BoundedUtf8Lines -Path $versionPath `
                -Description "Zanna version metadata" -MaximumBytes 4096)
        if ($versionLines.Count -gt 0) {
            ([string]$versionLines[0]).Trim()
        } else {
            "unknown"
        }
    } else {
        "unknown"
    }
    if ([string]::IsNullOrWhiteSpace($version)) {
        $version = "unknown"
    }
    $diffStatus = 1
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $revision = (& git -C $repoRoot rev-parse --short HEAD 2>$null | Select-Object -First 1)
        $statusLines = @(& git -C $repoRoot status --porcelain --untracked-files=normal 2>$null)
        if ($LASTEXITCODE -eq 0) {
            $diffStatus = if ($statusLines.Count -eq 0) { 0 } else { 1 }
        }
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ([string]::IsNullOrWhiteSpace($revision)) {
        $revision = "unknown"
    }
    $dirty = if ($diffStatus -eq 0) { "" } else { " dirty" }
    $binaryItem = Get-Item -LiteralPath $Binary
    if ($binaryItem.Length -le 0) {
        throw "Zanna Studio output is empty: $Binary"
    }
    $sha256 = (Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash.ToLowerInvariant()
    $lines = @(
        "Zanna Studio $version",
        "Schema: 1",
        "Build: $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture))",
        "Source: $revision$dirty",
        "Architecture: $Architecture",
        "Size: $($binaryItem.Length)",
        "SHA256: $sha256",
        "Output: $PublishedBinary",
        "Zanna: $ZannaExecutable"
    )
    $metadataText = [string]::Join([Environment]::NewLine, $lines) +
        [Environment]::NewLine
    $metadataBytes = [Text.UTF8Encoding]::new($false).GetBytes($metadataText)
    $metadataStream = [IO.File]::Open(
        $MetadataPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $metadataStream.Write($metadataBytes, 0, $metadataBytes.Length)
        $metadataStream.Flush($true)
    } finally {
        $metadataStream.Dispose()
    }
}

function Publish-StudioArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$StagedBinary,
        [Parameter(Mandatory = $true)][string]$StagedMetadata,
        [Parameter(Mandatory = $true)][string]$DestinationBinary,
        [Parameter(Mandatory = $true)][string]$DestinationMetadata,
        [Parameter(Mandatory = $true)]
        [ValidateSet("arm64", "x64")]
        [string]$Architecture
    )

    Assert-ArtifactDestination -Path $StagedBinary -Description "Staged Studio executable"
    Assert-ArtifactDestination -Path $StagedMetadata -Description "Staged Studio metadata"
    Assert-PortableExecutableArchitecture -Binary $StagedBinary -Architecture $Architecture
    $stagedBinaryHash = (Get-FileHash -LiteralPath $StagedBinary -Algorithm SHA256).Hash
    $stagedMetadataHash = (Get-FileHash -LiteralPath $StagedMetadata -Algorithm SHA256).Hash
    $token = [Guid]::NewGuid().ToString("N")
    $binaryBackup = "$DestinationBinary.zanna-backup-$token"
    $metadataBackup = "$DestinationMetadata.zanna-backup-$token"
    foreach ($backup in @($binaryBackup, $metadataBackup)) {
        Assert-ArtifactDestination -Path $backup -Description "Studio rollback path"
        if (Test-Path -LiteralPath $backup) {
            throw "Cannot allocate a private Studio rollback path: $backup"
        }
    }
    $hadBinary = Test-Path -LiteralPath $DestinationBinary -PathType Leaf
    $hadMetadata = Test-Path -LiteralPath $DestinationMetadata -PathType Leaf
    $publishedBinary = $false
    $publishedMetadata = $false
    $publicationSucceeded = $false
    try {
        if ($hadBinary) {
            Move-Item -LiteralPath $DestinationBinary -Destination $binaryBackup
        }
        if ($hadMetadata) {
            Move-Item -LiteralPath $DestinationMetadata -Destination $metadataBackup
        }
        Move-Item -LiteralPath $StagedBinary -Destination $DestinationBinary
        $publishedBinary = $true
        Move-Item -LiteralPath $StagedMetadata -Destination $DestinationMetadata
        $publishedMetadata = $true
        Assert-ArtifactDestination -Path $DestinationBinary `
            -Description "Published Studio executable"
        Assert-ArtifactDestination -Path $DestinationMetadata `
            -Description "Published Studio metadata"
        Assert-PortableExecutableArchitecture `
            -Binary $DestinationBinary -Architecture $Architecture
        if ((Get-FileHash -LiteralPath $DestinationBinary -Algorithm SHA256).Hash -cne
                $stagedBinaryHash -or
            (Get-FileHash -LiteralPath $DestinationMetadata -Algorithm SHA256).Hash -cne
                $stagedMetadataHash) {
            throw "Published Studio artifact pair changed during publication."
        }
        $publicationSucceeded = $true
    } catch {
        if ($publishedMetadata -and
            (Test-Path -LiteralPath $DestinationMetadata -PathType Leaf)) {
            Remove-Item -LiteralPath $DestinationMetadata -Force
        }
        if ($publishedBinary -and
            (Test-Path -LiteralPath $DestinationBinary -PathType Leaf)) {
            Remove-Item -LiteralPath $DestinationBinary -Force
        }
        if ($hadMetadata -and (Test-Path -LiteralPath $metadataBackup -PathType Leaf)) {
            Move-Item -LiteralPath $metadataBackup -Destination $DestinationMetadata
        }
        if ($hadBinary -and (Test-Path -LiteralPath $binaryBackup -PathType Leaf)) {
            Move-Item -LiteralPath $binaryBackup -Destination $DestinationBinary
        }
        throw
    } finally {
        if ($publicationSucceeded) {
            foreach ($backup in @($binaryBackup, $metadataBackup)) {
                if (Test-Path -LiteralPath $backup -PathType Leaf) {
                    Remove-Item -LiteralPath $backup -Force
                }
            }
        }
    }
}

$outputDir = Split-Path -Parent $outputFile
$outputMetadata = Join-Path $outputDir "zannastudio.buildinfo"
$compatDir = Split-Path -Parent $compatOutput
$compatMetadata = Join-Path $compatDir "zannastudio.buildinfo"
$automationPaths = @(
    $buildDir,
    $toolBuildDir,
    $ideBinDir,
    $outputFile,
    $outputMetadata,
    $zanna,
    $targetZanna
)
$artifactPaths = @($outputFile, $outputMetadata)
if ($skipCompatCopy -eq "0" -and
    -not (Test-PathsEqual -Left $outputFile -Right $compatOutput)) {
    $automationPaths += @($compatOutput, $compatMetadata)
    $artifactPaths += @($compatOutput, $compatMetadata)
}
foreach ($automationPath in $automationPaths) {
    Assert-SafeAutomationPath -Path $automationPath -Description "Zanna Studio automation path"
}
foreach ($pathEntry in $artifactPaths) {
    Assert-ArtifactDestination -Path $pathEntry -Description "Zanna Studio output"
}
foreach ($protectedPath in @(
        (Join-Path $ideDir "zanna.project"),
        $zanna,
        $targetZanna,
        (Join-Path $toolBuildDir "src\tools\zanna\zanna.exe"),
        (Join-Path $buildDir "src\tools\zanna\zanna.exe"),
        $outputMetadata)) {
    if (Test-PathsEqual -Left $outputFile -Right $protectedPath) {
        throw "Zanna Studio output collides with a protected input or metadata path: $outputFile"
    }
}
if ($skipCompatCopy -eq "0" -and
    -not (Test-PathsEqual -Left $outputFile -Right $compatOutput)) {
    $collisionPairs = @(
        [pscustomobject]@{ Left = $compatOutput; Right = $outputMetadata },
        [pscustomobject]@{ Left = $compatMetadata; Right = $outputFile },
        [pscustomobject]@{ Left = $compatMetadata; Right = $outputMetadata },
        [pscustomobject]@{ Left = $compatOutput; Right = $zanna },
        [pscustomobject]@{ Left = $compatOutput; Right = $targetZanna }
    )
    foreach ($pair in $collisionPairs) {
        if (Test-PathsEqual -Left $pair.Left -Right $pair.Right) {
            throw "Zanna Studio compatibility output collides with another artifact: $($pair.Left)"
        }
    }
}

$temporaryPaths = [Collections.Generic.List[string]]::new()
$previousBuildDir = [Environment]::GetEnvironmentVariable("ZANNA_BUILD_DIR", "Process")
$env:ZANNA_BUILD_DIR = $buildDir
$locationPushed = $false
try {
    Push-Location $repoRoot
    $locationPushed = $true
    Write-Host "Building Zanna Studio as a native $ideArch binary"
    Write-Host "=============================================="
    Write-Host ""
    Write-Host "Using Zanna tool: $toolBuildDir"
    Write-Host "Using target runtime build: $buildDir"
    Write-Host "Source: $ideDir"
    Write-Host "Output: $outputFile"
    Write-Host ""

    if (-not (Test-Path -LiteralPath (Join-Path $ideDir "zanna.project") -PathType Leaf)) {
        throw "No zanna.project found in $ideDir"
    }
    Assert-NoReparseTree -Root $ideDir -Description "Zanna Studio source tree"
    $toolCache = Join-Path $toolBuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $toolCache -PathType Leaf) {
        Assert-CMakeTreeArchitecture -Cache $toolCache -Architecture $hostArch `
            -Description "host Zanna tool"
    } elseif (Test-Path -LiteralPath $zanna -PathType Leaf) {
        throw "Host Zanna executable has no CMake cache proving its architecture: $zanna"
    }
    $targetCache = Join-Path $buildDir "CMakeCache.txt"
    if (-not (Test-PathsEqual -Left $toolBuildDir -Right $buildDir) -and
        (Test-Path -LiteralPath $targetCache -PathType Leaf)) {
        Assert-CMakeTreeArchitecture -Cache $targetCache -Architecture $ideArch `
            -Description "target-architecture Zanna runtime"
    } elseif (-not (Test-PathsEqual -Left $toolBuildDir -Right $buildDir) -and
              (Test-Path -LiteralPath $targetZanna -PathType Leaf)) {
        throw "Target Zanna executable has no CMake cache proving its architecture: $targetZanna"
    }
    if (-not (Test-Path -LiteralPath $zanna -PathType Leaf)) {
        Ensure-ZannaBuild -Tree $toolBuildDir -Architecture $hostArch `
            -TreeIsExplicit $toolBuildDirExplicit `
            -Description "host Zanna tool"
        $zanna = Resolve-ZannaExecutable -Tree $toolBuildDir -Configuration $buildType
    }
    Assert-PortableExecutableArchitecture -Binary $zanna -Architecture $hostArch
    if (-not (Test-PathsEqual -Left $toolBuildDir -Right $buildDir) -and
        -not (Test-Path -LiteralPath $targetZanna -PathType Leaf)) {
        Ensure-ZannaBuild -Tree $buildDir -Architecture $ideArch `
            -TreeIsExplicit $buildDirExplicit `
            -Description "target-architecture Zanna runtime"
        $targetZanna = Resolve-ZannaExecutable -Tree $buildDir -Configuration $buildType
    }
    if (-not (Test-PathsEqual -Left $toolBuildDir -Right $buildDir)) {
        Assert-PortableExecutableArchitecture -Binary $targetZanna -Architecture $ideArch
    }

    [void](New-Item -ItemType Directory -Path $outputDir -Force)
    if ($skipCompatCopy -eq "0" -and
        -not (Test-PathsEqual -Left $outputFile -Right $compatOutput)) {
        [void](New-Item -ItemType Directory -Path $compatDir -Force)
    }
    if ($clean) {
        Write-Host "Cleaning existing Zanna Studio artifacts..."
        $cleanPaths = @($outputFile, $outputMetadata)
        if ($skipCompatCopy -eq "0" -and
            -not (Test-PathsEqual -Left $outputFile -Right $compatOutput)) {
            $cleanPaths += @($compatOutput, $compatMetadata)
        }
        foreach ($path in $cleanPaths) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                Remove-Item -LiteralPath $path -Force
            }
        }
    }

    $buildToken = [Guid]::NewGuid().ToString("N")
    $stagedOutput = Join-Path $outputDir ".zannastudio-$PID-$buildToken.tmp.exe"
    $stagedMetadata = Join-Path $outputDir ".zannastudio-$PID-$buildToken.tmp.buildinfo"
    foreach ($stagedPath in @($stagedOutput, $stagedMetadata)) {
        Assert-ArtifactDestination -Path $stagedPath -Description "Studio staging path"
        if (Test-Path -LiteralPath $stagedPath) {
            throw "Cannot allocate a private Studio staging path: $stagedPath"
        }
    }
    $temporaryPaths.Add($stagedOutput)
    $temporaryPaths.Add($stagedMetadata)
    $buildArguments = @("build", $ideDir, "--arch", $ideArch, "-o", $stagedOutput)
    Write-Host "Compiling..."
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 promotes redirected native stderr to error
        # records even when Zanna is only reporting normal linker progress.
        $ErrorActionPreference = "Continue"
        $compilerOutput = @(& $zanna @buildArguments 2>&1)
        $buildStatus = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    $compilerOutput | ForEach-Object { Write-Host $_ }
    if ($buildStatus -ne 0) {
        throw "Zanna Studio compilation failed (exit $buildStatus)."
    }

    Assert-PortableExecutableArchitecture -Binary $stagedOutput -Architecture $ideArch
    Write-StagedBuildInfo -Binary $stagedOutput -PublishedBinary $outputFile `
        -MetadataPath $stagedMetadata -Architecture $ideArch -ZannaExecutable $zanna
    Publish-StudioArtifact -StagedBinary $stagedOutput -StagedMetadata $stagedMetadata `
        -DestinationBinary $outputFile -DestinationMetadata $outputMetadata `
        -Architecture $ideArch
    Write-Host "OK"
    if ($skipCompatCopy -eq "0" -and
        -not (Test-PathsEqual -Left $outputFile -Right $compatOutput)) {
        $compatToken = [Guid]::NewGuid().ToString("N")
        $stagedCompat =
            Join-Path $compatDir ".zannastudio-compat-$PID-$compatToken.tmp.exe"
        $stagedCompatMetadata =
            Join-Path $compatDir ".zannastudio-compat-$PID-$compatToken.tmp.buildinfo"
        foreach ($stagedPath in @($stagedCompat, $stagedCompatMetadata)) {
            Assert-ArtifactDestination -Path $stagedPath `
                -Description "Studio compatibility staging path"
            if (Test-Path -LiteralPath $stagedPath) {
                throw "Cannot allocate a private Studio compatibility staging path: $stagedPath"
            }
        }
        $temporaryPaths.Add($stagedCompat)
        $temporaryPaths.Add($stagedCompatMetadata)
        Copy-Item -LiteralPath $outputFile -Destination $stagedCompat
        Assert-PortableExecutableArchitecture -Binary $stagedCompat -Architecture $ideArch
        Write-StagedBuildInfo -Binary $stagedCompat -PublishedBinary $compatOutput `
            -MetadataPath $stagedCompatMetadata -Architecture $ideArch `
            -ZannaExecutable $zanna
        Publish-StudioArtifact -StagedBinary $stagedCompat `
            -StagedMetadata $stagedCompatMetadata -DestinationBinary $compatOutput `
            -DestinationMetadata $compatMetadata -Architecture $ideArch
        Write-Host "Compatibility copy: $compatOutput"
    }
    Write-Host "Built: $outputFile"
    Write-Host "Build info: $outputMetadata"
} finally {
    foreach ($temporaryPath in $temporaryPaths) {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
    if ($locationPushed) {
        Pop-Location
    }
    [Environment]::SetEnvironmentVariable("ZANNA_BUILD_DIR", $previousBuildDir, "Process")
}
