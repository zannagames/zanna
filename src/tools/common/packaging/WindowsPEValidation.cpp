//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/WindowsPEValidation.cpp
// Purpose: Validate bounded Windows PE32+ executable structure.
// Key invariants:
//   - PE offsets and extents are widened before arithmetic and checked before reads.
//   - Raw and virtual section ranges are aligned, bounded, and mutually disjoint.
//   - The entry point and data directories name loader-mapped executable content.
// Ownership/Lifetime:
//   - All temporary range inventories are function-local.
//   - No pointer into the caller's byte snapshot escapes validation.
// Links: WindowsPEValidation.hpp, PkgVerify.cpp, WindowsInstallerHost.cpp
//
//===----------------------------------------------------------------------===//

#include "WindowsPEValidation.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace zanna::pkg {
namespace {

constexpr uint16_t kMachineX64 = 0x8664U;
constexpr uint16_t kMachineArm64 = 0xAA64U;
constexpr uint16_t kPe32PlusMagic = 0x020BU;
constexpr uint16_t kExecutableImage = 0x0002U;
constexpr uint16_t kDllImage = 0x2000U;
constexpr uint32_t kSectionExecutable = 0x20000000U;
constexpr size_t kCoffHeaderBytes = 20U;
constexpr size_t kSectionHeaderBytes = 40U;
constexpr uint32_t kMaximumSections = 96U;
constexpr uint32_t kMaximumDataDirectories = 16U;
constexpr uint32_t kSecurityDirectoryIndex = 4U;

struct ImageRange {
    uint64_t begin{0};
    uint64_t end{0};
    uint16_t section{0};
    uint32_t characteristics{0};
};

bool hasRange(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

uint16_t readLe16(const uint8_t *value) {
    return static_cast<uint16_t>(value[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(value[1]) << 8U);
}

uint32_t readLe32(const uint8_t *value) {
    return static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8U) |
           (static_cast<uint32_t>(value[2]) << 16U) | (static_cast<uint32_t>(value[3]) << 24U);
}

bool isPowerOfTwo(uint32_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool alignUp(uint64_t value, uint32_t alignment, uint64_t &result) {
    if (!isPowerOfTwo(alignment))
        return false;
    const uint64_t mask = static_cast<uint64_t>(alignment) - 1U;
    if (value > std::numeric_limits<uint64_t>::max() - mask)
        return false;
    result = (value + mask) & ~mask;
    return true;
}

bool rangesOverlap(const ImageRange &left, const ImageRange &right) {
    return left.begin < right.end && right.begin < left.end;
}

bool rangeIsMapped(uint64_t begin,
                   uint64_t end,
                   uint32_t sizeOfHeaders,
                   const std::vector<ImageRange> &virtualRanges) {
    if (begin < end && end <= sizeOfHeaders)
        return true;
    for (const ImageRange &range : virtualRanges) {
        if (begin >= range.begin && end <= range.end)
            return true;
    }
    return false;
}

bool fail(std::string &error, const char *message) {
    error = message;
    return false;
}

} // namespace

bool validateWindowsPEImage(const uint8_t *data,
                            size_t size,
                            uint16_t expectedMachine,
                            WindowsPEImageInfo *outInfo,
                            std::string &error) {
    if (outInfo)
        *outInfo = {};
    error.clear();
    if (!data || size < 64U)
        return fail(error, "file is too small for a PE header");
    if (readLe16(data) != 0x5A4DU)
        return fail(error, "missing DOS 'MZ' magic");

    const uint32_t peOffset32 = readLe32(data + 60U);
    const size_t peOffset = static_cast<size_t>(peOffset32);
    if (peOffset32 < 64U || !hasRange(peOffset, 4U + kCoffHeaderBytes, size))
        return fail(error, "e_lfanew points past end of file or overlaps the DOS header");
    if (readLe32(data + peOffset) != 0x00004550U)
        return fail(error, "invalid PE signature");

    const size_t coffOffset = peOffset + 4U;
    const uint16_t machine = readLe16(data + coffOffset);
    if (machine != kMachineX64 && machine != kMachineArm64)
        return fail(error, "unsupported COFF machine");
    if (expectedMachine != 0U && machine != expectedMachine)
        return fail(error, "COFF machine does not match the required architecture");

    const uint16_t sectionCount = readLe16(data + coffOffset + 2U);
    const uint16_t optionalSize = readLe16(data + coffOffset + 16U);
    const uint16_t characteristics = readLe16(data + coffOffset + 18U);
    if (sectionCount == 0U || sectionCount > kMaximumSections)
        return fail(error, "invalid PE section count");
    if ((characteristics & kExecutableImage) == 0U || (characteristics & kDllImage) != 0U)
        return fail(error, "image is not a non-DLL executable");

    const size_t optionalOffset = coffOffset + kCoffHeaderBytes;
    if (optionalSize < 112U || !hasRange(optionalOffset, optionalSize, size))
        return fail(error, "truncated PE32+ optional header");
    if (readLe16(data + optionalOffset) != kPe32PlusMagic)
        return fail(error, "image is not PE32+");

    const uint32_t entryPoint = readLe32(data + optionalOffset + 16U);
    const uint32_t sectionAlignment = readLe32(data + optionalOffset + 32U);
    const uint32_t fileAlignment = readLe32(data + optionalOffset + 36U);
    const uint32_t sizeOfImage = readLe32(data + optionalOffset + 56U);
    const uint32_t sizeOfHeaders = readLe32(data + optionalOffset + 60U);
    const uint16_t subsystem = readLe16(data + optionalOffset + 68U);
    const uint32_t dataDirectoryCount = readLe32(data + optionalOffset + 108U);
    if (entryPoint == 0U)
        return fail(error, "image has no entry point");
    if (subsystem != 2U && subsystem != 3U)
        return fail(error, "unsupported Windows subsystem");
    if (!isPowerOfTwo(sectionAlignment) || !isPowerOfTwo(fileAlignment) ||
        sectionAlignment < fileAlignment) {
        return fail(error, "invalid PE section/file alignment relationship");
    }
    if ((sectionAlignment < 4096U && fileAlignment != sectionAlignment) ||
        (sectionAlignment >= 4096U && (fileAlignment < 512U || fileAlignment > 65536U))) {
        return fail(error, "PE file alignment is outside loader limits");
    }
    if (sizeOfImage == 0U || (sizeOfImage % sectionAlignment) != 0U)
        return fail(error, "SizeOfImage is zero or misaligned");
    if (sizeOfHeaders == 0U || sizeOfHeaders > size || (sizeOfHeaders % fileAlignment) != 0U) {
        return fail(error, "SizeOfHeaders is zero, out of range, or misaligned");
    }
    if (dataDirectoryCount > kMaximumDataDirectories ||
        static_cast<uint64_t>(dataDirectoryCount) * 8U >
            static_cast<uint64_t>(optionalSize - 112U)) {
        return fail(error, "invalid data-directory count");
    }

    const size_t sectionTableOffset = optionalOffset + optionalSize;
    if (sectionCount > (size - sectionTableOffset) / kSectionHeaderBytes)
        return fail(error, "truncated PE section table");
    const size_t sectionTableEnd =
        sectionTableOffset + static_cast<size_t>(sectionCount) * kSectionHeaderBytes;
    if (sizeOfHeaders < sectionTableEnd)
        return fail(error, "SizeOfHeaders does not cover the section table");

    uint64_t firstSectionRva = 0;
    if (!alignUp(sizeOfHeaders, sectionAlignment, firstSectionRva) ||
        firstSectionRva > sizeOfImage) {
        return fail(error, "headers cannot be mapped inside SizeOfImage");
    }

    std::vector<ImageRange> rawRanges;
    std::vector<ImageRange> virtualRanges;
    rawRanges.reserve(sectionCount);
    virtualRanges.reserve(sectionCount);
    uint64_t requiredImageEnd = firstSectionRva;
    bool entryPointFound = false;
    for (uint16_t index = 0; index < sectionCount; ++index) {
        const size_t headerOffset =
            sectionTableOffset + static_cast<size_t>(index) * kSectionHeaderBytes;
        const uint8_t *section = data + headerOffset;
        const uint32_t virtualSize = readLe32(section + 8U);
        const uint32_t virtualAddress = readLe32(section + 12U);
        const uint32_t rawSize = readLe32(section + 16U);
        const uint32_t rawOffset = readLe32(section + 20U);
        const uint32_t sectionCharacteristics = readLe32(section + 36U);
        const uint64_t mappedSize = std::max<uint64_t>(virtualSize, rawSize);

        if ((virtualAddress % sectionAlignment) != 0U || virtualAddress < firstSectionRva)
            return fail(error, "section virtual address is misaligned or overlaps headers");
        const uint64_t virtualEnd = static_cast<uint64_t>(virtualAddress) + mappedSize;
        if (virtualEnd > sizeOfImage)
            return fail(error, "section virtual range exceeds SizeOfImage");
        if (mappedSize > 0U) {
            virtualRanges.push_back({virtualAddress, virtualEnd, index, sectionCharacteristics});
            uint64_t alignedEnd = 0;
            if (!alignUp(virtualEnd, sectionAlignment, alignedEnd))
                return fail(error, "section virtual range overflows alignment");
            requiredImageEnd = std::max(requiredImageEnd, alignedEnd);
        }

        if (rawSize == 0U) {
            if (rawOffset != 0U)
                return fail(error, "empty section has a nonzero raw-data offset");
        } else {
            if ((rawOffset % fileAlignment) != 0U || (rawSize % fileAlignment) != 0U)
                return fail(error, "section raw storage is not file-aligned");
            const uint64_t rawEnd = static_cast<uint64_t>(rawOffset) + rawSize;
            if (rawOffset < sizeOfHeaders || rawEnd > size)
                return fail(error, "section raw storage is outside the file");
            rawRanges.push_back({rawOffset, rawEnd, index, sectionCharacteristics});
        }

        if (entryPoint >= virtualAddress && entryPoint < virtualEnd) {
            if ((sectionCharacteristics & kSectionExecutable) == 0U)
                return fail(error, "entry point belongs to a non-executable section");
            entryPointFound = true;
        }
    }
    if (!entryPointFound)
        return fail(error, "entry point is outside every executable section");
    if (requiredImageEnd > sizeOfImage)
        return fail(error, "SizeOfImage does not cover aligned section extents");

    const auto byStart = [](const ImageRange &left, const ImageRange &right) {
        if (left.begin != right.begin)
            return left.begin < right.begin;
        return left.end < right.end;
    };
    std::sort(rawRanges.begin(), rawRanges.end(), byStart);
    std::sort(virtualRanges.begin(), virtualRanges.end(), byStart);
    for (size_t index = 1; index < rawRanges.size(); ++index) {
        if (rangesOverlap(rawRanges[index - 1U], rawRanges[index]))
            return fail(error, "PE sections have overlapping raw storage");
    }
    for (size_t index = 1; index < virtualRanges.size(); ++index) {
        if (rangesOverlap(virtualRanges[index - 1U], virtualRanges[index]))
            return fail(error, "PE sections have overlapping virtual ranges");
    }

    for (uint32_t index = 0; index < dataDirectoryCount; ++index) {
        const size_t directoryOffset = optionalOffset + 112U + static_cast<size_t>(index) * 8U;
        const uint32_t address = readLe32(data + directoryOffset);
        const uint32_t length = readLe32(data + directoryOffset + 4U);
        if ((address == 0U) != (length == 0U))
            return fail(error, "data directory has a partial zero range");
        if (address == 0U)
            continue;
        const uint64_t end = static_cast<uint64_t>(address) + length;
        if (index == kSecurityDirectoryIndex) {
            if ((address & 7U) != 0U || (length & 7U) != 0U || address < sizeOfHeaders ||
                end > size) {
                return fail(error, "Authenticode certificate table is misaligned or out of range");
            }
            const ImageRange certificateRange{address, end, 0U, 0U};
            for (const ImageRange &rawRange : rawRanges) {
                if (rangesOverlap(certificateRange, rawRange)) {
                    return fail(error,
                                "Authenticode certificate table overlaps PE section storage");
                }
            }
        } else if (end > sizeOfImage ||
                   !rangeIsMapped(address, end, sizeOfHeaders, virtualRanges)) {
            return fail(error, "data directory is outside mapped image content");
        }
    }

    if (outInfo) {
        outInfo->machine = machine;
        outInfo->subsystem = subsystem;
        outInfo->entryPoint = entryPoint;
        outInfo->sizeOfImage = sizeOfImage;
        outInfo->sizeOfHeaders = sizeOfHeaders;
    }
    return true;
}

} // namespace zanna::pkg
