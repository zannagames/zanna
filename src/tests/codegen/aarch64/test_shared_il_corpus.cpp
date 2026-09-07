//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/aarch64/test_shared_il_corpus.cpp
// Purpose: Compile the shared IL corpus through the AArch64 command pipeline.
// Key invariants:
//   - Representative shared-corpus programs must lower to AArch64 assembly on
//     every host without executing generated ARM64 code.
//   - Repeated assembly emission for the same corpus file is deterministic.
// Ownership/Lifetime: Reads corpus files from the source tree and writes
//                     temporary assembly under build/test-out/aarch64.
// Links: docs/internals/testing.md, docs/specs/aarch64.md
//
//===----------------------------------------------------------------------===//

#include "tools/zanna/cmd_codegen_arm64.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct CorpusCase {
    const char *file;
    std::vector<std::string_view> requiredMnemonics;
};

[[nodiscard]] fs::path corpusRoot() {
    return fs::path(ZANNA_SHARED_IL_CORPUS_DIR) / "success";
}

[[nodiscard]] fs::path outputRoot() {
    fs::path dir{"build/test-out/aarch64/shared-corpus"};
    fs::create_directories(dir);
    return dir;
}

[[nodiscard]] std::string readFile(const fs::path &path) {
    std::ifstream input(path);
    if (!input) {
        std::cerr << "cannot read file: " << path << '\n';
        assert(false && "failed to read generated assembly");
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void compileAssembly(const fs::path &input, const fs::path &output) {
    const std::string in = input.string();
    const std::string out = output.string();
    const char *argv[] = {in.c_str(), "-S", out.c_str(), "-O0", "--target-darwin", "--verify-mir"};
    const int rc = zanna::tools::ilc::cmd_codegen_arm64(6, const_cast<char **>(argv));
    if (rc != 0) {
        std::cerr << "AArch64 corpus compile failed for " << input << " rc=" << rc << '\n';
        assert(false && "AArch64 shared corpus compile failed");
    }
}

void requireContains(std::string_view asmText, std::string_view needle, const fs::path &file) {
    if (asmText.find(needle) == std::string_view::npos) {
        std::cerr << "missing AArch64 assembly marker '" << needle << "' in " << file << "\n"
                  << asmText << '\n';
        assert(false && "AArch64 shared corpus assembly marker missing");
    }
}

void runCase(const CorpusCase &testCase) {
    const fs::path input = corpusRoot() / testCase.file;
    const fs::path firstOutput = outputRoot() / (fs::path(testCase.file).stem().string() + ".s");
    const fs::path secondOutput =
        outputRoot() / (fs::path(testCase.file).stem().string() + ".again.s");

    compileAssembly(input, firstOutput);
    compileAssembly(input, secondOutput);

    const std::string first = readFile(firstOutput);
    const std::string second = readFile(secondOutput);
    if (first != second) {
        std::cerr << "AArch64 assembly was not deterministic for " << testCase.file << '\n';
        assert(false && "AArch64 shared corpus assembly is nondeterministic");
    }

    requireContains(first, "main", firstOutput);
    requireContains(first, "ret", firstOutput);
    for (std::string_view mnemonic : testCase.requiredMnemonics)
        requireContains(first, mnemonic, firstOutput);
}

const std::vector<CorpusCase> &cases() {
    static const std::vector<CorpusCase> corpus = {
        {"ret_i64.il", {"mov"}},
        {"arith_chain.il", {"adds", "mul", "sdiv"}},
        {"block_params_sum.il", {"b."}},
        {"conditional_abs.il", {"subs", "b."}},
        {"recursive_factorial.il", {"bl"}},
        {"switch_i32.il", {"cmp"}},
        {"alloca_store_load.il", {"str", "ldr"}},
        {"global_scalar_load_store.il", {"counter"}},
        {"gep_stack.il", {"str"}},
        {"bitwise_shift.il", {"lsl", "eor", "orr", "and"}},
        {"unsigned_div_rem.il", {"udiv"}},
        {"signed_div_rem.il", {"sdiv"}},
        {"fp_cast_round.il", {"fadd"}},
        {"idxchk_normalize.il", {"cmp", "subs"}},
        {"call_two_args.il", {"bl"}},
        {"branch_join_value.il", {"b."}},
        {"i16_narrow_store.il", {"sturh", "ldurh"}},
        {"unsigned_compare.il", {"cmp"}},
    };
    return corpus;
}

} // namespace

int main() {
    std::cout << "Running AArch64 shared IL corpus codegen tests...\n";
    for (const CorpusCase &testCase : cases())
        runCase(testCase);
    std::cout << "AArch64 shared IL corpus codegen tests PASSED!\n";
    return 0;
}
