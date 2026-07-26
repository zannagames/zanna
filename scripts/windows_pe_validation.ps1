#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/windows_pe_validation.ps1
# Purpose: Provide one bounded PE32+ validator for Windows automation scripts.
# Key invariants:
#   - Validation holds a read-only, write-denying handle for the complete inspection.
#   - Loader-facing offsets, alignments, sections, entry point, and directories are checked.
#   - Only non-DLL x64 and ARM64 Windows GUI/console executables are accepted.
# Ownership/Lifetime: Functions borrow caller paths and dispose every stream before returning.
# Links: WindowsPEValidation.cpp, build_demos_win.ps1, build_ide_win.ps1
# Cross-platform touchpoints: This helper is Windows-only and mirrors the native
#                             packaging validator's architecture and loader policy.
#
#===----------------------------------------------------------------------===#

Set-StrictMode -Version 2.0

function Read-ZannaPeUInt16 {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)][uint64]$Offset,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $length = [uint64]$Stream.Length
    if ($Offset -gt $length -or 2 -gt $length - $Offset) {
        throw "truncated $Description"
    }
    $Stream.Position = [int64]$Offset
    return [uint16]$Reader.ReadUInt16()
}

function Read-ZannaPeUInt32 {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)][uint64]$Offset,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $length = [uint64]$Stream.Length
    if ($Offset -gt $length -or 4 -gt $length - $Offset) {
        throw "truncated $Description"
    }
    $Stream.Position = [int64]$Offset
    return [uint32]$Reader.ReadUInt32()
}

function Test-ZannaPePowerOfTwo {
    param([Parameter(Mandatory = $true)][uint64]$Value)

    return $Value -ne 0 -and ($Value -band ($Value - 1)) -eq 0
}

function Get-ZannaPeAlignedValue {
    param(
        [Parameter(Mandatory = $true)][uint64]$Value,
        [Parameter(Mandatory = $true)][uint64]$Alignment
    )

    if (-not (Test-ZannaPePowerOfTwo -Value $Alignment)) {
        throw "invalid alignment"
    }
    $mask = $Alignment - 1
    if ($Value -gt [uint64]::MaxValue - $mask) {
        throw "alignment overflow"
    }
    return [uint64](($Value + $mask) -band (-bnot $mask))
}

function Test-ZannaPeRangesOverlap {
    param(
        [Parameter(Mandatory = $true)]$Left,
        [Parameter(Mandatory = $true)]$Right
    )

    return [uint64]$Left.Begin -lt [uint64]$Right.End -and
        [uint64]$Right.Begin -lt [uint64]$Left.End
}

function Get-ZannaPeImageInfo {
    param([Parameter(Mandatory = $true)][string]$Binary)

    if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) {
        throw "Windows executable is missing or is not a regular file: $Binary"
    }
    $item = Get-Item -LiteralPath $Binary -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Windows executable must not be a reparse point: $Binary"
    }
    $linkType = $item.PSObject.Properties["LinkType"]
    if ($linkType -and $linkType.Value -eq "HardLink") {
        throw "Windows executable must not be a hard-link alias: $Binary"
    }

    $stream = [IO.File]::Open(
        $item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $fileLength = [uint64]$stream.Length
        if ($fileLength -lt 64) {
            throw "file is too small for a PE header"
        }
        $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::ASCII, $true)
        try {
            if ((Read-ZannaPeUInt16 -Reader $reader -Stream $stream -Offset 0 `
                    -Description "DOS header") -ne 0x5A4D) {
                throw "missing DOS MZ magic"
            }
            $peOffset = [uint64](Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset 60 -Description "DOS PE offset")
            if ($peOffset -lt 64 -or $peOffset -gt $fileLength -or
                24 -gt $fileLength - $peOffset) {
                throw "e_lfanew is out of range or overlaps the DOS header"
            }
            if ((Read-ZannaPeUInt32 -Reader $reader -Stream $stream -Offset $peOffset `
                    -Description "PE signature") -ne 0x00004550) {
                throw "invalid PE signature"
            }

            $coffOffset = $peOffset + 4
            $machine = Read-ZannaPeUInt16 -Reader $reader -Stream $stream `
                -Offset $coffOffset -Description "COFF machine"
            if ($machine -ne 0x8664 -and $machine -ne 0xAA64) {
                throw ("unsupported COFF machine 0x{0:X4}" -f $machine)
            }
            $sectionCount = Read-ZannaPeUInt16 -Reader $reader -Stream $stream `
                -Offset ($coffOffset + 2) -Description "COFF section count"
            $optionalSize = Read-ZannaPeUInt16 -Reader $reader -Stream $stream `
                -Offset ($coffOffset + 16) -Description "COFF optional-header size"
            $characteristics = Read-ZannaPeUInt16 -Reader $reader -Stream $stream `
                -Offset ($coffOffset + 18) -Description "COFF characteristics"
            if ($sectionCount -lt 1 -or $sectionCount -gt 96) {
                throw "invalid PE section count"
            }
            if (($characteristics -band 0x0002) -eq 0 -or
                ($characteristics -band 0x2000) -ne 0) {
                throw "image is not a non-DLL executable"
            }

            $optionalOffset = $coffOffset + 20
            if ($optionalSize -lt 112 -or $optionalOffset -gt $fileLength -or
                [uint64]$optionalSize -gt $fileLength - $optionalOffset) {
                throw "truncated PE32+ optional header"
            }
            if ((Read-ZannaPeUInt16 -Reader $reader -Stream $stream `
                    -Offset $optionalOffset -Description "optional-header magic") -ne 0x020B) {
                throw "image is not PE32+"
            }

            $entryPoint = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 16) -Description "entry point"
            $sectionAlignment = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 32) -Description "section alignment"
            $fileAlignment = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 36) -Description "file alignment"
            $sizeOfImage = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 56) -Description "SizeOfImage"
            $sizeOfHeaders = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 60) -Description "SizeOfHeaders"
            $subsystem = Read-ZannaPeUInt16 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 68) -Description "subsystem"
            $directoryCount = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                -Offset ($optionalOffset + 108) -Description "data-directory count"
            if ($entryPoint -eq 0) {
                throw "image has no entry point"
            }
            if ($subsystem -ne 2 -and $subsystem -ne 3) {
                throw "unsupported Windows subsystem"
            }
            if (-not (Test-ZannaPePowerOfTwo -Value $sectionAlignment) -or
                -not (Test-ZannaPePowerOfTwo -Value $fileAlignment) -or
                $sectionAlignment -lt $fileAlignment) {
                throw "invalid PE section/file alignment relationship"
            }
            if (($sectionAlignment -lt 4096 -and $fileAlignment -ne $sectionAlignment) -or
                ($sectionAlignment -ge 4096 -and
                    ($fileAlignment -lt 512 -or $fileAlignment -gt 65536))) {
                throw "PE file alignment is outside loader limits"
            }
            if ($sizeOfImage -eq 0 -or ($sizeOfImage % $sectionAlignment) -ne 0) {
                throw "SizeOfImage is zero or misaligned"
            }
            if ($sizeOfHeaders -eq 0 -or $sizeOfHeaders -gt $fileLength -or
                ($sizeOfHeaders % $fileAlignment) -ne 0) {
                throw "SizeOfHeaders is zero, out of range, or misaligned"
            }
            if ($directoryCount -gt 16 -or
                [uint64]$directoryCount * 8 -gt [uint64]$optionalSize - 112) {
                throw "invalid data-directory count"
            }

            $sectionTableOffset = $optionalOffset + [uint64]$optionalSize
            $sectionTableLength = [uint64]$sectionCount * 40
            if ($sectionTableOffset -gt $fileLength -or
                $sectionTableLength -gt $fileLength - $sectionTableOffset) {
                throw "truncated PE section table"
            }
            $sectionTableEnd = $sectionTableOffset + $sectionTableLength
            if ([uint64]$sizeOfHeaders -lt $sectionTableEnd) {
                throw "SizeOfHeaders does not cover the section table"
            }
            $firstSectionRva =
                Get-ZannaPeAlignedValue -Value $sizeOfHeaders -Alignment $sectionAlignment
            if ($firstSectionRva -gt $sizeOfImage) {
                throw "headers cannot be mapped inside SizeOfImage"
            }

            $rawRanges = [Collections.Generic.List[object]]::new()
            $virtualRanges = [Collections.Generic.List[object]]::new()
            [uint64]$requiredImageEnd = $firstSectionRva
            $entryPointFound = $false
            for ($index = 0; $index -lt $sectionCount; ++$index) {
                $sectionOffset = $sectionTableOffset + [uint64]$index * 40
                $virtualSize = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset ($sectionOffset + 8) -Description "section virtual size"
                $virtualAddress = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset ($sectionOffset + 12) -Description "section virtual address"
                $rawSize = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset ($sectionOffset + 16) -Description "section raw size"
                $rawOffset = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset ($sectionOffset + 20) -Description "section raw offset"
                $sectionCharacteristics = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset ($sectionOffset + 36) -Description "section characteristics"
                $mappedSize = [Math]::Max([uint64]$virtualSize, [uint64]$rawSize)
                if (($virtualAddress % $sectionAlignment) -ne 0 -or
                    $virtualAddress -lt $firstSectionRva) {
                    throw "section virtual address is misaligned or overlaps headers"
                }
                $virtualEnd = [uint64]$virtualAddress + $mappedSize
                if ($virtualEnd -gt $sizeOfImage) {
                    throw "section virtual range exceeds SizeOfImage"
                }
                if ($mappedSize -gt 0) {
                    $virtualRanges.Add([pscustomobject]@{
                            Begin = [uint64]$virtualAddress
                            End = $virtualEnd
                            Characteristics = [uint32]$sectionCharacteristics
                        })
                    $alignedEnd = Get-ZannaPeAlignedValue -Value $virtualEnd `
                        -Alignment $sectionAlignment
                    if ($alignedEnd -gt $requiredImageEnd) {
                        $requiredImageEnd = $alignedEnd
                    }
                }

                if ($rawSize -eq 0) {
                    if ($rawOffset -ne 0) {
                        throw "empty section has a nonzero raw-data offset"
                    }
                } else {
                    if (($rawOffset % $fileAlignment) -ne 0 -or
                        ($rawSize % $fileAlignment) -ne 0) {
                        throw "section raw storage is not file-aligned"
                    }
                    $rawEnd = [uint64]$rawOffset + [uint64]$rawSize
                    if ($rawOffset -lt $sizeOfHeaders -or $rawEnd -gt $fileLength) {
                        throw "section raw storage is outside the file"
                    }
                    $rawRanges.Add([pscustomobject]@{
                            Begin = [uint64]$rawOffset
                            End = $rawEnd
                            Characteristics = [uint32]$sectionCharacteristics
                        })
                }
                if ($entryPoint -ge $virtualAddress -and $entryPoint -lt $virtualEnd) {
                    if (($sectionCharacteristics -band 0x20000000) -eq 0) {
                        throw "entry point belongs to a non-executable section"
                    }
                    $entryPointFound = $true
                }
            }
            if (-not $entryPointFound) {
                throw "entry point is outside every executable section"
            }
            if ($requiredImageEnd -gt $sizeOfImage) {
                throw "SizeOfImage does not cover aligned section extents"
            }

            $sortedRaw = @($rawRanges | Sort-Object -Property Begin, End)
            $sortedVirtual = @($virtualRanges | Sort-Object -Property Begin, End)
            for ($index = 1; $index -lt $sortedRaw.Count; ++$index) {
                if (Test-ZannaPeRangesOverlap -Left $sortedRaw[$index - 1] `
                        -Right $sortedRaw[$index]) {
                    throw "PE sections have overlapping raw storage"
                }
            }
            for ($index = 1; $index -lt $sortedVirtual.Count; ++$index) {
                if (Test-ZannaPeRangesOverlap -Left $sortedVirtual[$index - 1] `
                        -Right $sortedVirtual[$index]) {
                    throw "PE sections have overlapping virtual ranges"
                }
            }

            for ($index = 0; $index -lt $directoryCount; ++$index) {
                $directoryOffset = $optionalOffset + 112 + [uint64]$index * 8
                $address = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset $directoryOffset -Description "data-directory address"
                $directorySize = Read-ZannaPeUInt32 -Reader $reader -Stream $stream `
                    -Offset ($directoryOffset + 4) -Description "data-directory size"
                if (($address -eq 0) -ne ($directorySize -eq 0)) {
                    throw "data directory has a partial zero range"
                }
                if ($address -eq 0) {
                    continue
                }
                $directoryEnd = [uint64]$address + [uint64]$directorySize
                if ($index -eq 4) {
                    if (($address -band 7) -ne 0 -or
                        ($directorySize -band 7) -ne 0 -or
                        $address -lt $sizeOfHeaders -or $directoryEnd -gt $fileLength) {
                        throw "Authenticode certificate table is misaligned or out of range"
                    }
                    $certificateRange =
                        [pscustomobject]@{ Begin = [uint64]$address; End = $directoryEnd }
                    foreach ($rawRange in $sortedRaw) {
                        if (Test-ZannaPeRangesOverlap -Left $certificateRange -Right $rawRange) {
                            throw "Authenticode certificate table overlaps PE section storage"
                        }
                    }
                    continue
                }
                if ($directoryEnd -gt $sizeOfImage) {
                    throw "data directory is outside mapped image content"
                }
                $mapped = $address -lt $directoryEnd -and
                    $directoryEnd -le $sizeOfHeaders
                if (-not $mapped) {
                    foreach ($virtualRange in $sortedVirtual) {
                        if ($address -ge $virtualRange.Begin -and
                            $directoryEnd -le $virtualRange.End) {
                            $mapped = $true
                            break
                        }
                    }
                }
                if (-not $mapped) {
                    throw "data directory is outside mapped image content"
                }
            }

            $architecture = if ($machine -eq 0xAA64) { "arm64" } else { "x64" }
            return [pscustomobject]@{
                Architecture = $architecture
                Machine = [uint16]$machine
                Subsystem = [uint16]$subsystem
                EntryPoint = [uint32]$entryPoint
                SizeOfImage = [uint32]$sizeOfImage
                SizeOfHeaders = [uint32]$sizeOfHeaders
            }
        } finally {
            $reader.Dispose()
        }
    } catch {
        throw "Invalid Windows PE '$Binary': $($_.Exception.Message)"
    } finally {
        $stream.Dispose()
    }
}

function Get-ZannaPeImageArchitecture {
    param([Parameter(Mandatory = $true)][string]$Binary)

    return [string](Get-ZannaPeImageInfo -Binary $Binary).Architecture
}

function Assert-ZannaPeImageArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)]
        [ValidateSet("arm64", "x64")]
        [string]$Architecture
    )

    $actual = Get-ZannaPeImageArchitecture -Binary $Binary
    if (-not [string]::Equals(
            $actual, $Architecture, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Windows executable architecture $actual does not match requested $Architecture`: $Binary"
    }
}
