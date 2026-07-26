#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/build_demos_win.ps1
# Purpose: Build, stage, and optionally smoke-run every curated Zia showcase demo on Windows.
# Key invariants:
#   - The shared demo manifest is the single project inventory.
#   - Host and target tool trees remain distinct for cross-architecture builds.
#   - Existing CMake trees are built without mutating their generator platform.
#   - Build, asset-stage, and requested launch failures contribute to the final exit code.
#   - Executables and assets publish together from a complete private directory.
#   - Smoke output and all process-tree waits are bounded.
# Ownership/Lifetime: Each validated binary and its assets own one examples/bin/<demo> directory;
#                     each smoke launch and its logs live in a private temporary directory.
# Links: scripts/demo_projects.list, scripts/build_demos.sh
# Cross-platform touchpoints: Architecture aliases match the Unix demo driver;
#                             CMake's Windows generator selects x64 or ARM64.
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

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Base,
        [Parameter(Mandatory = $true)][string]$Candidate,
        [switch]$AllowBase
    )

    $baseFull = [IO.Path]::GetFullPath($Base).TrimEnd('\', '/')
    $candidateFull = [IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    if ($AllowBase -and
        [string]::Equals($baseFull, $candidateFull, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $prefix = $baseFull + [IO.Path]::DirectorySeparatorChar
    return $candidateFull.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
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

function Show-Usage {
    Write-Host "Usage: build_demos_win.ps1 [--clean] [--run|--skip-run] [--arch arm64|x64]"
    Write-Host "  --clean      Remove existing binaries before building"
    Write-Host "  --run        Launch each built demo for smoke validation"
    Write-Host "  --skip-run   Build only; skip launch validation (default)"
    Write-Host "  --arch       Target architecture (default: host, or ZANNA_DEMO_ARCH)"
}

$clean = $false
$run = $false
$requestedArch = [Environment]::GetEnvironmentVariable("ZANNA_DEMO_ARCH", "Process")
$argumentIndex = 0
while ($argumentIndex -lt $args.Count) {
    $argument = [string]$args[$argumentIndex]
    switch -CaseSensitive ($argument) {
        "--clean" {
            $clean = $true
            ++$argumentIndex
        }
        "--run" {
            $run = $true
            ++$argumentIndex
        }
        "--skip-run" {
            $run = $false
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
$buildDirSetting = [Environment]::GetEnvironmentVariable("ZANNA_DEMO_BUILD_DIR", "Process")
$buildDirExplicit = -not [string]::IsNullOrWhiteSpace($buildDirSetting)
if (-not $buildDirExplicit) {
    $buildDirSetting = [Environment]::GetEnvironmentVariable("ZANNA_BUILD_DIR", "Process")
    $buildDirExplicit = -not [string]::IsNullOrWhiteSpace($buildDirSetting)
}
if (-not $buildDirExplicit) {
    $buildDirSetting = Join-Path $repoRoot "build"
}

$toolBuildDirSetting = [Environment]::GetEnvironmentVariable("ZANNA_DEMO_TOOL_BUILD_DIR", "Process")
$toolBuildDirExplicit = -not [string]::IsNullOrWhiteSpace($toolBuildDirSetting)
if (-not $toolBuildDirExplicit) {
    $toolBuildDirSetting = Join-Path $repoRoot "build"
}

$buildType = Get-EnvironmentValue -Name "ZANNA_BUILD_TYPE" -Default "Debug"
if ($buildType -notin @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
    throw "ZANNA_BUILD_TYPE must be Debug, Release, RelWithDebInfo, or MinSizeRel; received '$buildType'."
}
$jobsValue = Get-EnvironmentValue -Name "JOBS" `
    -Default (Get-EnvironmentValue -Name "NUMBER_OF_PROCESSORS" -Default "8")
$jobs = 0
if (-not [int]::TryParse($jobsValue, [ref]$jobs) -or $jobs -lt 1) {
    throw "JOBS must be a positive integer; received '$jobsValue'."
}

$nativeArchitecture = [Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITEW6432", "Process")
if ([string]::IsNullOrWhiteSpace($nativeArchitecture)) {
    $nativeArchitecture = [Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE", "Process")
}
$hostArch = switch ($nativeArchitecture.ToUpperInvariant()) {
    "ARM64" { "arm64" }
    "AMD64" { "x64" }
    default {
        throw "Unsupported native Windows host architecture '$nativeArchitecture'."
    }
}
if ([string]::IsNullOrWhiteSpace($requestedArch)) {
    $requestedArch = $hostArch
}
switch ($requestedArch.ToLowerInvariant()) {
    "aarch64" { $demoArch = "arm64" }
    "arm64" { $demoArch = "arm64" }
    "amd64" { $demoArch = "x64" }
    "x86_64" { $demoArch = "x64" }
    "x64" { $demoArch = "x64" }
    default {
        Write-Error "Invalid demo architecture '$requestedArch'; expected arm64 or x64."
        exit 1
    }
}

if ($run -and $demoArch -ne $hostArch) {
    throw "Cannot smoke-run $demoArch demos on the $hostArch host. Omit --run for cross-architecture builds."
}

$runTimeoutSeconds = 5
$maximumRunOutputBytes = 1048576
if ($run) {
    $runTimeoutValue = Get-EnvironmentValue -Name "ZANNA_DEMO_TIMEOUT" -Default "5"
    if (-not [int]::TryParse($runTimeoutValue, [ref]$runTimeoutSeconds) -or
        $runTimeoutSeconds -lt 1 -or $runTimeoutSeconds -gt 2147483) {
        throw "ZANNA_DEMO_TIMEOUT must be an integer from 1 through 2147483; received '$runTimeoutValue'."
    }
    $maximumRunOutputValue =
        Get-EnvironmentValue -Name "ZANNA_DEMO_MAX_OUTPUT_BYTES" -Default "1048576"
    if (-not [int]::TryParse($maximumRunOutputValue, [ref]$maximumRunOutputBytes) -or
        $maximumRunOutputBytes -lt 4096 -or $maximumRunOutputBytes -gt 16777216) {
        throw "ZANNA_DEMO_MAX_OUTPUT_BYTES must be an integer from 4096 through 16777216; received '$maximumRunOutputValue'."
    }
}

$windowsO0Demos = @(
    "3dbowling",
    "ridgebound",
    "xenoscape",
    "crackman",
    "chess",
    "zannasql"
)

if (-not $buildDirExplicit -and $demoArch -ne $hostArch) {
    $buildDirSetting = Join-Path $repoRoot "build-$demoArch"
}
if (-not $toolBuildDirExplicit -and $demoArch -eq $hostArch) {
    $toolBuildDirSetting = $buildDirSetting
}

$buildDir = Get-FullPath -Path $buildDirSetting -Base $repoRoot
$toolBuildDir = Get-FullPath -Path $toolBuildDirSetting -Base $repoRoot
$binDir = Join-Path $repoRoot "examples\bin"
$manifest = Join-Path $scriptRoot "demo_projects.list"

function Resolve-ZannaExecutable {
    param([Parameter(Mandatory = $true)][string]$Tree)

    $configured = Join-Path $Tree "src\tools\zanna\$buildType\zanna.exe"
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
    foreach ($line in Get-Content -LiteralPath $Cache) {
        if ($line.StartsWith($prefix, [StringComparison]::Ordinal)) {
            $separator = $line.IndexOf('=')
            if ($separator -ge 0) {
                return $line.Substring($separator + 1).Trim()
            }
        }
    }
    return ""
}

function Get-CMakeGeneratedSystemProcessor {
    param([Parameter(Mandatory = $true)][string]$Cache)

    $tree = Split-Path -Parent ([IO.Path]::GetFullPath($Cache))
    $cmakeFiles = Join-Path $tree "CMakeFiles"
    if (-not (Test-Path -LiteralPath $cmakeFiles -PathType Container)) {
        return ""
    }
    $reported = @{}
    $systemFiles = @(Get-ChildItem -LiteralPath $cmakeFiles -Filter "CMakeSystem.cmake" `
        -File -Recurse)
    if ($systemFiles.Count -gt 16) {
        throw "CMake tree has an implausible number of generated system descriptions: $tree"
    }
    foreach ($file in $systemFiles) {
        if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $file.Length -gt 65536) {
            throw "CMake generated system description is unsafe: $($file.FullName)"
        }
        foreach ($line in Get-Content -LiteralPath $file.FullName) {
            if ($line -match '^\s*set\(CMAKE_SYSTEM_PROCESSOR\s+"?([^"\s\)]+)"?\s*\)\s*$') {
                $reported[$Matches[1].ToLowerInvariant()] = $Matches[1]
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

function Assert-NoReparsePath {
    param(
        [Parameter(Mandatory = $true)][string]$Base,
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $baseFull = [IO.Path]::GetFullPath($Base).TrimEnd('\', '/')
    $candidateFull = [IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    if (-not (Test-PathWithin -Base $baseFull -Candidate $candidateFull -AllowBase)) {
        throw "$Description escapes its owned root: $candidateFull"
    }
    $relative = if ($candidateFull.Length -eq $baseFull.Length) {
        ""
    } else {
        $candidateFull.Substring($baseFull.Length + 1)
    }
    $current = $baseFull
    $components = @()
    if (-not [string]::IsNullOrEmpty($relative)) {
        $components = $relative.Split(
            [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar),
            [StringSplitOptions]::RemoveEmptyEntries)
    }
    foreach ($component in @("") + $components) {
        if (-not [string]::IsNullOrEmpty($component)) {
            $current = Join-Path $current $component
        }
        if (-not (Test-Path -LiteralPath $current)) {
            continue
        }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description traverses a reparse point: $current"
        }
    }
}

function Assert-NoReparseTree {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $rootItem = Get-Item -LiteralPath $Root -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description is a reparse point: $Root"
    }
    if ($rootItem.PSIsContainer) {
        foreach ($entry in Get-ChildItem -LiteralPath $Root -Force -Recurse) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $($entry.FullName)"
            }
        }
    }
}

function Assert-PortableExecutableArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)][ValidateSet("arm64", "x64")][string]$Architecture
    )

    if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) {
        throw "Expected demo executable was not produced: $Binary"
    }
    $machine = 0
    $stream = [IO.File]::Open(
        $Binary, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -lt 64) {
            throw "Demo executable is too small to contain a PE header: $Binary"
        }
        $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::ASCII, $true)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw "Demo executable is missing the MZ signature: $Binary"
            }
            $stream.Position = 0x3c
            $peOffset = [uint64]$reader.ReadUInt32()
            if ($peOffset -gt [uint64]($stream.Length - 26)) {
                throw "Demo executable has an out-of-range PE header: $Binary"
            }
            $stream.Position = [int64]$peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) {
                throw "Demo executable is missing the PE signature: $Binary"
            }
            $machine = $reader.ReadUInt16()
            $sectionCount = $reader.ReadUInt16()
            if ($sectionCount -lt 1 -or $sectionCount -gt 96) {
                throw "Demo executable has an invalid PE section count: $Binary"
            }
            $stream.Position = [int64]($peOffset + 20)
            $optionalHeaderSize = [uint64]$reader.ReadUInt16()
            $characteristics = $reader.ReadUInt16()
            if ($optionalHeaderSize -lt 112 -or
                $peOffset + 24 + $optionalHeaderSize -gt [uint64]$stream.Length) {
                throw "Demo executable has an invalid optional header: $Binary"
            }
            if (($characteristics -band 0x0002) -eq 0 -or
                ($characteristics -band 0x2000) -ne 0) {
                throw "Demo output is not a non-DLL executable image: $Binary"
            }
            $stream.Position = [int64]($peOffset + 24)
            if ($reader.ReadUInt16() -ne 0x020B) {
                throw "Demo executable is not a PE32+ image: $Binary"
            }
            $stream.Position = [int64]($peOffset + 24 + 16)
            if ($reader.ReadUInt32() -eq 0) {
                throw "Demo executable has no entry point: $Binary"
            }
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    $expectedMachine = if ($Architecture -eq "arm64") { 0xAA64 } else { 0x8664 }
    if ($machine -ne $expectedMachine) {
        throw ("Demo executable machine 0x{0:X4} does not match requested {1}: {2}" -f
            $machine, $Architecture, $Binary)
    }
}

function Publish-DemoDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $stageFull = [IO.Path]::GetFullPath($Stage)
    $destinationFull = [IO.Path]::GetFullPath($Destination)
    if (-not (Test-PathWithin -Base $binDir -Candidate $stageFull) -or
        -not (Test-PathWithin -Base $binDir -Candidate $destinationFull)) {
        throw "Demo directory publication escaped examples/bin."
    }
    Assert-NoReparsePath -Base $binDir -Candidate $stageFull `
        -Description "Demo directory staging path"
    Assert-NoReparsePath -Base $binDir -Candidate $destinationFull `
        -Description "Demo directory destination"
    Assert-NoReparseTree -Root $stageFull -Description "Demo directory staging tree"
    if (-not (Test-Path -LiteralPath $stageFull -PathType Container)) {
        throw "Validated demo directory stage disappeared: $stageFull"
    }
    if (Test-Path -LiteralPath $destinationFull -PathType Leaf) {
        throw "Demo directory destination is a file: $destinationFull"
    }

    $backup = Join-Path (Split-Path -Parent $destinationFull) (
        ".zanna-demo-backup-{0}-{1}" -f
            [IO.Path]::GetFileName($destinationFull),
            [Guid]::NewGuid().ToString("N"))
    $backedUp = $false
    $published = $false
    try {
        if (Test-Path -LiteralPath $destinationFull -PathType Container) {
            Assert-NoReparseTree -Root $destinationFull `
                -Description "Existing demo publication tree"
            [IO.Directory]::Move($destinationFull, $backup)
            $backedUp = $true
        }
        [IO.Directory]::Move($stageFull, $destinationFull)
        $published = $true
    } catch {
        if ($published -and (Test-Path -LiteralPath $destinationFull)) {
            Remove-DemoRunDirectoryEntry -Path $destinationFull
        }
        if ($backedUp -and (Test-Path -LiteralPath $backup -PathType Container)) {
            [IO.Directory]::Move($backup, $destinationFull)
        }
        throw
    }
    if ($backedUp -and (Test-Path -LiteralPath $backup -PathType Container)) {
        try {
            Remove-DemoRunDirectoryEntry -Path $backup
        } catch {
            Write-Warning "Published demo directory but could not remove rollback backup: $backup"
        }
    }
}

$zanna = Resolve-ZannaExecutable -Tree $toolBuildDir
$targetZanna = Resolve-ZannaExecutable -Tree $buildDir

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
        } elseif (-not $TreeIsExplicit) {
            $cmakeArch = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
            $configureArguments += @("-A", $cmakeArch)
        }
        $configureArguments += "-DCMAKE_BUILD_TYPE=$buildType"
        $configureArguments += @(ConvertFrom-NativeArgumentString -Value `
            ([Environment]::GetEnvironmentVariable("ZANNA_EXTRA_CMAKE_ARGS", "Process")))
        Invoke-CheckedNative -FilePath "cmake" -Arguments $configureArguments `
            -FailureMessage "CMake configuration failed for $Description"
    }
    Write-Host "Building $Description..."
    Invoke-CheckedNative -FilePath "cmake" `
        -Arguments @("--build", $Tree, "--config", $buildType, "--target", "zanna", "-j", [string]$jobs) `
        -FailureMessage "$Description build failed"
    $resolvedExecutable = Resolve-ZannaExecutable -Tree $Tree
    if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
        throw "$Description still not found in $Tree"
    }
    return $resolvedExecutable
}

function Copy-DemoAssetEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][hashtable]$Ownership
    )

    $sourceItem = Get-Item -LiteralPath $Source -Force
    if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Demo asset is a reparse point: $Source"
    }
    if ($sourceItem.PSIsContainer) {
        if (Test-Path -LiteralPath $Destination -PathType Leaf) {
            throw "Demo asset directory collides with a file: $Destination"
        }
        [void][IO.Directory]::CreateDirectory($Destination)
        Assert-NoReparsePath -Base $DestinationRoot -Candidate $Destination `
            -Description "Demo asset destination"
        foreach ($child in Get-ChildItem -LiteralPath $Source -Force) {
            Copy-DemoAssetEntry -Source $child.FullName `
                -Destination (Join-Path $Destination $child.Name) `
                -DestinationRoot $DestinationRoot -Ownership $Ownership
        }
        return
    }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Demo asset is not an ordinary file: $Source"
    }

    $parent = Split-Path -Parent $Destination
    [void][IO.Directory]::CreateDirectory($parent)
    Assert-NoReparsePath -Base $DestinationRoot -Candidate $parent `
        -Description "Demo asset destination parent"
    if (Test-Path -LiteralPath $Destination -PathType Container) {
        throw "Demo asset file collides with a directory: $Destination"
    }
    $key = [IO.Path]::GetFullPath($Destination).ToLowerInvariant()
    if ($Ownership.ContainsKey($key)) {
        $previousSource = [string]$Ownership[$key]
        if (-not [string]::Equals(
                $previousSource, $Source, [StringComparison]::OrdinalIgnoreCase)) {
            $sameLength = (Get-Item -LiteralPath $previousSource).Length -eq $sourceItem.Length
            $sameHash = $sameLength -and
                (Get-FileHash -LiteralPath $previousSource -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
            if (-not $sameHash) {
                throw "Demo asset destination collision: $Destination"
            }
        }
        return
    }
    [IO.File]::Copy($Source, $Destination, $true)
    $Ownership[$key] = $Source
}

function Copy-DemoAsset {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectDir,
        [Parameter(Mandatory = $true)][string]$SourceRelative,
        [Parameter(Mandatory = $true)][string]$TargetRelative,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][hashtable]$Ownership
    )

    if ([IO.Path]::IsPathRooted($SourceRelative) -or [IO.Path]::IsPathRooted($TargetRelative)) {
        throw "Demo asset paths must be relative to their project and examples/bin."
    }
    $source = [IO.Path]::GetFullPath((Join-Path $ProjectDir $SourceRelative))
    if (-not (Test-PathWithin -Base $ProjectDir -Candidate $source -AllowBase)) {
        throw "Asset source escapes the demo project: $SourceRelative"
    }
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Asset not found: $SourceRelative"
    }
    Assert-NoReparsePath -Base $ProjectDir -Candidate $source -Description "Demo asset source"
    Assert-NoReparseTree -Root $source -Description "Demo asset source"
    $destination = if ($TargetRelative -eq ".") {
        [IO.Path]::GetFullPath($DestinationRoot)
    } else {
        [IO.Path]::GetFullPath((Join-Path $DestinationRoot $TargetRelative))
    }
    if (-not (Test-PathWithin -Base $DestinationRoot -Candidate $destination -AllowBase)) {
        throw "Asset target escapes the demo output directory: $TargetRelative"
    }
    [void][IO.Directory]::CreateDirectory($destination)
    Assert-NoReparsePath -Base $DestinationRoot -Candidate $destination `
        -Description "Demo asset destination"
    if (Test-Path -LiteralPath $source -PathType Container) {
        foreach ($child in Get-ChildItem -LiteralPath $source -Force) {
            Copy-DemoAssetEntry -Source $child.FullName `
                -Destination (Join-Path $destination $child.Name) `
                -DestinationRoot $DestinationRoot -Ownership $Ownership
        }
    } else {
        Copy-DemoAssetEntry -Source $source `
            -Destination (Join-Path $destination ([IO.Path]::GetFileName($source))) `
            -DestinationRoot $DestinationRoot -Ownership $Ownership
    }
}

function Stage-DemoAssets {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectDir,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][hashtable]$Ownership
    )

    $projectFile = Join-Path $ProjectDir "zanna.project"
    foreach ($line in Get-Content -LiteralPath $projectFile) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith("#")) {
            continue
        }
        if ($trimmed -notmatch '^(?i:asset)(?:\s|$)') {
            continue
        }
        $parts = @(ConvertFrom-NativeArgumentString -Value $trimmed)
        if ($parts.Count -lt 2 -or $parts.Count -gt 3 -or $parts[0] -ine "asset" -or
            [string]::IsNullOrWhiteSpace($parts[1]) -or
            ($parts.Count -eq 3 -and [string]::IsNullOrWhiteSpace($parts[2]))) {
            throw "malformed asset directive in ${projectFile}: $trimmed"
        }
        $sourceRelative = $parts[1]
        $targetRelative = if ($parts.Count -eq 3) { $parts[2] } else { "." }
        Copy-DemoAsset -ProjectDir $ProjectDir -SourceRelative $sourceRelative `
            -TargetRelative $targetRelative -DestinationRoot $DestinationRoot `
            -Ownership $Ownership
    }
}

$processStopTimeoutMilliseconds = 5000

function New-DemoRunDirectory {
    param([Parameter(Mandatory = $true)][string]$Name)

    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $runDirectory = [IO.Path]::GetFullPath((Join-Path $temporaryRoot (
        "zanna_demo_run_{0}_{1}" -f $Name, [Guid]::NewGuid().ToString("N"))))
    if (-not (Test-PathWithin -Base $temporaryRoot -Candidate $runDirectory)) {
        throw "Demo smoke directory escaped the Windows temporary directory."
    }
    [void][IO.Directory]::CreateDirectory($runDirectory)
    Assert-NoReparsePath -Base $temporaryRoot -Candidate $runDirectory `
        -Description "Demo smoke directory"
    return $runDirectory
}

function Remove-DemoRunDirectoryEntry {
    param([Parameter(Mandatory = $true)][string]$Path)

    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        if ($item.PSIsContainer) {
            [IO.Directory]::Delete($item.FullName, $false)
        } else {
            [IO.File]::Delete($item.FullName)
        }
        return
    }
    if ($item.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $item.FullName -Force) {
            Remove-DemoRunDirectoryEntry -Path $child.FullName
        }
        [IO.Directory]::Delete($item.FullName, $false)
    } else {
        [IO.File]::Delete($item.FullName)
    }
}

function Remove-DemoRunDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $runDirectory = [IO.Path]::GetFullPath($Path)
    if (-not (Test-PathWithin -Base $temporaryRoot -Candidate $runDirectory) -or
        -not [IO.Path]::GetFileName($runDirectory).StartsWith(
            "zanna_demo_run_", [StringComparison]::Ordinal)) {
        throw "Refusing to remove an unexpected demo smoke directory: $runDirectory"
    }
    Remove-DemoRunDirectoryEntry -Path $runDirectory
}

function Stop-DemoProcessTree {
    param([Parameter(Mandatory = $true)][Diagnostics.Process]$Process)

    if ($Process.HasExited) {
        return
    }
    $taskkill = Join-Path $env:SystemRoot "System32\taskkill.exe"
    if (-not (Test-Path -LiteralPath $taskkill -PathType Leaf)) {
        throw "Cannot locate the Windows process-tree terminator: $taskkill"
    }
    $killInfo = [Diagnostics.ProcessStartInfo]::new()
    $killInfo.FileName = $taskkill
    $killInfo.Arguments = "/PID $($Process.Id) /T /F"
    $killInfo.UseShellExecute = $false
    $killInfo.CreateNoWindow = $true
    $killer = [Diagnostics.Process]::Start($killInfo)
    if ($null -eq $killer) {
        throw "Failed to start the Windows process-tree terminator."
    }
    try {
        if (-not $killer.WaitForExit($processStopTimeoutMilliseconds)) {
            try {
                $killer.Kill()
            } catch {
                # The bounded wait failure remains authoritative.
            }
            throw "Windows process-tree termination exceeded its bounded wait."
        }
        if ($killer.ExitCode -ne 0) {
            throw "Windows process-tree termination failed with exit code $($killer.ExitCode)."
        }
    } finally {
        $killer.Dispose()
    }
    if (-not $Process.WaitForExit($processStopTimeoutMilliseconds)) {
        throw "Demo process tree did not terminate within the bounded shutdown wait."
    }
}

function Write-DemoRunOutput {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-Item -LiteralPath $Path).Length -eq 0) {
        return
    }
    Write-Host "  ${Label}:"
    foreach ($line in Get-Content -LiteralPath $Path -TotalCount 20) {
        Write-Host "    $line"
    }
}

function Test-DemoRun {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$SourceDirectory
    )

    $timeoutSeconds = $runTimeoutSeconds
    if ($Name -iin @("3dbowling", "ridgebound", "zannasql", "xenoscape")) {
        $timeoutSeconds = [Math]::Max($timeoutSeconds, 10)
    }

    $runDirectory = New-DemoRunDirectory -Name $Name
    $runExecutable = Join-Path $runDirectory "$Name.exe"
    $stdoutPath = Join-Path $runDirectory "stdout.txt"
    $stderrPath = Join-Path $runDirectory "stderr.txt"
    $process = $null
    $succeeded = $false
    try {
        $runAssetOwners = @{}
        foreach ($child in Get-ChildItem -LiteralPath $SourceDirectory -Force) {
            Copy-DemoAssetEntry -Source $child.FullName `
                -Destination (Join-Path $runDirectory $child.Name) `
                -DestinationRoot $runDirectory -Ownership $runAssetOwners
        }
        Write-Host "  Launching for up to $timeoutSeconds second(s)..."
        $process = Start-Process -FilePath $runExecutable -WorkingDirectory $runDirectory -PassThru `
            -WindowStyle Hidden -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        # Windows PowerShell can lose ExitCode for a redirected process that exits
        # quickly unless the native process handle is materialized before waiting.
        $processHandle = $process.Handle
        if ($processHandle -eq [IntPtr]::Zero) {
            throw "Failed to acquire the demo process handle."
        }
        $elapsed = [Diagnostics.Stopwatch]::StartNew()
        $timedOut = $false
        while (-not $process.WaitForExit(50)) {
            $stdoutBytes = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
                (Get-Item -LiteralPath $stdoutPath).Length
            } else { 0 }
            $stderrBytes = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
                (Get-Item -LiteralPath $stderrPath).Length
            } else { 0 }
            if ($stdoutBytes -gt $maximumRunOutputBytes -or
                $stderrBytes -gt $maximumRunOutputBytes) {
                Stop-DemoProcessTree -Process $process
                throw "output exceeded the $maximumRunOutputBytes-byte smoke limit"
            }
            if ($elapsed.Elapsed.TotalSeconds -ge $timeoutSeconds) {
                Stop-DemoProcessTree -Process $process
                $timedOut = $true
                break
            }
        }
        $elapsed.Stop()
        if ($timedOut) {
            Write-Host "  Run smoke: OK (remained active until timeout)"
            $succeeded = $true
        } else {
            $stdoutBytes = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
                (Get-Item -LiteralPath $stdoutPath).Length
            } else { 0 }
            $stderrBytes = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
                (Get-Item -LiteralPath $stderrPath).Length
            } else { 0 }
            if ($stdoutBytes -gt $maximumRunOutputBytes -or
                $stderrBytes -gt $maximumRunOutputBytes) {
                throw "output exceeded the $maximumRunOutputBytes-byte smoke limit"
            }
            if ($process.ExitCode -eq 0) {
                Write-Host "  Run smoke: OK (exit 0)"
                $succeeded = $true
            } else {
                Write-Host "  Run smoke: FAILED (exit $($process.ExitCode))"
                Write-DemoRunOutput -Label "stdout" -Path $stdoutPath
                Write-DemoRunOutput -Label "stderr" -Path $stderrPath
            }
        }
    } catch {
        Write-Host "  Run smoke: FAILED ($($_.Exception.Message))"
        Write-DemoRunOutput -Label "stdout" -Path $stdoutPath
        Write-DemoRunOutput -Label "stderr" -Path $stderrPath
    } finally {
        if ($null -ne $process) {
            if (-not $process.HasExited) {
                try {
                    Stop-DemoProcessTree -Process $process
                } catch {
                    Write-Warning "Could not fully terminate demo '$Name': $($_.Exception.Message)"
                }
            }
            $process.Dispose()
        }
        Remove-DemoRunDirectory -Path $runDirectory
    }
    return $succeeded
}

function Build-Demo {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ProjectDir
    )

    Write-Host "Building $Name..."
    Write-Host "  Started: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
    if ($Name -ieq "zannasql") {
        Write-Host "  Note: zannasql is the slowest demo on Windows and can take several minutes."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $ProjectDir "zanna.project") -PathType Leaf)) {
        Write-Host "  ERROR: No zanna.project found in $ProjectDir"
        Write-Host "  Finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
        Write-Host ""
        return $false
    }

    $outputDirectory = Join-Path $binDir $Name
    $output = Join-Path $outputDirectory "$Name.exe"
    $stageDirectory = Join-Path $binDir (
        ".zanna-demo-stage-{0}-{1}" -f $Name, [Guid]::NewGuid().ToString("N"))
    [void][IO.Directory]::CreateDirectory($stageDirectory)
    Assert-NoReparsePath -Base $binDir -Candidate $stageDirectory `
        -Description "Demo staging directory"
    $stage = Join-Path $stageDirectory "$Name.exe"
    $buildArguments = @("build", $ProjectDir, "--arch", $demoArch)
    if ($windowsO0Demos -icontains $Name) {
        if ($Name -ieq "zannasql") {
            Write-Host "  Using -O0 to avoid pathological optimizer/codegen time for this large demo."
        } else {
            Write-Host "  Using -O0 to avoid the Windows native checked-integer optimizer miscompile."
        }
        $buildArguments += "-O0"
    }
    $buildArguments += @("-o", $stage)

    Write-Host "  Compiling..."
    $savedErrorActionPreference = $ErrorActionPreference
    $buildStatus = -1
    try {
        # Windows PowerShell 5.1 wraps redirected native stderr as non-terminating
        # NativeCommandError records. Zanna reports normal linker progress there,
        # so rely on its process status while retaining strict mode for script code.
        $ErrorActionPreference = "Continue"
        & $zanna @buildArguments 2>&1 | ForEach-Object { Write-Host $_ }
        $buildStatus = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($buildStatus -ne 0) {
        Write-Host "  FAILED"
        Write-Host "  Finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
        Write-Host ""
        Remove-DemoRunDirectoryEntry -Path $stageDirectory
        return $false
    }
    try {
        Assert-PortableExecutableArchitecture -Binary $stage -Architecture $demoArch
        $assetOwners = @{}
        Stage-DemoAssets -ProjectDir $ProjectDir -DestinationRoot $stageDirectory `
            -Ownership $assetOwners
        if ($run -and -not (Test-DemoRun -Name $Name -SourceDirectory $stageDirectory)) {
            Write-Host "  FAILED"
            Write-Host "  Finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
            Write-Host ""
            return $false
        }
        Publish-DemoDirectory -Stage $stageDirectory -Destination $outputDirectory
    } catch {
        Write-Host "  ERROR: $($_.Exception.Message)"
        Write-Host "  FAILED"
        Write-Host "  Finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
        Write-Host ""
        return $false
    } finally {
        if (Test-Path -LiteralPath $stageDirectory) {
            Remove-DemoRunDirectoryEntry -Path $stageDirectory
        }
    }

    Write-Host "  OK"
    Write-Host "  Built: $output"
    Write-Host "  Finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
    Write-Host ""
    return $true
}

$previousBuildDir = [Environment]::GetEnvironmentVariable("ZANNA_BUILD_DIR", "Process")
$env:ZANNA_BUILD_DIR = $buildDir
Push-Location $repoRoot
try {
    Write-Host "Building Zanna demos as native $demoArch binaries"
    Write-Host "=============================================="
    Write-Host ""
    Write-Host "Note: larger demos can stay quiet for several minutes while codegen runs."
    Write-Host "Using Zanna tool: $toolBuildDir"
    Write-Host "Using target runtime build: $buildDir"
    if ($run) {
        Write-Host "Run validation: private per-demo launch with timeout=$runTimeoutSeconds second(s)"
    }
    Write-Host ""

    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "Demo manifest not found: $manifest"
    }
    $toolCache = Join-Path $toolBuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $toolCache -PathType Leaf) {
        Assert-CMakeTreeArchitecture -Cache $toolCache -Architecture $hostArch `
            -Description "host Zanna tool"
    }
    $targetCache = Join-Path $buildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $targetCache -PathType Leaf) {
        Assert-CMakeTreeArchitecture -Cache $targetCache -Architecture $demoArch `
            -Description "target-architecture Zanna runtime"
    }
    if (-not (Test-Path -LiteralPath $zanna -PathType Leaf)) {
        $zanna = Ensure-ZannaBuild -Tree $toolBuildDir -Architecture $hostArch `
            -TreeIsExplicit $toolBuildDirExplicit -Description "host Zanna tool"
    }
    if ($toolBuildDir -ine $buildDir -and
        -not (Test-Path -LiteralPath $targetZanna -PathType Leaf)) {
        $targetZanna = Ensure-ZannaBuild -Tree $buildDir -Architecture $demoArch `
            -TreeIsExplicit $buildDirExplicit -Description "target-architecture Zanna runtime"
    }

    if (Test-Path -LiteralPath $binDir) {
        $binItem = Get-Item -LiteralPath $binDir -Force
        if (-not $binItem.PSIsContainer -or
            ($binItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Demo output root must be an ordinary directory: $binDir"
        }
    } else {
        [void][IO.Directory]::CreateDirectory($binDir)
    }
    Assert-NoReparsePath -Base $repoRoot -Candidate $binDir `
        -Description "Demo output root"
    if ($clean) {
        Write-Host "Cleaning existing binaries..."
        $expectedBin = [IO.Path]::GetFullPath((Join-Path $repoRoot "examples\bin"))
        if ([IO.Path]::GetFullPath($binDir) -ne $expectedBin) {
            throw "Refusing to clean unexpected demo directory: $binDir"
        }
        foreach ($entry in Get-ChildItem -LiteralPath $binDir -Force) {
            Remove-DemoRunDirectoryEntry -Path $entry.FullName
        }
    }

    Write-Host "=== Zia Showcase Demos ==="
    Write-Host ""
    $failed = 0
    $succeeded = 0
    $entries = 0
    $seenNames = @{}
    foreach ($line in Get-Content -LiteralPath $manifest) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith("#")) {
            continue
        }
        ++$entries
        $fields = $trimmed.Split('|')
        if ($fields.Count -ne 3) {
            Write-Host "ERROR: invalid demo manifest entry: $trimmed"
            ++$failed
            continue
        }
        $name = $fields[0].Trim()
        $category = $fields[1].Trim()
        $directory = $fields[2].Trim()
        if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
            $directory -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
            Write-Host "ERROR: unsafe demo name or directory in manifest entry: $trimmed"
            ++$failed
            continue
        }
        $nameKey = $name.ToLowerInvariant()
        if ($seenNames.ContainsKey($nameKey)) {
            Write-Host "ERROR: duplicate demo executable name '$name'"
            ++$failed
            continue
        }
        $seenNames[$nameKey] = $true
        if ($category -ieq "games") {
            $categoryRoot = Join-Path $repoRoot "examples\games"
        } elseif ($category -ieq "apps") {
            $categoryRoot = Join-Path $repoRoot "examples\apps"
        } else {
            Write-Host "ERROR: invalid demo category '$category' for '$name'"
            ++$failed
            continue
        }
        $projectDir = [IO.Path]::GetFullPath((Join-Path $categoryRoot $directory))
        if (-not (Test-PathWithin -Base $categoryRoot -Candidate $projectDir)) {
            Write-Host "ERROR: demo directory escapes its category for '$name'"
            ++$failed
            continue
        }
        if (Build-Demo -Name $name -ProjectDir $projectDir) {
            ++$succeeded
        } else {
            ++$failed
        }
    }
    if ($entries -eq 0) {
        throw "Demo manifest contains no projects."
    }

    Write-Host "=============================================="
    if ($failed -eq 0) {
        if ($run) {
            Write-Host "All $succeeded demos built and passed launch smoke validation."
        } else {
            Write-Host "All $succeeded demos built successfully."
        }
        Write-Host ""
        Write-Host "Binaries and assets are in per-demo directories under: $binDir"
        Get-ChildItem -LiteralPath $binDir -File -Recurse |
            Where-Object { $_.Extension -ieq ".exe" } |
            Format-Table Mode, LastWriteTime, Length, FullName
        exit 0
    }
    Write-Host "$failed demo(s) failed, $succeeded succeeded"
    exit $failed
} finally {
    Pop-Location
    [Environment]::SetEnvironmentVariable("ZANNA_BUILD_DIR", $previousBuildDir, "Process")
}
