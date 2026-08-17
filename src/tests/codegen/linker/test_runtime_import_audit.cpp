//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/codegen/linker/test_runtime_import_audit.cpp
// Purpose: Audit the built runtime/support archives for unresolved imports that
//          are not covered by the host native-link dynamic import policy.
//
//===----------------------------------------------------------------------===//

#include "codegen/common/LinkerSupport.hpp"
#include "codegen/common/RuntimeComponents.hpp"
#include "codegen/common/linker/ArchiveReader.hpp"
#include "codegen/common/linker/DynamicSymbolPolicy.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/PlatformImportPlanner.hpp"
#include "tests/TestHarness.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace zanna::codegen;
using namespace zanna::codegen::common;
using namespace zanna::codegen::linker;

namespace {

using SymbolOriginMap = std::unordered_map<std::string, std::unordered_set<std::string>>;

void addObjectSymbols(const ObjFile &obj,
                      const std::string &origin,
                      std::unordered_set<std::string> &defined,
                      std::unordered_set<std::string> &undefined,
                      SymbolOriginMap &undefinedOrigins) {
    for (size_t i = 1; i < obj.symbols.size(); ++i) {
        const auto &sym = obj.symbols[i];
        if (sym.name.empty())
            continue;
        if (sym.binding == ObjSymbol::Undefined) {
            undefined.insert(sym.name);
            undefinedOrigins[sym.name].insert(origin);
            continue;
        }
        if (sym.binding == ObjSymbol::Local)
            continue;
        defined.insert(sym.name);
    }
}

std::vector<std::filesystem::path> collectAuditArchives(const std::filesystem::path &buildDir) {
    std::vector<std::filesystem::path> paths;
    std::unordered_set<std::string> seen;

    auto appendIfExists = [&](const std::filesystem::path &path) {
        if (!fileExists(path))
            return;
        const std::string normalized = path.lexically_normal().string();
        if (seen.insert(normalized).second)
            paths.push_back(path);
    };

    for (size_t i = 0; i < static_cast<size_t>(RtComponent::Count); ++i) {
        const auto comp = static_cast<RtComponent>(i);
        appendIfExists(runtimeArchivePath(buildDir, archiveNameForComponent(comp)));
    }

    appendIfExists(supportLibraryPath(buildDir, "zannagui"));
    appendIfExists(supportLibraryPath(buildDir, "zanna_text_core"));
    appendIfExists(supportLibraryPath(buildDir, "zanna_regex_engine"));
    appendIfExists(supportLibraryPath(buildDir, "zannagfx"));
    appendIfExists(supportLibraryPath(buildDir, "zannaaud"));
    return paths;
}

bool usesDebugWindowsRuntimeArchives(const std::vector<std::filesystem::path> &archives) {
    for (const auto &path : archives) {
        std::string lower = path.lexically_normal().string();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower.find("\\debug\\") != std::string::npos ||
            lower.find("/debug/") != std::string::npos ||
            lower.rfind("msvcrtd.lib") != std::string::npos ||
            lower.rfind("ucrtd.lib") != std::string::npos ||
            lower.rfind("vcruntimed.lib") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool addArchiveObjectDefinitionsOnly(const std::filesystem::path &archivePath,
                                     std::unordered_set<std::string> &defined,
                                     std::ostream &err) {
    Archive archive;
    if (!readArchive(archivePath.string(), archive, err))
        return false;

    for (const auto &member : archive.members) {
        const auto memberBytes = extractMember(archive, member);
        if (memberBytes.empty())
            continue;
        if (isCoffImportLibraryMember(memberBytes.data(), memberBytes.size()))
            continue;

        ObjFile obj;
        std::ostringstream objErr;
        if (!readObjFile(memberBytes.data(),
                         memberBytes.size(),
                         archive.path + "(" + member.name + ")",
                         obj,
                         objErr)) {
            continue;
        }

        std::unordered_set<std::string> memberUndefined;
        SymbolOriginMap memberUndefinedOrigins;
        addObjectSymbols(obj,
                         archive.path + "(" + member.name + ")",
                         defined,
                         memberUndefined,
                         memberUndefinedOrigins);
    }
    return true;
}

} // namespace

TEST(LinkerRuntimeImportAudit, HostRuntimeArchivesUseKnownDynamicImports) {
    const auto buildDir = findBuildDir();
    ASSERT_TRUE(buildDir.has_value());

    const std::vector<std::filesystem::path> archives = collectAuditArchives(*buildDir);
    ASSERT_FALSE(archives.empty());

    std::unordered_set<std::string> defined;
    std::unordered_set<std::string> undefined;
    SymbolOriginMap undefinedOrigins;

    for (const auto &archivePath : archives) {
        Archive archive;
        std::ostringstream err;
        ASSERT_TRUE(readArchive(archivePath.string(), archive, err));
        ASSERT_TRUE(err.str().empty());

        for (const auto &member : archive.members) {
            const auto memberBytes = extractMember(archive, member);
            ASSERT_FALSE(memberBytes.empty());

            ObjFile obj;
            std::ostringstream objErr;
            ASSERT_TRUE(readObjFile(memberBytes.data(),
                                    memberBytes.size(),
                                    archive.path + "(" + member.name + ")",
                                    obj,
                                    objErr));
            ASSERT_TRUE(objErr.str().empty());
            addObjectSymbols(
                obj, archive.path + "(" + member.name + ")", defined, undefined, undefinedOrigins);
        }
    }

    if (detectLinkPlatform() == LinkPlatform::Windows) {
        const std::string archName = detectLinkArch() == LinkArch::AArch64 ? "arm64" : "x64";
        const bool debugRuntime = usesDebugWindowsRuntimeArchives(archives);
        for (const auto &archivePath :
             windowsMsvcCxxRuntimeArchives(*buildDir, archName, debugRuntime)) {
            std::ostringstream err;
            if (!addArchiveObjectDefinitionsOnly(archivePath, defined, err))
                std::cerr << err.str();
            ASSERT_TRUE(err.str().empty());
        }
    }

    std::vector<std::string> unresolved;
    unresolved.reserve(undefined.size());
    for (const auto &sym : undefined) {
        if (defined.count(sym) == 0)
            unresolved.push_back(sym);
    }
    std::sort(unresolved.begin(), unresolved.end());
    unresolved.erase(std::unique(unresolved.begin(), unresolved.end()), unresolved.end());

    const LinkPlatform platform = detectLinkPlatform();
    std::unordered_set<std::string> dynamicSyms;
    std::vector<std::string> unknown;

    for (const auto &sym : unresolved) {
        // Linker-defined names (the ELF GOT base) are supplied by the link
        // itself, so they are neither loader imports nor unclassified.
        if (isLinkerDefinedSymbol(sym, platform))
            continue;
        const bool allowSynthetic =
            platform == LinkPlatform::Windows && isWindowsLinkerHelperSymbol(sym);
        const bool allowDynamic = allowSynthetic || isKnownDynamicSymbol(sym, platform);
        if (allowDynamic) {
            dynamicSyms.insert(sym);
            continue;
        }
        unknown.push_back(sym);
    }

    if (!unknown.empty()) {
        std::ostringstream msg;
        msg << "Unclassified runtime imports:\n";
        for (const auto &sym : unknown) {
            msg << "  " << sym;
            auto it = undefinedOrigins.find(sym);
            if (it != undefinedOrigins.end() && !it->second.empty()) {
                std::vector<std::string> origins(it->second.begin(), it->second.end());
                std::sort(origins.begin(), origins.end());
                msg << " <- ";
                for (size_t i = 0; i < origins.size(); ++i) {
                    if (i != 0)
                        msg << ", ";
                    msg << origins[i];
                }
            }
            msg << "\n";
        }
        std::cerr << msg.str() << "\n";
        ASSERT_TRUE(unknown.empty());
    }

    std::ostringstream planErr;
    switch (platform) {
        case LinkPlatform::Linux: {
            LinuxImportPlan plan;
            ASSERT_TRUE(planLinuxImports(dynamicSyms, plan, planErr));
            break;
        }
        case LinkPlatform::macOS: {
            MacImportPlan plan;
            const bool planned = planMacImports(dynamicSyms, plan, planErr);
            if (!planned)
                std::cerr << planErr.str();
            ASSERT_TRUE(planned);
            break;
        }
        case LinkPlatform::Windows: {
            WindowsImportPlan plan;
            const bool planned = generateWindowsImports(detectLinkArch(),
                                                        dynamicSyms,
                                                        usesDebugWindowsRuntimeArchives(archives),
                                                        plan,
                                                        planErr);
            if (!planned)
                std::cerr << planErr.str();
            ASSERT_TRUE(planned);
            break;
        }
    }

    EXPECT_TRUE(planErr.str().empty());
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
