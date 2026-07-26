//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the top-level `zanna` driver. The executable dispatches to
// subcommands that run IL programs, compile BASIC, or apply optimizer passes.
// Shared CLI plumbing lives in cli.cpp; this file wires those helpers into the
// `main` entry point and prints user-facing usage information.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Entry point and usage utilities for the `zanna` driver.
/// @details The translation unit owns only user-interface glue; heavy lifting
///          such as pass management or VM execution is delegated to subcommands.

#include "cli.hpp"
#include "cmd_asset.hpp"
#include "cmd_codegen_arm64.hpp"
#include "cmd_codegen_x64.hpp"
#include "common/PlatformCapabilities.hpp"
#include "common/Utf8CommandLine.hpp"
#include "il/core/Module.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "support/diag_expected.hpp"
#include "zanna/version.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if ZANNA_HOST_WINDOWS
#include <cstdlib>
#endif

namespace {

/// @brief Print the zanna version banner and runtime configuration summary.
///
/// @details The banner includes the zanna version, current IL version, and whether
///          deterministic numerics are enabled. The routine is factored out so
///          both `main` and future subcommands can reuse it when handling
///          `--version` flags.
void printVersion() {
    // Version banner
    std::cout << "zanna v" << ZANNA_VERSION_STR << "\n";
    if (std::string(ZANNA_SNAPSHOT_STR).size())
        std::cout << "snap: " << ZANNA_SNAPSHOT_STR << "\n";
    if (std::string(ZANNA_SOURCE_COMMIT_STR).size()) {
        std::cout << "source: " << ZANNA_SOURCE_COMMIT_STR << " (" << ZANNA_SOURCE_STATE_STR
                  << ")\n";
    } else {
        std::cout << "source: unknown (" << ZANNA_SOURCE_STATE_STR << ")\n";
    }
    std::cout << "IL current: " << ZANNA_IL_VERSION_STR << "\n";
    std::cout << "IL supported: " << ZANNA_IL_VERSION_STR << "\n";
    std::cout << "Precise Numerics: enabled\n";
}

} // namespace

namespace {
/// @brief Implement `--dump-runtime-descriptors`: print the runtime descriptor
///        table in a stable, sorted format to stdout.
/// @return 0 on success.
int dumpRuntimeDescriptors() {
    using il::runtime::findRuntimeSignatureId;
    using il::runtime::RuntimeDescriptor;
    using il::runtime::runtimeRegistry;

    const auto &reg = runtimeRegistry();

    /// @brief Runtime descriptors sharing one signature id and handler.
    struct DescriptorGroup {
        std::optional<il::runtime::RtSig> sig{};
        il::runtime::RuntimeHandler handler{nullptr};
        std::vector<const RuntimeDescriptor *> descriptors;
        size_t firstIndex{0};

        /// @brief Test whether a descriptor belongs to this grouping key.
        /// @param otherSig Candidate runtime signature identifier.
        /// @param otherHandler Candidate runtime implementation handler.
        /// @return true when both key components match this group.
        bool matches(std::optional<il::runtime::RtSig> otherSig,
                     il::runtime::RuntimeHandler otherHandler) const noexcept {
            return sig == otherSig && handler == otherHandler;
        }
    };

    std::vector<DescriptorGroup> groups;
    groups.reserve(reg.size());

    for (size_t i = 0; i < reg.size(); ++i) {
        const auto &d = reg[i];
        const auto sig = findRuntimeSignatureId(d.name);
        /// @brief Match an existing descriptor group by signature and implementation handler.
        /// @param group Candidate group.
        /// @return `true` when `group` accepts the current descriptor key.
        auto it = std::find_if(groups.begin(), groups.end(), [&](const DescriptorGroup &group) {
            return group.matches(sig, d.handler);
        });
        if (it == groups.end()) {
            DescriptorGroup group;
            group.sig = sig;
            group.handler = d.handler;
            group.firstIndex = i;
            group.descriptors.push_back(&d);
            groups.push_back(std::move(group));
        } else {
            it->descriptors.push_back(&d);
        }
    }

    /// @brief Render a comma-separated list of IL type spellings.
    /// @param ts Types to serialize.
    /// @return Comma-delimited type text.
    auto typeListToString = [](const std::vector<il::core::Type> &ts) -> std::string {
        std::ostringstream os;
        for (size_t i = 0; i < ts.size(); ++i) {
            if (i)
                os << ',';
            os << ts[i].toString();
        }
        return os.str();
    };

    /// @brief Render runtime signature effects and trap classification.
    /// @param s Signature whose throw, read, and purity flags are serialized.
    /// @param trap Runtime trap classification to append.
    /// @return Human-readable comma-separated effect list.
    auto effectsToString = [](const il::runtime::RuntimeSignature &s,
                              il::runtime::RuntimeTrapClass trap) -> std::string {
        std::vector<std::string> items;
        items.push_back(s.nothrow ? "NoThrow" : "MayThrow");
        if (s.readonly)
            items.push_back("ReadOnly");
        if (s.pure)
            items.push_back("Pure");
        if (trap != il::runtime::RuntimeTrapClass::None) {
            switch (trap) {
                case il::runtime::RuntimeTrapClass::PowDomainOverflow:
                    items.emplace_back("Trap:PowDomainOverflow");
                    break;
                default:
                    items.emplace_back("Trap:Unknown");
                    break;
            }
        }
        if (items.empty())
            return std::string("None");
        std::ostringstream os;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i)
                os << ", ";
            os << items[i];
        }
        return os.str();
    };

    /// @brief Restore descriptor groups to their first registry occurrence.
    /// @param a Left-hand group.
    /// @param b Right-hand group.
    /// @return `true` when `a` first appeared before `b`.
    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
        return a.firstIndex < b.firstIndex;
    });

    for (const auto &entry : groups) {
        const auto &vec = entry.descriptors;
        // choose canonical: prefer Zanna.* else first
        const RuntimeDescriptor *canonical = vec.front();
        for (const auto *d : vec) {
            if (d->name.rfind("Zanna.", 0) == 0) {
                canonical = d;
                break;
            }
        }

        // collect aliases (rt_* only)
        std::vector<std::string_view> aliases;
        for (const auto *d : vec) {
            if (d == canonical)
                continue;
            if (d->name.rfind("rt_", 0) == 0)
                aliases.push_back(d->name);
        }

        // Print block
        std::cout << "NAME: " << canonical->name << "\n";

        std::cout << "  ALIASES: ";
        if (aliases.empty()) {
            std::cout << "(none)\n";
        } else {
            for (size_t i = 0; i < aliases.size(); ++i) {
                if (i)
                    std::cout << ", ";
                std::cout << aliases[i];
            }
            std::cout << "\n";
        }

        const auto &sig = canonical->signature;
        std::cout << "  SIGNATURE: " << sig.retType.toString() << '('
                  << typeListToString(sig.paramTypes) << ")\n";
        std::cout << "  EFFECTS: " << effectsToString(sig, canonical->trapClass) << "\n";
    }
    return 0;
}
} // namespace

namespace {
/// @brief Return true when @p text begins with @p prefix.
/// @details The runtime API dump uses this helper for portable namespace and
///          token classification without relying on platform-specific APIs.
/// @param text Candidate text to inspect.
/// @param prefix Prefix to test.
/// @return True if @p text starts with @p prefix.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// @brief Return true when @p text ends with @p suffix.
/// @details Used for compact signature and public-name classification.
/// @param text Candidate text to inspect.
/// @param suffix Suffix to test.
/// @return True if @p text ends with @p suffix.
bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// @brief Return whether a name belongs to the reviewed Graphics3D/Game3D boundary.
/// @param name Public runtime function, class, property-target, or method-target name.
/// @return true for names rooted at `Zanna.Graphics3D.` or `Zanna.Game3D.`.
bool isThreeDRuntimeName(std::string_view name) {
    return startsWith(name, "Zanna.Graphics3D.") || startsWith(name, "Zanna.Game3D.");
}

/// @brief Return whether a name belongs to the reviewed GUI runtime boundary.
/// @param name Public runtime function, class, property-target, or method-target name.
/// @return True for names rooted at `Zanna.GUI.`.
bool isGuiRuntimeName(std::string_view name) {
    return startsWith(name, "Zanna.GUI.");
}

/// @brief Return the authored contract-policy identifier for a runtime API row.
/// @details Reviewed GUI and 3D boundaries use explicit policies; all remaining rows retain the
///          general inference marker until their domain receives an equivalent contract review.
/// @param name Public runtime function or binding-target name.
/// @return Stable machine-readable contract source.
std::string_view runtimeContractSource(std::string_view name) {
    if (isGuiRuntimeName(name))
        return "gui-boundary-policy";
    if (isThreeDRuntimeName(name))
        return "three-d-boundary-policy";
    return "inferred";
}

/// @brief Return the last dot-separated segment of a runtime name.
/// @details `Zanna.Network.Http.Get` becomes `Get`; names without dots are
///          returned unchanged.
/// @param name Public runtime name or member name.
/// @return View covering the final segment.
std::string_view lastRuntimeNameSegment(std::string_view name) {
    const size_t pos = name.rfind('.');
    if (pos == std::string_view::npos)
        return name;
    return name.substr(pos + 1);
}

/// @brief Return the owner namespace/class path for a public runtime name.
/// @details `Zanna.Network.Http.Get` becomes `Zanna.Network.Http`.
/// @param name Public runtime name.
/// @return Owning namespace/class path, or an empty string for root names.
std::string runtimeApiOwner(std::string_view name) {
    const size_t pos = name.rfind('.');
    if (pos == std::string_view::npos)
        return {};
    return std::string(name.substr(0, pos));
}

/// @brief Build a lowercase ASCII slug from a public runtime name.
/// @details Non-alphanumeric runs collapse to one dash. The result is a
///          best-effort Markdown anchor hint for generated docs links.
/// @param text Runtime name, class name, or phrase.
/// @return Lowercase slug string.
std::string runtimeApiSlug(std::string_view text) {
    std::string slug;
    bool previousDash = false;
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            previousDash = false;
        } else if (!previousDash) {
            slug.push_back('-');
            previousDash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    while (!slug.empty() && slug.front() == '-')
        slug.erase(slug.begin());
    return slug;
}

/// @brief Split a comma-separated runtime type list at top-level commas.
/// @details Generic-looking tokens such as `seq<str>` and
///          `obj<Zanna.Collections.Seq>` may contain nested punctuation; this
///          helper preserves those tokens while separating parameters.
/// @param text Text inside a signature's parentheses.
/// @return Individual parameter type tokens.
std::vector<std::string> splitRuntimeTypeList(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    int angleDepth = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (text[i] == '<') {
                ++angleDepth;
            } else if (text[i] == '>' && angleDepth > 0) {
                --angleDepth;
            }
        }
        if (i == text.size() || (text[i] == ',' && angleDepth == 0)) {
            std::string item(text.substr(start, i - start));
            /// @brief Identify the first non-whitespace byte of a parameter token.
            /// @param c Byte to inspect.
            /// @return `true` when `c` is not whitespace.
            item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char c) {
                           return !std::isspace(c);
                       }));
            item.erase(std::find_if(item.rbegin(),
                                    item.rend(),
                                    /// @brief Identify the last non-whitespace byte of a token.
                                    /// @param c Byte to inspect.
                                    /// @return `true` when `c` is not whitespace.
                                    [](unsigned char c) { return !std::isspace(c); })
                           .base(),
                       item.end());
            if (!item.empty())
                out.push_back(std::move(item));
            start = i + 1;
        }
    }
    return out;
}

/// @brief Parsed view of the compact runtime.def signature dialect.
/// @details The public dump keeps exact signature text and adds this parsed
///          structure so tools do not need to implement their own ad hoc parser.
struct RuntimeApiSignatureParts {
    std::string returnType;              ///< Return type token before `(`.
    std::vector<std::string> paramTypes; ///< Parameter type tokens inside `(...)`.
    bool valid{false};                   ///< True when the signature had `ret(args)` shape.
};

/// @brief Parse a compact runtime.def signature.
/// @param signature Signature text such as `str(i64,str)`.
/// @return Parsed return and parameter tokens, or `valid=false` when malformed.
RuntimeApiSignatureParts parseRuntimeApiSignature(std::string_view signature) {
    RuntimeApiSignatureParts parts;
    const size_t open = signature.find('(');
    const size_t close = signature.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return parts;
    parts.returnType = std::string(signature.substr(0, open));
    parts.paramTypes = splitRuntimeTypeList(signature.substr(open + 1, close - open - 1));
    parts.valid = true;
    return parts;
}

/// @brief Emit a JSON string array.
/// @param os Stream receiving JSON.
/// @param values Values to emit as JSON strings.
void emitStringArrayJson(std::ostream &os, const std::vector<std::string> &values) {
    using il::support::printJsonStringEscaped;
    os << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            os << ',';
        printJsonStringEscaped(os, values[i]);
    }
    os << ']';
}

/// @brief Emit structured JSON metadata for one runtime type token.
/// @details The `raw` field always preserves the exact public spelling. The
///          `nullable` flag records current suffix-nullable spellings without
///          requiring tools to guess what punctuation means.
/// @param os Stream receiving JSON.
/// @param rawType Type token from a public signature.
/// @param nullableOverride Optional authoritative nullability replacing suffix inference.
void emitRuntimeApiTypeJson(std::ostream &os,
                            std::string_view rawType,
                            std::optional<bool> nullableOverride = std::nullopt) {
    using il::support::printJsonStringEscaped;

    std::string type(rawType);
    bool nullable = false;
    if (!type.empty() && type.back() == '?') {
        nullable = true;
        type.pop_back();
    }
    if (nullableOverride.has_value())
        nullable = *nullableOverride;

    os << "{\"raw\":";
    printJsonStringEscaped(os, rawType);
    os << ",\"kind\":";

    if (type == "void") {
        printJsonStringEscaped(os, "void");
    } else if (type == "i1") {
        printJsonStringEscaped(os, "boolean");
    } else if (type == "i8" || type == "i16" || type == "i32" || type == "i64") {
        printJsonStringEscaped(os, "integer");
    } else if (type == "f32" || type == "f64") {
        printJsonStringEscaped(os, "number");
    } else if (type == "str") {
        printJsonStringEscaped(os, "string");
    } else if (startsWith(type, "seq<") && endsWith(type, ">")) {
        printJsonStringEscaped(os, "sequence");
        os << ",\"element_type\":";
        emitRuntimeApiTypeJson(os, std::string_view(type).substr(4, type.size() - 5));
    } else if (startsWith(type, "obj<") && endsWith(type, ">")) {
        printJsonStringEscaped(os, "object");
        os << ",\"class\":";
        printJsonStringEscaped(os, std::string_view(type).substr(4, type.size() - 5));
    } else if (type == "obj") {
        printJsonStringEscaped(os, "object");
    } else if (type == "ptr") {
        printJsonStringEscaped(os, "pointer");
    } else {
        printJsonStringEscaped(os, "unknown");
    }

    os << ",\"nullable\":" << (nullable ? "true" : "false") << '}';
}

/// @brief Resolve row-level return nullability beyond the compact signature spelling.
/// @details The runtime-def-v1 dialect erases object nullability to `obj`. For the reviewed 3D
///          domains, the raw C boundary can return null for invalid handles, absence, failed
///          readback/import, or a trapped allocation, so object/pointer results are conservatively
///          and explicitly nullable. Primitive, string, and void results are non-nullable.
/// @param name Public runtime function or binding-target name.
/// @param signature Compact runtime signature used to classify the return type.
/// @return Explicit nullability when known, or nullopt when the row is not classified.
std::optional<bool> runtimeReturnNullable(std::string_view name, std::string_view signature) {
    const RuntimeApiSignatureParts sig = parseRuntimeApiSignature(signature);
    if (!sig.valid)
        return std::nullopt;
    if (endsWith(sig.returnType, "?"))
        return true;
    // System APIs that return NULL to signal startup/absence rather than a valid
    // handle, so tools do not mistake absence for a live object (VDOC-222).
    // Time object constructors/operations that return NULL on ordinary (non-trap)
    // failure — invalid civil input, an out-of-domain result, a failed parse, or
    // disjoint/gapped ranges — likewise need explicit nullability so tools emit the
    // null branch (VDOC-232).
    static constexpr std::string_view kNullableReturns[] = {
        "Zanna.System.Process.Start",
        "Zanna.System.Process.StartWithEnv",
        "Zanna.Time.DateOnly.FromParts",     // NULL for an invalid or out-of-domain date
        "Zanna.Time.DateOnly.Today",         // NULL when the wall clock is unavailable
        "Zanna.Time.DateOnly.Parse",         // NULL for a malformed date string
        "Zanna.Time.DateOnly.FromDays",      // NULL when the day count leaves the year domain
        "Zanna.Time.DateOnly.AddDays",       // NULL when the result leaves the year domain
        "Zanna.Time.DateOnly.AddMonths",     // NULL when the result leaves the year domain
        "Zanna.Time.DateOnly.AddYears",      // NULL when the result leaves the year domain
        "Zanna.Time.DateRange.Intersection", // NULL when the ranges are disjoint
        "Zanna.Time.DateRange.Union",        // NULL when the ranges have a gap
        // SpriteSheet constructors return NULL for a wrong-class/null atlas or an
        // atlas that does not divide evenly; GetRegion returns NULL for a name that
        // is not registered. Concrete typed-but-nullable results (VDOC-284).
        "Zanna.Graphics.SpriteSheet.New",
        "Zanna.Graphics.SpriteSheet.FromGrid",
        "Zanna.Graphics.SpriteSheet.GetRegion",
    };
    for (auto n : kNullableReturns)
        if (name == n)
            return true;
    if (isGuiRuntimeName(name)) {
        const bool isObject = sig.returnType == "obj" || sig.returnType == "ptr" ||
                              startsWith(sig.returnType, "obj<");
        if (!isObject)
            return false;
        if (startsWith(sig.returnType, "obj<Zanna.Result") ||
            startsWith(sig.returnType, "obj<Zanna.Option") ||
            startsWith(sig.returnType, "obj<Zanna.Collections.Map") ||
            startsWith(sig.returnType, "obj<Zanna.Collections.Seq"))
            return false;
        return true;
    }
    if (!isThreeDRuntimeName(name))
        return std::nullopt;
    return sig.returnType == "obj" || sig.returnType == "ptr" || startsWith(sig.returnType, "obj<");
}

/// @brief Return the generated domain reference that owns a runtime API row.
/// @details Domain pages are rendered from this same live registry, so the link
///          target and the machine-readable API surface cannot drift.
/// @param name Public runtime name or class name.
/// @return Repository-relative documentation path.
std::string docsFileForRuntimeName(std::string_view name) {
    constexpr std::string_view prefix = "Zanna.";
    std::string_view domain = name;
    if (startsWith(domain, prefix))
        domain.remove_prefix(prefix.size());
    if (const size_t dot = domain.find('.'); dot != std::string_view::npos)
        domain = domain.substr(0, dot);
    std::string slug = runtimeApiSlug(domain.empty() ? std::string_view("core") : domain);
    return "docs/generated/runtime/" + slug + ".md";
}

/// @brief Emit a best-effort docs anchor hint for a runtime row.
/// @param os Stream receiving JSON.
/// @param name Runtime API row name.
void emitDocsAnchorJson(std::ostream &os, std::string_view name) {
    std::string anchor = docsFileForRuntimeName(name);
    anchor += '#';
    anchor += runtimeApiSlug(name);
    il::support::printJsonStringEscaped(os, anchor);
}

/// @brief Infer capability tags for a runtime API row from its namespace.
/// @details Tags are broad and additive; future source-authored registry
///          metadata can replace these heuristics without changing consumers.
/// @param name Public runtime name or class name.
/// @return Ordered capability tags.
std::vector<std::string> inferRuntimeCapabilities(std::string_view name) {
    std::vector<std::string> caps;
    if (startsWith(name, "Zanna.GUI.")) {
        caps.push_back("graphics");
        caps.push_back("gui");
    } else if (startsWith(name, "Zanna.Graphics3D.") || startsWith(name, "Zanna.Game3D.")) {
        caps.push_back("graphics");
        caps.push_back("graphics3d");
    } else if (startsWith(name, "Zanna.Graphics2D.") || startsWith(name, "Zanna.Game2D.")) {
        caps.push_back("graphics");
        caps.push_back("graphics2d");
    } else if (startsWith(name, "Zanna.Graphics.")) {
        caps.push_back("graphics");
    }
    if (startsWith(name, "Zanna.Game."))
        caps.push_back("game");
    if (startsWith(name, "Zanna.Input."))
        caps.push_back("input");
    if (startsWith(name, "Zanna.Network."))
        caps.push_back("network");
    if (startsWith(name, "Zanna.Crypto."))
        caps.push_back("crypto");
    if (startsWith(name, "Zanna.IO.") || startsWith(name, "Zanna.Assets."))
        caps.push_back("filesystem");
    if (startsWith(name, "Zanna.Audio."))
        caps.push_back("audio");
    if (startsWith(name, "Zanna.Threads."))
        caps.push_back("threads");
    if (startsWith(name, "Zanna.Localization."))
        caps.push_back("localization");
    if (startsWith(name, "Zanna.Zia.") || startsWith(name, "Zanna.Basic.") ||
        startsWith(name, "Zanna.Project.") || startsWith(name, "Zanna.Workspace."))
        caps.push_back("tooling");
    if (caps.empty())
        caps.push_back("core");
    return caps;
}

/// @brief Return the preferred public replacement for a compatibility row.
/// @details The overhaul preserves old API names for source compatibility, but
///          tools need a machine-readable pointer to the modern spelling. An
///          empty result means the row is not a compatibility alias or no
///          direct one-for-one replacement exists.
/// @param name Public runtime name or class name.
/// @return Preferred replacement runtime name, or empty string.
std::string inferRuntimeMigrationTarget(std::string_view name) {
    // The pre-alpha compatibility sweep removed every alias and legacy
    // spelling from the public catalog, so no row currently carries a
    // migration target. Future deprecations should add entries here so
    // tools regain machine-readable pointers to modern spellings.
    (void)name;
    return {};
}

/// @brief Infer a stability tier for a runtime API row.
/// @details Classifications are discovery metadata only; they do not remove,
///          hide, or disable any existing API.
/// @param name Public runtime name or class name.
/// @return Stability tier string.
std::string inferRuntimeStability(std::string_view name) {
    const std::string_view leaf = lastRuntimeNameSegment(name);
    if (leaf == "AllowInsecureCertificatesForTesting" || leaf == "EnableApprovedModeForProcess" ||
        leaf == "DisableApprovedModeForProcess" || name == "Zanna.Network.HttpReq.SetTlsVerify" ||
        startsWith(name, "Zanna.Runtime.Unsafe."))
        return "unsafe";
    if (!inferRuntimeMigrationTarget(name).empty())
        return "legacy";
    if (startsWith(name, "Zanna.Crypto.Legacy."))
        return "legacy";
    if (startsWith(name, "Zanna.Zia.") || startsWith(name, "Zanna.Basic.") ||
        startsWith(name, "Zanna.Project.") || startsWith(name, "Zanna.Workspace.") ||
        startsWith(name, "Zanna.Assets.") || startsWith(name, "Zanna.GUI.") ||
        startsWith(name, "Zanna.Game3D.") || startsWith(name, "Zanna.Graphics3D."))
        return "preview";
    return "stable";
}

/// @brief Return whether a row exposes diagnostic or mutable last-result state.
/// @details This uses exact public names instead of broad `Error`/`Last`
///          prefixes so ordinary APIs such as `Diagnostics.Log.Error`,
///          `String.LastIndexOf`, and collection `Last()` methods are not
///          reported as side-channel diagnostics.
/// @param name Public runtime name.
/// @param leaf Last dotted segment of @p name.
/// @return True when the row is side-channel diagnostic state.
bool isRuntimeSideChannelDiagnostic(std::string_view name, std::string_view leaf) {
    // The compatibility sweep removed every named side-channel row
    // (Tls.Error, RestClient.Last*, SceneDocument.LastError, ...). The leaf
    // rule remains so any future regression is classified immediately.
    (void)name;
    if (leaf == "get_LastError" || leaf == "get_LastStatus" || leaf == "get_LastResponse")
        return true;
    return false;
}

/// @brief Infer fallibility classification from a public name and signature.
/// @details The value documents current behavior without changing it. Exact
///          future annotations can override these conservative heuristics.
/// @param name Public runtime name or class-qualified member name.
/// @param signature Compact runtime.def signature.
/// @return Fallibility class.
/// @brief Explicit fallibility contracts for IO/workspace/project entries whose
///        trapping behavior the name/signature heuristics cannot infer.
/// @details These allocate, open, or validate a backing resource and trap on
///          failure, so the heuristic default of "infallible" was wrong
///          (VDOC-198). Kept as an exact-name table so the correction is
///          auditable and does not over-broaden the general rules.
std::optional<std::string_view> explicitRuntimeFallibility(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> kOverrides[] = {
        {"Zanna.IO.Stream.AsBinFile", "traps"},   // traps on wrong backing type
        {"Zanna.IO.Stream.AsMemStream", "traps"}, // traps on wrong backing type
        {"Zanna.IO.Stream.ToBytes", "traps"},     // traps on wrong backing type
        {"Zanna.IO.LineWriter.Append", "traps"},  // opens the file; traps on failure
        {"Zanna.IO.Watcher.New", "traps"},        // traps on watch resource failure
        {"Zanna.IO.Watcher.Start", "traps"},      // traps on watch resource failure
        {"Zanna.IO.Archive.Create", "traps"},     // opens the archive; traps on failure
        // Math domain traps the name/signature heuristics cannot infer (VDOC-209):
        {"Zanna.Math.BigInt.Div", "traps"},          // traps on zero divisor
        {"Zanna.Math.BigInt.Mod", "traps"},          // traps on zero divisor
        {"Zanna.Math.BigInt.Pow", "traps"},          // traps on negative exponent
        {"Zanna.Math.BigInt.PowMod", "traps"},       // traps on negative exponent / zero modulus
        {"Zanna.Math.BigInt.Sqrt", "traps"},         // traps on a negative value
        {"Zanna.Math.BigInt.ToStringBase", "traps"}, // traps on a base outside 2..36
        {"Zanna.Math.Mat3.Inverse", "traps"},        // traps on a singular matrix
        {"Zanna.Math.Mat4.Inverse", "traps"},        // traps on a singular matrix
        {"Zanna.Math.Quat.Inverse", "traps"},        // traps on a zero/non-finite length
        {"Zanna.Math.Quat.Slerp", "traps"},          // traps on a non-finite t
        {"Zanna.Math.Vec3.Div", "traps"},            // traps on a zero/non-finite divisor
        {"Zanna.Math.Vec2.Div", "traps"},            // traps on a zero/non-finite divisor
        // System entries the heuristic missed (VDOC-222):
        {"Zanna.Runtime.Unsafe.Release", "traps"},        // traps on an invalid/freed heap object
        {"Zanna.Runtime.Unsafe.ReleaseStr", "traps"},     // traps on an invalid string handle
        {"Zanna.System.Pty.PtySession.Resize", "status"}, // returns a boolean success indicator
        // Time parsers the "Parse* traps" heuristic mislabeled (VDOC-232): these
        // signal failure with a sentinel (0 / -1 / NULL), not a trap.
        {"Zanna.Time.DateTime.ParseIso8601", "sentinel"}, // returns 0 on failure
        {"Zanna.Time.DateTime.ParseDate", "sentinel"},    // returns 0 on failure
        {"Zanna.Time.DateTime.ParseTime", "sentinel"},    // returns -1 on failure
        {"Zanna.Time.DateOnly.Parse", "sentinel"},        // returns NULL on failure
    };
    for (const auto &[key, value] : kOverrides)
        if (name == key)
            return value;
    return std::nullopt;
}

/// @brief Infer how a runtime API row communicates operational failure.
/// @details Applies explicit per-row policy first, then recognizes option/result,
///          nullable, status, sentinel, side-channel, and trap conventions from
///          the reviewed API boundaries and public name/signature shape.
/// @param name Public runtime function or binding-target name.
/// @param signature Compact runtime signature.
/// @return Stable fallibility classification for generated API metadata.
std::string inferRuntimeFallibility(std::string_view name, std::string_view signature) {
    const RuntimeApiSignatureParts sig = parseRuntimeApiSignature(signature);
    const std::string_view leaf = lastRuntimeNameSegment(name);
    if (isRuntimeSideChannelDiagnostic(name, leaf))
        return "side-channel";
    if (auto explicitContract = explicitRuntimeFallibility(name))
        return std::string(*explicitContract);
    if (sig.valid) {
        if (endsWith(sig.returnType, "?"))
            return "option";
        if (startsWith(sig.returnType, "obj<Zanna.Result"))
            return "result";
        if (startsWith(sig.returnType, "obj<Zanna.Option"))
            return "option";
        if (startsWith(sig.returnType, "obj<Zanna.Game.PathResult") ||
            startsWith(sig.returnType, "obj<Zanna.Game.QueryResult") ||
            startsWith(sig.returnType, "obj<Zanna.Game.QuadtreePairResult"))
            return "result";
    }
    if (isThreeDRuntimeName(name) || isGuiRuntimeName(name)) {
        if (startsWith(leaf, "Try") && sig.valid && sig.returnType == "i1")
            return "status";
        if (runtimeReturnNullable(name, signature).value_or(false))
            return "nullable";
    }
    if (startsWith(leaf, "Try") || startsWith(leaf, "Find")) {
        if (leaf == "FindAll")
            return "infallible";
        if (name == "Zanna.Time.TimeZone.Find")
            return "traps";
        if (sig.valid && sig.returnType == "i1")
            return "infallible";
        return "sentinel";
    }
    if (startsWith(leaf, "Unwrap") || startsWith(leaf, "Expect"))
        return "traps";
    if ((startsWith(leaf, "Save") || startsWith(leaf, "Load")) && sig.valid &&
        sig.returnType == "i1")
        return "status";
    if ((startsWith(name, "Zanna.IO.") || startsWith(name, "Zanna.System.")) &&
        (startsWith(leaf, "Read") || startsWith(leaf, "Write")))
        return "traps";
    if (leaf == "Parse" || startsWith(leaf, "Parse") || startsWith(leaf, "Load") ||
        startsWith(leaf, "Open") || startsWith(leaf, "Connect") || startsWith(leaf, "Decrypt"))
        return "traps";
    return "infallible";
}

/// @brief Infer return ownership for a runtime API row.
/// @details Advisory metadata for generated docs and tools. Exact ownership
///          rules remain governed by runtime docs until source annotations land.
/// @param name Public runtime name or class-qualified member name.
/// @param signature Compact runtime.def signature.
/// @return Ownership classification.
std::string inferRuntimeOwnership(std::string_view name, std::string_view signature) {
    const RuntimeApiSignatureParts sig = parseRuntimeApiSignature(signature);
    if (!sig.valid || sig.returnType == "void")
        return "none";
    if (sig.returnType == "i1" || sig.returnType == "i8" || sig.returnType == "i16" ||
        sig.returnType == "i32" || sig.returnType == "i64" || sig.returnType == "f32" ||
        sig.returnType == "f64" || sig.returnType == "bool")
        return "value";
    if (isGuiRuntimeName(name)) {
        if (startsWith(sig.returnType, "obj<Zanna.Option") ||
            startsWith(sig.returnType, "obj<Zanna.Result") ||
            startsWith(sig.returnType, "obj<Zanna.Threads.Future") ||
            startsWith(sig.returnType, "obj<Zanna.Collections.Map") ||
            startsWith(sig.returnType, "obj<Zanna.Collections.Seq") ||
            startsWith(sig.returnType, "seq<"))
            return "owned";
        if (sig.returnType == "str" || sig.returnType == "string")
            return "managed";
        const std::string_view leaf = lastRuntimeNameSegment(name);
        if (startsWith(leaf, "New") || startsWith(leaf, "From") || startsWith(leaf, "Load") ||
            leaf == "Clone" || leaf == "AttachBuffer" || name == "Zanna.GUI.Theme.GetPalette")
            return "managed";
        if (sig.returnType == "obj" || sig.returnType == "ptr" ||
            startsWith(sig.returnType, "obj<"))
            return "borrowed";
    }
    if (isThreeDRuntimeName(name)) {
        if (sig.returnType == "ptr")
            return "borrowed";
        if (sig.returnType == "str" || sig.returnType == "string" || sig.returnType == "obj" ||
            startsWith(sig.returnType, "obj<") || startsWith(sig.returnType, "seq<"))
            return "managed";
    }
    if (sig.returnType == "str" || sig.returnType == "string")
        return "owned";
    // A few concretely typed object returns hand back a borrowed handle into
    // process-lifetime static data rather than a freshly allocated value the
    // caller owns. They must be classified "borrowed" before the generic
    // `obj<...>` = owned rule below, or tools would schedule an erroneous release
    // of static data (VDOC-228).
    static constexpr std::string_view kBorrowedTypedObj[] = {
        "Zanna.Time.TimeZone.Find", // returns a static, must-not-free TimeZone handle
    };
    for (auto entry : kBorrowedTypedObj)
        if (name == entry)
            return "borrowed";
    if (startsWith(sig.returnType, "obj<Zanna.Option") ||
        startsWith(sig.returnType, "obj<Zanna.Result") ||
        startsWith(sig.returnType, "obj<Zanna.Threads.Future"))
        return "owned";
    const std::string_view leaf = lastRuntimeNameSegment(name);
    if (startsWith(leaf, "New") || startsWith(leaf, "From") || startsWith(leaf, "Load") ||
        startsWith(leaf, "Open") || startsWith(leaf, "Create") || leaf == "Clone" || leaf == "Copy")
        return "owned";
    // A concretely typed sequence or object return is a freshly allocated GC
    // value the caller owns a reference to (fresh Seq/Map/Bytes and typed
    // wrappers like Stream.As*), not a borrowed handle — the previous default of
    // "unknown" understated the contract (VDOC-198). Bare `obj`/`ptr` stays
    // "unknown" because it can be borrowed.
    if (startsWith(sig.returnType, "seq<") || startsWith(sig.returnType, "obj<"))
        return "owned";
    // A handful of typed returns are declared as bare `obj` but are documented
    // owned allocations; annotate them explicitly.
    static constexpr std::string_view kOwnedBareObj[] = {
        "Zanna.IO.LineWriter.Append", // returns a new owned LineWriter
    };
    for (auto entry : kOwnedBareObj)
        if (name == entry)
            return "owned";
    // Math values are immutable and value-oriented: every operation that returns
    // an object (BigInt, Vec*, Mat*, Quat, Spline result) allocates a fresh GC
    // value the caller owns, even when the registry types it as bare `obj`. The
    // previous "unknown" understated this for the exact APIs the inventory is
    // meant to describe (VDOC-209).
    if (startsWith(name, "Zanna.Math.") &&
        (sig.returnType == "obj" || startsWith(sig.returnType, "seq")))
        return "owned";
    // Time object results (DateOnly/DateRange/Stopwatch arithmetic, queries, and
    // parses such as AddDays, StartOfMonth, Intersection, Today, Parse, StartNew)
    // are freshly allocated GC values the caller owns, even when the registry types
    // them as bare `obj`. TimeZone.Find is the one Time object that is borrowed and
    // is handled by the borrowed override above, so a blanket owned rule for the
    // remaining `Zanna.Time.*` object returns is correct (VDOC-232).
    if (startsWith(name, "Zanna.Time.") &&
        (sig.returnType == "obj" || startsWith(sig.returnType, "seq")))
        return "owned";
    return "unknown";
}

/// @brief Infer a unit for a numeric parameter.
/// @param name Public runtime name or class-qualified member name.
/// @param type Parameter type token.
/// @return Unit name, or empty when no unit is inferred.
std::string inferRuntimeParamUnit(std::string_view name, std::string_view type) {
    if (type != "i64" && type != "f64")
        return {};
    const std::string_view leaf = lastRuntimeNameSegment(name);
    if (leaf.find("Seconds") != std::string_view::npos ||
        leaf.find("Sec") != std::string_view::npos)
        return "seconds";
    if (leaf.find("Milliseconds") != std::string_view::npos ||
        leaf.find("Millis") != std::string_view::npos ||
        leaf.find("Ms") != std::string_view::npos ||
        leaf.find("Timeout") != std::string_view::npos ||
        leaf.find("Sleep") != std::string_view::npos ||
        leaf.find("Wait") != std::string_view::npos ||
        leaf.find("Delay") != std::string_view::npos ||
        leaf.find("Duration") != std::string_view::npos || endsWith(leaf, "For"))
        return "milliseconds";
    if (leaf == "Update" && type == "f64")
        return "seconds";
    return {};
}

/// @brief Infer a semantic domain for a primitive parameter.
/// @param name Public runtime name or class-qualified member name.
/// @param type Parameter type token.
/// @return Domain name, or empty when no domain is inferred.
std::string inferRuntimeParamDomain(std::string_view name, std::string_view type) {
    if (type != "i64")
        return {};
    if (name.find("Key") != std::string_view::npos)
        return "input-key";
    if (name.find("Mouse") != std::string_view::npos)
        return "mouse-button";
    if (name.find("Color") != std::string_view::npos || name.find("RGB") != std::string_view::npos)
        return "color";
    if (name.find("Layer") != std::string_view::npos || name.find("Mask") != std::string_view::npos)
        return "layer-mask";
    if (name.find("Status") != std::string_view::npos)
        return "status-code";
    if (name.find("Log") != std::string_view::npos)
        return "log-level";
    return {};
}

/// @brief Emit parsed return and parameter metadata for a signature.
/// @param os Stream receiving JSON.
/// @param name Runtime row name used for unit/domain hints.
/// @param signature Compact runtime.def signature.
void emitRuntimeSignatureMetadataJson(std::ostream &os,
                                      std::string_view name,
                                      std::string_view signature) {
    const RuntimeApiSignatureParts sig = parseRuntimeApiSignature(signature);
    os << ",\"return_type\":";
    if (sig.valid) {
        emitRuntimeApiTypeJson(os, sig.returnType, runtimeReturnNullable(name, signature));
    } else {
        os << "null";
    }
    os << ",\"params\":[";
    if (sig.valid) {
        for (size_t i = 0; i < sig.paramTypes.size(); ++i) {
            if (i)
                os << ',';
            os << "{\"index\":" << i << ",\"type\":";
            emitRuntimeApiTypeJson(os, sig.paramTypes[i]);
            if (const std::string unit = inferRuntimeParamUnit(name, sig.paramTypes[i]);
                !unit.empty()) {
                os << ",\"unit\":";
                il::support::printJsonStringEscaped(os, unit);
            }
            if (const std::string domain = inferRuntimeParamDomain(name, sig.paramTypes[i]);
                !domain.empty()) {
                os << ",\"domain\":";
                il::support::printJsonStringEscaped(os, domain);
            }
            os << '}';
        }
    }
    os << ']';
}

/// @brief Emit common contract metadata fields for one runtime row.
/// @param os Stream receiving JSON.
/// @param name Public runtime name or class-qualified member name.
/// @param signature Compact runtime.def signature.
void emitRuntimeContractMetadataJson(std::ostream &os,
                                     std::string_view name,
                                     std::string_view signature) {
    os << ",\"stability\":";
    il::support::printJsonStringEscaped(os, inferRuntimeStability(name));
    os << ",\"capabilities\":";
    emitStringArrayJson(os, inferRuntimeCapabilities(name));
    os << ",\"fallibility\":";
    il::support::printJsonStringEscaped(os, inferRuntimeFallibility(name, signature));
    os << ",\"ownership\":";
    il::support::printJsonStringEscaped(os, inferRuntimeOwnership(name, signature));
    os << ",\"contract_source\":";
    il::support::printJsonStringEscaped(os, runtimeContractSource(name));
    if (const std::string migrationTarget = inferRuntimeMigrationTarget(name);
        !migrationTarget.empty()) {
        os << ",\"migration_target\":";
        il::support::printJsonStringEscaped(os, migrationTarget);
    }
    os << ",\"docs_anchor\":";
    emitDocsAnchorJson(os, name);
}

/// @brief Infer the broad public contract kind for a runtime class.
/// @details The inference uses existing layout, constructor, and member shape so
///          no runtime.def syntax changes are required for this additive slice.
/// @param klass Runtime class descriptor from the generated catalog.
/// @return Class kind string.
std::string inferRuntimeClassKind(const il::runtime::RuntimeClass &klass) {
    const std::string_view name = klass.qname ? std::string_view(klass.qname) : std::string_view();
    const std::string_view layout =
        klass.layout ? std::string_view(klass.layout) : std::string_view();
    const bool hasCtor = klass.ctor && std::string_view(klass.ctor).size() > 0;
    if (layout == "none" && klass.methods.empty() && !klass.properties.empty())
        return "enum-like";
    if (!hasCtor && !klass.methods.empty() && klass.properties.empty())
        return "static-module";
    if (name.find("Vec") != std::string_view::npos || name.find("Mat") != std::string_view::npos ||
        name.find("Quat") != std::string_view::npos || endsWith(name, ".Color") ||
        endsWith(name, ".Duration") || endsWith(name, ".DateTime") || endsWith(name, ".DateOnly"))
        return "value-object";
    if (layout == "none" && !hasCtor)
        return "static-module";
    if (hasCtor)
        return "instance-handle";
    return "namespace-facade";
}

/// @brief Build a descriptor lookup map keyed by canonical runtime function name.
/// @details Class methods reference canonical target names. This map lets the
///          API dump compare method and target arity to classify static methods.
/// @return Map from canonical name to runtime descriptor pointer.
std::unordered_map<std::string_view, const il::runtime::RuntimeDescriptor *>
buildRuntimeDescriptorByName() {
    std::unordered_map<std::string_view, const il::runtime::RuntimeDescriptor *> map;
    for (const auto &descriptor : il::runtime::runtimeRegistry())
        map.emplace(descriptor.name, &descriptor);
    return map;
}

/// @brief Infer whether a class method is receiverless/static.
/// @details Method signatures omit implicit receivers; descriptor signatures
///          include the C ABI shape. Equal arity means receiverless, while one
///          extra descriptor parameter means implicit receiver.
/// @param method Class method descriptor.
/// @param descriptors Runtime descriptor map keyed by canonical target name.
/// @return True when the method appears receiverless/static.
bool inferRuntimeMethodIsStatic(
    const il::runtime::RuntimeMethod &method,
    const std::unordered_map<std::string_view, const il::runtime::RuntimeDescriptor *>
        &descriptors) {
    const RuntimeApiSignatureParts methodSig =
        parseRuntimeApiSignature(method.signature ? method.signature : "");
    auto it =
        descriptors.find(method.target ? std::string_view(method.target) : std::string_view());
    if (!methodSig.valid || it == descriptors.end())
        return false;
    const RuntimeApiSignatureParts targetSig = parseRuntimeApiSignature(it->second->signatureText);
    if (!targetSig.valid)
        return false;
    return targetSig.paramTypes.size() == methodSig.paramTypes.size();
}

/// @brief Map an opcode TypeCategory to its stable JSON name.
/// @param category Opcode result or operand type category.
/// @return Static lowercase category spelling used by `--dump-opcodes`.
const char *typeCategoryName(il::core::TypeCategory category) {
    using il::core::TypeCategory;
    switch (category) {
        case TypeCategory::None:
            return "none";
        case TypeCategory::Void:
            return "void";
        case TypeCategory::I1:
            return "i1";
        case TypeCategory::I16:
            return "i16";
        case TypeCategory::I32:
            return "i32";
        case TypeCategory::I64:
            return "i64";
        case TypeCategory::F64:
            return "f64";
        case TypeCategory::Ptr:
            return "ptr";
        case TypeCategory::Str:
            return "str";
        case TypeCategory::Error:
            return "error";
        case TypeCategory::ResumeTok:
            return "resume_tok";
        case TypeCategory::Any:
            return "any";
        case TypeCategory::InstrType:
            return "instr_type";
        case TypeCategory::Dynamic:
            return "dynamic";
    }
    return "none";
}

/// @brief Implement `--dump-opcodes`: print the IL opcode registry as a JSON
///        array on stdout, generated from the live opcode metadata table so it
///        cannot drift from Opcode.def.
/// @return 0 on success.
int dumpOpcodes() {
    using il::core::ResultArity;
    using il::support::printJsonStringEscaped;
    std::ostream &os = std::cout;

    os << "{\"ilVersion\":";
    printJsonStringEscaped(os, ZANNA_IL_VERSION_STR);
    os << ",\"opcodes\":[";
    bool first = true;
    for (const auto op : il::core::all_opcodes()) {
        const auto &info = il::core::getOpcodeInfo(op);
        if (!first)
            os << ",";
        first = false;
        os << "{\"mnemonic\":";
        printJsonStringEscaped(os, info.name);
        os << ",\"resultArity\":\""
           << (info.resultArity == ResultArity::None
                   ? "none"
                   : (info.resultArity == ResultArity::One ? "one" : "optional"))
           << "\"";
        os << ",\"resultType\":\"" << typeCategoryName(info.resultType) << "\"";
        os << ",\"operandsMin\":" << static_cast<int>(info.numOperandsMin);
        if (info.numOperandsMax == il::core::kVariadicOperandCount)
            os << ",\"operandsMax\":-1";
        else
            os << ",\"operandsMax\":" << static_cast<int>(info.numOperandsMax);
        os << ",\"operandTypes\":[";
        for (size_t i = 0; i < info.operandTypes.size(); ++i) {
            if (i)
                os << ",";
            os << "\"" << typeCategoryName(info.operandTypes[i]) << "\"";
        }
        os << "]";
        os << ",\"sideEffects\":" << (info.hasSideEffects ? "true" : "false");
        os << ",\"successors\":" << static_cast<int>(info.numSuccessors);
        os << ",\"terminator\":" << (info.isTerminator ? "true" : "false");
        os << "}";
    }
    os << "]}\n";
    return 0;
}

/// @brief Implement `--dump-runtime-api`: print the full runtime surface
///        (global functions and classes with members) as one JSON document on
///        stdout. The document is generated from the live registry, so it can
///        never drift from the binary.
/// @return 0 on success.
int dumpRuntimeApi() {
    using il::support::printJsonStringEscaped;
    std::ostream &os = std::cout;
    const auto descriptorsByName = buildRuntimeDescriptorByName();

    os << "{\"version\":";
    printJsonStringEscaped(os, ZANNA_VERSION_STR);
    os << ",\"schema_version\":4";
    os << ",\"signature_dialect\":\"runtime-def-v1\"";
    os << ",\"public_boundary\":\"registry\"";
    os << ",\"c_abi_status\":\"internal-embedding\"";

    // Global runtime functions with canonical Zanna.* names.
    os << ",\"functions\":[";
    {
        const auto &reg = il::runtime::runtimeRegistry();
        bool first = true;
        for (const auto &d : reg) {
            if (d.name.rfind("Zanna.", 0) != 0)
                continue; // skip rt_* aliases; canonical names carry the API
            if (!d.publicSurface)
                continue;
            if (!first)
                os << ",";
            first = false;
            os << "{\"name\":";
            printJsonStringEscaped(os, d.name);
            os << ",\"signature\":";
            printJsonStringEscaped(os, d.signatureText);
            os << ",\"c_symbol\":";
            if (!d.cSymbol.empty())
                printJsonStringEscaped(os, d.cSymbol);
            else
                os << "null";
            os << ",\"kind\":\"function\"";
            os << ",\"owner\":";
            printJsonStringEscaped(os, runtimeApiOwner(d.name));
            emitRuntimeSignatureMetadataJson(os, d.name, d.signatureText);
            emitRuntimeContractMetadataJson(os, d.name, d.signatureText);
            os << "}";
        }
    }
    os << "]";

    // Runtime classes with properties and methods.
    os << ",\"classes\":[";
    {
        const auto &classes = il::runtime::runtimeClassCatalog();
        bool firstClass = true;
        for (const auto &c : classes) {
            if (!firstClass)
                os << ",";
            firstClass = false;
            os << "{\"name\":";
            printJsonStringEscaped(os, c.qname ? c.qname : "");
            os << ",\"constructor\":";
            printJsonStringEscaped(os, c.ctor ? c.ctor : "");
            os << ",\"constructor_c_symbol\":";
            {
                const auto ctor =
                    descriptorsByName.find(c.ctor ? std::string_view(c.ctor) : std::string_view());
                if (ctor != descriptorsByName.end() && !ctor->second->cSymbol.empty())
                    printJsonStringEscaped(os, ctor->second->cSymbol);
                else
                    os << "null";
            }
            os << ",\"kind\":\"class\"";
            os << ",\"owner\":";
            printJsonStringEscaped(os, runtimeApiOwner(c.qname ? c.qname : ""));
            os << ",\"class_kind\":";
            printJsonStringEscaped(os, inferRuntimeClassKind(c));
            os << ",\"stability\":";
            printJsonStringEscaped(os, inferRuntimeStability(c.qname ? c.qname : ""));
            if (const std::string migrationTarget =
                    inferRuntimeMigrationTarget(c.qname ? c.qname : "");
                !migrationTarget.empty()) {
                os << ",\"migration_target\":";
                printJsonStringEscaped(os, migrationTarget);
            }
            os << ",\"capabilities\":";
            emitStringArrayJson(os, inferRuntimeCapabilities(c.qname ? c.qname : ""));
            os << ",\"contract_source\":";
            printJsonStringEscaped(os, runtimeContractSource(c.qname ? c.qname : ""));
            os << ",\"docs_anchor\":";
            emitDocsAnchorJson(os, c.qname ? c.qname : "");
            os << ",\"documentation\":{\"summary\":";
            printJsonStringEscaped(os, c.summary ? c.summary : "");
            os << ",\"details\":";
            printJsonStringEscaped(os, c.details ? c.details : "");
            os << ",\"format\":\"markdown\"}";
            os << ",\"properties\":[";
            bool firstProp = true;
            for (const auto &p : c.properties) {
                if (!firstProp)
                    os << ",";
                firstProp = false;
                os << "{\"name\":";
                printJsonStringEscaped(os, p.name ? p.name : "");
                os << ",\"type\":";
                printJsonStringEscaped(os, p.type ? p.type : "");
                os << ",\"readonly\":" << (p.readonly ? "true" : "false");
                os << ",\"kind\":\"property\"";
                os << ",\"owner\":";
                printJsonStringEscaped(os, c.qname ? c.qname : "");
                os << ",\"getter\":";
                printJsonStringEscaped(os, p.getter ? p.getter : "");
                os << ",\"getter_c_symbol\":";
                {
                    const auto getter = descriptorsByName.find(p.getter ? std::string_view(p.getter)
                                                                        : std::string_view());
                    if (getter != descriptorsByName.end() && !getter->second->cSymbol.empty())
                        printJsonStringEscaped(os, getter->second->cSymbol);
                    else
                        os << "null";
                }
                os << ",\"setter\":";
                if (p.setter)
                    printJsonStringEscaped(os, p.setter);
                else
                    os << "null";
                os << ",\"setter_c_symbol\":";
                {
                    const auto setter = descriptorsByName.find(p.setter ? std::string_view(p.setter)
                                                                        : std::string_view());
                    if (setter != descriptorsByName.end() && !setter->second->cSymbol.empty())
                        printJsonStringEscaped(os, setter->second->cSymbol);
                    else
                        os << "null";
                }
                const std::string propertyName =
                    std::string(c.qname ? c.qname : "") + "." + (p.name ? p.name : "");
                const std::string propertyTarget =
                    p.getter && *p.getter != '\0'
                        ? p.getter
                        : (p.setter && *p.setter != '\0' ? p.setter : propertyName);
                const std::string propertySignature = std::string(p.type ? p.type : "") + "()";
                os << ",\"return_type\":";
                emitRuntimeApiTypeJson(os,
                                       p.type ? p.type : "",
                                       runtimeReturnNullable(propertyTarget, propertySignature));
                os << ",\"stability\":";
                printJsonStringEscaped(os, inferRuntimeStability(p.getter ? p.getter : ""));
                os << ",\"capabilities\":";
                emitStringArrayJson(os, inferRuntimeCapabilities(c.qname ? c.qname : ""));
                os << ",\"fallibility\":";
                printJsonStringEscaped(os,
                                       inferRuntimeFallibility(propertyTarget, propertySignature));
                os << ",\"ownership\":";
                printJsonStringEscaped(os,
                                       inferRuntimeOwnership(propertyTarget, propertySignature));
                os << ",\"contract_source\":";
                printJsonStringEscaped(os, runtimeContractSource(propertyTarget));
                os << ",\"docs_anchor\":";
                emitDocsAnchorJson(os, propertyName);
                os << "}";
            }
            os << "],\"methods\":[";
            bool firstMethod = true;
            for (const auto &m : c.methods) {
                if (!firstMethod)
                    os << ",";
                firstMethod = false;
                os << "{\"name\":";
                printJsonStringEscaped(os, m.name ? m.name : "");
                os << ",\"signature\":";
                printJsonStringEscaped(os, m.signature ? m.signature : "");
                os << ",\"kind\":\"method\"";
                os << ",\"owner\":";
                printJsonStringEscaped(os, c.qname ? c.qname : "");
                os << ",\"target\":";
                printJsonStringEscaped(os, m.target ? m.target : "");
                os << ",\"c_symbol\":";
                {
                    const auto target = descriptorsByName.find(m.target ? std::string_view(m.target)
                                                                        : std::string_view());
                    if (target != descriptorsByName.end() && !target->second->cSymbol.empty())
                        printJsonStringEscaped(os, target->second->cSymbol);
                    else
                        os << "null";
                }
                os << ",\"is_static\":"
                   << (inferRuntimeMethodIsStatic(m, descriptorsByName) ? "true" : "false");
                const std::string methodName =
                    std::string(c.qname ? c.qname : "") + "." + (m.name ? m.name : "");
                emitRuntimeSignatureMetadataJson(os, methodName, m.signature ? m.signature : "");
                emitRuntimeContractMetadataJson(
                    os, m.target ? m.target : methodName, m.signature ? m.signature : "");
                os << "}";
            }
            os << "]}";
        }
    }
    os << "]}\n";
    return 0;
}

/// @brief Implement `--dump-runtime-classes`: print the runtime class catalog
///        (properties, methods, constructors) in a stable format to stdout.
/// @return 0 on success.
int dumpRuntimeClasses() {
    const auto &classes = il::runtime::runtimeClassCatalog();
    for (const auto &c : classes) {
        std::cout << "CLASS " << (c.qname ? c.qname : "<unnamed>")
                  << " (type: " << (c.layout ? c.layout : "<unknown>") << ")\n";
        for (const auto &p : c.properties) {
            std::cout << "  PROP " << (p.name ? p.name : "<unnamed>") << ": "
                      << (p.type ? p.type : "<type>") << "  -> "
                      << (p.getter ? p.getter : "<getter>") << "\n";
        }
        for (const auto &m : c.methods) {
            std::cout << "  METH " << (m.name ? m.name : "<unnamed>") << "("
                      << (m.signature ? m.signature : "") << ") -> "
                      << (m.target ? m.target : "<target>") << "\n";
        }
        if (c.ctor && std::string(c.ctor).size())
            std::cout << "  CTOR -> " << c.ctor << "\n";
    }
    return 0;
}
} // namespace

/// @brief Print synopsis and option hints for the `zanna` CLI.
///
/// @details The top-level help intentionally stays short. Subcommand-specific
///          help carries detailed flags so unrelated implementation options do
///          not crowd the common command list.
/// @param out Stream that receives the formatted top-level help text.
void printTopLevelUsage(std::ostream &out) {
    out << "zanna v" << ZANNA_VERSION_STR << "\n"
        << "Usage: zanna <command> [arguments]\n"
        << "\n"
        << "Common commands:\n"
        << "       zanna run [target] [options] [-- program-args...]\n"
        << "       zanna build [target] [-o output] [options]\n"
        << "       zanna build-many --output-dir DIR name=project [...]\n"
        << "       zanna check [target] [options]\n"
        << "       zanna init <project-name> [--lang zia|basic]\n"
        << "       zanna repl [zia|basic]\n"
        << "       zanna eval [options] [code]\n"
        << "       zanna explain <diagnostic-code>\n"
        << "       zanna package [target] [--target macos|linux|windows|tarball] [-o output]\n"
        << "       zanna asset bake|validate <model> [output.scene3d] [options]\n"
        << "\n"
        << "Developer commands:\n"
        << "       zanna front zia|basic ...\n"
        << "       zanna -run <file.il> ...\n"
        << "       zanna il-opt <file.il> -o <out.il> ...\n"
        << "       zanna codegen x64|arm64 <file.il> ...\n"
        << "       zanna bench <file.il> ...\n"
        << "       zanna install-package ...\n"
        << "\n"
        << "Targets:\n"
        << "  A target is a .zia file, .bas file, directory, or zanna.project path.\n"
        << "  'zanna run' additionally accepts a .il file and executes it on the VM.\n"
        << "  If omitted where supported, the target defaults to the current directory.\n"
        << "\n"
        << "Help:\n"
        << "       zanna help <command>\n"
        << "       zanna help package\n"
        << "       zanna help front zia|basic\n"
        << "       zanna help codegen x64|arm64\n"
        << "       zanna --version\n";
}

/// @brief Print top-level usage to the standard error stream.
void usage() {
    printTopLevelUsage(std::cerr);
}

/// @brief Invoke a subcommand handler with a synthetic `--help` argument vector.
/// @details Lets `zanna help <command>` reuse each subcommand's own help text.
int invokeHelp(int (*handler)(int, char **)) {
    char helpFlag[] = "--help";
    char *helpArgv[] = {helpFlag};
    return handler(1, helpArgv);
}

/// @brief Print usage for the `zanna codegen` subcommand (architectures + options).
/// @param out Stream that receives the code-generation help text.
void codegenUsage(std::ostream &out = std::cerr) {
    out << "Usage: zanna codegen <arch> <file.il> [options]\n"
        << "\n"
        << "Architectures:\n"
        << "  x64\n"
        << "  arm64\n"
        << "\n"
        << "Use 'zanna help codegen x64' or 'zanna help codegen arm64' for backend options.\n";
}

namespace zanna::tools::ilc {

/// @brief Adapter invoked by `zanna codegen x64` from the top-level driver.
/// @details The driver hands control to this helper with `argv` still pointing
///          at the architecture token. The helper strips it before delegating to
///          the actual command implementation so existing parsing logic
///          continues to expect the IL input path as the leading argument.
/// @param argc Number of command-line arguments following the "codegen"
///             subcommand.
/// @param argv Argument vector beginning with the target architecture token.
/// @return Exit status propagated from @ref cmd_codegen_x64.
int run_codegen_x64(int argc, char **argv) {
    return cmd_codegen_x64(argc - 1, argv + 1);
}

} // namespace zanna::tools::ilc

/// @brief Program entry for the `zanna` command-line tool.
///
/// @param argc Number of command-line arguments.
/// @param argv Array of argument strings.
/// @return Exit status of the selected subcommand or `1` on error.
/// @details The first argument determines which handler processes the request:
///          `cmdRunIL` executes `.il` programs, `cmdILOpt` performs
///          optimization passes, and `cmdFrontZia` / `cmdFrontBasic` drive the
///          legacy frontend entry points. Step-by-step summary:
///          1. Verify that at least one subcommand argument is provided.
///          2. Handle `--version` by delegating to @ref printVersion.
///          3. Dispatch to the matching handler with the remaining arguments.
///          4. Fall back to displaying usage when no match exists.
int main(int argc, char **argv) {
    zanna::tools::Utf8CommandLine commandLine(argc, argv);
    if (!commandLine.applyOrReport(argc, argv))
        return 1;
#if ZANNA_HOST_WINDOWS
    // Disable Windows abort dialog so runtime panics exit cleanly
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    if (argc < 2) {
        usage();
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") {
        printTopLevelUsage(std::cout);
        return 0;
    }
    if (cmd == "--version") {
        printVersion();
        return 0;
    }
    if (cmd == "--dump-runtime-descriptors") {
        return dumpRuntimeDescriptors();
    }
    if (cmd == "--dump-runtime-classes") {
        return dumpRuntimeClasses();
    }
    if (cmd == "--dump-runtime-api") {
        return dumpRuntimeApi();
    }
    if (cmd == "--dump-opcodes") {
        return dumpOpcodes();
    }
    if (cmd == "--print-error-codes") {
        bool json = false;
        if (argc == 3 && std::string_view(argv[2]) == "--json") {
            json = true;
        } else if (argc > 2) {
            std::cerr << "error: --print-error-codes accepts only optional --json\n";
            return 1;
        }
        return printErrorCodes(json);
    }
    if (cmd == "run") {
        return cmdRun(argc - 2, argv + 2);
    }
    if (cmd == "build") {
        return cmdBuild(argc - 2, argv + 2);
    }
    if (cmd == "build-many") {
        return cmdBuildMany(argc - 2, argv + 2);
    }
    if (cmd == "check") {
        return cmdCheck(argc - 2, argv + 2);
    }
    if (cmd == "init") {
        return cmdInit(argc - 2, argv + 2);
    }
    if (cmd == "asset") {
        return cmdAsset(argc - 2, argv + 2);
    }
    if (cmd == "package") {
        return cmdPackage(argc - 2, argv + 2);
    }
    if (cmd == "install-package") {
        return cmdInstallPackage(argc - 2, argv + 2);
    }
    if (cmd == "repl") {
        return cmdRepl(argc - 2, argv + 2);
    }
    if (cmd == "eval") {
        return cmdEval(argc - 2, argv + 2);
    }
    if (cmd == "explain") {
        return cmdExplain(argc - 2, argv + 2);
    }
    if (cmd == "help") {
        if (argc < 3) {
            printTopLevelUsage(std::cout);
            return 1;
        }
        const std::string_view topic = argv[2];
        if (topic == "run")
            return invokeHelp(cmdRun);
        if (topic == "build")
            return invokeHelp(cmdBuild);
        if (topic == "build-many")
            return invokeHelp(cmdBuildMany);
        if (topic == "check")
            return invokeHelp(cmdCheck);
        if (topic == "init")
            return invokeHelp(cmdInit);
        if (topic == "package")
            return invokeHelp(cmdPackage);
        if (topic == "install-package")
            return invokeHelp(cmdInstallPackage);
        if (topic == "repl")
            return invokeHelp(cmdRepl);
        if (topic == "asset")
            return cmdAssetHelp(stdout);
        if (topic == "eval")
            return invokeHelp(cmdEval);
        if (topic == "explain")
            return invokeHelp(cmdExplain);
        if (topic == "-run")
            return invokeHelp(cmdRunIL);
        if (topic == "il-opt")
            return invokeHelp(cmdILOpt);
        if (topic == "bench")
            return invokeHelp(cmdBench);
        if (topic == "front") {
            if (argc >= 4 && std::string_view(argv[3]) == "zia")
                return invokeHelp(cmdFrontZia);
            if (argc >= 4 && std::string_view(argv[3]) == "basic")
                return invokeHelp(cmdFrontBasic);
            std::cerr << "Usage: zanna front zia|basic ...\n"
                      << "Use 'zanna help front zia' or 'zanna help front basic'.\n";
            return 1;
        }
        if (topic == "codegen") {
            if (argc >= 4 && std::string_view(argv[3]) == "x64")
                return invokeHelp(zanna::tools::ilc::cmd_codegen_x64);
            if (argc >= 4 && std::string_view(argv[3]) == "arm64")
                return invokeHelp(zanna::tools::ilc::cmd_codegen_arm64);
            codegenUsage(std::cout);
            return 0;
        }
        std::cerr << "unknown help topic: " << topic << "\n";
        usage();
        return 1;
    }
    if (cmd == "-run") {
        return cmdRunIL(argc - 2, argv + 2);
    }
    if (cmd == "il-opt") {
        return cmdILOpt(argc - 2, argv + 2);
    }
    if (cmd == "bench") {
        return cmdBench(argc - 2, argv + 2);
    }
    if (cmd == "codegen") {
        if (argc >= 3 &&
            (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
            codegenUsage(std::cout);
            return 0;
        }
        if (argc >= 3) {
            if (std::string_view(argv[2]) == "x64") {
                return zanna::tools::ilc::run_codegen_x64(argc - 2, argv + 2);
            }
            if (std::string_view(argv[2]) == "arm64") {
                return zanna::tools::ilc::cmd_codegen_arm64(argc - 3, argv + 3);
            }
        }
        codegenUsage();
        return 1;
    }
    if (cmd == "front" && argc >= 3 && std::string(argv[2]) == "basic") {
        return cmdFrontBasic(argc - 3, argv + 3);
    }
    if (cmd == "front" && argc >= 3 && std::string(argv[2]) == "zia") {
        return cmdFrontZia(argc - 3, argv + 3);
    }
    usage();
    return 1;
}
