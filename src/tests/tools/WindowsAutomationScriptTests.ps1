#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/WindowsAutomationScriptTests.ps1
# Purpose: Verify failure-atomic signing and Windows installer/demo-driver safety contracts.
# Key invariants:
#   - A failed signer cannot replace an existing artifact or its metadata.
#   - Successful signing publishes both the artifact and canonical hash metadata.
#   - Signing validates output ancestry before and after directory creation.
#   - Demo automation confines external roots and authorizes destructive cleanup exactly.
#   - Studio artifacts are staged, PE-validated, provenance-bound, and pair-published.
#   - The cmd.exe demo compatibility entry point remains a logic-free forwarding shim.
#   - Installer automation recognizes every existing-input spelling and requires Studio by default.
#   - Canonical Windows builds do not clean an unconfigured or absent build tree.
#   - End-to-end validation has bounded child processes and path-confined cleanup.
# Ownership/Lifetime: The caller-owned work directory contains all temporary fixtures.
# Links: scripts/sign-windows-installer.ps1, scripts/build_demos_win.ps1,
#        scripts/build_ide_win.ps1, scripts/build_installer.ps1,
#        scripts/build_zanna_win.ps1,
#        scripts/validate-windows-toolchain-installer.ps1
#
#===----------------------------------------------------------------------===#

param(
    [Parameter(Mandatory = $true)][string]$SignScript,
    [Parameter(Mandatory = $true)][string]$DemoScript,
    [Parameter(Mandatory = $true)][string]$IdeScript,
    [Parameter(Mandatory = $true)][string]$InstallerScript,
    [Parameter(Mandatory = $true)][string]$BuildScript,
    [Parameter(Mandatory = $true)][string]$ValidatorScript,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-PowerShellScript {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string[]]$Arguments = @()
    )
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Path @Arguments `
            2>$null | Out-Null
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
}

$root = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
[void](New-Item -ItemType Directory -Path $root)

$input = Join-Path $root "unsigned.exe"
$output = Join-Path $root "signed.exe"
$metadata = "$output.sha256.txt"
$failingSigner = Join-Path $root "failing-signtool.cmd"
$successfulSigner = Join-Path $root "successful-signtool.cmd"
[IO.File]::WriteAllBytes($input, [byte[]](0x4D, 0x5A, 0x01, 0x02, 0x03))
[IO.File]::WriteAllText($output, "existing-artifact", [Text.Encoding]::ASCII)
[IO.File]::WriteAllText($metadata, "existing-metadata", [Text.Encoding]::ASCII)
[IO.File]::WriteAllText($failingSigner, "@echo off`r`nexit /b 23`r`n", [Text.Encoding]::ASCII)
[IO.File]::WriteAllText($successfulSigner, "@echo off`r`nexit /b 0`r`n", [Text.Encoding]::ASCII)

$common = @(
    "-InputPath", $input,
    "-OutputPath", $output,
    "-Thumbprint", ("A" * 40),
    "-TimestampUrl", "https://timestamp.example.test",
    "-SignToolPath"
)
$status = Invoke-PowerShellScript -Path $SignScript -Arguments ($common + $failingSigner)
Assert-True ($status -ne 0) "A failing signtool unexpectedly succeeded."
Assert-True ([IO.File]::ReadAllText($output) -eq "existing-artifact") `
    "Failed signing replaced the existing artifact."
Assert-True ([IO.File]::ReadAllText($metadata) -eq "existing-metadata") `
    "Failed signing replaced the existing metadata."
Assert-True (@(Get-ChildItem -LiteralPath $root -Filter ".*.tmp").Count -eq 0) `
    "Failed signing left a staging file behind."

$status = Invoke-PowerShellScript -Path $SignScript -Arguments ($common + $successfulSigner)
Assert-True ($status -eq 0) "The successful signing fixture failed."
Assert-True ((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash -eq
             (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash) `
    "The staged signing artifact was not published."
$metadataText = [IO.File]::ReadAllText($metadata)
Assert-True ($metadataText.Contains("unsigned_sha256 ") -and
             $metadataText.Contains("signed_sha256 ") -and
             $metadataText.Contains("timestamp_url https://timestamp.example.test")) `
    "Signed-artifact metadata is incomplete."

$inPlace = Join-Path $root "in-place.exe"
[IO.File]::WriteAllBytes($inPlace, [byte[]](0x4D, 0x5A, 0x05, 0x06))
$status = Invoke-PowerShellScript -Path $SignScript -Arguments @(
    "-InputPath", $inPlace,
    "-Thumbprint", ("A" * 40),
    "-TimestampUrl", "https://timestamp.example.test",
    "-SignToolPath", $successfulSigner
)
Assert-True ($status -eq 0) "Rollback-protected in-place signing failed."
Assert-True (Test-Path -LiteralPath "$inPlace.sha256.txt" -PathType Leaf) `
    "In-place signing did not publish its metadata pair."

$blockedOutput = Join-Path $root "blocked-output.exe"
$blockedMetadata = "$blockedOutput.sha256.txt"
[IO.File]::WriteAllText($blockedOutput, "preserve-blocked-output", [Text.Encoding]::ASCII)
[void](New-Item -ItemType Directory -Path $blockedMetadata)
$status = Invoke-PowerShellScript -Path $SignScript -Arguments @(
    "-InputPath", $input,
    "-OutputPath", $blockedOutput,
    "-Thumbprint", ("A" * 40),
    "-TimestampUrl", "https://timestamp.example.test",
    "-SignToolPath", $successfulSigner
)
Assert-True ($status -ne 0) "A directory-valued metadata destination was accepted."
Assert-True ([IO.File]::ReadAllText($blockedOutput) -eq "preserve-blocked-output") `
    "Metadata preflight failure replaced the existing signed output."

$racingOutput = Join-Path $root "racing-output.exe"
$racingSigner = Join-Path $root "racing-signtool.cmd"
[IO.File]::WriteAllText($racingOutput, "preserve-racing-output", [Text.Encoding]::ASCII)
$escapedInput = $input.Replace("%", "%%")
[IO.File]::WriteAllText(
    $racingSigner,
    "@echo off`r`n> `"$escapedInput`" echo changed-during-signing`r`nexit /b 0`r`n",
    [Text.Encoding]::ASCII)
$status = Invoke-PowerShellScript -Path $SignScript -Arguments @(
    "-InputPath", $input,
    "-OutputPath", $racingOutput,
    "-Thumbprint", ("A" * 40),
    "-TimestampUrl", "https://timestamp.example.test",
    "-SignToolPath", $racingSigner
)
Assert-True ($status -ne 0) "An input mutation during signing was not detected."
Assert-True ([IO.File]::ReadAllText($racingOutput) -eq "preserve-racing-output") `
    "Input-race failure replaced the existing output."

$status = Invoke-PowerShellScript -Path $SignScript -Arguments @(
    "-InputPath", $input,
    "-OutputPath", $output,
    "-Thumbprint", ("A" * 40),
    "-TimestampUrl", "https://user:secret@timestamp.example.test",
    "-SignToolPath", $successfulSigner
)
Assert-True ($status -ne 0) "A credential-bearing timestamp URL was accepted."

$status = Invoke-PowerShellScript -Path $SignScript -Arguments @(
    "-InputPath", $inPlace,
    "-Thumbprint", ("A" * 40),
    "-TimestampUrl", "https://timestamp.example.test/path with-space",
    "-SignToolPath", $successfulSigner
)
Assert-True ($status -ne 0) "A whitespace-bearing timestamp URL was accepted."

$signSource = [IO.File]::ReadAllText($SignScript)
$outputPreflight = $signSource.IndexOf(
    'Assert-FileDestination -Path $outputFull',
    [StringComparison]::Ordinal)
$directoryCreate = $signSource.IndexOf(
    '[IO.Directory]::CreateDirectory($parent)',
    [StringComparison]::Ordinal)
$outputRecheck = if ($outputPreflight -ge 0) {
    $signSource.IndexOf(
        'Assert-FileDestination -Path $outputFull',
        $outputPreflight + 1,
        [StringComparison]::Ordinal)
} else {
    -1
}
Assert-True ($outputPreflight -ge 0 -and $directoryCreate -gt $outputPreflight -and
             $outputRecheck -gt $directoryCreate) `
    "The signing script does not validate output ancestry around directory creation."
Assert-True ($signSource.Contains('Signing artifact staging path') -and
             $signSource.Contains('Signed installer backup path')) `
    "The signing script does not preflight every staging and backup destination."

$peValidationScript = Join-Path (Split-Path -Parent $DemoScript) "windows_pe_validation.ps1"
Assert-True (Test-Path -LiteralPath $peValidationScript -PathType Leaf) `
    "The shared Windows PE validator is missing."
. $peValidationScript
$validPe = Join-Path $root "valid-pe.exe"
[IO.File]::Copy($env:ComSpec, $validPe, $false)
$systemPe = Get-ZannaPeImageInfo -Binary $validPe
Assert-True ($systemPe.Architecture -eq "x64" -or $systemPe.Architecture -eq "arm64") `
    "The shared PE validator rejected the native command processor architecture."
Assert-ZannaPeImageArchitecture -Binary $validPe -Architecture $systemPe.Architecture
$wrongArchitecture = if ($systemPe.Architecture -eq "arm64") { "x64" } else { "arm64" }
$rejectedWrongArchitecture = $false
try {
    Assert-ZannaPeImageArchitecture -Binary $validPe -Architecture $wrongArchitecture
} catch {
    $rejectedWrongArchitecture = $true
}
Assert-True $rejectedWrongArchitecture `
    "The shared PE validator accepted a mismatched target architecture."

$invalidEntryPointPe = Join-Path $root "invalid-entrypoint.exe"
[IO.File]::Copy($validPe, $invalidEntryPointPe, $false)
$mutationStream = [IO.File]::Open(
    $invalidEntryPointPe, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $reader = [IO.BinaryReader]::new($mutationStream, [Text.Encoding]::ASCII, $true)
    $writer = [IO.BinaryWriter]::new($mutationStream, [Text.Encoding]::ASCII, $true)
    try {
        $mutationStream.Position = 60
        $peOffset = [uint64]$reader.ReadUInt32()
        $mutationStream.Position = [int64]($peOffset + 24 + 16)
        $writer.Write([uint32]0)
        $writer.Flush()
        $mutationStream.Flush($true)
    } finally {
        $writer.Dispose()
        $reader.Dispose()
    }
} finally {
    $mutationStream.Dispose()
}
$rejectedInvalidEntryPoint = $false
try {
    [void](Get-ZannaPeImageInfo -Binary $invalidEntryPointPe)
} catch {
    $rejectedInvalidEntryPoint = $_.Exception.Message.Contains("no entry point")
}
Assert-True $rejectedInvalidEntryPoint `
    "The shared PE validator accepted an image with no entry point."

$peValidationSource = [IO.File]::ReadAllText($peValidationScript)
Assert-True ($peValidationSource.Contains("FileShare]::Read") -and
             $peValidationSource.Contains("entry point belongs to a non-executable section") -and
             $peValidationSource.Contains("overlapping raw storage") -and
             $peValidationSource.Contains("Authenticode certificate table overlaps") -and
             $peValidationSource.Contains("data directory is outside mapped image content")) `
    "The shared script validator lacks stable-snapshot or loader-structure checks."

$status = Invoke-PowerShellScript -Path $DemoScript -Arguments @("--help")
Assert-True ($status -eq 0) "The Windows demo driver help path failed."
$demoSource = [IO.File]::ReadAllText($DemoScript)
Assert-True ($demoSource.Contains("windows_pe_validation.ps1")) `
    "The demo driver does not use the shared PE validator."
Assert-True ($demoSource.Contains('src\tools\zanna\zanna.exe')) `
    "The demo driver lacks single-config executable discovery."
Assert-True ($demoSource.Contains('Assert-CMakeTreeArchitecture')) `
    "The demo driver lacks CMake architecture validation."
Assert-True ($demoSource.Contains('Get-CMakeGeneratedSystemProcessor')) `
    "The demo driver cannot prove architecture from generated CMake system data."
Assert-True ($demoSource.Contains('Test-PathWithin')) `
    "The demo driver lacks asset and project path confinement."
Assert-True ($demoSource.Contains('Assert-NoReparseAbsolutePath') -and
             $demoSource.Contains('Assert-DemoPathConfiguration') -and
             $demoSource.Contains('Demo output root must not contain protected path')) `
    "The demo driver does not validate external-root ancestry or protected-path overlap."
Assert-True ($demoSource.Contains('duplicate demo executable name')) `
    "The demo driver lacks duplicate-output rejection."
Assert-True ($demoSource.Contains('$outputDirectory = Join-Path $binDir $Name')) `
    "The demo driver does not isolate each demo's published executable and assets."
Assert-True ($demoSource.Contains("Assert-NoReparsePath") -and
             $demoSource.Contains("Assert-PortableExecutableArchitecture") -and
             $demoSource.Contains("Publish-DemoDirectory") -and
             $demoSource.Contains("[IO.Directory]::Move(`$stageFull, `$destinationFull)")) `
    "The demo driver lacks indirection checks or transactional directory publication."
Assert-True ($demoSource.Contains("New-DemoRunDirectory") -and
             $demoSource.Contains("Stop-DemoProcessTree") -and
             $demoSource.Contains("taskkill.exe") -and
             -not $demoSource.Contains(
                 '$killer.ExitCode -ne 0 -and -not $Process.HasExited')) `
    "The demo driver does not isolate smoke runs or terminate child process trees."
Assert-True ($demoSource.Contains('WaitForExit($processStopTimeoutMilliseconds)') -and
             -not $demoSource.Contains('$process.WaitForExit()')) `
    "The demo driver retains an unbounded redirected-process wait."
Assert-True ($demoSource.Contains("ZANNA_DEMO_MAX_OUTPUT_BYTES") -and
             $demoSource.Contains("[Diagnostics.Stopwatch]::StartNew()") -and
             $demoSource.Contains("output exceeded the")) `
    "The demo driver does not bound smoke output with a monotonic deadline."
Assert-True ($demoSource.Contains("SourceDirectory `$stageDirectory") -and
             $demoSource.Contains("Stage-DemoAssets -ProjectDir `$ProjectDir " +
                                  "-DestinationRoot `$stageDirectory") -and
             $demoSource.Contains("Existing demo publication tree")) `
    "The demo driver does not smoke and publish one complete executable/asset generation."
Assert-True (([regex]::Matches(
                 $demoSource, [regex]::Escape('& $zanna @buildArguments'))).Count -eq 1) `
    "The demo driver still recompiles a failed project to recover diagnostics."
Assert-True ($demoSource.Contains("Remove-DemoRunDirectoryEntry -Path `$entry.FullName") -and
             -not $demoSource.Contains(
                 "Remove-Item -LiteralPath `$entry.FullName -Recurse -Force")) `
    "The demo clean path can recursively traverse a reparse point."
Assert-True ($demoSource.Contains('Test-PathsEqual -Left $binDir -Right $cleanOwnedBinDir') -and
             -not $demoSource.Contains(
                 '[IO.Path]::GetFullPath($binDir) -ne $expectedBinDir')) `
    "The demo clean path does not require the conventional owned output root."
Assert-True ($demoSource.Contains('ZANNA_DEMO_BIN_DIR must be set to') -and
             $demoSource.Contains('Assert-DemoPathConfiguration') -and
             $demoSource.IndexOf('Assert-DemoPathConfiguration') -lt
                 $demoSource.IndexOf('Ensure-ZannaBuild -Tree')) `
    "Unsafe demo output configuration is not rejected before tool builds and cleanup."
Assert-True ($demoSource.Contains("ConvertFrom-NativeArgumentString -Value `$trimmed") -and
             $demoSource.Contains("malformed asset directive")) `
    "The demo driver does not parse quoted asset paths fail-closed."
Assert-True ($demoSource.Contains("cannot prove its target architecture") -and
             $demoSource.Contains("Unsupported native Windows host architecture")) `
    "The demo driver still guesses unknown CMake or native-host architectures."
Assert-True (-not $demoSource.Contains("Get-DemoBinSnapshot") -and
             -not $demoSource.Contains("Remove-NewDemoArtifacts")) `
    "Demo smoke cleanup can still mutate the shared published output directory."
Assert-True ($demoSource.Contains("Read-SafeAutomationLines") -and
             $demoSource.Contains("reserved Windows device name") -and
             $demoSource.Contains('CLOCK\$') -and
             $demoSource.Contains('CONIN\$') -and
             $demoSource.Contains('CONOUT\$') -and
             $demoSource.Contains('[char]127') -and
             $demoSource.Contains("[IO.FileMode]::CreateNew") -and
             $demoSource.Contains("<generated demo executable>") -and
             $demoSource.Contains("Published demo generation")) `
    "The demo driver lacks bounded metadata, Windows path, no-clobber, or publication checks."
Assert-True (([regex]::Matches(
                 $demoSource,
                 [regex]::Escape(
                     "Assert-PortableExecutableArchitecture -Binary `$stage " +
                     "-Architecture `$demoArch"))).Count -ge 2 -and
             $demoSource.Contains("Demo directory staging tree") -and
             $demoSource.Contains("Published demo generation")) `
    "The demo driver does not revalidate executable identity across staging and publication."
Assert-True (-not $demoSource.Contains(
                 "Get-ChildItem -LiteralPath `$Root -Force -Recurse")) `
    "The demo driver still recursively follows an unbounded source-tree enumeration."
Assert-True ($demoSource.Contains('JOBS must be an integer from 1 through 1024') -and
             $demoSource.Contains('Cannot determine the native Windows host architecture') -and
             $demoSource.Contains('switch ($buildTypeValue.ToLowerInvariant())')) `
    "The demo driver does not bound worker fanout or normalize host/build configuration."
Assert-True ($demoSource.Contains('Demo manifest rows must not have surrounding whitespace') -and
             $demoSource.Contains("'^[a-z0-9][a-z0-9_-]*$'") -and
             $demoSource.Contains('switch -CaseSensitive ($category)') -and
             $demoSource.Contains("must declare 'lang zia'")) `
    "The Windows demo manifest contract still diverges from the cross-platform audit."
Assert-True ($demoSource.Contains('Demo asset entry name') -and
             $demoSource.Contains('-RequireUtf8') -and
             $demoSource.Contains('contains an unsupported byte-order mark')) `
    "Demo metadata and recursively copied asset names are not canonicalized fail-closed."
Assert-True ($demoSource.Contains('$killer.Kill()') -and
             $demoSource.Contains('$killer.WaitForExit($processStopTimeoutMilliseconds)') -and
             $demoSource.Contains('Could not fully terminate demo') -and
             $demoSource.Contains('$succeeded = $false')) `
    "Demo smoke teardown can report success without reaping its process-tree helper."

$demoCmd = [IO.Path]::ChangeExtension($DemoScript, ".cmd")
Assert-True (Test-Path -LiteralPath $demoCmd -PathType Leaf) `
    "The cmd.exe demo compatibility entry point is missing."
$savedPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = "Continue"
    & $env:ComSpec /d /c "`"$demoCmd`" --help" 2>$null | Out-Null
    $status = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedPreference
}
Assert-True ($status -eq 0) "The cmd.exe demo compatibility help path failed."
$cmdSource = [IO.File]::ReadAllText($demoCmd)
Assert-True ($cmdSource.Contains("build_demos_win.ps1") -and
             $cmdSource.Contains("%*") -and
             $cmdSource.Contains("%ERRORLEVEL%")) `
    "The cmd.exe demo shim does not forward arguments and status."
Assert-True (-not $cmdSource.Contains("cmake") -and -not $cmdSource.Contains("zanna build")) `
    "The cmd.exe demo shim duplicates build logic."

$status = Invoke-PowerShellScript -Path $IdeScript -Arguments @("--help")
Assert-True ($status -eq 0) "The Windows Zanna Studio driver help path failed."
$ideSource = [IO.File]::ReadAllText($IdeScript)
Assert-True ($ideSource.Contains("windows_pe_validation.ps1")) `
    "The Studio driver does not use the shared PE validator."
Assert-True ($ideSource.Contains("PROCESSOR_ARCHITEW6432") -and
             $ideSource.Contains("Resolve-ZannaExecutable") -and
             $ideSource.Contains("Assert-CMakeTreeArchitecture")) `
    "The Studio driver lacks native-host and CMake-tree architecture handling."
Assert-True ($ideSource.Contains("Assert-PortableExecutableArchitecture") -and
             $ideSource.Contains('"Schema: 1"') -and
             $ideSource.Contains('"SHA256: $sha256"')) `
    "The Studio driver lacks PE and provenance validation."
Assert-True ($ideSource.Contains("Publish-StudioArtifact") -and
             $ideSource.Contains("zanna-backup")) `
    "The Studio driver does not publish the binary/buildinfo pair transactionally."
Assert-True ($ideSource.Contains("Read-BoundedUtf8Lines") -and
             $ideSource.Contains("cannot prove its target architecture") -and
             $ideSource.Contains("Published Studio artifact pair changed") -and
             $ideSource.Contains("Assert-NoReparseTree")) `
    "The Studio driver lacks bounded provenance, exact architecture, or publication checks."

$status = Invoke-PowerShellScript -Path $InstallerScript -Arguments @("--help")
Assert-True ($status -eq 0) "The Windows installer wrapper help path failed."
$installerSource = [IO.File]::ReadAllText($InstallerScript)
Assert-True ($installerSource.Contains("windows_pe_validation.ps1")) `
    "The installer wrapper does not use the shared PE validator."
Assert-True ($installerSource.Contains('"$inputOption="') -and
             $installerSource.Contains("normalizedArguments") -and
             $installerSource.Contains("Specify at most one of")) `
    "The installer wrapper does not recognize equals-form existing inputs."
Assert-True ($installerSource.Contains("ZANNA_INSTALL_ZANNASTUDIO") -and
             $installerSource.Contains("zannastudio.buildinfo") -and
             $installerSource.Contains("Assert-ZannaStudioArtifact") -and
             $installerSource.Contains('settingType -ine "BOOL"')) `
    "The installer wrapper does not verify the default Zanna Studio build."
Assert-True ($installerSource.Contains("[IO.Path]::GetFullPath(`$buildDir)")) `
    "The installer wrapper does not normalize an absolute build directory."
Assert-True ($installerSource.Contains("Invoke-StagedPackageDriver") -and
             $installerSource.Contains(".zanna-installer-driver-") -and
             $installerSource.Contains("[IO.FileMode]::CreateNew") -and
             $installerSource.Contains("failed SHA-256 verification") -and
             -not $installerSource.Contains("& `$zanna @packageArguments")) `
    "The installer wrapper can execute a package driver from a relink target."

$buildSource = [IO.File]::ReadAllText($BuildScript)
Assert-True ($buildSource.Contains(
                 'Test-Path -LiteralPath (Join-Path $buildRoot "CMakeCache.txt") -PathType Leaf') -and
             $buildSource.Contains('} elseif ($configuredBuild) {') -and
             $buildSource.Contains(
                 'Skipping pre-configure clean because the build tree is not configured.')) `
    "The canonical Windows build cleans an absent or unconfigured build tree."
Assert-True ($buildSource.Contains('must be 0 or 1; received') -and
             $buildSource.Contains('ZANNA_SKIP_STUDIO') -and
             $buildSource.Contains('must be an integer from 1 through 1024')) `
    "The canonical Windows build does not bound boolean controls and worker counts."
$extraCmakePosition = $buildSource.IndexOf(
    '$configArguments += @(ConvertFrom-NativeArgumentString -Value $extraArguments)')
$studioPolicyPosition = $buildSource.IndexOf(
    '$configArguments += "-DZANNA_INSTALL_ZANNASTUDIO=$studioInstallSetting"')
Assert-True ($extraCmakePosition -ge 0 -and $studioPolicyPosition -gt $extraCmakePosition) `
    "Extra CMake arguments can override the canonical Windows Studio-build policy."

$validatorSource = [IO.File]::ReadAllText($ValidatorScript)
Assert-True ($validatorSource.Contains("windows_pe_validation.ps1")) `
    "The installer lifecycle validator does not use the shared PE validator."
Assert-True ($validatorSource.Contains("ProcessTimeoutSeconds") -and
             $validatorSource.Contains("MaximumCaptureBytes") -and
             $validatorSource.Contains("MaximumInspectBytes") -and
             $validatorSource.Contains("Process timed out after") -and
             $validatorSource.Contains("[Diagnostics.Stopwatch]::StartNew()")) `
    "The installer validator lacks bounded process and inspect handling."
Assert-True ($validatorSource.Contains("Stop-CheckedProcessTree") -and
             $validatorSource.Contains("taskkill.exe") -and
             $validatorSource.Contains("did not reap within 10 seconds") -and
             -not $validatorSource.Contains(
                 '$killer.ExitCode -ne 0 -and -not $Process.HasExited') -and
             -not $validatorSource.Contains("[void]`$process.Kill()")) `
    "The installer validator does not terminate and confirm complete process trees."
Assert-True ($validatorSource.Contains("Resolve-SafeRelativePath") -and
             $validatorSource.Contains("Test-PathWithin") -and
             $validatorSource.Contains("[IO.FileMode]::CreateNew")) `
    "The installer validator lacks stale-path confinement and no-clobber sentinels."
Assert-True ($validatorSource.Contains("Existing maintenance cache identity") -and
             $validatorSource.Contains("Assert-ZannaStudioBuildInfo") -and
             $validatorSource.Contains("PROCESSOR_ARCHITEW6432")) `
    "The installer validator lacks cache identity, Studio provenance, or native-host checks."
Assert-True ($validatorSource.Contains("Get-ZannaProductVersion") -and
             $validatorSource.Contains('-Version $studioProductVersion') -and
             $validatorSource.Contains('(?:\r?\n|\z)') -and
             -not $validatorSource.Contains('\r?\n?\z') -and
             -not $validatorSource.Contains('-Version ([string]$metadata.version)')) `
    "The installer validator conflates version domains or rejects canonical multiline output."

Write-Host "Windows automation script tests passed."
