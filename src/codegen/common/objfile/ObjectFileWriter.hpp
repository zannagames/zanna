//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/ObjectFileWriter.hpp
// Purpose: Abstract interface for object file writers (ELF, Mach-O, COFF).
//          Concrete implementations serialize CodeSection data into the
//          appropriate binary format.
// Key invariants:
//   - Each writer handles format-specific relocation mapping and symbol mangling
//   - The factory function creates the writer for the target platform
// Ownership/Lifetime:
//   - Created via factory, caller owns the unique_ptr
// Links: codegen/common/objfile/CodeSection.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ObjectFileWriter.hpp
 * @brief Declares the common ELF, Mach-O, and COFF writer interface and factory.
 *
 * Concrete writers consume the same @ref CodeSection representation and may
 * commit their result either to a filesystem path or an in-memory byte vector.
 */

#pragma once

#include "codegen/common/objfile/CodeSection.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace zanna::codegen::objfile {

/// @brief Target instruction-set architecture for a relocatable object.
enum class ObjArch : uint8_t {
    X86_64,
    AArch64,
};

/// @brief Target relocatable-object container format.
enum class ObjFormat : uint8_t {
    ELF,   ///< Linux.
    MachO, ///< macOS.
    COFF,  ///< Windows.
};

/// @brief Abstract serializer shared by all supported native object formats.
class ObjectFileWriter {
  public:
    /// @brief Destroy a writer through the polymorphic interface.
    virtual ~ObjectFileWriter() = default;

    /// @brief Supply pre-encoded DWARF `.debug_line` bytes.
    /// @param data Complete section payload; empty data disables debug-line output.
    void setDebugLineData(std::vector<uint8_t> data) {
        debugLineData_ = std::move(data);
    }

    /// @brief Supply the writable initialized-data section.
    /// @details Nonempty data is emitted as `.data` or `__DATA,__data`; its
    ///          globals may satisfy text relocations by name.
    /// @param data Section moved into writer configuration.
    void setDataSection(CodeSection data) {
        dataSection_ = std::move(data);
    }

    /// @brief Write a relocatable object with one combined text section.
    /// @param path Destination filesystem path.
    /// @param text Machine code and associated metadata.
    /// @param rodata Read-only data, which may be empty.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after the complete object is committed.
    virtual bool write(const std::string &path,
                       const CodeSection &text,
                       const CodeSection &rodata,
                       std::ostream &err) = 0;

    /// @brief Write a relocatable object from per-function text sections.
    /// @details The default implementation concatenates the sections and calls
    ///          the combined-text overload; formats that support discardable
    ///          function atoms override this behavior.
    /// @param path Destination filesystem path.
    /// @param textSections Ordered function-level sections.
    /// @param rodata Shared read-only data section.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after the complete object is committed.
    virtual bool write(const std::string &path,
                       const std::vector<CodeSection> &textSections,
                       const CodeSection &rodata,
                       std::ostream &err);

    /// @brief Serialize a single-text-section relocatable object into memory.
    /// @details Uses the same concrete ELF, Mach-O, or COFF serializer as
    ///          @ref write, but captures its completed byte buffer instead of
    ///          opening a temporary file. Writer configuration such as debug
    ///          lines and writable data sections is honored unchanged.
    /// @param output Receives the complete relocatable object bytes on success.
    /// @param text Machine-code text section.
    /// @param rodata Read-only data section, which may be empty.
    /// @param err Diagnostic stream for serialization failures.
    /// @return `true` when serialization completed and @p output is valid.
    bool writeToMemory(std::vector<uint8_t> &output,
                       const CodeSection &text,
                       const CodeSection &rodata,
                       std::ostream &err);

    /// @brief Serialize per-function text sections into an in-memory object.
    /// @details Preserves the concrete writer's per-function section behavior,
    ///          enabling the native linker to dead-strip functions without an
    ///          intermediate filesystem write/read cycle.
    /// @param output Receives the complete relocatable object bytes on success.
    /// @param textSections Ordered function text sections.
    /// @param rodata Read-only data section, which may be empty.
    /// @param err Diagnostic stream for serialization failures.
    /// @return `true` when serialization completed and @p output is valid.
    bool writeToMemory(std::vector<uint8_t> &output,
                       const std::vector<CodeSection> &textSections,
                       const CodeSection &rodata,
                       std::ostream &err);

  protected:
    /// @brief Commit a concrete writer's completed object bytes.
    /// @details Captures bytes for an active @ref writeToMemory call; otherwise
    ///          writes them through the ordinary checked file stream path.
    /// @param path Destination path used for filesystem output and diagnostics.
    /// @param bytes Fully serialized object bytes.
    /// @param writerName Concrete writer name used in diagnostics.
    /// @param err Diagnostic stream for open/write failures.
    /// @return `true` when the memory capture or complete file write succeeds.
    bool commitOutput(const std::string &path,
                      const std::vector<uint8_t> &bytes,
                      const char *writerName,
                      std::ostream &err);

    std::vector<uint8_t> debugLineData_;     ///< Pre-encoded DWARF .debug_line bytes.
    std::optional<CodeSection> dataSection_; ///< Writable initialized-data section (if any).

  private:
    std::vector<uint8_t> *memoryOutput_ = nullptr; ///< Active in-memory serialization sink.
};

/// @brief Detect the build host's native object format at compile time.
/// @return Mach-O on Apple, COFF on Windows, and ELF otherwise.
constexpr ObjFormat detectHostFormat() {
#if defined(__APPLE__)
    return ObjFormat::MachO;
#elif defined(_WIN32)
    return ObjFormat::COFF;
#else
    return ObjFormat::ELF;
#endif
}

/// @brief Detect the build host's native object architecture at compile time.
/// @return AArch64 for recognized ARM64 compiler targets, otherwise x86-64.
constexpr ObjArch detectHostArch() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return ObjArch::AArch64;
#else
    return ObjArch::X86_64;
#endif
}

/// @brief Create a concrete writer for a requested format and architecture.
/// @param format Target object container.
/// @param arch Target instruction-set architecture.
/// @return Owning writer pointer, or null for an unhandled future format.
std::unique_ptr<ObjectFileWriter> createObjectFileWriter(ObjFormat format, ObjArch arch);

/// @brief Create a writer for the compile-time host format and architecture.
/// @return Owning host-native writer pointer.
inline std::unique_ptr<ObjectFileWriter> createHostObjectFileWriter() {
    return createObjectFileWriter(detectHostFormat(), detectHostArch());
}

} // namespace zanna::codegen::objfile
