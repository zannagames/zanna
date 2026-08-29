#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/build_zanna_win.ps1
# Purpose: Configure, build, test, audit, and install Zanna on Windows.
#
# Key invariants:
#   - User-selected generators and compilers are forwarded as discrete native
#     arguments, without an intervening command shell.
#   - Pre-configure cleaning runs only for an already configured build tree.
#   - Only a validated POSIX shell is used by CTest and script-based checks.
#   - Tests without an explicit CMake timeout receive a bounded default.
#   - A failed validation stage causes the script to return a nonzero status.
#
# Ownership/Lifetime: Build outputs remain in ZANNA_BUILD_DIR; recursive
#                     cleanup is restricted to known children of that tree.
#
# Links: AGENTS.md, docs/internals/testing.md
# Cross-platform touchpoints: This is the Windows build adapter; shared policy
#                             and smoke checks execute through validated Git Bash.
#
#===----------------------------------------------------------------------===#

[CmdletBinding()]
param()

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

function Set-EnvironmentDefault {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Default
    )

    $value = Get-EnvironmentValue -Name $Name -Default $Default
    [Environment]::SetEnvironmentVariable($Name, $value, "Process")
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
            continue
        }

        if ($character -eq '"' -or $character -eq "'") {
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

function Get-PositiveInteger {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $parsed = 0
    if (-not [int]::TryParse($Value, [ref]$parsed) -or $parsed -lt 1 -or $parsed -gt 1024) {
        throw "$Name must be an integer from 1 through 1024; received '$Value'."
    }
    return $parsed
}

function Get-TimeoutSeconds {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $parsed = 0
    if (-not [int]::TryParse($Value, [ref]$parsed) -or $parsed -lt 1 -or
        $parsed -gt 86400) {
        throw "$Name must be an integer from 1 through 86400; received '$Value'."
    }
    return $parsed
}

function Assert-BinaryToggle {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    if ($Value -notin @("0", "1")) {
        throw "$Name must be 0 or 1; received '$Value'."
    }
}

function Get-FullPathFromRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Remove-BuildChild {
    param(
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][string]$Child,
        [switch]$Recurse
    )

    if (-not (Test-Path -LiteralPath $Child)) {
        return
    }
    $rootPath = [IO.Path]::GetFullPath($BuildRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $childPath = [IO.Path]::GetFullPath($Child)
    if (-not $childPath.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside the build tree: $childPath"
    }
    Remove-Item -LiteralPath $childPath -Force -Recurse:$Recurse
}

function Test-ToolExists {
    param([Parameter(Mandatory = $true)][string]$Tool)

    if (Test-Path -LiteralPath $Tool -PathType Leaf) {
        return $true
    }
    if ($Tool.IndexOfAny(@([char]'\', [char]'/')) -ge 0) {
        return $false
    }
    return $null -ne (Get-Command $Tool -ErrorAction SilentlyContinue)
}

function Find-UsableBash {
    $candidates = [Collections.Generic.List[string]]::new()
    $configured = [Environment]::GetEnvironmentVariable("ZANNA_BASH_EXECUTABLE", "Process")
    if (-not [string]::IsNullOrWhiteSpace($configured)) {
        $candidates.Add($configured)
    }

    $programFiles = [Environment]::GetEnvironmentVariable("ProgramFiles", "Process")
    if (-not [string]::IsNullOrWhiteSpace($programFiles)) {
        $candidates.Add((Join-Path $programFiles "Git\bin\bash.exe"))
        $candidates.Add((Join-Path $programFiles "Git\usr\bin\bash.exe"))
    }
    $localAppData = [Environment]::GetEnvironmentVariable("LOCALAPPDATA", "Process")
    if (-not [string]::IsNullOrWhiteSpace($localAppData)) {
        $candidates.Add((Join-Path $localAppData "Programs\Git\bin\bash.exe"))
    }
    foreach ($command in @(Get-Command bash.exe -All -ErrorAction SilentlyContinue)) {
        $candidates.Add($command.Source)
    }

    $seen = @{}
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate) -or $seen.ContainsKey($candidate)) {
            continue
        }
        $seen[$candidate] = $true
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }
        try {
            & $candidate -lc "exit 0" *> $null
            if ($LASTEXITCODE -eq 0) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        } catch {
            continue
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($configured)) {
        Write-Warning "ZANNA_BASH_EXECUTABLE is not usable: $configured"
    }
    return $null
}

function Test-CachedCompilerMatchesRequest {
    param(
        [Parameter(Mandatory = $true)][string]$Compiler,
        [Parameter(Mandatory = $true)][string]$RequestedCompiler
    )

    $leaf = [IO.Path]::GetFileName($Compiler)
    $isClangCl = $leaf -ieq "clang-cl" -or $leaf -ieq "clang-cl.exe"
    if ($RequestedCompiler -eq "clang-cl") {
        return $isClangCl
    }
    return -not $isClangCl
}

function Reset-StaleCompilerCache {
    param(
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][string]$RequestedCompiler
    )

    $cacheFile = Join-Path $BuildRoot "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
        return $false
    }

    $values = @{}
    foreach ($line in Get-Content -LiteralPath $cacheFile) {
        if ($line -match '^(CMAKE_C_COMPILER|CMAKE_CXX_COMPILER):(FILEPATH|STRING)=(.*)$') {
            $values[$Matches[1]] = $Matches[3]
        }
    }

    $stale = $false
    foreach ($key in @("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER")) {
        if (-not $values.ContainsKey($key) -or [string]::IsNullOrWhiteSpace($values[$key])) {
            continue
        }
        $compiler = [string]$values[$key]
        if (-not (Test-CachedCompilerMatchesRequest -Compiler $compiler `
                  -RequestedCompiler $RequestedCompiler)) {
            Write-Host "Cached $key does not match requested compiler '$RequestedCompiler': $compiler"
            $stale = $true
        } elseif (-not (Test-ToolExists -Tool $compiler)) {
            Write-Host "Cached $key no longer exists: $compiler"
            $stale = $true
        }
    }

    if (-not $stale) {
        return $false
    }

    Write-Host "Detected stale CMake compiler cache in '$BuildRoot'."
    Write-Host "Resetting cached configure state; build outputs will be regenerated as needed."
    Remove-BuildChild -BuildRoot $BuildRoot -Child $cacheFile
    Remove-BuildChild -BuildRoot $BuildRoot -Child (Join-Path $BuildRoot "CMakeFiles") -Recurse
    return $true
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot ".."))

# An inherited Windows environment block can contain both Path and PATH even
# though lookups are case-insensitive. Recreate the process entry once because
# MSBuild's ToolTask rejects the duplicate while launching cl.exe.
$normalizedPath = [Environment]::GetEnvironmentVariable("PATH", "Process")
[Environment]::SetEnvironmentVariable("Path", $null, "Process")
[Environment]::SetEnvironmentVariable("PATH", $null, "Process")
[Environment]::SetEnvironmentVariable("Path", $normalizedPath, "Process")

$automaticJobs = Get-EnvironmentValue -Name "NUMBER_OF_PROCESSORS" -Default "8"
$requestedJobs = [Environment]::GetEnvironmentVariable("ZANNA_JOBS", "Process")
if ([string]::IsNullOrWhiteSpace($requestedJobs)) {
    $jobs = [Math]::Min((Get-PositiveInteger -Name "NUMBER_OF_PROCESSORS" -Value $automaticJobs), 8)
} else {
    $jobs = Get-PositiveInteger -Name "ZANNA_JOBS" -Value $requestedJobs
}
$ctestJobsValue = Get-EnvironmentValue -Name "ZANNA_CTEST_JOBS" -Default ([string]$jobs)
$ctestJobs = Get-PositiveInteger -Name "ZANNA_CTEST_JOBS" -Value $ctestJobsValue
$ctestTimeoutValue = Get-EnvironmentValue -Name "ZANNA_CTEST_TIMEOUT" -Default "600"
$ctestTimeout = Get-TimeoutSeconds -Name "ZANNA_CTEST_TIMEOUT" -Value $ctestTimeoutValue

$env:MSBUILDDISABLENODEREUSE = Get-EnvironmentValue -Name "MSBUILDDISABLENODEREUSE" -Default "1"
$buildDir = Set-EnvironmentDefault -Name "ZANNA_BUILD_DIR" -Default "build"
$buildType = Set-EnvironmentDefault -Name "ZANNA_BUILD_TYPE" -Default "Debug"
$skipInstall = Set-EnvironmentDefault -Name "ZANNA_SKIP_INSTALL" -Default "0"
$skipTests = Set-EnvironmentDefault -Name "ZANNA_SKIP_TESTS" -Default "0"
$skipLint = Set-EnvironmentDefault -Name "ZANNA_SKIP_LINT" -Default "0"
$skipAudit = Set-EnvironmentDefault -Name "ZANNA_SKIP_AUDIT" -Default "0"
$lintChangedOnly = Set-EnvironmentDefault -Name "ZANNA_LINT_CHANGED_ONLY" -Default "1"
$skipSmoke = Set-EnvironmentDefault -Name "ZANNA_SKIP_SMOKE" -Default "0"
$skipClean = Set-EnvironmentDefault -Name "ZANNA_SKIP_CLEAN" -Default "0"
$runSlowTests = Set-EnvironmentDefault -Name "ZANNA_RUN_SLOW_TESTS" -Default "0"
$fastDebug = Set-EnvironmentDefault -Name "ZANNA_FAST_DEBUG" -Default "1"
$skipStudio = Set-EnvironmentDefault -Name "ZANNA_SKIP_STUDIO" -Default "0"
foreach ($toggle in @(
        @{ Name = "ZANNA_SKIP_INSTALL"; Value = $skipInstall },
        @{ Name = "ZANNA_SKIP_TESTS"; Value = $skipTests },
        @{ Name = "ZANNA_SKIP_LINT"; Value = $skipLint },
        @{ Name = "ZANNA_SKIP_AUDIT"; Value = $skipAudit },
        @{ Name = "ZANNA_LINT_CHANGED_ONLY"; Value = $lintChangedOnly },
        @{ Name = "ZANNA_SKIP_SMOKE"; Value = $skipSmoke },
        @{ Name = "ZANNA_SKIP_CLEAN"; Value = $skipClean },
        @{ Name = "ZANNA_RUN_SLOW_TESTS"; Value = $runSlowTests },
        @{ Name = "ZANNA_FAST_DEBUG"; Value = $fastDebug },
        @{ Name = "ZANNA_SKIP_STUDIO"; Value = $skipStudio })) {
    Assert-BinaryToggle -Name $toggle.Name -Value $toggle.Value
}
$buildRoot = Get-FullPathFromRoot -Path $buildDir -Root $repoRoot
$bashBuildDir = $buildDir.Replace('\', '/')

Push-Location $repoRoot
try {
    Write-Host "=========================================="
    Write-Host "Building Zanna on Windows"
    Write-Host "=========================================="
    Write-Host ""
    Write-Host "Using $jobs build jobs"
    Write-Host "Using $ctestJobs CTest jobs"
    Write-Host "Using a $ctestTimeout-second default timeout for otherwise untimed tests"
    Write-Host "Build type: $buildType"
    Write-Host "Fast Debug: $fastDebug"

    $bashExe = Find-UsableBash
    if ($null -ne $bashExe) {
        Write-Host "POSIX shell: $bashExe"
        if ($null -eq (Get-Command clang.exe -ErrorAction SilentlyContinue)) {
            $programFiles = [Environment]::GetEnvironmentVariable("ProgramFiles", "Process")
            if (-not [string]::IsNullOrWhiteSpace($programFiles)) {
                $llvmBin = Join-Path $programFiles "LLVM\bin"
                if (Test-Path -LiteralPath (Join-Path $llvmBin "clang.exe") -PathType Leaf) {
                    $env:Path = "$($env:Path);$llvmBin"
                }
            }
        }
    } else {
        Write-Host "POSIX shell: unavailable"
    }

    $configArguments = @("-DZANNA_FAST_DEBUG=$fastDebug")
    # The Zanna Studio native compile is the longest single build step. Pass
    # the toggle explicitly every configure so a skipped run cannot leave a
    # stale OFF cached into later full runs.
    if ($skipStudio -eq "1") {
        Write-Host "Skipping Zanna Studio build (ZANNA_SKIP_STUDIO=1)"
        $studioInstallSetting = "OFF"
    } else {
        $studioInstallSetting = "ON"
    }
    if ($null -ne $bashExe) {
        $configArguments += "-DZANNA_BASH_EXECUTABLE:FILEPATH=$bashExe"
    }
    $generator = [Environment]::GetEnvironmentVariable("ZANNA_CMAKE_GENERATOR", "Process")
    if (-not [string]::IsNullOrWhiteSpace($generator)) {
        $configArguments += @("-G", $generator)
    }
    $extraArguments = [Environment]::GetEnvironmentVariable("ZANNA_EXTRA_CMAKE_ARGS", "Process")
    $configArguments += @(ConvertFrom-NativeArgumentString -Value $extraArguments)
    # This policy flag is deliberately last so generic extra arguments cannot
    # leave a skipped Studio setting cached into a later canonical full build.
    $configArguments += "-DZANNA_INSTALL_ZANNASTUDIO=$studioInstallSetting"

    $warnAsError = [Environment]::GetEnvironmentVariable("ZANNA_WARN_AS_ERROR", "Process")
    if ([string]::IsNullOrWhiteSpace($warnAsError)) {
        if ([Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE", "Process") -ieq "ARM64") {
            $configArguments += "-DIL_WARN_AS_ERROR=OFF"
        }
    } else {
        $configArguments += "-DIL_WARN_AS_ERROR=$warnAsError"
    }

    $compilerArguments = @()
    $requestedCompiler = "msvc"
    $windowsCompiler = [Environment]::GetEnvironmentVariable("ZANNA_WINDOWS_COMPILER", "Process")
    if ($windowsCompiler -ieq "clang-cl") {
        if ($null -eq (Get-Command clang-cl.exe -ErrorAction SilentlyContinue)) {
            throw "ZANNA_WINDOWS_COMPILER=clang-cl but clang-cl was not found in PATH."
        }
        Write-Host "Using clang-cl (ZANNA_WINDOWS_COMPILER=clang-cl)"
        $requestedCompiler = "clang-cl"
        $compilerArguments = @("-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl")
    } else {
        Write-Host "Using default compiler MSVC"
    }
    Write-Host ""

    $cacheReset = Reset-StaleCompilerCache -BuildRoot $buildRoot `
        -RequestedCompiler $requestedCompiler

    $configuredBuild =
        Test-Path -LiteralPath (Join-Path $buildRoot "CMakeCache.txt") -PathType Leaf
    if ($skipClean -eq "1") {
        Write-Host "Skipping clean (ZANNA_SKIP_CLEAN=1); incremental rebuild"
    } elseif ($cacheReset) {
        Write-Host "Skipping pre-configure clean because cached compiler state was reset."
    } elseif ($configuredBuild) {
        & cmake --build $buildDir --target clean-all 2>$null
    } else {
        Write-Host "Skipping pre-configure clean because the build tree is not configured."
    }

    Write-Host "Configuring with CMake..."
    $configureArguments = @("-S", ".", "-B", $buildDir) + $compilerArguments +
        @("-DCMAKE_BUILD_TYPE=$buildType") + $configArguments
    Invoke-CheckedNative -FilePath "cmake" -Arguments $configureArguments `
        -FailureMessage "CMake configuration failed"
    Write-Host ""

    Write-Host "Building with $jobs jobs..."
    Invoke-CheckedNative -FilePath "cmake" `
        -Arguments @("--build", $buildDir, "--config", $buildType, "-j", [string]$jobs) `
        -FailureMessage "Build failed"
    Write-Host ""

    Start-Sleep -Seconds 1

    $validationFailed = $false
    if ($skipTests -eq "1") {
        Write-Host "Skipping tests (ZANNA_SKIP_TESTS=1)"
    } else {
        Write-Host "Running tests..."
        $ctestArguments = @(
            "--test-dir", $buildDir,
            "-C", $buildType,
            "--output-on-failure",
            "--timeout", [string]$ctestTimeout,
            "-j", [string]$ctestJobs
        )
        $testLabel = [Environment]::GetEnvironmentVariable("ZANNA_TEST_LABEL", "Process")
        if (-not [string]::IsNullOrWhiteSpace($testLabel)) {
            Write-Host "Running only tests labeled '$testLabel' (ZANNA_TEST_LABEL)"
            $ctestArguments += @("-L", $testLabel)
        }
        if ($runSlowTests -ne "1") {
            Write-Host "Skipping tests labeled slow (set ZANNA_RUN_SLOW_TESTS=1 to include them)"
            $ctestArguments += @("-LE", "slow")
        }

        $prettyScript = Join-Path $scriptRoot "run_ctest_pretty.ps1"
        $windowsPowerShell = Get-Command powershell.exe -ErrorAction SilentlyContinue
        if ($null -ne $windowsPowerShell -and (Test-Path -LiteralPath $prettyScript -PathType Leaf)) {
            & $windowsPowerShell.Source -NoProfile -ExecutionPolicy Bypass -File $prettyScript @ctestArguments
        } else {
            & ctest @ctestArguments
        }
        if ($LASTEXITCODE -ne 0) {
            $validationFailed = $true
            Write-Host ""
            Write-Warning "Some tests failed"
        } else {
            Write-Host ""
            Write-Host "All tests passed."
        }
    }

    if ($skipLint -eq "0") {
        if ($null -ne $bashExe) {
            Write-Host "Running platform policy lint..."
            $lintArguments = @("--login", "scripts/lint_platform_policy.sh")
            if ($lintChangedOnly -eq "1") {
                $lintArguments += "--changed-only"
            }
            & $bashExe @lintArguments
            if ($LASTEXITCODE -ne 0) {
                $validationFailed = $true
            }
        } else {
            Write-Host "Skipping platform policy lint (no usable POSIX shell found)"
        }
    }

    if ($skipAudit -eq "0") {
        if ($null -ne $bashExe) {
            Write-Host "Running runtime surface audit..."
            & $bashExe --login scripts/audit_runtime_surface.sh `
                "--build-dir=$bashBuildDir" "--config=$buildType"
            if ($LASTEXITCODE -ne 0) {
                $validationFailed = $true
            }
        } else {
            Write-Host "Skipping runtime surface audit (no usable POSIX shell found)"
        }
    }

    if ($skipSmoke -eq "0") {
        if ($null -ne $bashExe) {
            Write-Host "Running cross-platform smoke tests..."
            & $bashExe --login scripts/run_cross_platform_smoke.sh `
                --build-dir $bashBuildDir --config $buildType
            if ($LASTEXITCODE -ne 0) {
                $validationFailed = $true
            }
        } else {
            Write-Host "Skipping cross-platform smoke tests (no usable POSIX shell found)"
        }
    }

    Write-Host ""
    if ($skipInstall -eq "1") {
        Write-Host "Skipping install (ZANNA_SKIP_INSTALL=1)"
    } else {
        Write-Host "Installing Zanna..."
        $localAppData = [Environment]::GetEnvironmentVariable("LOCALAPPDATA", "Process")
        if (-not [string]::IsNullOrWhiteSpace($localAppData)) {
            $installPrefix = Join-Path $localAppData "zanna"
        } else {
            $installPrefix = Join-Path ([Environment]::GetEnvironmentVariable("USERPROFILE", "Process")) "zanna"
        }
        $configuredPrefix = [Environment]::GetEnvironmentVariable("ZANNA_INSTALL_PREFIX", "Process")
        if (-not [string]::IsNullOrWhiteSpace($configuredPrefix)) {
            $installPrefix = $configuredPrefix
        }
        & cmake --install $buildDir --prefix $installPrefix --config $buildType
        if ($LASTEXITCODE -ne 0) {
            $validationFailed = $true
            Write-Warning "Install failed"
        } else {
            Write-Host "Installed to $installPrefix"
        }
    }

    Write-Host ""
    Write-Host "=========================================="
    Write-Host "Build complete"
    Write-Host "=========================================="

    if ($validationFailed) {
        Write-Error "One or more Windows validation stages failed."
        exit 1
    }
    exit 0
} finally {
    Pop-Location
}
