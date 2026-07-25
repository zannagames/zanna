#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/build_installer.ps1
# Purpose: Build or reuse a staged Zanna tree and invoke install-package.
# Key invariants:
#   - Fresh package builds use the canonical Windows build script.
#   - Installer stages include Zanna Studio unless the caller chose explicitly.
#   - Packaging runs from a private driver copy outside every relink target.
#   - All install-package arguments are forwarded as discrete native arguments.
# Ownership/Lifetime: Build and package outputs are owned by their caller-selected paths.
# Links: scripts/build_zanna_win.ps1, docs/installer-release.md
# Cross-platform touchpoints: This Windows wrapper selects the native installer
#                             target; packaging policy lives in install-package.
#
#===----------------------------------------------------------------------===#

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

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

function Get-PeArchitecture {
    param([Parameter(Mandatory = $true)][string]$Binary)

    $machine = 0
    $stream = [IO.File]::Open(
        $Binary, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -lt 64) {
            throw "Zanna Studio executable is too small to contain a PE header."
        }
        $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::ASCII, $true)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw "Zanna Studio executable is missing the MZ signature."
            }
            $stream.Position = 0x3c
            $peOffset = [uint64]$reader.ReadUInt32()
            if ($peOffset -gt [uint64]($stream.Length - 26)) {
                throw "Zanna Studio executable has an out-of-range PE header."
            }
            $stream.Position = [int64]$peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) {
                throw "Zanna Studio executable is missing the PE signature."
            }
            $machine = $reader.ReadUInt16()
            $stream.Position = [int64]($peOffset + 20)
            $optionalHeaderSize = [uint64]$reader.ReadUInt16()
            if ($optionalHeaderSize -lt 2 -or
                $peOffset + 24 + $optionalHeaderSize -gt [uint64]$stream.Length) {
                throw "Zanna Studio executable has an invalid optional header."
            }
            $stream.Position = [int64]($peOffset + 24)
            if ($reader.ReadUInt16() -ne 0x020B) {
                throw "Zanna Studio executable is not a PE32+ image."
            }
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    switch ($machine) {
        0x8664 { return "x64" }
        0xAA64 { return "arm64" }
        default { throw ("Unsupported Zanna Studio PE machine 0x{0:X4}." -f $machine) }
    }
}

function Assert-ZannaStudioArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)][string]$BuildInfo,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion
    )

    if (-not (Test-Path -LiteralPath $Binary -PathType Leaf) -or
        -not (Test-Path -LiteralPath $BuildInfo -PathType Leaf)) {
        throw "The Windows toolchain build did not produce the required Zanna Studio executable and build metadata."
    }
    $binaryItem = Get-Item -LiteralPath $Binary
    $metadataItem = Get-Item -LiteralPath $BuildInfo
    if ($binaryItem.Length -le 0 -or $metadataItem.Length -le 0 -or
        $metadataItem.Length -gt 16384) {
        throw "The Zanna Studio executable or build metadata has an invalid size."
    }
    $fields = @{}
    $lines = [IO.File]::ReadAllLines($BuildInfo, [Text.UTF8Encoding]::new($false, $true))
    if ($lines.Count -lt 2 -or
        -not [string]::Equals(
            $lines[0], "Zanna Studio $ExpectedVersion", [StringComparison]::Ordinal)) {
        throw "The Zanna Studio build metadata version does not match this toolchain."
    }
    foreach ($line in $lines | Select-Object -Skip 1) {
        if ($line.Length -gt 4096) {
            throw "The Zanna Studio build metadata contains an oversized line."
        }
        $separator = $line.IndexOf(": ", [StringComparison]::Ordinal)
        if ($separator -le 0 -or $separator + 2 -ge $line.Length) {
            throw "The Zanna Studio build metadata contains an invalid field."
        }
        $key = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 2)
        if ($fields.ContainsKey($key)) {
            throw "The Zanna Studio build metadata contains duplicate '$key' fields."
        }
        $fields[$key] = $value
    }
    foreach ($required in @(
            "Schema", "Build", "Source", "Architecture", "Size", "SHA256", "Output", "Zanna")) {
        if (-not $fields.ContainsKey($required)) {
            throw "The Zanna Studio build metadata is missing '$required'."
        }
    }
    if ($fields.Count -ne 8) {
        throw "The Zanna Studio build metadata contains an unknown field."
    }
    if ($fields["Schema"] -ne "1") {
        throw "The Zanna Studio build metadata uses an unsupported schema."
    }
    [uint64]$recordedSize = 0
    if (-not [uint64]::TryParse(
            $fields["Size"],
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$recordedSize) -or
        $recordedSize -ne [uint64]$binaryItem.Length) {
        throw "The Zanna Studio build metadata size does not match the executable."
    }
    $architecture = Get-PeArchitecture -Binary $Binary
    if ($fields["Architecture"] -ne $architecture) {
        throw "The Zanna Studio build metadata architecture does not match the executable."
    }
    $actualHash = (Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($fields["SHA256"] -cnotmatch '^[0-9a-f]{64}$' -or
        $fields["SHA256"] -cne $actualHash) {
        throw "The Zanna Studio build metadata SHA-256 does not match the executable."
    }
}

function Invoke-StagedPackageDriver {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$BuildDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $sourceItem = Get-Item -LiteralPath $Source -Force
    if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $sourceItem.Length -le 0) {
        throw "The Zanna package driver must be a non-empty regular file: $Source"
    }

    $buildRoot = [IO.Path]::GetFullPath($BuildDirectory)
    $driverDirectory = Join-Path $buildRoot (
        ".zanna-installer-driver-" + [Guid]::NewGuid().ToString("N"))
    $driverPath = Join-Path $driverDirectory "zanna.exe"
    $rootPrefix = $buildRoot.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $driverDirectory.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $driverDirectory)) {
        throw "Cannot allocate a confined private Zanna package driver directory."
    }

    [void][IO.Directory]::CreateDirectory($driverDirectory)
    $packageExitCode = 1
    try {
        $directoryItem = Get-Item -LiteralPath $driverDirectory -Force
        if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "The private Zanna package driver directory is an indirect path."
        }

        $input = [IO.File]::Open(
            $sourceItem.FullName,
            [IO.FileMode]::Open,
            [IO.FileAccess]::Read,
            [IO.FileShare]::Read)
        try {
            $output = [IO.File]::Open(
                $driverPath,
                [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write,
                [IO.FileShare]::None)
            try {
                $input.CopyTo($output)
                $output.Flush($true)
            } finally {
                $output.Dispose()
            }
        } finally {
            $input.Dispose()
        }

        $driverItem = Get-Item -LiteralPath $driverPath -Force
        if (($driverItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $driverItem.Length -ne $sourceItem.Length) {
            throw "The private Zanna package driver copy has an invalid shape."
        }
        $sourceHash = (Get-FileHash -LiteralPath $sourceItem.FullName -Algorithm SHA256).Hash
        $driverHash = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash
        if ($sourceHash -cne $driverHash) {
            throw "The private Zanna package driver copy failed SHA-256 verification."
        }

        & $driverPath @Arguments
        $packageExitCode = $LASTEXITCODE
    } finally {
        if (Test-Path -LiteralPath $driverPath) {
            [IO.File]::Delete($driverPath)
        }
        if (Test-Path -LiteralPath $driverDirectory) {
            $directoryItem = Get-Item -LiteralPath $driverDirectory -Force
            if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to remove an indirect Zanna package driver directory."
            }
            [IO.Directory]::Delete($driverDirectory, $false)
        }
    }
    return $packageExitCode
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot ".."))
$forwardArguments = @($args | ForEach-Object { [string]$_ })
if (@($forwardArguments | Where-Object { $_ -ieq "--help" -or $_ -eq "-h" }).Count -gt 0) {
    Write-Host "Usage: build_installer.ps1 [zanna install-package options]"
    Write-Host "Builds a Release toolchain package with Zanna Studio unless an existing input is supplied."
    Write-Host "Use --stage-dir, --build-dir, or --verify-only to reuse caller-owned input."
    exit 0
}

$normalizedArguments = [Collections.Generic.List[string]]::new()
$usesExistingInput = $false
$hasExplicitBuildDir = $false
$inputModeCount = 0
for ($argumentIndex = 0; $argumentIndex -lt $forwardArguments.Count; ++$argumentIndex) {
    $argument = $forwardArguments[$argumentIndex]
    $matchedInputOption = $false
    foreach ($inputOption in @("--build-dir", "--stage-dir", "--verify-only")) {
        if ($argument -ieq $inputOption) {
            if ($argumentIndex + 1 -ge $forwardArguments.Count -or
                [string]::IsNullOrWhiteSpace($forwardArguments[$argumentIndex + 1])) {
                throw "$inputOption requires a non-empty value."
            }
            $normalizedArguments.Add($inputOption)
            ++$argumentIndex
            $normalizedArguments.Add($forwardArguments[$argumentIndex])
            $matchedInputOption = $true
        } elseif ($argument.StartsWith(
                "$inputOption=", [StringComparison]::OrdinalIgnoreCase)) {
            $value = $argument.Substring($inputOption.Length + 1)
            if ([string]::IsNullOrWhiteSpace($value)) {
                throw "$inputOption requires a non-empty value."
            }
            $normalizedArguments.Add($inputOption)
            $normalizedArguments.Add($value)
            $matchedInputOption = $true
        }
        if ($matchedInputOption) {
            ++$inputModeCount
            $usesExistingInput = $true
            if ($inputOption -eq "--build-dir") {
                $hasExplicitBuildDir = $true
            }
            break
        }
    }
    if (-not $matchedInputOption) {
        $normalizedArguments.Add($argument)
    }
}
if ($inputModeCount -gt 1) {
    throw "Specify at most one of --build-dir, --stage-dir, or --verify-only."
}
$forwardArguments = $normalizedArguments.ToArray()

$buildDir = [Environment]::GetEnvironmentVariable("ZANNA_BUILD_DIR", "Process")
if ([string]::IsNullOrWhiteSpace($buildDir)) {
    $buildDir = Join-Path $repoRoot "build"
} elseif (-not [IO.Path]::IsPathRooted($buildDir)) {
    $buildDir = [IO.Path]::GetFullPath((Join-Path $repoRoot $buildDir))
} else {
    $buildDir = [IO.Path]::GetFullPath($buildDir)
}
$env:ZANNA_BUILD_DIR = $buildDir

$buildType = [Environment]::GetEnvironmentVariable("ZANNA_BUILD_TYPE", "Process")
if ([string]::IsNullOrWhiteSpace($buildType)) {
    $buildType = "Release"
}
switch ($buildType.ToLowerInvariant()) {
    "release" { $buildType = "Release" }
    "relwithdebinfo" { $buildType = "RelWithDebInfo" }
    "debug" {
        if (-not $usesExistingInput) {
            throw "Fresh Windows installer builds require ZANNA_BUILD_TYPE=Release or RelWithDebInfo."
        }
        $buildType = "Debug"
    }
    "minsizerel" {
        if (-not $usesExistingInput) {
            throw "Fresh Windows installer builds require ZANNA_BUILD_TYPE=Release or RelWithDebInfo."
        }
        $buildType = "MinSizeRel"
    }
    default {
        throw "ZANNA_BUILD_TYPE must be Debug, Release, RelWithDebInfo, or MinSizeRel."
    }
}
$env:ZANNA_BUILD_TYPE = $buildType
if ([string]::IsNullOrWhiteSpace(
        [Environment]::GetEnvironmentVariable("ZANNA_SKIP_INSTALL", "Process"))) {
    $env:ZANNA_SKIP_INSTALL = "1"
}

if (-not $usesExistingInput) {
    $extraArguments = [Environment]::GetEnvironmentVariable("ZANNA_EXTRA_CMAKE_ARGS", "Process")
    $extraTokens = @(ConvertFrom-NativeArgumentString -Value ([string]$extraArguments))
    $requireZannaStudio = $false
    $studioValues = [Collections.Generic.List[bool]]::new()
    for ($tokenIndex = 0; $tokenIndex -lt $extraTokens.Count; ++$tokenIndex) {
        $definition = $extraTokens[$tokenIndex]
        if ($definition -eq "-D") {
            if ($tokenIndex + 1 -ge $extraTokens.Count) {
                throw "ZANNA_EXTRA_CMAKE_ARGS ends with an incomplete -D option."
            }
            ++$tokenIndex
            $definition = "-D" + $extraTokens[$tokenIndex]
        }
        if (-not $definition.StartsWith(
                "-DZANNA_INSTALL_ZANNASTUDIO", [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $settingMatch = [regex]::Match(
            $definition,
            '^(?i:-DZANNA_INSTALL_ZANNASTUDIO)(?::(?<type>[^=]+))?=(?<value>.*)$')
        if (-not $settingMatch.Success) {
            throw "Malformed ZANNA_INSTALL_ZANNASTUDIO CMake definition '$definition'."
        }
        $settingType = $settingMatch.Groups["type"].Value
        if (-not [string]::IsNullOrEmpty($settingType) -and
            $settingType -ine "BOOL") {
            throw "ZANNA_INSTALL_ZANNASTUDIO must be untyped or use the BOOL CMake type."
        }
        $studioSetting = $settingMatch.Groups["value"].Value
        if ($studioSetting -match '^(?i:ON|1|TRUE|YES)$') {
            $studioValues.Add($true)
        } elseif ($studioSetting -match '^(?i:OFF|0|FALSE|NO)$') {
            $studioValues.Add($false)
        } else {
            throw "Unsupported ZANNA_INSTALL_ZANNASTUDIO value '$studioSetting'."
        }
    }
    if ($studioValues.Count -eq 0) {
        if ([string]::IsNullOrWhiteSpace($extraArguments)) {
            $env:ZANNA_EXTRA_CMAKE_ARGS = "-DZANNA_INSTALL_ZANNASTUDIO=ON"
        } else {
            $env:ZANNA_EXTRA_CMAKE_ARGS = "$extraArguments -DZANNA_INSTALL_ZANNASTUDIO=ON"
        }
        $requireZannaStudio = $true
    } else {
        $requireZannaStudio = $studioValues[0]
        foreach ($studioValue in $studioValues) {
            if ($studioValue -ne $requireZannaStudio) {
                throw "Conflicting ZANNA_INSTALL_ZANNASTUDIO definitions are not allowed."
            }
        }
    }

    $buildScript = Join-Path $scriptRoot "build_zanna_win.ps1"
    if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
        throw "Canonical Windows build script not found: $buildScript"
    }
    $powerShellHost = Join-Path $PSHOME "pwsh.exe"
    if (-not (Test-Path -LiteralPath $powerShellHost -PathType Leaf)) {
        $powerShellHost = Join-Path $PSHOME "powershell.exe"
    }
    if (-not (Test-Path -LiteralPath $powerShellHost -PathType Leaf)) {
        throw "Unable to locate the current PowerShell host under $PSHOME."
    }
    & $powerShellHost -NoProfile -ExecutionPolicy Bypass -File $buildScript
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    if ($requireZannaStudio) {
        $studio = Join-Path $buildDir "zannastudio\zannastudio.exe"
        $studioBuildInfo = Join-Path $buildDir "zannastudio\zannastudio.buildinfo"
        $versionPath = Join-Path $repoRoot "src\buildmeta\VERSION"
        if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
            throw "Cannot bind Zanna Studio metadata without $versionPath."
        }
        $expectedVersion = ([string](
                Get-Content -LiteralPath $versionPath -TotalCount 1)).Trim()
        if ([string]::IsNullOrWhiteSpace($expectedVersion)) {
            throw "The Zanna version metadata is empty: $versionPath"
        }
        Assert-ZannaStudioArtifact -Binary $studio -BuildInfo $studioBuildInfo `
            -ExpectedVersion $expectedVersion
    }
}

$zanna = Join-Path $buildDir "src\tools\zanna\zanna.exe"
$configuredZanna = Join-Path $buildDir "src\tools\zanna\$buildType\zanna.exe"
if (Test-Path -LiteralPath $configuredZanna -PathType Leaf) {
    $zanna = $configuredZanna
}
if (-not (Test-Path -LiteralPath $zanna -PathType Leaf)) {
    Write-Error "Zanna executable not found at '$zanna'. Build Zanna first or set ZANNA_BUILD_DIR."
    exit 1
}

$packageArguments = @("install-package")
if (-not $usesExistingInput -and -not $hasExplicitBuildDir) {
    $packageArguments += @(
        "--build-dir", $buildDir,
        "--config", $buildType,
        "--skip-build"
    )
}
$packageArguments += $forwardArguments
$packageExitCode = Invoke-StagedPackageDriver -Source $zanna -BuildDirectory $buildDir `
    -Arguments $packageArguments
exit $packageExitCode
