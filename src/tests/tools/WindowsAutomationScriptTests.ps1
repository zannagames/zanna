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
#   - Demo automation retains single-config lookup and path-confinement guards.
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

$status = Invoke-PowerShellScript -Path $DemoScript -Arguments @("--help")
Assert-True ($status -eq 0) "The Windows demo driver help path failed."
$demoSource = [IO.File]::ReadAllText($DemoScript)
Assert-True ($demoSource.Contains('src\tools\zanna\zanna.exe')) `
    "The demo driver lacks single-config executable discovery."
Assert-True ($demoSource.Contains('Assert-CMakeTreeArchitecture')) `
    "The demo driver lacks CMake architecture validation."
Assert-True ($demoSource.Contains('Get-CMakeGeneratedSystemProcessor')) `
    "The demo driver cannot prove architecture from generated CMake system data."
Assert-True ($demoSource.Contains('Test-PathWithin')) `
    "The demo driver lacks asset and project path confinement."
Assert-True ($demoSource.Contains('duplicate demo executable name')) `
    "The demo driver lacks duplicate-output rejection."
Assert-True ($demoSource.Contains('$outputDirectory = Join-Path $binDir $Name')) `
    "The demo driver does not isolate each demo's published executable and assets."
Assert-True ($demoSource.Contains("Assert-NoReparsePath") -and
             $demoSource.Contains("Assert-PortableExecutableArchitecture") -and
             $demoSource.Contains("Publish-DemoExecutable")) `
    "The demo driver lacks indirection checks or transactional PE publication."
Assert-True ($demoSource.Contains("New-DemoRunDirectory") -and
             $demoSource.Contains("Stop-DemoProcessTree") -and
             $demoSource.Contains("taskkill.exe")) `
    "The demo driver does not isolate smoke runs or terminate child process trees."
Assert-True ($demoSource.Contains('WaitForExit($processStopTimeoutMilliseconds)') -and
             -not $demoSource.Contains('$process.WaitForExit()')) `
    "The demo driver retains an unbounded redirected-process wait."
Assert-True ($demoSource.Contains("ConvertFrom-NativeArgumentString -Value `$trimmed") -and
             $demoSource.Contains("malformed asset directive")) `
    "The demo driver does not parse quoted asset paths fail-closed."
Assert-True ($demoSource.Contains("cannot prove its target architecture") -and
             $demoSource.Contains("Unsupported native Windows host architecture")) `
    "The demo driver still guesses unknown CMake or native-host architectures."
Assert-True (-not $demoSource.Contains("Get-DemoBinSnapshot") -and
             -not $demoSource.Contains("Remove-NewDemoArtifacts")) `
    "Demo smoke cleanup can still mutate the shared published output directory."

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

$status = Invoke-PowerShellScript -Path $InstallerScript -Arguments @("--help")
Assert-True ($status -eq 0) "The Windows installer wrapper help path failed."
$installerSource = [IO.File]::ReadAllText($InstallerScript)
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

$validatorSource = [IO.File]::ReadAllText($ValidatorScript)
Assert-True ($validatorSource.Contains("ProcessTimeoutSeconds") -and
             $validatorSource.Contains("MaximumCaptureBytes") -and
             $validatorSource.Contains("MaximumInspectBytes") -and
             $validatorSource.Contains("Process timed out after")) `
    "The installer validator lacks bounded process and inspect handling."
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
