//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTAssetDiscoveryTests.cpp
// Purpose: Verify that asset initialization discovers .zpak pack files beside
//          the executable, which is how packaged games and store depots ship
//          `pack` groups.
// Key invariants:
//   - The test owns process-wide asset initialization: no asset call may run
//     before the pack file exists, because initialization happens once.
//   - The pack and entry names embed the process id so concurrent test runs in
//     a shared build directory cannot see each other's packs by name.
// Ownership/Lifetime:
//   - The temporary pack file is removed before the process exits.
// Links: src/runtime/io/rt_asset.c, src/tools/common/asset/ZpakWriter.hpp,
//        docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

#include "ZpakWriter.hpp"
#include "rt.hpp"
#include "rt_asset.h"
#include "rt_path.h"
#include "rt_string.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

int main() {
    char *exeDir = rt_path_exe_dir_cstr();
    if (!exeDir) {
        std::fprintf(stderr, "FAIL: executable directory is unavailable\n");
        return 1;
    }
    const std::string pid = std::to_string(static_cast<long long>(GETPID()));
    const std::string packPath = std::string(exeDir) + "/zanna-asset-discovery-" + pid + ".zpak";
    std::free(exeDir);
    const std::string entryName = "discovery/" + pid + ".txt";
    const std::string payload = "discovered beside the executable\n";

    zanna::asset::ZpakWriter writer;
    writer.addEntry(
        entryName, reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), false);
    std::string err;
    if (!writer.writeToFile(packPath, err)) {
        std::fprintf(stderr, "FAIL: cannot write %s: %s\n", packPath.c_str(), err.c_str());
        return 1;
    }

    // First asset call: initialization scans the executable directory now.
    const int64_t exists = rt_asset_exists(rt_const_cstr(entryName.c_str()));
    const int64_t size = rt_asset_size(rt_const_cstr(entryName.c_str()));
    std::remove(packPath.c_str());

    if (exists != 1) {
        std::fprintf(
            stderr, "FAIL: %s was not discovered beside the executable\n", packPath.c_str());
        return 1;
    }
    if (size != static_cast<int64_t>(payload.size())) {
        std::fprintf(stderr,
                     "FAIL: discovered entry size %lld, expected %zu\n",
                     static_cast<long long>(size),
                     payload.size());
        return 1;
    }
    std::printf("RESULT: ok\n");
    return 0;
}
