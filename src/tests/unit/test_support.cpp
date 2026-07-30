//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_support.cpp
// Purpose: Validate support-layer allocators, containers, diagnostics, source
//          tracking, string interning, and filesystem helpers.
// Key invariants:
//   - Arena allocation arithmetic never wraps and preserves requested alignment.
//   - GrowingArena destroys tracked objects exactly once in LIFO-safe batches.
//   - Stable symbols and source identifiers keep their documented value semantics.
// Ownership/Lifetime:
//   - Test-local support objects own all temporary allocations and streams.
//   - Temporary filesystem paths are removed by their existing scoped fixtures.
// Links: src/support/arena.hpp, src/support/string_interner.hpp,
//        src/support/source_manager.hpp
//
//===----------------------------------------------------------------------===//

#include "common/Filesystem.hpp"
#include "support/alignment.hpp"
#include "support/arena.hpp"
#include "support/diag_capture.hpp"
#include "support/diag_expected.hpp"
#include "support/diagnostics.hpp"
#include "support/small_vector.hpp"
#include "support/source_location.hpp"
#include "support/source_manager.hpp"
#include "support/string_interner.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace il::support {
struct SourceManagerTestAccess {
    static void setNextFileId(SourceManager &sm, uint64_t next) {
        sm.next_file_id_ = next;
    }

    static size_t storedPathCount(const SourceManager &sm) {
        return sm.files_.size();
    }
};
} // namespace il::support

int main() {
    // String interner uniqueness and lookup
    il::support::StringInterner interner;
    auto a = interner.intern("hello");
    auto b = interner.intern("hello");
    assert(a == b);
    assert(interner.lookup(a) == "hello");
    assert(interner.contains(a));
    auto emptySym = interner.intern("");
    assert(emptySym);
    assert(interner.lookup(emptySym).empty());
    auto emptyLookup = interner.lookupOptional(emptySym);
    assert(emptyLookup.has_value());
    assert(emptyLookup->empty());
    assert(!interner.lookupOptional(il::support::Symbol{}).has_value());

    il::support::StringInterner moveSource;
    auto movedSym = moveSource.intern("moved");
    il::support::StringInterner moveConstructed(std::move(moveSource));
    assert(moveConstructed.lookup(movedSym) == "moved");
    assert(moveSource.size() == 0);
    auto reusedSourceSym = moveSource.intern("source-reused");
    assert(reusedSourceSym.id == 1);
    assert(moveSource.lookup(reusedSourceSym) == "source-reused");
    il::support::StringInterner moveAssigned;
    moveAssigned = std::move(moveConstructed);
    assert(moveAssigned.lookup(movedSym) == "moved");
    assert(moveConstructed.size() == 0);
    auto reusedConstructedSym = moveConstructed.intern("move-assigned-source-reused");
    assert(reusedConstructedSym.id == 1);
    assert(moveConstructed.lookup(reusedConstructedSym) == "move-assigned-source-reused");
    il::support::StringInterner copyAssigned;
    copyAssigned.intern("old");
    copyAssigned = moveAssigned;
    assert(copyAssigned.lookup(movedSym) == "moved");
    auto copiedFresh = copyAssigned.intern("fresh");
    assert(copyAssigned.lookup(copiedFresh) == "fresh");
    il::support::StringInterner movedAgain;
    movedAgain = std::move(copyAssigned);
    assert(movedAgain.lookup(movedSym) == "moved");
    assert(movedAgain.lookup(copiedFresh) == "fresh");

    // Cached string_view from lookup must survive further growth.
    il::support::StringInterner stableInterner;
    auto stableSym = stableInterner.intern("stable");
    std::string_view cachedView = stableInterner.lookup(stableSym);
    const char *cachedData = cachedView.data();
    const size_t cachedSize = cachedView.size();
    for (int i = 0; i < 1024; ++i) {
        stableInterner.intern("padding_" + std::to_string(i));
        std::string_view probe = stableInterner.lookup(stableSym);
        assert(probe.data() == cachedData);
        assert(probe.size() == cachedSize);
    }
    assert(cachedView.data() == cachedData);
    assert(cachedView.size() == cachedSize);
    assert(cachedView == "stable");

    // Diagnostic formatting
    il::support::SourceManager sm;
    il::support::SourceLoc loc{sm.addFile("test"), 1, 1};
    il::support::DiagnosticEngine de;
    de.report({il::support::Severity::Error, "oops", loc, {}});
    std::ostringstream oss;
    de.printAll(oss, &sm);
    assert(oss.str().find("error: oops") != std::string::npos);
    assert(oss.str().find("test:1:1") != std::string::npos);

    il::support::SourceLoc partial{loc.file_id, 2, 0};
    assert(partial.isValid());
    assert(partial.hasLine());
    assert(!partial.hasColumn());
    il::support::SourceRange mixed{loc, partial};
    assert(!mixed.isValid());
    assert(mixed.isTracked());
    assert(!mixed.end.hasColumn());

    il::support::SourceLoc otherFile{sm.addFile("other"), 3, 5};
    il::support::SourceRange mismatched{loc, otherFile};
    assert(!mismatched.isValid());

    il::support::SourceRange reversed{partial, loc};
    assert(!reversed.isValid());

    il::support::SourceLoc sameLineBegin{loc.file_id, 4, 7};
    il::support::SourceLoc missingColumnEnd{loc.file_id, 4, 0};
    il::support::SourceRange missingColumnRange{sameLineBegin, missingColumnEnd};
    assert(!missingColumnRange.isValid());
    assert(missingColumnRange.isTracked());
    assert(!missingColumnRange.isConcrete());

    il::support::SourceLoc missingLineEnd{loc.file_id, 0, 0};
    il::support::SourceRange missingLineRange{sameLineBegin, missingLineEnd};
    assert(!missingLineRange.isValid());
    assert(missingLineRange.isTracked());
    assert(!missingLineRange.isConcrete());

    il::support::SourceRange insertionRange{sameLineBegin, sameLineBegin};
    assert(insertionRange.isValid());
    assert(insertionRange.isInsertion());
    assert(!insertionRange.isConcrete());
    il::support::Diag partialDiag{il::support::Severity::Error, "partial coordinates", partial, {}};
    std::ostringstream partialStream;
    il::support::printDiag(partialDiag, partialStream, &sm);
    const std::string partialText = partialStream.str();
    assert(partialText.find("test:2: error: partial coordinates") != std::string::npos);
    assert(partialText.find("test:2:0") == std::string::npos);

    const uint32_t memoryFile = sm.addFile("<memory>.zia");
    sm.setSource(memoryFile, "module Test;\nfunc start() { badCall(); }\n");
    il::support::Diag rangedDiag{
        il::support::Severity::Error,
        "bad call",
        il::support::SourceLoc{memoryFile, 2, 16},
        "T001",
    };
    rangedDiag.range = il::support::SourceRange{
        il::support::SourceLoc{memoryFile, 2, 16},
        il::support::SourceLoc{memoryFile, 2, 23},
    };
    rangedDiag.notes.push_back({il::support::SourceLoc{memoryFile, 1, 8}, "module declared here"});
    rangedDiag.stage = "sema";
    rangedDiag.help = "Check the callee name.";
    rangedDiag.fixits.push_back({rangedDiag.range, "goodCall", "replace bad call"});
    std::ostringstream rangedStream;
    il::support::printDiag(rangedDiag, rangedStream, &sm);
    const std::string rangedText = rangedStream.str();
    assert(rangedText.find("<memory>.zia:2:16: error[T001]: bad call") != std::string::npos);
    assert(rangedText.find("2 | func start() { badCall(); }") != std::string::npos);
    assert(rangedText.find("^~~~~~~") != std::string::npos);
    assert(rangedText.find("<memory>.zia:1:8: note: module declared here") != std::string::npos);
    assert(rangedText.find("  stage: sema") != std::string::npos);
    assert(rangedText.find("  help: Check the callee name.") != std::string::npos);
    assert(rangedText.find("  fix-it: replace bad call at <memory>.zia:2:16-2:23 -> goodCall") !=
           std::string::npos);

    il::support::Diag sanitizedTextDiag{
        il::support::Severity::Error,
        "first line\nsecond\tline",
        il::support::SourceLoc{memoryFile, 2, 1},
        {},
    };
    std::ostringstream sanitizedTextStream;
    il::support::printDiag(sanitizedTextDiag, sanitizedTextStream, &sm);
    assert(sanitizedTextStream.str().find("first line\\nsecond\\tline") != std::string::npos);

    const uint32_t controlFile = sm.addFile("<control>.zia");
    sm.setSource(controlFile, std::string("a\t") + static_cast<char>(0x1b) + "[31m\n");
    il::support::Diag controlDiag{
        il::support::Severity::Error,
        "control source",
        il::support::SourceLoc{controlFile, 1, 2},
        {},
    };
    std::ostringstream controlTextStream;
    il::support::printDiag(controlDiag, controlTextStream, &sm);
    const std::string controlText = controlTextStream.str();
    assert(controlText.find("a\\t\\x1b[31m") != std::string::npos);
    assert(controlText.find(static_cast<char>(0x1b)) == std::string::npos);

    std::ostringstream jsonStream;
    il::support::printDiagJson(rangedDiag, jsonStream, &sm);
    const std::string jsonText = jsonStream.str();
    assert(jsonText.find("\"stage\":\"sema\"") != std::string::npos);
    assert(jsonText.find("\"source\":\"func start() { badCall(); }\"") != std::string::npos);
    assert(jsonText.find("\"help\":\"Check the callee name.\"") != std::string::npos);
    assert(jsonText.find("\"replacement\":\"goodCall\"") != std::string::npos);

    il::support::Diag insertFixitDiag{
        il::support::Severity::Error,
        "missing token",
        il::support::SourceLoc{memoryFile, 2, 16},
        {},
    };
    insertFixitDiag.fixits.push_back({{}, "inserted", "insert token"});
    std::ostringstream insertFixitJson;
    il::support::printDiagJson(insertFixitDiag, insertFixitJson, &sm);
    const std::string insertFixitText = insertFixitJson.str();
    assert(insertFixitText.find("\"message\":\"insert token\"") != std::string::npos);
    assert(insertFixitText.find("\"replacement\":\"inserted\"") != std::string::npos);
    assert(insertFixitText.find("\"end\":{\"file_id\":" + std::to_string(memoryFile) +
                                ",\"line\":2,\"column\":16") != std::string::npos);

    const uint32_t blankFile = sm.addFile("blank.zia");
    sm.setSource(blankFile, "first\n\nthird\n");
    assert(sm.hasLine(blankFile, 2));
    assert(sm.getLine(blankFile, 2).empty());
    il::support::Diag blankDiag{
        il::support::Severity::Error,
        "blank line",
        il::support::SourceLoc{blankFile, 2, 1},
        {},
    };
    std::ostringstream blankTextStream;
    il::support::printDiag(blankDiag, blankTextStream, &sm);
    const std::string blankText = blankTextStream.str();
    assert(blankText.find("2 | \n") != std::string::npos);
    assert(blankText.find(" | ^") != std::string::npos);

    std::ostringstream blankJsonStream;
    il::support::printDiagJson(blankDiag, blankJsonStream, &sm);
    assert(blankJsonStream.str().find("\"source\":\"\"") != std::string::npos);

    il::support::Diag unknownPathJson{
        il::support::Severity::Error,
        "unknown path json",
        il::support::SourceLoc{9999, 1, 1},
        {},
    };
    std::ostringstream unknownPathJsonStream;
    il::support::printDiagJson(unknownPathJson, unknownPathJsonStream, &sm);
    assert(unknownPathJsonStream.str().find("\"file\":null") != std::string::npos);

    il::support::Diag invalidUtf8Diag{
        il::support::Severity::Error,
        std::string("bad byte ") + static_cast<char>(0xff),
        {},
        {},
    };
    std::ostringstream invalidUtf8Json;
    il::support::printDiagJson(invalidUtf8Diag, invalidUtf8Json, &sm);
    assert(invalidUtf8Json.str().find("\\ufffd") != std::string::npos);

    il::support::Diag unknownSeverityDiag{
        static_cast<il::support::Severity>(99),
        "odd severity",
        {},
        {},
    };
    std::ostringstream unknownSeverityText;
    il::support::printDiag(unknownSeverityDiag, unknownSeverityText, &sm);
    assert(unknownSeverityText.str().find("unknown: odd severity") != std::string::npos);

    // Captured string views must remain valid after subsequent insertions.
    il::support::SourceManager viewSm;
    const uint32_t first_id = viewSm.addFile("first");
    std::string_view first_view = viewSm.getPath(first_id);
    const char *first_data = first_view.data();
    assert(first_view == "first");
    (void)viewSm.addFile("second");
    (void)viewSm.addFile("third");
    std::string_view refreshed_view = viewSm.getPath(first_id);
    assert(first_view == refreshed_view);
    assert(first_data == refreshed_view.data());
    const uint32_t emptyPathId = viewSm.addFile("");
    assert(emptyPathId != 0);
    assert(viewSm.getPath(emptyPathId) == "<unknown>");

    // Re-adding an existing path should reuse the identifier and avoid growth.
    il::support::SourceManager dedupeSm;
    const uint32_t dedupeFirst = dedupeSm.addFile("./dupe/path/../file.txt");
    assert(dedupeFirst != 0);
    const size_t stored_before = il::support::SourceManagerTestAccess::storedPathCount(dedupeSm);
    const uint32_t dedupeSecond = dedupeSm.addFile("dupe/./file.txt");
    assert(dedupeSecond == dedupeFirst);
    [[maybe_unused]] const size_t stored_after =
        il::support::SourceManagerTestAccess::storedPathCount(dedupeSm);
    assert(stored_before == stored_after);
    assert(dedupeSm.getPath(dedupeFirst) == "dupe/file.txt");

    {
        const auto tempPath =
            std::filesystem::temp_directory_path() / "zanna_support_source_manager_lines.tmp";
        std::filesystem::remove(tempPath);
        il::support::SourceManager fileSm;
        const uint32_t tempId = fileSm.addFile(tempPath.string());
        assert(!fileSm.hasLine(tempId, 1));
        {
            std::ofstream out(tempPath, std::ios::binary);
            out << "alpha\r\n\r\nomega\r\n";
        }
        assert(!fileSm.hasLine(tempId, 1));
        fileSm.invalidateSource(tempId);
        assert(fileSm.hasLine(tempId, 1));
        assert(fileSm.hasLine(tempId, 2));
        assert(fileSm.getLine(tempId, 1) == "alpha");
        assert(fileSm.getLine(tempId, 2).empty());
        assert(fileSm.getLine(tempId, 3) == "omega");
        std::string_view oldLineView = fileSm.getLine(tempId, 1);
        {
            std::ofstream out(tempPath, std::ios::binary);
            out << "updated\n";
        }
        fileSm.invalidateSource(tempId);
        assert(oldLineView == "alpha");
        assert(fileSm.getLine(tempId, 1) == "updated");
        std::filesystem::remove(tempPath);
    }

    {
        const auto emptyPath =
            std::filesystem::temp_directory_path() / "zanna_support_source_manager_empty.tmp";
        {
            std::ofstream out(emptyPath, std::ios::binary);
        }
        il::support::SourceManager emptyFileSm;
        const uint32_t emptyFileId = emptyFileSm.addFile(emptyPath.string());
        assert(emptyFileSm.hasLine(emptyFileId, 1));
        assert(emptyFileSm.getLine(emptyFileId, 1).empty());
        std::filesystem::remove(emptyPath);
    }

    {
        const auto tempDir =
            std::filesystem::temp_directory_path() / "zanna_support_source_manager_cwd";
        std::filesystem::create_directories(tempDir);
        const auto oldCwd = std::filesystem::current_path();
        const auto relFile = tempDir / "relative.bas";
        {
            std::ofstream out(relFile);
            out << "from original cwd\n";
        }
        il::support::SourceManager relSm;
        std::filesystem::current_path(tempDir);
        const uint32_t relId = relSm.addFile("relative.bas");
        std::filesystem::current_path(oldCwd);
        assert(relSm.getLine(relId, 1) == "from original cwd");
        std::filesystem::remove(relFile);
        std::filesystem::remove(tempDir);
    }

    {
        const auto tempRoot =
            std::filesystem::temp_directory_path() / "zanna_support_source_manager_cwd_dedupe";
        const auto dirA = tempRoot / "a";
        const auto dirB = tempRoot / "b";
        std::filesystem::remove_all(tempRoot);
        std::filesystem::create_directories(dirA);
        std::filesystem::create_directories(dirB);
        {
            std::ofstream out(dirA / "shared.bas");
            out << "from a\n";
        }
        {
            std::ofstream out(dirB / "shared.bas");
            out << "from b\n";
        }
        const auto oldCwd = std::filesystem::current_path();
        il::support::SourceManager cwdSm;
        std::filesystem::current_path(dirA);
        const uint32_t idA = cwdSm.addFile("shared.bas");
        std::filesystem::current_path(dirB);
        const uint32_t idB = cwdSm.addFile("shared.bas");
        std::filesystem::current_path(oldCwd);
        assert(idA != 0);
        assert(idB != 0);
        assert(idA != idB);
        assert(cwdSm.getLine(idA, 1) == "from a");
        assert(cwdSm.getLine(idB, 1) == "from b");
        std::filesystem::remove_all(tempRoot);
    }

    {
        il::support::SourceManager stableLineSm;
        const uint32_t stableLineId = stableLineSm.addFile("stable-lines");
        stableLineSm.setSource(stableLineId, "old\n");
        std::string_view oldView = stableLineSm.getLine(stableLineId, 1);
        stableLineSm.setSource(stableLineId, "new\n");
        assert(oldView == "old");
        assert(stableLineSm.getLine(stableLineId, 1) == "new");
    }

    {
        il::support::SourceManager invalidSetSm;
        invalidSetSm.setSource(42, "ghost\n");
        assert(!invalidSetSm.hasLine(42, 1));
    }

#ifdef _WIN32
    {
        const std::filesystem::path unicodeRoot =
            std::filesystem::temp_directory_path() /
            std::filesystem::path(L"zanna-source-\u6771\u4eac-\u03b1");
        const std::filesystem::path unicodeFile =
            unicodeRoot / std::filesystem::path(L"\u5165\u529b-\u732b.zia");
        std::filesystem::remove_all(unicodeRoot);
        std::filesystem::create_directories(unicodeRoot);
        {
            std::ofstream out(unicodeFile, std::ios::binary);
            out << "unicode source\n";
        }
        il::support::SourceManager unicodeSm;
        const std::string unicodeUtf8 = zanna::filesystem::pathToUtf8(unicodeFile);
        const uint32_t unicodeId = unicodeSm.addFile(unicodeUtf8);
        assert(unicodeId != 0);
        assert(unicodeSm.getLine(unicodeId, 1) == "unicode source");
        assert(unicodeSm.getPath(unicodeId).find("\xE6\x9D\xB1\xE4\xBA\xAC") !=
               std::string_view::npos);
        std::filesystem::remove_all(unicodeRoot);
    }

    // Windows path normalization should ignore ASCII casing to align with
    // case-insensitive filesystem semantics.
    il::support::SourceManager caseInsensitiveSm;
    const uint32_t winFirst = caseInsensitiveSm.addFile("Case/FILE.TXT");
    assert(winFirst != 0);
    const size_t winStoredBefore =
        il::support::SourceManagerTestAccess::storedPathCount(caseInsensitiveSm);
    const uint32_t winSecond = caseInsensitiveSm.addFile("case/file.txt");
    assert(winSecond == winFirst);
    const size_t winStoredAfter =
        il::support::SourceManagerTestAccess::storedPathCount(caseInsensitiveSm);
    assert(winStoredBefore == winStoredAfter);
    assert(caseInsensitiveSm.getPath(winFirst) == "Case/FILE.TXT");
#endif

    // Diagnostics missing a registered path should not emit a leading colon.
    il::support::Diag missingPath{
        il::support::Severity::Error,
        "missing path context",
        il::support::SourceLoc{42, 2, 7},
        {},
    };
    std::ostringstream missingDiag;
    il::support::printDiag(missingPath, missingDiag, &sm);
    const std::string missingMessage = missingDiag.str();
    assert(missingMessage.rfind("error: missing path context", 0) == 0);
    assert(!missingMessage.empty() && missingMessage.front() != ':');

    // Expected<Diag> success versus error disambiguation
    std::string diagValueMessage = "value diag";
    il::support::Diag diagValue = il::support::makeError({}, diagValueMessage);
    il::support::Expected<il::support::Diag> ok(il::support::kSuccessDiag, std::move(diagValue));
    assert(ok.hasValue());
    assert(ok.value().message == diagValueMessage);

    std::string diagErrorMessage = "error diag";
    il::support::Diag diagError = il::support::makeError({}, diagErrorMessage);
    il::support::Expected<il::support::Diag> err(diagError);
    assert(!err.hasValue());
    assert(err.error().message == diagErrorMessage);

    il::support::DiagCapture formattedCapture;
    formattedCapture.ss << "error: already formatted\n";
    assert(formattedCapture.toDiag().message == "already formatted");
    il::support::DiagCapture warningCapture;
    warningCapture.ss << "warning: be careful\n";
    auto warningDiag = warningCapture.toDiag();
    assert(warningDiag.severity == il::support::Severity::Warning);
    assert(warningDiag.message == "be careful");
    il::support::DiagCapture locatedCapture;
    locatedCapture.ss << "file.bas:2:4: note: details here\n";
    auto locatedDiag = locatedCapture.toDiag();
    assert(locatedDiag.severity == il::support::Severity::Note);
    assert(locatedDiag.message == "details here");
    il::support::DiagCapture literalMarkerCapture;
    literalMarkerCapture.ss << "error: message mentions : warning: literally\n";
    assert(literalMarkerCapture.toDiag().message == "message mentions : warning: literally");
    il::support::DiagCapture multiCapture;
    multiCapture.ss << "error: first\nwarning: second\n";
    auto capturedDiagnostics = multiCapture.toDiagnostics();
    assert(capturedDiagnostics.size() == 2);
    assert(capturedDiagnostics[0].severity == il::support::Severity::Error);
    assert(capturedDiagnostics[1].severity == il::support::Severity::Warning);
    auto aggregateDiag = multiCapture.toDiag();
    assert(aggregateDiag.message == "first");
    assert(aggregateDiag.notes.size() == 1);
    assert(aggregateDiag.notes[0].message == "second");
    il::support::DiagCapture emptyCapture;
    auto capturedFailure = il::support::capture_to_expected_impl(false, emptyCapture);
    assert(!capturedFailure);
    assert(capturedFailure.error().message == "legacy operation failed without diagnostic output");

    // SmallVector should manage non-trivial element lifetimes exactly.
    {
        int liveCount = 0;
        int destroyedCount = 0;

        struct Counted {
            int *live;
            int *destroyed;
            int value;

            Counted(int *liveCounter, int *destroyedCounter, int v)
                : live(liveCounter), destroyed(destroyedCounter), value(v) {
                ++(*live);
            }

            Counted(const Counted &other)
                : live(other.live), destroyed(other.destroyed), value(other.value) {
                ++(*live);
            }

            Counted(Counted &&other) noexcept
                : live(other.live), destroyed(other.destroyed), value(other.value) {
                ++(*live);
            }

            ~Counted() {
                --(*live);
                ++(*destroyed);
            }
        };

        {
            il::support::SmallVector<Counted, 2> values;
            values.emplace_back(&liveCount, &destroyedCount, 1);
            values.emplace_back(&liveCount, &destroyedCount, 2);
            assert(values.size() == 2);
            assert(liveCount == 2);
            values.emplace_back(&liveCount, &destroyedCount, 3);
            assert(values.size() == 3);
            assert(values[2].value == 3);
            assert(liveCount == 3);
            values.pop_back();
            assert(values.size() == 2);
            assert(liveCount == 2);
            values.clear();
            assert(values.empty());
            assert(liveCount == 0);
            values.emplace_back(&liveCount, &destroyedCount, 4);
            il::support::SmallVector<Counted, 2> moved(std::move(values));
            assert(values.empty());
            assert(moved.size() == 1);
            assert(moved.front().value == 4);
            assert(liveCount == 1);
        }
        assert(liveCount == 0);
        assert(destroyedCount > 0);
    }

    {
        struct NoDefault {
            int value;

            explicit NoDefault(int v) : value(v) {}

            NoDefault(const NoDefault &) = default;
            NoDefault(NoDefault &&) noexcept = default;
        };

        il::support::SmallVector<NoDefault, 1> values;
        values.emplace_back(7);
        values.emplace_back(9);
        assert(values.size() == 2);
        assert(values[0].value == 7);
        assert(values[1].value == 9);
    }

    {
        il::support::SmallVector<std::string, 1> values;
        values.emplace_back("alpha");
        values.emplace_back(values[0]);
        assert(values.size() == 2);
        assert(values[0] == "alpha");
        assert(values[1] == "alpha");

        il::support::SmallVector<std::string, 1> resized;
        resized.emplace_back("seed");
        resized.resize(3, resized[0]);
        assert(resized.size() == 3);
        assert(resized[0] == "seed");
        assert(resized[1] == "seed");
        assert(resized[2] == "seed");
    }

    // Arena alignment
    assert(il::support::isPowerOfTwo(8u));
    assert(!il::support::isPowerOfTwo(0u));
    auto checkedAligned = il::support::checkedAlignUp<uint32_t>(5u, 4u);
    assert(checkedAligned.has_value());
    assert(*checkedAligned == 8u);
    assert(!il::support::checkedAlignUp<uint32_t>(std::numeric_limits<uint32_t>::max() - 1u, 8u));
    bool invalidAlignThrew = false;
    try {
        (void)il::support::alignUp<uint32_t>(5u, 0u);
    } catch (const std::overflow_error &) {
        invalidAlignThrew = true;
    }
    assert(invalidAlignThrew);
    assert(!il::support::isAligned<uint32_t>(8u, 0u));
    il::support::Arena arena(64);
    void *p1 = arena.allocate(1, 1);
    (void)p1;
    [[maybe_unused]] void *p2 = arena.allocate(sizeof(double), alignof(double));
    assert(reinterpret_cast<uintptr_t>(p2) % alignof(double) == 0);
    const size_t large_align = alignof(std::max_align_t) << 1;
    il::support::Arena large_arena(256);
    [[maybe_unused]] void *p3 = large_arena.allocate(16, large_align);
    assert(p3 != nullptr);
    assert(reinterpret_cast<uintptr_t>(p3) % large_align == 0);
    assert(arena.allocate(1, 0) == nullptr);
    assert(arena.allocate(1, 3) == nullptr);
    arena.reset();
    arena.allocate(32, 1);
    assert(arena.allocate(std::numeric_limits<size_t>::max() - 15, 1) == nullptr);

    // GrowingArena basic allocation
    {
        il::support::GrowingArena growArena(64, 128);
        void *g1 = growArena.allocate(32, 8);
        assert(g1 != nullptr);
        assert(reinterpret_cast<uintptr_t>(g1) % 8 == 0);

        // Force growth by allocating more than initial chunk
        void *g2 = growArena.allocate(100, 8);
        assert(g2 != nullptr);
        assert(growArena.chunkCount() >= 2);

        // Test create<T>()
        struct TestStruct {
            int a;
            double b;

            TestStruct(int x, double y) : a(x), b(y) {}
        };

        auto *ts = growArena.create<TestStruct>(42, 3.14);
        assert(ts != nullptr);
        assert(ts->a == 42);
        assert(ts->b == 3.14);

        // Test totalAllocated
        assert(growArena.totalAllocated() > 0);
    }

    // GrowingArena destructor tracking
    {
        static int destructorCount = 0;

        struct TrackedObj {
            int *counter;

            TrackedObj(int *c) : counter(c) {}

            ~TrackedObj() {
                ++(*counter);
            }
        };

        {
            il::support::GrowingArena destructArena(256);
            destructArena.create<TrackedObj>(&destructorCount);
            destructArena.create<TrackedObj>(&destructorCount);
            destructArena.create<TrackedObj>(&destructorCount);
            assert(destructorCount == 0); // Not destroyed yet
        }
        // Arena destroyed - objects should be destroyed
        assert(destructorCount == 3);

        // Test reset
        destructorCount = 0;
        {
            il::support::GrowingArena resetArena(256);
            resetArena.create<TrackedObj>(&destructorCount);
            resetArena.create<TrackedObj>(&destructorCount);
            resetArena.reset();
            assert(destructorCount == 2);

            resetArena.create<TrackedObj>(&destructorCount);
            // Will be destroyed on scope exit
        }
        assert(destructorCount == 3);
    }

    // A tracked destructor may construct another tracked object. The new
    // destructor must join the same reset cycle without invalidating iteration.
    {
        int spawnerDestructions = 0;
        int spawnedDestructions = 0;

        struct SpawnedObj {
            int *counter;

            ~SpawnedObj() {
                ++(*counter);
            }
        };

        struct SpawnerObj {
            il::support::GrowingArena *arena;
            int *spawnerCounter;
            int *spawnedCounter;

            ~SpawnerObj() {
                ++(*spawnerCounter);
                arena->create<SpawnedObj>(spawnedCounter);
            }
        };

        il::support::GrowingArena reentrantArena(256);
        reentrantArena.create<SpawnerObj>(
            &reentrantArena, &spawnerDestructions, &spawnedDestructions);
        reentrantArena.reset();
        assert(spawnerDestructions == 1);
        assert(spawnedDestructions == 1);
    }

    // Reentrant reset must not invalidate objects still awaiting destruction.
    {
        int resetterDestructions = 0;
        int olderDestructions = 0;

        struct OlderObj {
            int *counter;

            ~OlderObj() {
                ++(*counter);
            }
        };

        struct ResettingObj {
            il::support::GrowingArena *arena;
            int *counter;

            ~ResettingObj() {
                ++(*counter);
                arena->reset();
            }
        };

        il::support::GrowingArena resetReentrantArena(256);
        resetReentrantArena.create<OlderObj>(&olderDestructions);
        resetReentrantArena.create<ResettingObj>(&resetReentrantArena, &resetterDestructions);
        resetReentrantArena.reset();
        assert(resetterDestructions == 1);
        assert(olderDestructions == 1);
        assert(resetReentrantArena.totalAllocated() == 0);
    }

    {
        struct ThrowingObj {
            ThrowingObj() {
                throw 7;
            }
        };

        il::support::GrowingArena throwArena(64, 64);
        const size_t before = throwArena.totalAllocated();
        bool threw = false;
        try {
            throwArena.create<ThrowingObj>();
        } catch (int) {
            threw = true;
        }
        assert(threw);
        assert(throwArena.totalAllocated() == before);
    }

    // String interner overflow handling
    il::support::StringInterner boundedInterner(2);
    auto s0 = boundedInterner.intern("s0");
    auto s1 = boundedInterner.intern("s1");
    assert(s0);
    assert(s1);
    auto overflow = boundedInterner.intern("s2");
    assert(!overflow);
    assert(boundedInterner.lookup(overflow).empty());
    assert(boundedInterner.intern("s0") == s0);

    // SourceManager overflow handling
    {
        il::support::SourceManager overflowSm;
        std::stringstream captured;
        auto *old = std::cerr.rdbuf(captured.rdbuf());
        il::support::SourceManagerTestAccess::setNextFileId(
            overflowSm, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1);
        uint32_t overflowId = overflowSm.addFile("overflow");
        std::cerr.rdbuf(old);
        assert(overflowId == 0);
        auto diagText = captured.str();
        assert(diagText.empty());
    }

    return 0;
}
