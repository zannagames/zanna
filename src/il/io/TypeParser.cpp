//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Provides the textual-to-enumeration bridge for IL type mnemonics.  Tools and
// tests rely on this translation unit to map lowercase tokens such as `i64` and
// `ptr` into @ref il::core::Type instances without instantiating the full parser
// stack.  Keeping the logic concentrated here ensures that every consumer shares
// the same keyword spelling and optional success flag semantics.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Helpers for converting textual IL type names into @ref il::core::Type.
/// @details The parsing functions accept lower-case mnemonic tokens and translate
///          them into the strongly typed representation used by the rest of the
///          compiler.  The helpers are pure and do not retain references to
///          caller data.

#include "il/internal/io/TypeParser.hpp"

namespace il::io {

/// @brief Resolve a primitive IL type token to a concrete Type value.
/// @details Performs a string comparison against the canonical lowercase
///          mnemonics recognised by the IL textual format.  When @p token matches
///          a known type the helper constructs the corresponding
///          @ref il::core::Type and, if provided, stores @c true into @p ok.  On
///          failure the returned type is default constructed and @p ok receives
///          @c false, allowing callers to distinguish unsupported tokens without
///          relying on exceptions.
/// @param token Lowercase identifier naming a supported primitive type (e.g., "i64").
/// @param ok Optional pointer that receives true when parsing succeeds and false on failure.
/// @return Matching @ref il::core::Type on success or default constructed when unsupported.
/// @note When @p ok is null the caller is opting out of explicit success signalling; failure is
///       observable via the returned default-constructed Type.
il::core::Type parseType(const std::string &token, bool *ok) {
    /// Construct a recognized type and update the optional success flag.
    auto makeType = [ok](il::core::Type::Kind kind) {
        if (ok)
            *ok = true;
        return il::core::Type(kind);
    };

    if (token == "i16")
        return makeType(il::core::Type::Kind::I16);
    if (token == "i32")
        return makeType(il::core::Type::Kind::I32);
    if (token == "i64")
        return makeType(il::core::Type::Kind::I64);
    if (token == "i1")
        return makeType(il::core::Type::Kind::I1);
    if (token == "f64")
        return makeType(il::core::Type::Kind::F64);
    if (token == "ptr")
        return makeType(il::core::Type::Kind::Ptr);
    if (token == "str")
        return makeType(il::core::Type::Kind::Str);
    if (token == "error" || token == "Error")
        return makeType(il::core::Type::Kind::Error);
    if (token == "resume_tok" || token == "ResumeTok")
        return makeType(il::core::Type::Kind::ResumeTok);
    if (token == "void")
        return makeType(il::core::Type::Kind::Void);

    if (ok)
        *ok = false;
    return il::core::Type();
}

} // namespace il::io
