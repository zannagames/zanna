//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Token.hpp
/// @brief Token kinds and token structure for the Zia lexer.
///
/// This header defines the complete set of token types recognized by the
/// Zia lexer, along with the Token structure that carries lexical
/// information through the parsing pipeline.
///
/// ## Token Categories
///
/// Tokens are organized into logical groups:
///
/// 1. **Special Tokens**: End-of-file marker and error recovery tokens
/// 2. **Literals**: Numeric constants, strings, and identifiers
/// 3. **String Interpolation**: Tokens for `"text ${expr} more"` syntax
/// 4. **Keywords**: Reserved words organized by purpose:
///    - Type definitions (struct, class, interface)
///    - Modifiers (final, expose, hide, override, weak)
///    - Declarations (module, bind, func, return, var, new)
///    - Control flow (if, else, match, while, for, guard, etc.)
///    - Inheritance (extends, implements, self, super)
///    - Literal keywords (true, false, null)
/// 5. **Operators**: Arithmetic, comparison, logical, and special operators
/// 6. **Brackets**: Parentheses, square brackets, and curly braces
///
/// ## Token Lifetime
///
/// Tokens are struct types that own their string data. When a Token is copied,
/// the string content is also copied. Tokens are typically produced by the
/// Lexer and consumed by the Parser in a streaming fashion.
///
/// ## String Interpolation
///
/// String interpolation uses three special tokens to handle embedded expressions:
/// - `StringStart`: The opening `"text${` portion
/// - `StringMid`: Middle `}text${` portions between expressions
/// - `StringEnd`: The closing `}text"` portion
///
/// For example, `"Hello ${name}!"` produces:
/// 1. StringStart with text "Hello "
/// 2. Identifier "name"
/// 3. StringEnd with text "!"
///
/// @invariant Each token has a valid TokenKind and SourceLoc.
/// @invariant Literal tokens (IntegerLiteral, NumberLiteral, StringLiteral)
///            have their corresponding value fields populated.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "support/diagnostics.hpp"
#include <cstdint>
#include <string>

namespace il::frontends::zia {

//=============================================================================
/// @brief Enumeration of all token kinds recognized by the Zia lexer.
///
/// Token kinds are categorized for organizational purposes. The categories are:
/// - Special tokens (Eof, Error)
/// - Literals (integers, floats, strings, identifiers)
/// - String interpolation tokens
/// - Keywords (grouped by function)
/// - Operators (arithmetic, comparison, logical, etc.)
/// - Brackets (parentheses, square brackets, braces)
///
/// @note The order of enumerators is significant for documentation but not
///       for functionality. Token comparison uses the enum values directly.
//=============================================================================
enum class TokenKind {
    //=========================================================================
    /// @name Special Tokens
    /// @brief Tokens that indicate lexer state rather than source content.
    /// @{
    //=========================================================================

    /// @brief End of file marker.
    /// @details Returned by the lexer when all input has been consumed.
    /// The parser uses this to know when to stop requesting tokens.
    Eof,

    /// @brief Error token for unrecognized input.
    /// @details Produced when the lexer encounters invalid characters or
    /// malformed literals. The text field contains the problematic input.
    /// Error recovery continues from the next character.
    Error,

    /// @}

    //=========================================================================
    /// @name Literal Tokens
    /// @brief Tokens representing constant values and names.
    /// @{
    //=========================================================================

    /// @brief Integer literal in decimal, hexadecimal, or binary notation.
    /// @details Examples: `42`, `0xFF`, `0b1010`, `-17`
    /// The intValue field contains the parsed numeric value.
    /// Supports underscore separators: `1_000_000`
    IntegerLiteral,

    /// @brief Floating-point literal with optional exponent.
    /// @details Examples: `3.14`, `6.02e23`, `.5`, `1.`
    /// The floatValue field contains the parsed numeric value.
    /// Uses IEEE 754 double-precision representation.
    NumberLiteral,

    /// @brief String literal enclosed in double quotes.
    /// @details Examples: `"hello"`, `"line1\nline2"`, `""`
    /// The stringValue field contains the unescaped content.
    /// Supports escape sequences: \n, \t, \r, \\, \", \0, \xNN
    StringLiteral,

    /// @brief User-defined identifier for variables, functions, types.
    /// @details Must start with a letter or underscore, followed by
    /// letters, digits, or underscores. Case-sensitive.
    /// Examples: `foo`, `_private`, `MyClass`, `x2`
    Identifier,

    /// @}

    //=========================================================================
    /// @name String Interpolation Tokens
    /// @brief Tokens for string literals containing embedded expressions.
    ///
    /// These tokens enable the `"text ${expression} more"` syntax where
    /// expressions can be embedded within string literals. The lexer tracks
    /// interpolation depth to handle nested strings correctly.
    /// @{
    //=========================================================================

    /// @brief Start of an interpolated string: `"text${`
    /// @details The stringValue contains the text before the first `${`.
    /// After producing this token, the lexer switches to expression mode.
    StringStart,

    /// @brief Middle part of an interpolated string: `}text${`
    /// @details Produced after an interpolated expression ends with `}`
    /// and before the next `${`. The stringValue contains the text between.
    StringMid,

    /// @brief End of an interpolated string: `}text"`
    /// @details The stringValue contains the text after the last expression.
    /// After producing this token, the lexer returns to normal mode.
    StringEnd,

    /// @}

    //=========================================================================
    /// @name Type Definition Keywords
    /// @brief Keywords for declaring new types.
    /// @{
    //=========================================================================

    /// @brief Struct type declaration keyword.
    /// @details Introduces a stack-allocated type with value semantics.
    /// Syntax: `struct Point { x: Integer; y: Integer; }`
    KwStruct,

    /// @brief Class type declaration keyword.
    /// @details Introduces a heap-allocated reference type with identity.
    /// Syntax: `class Player { var name: String; func move() { ... } }`
    KwClass,

    /// @brief Enum type declaration keyword.
    /// @details Introduces a set of named integer constants.
    /// Syntax: `enum Color { Red, Green, Blue = 10, Alpha }`
    KwEnum,

    /// @brief Interface declaration keyword.
    /// @details Introduces a contract that classes can implement.
    /// Syntax: `interface Drawable { func draw(); }`
    KwInterface,

    /// @brief Type alias declaration keyword.
    /// @details Introduces a type alias: `type UserId = String;`
    KwType,

    /// @}

    //=========================================================================
    /// @name Modifier Keywords
    /// @brief Keywords that modify declarations.
    /// @{
    //=========================================================================

    /// @brief Immutability modifier for variables.
    /// @details Indicates that a variable cannot be reassigned after initialization.
    /// Syntax: `final x = 42;`
    /// Alias for statements/globals: `let x = 42;`
    KwFinal,

    /// @brief Visibility modifier for public access.
    /// @details Makes a member accessible outside its defining type.
    /// Syntax: `expose func publicMethod() { ... }`
    /// Aliases: `export`, `public`
    KwExpose,

    /// @brief Visibility modifier for private access.
    /// @details Restricts a member to its defining type only.
    /// Syntax: `hide var privateField: Integer;`
    /// Alias: `private`
    KwHide,

    /// @brief Foreign function import declaration.
    /// @details Declares a function defined in another module (no body).
    /// Syntax: `foreign func helper(n: Integer) -> Integer`
    KwForeign,

    /// @brief Method override indicator.
    /// @details Indicates that a method overrides a parent class method.
    /// Syntax: `override func toString() -> String { ... }`
    KwOverride,

    /// @brief Destructor declaration keyword.
    /// @details Declares a destructor for class cleanup.
    /// Syntax: `deinit { cleanup code }`
    KwDeinit,

    /// @brief Property declaration keyword.
    /// @details Declares a computed property with getter and optional setter.
    /// Syntax: `property name: Type { get { ... } set(value) { ... } }`
    KwProperty,

    /// @brief Static member modifier.
    /// @details Declares a field or method as belonging to the type, not instances.
    /// Syntax: `static count: Integer = 0` or `static func create() -> Self`
    KwStatic,

    /// @brief Weak reference modifier.
    /// @details Creates a reference that doesn't prevent garbage collection.
    /// Syntax: `weak var parent: Node?;`
    KwWeak,

    /// @brief Async function modifier.
    /// @details Marks a function as asynchronous, returning a Future.
    /// Syntax: `async func fetchData(url: String) -> String { ... }`
    KwAsync,

    /// @brief Await expression keyword.
    /// @details Suspends execution until a Future resolves to a value.
    /// Syntax: `let result = await someFuture`
    KwAwait,

    /// @}

    //=========================================================================
    /// @name Declaration Keywords
    /// @brief Keywords for introducing named declarations.
    /// @{
    //=========================================================================

    /// @brief Module declaration keyword.
    /// @details Declares the module name at the start of a source file.
    /// Syntax: `module MyApp;`
    KwModule,

    /// @brief Namespace block keyword.
    /// @details Groups declarations under a namespace for qualified access.
    /// Syntax: `namespace MyLib { class Foo { ... } }`
    /// Access via: `MyLib.Foo`
    KwNamespace,

    /// @brief Bind statement keyword.
    /// @details Binds a namespace to an alias for use in the current module.
    /// Syntax: `bind Zanna.Terminal as Term;`
    KwBind,

    /// @brief Function declaration keyword.
    /// @details Introduces a function or method definition.
    /// Syntax: `func add(a: Integer, b: Integer) -> Integer { return a + b; }`
    KwFunc,

    /// @brief Return statement keyword.
    /// @details Returns a value from a function or exits early.
    /// Syntax: `return result;` or `return;`
    KwReturn,

    /// @brief Variable declaration keyword.
    /// @details Introduces a mutable variable binding.
    /// Syntax: `var x = 42;` or `var x: Integer;`
    KwVar,

    /// @brief Object instantiation keyword.
    /// @details Creates a new instance of a class type.
    /// Syntax: `new Player("Alice")`
    KwNew,

    /// @}

    //=========================================================================
    /// @name Control Flow Keywords
    /// @brief Keywords for branching, looping, and pattern matching.
    /// @{
    //=========================================================================

    /// @brief Conditional branch keyword.
    /// @details Executes code based on a boolean condition.
    /// Syntax: `if condition { ... }`
    KwIf,

    /// @brief Alternative branch keyword.
    /// @details Provides an alternative path when the if condition is false.
    /// Syntax: `if cond { ... } else { ... }`
    KwElse,

    /// @brief Pattern binding keyword.
    /// @details Introduces an immutable binding with pattern matching.
    /// Syntax: `let x = getValue();` or `let (a, b) = getPair();`
    KwLet,

    /// @brief Pattern matching statement keyword.
    /// @details Matches a value against multiple patterns.
    /// Syntax: `match value { 0 => "zero"; _ => "other"; }`
    KwMatch,

    /// @brief While loop keyword.
    /// @details Repeats code while a condition is true.
    /// Syntax: `while condition { ... }`
    KwWhile,

    /// @brief For loop keyword.
    /// @details Iterates over a range or collection.
    /// Syntax: `for i in 0..10 { ... }` or `for item in list { ... }`
    KwFor,

    /// @brief Collection iteration keyword.
    /// @details Used with for loops to specify the source collection.
    /// Syntax: `for x in collection { ... }`
    KwIn,

    /// @brief Type checking keyword.
    /// @details Tests if a value is of a specific type.
    /// Syntax: `if value is String { ... }`
    KwIs,

    /// @brief Guard statement keyword.
    /// @details Early exit if a condition is not met.
    /// Syntax: `guard condition else { return; }`
    KwGuard,

    /// @brief Loop break keyword.
    /// @details Exits the innermost enclosing loop immediately.
    /// Syntax: `break;`
    KwBreak,

    /// @brief Loop continue keyword.
    /// @details Skips to the next iteration of the innermost loop.
    /// Syntax: `continue;`
    KwContinue,

    /// @brief Scope cleanup keyword.
    /// @details Registers a cleanup action that runs at scope exit.
    /// Syntax: `defer cleanup();` or `defer { cleanup(); }`
    KwDefer,

    /// @brief Try block keyword.
    /// @details Begins an exception handling block.
    /// Syntax: `try { ... } catch(e) { ... } finally { ... }`
    KwTry,

    /// @brief Catch block keyword.
    /// @details Handles exceptions from the preceding try block.
    /// Syntax: `catch(e) { ... }`
    KwCatch,

    /// @brief Finally block keyword.
    /// @details Code that always executes after try/catch.
    /// Syntax: `finally { ... }`
    KwFinally,

    /// @brief Throw expression keyword.
    /// @details Raises an exception.
    /// Syntax: `throw expr;`
    KwThrow,

    /// @}

    //=========================================================================
    /// @name Inheritance Keywords
    /// @brief Keywords for object-oriented programming features.
    /// @{
    //=========================================================================

    /// @brief Base class specification keyword.
    /// @details Indicates that a class inherits from another.
    /// Syntax: `class Child extends Parent { ... }`
    KwExtends,

    /// @brief Interface implementation keyword.
    /// @details Indicates that a class implements an interface.
    /// Syntax: `class Shape implements Drawable { ... }`
    KwImplements,

    /// @brief Self-reference keyword.
    /// @details References the current object instance within a method.
    /// Syntax: `self.field = value;`
    KwSelf,

    /// @brief Parent class reference keyword.
    /// @details Calls methods or accesses members of the parent class.
    /// Syntax: `super.init();`
    KwSuper,

    /// @brief Type cast keyword.
    /// @details Converts a value to a different type.
    /// Syntax: `value as String`
    KwAs,

    /// @}

    //=========================================================================
    /// @name Literal Value Keywords
    /// @brief Keywords representing built-in constant values.
    /// @{
    //=========================================================================

    /// @brief Boolean true literal.
    KwTrue,

    /// @brief Boolean false literal.
    KwFalse,

    /// @brief Null reference literal.
    /// @details Represents the absence of a value for optional types.
    KwNull,

    /// @}

    //=========================================================================
    /// @name Boolean Operator Keywords
    /// @brief Word-form boolean operators (alternative to &&, ||, !).
    /// @{
    //=========================================================================

    /// @brief Logical AND keyword.
    /// @details Alternative to `&&` operator.
    /// Syntax: `if a and b { ... }`
    KwAnd,

    /// @brief Logical OR keyword.
    /// @details Alternative to `||` operator.
    /// Syntax: `if a or b { ... }`
    KwOr,

    /// @brief Logical NOT keyword.
    /// @details Alternative to `!` operator.
    /// Syntax: `if not condition { ... }`
    KwNot,

    /// @}

    //=========================================================================
    /// @name Arithmetic Operators
    /// @brief Operators for mathematical computations.
    /// @{
    //=========================================================================

    /// @brief Addition operator `+`.
    /// @details Also used for string concatenation.
    Plus,

    /// @brief Subtraction operator `-`.
    /// @details Also used as unary negation.
    Minus,

    /// @brief Multiplication operator `*`.
    Star,

    /// @brief Division operator `/`.
    Slash,

    /// @brief Modulo (remainder) operator `%`.
    Percent,

    /// @brief Compound addition assignment `+=`.
    PlusEqual,

    /// @brief Compound subtraction assignment `-=`.
    MinusEqual,

    /// @brief Compound multiplication assignment `*=`.
    StarEqual,

    /// @brief Compound division assignment `/=`.
    SlashEqual,

    /// @brief Compound modulo assignment `%=`.
    PercentEqual,

    /// @brief Compound left shift assignment `<<=`.
    ShiftLeftEqual,

    /// @brief Compound right shift assignment `>>=`.
    ShiftRightEqual,

    /// @brief Compound bitwise AND assignment `&=`.
    AmpersandEqual,

    /// @brief Compound bitwise OR assignment `|=`.
    PipeEqual,

    /// @brief Compound bitwise XOR assignment `^=`.
    CaretEqual,

    /// @}

    //=========================================================================
    /// @name Bitwise Operators
    /// @brief Operators for bit-level manipulation.
    /// @{
    //=========================================================================

    /// @brief Bitwise AND operator `&`.
    Ampersand,

    /// @brief Bitwise OR operator `|`.
    Pipe,

    /// @brief Bitwise XOR operator `^`.
    Caret,

    /// @brief Bitwise NOT operator `~`.
    Tilde,

    /// @brief Left shift operator `<<`.
    ShiftLeft,

    /// @brief Right shift operator `>>`.
    ShiftRight,

    /// @}

    //=========================================================================
    /// @name Logical and Comparison Operators
    /// @brief Operators for boolean logic and value comparison.
    /// @{
    //=========================================================================

    /// @brief Logical NOT operator `!`.
    Bang,

    /// @brief Assignment operator `=`.
    Equal,

    /// @brief Equality comparison operator `==`.
    EqualEqual,

    /// @brief Inequality comparison operator `!=`.
    NotEqual,

    /// @brief Less-than comparison operator `<`.
    Less,

    /// @brief Less-than-or-equal comparison operator `<=`.
    LessEqual,

    /// @brief Greater-than comparison operator `>`.
    Greater,

    /// @brief Greater-than-or-equal comparison operator `>=`.
    GreaterEqual,

    /// @brief Logical AND operator `&&`.
    /// @details Short-circuits: right operand not evaluated if left is false.
    AmpAmp,

    /// @brief Logical OR operator `||`.
    /// @details Short-circuits: right operand not evaluated if left is true.
    PipePipe,

    /// @}

    //=========================================================================
    /// @name Special Operators
    /// @brief Operators with special syntactic meaning.
    /// @{
    //=========================================================================

    /// @brief Ellipsis `...` for variadic parameters.
    /// @details Marks the last parameter as accepting zero or more arguments.
    /// Syntax: `func sum(nums: ...Integer) -> Integer`
    Ellipsis,

    /// @brief Return type arrow `->`.
    /// @details Separates function parameters from return type.
    /// Syntax: `func add(a: Int, b: Int) -> Int`
    Arrow,

    /// @brief Lambda arrow `=>`.
    /// @details Separates lambda parameters from body.
    /// Syntax: `(x: Integer) => x + 1`
    FatArrow,

    /// @brief Optional/try operator `?`.
    /// @details Propagates null or error values.
    /// Syntax: `value?` or `func()?`
    Question,

    /// @brief Null coalescing operator `??`.
    /// @details Provides a default value if left operand is null.
    /// Syntax: `value ?? defaultValue`
    QuestionQuestion,

    /// @brief Optional chaining operator `?.`.
    /// @details Safe member access that returns null if base is null.
    /// Syntax: `obj?.field`
    QuestionDot,

    /// @brief Member access operator `.`.
    /// @details Accesses fields and methods of objects.
    /// Syntax: `obj.field` or `obj.method()`
    Dot,

    /// @brief Range operator `..`.
    /// @details Creates a half-open range [start, end).
    /// Syntax: `0..10`
    DotDot,

    /// @brief Inclusive range operator `..=`.
    /// @details Creates a closed range [start, end].
    /// Syntax: `0..=10`
    DotDotEqual,

    /// @brief Type annotation separator `:`.
    /// @details Separates name from type in declarations.
    /// Syntax: `var x: Integer`
    Colon,

    /// @brief Statement terminator `;`.
    /// @details Ends statements and declarations.
    Semicolon,

    /// @brief Argument/element separator `,`.
    /// @details Separates items in lists, parameters, and arguments.
    Comma,

    /// @brief Attribute marker `@`.
    /// @details Introduces an attribute or decorator.
    /// Syntax: `@deprecated func old() { ... }`
    At,

    /// @}

    //=========================================================================
    /// @name Bracket Tokens
    /// @brief Paired delimiters for grouping and collections.
    /// @{
    //=========================================================================

    /// @brief Left parenthesis `(`.
    /// @details Groups expressions, encloses function parameters.
    LParen,

    /// @brief Right parenthesis `)`.
    RParen,

    /// @brief Left square bracket `[`.
    /// @details Introduces array literals, array indexing, generic params.
    LBracket,

    /// @brief Right square bracket `]`.
    RBracket,

    /// @brief Left curly brace `{`.
    /// @details Introduces blocks, type bodies, map/set literals.
    LBrace,

    /// @brief Right curly brace `}`.
    RBrace,

    /// @}
};

//=============================================================================
/// @brief Convert a TokenKind to its string representation for debugging.
///
/// Returns a human-readable name for the token kind, useful for error
/// messages and debugging output.
///
/// @param kind The token kind to convert.
/// @return A null-terminated string naming the token kind.
///
/// @par Example:
/// @code
/// TokenKind k = TokenKind::Plus;
/// printf("Token: %s\n", tokenKindToString(k)); // Prints "Token: +"
/// @endcode
//=============================================================================
const char *tokenKindToString(TokenKind kind);

//=============================================================================
/// @brief Token structure holding lexical information from the source.
///
/// A Token represents a single lexical unit from the source code. It carries:
/// - The token kind (what type of token this is)
/// - The source location (file, line, column)
/// - The original source text
/// - Parsed literal values (for numeric and string literals)
///
/// ## Memory Management
///
/// Tokens own their string data (text and stringValue fields). Copying a
/// Token also copies these strings. For performance-critical code, consider
/// moving tokens rather than copying.
///
/// ## Literal Value Fields
///
/// Only one of the literal value fields is meaningful for any given token:
/// - `intValue` for IntegerLiteral tokens
/// - `floatValue` for NumberLiteral tokens
/// - `stringValue` for StringLiteral, StringStart, StringMid, StringEnd
///
/// ## Usage Example
///
/// @code
/// Token tok = lexer.next();
/// if (tok.is(TokenKind::IntegerLiteral)) {
///     int64_t value = tok.intValue;
///     // Use the integer value...
/// }
/// @endcode
//=============================================================================
struct Token {
    /// @brief The kind of token this represents.
    /// @details Defaults to Eof, indicating no token has been read.
    TokenKind kind = TokenKind::Eof;

    /// @brief Source location where this token appears.
    /// @details Contains file ID, line number (1-based), and column (1-based).
    il::support::SourceLoc loc{};

    /// @brief Original source text of the token.
    /// @details For most tokens, this is the exact characters from the source.
    /// For string literals, this includes the quotes and escape sequences.
    std::string text;

    /// @brief Parsed integer value for IntegerLiteral tokens.
    /// @details Contains the numeric value after parsing hex, binary, or
    /// decimal notation. Valid only when kind == IntegerLiteral.
    int64_t intValue = 0;

    /// @brief True if this integer literal requires negation to be valid.
    /// @details Set when the literal is exactly 9223372036854775808, which
    /// overflows int64_t but becomes valid INT64_MIN when negated.
    /// Used by the parser to handle `-9223372036854775808`.
    bool requiresNegation = false;

    /// @brief Parsed floating-point value for NumberLiteral tokens.
    /// @details Contains the numeric value after parsing. Uses IEEE 754
    /// double-precision representation. Valid only when kind == NumberLiteral.
    double floatValue = 0.0;

    /// @brief Unescaped string content for string literal tokens.
    /// @details Contains the string value after processing escape sequences.
    /// Valid for StringLiteral, StringStart, StringMid, and StringEnd tokens.
    /// Does not include the surrounding quotes.
    std::string stringValue;

    //=========================================================================
    /// @brief Check if this token is of a specific kind.
    ///
    /// @param k The token kind to check against.
    /// @return true if this token's kind matches k, false otherwise.
    ///
    /// @par Example:
    /// @code
    /// if (token.is(TokenKind::Semicolon)) {
    ///     // Handle end of statement
    /// }
    /// @endcode
    //=========================================================================
    bool is(TokenKind k) const {
        return kind == k;
    }

    //=========================================================================
    /// @brief Check if this token is one of several kinds.
    ///
    /// Uses fold expressions to efficiently check multiple kinds at once.
    /// Short-circuits on the first match.
    ///
    /// @tparam Kinds Variadic list of TokenKind values to check.
    /// @param kinds The token kinds to check against.
    /// @return true if this token matches any of the given kinds.
    ///
    /// @par Example:
    /// @code
    /// if (token.isOneOf(TokenKind::Plus, TokenKind::Minus)) {
    ///     // Handle additive operator
    /// }
    /// @endcode
    //=========================================================================
    template <typename... Kinds> bool isOneOf(Kinds... kinds) const {
        return (is(kinds) || ...);
    }

    //=========================================================================
    /// @brief Check if this token is any keyword.
    ///
    /// Keywords are reserved words that have special meaning in the language.
    /// This includes type keywords (struct, class), control flow keywords
    /// (if, while, for), and literal keywords (true, false, null).
    ///
    /// @return true if this token is a keyword, false otherwise.
    ///
    /// @par Example:
    /// @code
    /// if (token.isKeyword()) {
    ///     error("Cannot use keyword as identifier");
    /// }
    /// @endcode
    //=========================================================================
    bool isKeyword() const;
};

} // namespace il::frontends::zia
