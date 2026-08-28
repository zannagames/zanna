#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/validate-windows-toolchain-installer.ps1
# Purpose: Validate a native Zanna toolchain installer on a disposable Windows host.
#
# Key invariants:
#   - Package identity and paths come from the recursively verified /inspect record.
#   - Existing installations are never removed without an explicit opt-in.
#   - Validation finishes through the product uninstaller and checks detached cleanup.
#
# Ownership/Lifetime: A GUID-named temporary workspace and this run's product state.
#
# Links: docs/installer-release.md, WindowsToolchainInstallerE2E.cmake
#
#===----------------------------------------------------------------------===#

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer,

    [string]$BaselineInstaller = "",
    [string]$UpgradeStaleRelativePath = "share\zanna\installer-upgrade-stale.txt",
    [string]$InstallRoot = "",
    [string]$Scope = "",
    [string]$SignToolPath = "signtool.exe",

    [ValidateRange(5, 3600)]
    [int]$ProcessTimeoutSeconds = 300,

    [ValidateRange(4096, 16777216)]
    [int]$MaximumCaptureBytes = 1048576,

    [ValidateRange(4096, 16777216)]
    [int]$MaximumInspectBytes = 1048576,

    [switch]$RequireSignature,
    [switch]$ReplaceExisting,
    [switch]$KeepInstalled
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptRoot "windows_pe_validation.ps1")

function Quote-ProcessArgument {
    param([AllowNull()][string]$Argument)

    if ($null -eq $Argument) {
        return '""'
    }
    $escaped = $Argument -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    if ($escaped.Length -eq 0 -or $escaped -match '[\s"]') {
        return '"' + $escaped + '"'
    }
    return $escaped
}

function Stop-CheckedProcessTree {
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
        if (-not $killer.WaitForExit(10000)) {
            try {
                $killer.Kill()
            } catch {
                # The bounded wait failure remains authoritative.
            }
            throw "The Windows process-tree terminator did not exit within 10 seconds."
        }
        if ($killer.ExitCode -ne 0) {
            throw "The Windows process-tree terminator failed with exit code $($killer.ExitCode)."
        }
    } finally {
        $killer.Dispose()
    }
    if (-not $Process.WaitForExit(10000)) {
        throw "The terminated process tree did not reap within 10 seconds."
    }
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
    return $candidateFull.StartsWith(
        $baseFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseAncestors {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $current = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description traverses a reparse point: $current"
            }
            $linkTypeProperty = $item.PSObject.Properties["LinkType"]
            if ($linkTypeProperty -and $linkTypeProperty.Value -eq "HardLink") {
                throw "$Description traverses a hard-link alias: $current"
            }
        }
        $parent = Split-Path -Parent $current
        if ([string]::IsNullOrWhiteSpace($parent) -or
            [string]::Equals($parent, $current, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent
    }
}

function Resolve-SafeRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath.IndexOfAny([char[]](0..31)) -ge 0 -or
        $RelativePath.IndexOfAny([char[]]('<', '>', ':', '"', '|', '?', '*')) -ge 0) {
        throw "$Description must be a safe relative Windows path."
    }
    foreach ($component in $RelativePath.Split([char[]]('\', '/'),
                                                [StringSplitOptions]::None)) {
        if ([string]::IsNullOrWhiteSpace($component) -or $component -in @(".", "..") -or
            $component.EndsWith(".", [StringComparison]::Ordinal) -or
            $component.EndsWith(" ", [StringComparison]::Ordinal)) {
            throw "$Description contains an unsafe path component."
        }
    }
    $resolved = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
    if (-not (Test-PathWithin -Base $Root -Candidate $resolved)) {
        throw "$Description escapes the selected installation root."
    }
    return $resolved
}

function Write-NewSentinel {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Get-PeArchitecture {
    param([Parameter(Mandatory = $true)][string]$Binary)

    return Get-ZannaPeImageArchitecture -Binary $Binary
}

function Assert-ZannaStudioBuildInfo {
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)][string]$BuildInfo,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][string]$Version
    )

    if (-not (Test-Path -LiteralPath $BuildInfo -PathType Leaf)) {
        throw "Installed Zanna Studio build metadata is missing: $BuildInfo"
    }
    Assert-NoReparseAncestors -Path $Binary -Description "Installed Zanna Studio executable"
    Assert-NoReparseAncestors -Path $BuildInfo -Description "Installed Zanna Studio build metadata"
    $binaryItem = Get-Item -LiteralPath $Binary
    $metadataItem = Get-Item -LiteralPath $BuildInfo
    if ($binaryItem.Length -le 0 -or $metadataItem.Length -le 0 -or
        $metadataItem.Length -gt 16384) {
        throw "Installed Zanna Studio provenance has an invalid size."
    }
    $fields = @{}
    $lines = [IO.File]::ReadAllLines(
        $BuildInfo, [Text.UTF8Encoding]::new($false, $true))
    if ($lines.Count -lt 2 -or
        -not [string]::Equals(
            $lines[0], "Zanna Studio $Version", [StringComparison]::Ordinal)) {
        throw "Installed Zanna Studio provenance version does not match the package."
    }
    foreach ($line in $lines | Select-Object -Skip 1) {
        if ($line.Length -gt 4096) {
            throw "Installed Zanna Studio provenance contains an oversized line."
        }
        $separator = $line.IndexOf(": ", [StringComparison]::Ordinal)
        if ($separator -le 0 -or $separator + 2 -ge $line.Length) {
            throw "Installed Zanna Studio provenance contains an invalid field."
        }
        $key = $line.Substring(0, $separator)
        if ($fields.ContainsKey($key)) {
            throw "Installed Zanna Studio provenance contains a duplicate field."
        }
        $fields[$key] = $line.Substring($separator + 2)
    }
    foreach ($required in @(
            "Schema", "Build", "Source", "Architecture", "Size", "SHA256", "Output", "Zanna")) {
        if (-not $fields.ContainsKey($required)) {
            throw "Installed Zanna Studio provenance is missing $required."
        }
    }
    if ($fields.Count -ne 8) {
        throw "Installed Zanna Studio provenance contains an unknown field."
    }
    [uint64]$recordedSize = 0
    if ($fields["Schema"] -ne "1" -or $fields["Architecture"] -ne $Architecture -or
        (Get-PeArchitecture -Binary $Binary) -ne $Architecture -or
        -not [uint64]::TryParse(
            $fields["Size"],
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$recordedSize) -or
        $recordedSize -ne [uint64]$binaryItem.Length) {
        throw "Installed Zanna Studio provenance does not match the installed PE."
    }
    $hash = (Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($fields["SHA256"] -cnotmatch '^[0-9a-f]{64}$' -or $fields["SHA256"] -cne $hash) {
        throw "Installed Zanna Studio provenance hash does not match the installed PE."
    }
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = (Get-Location).Path
    )

    $captureToken = "$PID-$([Guid]::NewGuid().ToString('N'))"
    $captureRoot = [IO.Path]::GetTempPath()
    $stdoutPath = Join-Path $captureRoot ".zanna-process-$captureToken.stdout"
    $stderrPath = Join-Path $captureRoot ".zanna-process-$captureToken.stderr"
    $stdoutStream = $null
    $stderrStream = $null
    $process = $null
    try {
        $stdoutStream = [IO.FileStream]::new(
            $stdoutPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::Read,
            4096,
            [IO.FileOptions]::Asynchronous)
        $stderrStream = [IO.FileStream]::new(
            $stderrPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::Read,
            4096,
            [IO.FileOptions]::Asynchronous)

        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $FilePath
        if ($psi.PSObject.Properties.Name -contains "ArgumentList") {
            foreach ($argument in $Arguments) {
                [void]$psi.ArgumentList.Add($argument)
            }
        } else {
            $psi.Arguments = ($Arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join " "
        }
        $psi.WorkingDirectory = $WorkingDirectory
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($psi)
        if ($null -eq $process) {
            throw "Failed to start process: $FilePath"
        }

        $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
        $stderrTask = $process.StandardError.BaseStream.CopyToAsync($stderrStream)
        $elapsed = [Diagnostics.Stopwatch]::StartNew()
        $failure = $null
        while (-not $process.WaitForExit(25)) {
            if ($stdoutStream.Length -gt $MaximumCaptureBytes -or
                $stderrStream.Length -gt $MaximumCaptureBytes) {
                $failure =
                    "Process output exceeded the $MaximumCaptureBytes-byte capture limit: $FilePath"
                break
            }
            if ($elapsed.Elapsed.TotalSeconds -ge $ProcessTimeoutSeconds) {
                $failure = "Process timed out after $ProcessTimeoutSeconds seconds: $FilePath"
                break
            }
        }
        $elapsed.Stop()
        if ($failure) {
            Stop-CheckedProcessTree -Process $process
            [void]$process.StandardOutput.Close()
            [void]$process.StandardError.Close()
        } else {
            [void]$process.WaitForExit()
        }
        $drainTask = [Threading.Tasks.Task]::WhenAll(
            [Threading.Tasks.Task[]]@($stdoutTask, $stderrTask))
        $captureDrained = $false
        try {
            $captureDrained = $drainTask.Wait(10000)
        } catch {
            if (-not $failure) {
                $failure = "Process output capture failed: $FilePath"
            }
        }
        if (-not $captureDrained) {
            [void]$process.StandardOutput.Close()
            [void]$process.StandardError.Close()
            if (-not $failure) {
                $failure = "Process output did not drain within 10 seconds: $FilePath"
            }
            try {
                [void]$drainTask.Wait(1000)
            } catch {
                # The primary timeout/limit diagnostic remains authoritative.
            }
        }
        try {
            [void]$stdoutTask.GetAwaiter().GetResult()
            [void]$stderrTask.GetAwaiter().GetResult()
        } catch {
            if (-not $failure) {
                throw
            }
        }
        [void]$stdoutStream.Flush()
        [void]$stderrStream.Flush()
        if (-not $failure -and
            ($stdoutStream.Length -gt $MaximumCaptureBytes -or
             $stderrStream.Length -gt $MaximumCaptureBytes)) {
            $failure =
                "Process output exceeded the $MaximumCaptureBytes-byte capture limit: $FilePath"
        }
        if ($failure) {
            throw $failure
        }
        $exitCode = $process.ExitCode
        [void]$stdoutStream.Dispose()
        $stdoutStream = $null
        [void]$stderrStream.Dispose()
        $stderrStream = $null
        $captureEncoding = [Text.UTF8Encoding]::new($false, $false)
        return [pscustomobject]@{
            ExitCode = $exitCode
            Stdout = [IO.File]::ReadAllText($stdoutPath, $captureEncoding)
            Stderr = [IO.File]::ReadAllText($stderrPath, $captureEncoding)
        }
    } finally {
        if ($null -ne $process) {
            [void]$process.Dispose()
        }
        if ($null -ne $stdoutStream) {
            [void]$stdoutStream.Dispose()
        }
        if ($null -ne $stderrStream) {
            [void]$stderrStream.Dispose()
        }
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-CheckedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = (Get-Location).Path,
        [int[]]$SuccessCodes = @(0)
    )

    $result = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments `
        -WorkingDirectory $WorkingDirectory
    if ($result.ExitCode -notin $SuccessCodes) {
        throw "$FilePath failed with exit code $($result.ExitCode)`n" +
            "stdout:`n$($result.Stdout)`nstderr:`n$($result.Stderr)"
    }
    return $result.Stdout
}

function Get-ZannaProductVersion {
    param([Parameter(Mandatory = $true)][string]$Binary)

    $output = Invoke-CheckedProcess -FilePath $Binary -Arguments @("--version")
    if ($output.Length -eq 0 -or $output.Length -gt 256 -or
        $output.IndexOf([char]0) -ge 0) {
        throw "Installed zanna returned invalid version output."
    }
    $versionMatch = [regex]::Match(
        $output,
        '\Azanna v(?<version>[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)(?:\r?\n|\z)',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $versionMatch.Success) {
        throw "Installed zanna returned malformed version output."
    }
    return $versionMatch.Groups["version"].Value
}

function Get-InstallerMetadata {
    param([Parameter(Mandatory = $true)][string]$Path)

    $jsonPath = Join-Path ([IO.Path]::GetTempPath()) `
        ("zanna-installer-inspect-{0}.json" -f [Guid]::NewGuid().ToString("N"))
    try {
        $result = Invoke-CapturedProcess -FilePath $Path `
            -Arguments @("/inspect", "/output", $jsonPath)
        if ($result.ExitCode -ne 0) {
            throw "Installer inspection failed for $Path`n$($result.Stdout)`n$($result.Stderr)"
        }
        if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
            throw "Installer inspection did not create $jsonPath."
        }
        $jsonItem = Get-Item -LiteralPath $jsonPath -Force
        if (($jsonItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $jsonItem.Length -le 0 -or $jsonItem.Length -gt $MaximumInspectBytes) {
            throw "Installer inspection output has an invalid type or size."
        }
        $json = [IO.File]::ReadAllText(
            $jsonPath, [Text.UTF8Encoding]::new($false, $true))
        $metadata = $json | ConvertFrom-Json
    } catch {
        throw "Installer /inspect did not return valid JSON for $Path`: $($_.Exception.Message)"
    } finally {
        Remove-Item -LiteralPath $jsonPath -Force -ErrorAction SilentlyContinue
    }
    $requiredFields = @(
        "schema_version", "kind", "identifier", "display_name", "version",
        "architecture", "channel", "default_scope", "default_install_dir", "components"
    )
    foreach ($field in $requiredFields) {
        if ($metadata.PSObject.Properties.Name -notcontains $field) {
            throw "Installer /inspect is missing required schema-3 field '$field'."
        }
    }
    [int]$schemaVersion = 0
    if (-not [int]::TryParse(
            [string]$metadata.schema_version,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$schemaVersion) -or
        $schemaVersion -ne 3 -or $metadata.kind -cne "toolchain") {
        throw "Installer does not expose the required native schema-3 toolchain identity."
    }
    $identifierText = [string]$metadata.identifier
    $displayNameText = [string]$metadata.display_name
    $versionText = [string]$metadata.version
    $channelText = [string]$metadata.channel
    $architectureText = [string]$metadata.architecture
    $scopeText = [string]$metadata.default_scope
    $installDirText = [string]$metadata.default_install_dir
    if ($identifierText -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
        [string]::IsNullOrWhiteSpace($displayNameText) -or $displayNameText.Length -gt 256 -or
        [string]::IsNullOrWhiteSpace($versionText) -or $versionText.Length -gt 128 -or
        $channelText -cnotmatch '^[a-z0-9][a-z0-9-]{0,63}$' -or
        $architectureText -notin @("x64", "arm64") -or
        $scopeText -notin @("user", "machine") -or
        [string]::IsNullOrWhiteSpace($installDirText) -or $installDirText.Length -gt 128 -or
        $installDirText.IndexOfAny([char[]]('\', '/', ':')) -ge 0 -or
        $installDirText -in @(".", "..") -or $installDirText.EndsWith(".") -or
        $installDirText.EndsWith(" ")) {
        throw "Installer /inspect contains an invalid schema-3 identity value."
    }
    $componentNames = @($metadata.components)
    if ($componentNames.Count -gt 16) {
        throw "Installer /inspect contains too many components."
    }
    $seenComponents = @{}
    foreach ($component in $componentNames) {
        $componentText = [string]$component
        if ($componentText -cnotmatch '^[a-z0-9][a-z0-9-]{0,31}$' -or
            $seenComponents.ContainsKey($componentText)) {
            throw "Installer /inspect contains an invalid or duplicate component."
        }
        $seenComponents[$componentText] = $true
    }
    return $metadata
}

function Wait-PathAbsent {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$Seconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline -and (Test-Path -LiteralPath $Path)) {
        Start-Sleep -Milliseconds 100
    }
    return -not (Test-Path -LiteralPath $Path)
}

function Test-PathEntry {
    param([AllowNull()][string]$Value, [Parameter(Mandatory = $true)][string]$Expected)

    foreach ($entry in @($Value -split ";")) {
        if ($entry -and [string]::Equals(
                $entry.Trim().TrimEnd('\'),
                $Expected.TrimEnd('\'),
                [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Assert-AssociationState {
    param(
        [Parameter(Mandatory = $true)][string]$ClassesRoot,
        [Parameter(Mandatory = $true)][string]$Identifier,
        [Parameter(Mandatory = $true)][bool]$Expected
    )

    foreach ($extension in @("zia", "bas", "il")) {
        $progId = "$Identifier.$extension"
        $openWith = Join-Path $ClassesRoot ".$extension\OpenWithProgids"
        $present = $false
        if (Test-Path -LiteralPath $openWith) {
            $properties = Get-ItemProperty -LiteralPath $openWith
            $present = $properties.PSObject.Properties.Name -contains $progId
        }
        if ($present -ne $Expected) {
            throw "Open With state for $progId was $present; expected $Expected."
        }
        $progIdPath = Join-Path $ClassesRoot $progId
        if ((Test-Path -LiteralPath $progIdPath) -ne $Expected) {
            throw "ProgID state for $progIdPath did not match expected state $Expected."
        }
    }
}

function Assert-AdministratorForMachineScope {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Machine-scope validation must run from an elevated PowerShell session."
    }
}

$installerPath = (Resolve-Path -LiteralPath $Installer).Path
Assert-NoReparseAncestors -Path $installerPath -Description "Installer input"
$metadata = Get-InstallerMetadata $installerPath
$baselinePath = $null
if (-not [string]::IsNullOrWhiteSpace($BaselineInstaller)) {
    $baselinePath = (Resolve-Path -LiteralPath $BaselineInstaller).Path
    Assert-NoReparseAncestors -Path $baselinePath -Description "Baseline installer input"
    $baselineMetadata = Get-InstallerMetadata $baselinePath
    if ($baselineMetadata.identifier -ne $metadata.identifier -or
        $baselineMetadata.architecture -ne $metadata.architecture) {
        throw "Baseline and current installers must have the same identifier and architecture."
    }
    if ($baselineMetadata.version -eq $metadata.version) {
        throw "BaselineInstaller must describe a different version for upgrade validation."
    }
}

if ([string]::IsNullOrWhiteSpace($Scope)) {
    $Scope = [string]$metadata.default_scope
}
$Scope = $Scope.ToLowerInvariant()
if ($Scope -notin @("user", "machine")) {
    throw "Scope must be user or machine."
}
if ($Scope -eq "machine") {
    Assert-AdministratorForMachineScope
}

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $base = if ($Scope -eq "machine") {
        $env:ProgramFiles
    } else {
        Join-Path $env:LOCALAPPDATA "Programs"
    }
    if ([string]::IsNullOrWhiteSpace($base)) {
        throw "Cannot resolve the default base directory for $Scope scope."
    }
    $InstallRoot = Join-Path $base ([string]$metadata.default_install_dir)
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
if ($InstallRoot -eq [IO.Path]::GetPathRoot($InstallRoot) -or $InstallRoot.Length -lt 8) {
    throw "Refusing unsafe validation install root: $InstallRoot"
}
$windowsRoot = [IO.Path]::GetFullPath($env:SystemRoot)
if (Test-PathWithin -Base $windowsRoot -Candidate $InstallRoot -AllowBase) {
    throw "Validation install root must not be the Windows directory or one of its descendants."
}
if ((Test-Path -LiteralPath $InstallRoot) -and
    -not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    throw "Validation install root is not a directory: $InstallRoot"
}
Assert-NoReparseAncestors -Path $InstallRoot -Description "Validation install root"
foreach ($protectedRoot in @(
        $env:SystemRoot,
        $env:ProgramFiles,
        $env:ProgramData,
        $env:LOCALAPPDATA,
        $env:USERPROFILE,
        [IO.Path]::GetTempPath())) {
    if (-not [string]::IsNullOrWhiteSpace($protectedRoot) -and
        [string]::Equals(
            $InstallRoot.TrimEnd('\', '/'),
            [IO.Path]::GetFullPath($protectedRoot).TrimEnd('\', '/'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Validation install root must be below, not equal to, a protected Windows directory."
    }
}

$identifier = [string]$metadata.identifier
$machineScope = $Scope -eq "machine"
$classesRoot = if ($machineScope) { "HKLM:\Software\Classes" } else { "HKCU:\Software\Classes" }
$uninstallRegistry = if ($machineScope) {
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$identifier"
} else {
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$identifier"
}
$pathScope = if ($machineScope) { "Machine" } else { "User" }
$maintenanceCacheBase = if ($machineScope) { $env:ProgramData } else { $env:LOCALAPPDATA }
if ([string]::IsNullOrWhiteSpace($maintenanceCacheBase)) {
    throw "Cannot resolve the Windows maintenance-cache base directory."
}
$maintenanceCacheRoot = Join-Path $maintenanceCacheBase "Zanna\InstallerCache"
$installedPathEntry = Join-Path $InstallRoot "bin"
$startMenuBase = if ($machineScope) {
    Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs"
} else {
    Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
}
$startMenu = Join-Path $startMenuBase ([string]$metadata.default_install_dir)
$work = Join-Path ([IO.Path]::GetTempPath()) (
    "zanna-installer-vm-" + [Guid]::NewGuid().ToString("N"))
$powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
$originalPath = $env:Path
$originalZannaLibPath = [Environment]::GetEnvironmentVariable("ZANNA_LIB_PATH", "Process")
$unownedSentinel =
    Join-Path $InstallRoot ("validator-unowned-{0}.txt" -f [Guid]::NewGuid().ToString("N"))
$stalePath = if ($UpgradeStaleRelativePath) {
    Resolve-SafeRelativePath -Root $InstallRoot -RelativePath $UpgradeStaleRelativePath `
        -Description "UpgradeStaleRelativePath"
} else {
    $null
}
$installationAttempted = $false
$installRootWasAbsentBeforeInstall = $false
$maintenanceCache = $null
$components = @($metadata.components)
if (-not (Test-PathWithin -Base ([IO.Path]::GetTempPath()) -Candidate $work)) {
    throw "Refusing validation workspace outside the Windows temporary directory: $work"
}
New-Item -ItemType Directory -Path $work | Out-Null
Assert-NoReparseAncestors -Path $work -Description "Validation workspace"

try {
    $nativeArchitecture = $env:PROCESSOR_ARCHITEW6432
    if ([string]::IsNullOrWhiteSpace($nativeArchitecture)) {
        $nativeArchitecture = $env:PROCESSOR_ARCHITECTURE
    }
    if ([string]::IsNullOrWhiteSpace($nativeArchitecture)) {
        throw "Cannot determine the native Windows validation host architecture."
    }
    $hostArchitecture = switch ($nativeArchitecture.ToLowerInvariant()) {
        "arm64" { "arm64" }
        "amd64" { "x64" }
        "x86_64" { "x64" }
        default { throw "Unsupported Windows validation host architecture '$nativeArchitecture'." }
    }
    if ($metadata.architecture -ne $hostArchitecture) {
        throw "Package architecture $($metadata.architecture) does not match validation host $hostArchitecture."
    }

    if ($RequireSignature) {
        Invoke-CheckedProcess -FilePath $SignToolPath `
            -Arguments @("verify", "/pa", "/all", "/tw", "/v", $installerPath) | Out-Null
    }

    $existingProduct = Test-Path -LiteralPath $uninstallRegistry
    if ($existingProduct -and -not $ReplaceExisting) {
        throw "A product with identifier $identifier is already installed. Use a disposable host " +
            "or pass -ReplaceExisting to remove it explicitly."
    }
    if ($existingProduct) {
        $existing = Get-ItemProperty -LiteralPath $uninstallRegistry
        $existingMaintenance = if ($existing.PSObject.Properties.Name -contains
            "ZannaMaintenanceCache") {
            [string]$existing.ZannaMaintenanceCache
        } else {
            ""
        }
        if ([string]::IsNullOrWhiteSpace($existingMaintenance) -or
            -not (Test-Path -LiteralPath $existingMaintenance -PathType Leaf)) {
            throw "Existing product has no verified maintenance cache: $identifier"
        }
        $existingMaintenance = [IO.Path]::GetFullPath($existingMaintenance)
        if (-not (Test-PathWithin -Base $maintenanceCacheRoot -Candidate $existingMaintenance) -or
            [IO.Path]::GetFileName($existingMaintenance) -ine "maintenance.exe") {
            throw "Existing product maintenance cache is outside the owned cache root."
        }
        Assert-NoReparseAncestors -Path $existingMaintenance `
            -Description "Existing maintenance cache"
        if ($RequireSignature) {
            Invoke-CheckedProcess -FilePath $SignToolPath `
                -Arguments @("verify", "/pa", "/all", "/tw", "/v", $existingMaintenance) |
                Out-Null
        }
        $existingMetadata = Get-InstallerMetadata $existingMaintenance
        if ($existingMetadata.identifier -ne $metadata.identifier -or
            $existingMetadata.architecture -ne $metadata.architecture) {
            throw "Existing maintenance cache identity does not match the selected package."
        }
        Invoke-CheckedProcess -FilePath $existingMaintenance `
            -Arguments @("/uninstall", "/quiet", "/norestart") -SuccessCodes @(0, 3010) | Out-Null
        if (-not (Wait-PathAbsent -Path $uninstallRegistry)) {
            throw "Existing product registration did not disappear after uninstall."
        }
        if (-not (Wait-PathAbsent -Path $existingMaintenance)) {
            throw "Existing product maintenance cache was not detached after uninstall."
        }
    }
    if (Test-Path -LiteralPath $InstallRoot) {
        $preinstallEntries = @(Get-ChildItem -LiteralPath $InstallRoot -Force)
        if ($preinstallEntries.Count -ne 0) {
            throw "Validation install root contains unrelated pre-existing content: $InstallRoot"
        }
    } else {
        $installRootWasAbsentBeforeInstall = $true
    }

    $installArguments = @(
        "/install", "/quiet", "/norestart", "/scope", $Scope,
        "/installDir", $InstallRoot, "/type", "complete",
        "/addToPath", "/associations", "/shortcuts"
    )
    if ($baselinePath) {
        $installationAttempted = $true
        Invoke-CheckedProcess -FilePath $baselinePath -Arguments $installArguments `
            -SuccessCodes @(0, 3010) | Out-Null
        if ($stalePath -and -not (Test-Path -LiteralPath $stalePath -PathType Leaf)) {
            throw "Baseline installer omitted the expected stale-owned file: $stalePath"
        }
        New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
        Write-NewSentinel -Path $unownedSentinel `
            -Text "preserve-unowned-upgrade-content"
    }

    $installationAttempted = $true
    Invoke-CheckedProcess -FilePath $installerPath -Arguments $installArguments `
        -SuccessCodes @(0, 3010) | Out-Null

    if ($stalePath -and $baselinePath -and (Test-Path -LiteralPath $stalePath)) {
        throw "Upgrade left a file owned only by the baseline package: $stalePath"
    }
    if ($baselinePath -and
        [IO.File]::ReadAllText($unownedSentinel) -ne "preserve-unowned-upgrade-content") {
        throw "Upgrade modified unrelated content in the install tree."
    }

    if (-not (Test-Path -LiteralPath $uninstallRegistry)) {
        throw "Expected Apps & Features registration at $uninstallRegistry"
    }
    $arp = Get-ItemProperty -LiteralPath $uninstallRegistry
    foreach ($requiredProperty in @(
            "DisplayName", "DisplayVersion", "Publisher", "InstallLocation",
            "UninstallString", "QuietUninstallString", "ModifyPath", "RepairPath",
            "EstimatedSize", "ZannaMaintenanceCache")) {
        if ($arp.PSObject.Properties.Name -notcontains $requiredProperty -or
            [string]::IsNullOrWhiteSpace([string]$arp.$requiredProperty)) {
            throw "Apps & Features metadata is missing $requiredProperty."
        }
    }
    if (-not [string]::Equals(
            [IO.Path]::GetFullPath([string]$arp.InstallLocation).TrimEnd('\'),
            $InstallRoot.TrimEnd('\'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Apps & Features InstallLocation does not match the selected root."
    }
    if ([string]$arp.DisplayName -ne [string]$metadata.display_name -or
        [string]$arp.DisplayVersion -ne [string]$metadata.version) {
        throw "Apps & Features identity does not match the inspected package."
    }
    $maintenanceCache = [string]$arp.ZannaMaintenanceCache
    if (-not (Test-Path -LiteralPath $maintenanceCache -PathType Leaf)) {
        throw "Verified maintenance cache is missing: $maintenanceCache"
    }
    $maintenanceCache = [IO.Path]::GetFullPath($maintenanceCache)
    if (-not (Test-PathWithin -Base $maintenanceCacheRoot -Candidate $maintenanceCache) -or
        [IO.Path]::GetFileName($maintenanceCache) -ine "maintenance.exe") {
        throw "Installed maintenance cache is outside the owned cache root."
    }
    Assert-NoReparseAncestors -Path $maintenanceCache `
        -Description "Installed maintenance cache"
    $maintenanceMetadata = Get-InstallerMetadata $maintenanceCache
    if ($maintenanceMetadata.identifier -ne $metadata.identifier -or
        $maintenanceMetadata.version -ne $metadata.version -or
        $maintenanceMetadata.architecture -ne $metadata.architecture) {
        throw "Installed maintenance cache identity does not match the package."
    }

    $requiredTools = @(
        "zanna", "zia", "zbasic", "ilrun", "il-verify", "il-dis",
        "zia-server", "zbasic-server", "basic-ast-dump", "basic-lex-dump"
    )
    if ($components -contains "zannastudio") {
        $requiredTools += "zannastudio"
    }
    foreach ($tool in $requiredTools) {
        $toolPath = Join-Path $InstallRoot "bin\$tool.exe"
        if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
            throw "Expected installed tool: $toolPath"
        }
        Assert-NoReparseAncestors -Path $toolPath -Description "Installed tool"
        if ((Get-PeArchitecture -Binary $toolPath) -ne [string]$metadata.architecture) {
            throw "Installed tool architecture does not match the package: $toolPath"
        }
    }
    $zanna = Join-Path $InstallRoot "bin\zanna.exe"
    $studioProductVersion = ""
    if ($components -contains "zannastudio") {
        $studioProductVersion = Get-ZannaProductVersion -Binary $zanna
        Assert-ZannaStudioBuildInfo `
            -Binary (Join-Path $InstallRoot "bin\zannastudio.exe") `
            -BuildInfo (Join-Path $InstallRoot "bin\zannastudio.buildinfo") `
            -Architecture ([string]$metadata.architecture) `
            -Version $studioProductVersion
    }

    $developerPrompt = Join-Path $InstallRoot "bin\zanna-dev.cmd"
    if (-not (Test-Path -LiteralPath $developerPrompt -PathType Leaf)) {
        throw "Expected installed developer prompt: $developerPrompt"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $startMenu "Zanna Developer Prompt.lnk"))) {
        throw "Expected developer prompt Start Menu shortcut under $startMenu"
    }
    if ($components -contains "zannastudio" -and
        -not (Test-Path -LiteralPath (Join-Path $startMenu "Zanna Studio.lnk"))) {
        throw "Expected Zanna Studio Start Menu shortcut under $startMenu"
    }

    if (-not (Test-PathEntry `
            -Value ([Environment]::GetEnvironmentVariable("Path", $pathScope)) `
            -Expected $installedPathEntry)) {
        throw "Installer did not add its selected PATH entry: $installedPathEntry"
    }
    Assert-AssociationState -ClassesRoot $classesRoot -Identifier $identifier -Expected $true

    if ($components.Count -gt 1) {
        Invoke-CheckedProcess -FilePath $maintenanceCache -Arguments @(
            "/modify", "/quiet", "/norestart", "/type", "minimal",
            "/noPath", "/noAssociations", "/noShortcuts") -SuccessCodes @(0, 3010) | Out-Null
        if ($components -contains "zannastudio" -and
            (Test-Path -LiteralPath (Join-Path $InstallRoot "bin\zannastudio.exe"))) {
            throw "Minimal modify left the Zanna Studio component installed."
        }
        if (Test-PathEntry `
                -Value ([Environment]::GetEnvironmentVariable("Path", $pathScope)) `
                -Expected $installedPathEntry) {
            throw "Minimal modify left an integration that was disabled: PATH"
        }
        Assert-AssociationState -ClassesRoot $classesRoot -Identifier $identifier -Expected $false

        Invoke-CheckedProcess -FilePath $maintenanceCache -Arguments @(
            "/modify", "/quiet", "/norestart", "/type", "complete",
            "/addToPath", "/associations", "/shortcuts") -SuccessCodes @(0, 3010) | Out-Null
        Assert-AssociationState -ClassesRoot $classesRoot -Identifier $identifier -Expected $true
        if ($components -contains "zannastudio") {
            Assert-ZannaStudioBuildInfo `
                -Binary (Join-Path $InstallRoot "bin\zannastudio.exe") `
                -BuildInfo (Join-Path $InstallRoot "bin\zannastudio.buildinfo") `
                -Architecture ([string]$metadata.architecture) `
                -Version $studioProductVersion
        }
    }

    $expectedZannaHash = (Get-FileHash -LiteralPath $zanna -Algorithm SHA256).Hash
    if (-not (Test-Path -LiteralPath $unownedSentinel)) {
        Write-NewSentinel -Path $unownedSentinel `
            -Text "preserve-unowned-repair-content"
    }
    [IO.File]::WriteAllText($zanna, "intentionally-corrupt-for-repair")
    Invoke-CheckedProcess -FilePath $maintenanceCache `
        -Arguments @("/repair", "/quiet", "/norestart") -SuccessCodes @(0, 3010) | Out-Null
    if ((Get-FileHash -LiteralPath $zanna -Algorithm SHA256).Hash -ne $expectedZannaHash) {
        throw "Repair did not restore the exact owned zanna.exe bytes."
    }
    if (-not (Test-Path -LiteralPath $unownedSentinel -PathType Leaf)) {
        throw "Repair removed unrelated developer content."
    }

    if ($RequireSignature) {
        Invoke-CheckedProcess -FilePath $SignToolPath `
            -Arguments @("verify", "/pa", "/all", "/tw", "/v", $maintenanceCache) | Out-Null
        foreach ($pe in Get-ChildItem -LiteralPath (Join-Path $InstallRoot "bin") `
                -File -Recurse |
                Where-Object { $_.Extension -in @(".exe", ".dll") }) {
            Invoke-CheckedProcess -FilePath $SignToolPath `
                -Arguments @("verify", "/pa", "/all", "/tw", "/v", $pe.FullName) | Out-Null
        }
    }

    Invoke-CheckedProcess -FilePath $zanna -Arguments @("--version") | Out-Host
    $pathFromRegistry = ([Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                         [Environment]::GetEnvironmentVariable("Path", "User"))
    $env:Path = $pathFromRegistry
    try {
        Invoke-CheckedProcess -FilePath $powershell -Arguments @(
            "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-Command", "zanna --version") | Out-Host
    } finally {
        $env:Path = $originalPath
    }

    $basic = Join-Path $work "installer-run-smoke.bas"
    [IO.File]::WriteAllText($basic, '10 PRINT "INSTALLER-RUN-SMOKE"')
    $runOut = Invoke-CheckedProcess -FilePath $zanna -Arguments @("run", $basic)
    if ($runOut -notmatch "INSTALLER-RUN-SMOKE") {
        throw "zanna run produced unexpected output: $runOut"
    }

    $il = Join-Path $work "installer-native-smoke.il"
    $nativeExe = Join-Path $work "installer-native-smoke.exe"
    Remove-Item Env:ZANNA_LIB_PATH -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText($il, @'
il 0.3.0

extern @Zanna.Terminal.PrintStr(str) -> void
global const str @.msg = "INSTALLER-NATIVE-SMOKE"

func @main() -> i64 {
entry:
  %msg = const_str @.msg
  call @Zanna.Terminal.PrintStr(%msg)
  ret 0
}
'@)
    Invoke-CheckedProcess -FilePath $zanna -Arguments @(
        "codegen", [string]$metadata.architecture, $il, "-o", $nativeExe) `
        -WorkingDirectory $work | Out-Null
    $nativeOut = Invoke-CheckedProcess -FilePath $nativeExe -WorkingDirectory $work
    if ($nativeOut -notmatch "INSTALLER-NATIVE-SMOKE") {
        throw "Native smoke produced unexpected output: $nativeOut"
    }

    if ($components -contains "sdk") {
        $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
        $cmd = Join-Path $env:SystemRoot "System32\cmd.exe"
        $consumerSource = Join-Path $work "cmake-consumer-source"
        $consumerBuild = Join-Path $work "cmake-consumer-build"
        New-Item -ItemType Directory -Path $consumerSource -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $consumerSource "CMakeLists.txt"), @'
cmake_minimum_required(VERSION 3.20)
project(zanna_installer_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Zanna CONFIG REQUIRED)
add_executable(zanna_installer_consumer main.cpp)
target_link_libraries(zanna_installer_consumer PRIVATE zanna::il_core zanna::il_io)
'@)
        [IO.File]::WriteAllText((Join-Path $consumerSource "main.cpp"), @'
#include <sstream>
#include <zanna/il/core/Module.hpp>
#include <zanna/il/io/Serializer.hpp>

int main() {
    il::core::Module module;
    std::ostringstream out;
    il::io::Serializer::write(module, out);
    return out.str().empty() ? 1 : 0;
}
'@)
        $consumerDriver = Join-Path $work "build-cmake-consumer.cmd"
        $developerPromptBatch = $developerPrompt.Replace("%", "%%")
        $cmakeBatch = $cmake.Replace("%", "%%")
        $consumerSourceBatch = $consumerSource.Replace("%", "%%")
        $consumerBuildBatch = $consumerBuild.Replace("%", "%%")
        [IO.File]::WriteAllText($consumerDriver, @"
@echo off
chcp 65001 >nul
call "$developerPromptBatch"
if errorlevel 1 exit /b %errorlevel%
"$cmakeBatch" -S "$consumerSourceBatch" -B "$consumerBuildBatch"
if errorlevel 1 exit /b %errorlevel%
"$cmakeBatch" --build "$consumerBuildBatch" --config Release
exit /b %errorlevel%
"@)
        Invoke-CheckedProcess -FilePath $cmd -Arguments @("/d", "/c", $consumerDriver) | Out-Host
        $consumerExe = @(
            (Join-Path $consumerBuild "Release\zanna_installer_consumer.exe"),
            (Join-Path $consumerBuild "zanna_installer_consumer.exe")
        ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if (-not $consumerExe) {
            throw "CMake consumer build did not produce zanna_installer_consumer.exe."
        }
        Invoke-CheckedProcess -FilePath $consumerExe | Out-Null
    }

    Write-Host "Windows installer validation passed for $($metadata.display_name) " `
        "$($metadata.version) $($metadata.architecture)."
} finally {
    $env:Path = $originalPath
    [Environment]::SetEnvironmentVariable(
        "ZANNA_LIB_PATH", $originalZannaLibPath, "Process")
    if (-not $KeepInstalled -and $installationAttempted -and
        (Test-Path -LiteralPath $uninstallRegistry)) {
        $rootUninstaller = Join-Path $InstallRoot "uninstall.exe"
        $cleanupExecutable = if (Test-Path -LiteralPath $rootUninstaller -PathType Leaf) {
            $rootUninstaller
        } elseif ($maintenanceCache -and
            (Test-Path -LiteralPath $maintenanceCache -PathType Leaf)) {
            $maintenanceCache
        } else {
            throw "Installed product has no available uninstaller."
        }
        Invoke-CheckedProcess -FilePath $cleanupExecutable `
            -Arguments @("/uninstall", "/quiet", "/norestart") -SuccessCodes @(0, 3010) | Out-Null
        if (-not (Wait-PathAbsent -Path $uninstallRegistry)) {
            throw "Uninstaller left Apps & Features registration: $uninstallRegistry"
        }
        if ($maintenanceCache -and -not (Wait-PathAbsent -Path $maintenanceCache)) {
            throw "Detached cleanup left the maintenance cache: $maintenanceCache"
        }
        foreach ($ownedPath in @(
                (Join-Path $InstallRoot "bin\zanna.exe"),
                (Join-Path $InstallRoot "uninstall.exe"),
                (Join-Path $InstallRoot ".zanna-install-manifest.txt"))) {
            if (Test-Path -LiteralPath $ownedPath) {
                throw "Uninstaller left an owned path: $ownedPath"
            }
        }
        Assert-AssociationState -ClassesRoot $classesRoot -Identifier $identifier -Expected $false
        if (Test-PathEntry `
                -Value ([Environment]::GetEnvironmentVariable("Path", $pathScope)) `
                -Expected $installedPathEntry) {
            throw "Uninstaller left its PATH entry: $installedPathEntry"
        }
        if (Test-Path -LiteralPath $startMenu) {
            throw "Uninstaller left its Start Menu directory: $startMenu"
        }
        if (Test-Path -LiteralPath $unownedSentinel -PathType Leaf) {
            $sentinelText = [IO.File]::ReadAllText($unownedSentinel)
            if ($sentinelText -notmatch '^preserve-unowned-') {
                throw "Uninstaller modified unrelated content: $unownedSentinel"
            }
            Remove-Item -LiteralPath $unownedSentinel -Force
        }
        if (Test-Path -LiteralPath $InstallRoot) {
            $remaining = @(Get-ChildItem -LiteralPath $InstallRoot -Force -Recurse)
            if ($remaining.Count -ne 0) {
                throw "Uninstaller left unexpected content under $InstallRoot`: " +
                    (($remaining | Select-Object -ExpandProperty FullName) -join ", ")
            }
            Remove-Item -LiteralPath $InstallRoot -Force
        }
    } elseif (-not $KeepInstalled -and $installationAttempted -and
              $installRootWasAbsentBeforeInstall -and
              (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
        Assert-NoReparseAncestors -Path $InstallRoot `
            -Description "Partially installed validation root"
        if (-not (Test-PathWithin -Base ([IO.Path]::GetPathRoot($InstallRoot)) `
                -Candidate $InstallRoot)) {
            throw "Refusing to clean an unsafe partial installation root: $InstallRoot"
        }
        if (@(Get-ChildItem -LiteralPath $InstallRoot -Force -Recurse |
                Where-Object {
                    ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
                }).Count -ne 0) {
            throw "Refusing to recursively clean a partial installation containing reparse points."
        }
        Remove-Item -LiteralPath $InstallRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $work) {
        $resolvedWork = (Resolve-Path -LiteralPath $work).Path
        if (-not (Test-PathWithin -Base ([IO.Path]::GetTempPath()) `
                -Candidate $resolvedWork)) {
            throw "Refusing to clean unexpected validation workspace: $resolvedWork"
        }
        if (@(Get-ChildItem -LiteralPath $resolvedWork -Force -Recurse |
                Where-Object {
                    ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
                }).Count -ne 0) {
            throw "Refusing to recursively clean a validation workspace containing reparse points."
        }
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
