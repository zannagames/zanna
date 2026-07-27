---
status: active
audience: developers
last-verified: 2026-07-26
---

# How to Write a Zanna Frontend

Complete implementation guide for building language frontends that compile to Zanna IL. Designed for C++ programmers
with no prior Zanna experience.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Quick Start: Minimal Frontend](#2-quick-start-minimal-frontend)
3. [Build System Integration](#3-build-system-integration)
4. [High-Level Architecture](#4-high-level-architecture)
5. [Parser Design](#5-parser-design)
6. [AST Design](#6-ast-design)
7. [Semantic Analysis](#7-semantic-analysis)
8. [IL Lowering with IRBuilder](#8-il-lowering-with-irbuilder)
9. [Type System Integration](#9-type-system-integration)
10. [Runtime ABI Reference](#10-runtime-abi-reference)
11. [Diagnostics and Error Reporting](#11-diagnostics-and-error-reporting)
12. [Value and Symbol Management](#12-value-and-symbol-management)
13. [Special Features](#13-special-features)
14. [Testing Strategy](#14-testing-strategy)
15. [Common Pitfalls and Debugging](#15-common-pitfalls-and-debugging)
16. [Reference Materials](#16-reference-materials)

---

## 1. Introduction

### What You'll Learn

This guide teaches you how to implement a complete language frontend for Zanna:

- **Parsing**: Convert source code into an Abstract Syntax Tree (AST)
- **Semantic Analysis**: Type checking, name resolution, and validation
- **IL Lowering**: Transform AST to Zanna IL using the IRBuilder API
- **Build Integration**: Add your frontend to the CMake build system
- **Testing**: Write comprehensive tests for your frontend

### What is Zanna?

Zanna is a compiler infrastructure with multiple components:

| Component    | Description                                                |
|--------------|------------------------------------------------------------|
| **IL**       | SSA-based intermediate representation (similar to LLVM IR) |
| **VM**       | Virtual machine for rapid development and testing          |
| **Runtime**  | C-based runtime library (strings, arrays, I/O, GC)         |
| **Verifier** | Static analysis and type checking for IL                   |
| **Codegen**  | Native code generation (x86-64 and AArch64)                |

**Compilation flow:**

```text
Source Language → Frontend → IL → VM/Codegen → Execution
```

Zanna currently includes two frontends: **Zia** and **BASIC**. Both compile to the same IL and share the
runtime library.

### Prerequisites

**Required Knowledge:**

- **Modern C++**: C++20 features (`std::optional`, structured bindings; concepts/ranges helpful)
- **Compiler Basics**: Lexing, parsing, AST construction
- **SSA Form**: Basic understanding of phi nodes and basic blocks

**Required Tools:**

- CMake 3.20 or later
- C++20-capable compiler (Clang or GCC recommended)
- Git

**Recommended Reading:**

- **[IL Guide](../il/il-guide.md)** — Zanna IL specification and examples
- **[Getting Started](../getting-started.md)** — Build and run Zanna
- **[Architecture](architecture.md)** — System architecture overview

---

## 2. Quick Start: Minimal Frontend

Let's build a minimal frontend that compiles `PRINT 42` to executable IL. This demonstrates the complete pipeline in ~
150 lines.

### Understanding the Pipeline

Before diving into code, understand what a frontend must accomplish:

1. **Lexical Analysis (Lexer)**: Convert raw source text into a stream of tokens. For example, `"PRINT 42"` becomes
   tokens `[PRINT, NUMBER(42)]`. The lexer handles whitespace, keywords, and basic syntax validation.

2. **Syntax Analysis (Parser)**: Build an Abstract Syntax Tree (AST) from the token stream. The AST represents the
   program's structure in memory. For our example, we'd create a `PrintStmt` node containing the value `42`.

3. **Semantic Analysis**: Validate the AST (type checking, name resolution). Our minimal example skips this to focus on
   the core pipeline, but real frontends need this phase.

4. **IL Lowering**: Convert the AST into Zanna IL using the IRBuilder API. This is where you emit SSA instructions,
   manage basic blocks, and call runtime functions. The IL is a platform-independent representation that can be
   interpreted by the VM or compiled to native code.

The minimal frontend below implements steps 1, 2, and 4 in a single file, demonstrating the complete flow from source
text to executable IL.

### File: minimal_frontend.cpp

```cpp
#include "zanna/il/IRBuilder.hpp"
#include "zanna/il/Module.hpp"
#include "zanna/il/IO.hpp"
#include "support/source_manager.hpp"
#include <iostream>
#include <string>

using namespace il::core;
using namespace il::build;

// ============================================================================
// 1. TOKEN TYPES
// ============================================================================

enum class TokenKind { Number, Print, Eof };

struct Token {
    TokenKind kind;
    std::string text;
    int64_t value{0};
};

// ============================================================================
// 2. LEXER (Tokenization)
// ============================================================================
//
// The lexer's job is to convert raw source text into a stream of tokens.
// It's a simple state machine that walks through the source string character
// by character, recognizing patterns (keywords, numbers, operators).
//
// Key design decisions:
// - Single-character lookahead via pos_ index (simple but effective)
// - Eager token construction (tokens are fully formed before returning)
// - Whitespace is skipped automatically between tokens
// - Keywords are recognized by exact string matching (for simplicity)
//
// In a production frontend, you'd typically add:
// - Line/column tracking for error messages
// - A keyword table for efficient keyword recognition
// - String literal support with escape sequences
// - Multi-character operators (<=, >=, etc.)
//
class Lexer {
    std::string source_;
    size_t pos_{0};

public:
    explicit Lexer(std::string src) : source_(std::move(src)) {}

    Token next() {
        // Skip whitespace: This normalizes input, so the parser never
        // sees spaces/tabs/newlines between meaningful tokens.
        while (pos_ < source_.size() && isspace(source_[pos_]))
            pos_++;

        // Check for end of input
        if (pos_ >= source_.size())
            return {TokenKind::Eof, "", 0};

        // Check for PRINT keyword: Simple substring match.
        // Production lexers use a keyword table (std::unordered_map)
        // to avoid repeating this pattern for every keyword.
        if (source_.substr(pos_, 5) == "PRINT") {
            pos_ += 5;
            return {TokenKind::Print, "PRINT", 0};
        }

        // Check for number: Consume consecutive digits and convert to i64.
        // This only handles non-negative integers. Production lexers
        // would handle negative signs, floating-point, hex literals, etc.
        if (isdigit(source_[pos_])) {
            size_t start = pos_;
            while (pos_ < source_.size() && isdigit(source_[pos_]))
                pos_++;
            std::string numStr = source_.substr(start, pos_ - start);
            return {TokenKind::Number, numStr, std::stoll(numStr)};
        }

        // Unknown character: In production, you'd emit an error diagnostic
        // here instead of returning Eof.
        return {TokenKind::Eof, "", 0};
    }
};

// ============================================================================
// 3. AST (Abstract Syntax Tree)
// ============================================================================
//
// The AST is an in-memory tree representation of your program's structure.
// Each node represents a language construct (statement, expression, declaration).
//
// Key design principles:
// - Make illegal states unrepresentable (use types to enforce invariants)
// - Keep AST nodes simple and data-focused (no behavior, just structure)
// - Use unique_ptr for ownership, raw pointers for non-owning references
// - Separate statement nodes (no value) from expression nodes (produce values)
//
// This minimal AST has just two node types. Production frontends typically have:
// - Expression hierarchy (BinaryExpr, UnaryExpr, CallExpr, LiteralExpr, etc.)
// - Statement hierarchy (IfStmt, WhileStmt, ForStmt, ReturnStmt, etc.)
// - Declaration nodes (FuncDecl, VarDecl, ClassDecl, etc.)
// - A top-level Program/Module node that owns all declarations
//
struct PrintStmt {
    int64_t value;  // The integer to print
};

struct Program {
    std::vector<PrintStmt> statements;  // Sequence of statements to execute
};

// ============================================================================
// 4. PARSER (Syntax Analysis)
// ============================================================================
//
// The parser's job is to build an AST from the token stream. It enforces
// the grammar rules of your language and detects syntax errors.
//
// This parser uses the "recursive descent" technique with single-token
// lookahead. The current_ token is always the next unconsumed token.
//
// Key patterns:
// - Lookahead: Check current_.kind to decide what to parse next
// - Consume: Call lexer_.next() to advance to the next token
// - Build: Construct AST nodes as you match grammar rules
// - Error recovery: Detect syntax errors and emit diagnostics (not shown here)
//
// Grammar being parsed:
//   Program → Statement*
//   Statement → "PRINT" Number
//
// Production parsers typically use precedence climbing or Pratt parsing
// for expressions, and more sophisticated error recovery strategies.
//
class Parser {
    Lexer lexer_;
    Token current_;  // Lookahead: the next unconsumed token

public:
    explicit Parser(std::string source)
        : lexer_(std::move(source)), current_(lexer_.next()) {}

    Program parseProgram() {
        Program prog;

        // Parse zero or more statements until we hit EOF
        while (current_.kind != TokenKind::Eof) {
            if (current_.kind == TokenKind::Print) {
                // Match "PRINT" token
                current_ = lexer_.next();  // consume PRINT

                // Expect a number after PRINT
                if (current_.kind == TokenKind::Number) {
                    // Build PrintStmt node and add to program
                    prog.statements.push_back({current_.value});
                    current_ = lexer_.next();  // consume number
                }
                // Production code would emit an error diagnostic here
                // if the next token isn't a number
            } else {
                // Unknown token: skip it (production code would error)
                current_ = lexer_.next();
            }
        }
        return prog;
    }
};

// ============================================================================
// 5. LOWERER (AST → IL Translation)
// ============================================================================
//
// The lowerer (also called "IL generator" or "backend") converts your AST
// into Zanna IL using the IRBuilder API. This is where your language semantics
// are translated into low-level SSA instructions.
//
// Key concepts:
//
// 1. **Module**: Container for all IL (functions, globals, externs).
//    Think of it like a compilation unit in C.
//
// 2. **IRBuilder**: Fluent API for constructing IL. It handles SSA temporary
//    allocation, instruction emission, and basic block management.
//
// 3. **Function**: Top-level IL construct. Has a name, signature (params/return),
//    and a sequence of basic blocks. Every executable IL module needs at least
//    one function (typically @main).
//
// 4. **BasicBlock**: A sequence of instructions with a single entry point and
//    a terminator (ret, br, cbr, switch). Control flow only happens via
//    terminators at the end of blocks.
//
// 5. **SSA (Static Single Assignment)**: Every value is defined exactly once.
//    Instead of mutable variables, we create new temporaries (%t0, %t1, %t2, ...).
//    The IRBuilder automatically allocates these for you.
//
// 6. **Insert Point**: The IRBuilder needs to know WHERE to emit instructions.
//    You set the current block via setInsertPoint(), and all subsequent
//    emitXxx() calls append to that block.
//
// 7. **Runtime ABI**: Many language features (string ops, array ops, I/O) are
//    implemented as C functions in the runtime library. You declare them as
//    "extern" and call them like regular functions. The VM automatically
//    loads these via FFI.
//
// This minimal lowerer demonstrates:
// - Declaring external runtime functions (addExtern)
// - Creating a new function (startFunction)
// - Managing basic blocks (addBlock, setInsertPoint)
// - Emitting function calls (emitCall)
// - Returning from functions (emitRet)
//
class Lowerer {
    Module &mod_;       // The IL module we're building
    IRBuilder builder_; // API for emitting IL instructions

public:
    explicit Lowerer(Module &m) : mod_(m), builder_(m) {}

    void lower(const Program &prog) {
        // Step 1: Declare external runtime functions
        //
        // Runtime functions are implemented in C (src/runtime/) and linked
        // via FFI at execution time. We declare them as "extern" so the IL
        // knows their signature, but we don't provide implementation.
        //
        // Canonical IL extern name: Zanna.Terminal.PrintI64
        // Backed by C function rt_print_i64; legacy `rt_print_i64` extern
        // remains accepted while ZANNA_RUNTIME_NS_DUAL=ON (the current default).
        //
        builder_.addExtern("Zanna.Terminal.PrintI64",
                          Type(Type::Kind::Void),    // return type
                          {Type(Type::Kind::I64)});  // parameter types

        // Step 2: Create the @main function
        //
        // Every IL program needs an entry point. By convention, it's called
        // @main and returns i64 (exit code). The VM will call @main when
        // executing the program.
        //
        // Signature: i64 @main()
        //
        Function &mainFn = builder_.startFunction(
            "main",                    // function name
            Type(Type::Kind::I64),     // return type
            {}                         // parameters (empty for main)
        );

        // Step 3: Create the entry basic block
        //
        // Every function needs at least one basic block. The first block is
        // the entry point (where execution begins). We name it "entry" by
        // convention, but the name is just a label for readability.
        //
        BasicBlock &entry = builder_.addBlock(mainFn, "entry");

        // Step 4: Set insert point
        //
        // Tell the IRBuilder to emit instructions into the "entry" block.
        // All subsequent emitXxx() calls will append to this block until
        // we call setInsertPoint() again with a different block.
        //
        builder_.setInsertPoint(entry);

        // Step 5: Emit PRINT statements
        //
        // For each PrintStmt in the AST, emit a call to rt_print_i64.
        // This demonstrates:
        // - Creating constant values (Value::constInt)
        // - Emitting void function calls (std::nullopt for no return value)
        // - Source location tracking (empty SourceLoc for simplicity)
        //
        for (const auto &stmt : prog.statements) {
            builder_.emitCall(
                "rt_print_i64",                // callee name
                {Value::constInt(stmt.value)}, // arguments
                std::nullopt,                  // destination (void = no return)
                il::support::SourceLoc{}       // source location (for diagnostics)
            );
        }

        // Step 6: Return from main
        //
        // Every basic block MUST end with a terminator (ret, br, cbr, switch).
        // Here we return the constant 0 (success exit code) from @main.
        //
        // emitRet() is a terminator, so no more instructions can be added to
        // this block after this point. If you need to emit more code, create
        // a new basic block and set it as the insert point.
        //
        builder_.emitRet(Value::constInt(0), il::support::SourceLoc{});
    }
};

// ============================================================================
// 6. MAIN DRIVER (Putting It All Together)
// ============================================================================
//
// The main driver orchestrates the entire compilation pipeline. It:
// 1. Reads source code from command line
// 2. Runs the parser (which internally uses the lexer)
// 3. Runs the lowerer to generate IL
// 4. Serializes the IL to stdout
//
// In production frontends, you'd also:
// - Read from files instead of command-line arguments
// - Run semantic analysis between parsing and lowering
// - Handle diagnostics (errors/warnings) gracefully
// - Support multiple output modes (IL text, IL binary, direct VM execution)
//
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: minimal_frontend <source>\n";
        return 1;
    }

    std::string source = argv[1];

    // PHASE 1: Parse source → AST
    //
    // The parser internally uses the lexer to get tokens. We get back
    // a fully-formed AST representing the program structure.
    //
    Parser parser(source);
    Program prog = parser.parseProgram();

    // PHASE 2: Lower AST → IL
    //
    // The lowerer walks the AST and emits IL instructions via IRBuilder.
    // The Module object accumulates all generated IL (functions, globals, etc.).
    //
    Module mod;
    Lowerer lowerer(mod);
    lowerer.lower(prog);

    // PHASE 3: Serialize IL to stdout
    //
    // The Serializer converts the in-memory IL Module to textual IL format.
    // This is the same format you see in *.il test files.
    //
    // You could also:
    // - Write to a file: Serializer::write(mod, outFile)
    // - Execute directly in VM: il::vm::Runner runner(mod, cfg); runner.run()
    // - Verify IL: il::verify::Verifier::verify(mod)
    //
    il::io::Serializer::write(mod, std::cout);

    return 0;
}
```

### Build and Run

```bash
# Add to CMakeLists.txt:
add_executable(minimal_frontend minimal_frontend.cpp)
target_link_libraries(minimal_frontend PRIVATE zanna_il_full il_build)

# Build
cmake --build build

# Run
./build/minimal_frontend "PRINT 42"
```

**Output (IL text format):**

```il
il 0.3.0

extern @Zanna.Terminal.PrintI64(i64) -> void

func @main() -> i64 {
entry:
  call @Zanna.Terminal.PrintI64(42)
  ret 0
}
```

**Understanding the IL output:**

- `il 0.3.0` — IL version header (required by spec)
- `extern @Zanna.Terminal.PrintI64(i64) -> void` — External function declaration (implemented in C runtime)
- `func @main() -> i64 { ... }` — Function definition with signature
- `entry:` — Basic block label
- `call @Zanna.Terminal.PrintI64(42)` — Function call instruction with constant argument
- `ret 0` — Return terminator (exit code)

> **Runtime namespace compatibility:** Zanna's runtime symbols use the canonical `@Zanna.*` form. When the
> runtime is built with `-DZANNA_RUNTIME_NS_DUAL=ON` (the current default), legacy `@rt_*` aliases such as
> `@rt_print_i64` are also accepted, but new frontends should emit the canonical names.

This IL can be:

- **Verified**: `il-verify output.il` checks for structural correctness
- **Executed**: `zanna -run output.il` runs it in the VM
- **Compiled**: `zanna codegen x64 output.il -S out.s` compiles to native x86-64 assembly

### Key Takeaways

This minimal example demonstrates the complete frontend pipeline:

1. **Lexer** — Converts source text to tokens. Uses simple state machine with position tracking. Production lexers add
   line/column info and keyword tables.

2. **Parser** — Builds AST from tokens using recursive descent. Enforces grammar rules. Production parsers add error
   recovery and precedence climbing for expressions.

3. **AST** — In-memory tree structure representing program semantics. Keep nodes simple and data-focused. Use unique_ptr
   for ownership.

4. **Lowerer** — Translates AST to IL using IRBuilder API. Handles SSA generation, basic block management, and runtime
   function calls. This is where language semantics meet IL instructions.

5. **Module** — Container for all IL (functions, globals, externs). Passed to Serializer for output or VM for execution.

6. **IRBuilder** — Fluent API for emitting IL. Automatically manages SSA temporaries and enforces IL structural rules (
   every block needs terminator, etc.).

7. **Runtime ABI** — Many language features are implemented as C functions (string ops, array ops, I/O). Declare them as
   extern and call them like regular IL functions.

**What we skipped in this minimal example:**

- **Semantic Analysis**: Type checking, name resolution, control flow validation. Real frontends need this phase between
  parsing and lowering.
- **Error Handling**: Diagnostics with source locations, error recovery strategies.
- **Expressions**: Operator precedence, type conversions, function calls.
- **Variables**: SSA slot management (alloca/load/store), scoping.
- **Control Flow**: If/while/for with multi-block CFG construction.

The remaining sections of this guide cover these production frontend concerns in depth.

Now let's explore each component in detail, starting with build system integration.

---

## 3. Build System Integration

Zanna uses CMake for build management. This section explains how to integrate your frontend into the build system so it
can be compiled and used by the `zanna` command-line tool.

### Directory Structure

**Why this organization?** Zanna organizes frontends under `src/frontends/` to keep them separate from the IL core
infrastructure. Each frontend is a self-contained directory with its own CMakeLists.txt.

Create your frontend under `src/frontends/`:

```text
src/frontends/yourfrontend/
├── CMakeLists.txt              # Build configuration (dependencies, sources)
├── Lexer.{cpp,hpp}             # Tokenization
├── Parser.{cpp,hpp}            # Syntax analysis (AST construction)
├── AST.hpp                     # AST node definitions
├── SemanticAnalyzer.{cpp,hpp}  # Type checking and validation
├── Lowerer.{cpp,hpp}           # IL generation (AST → IL)
└── TypeRules.{cpp,hpp}         # Type system and coercion rules
```

**Design rationale:**

- **Header-only AST**: AST nodes are typically simple data structures (no complex behavior), so they live in a single
  header file.
- **Separate compilation units**: Lexer, Parser, SemanticAnalyzer, and Lowerer are split into .cpp/.hpp to speed up
  compilation (large frontends can take minutes to rebuild).
- **TypeRules separation**: Type system logic (coercion, promotion, compatibility) is complex enough to warrant its own
  file.

### CMakeLists.txt

**What this does:** Defines a static library target (`fe_yourfrontend`) that compiles your frontend sources and links
against Zanna IL infrastructure. The library can then be linked into the `zanna` tool or other programs.

**File:** `src/frontends/yourfrontend/CMakeLists.txt`

```cmake
# Create library target
# Static library = compiled once, linked into multiple executables.
# Alternative: SHARED library if you want dynamic linking (rarely needed).
add_library(fe_yourfrontend STATIC)

# Add source files
# Only list .cpp files here; headers are found automatically via #include.
# PRIVATE = these sources are only used to build this library (not exposed to consumers).
target_sources(fe_yourfrontend PRIVATE
    Lexer.cpp
    Parser.cpp
    SemanticAnalyzer.cpp
    Lowerer.cpp
    TypeRules.cpp
)

# Link required dependencies
# PUBLIC = consumers of fe_yourfrontend also get these dependencies.
#
# Key dependencies:
# - zanna_il_full: Full IL infrastructure (Module, Function, BasicBlock, etc.)
# - il_build: IRBuilder API (emitCall, emitRet, startFunction, etc.)
# - il_runtime: Runtime function signatures (rt_print_i64, rt_str_concat, etc.)
#
# Why PUBLIC? Because your headers (Lowerer.hpp, Parser.hpp) will include IL
# headers (il/core/Module.hpp, il/build/IRBuilder.hpp), so any code that
# includes your headers also needs these dependencies.
target_link_libraries(fe_yourfrontend PUBLIC
    zanna_il_full      # Full IL infrastructure
    il_build           # IRBuilder API
    il_runtime         # Runtime function signatures
)

# Setup include paths
# This allows your code to #include "yourfrontend/Lexer.hpp" and
# #include "il/build/IRBuilder.hpp".
#
# $<BUILD_INTERFACE:...> = only apply these paths during build (not installation)
# ${CMAKE_CURRENT_SOURCE_DIR}/../.. = src/ directory (parent of frontends/)
# ${CMAKE_BINARY_DIR}/generated/include = for generated headers (if any)
target_include_directories(fe_yourfrontend PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated/include>
)

# Enable C++20
# PUBLIC = consumers also require C++20 (since your headers may use C++20 features)
target_compile_features(fe_yourfrontend PUBLIC cxx_std_20)
```

### Register with zanna Tool

**Why register with zanna?** The `zanna` tool is Zanna's command-line interface for all compilation
operations. It provides a unified interface:

```bash
zanna run source.bas                  # Unified built-in frontend path
zanna build source.bas -o source.il   # Emit IL through the unified driver
zanna front yourfrontend -run test.src  # Low-level entry point while bringing up a new frontend
```

Registering your frontend makes it accessible via `zanna front yourfrontend`,
which is the simplest low-level path for compiler bring-up and test integration.
If you want first-class `zanna run` / `zanna build` support for your new source
type, extend target detection after the frontend command works.

**File:** `src/CMakeLists.txt` (the `zanna` executable is defined there from the
`ZANNA_CLI_SOURCES` list; there is no per-tool `CMakeLists.txt` for it)

```cmake
# Add your command handler source
# This source file implements cmdFrontYourFrontend(), the entry point
# for your frontend when invoked via zanna.
target_sources(zanna PRIVATE
    tools/zanna/cmd_front_yourfrontend.cpp
)

# Link your frontend library into zanna
# PRIVATE = zanna binary needs your frontend, but downstream consumers don't
target_link_libraries(zanna PRIVATE
    fe_yourfrontend
)
```

**File:** `src/tools/zanna/main.cpp` (add to command dispatch)

**What this does:** Adds a low-level command dispatch path for your frontend.
When the user runs `zanna front yourfrontend <args>`, this code routes control to
your `cmdFrontYourFrontend()` function.

```cpp
// Forward declare your command handler
// Implementation lives in cmd_front_yourfrontend.cpp
int cmdFrontYourFrontend(int argc, char **argv);

int main(int argc, char **argv) {
    // ... existing code that parses argv[1] into 'cmd' ...

    // Add your frontend command
    // Command format: zanna front yourfrontend [options] source.src
    //
    // argv layout after this check:
    //   argv[0] = "zanna"
    //   argv[1] = "front"
    //   argv[2] = "yourfrontend"
    //   argv[3..] = your frontend's arguments
    //
    // We pass (argc - 3, argv + 3) to skip the first three args
    if (cmd == "front" && argc >= 3 && std::string(argv[2]) == "yourfrontend") {
        return cmdFrontYourFrontend(argc - 3, argv + 3);
    }

    // ... rest of dispatch (existing frontends like "basic") ...
}
```

**Design pattern note:** Zanna uses a simple string-based dispatch instead of a command framework (like CLI11 or boost::
program_options). This keeps dependencies minimal and build times fast.

### Command Handler Template

**What this does:** Implements the low-level entry point for your frontend when
invoked via `zanna front yourfrontend`. This is the glue code that:

1. Parses command-line options
2. Loads source files
3. Invokes your compiler
4. Handles the result (emit IL, run in VM, or report errors)

**File:** `src/tools/zanna/cmd_front_yourfrontend.cpp`

```cpp
#include "yourfrontend/Compiler.hpp"   // Your frontend's main API
#include "il/io/Serializer.hpp"        // IL text serialization
#include "zanna/il/Verify.hpp"         // IL structural validation
#include "zanna/vm/VM.hpp"             // VM execution
#include <fstream>
#include <iostream>

int cmdFrontYourFrontend(int argc, char **argv) {
    //
    // PHASE 1: Parse command-line options
    //
    // Zanna's convention: support -emit-il (print IL text) and -run (execute in VM).
    // You can add more options (-O2, -Wall, --dump-ast, etc.) as needed.
    //
    std::string sourcePath;
    bool emitIl = false;  // Print IL to stdout
    bool run = false;     // Execute in VM

    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-emit-il" && i + 1 < argc) {
            emitIl = true;
            sourcePath = argv[++i];
        } else if (arg == "-run" && i + 1 < argc) {
            run = true;
            sourcePath = argv[++i];
        }
        // Add more options here (-dump-ast, -O2, -Wall, etc.)
    }

    if (sourcePath.empty()) {
        std::cerr << "Usage: zanna front yourfrontend [-emit-il|-run] <source>\n";
        return 1;
    }

    //
    // PHASE 2: Load source file
    //
    // Read entire file into a string. For multi-file projects, you'd invoke
    // the compiler once per file and link the resulting IL modules.
    //
    std::ifstream in(sourcePath);
    if (!in) {
        std::cerr << "Error: cannot open " << sourcePath << "\n";
        return 1;
    }
    std::string source((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());

    //
    // PHASE 3: Setup diagnostics infrastructure
    //
    // SourceManager: Maps file IDs to filenames for error messages.
    // DiagnosticEngine: Accumulates errors/warnings with source locations.
    //
    // Your compiler will call de.error("message", loc) when it finds problems.
    // After compilation, check de.errorCount() to decide if compilation succeeded.
    //
    il::support::SourceManager sm;
    il::support::DiagnosticEngine de;
    uint32_t fileId = sm.addFile(sourcePath);

    //
    // PHASE 4: Invoke your compiler
    //
    // The compile() function should:
    // 1. Lex and parse the source into an AST
    // 2. Run semantic analysis (type checking, name resolution)
    // 3. Lower AST to IL using IRBuilder
    // 4. Return a CompilerResult containing the IL Module and diagnostics
    //
    // If compilation fails (syntax/semantic errors), the module may be
    // incomplete or invalid. Check de.errorCount() before proceeding.
    //
    yourfrontend::CompilerResult result =
        yourfrontend::compile(source, fileId, de, sm);

    //
    // PHASE 5: Check for compilation errors
    //
    // If any errors occurred (de.errorCount() > 0), print them to stderr
    // and exit with a non-zero code. The convention is to print diagnostics
    // in "file:line:col: message" format for editor integration.
    //
    if (de.errorCount() > 0) {
        // Print errors to stderr
        for (const auto &diag : de.diagnostics()) {
            std::cerr << diag.message << "\n";
        }
        return 1;  // Non-zero = compilation failed
    }

    //
    // PHASE 6: Emit IL or run in VM
    //
    // If -emit-il: Serialize IL module to stdout (for inspection or piping to zanna)
    // If -run: Verify IL structure, then execute @main() in the VM
    //

    if (emitIl) {
        // Serialize IL to stdout in textual format.
        // Output can be saved to a .il file or piped to another tool.
        // Example: zanna front yourfrontend -emit-il test.src > test.il
        il::io::Serializer::write(result.module, std::cout);
        return 0;
    }

    if (run) {
        // Step 1: Verify IL structure
        //
        // The Verifier checks:
        // - Every basic block ends with a terminator (ret/br/cbr/switch)
        // - All SSA temporaries are defined before use
        // - Function signatures match call sites
        // - Type constraints are satisfied
        //
        // If verification fails, the IL is malformed (probably a frontend bug).
        //
        auto verifyResult = il::verify::Verifier::verify(result.module);
        if (!verifyResult) {
            std::cerr << "Verification failed\n";
            return 1;
        }

        // Step 2: Execute in VM
        //
        // The VM interprets IL instructions directly. It:
        // - Calls @main() function (must exist and return i64)
        // - Loads runtime functions via FFI (rt_print_i64, rt_str_concat, etc.)
        // - Returns the i64 result from @main (typically 0 for success)
        //
        // RunConfig can customize VM behavior (heap size, stack depth, etc.).
        //
        il::vm::RunConfig cfg;  // Default config (usually sufficient)
        il::vm::Runner runner(result.module, cfg);
        int64_t exitCode = runner.run();
        return static_cast<int>(exitCode);  // Forward exit code to shell
    }

    // Neither -emit-il nor -run specified: print usage
    std::cerr << "Specify -emit-il or -run\n";
    return 1;
}
```

**Key design patterns demonstrated:**

1. **Option parsing**: Simple loop-based parsing. For complex CLI interfaces, consider using a library (CLI11, cxxopts)
   or Zanna's own option parser.

2. **Diagnostics accumulation**: Errors are collected during compilation (not immediately fatal). This allows reporting
   multiple errors at once, improving the developer experience.

3. **Separate IL emission and execution paths**: `-emit-il` is for debugging and inspection; `-run` is for testing. This
   separation makes it easy to compare IL output (golden tests) and runtime behavior (E2E tests).

4. **Verification before execution**: Always verify IL before running it. Verification catches frontend bugs that would
   otherwise cause VM crashes or undefined behavior.

5. **Exit code forwarding**: The VM's exit code (return value from @main) is forwarded to the shell. This enables shell
   scripting and CI integration.

### Build and Test

```bash
# Configure
cmake -S . -B build

# Build your frontend
cmake --build build --target fe_yourfrontend

# Build zanna tool
cmake --build build --target zanna

# Test
./build/src/tools/zanna/zanna front yourfrontend -emit-il test.src
./build/src/tools/zanna/zanna front yourfrontend -run test.src
```

---

## 4. High-Level Architecture

This section explains the big-picture organization of a Zanna frontend: the compilation pipeline, error handling
strategy, and file organization patterns.

### Compilation Pipeline

**Understanding the flow:** Modern compilers use a multi-stage pipeline where each stage transforms the input into a
progressively lower-level representation. This modularity makes the compiler easier to understand, test, and debug.

```text
Source Code
    ↓
┌──────────────────┐
│  1. Lex          │ → Token Stream
└──────────────────┘   Convert raw text to tokens (keywords, identifiers, literals)
    ↓                  Example: "if (x > 0)" → [If, LeftParen, Identifier("x"), Greater, IntLiteral(0), RightParen]
┌──────────────────┐
│  2. Parse        │ → AST (Abstract Syntax Tree)
└──────────────────┘   Build tree structure from tokens, enforce grammar rules
    ↓                  Example: [If, ...] → IfStmt{condition: BinaryExpr{...}, thenBranch: ...}
┌──────────────────┐
│  3. Post-Parse   │ → Annotated AST
│     Passes       │   Optional: Qualify names, resolve imports, desugar syntax
└──────────────────┘   Example: "List" → "Collections.List" after USING directive
    ↓                  (Not all frontends need this phase)
┌──────────────────┐
│  4. Constant     │ → Simplified AST
│     Folding      │   Optional: Evaluate compile-time constants
└──────────────────┘   Example: "3 + 5" → IntLiteral(8)
    ↓                  (Improves IL quality, but not required)
┌──────────────────┐
│  5. Semantic     │ → Validated AST + Symbol Tables
│     Analysis     │   Type checking, name resolution, flow analysis
└──────────────────┘   Catches errors: undefined variables, type mismatches, unreachable code
    ↓                  Builds: Symbol tables, type annotations on AST nodes
┌──────────────────┐
│  6. IL Lowering  │ → IL Module (SSA form)
└──────────────────┘   Convert AST to IL using IRBuilder
    ↓                  This is where language semantics meet IL instructions
┌──────────────────┐
│  7. Verification │ → Validated IL
└──────────────────┘   Check IL structural constraints (every block ends with terminator, etc.)
    ↓                  Catches frontend bugs before execution
┌──────────────────┐
│  8. Execution    │ → Exit Code
│     (VM/Codegen) │   VM interprets IL directly, or codegen emits x86-64
└──────────────────┘   Result: Program output + exit code
```

**Why so many stages?** Each stage has a clear responsibility and can be tested independently. For example:

- **Parsing tests**: Compare AST output to expected structure (golden tests)
- **Semantic tests**: Check that errors are correctly detected
- **IL tests**: Compare generated IL to expected IL (golden tests)
- **E2E tests**: Compare program output to expected output

### Key Principle: Abort-Early Error Handling

**Design philosophy:** Accumulate ALL errors from a stage before aborting. This improves developer experience by showing
multiple errors at once (instead of the frustrating "fix one error, get another" cycle).

Each stage checks for errors before proceeding:

```cpp
CompilerResult compile(const std::string &source, ...) {
    CompilerResult result;

    // 1. Parse: Build AST from tokens
    //    Errors: Syntax errors (missing semicolons, unmatched parens, etc.)
    //    Abort if parse failed (can't proceed without valid AST)
    Parser parser(source);
    auto program = parser.parseProgram();
    if (!program) return result;  // Abort if parse failed

    // 2. Post-parse passes: Optional AST transformations
    //    Examples: Name qualification, import resolution, macro expansion
    //    May emit diagnostics but usually doesn't abort
    runPostParsePasses(*program);

    // 3. Semantic analysis: Validate AST semantics
    //    Errors: Undefined variables, type mismatches, etc.
    //    Abort if errors found (can't generate correct IL from invalid AST)
    SemanticAnalyzer sema(diagnostics);
    sema.analyze(*program);
    if (diagnostics.errorCount() > 0) return result;  // Abort if errors

    // 4. Lower to IL: Convert AST → IL
    //    May emit additional diagnostics during lowering (e.g., unsupported features)
    //    Abort if errors found (incomplete IL module would crash VM or verifier)
    Lowerer lowerer(result.module);
    lowerer.lower(*program, sema);
    if (diagnostics.errorCount() > 0) return result;  // Abort if errors

    return result;
}
```

**Key pattern:** `if (diagnostics.errorCount() > 0) return result;`

This check appears after every stage that can emit errors. The frontend accumulates ALL errors from the stage before
checking the count, so users see all problems at once.

### File Organization Pattern

**Why organize by phase?** Compiler frontends are large (10k+ lines). Organizing by compilation phase makes it easy to
navigate the code ("where does type checking happen?" → SemanticAnalyzer.cpp).

Organize by **compilation phase**, not by language feature:

```text
src/frontends/yourfrontend/
├── Lexer.{cpp,hpp}              # Tokenization (Phase 1)
├── Parser.{cpp,hpp}             # AST construction (Phase 2)
│   ├── Parser_Expr.cpp          # Expression parsing (precedence climbing)
│   ├── Parser_Stmt.cpp          # Statement parsing (if/while/for/return)
│   └── Parser_Decl.cpp          # Declaration parsing (functions/variables/classes)
├── ast/                         # AST node definitions (data structures only)
│   ├── Expr.hpp                 # Expression hierarchy (literals, operators, calls)
│   ├── Stmt.hpp                 # Statement hierarchy (if/while/return/block)
│   └── Decl.hpp                 # Declaration hierarchy (functions/variables/classes)
├── SemanticAnalyzer.{cpp,hpp}   # Validation and type checking (Phase 3)
│   ├── SemanticAnalyzer_Expr.cpp  # Type inference for expressions
│   ├── SemanticAnalyzer_Stmt.cpp  # Control flow validation
│   └── SemanticAnalyzer_Decl.cpp  # Symbol table management
├── Lowerer.{cpp,hpp}            # AST → IL lowering (Phase 4)
│   ├── Lowerer_Expr.cpp         # Expression lowering (operators → IL instructions)
│   ├── Lowerer_Stmt.cpp         # Statement lowering (control flow → basic blocks)
│   └── Lowerer_Decl.cpp         # Declaration lowering (functions → IL functions)
├── TypeRules.{cpp,hpp}          # Type system and coercion (shared by sema + lowerer)
├── BuiltinRegistry.{cpp,hpp}    # Builtin function metadata (print, string ops, math)
└── passes/                      # AST transformation passes (optional)
    ├── ConstantFolding.cpp      # Evaluate compile-time constants
    └── NameQualification.cpp    # Resolve qualified names (A.B.C)
```

**Splitting large files:** Each phase (Parser, SemanticAnalyzer, Lowerer) is split by AST node type:

- **_Expr.cpp**: Expression handling
- **_Stmt.cpp**: Statement handling
- **_Decl.cpp**: Declaration handling

This pattern scales well: The BASIC frontend has 40+ files organized this way, and navigation is straightforward ("where
is FOR loop parsing?" → Parser_Stmt.cpp, search for "parseFor").

**Alternative organization (not recommended):** Organizing by language feature (if.cpp, while.cpp, for.cpp) scatters
related code across the codebase and makes it hard to understand the compilation flow.

---

## 5. Parser Design

This section provides production-quality lexer and parser implementations. The lexer converts source text into tokens,
and the parser builds an AST from those tokens using recursive descent with precedence climbing for expressions.

**Key design decisions explained:**

- **Hand-written vs generated**: We use hand-written lexer/parser instead of generator tools (flex/bison, ANTLR) for
  several reasons: (1) easier debugging, (2) better error messages, (3) no tool dependencies, (4) full control over
  error recovery.
- **Recursive descent**: Simple, fast, and easy to understand. Each grammar rule becomes a function.
- **Precedence climbing**: Handles operator precedence elegantly without explicit grammar rules for each precedence
  level.
- **Single-token lookahead**: Sufficient for most languages; more lookahead adds complexity without much benefit.

### Lexer Implementation

**Purpose**: The lexer (also called "scanner" or "tokenizer") is the first phase of compilation. It converts raw source
text into a stream of tokens, handling:

- Whitespace normalization (spaces/tabs/newlines don't affect parsing)
- Comment removal
- Keyword recognition (if/while/for vs identifiers)
- Literal parsing (numbers, strings)
- Source location tracking (file, line, column for error messages)

**Design pattern**: The lexer maintains a position cursor (`pos_`) that advances through the source string. It uses
`peek_char()` to look ahead without consuming and `get_char()` to consume the current character.

**File:** `Lexer.hpp`

```cpp
#pragma once
#include "support/source_location.hpp"
#include <string>
#include <string_view>

enum class TokenKind {
    // Literals
    IntLiteral,     // 42
    FloatLiteral,   // 3.14
    StringLiteral,  // "hello"

    // Keywords
    If, Then, Else, While, For, Function, Return,

    // Operators
    Plus, Minus, Star, Slash,
    Equal, NotEqual, Less, Greater,

    // Punctuation
    LeftParen, RightParen, LeftBrace, RightBrace,
    Comma, Semicolon,

    // Special
    Identifier,
    Eof,
    Error
};

struct Token {
    TokenKind kind;
    std::string text;        // Original source text
    il::support::SourceLoc loc;  // File, line, column

    // For literals
    union {
        int64_t intValue;
        double floatValue;
    };
};

class Lexer {
public:
    explicit Lexer(std::string source, uint32_t fileId);

    Token next();           // Get next token
    Token peek() const;     // Look ahead without consuming

private:
    std::string source_;
    uint32_t fileId_;
    size_t pos_{0};
    int line_{1};
    int column_{1};

    char peek_char() const;
    char get_char();
    void skipWhitespace();
    void skipComment();

    Token lexNumber();
    Token lexIdentifierOrKeyword();
    Token lexString();
    Token lexOperator();

    bool isKeyword(const std::string &text) const;
    TokenKind keywordKind(const std::string &text) const;

    il::support::SourceLoc currentLoc() const {
        return {fileId_, line_, column_};
    }
};
```

**File:** `Lexer.cpp`

```cpp
#include "Lexer.hpp"
#include <cctype>
#include <unordered_map>

namespace {

// Keyword table (sorted for binary search)
const std::unordered_map<std::string, TokenKind> keywords = {
    {"if", TokenKind::If},
    {"then", TokenKind::Then},
    {"else", TokenKind::Else},
    {"while", TokenKind::While},
    {"for", TokenKind::For},
    {"function", TokenKind::Function},
    {"return", TokenKind::Return},
};

} // anonymous namespace

Lexer::Lexer(std::string source, uint32_t fileId)
    : source_(std::move(source)), fileId_(fileId) {}

Token Lexer::next() {
    skipWhitespace();

    if (pos_ >= source_.size())
        return {TokenKind::Eof, "", currentLoc()};

    char ch = peek_char();

    // Numbers
    if (isdigit(ch))
        return lexNumber();

    // Identifiers and keywords
    if (isalpha(ch) || ch == '_')
        return lexIdentifierOrKeyword();

    // Strings
    if (ch == '"')
        return lexString();

    // Operators and punctuation
    return lexOperator();
}

void Lexer::skipWhitespace() {
    while (pos_ < source_.size()) {
        char ch = source_[pos_];
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            pos_++;
            column_++;
        } else if (ch == '\n') {
            pos_++;
            line_++;
            column_ = 1;
        } else if (ch == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
            // Line comment
            skipComment();
        } else {
            break;
        }
    }
}

void Lexer::skipComment() {
    // Skip until end of line
    while (pos_ < source_.size() && source_[pos_] != '\n')
        pos_++;
}

Token Lexer::lexNumber() {
    il::support::SourceLoc loc = currentLoc();
    size_t start = pos_;

    // Integer part
    while (pos_ < source_.size() && isdigit(source_[pos_])) {
        pos_++;
        column_++;
    }

    // Check for decimal point
    bool isFloat = false;
    if (pos_ < source_.size() && source_[pos_] == '.') {
        isFloat = true;
        pos_++;
        column_++;

        // Fractional part
        while (pos_ < source_.size() && isdigit(source_[pos_])) {
            pos_++;
            column_++;
        }
    }

    std::string text = source_.substr(start, pos_ - start);
    Token tok{isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, text, loc};

    if (isFloat)
        tok.floatValue = std::stod(text);
    else
        tok.intValue = std::stoll(text);

    return tok;
}

Token Lexer::lexIdentifierOrKeyword() {
    il::support::SourceLoc loc = currentLoc();
    size_t start = pos_;

    while (pos_ < source_.size() && (isalnum(source_[pos_]) || source_[pos_] == '_')) {
        pos_++;
        column_++;
    }

    std::string text = source_.substr(start, pos_ - start);

    // Check if keyword
    auto it = keywords.find(text);
    if (it != keywords.end())
        return {it->second, text, loc};

    return {TokenKind::Identifier, text, loc};
}

Token Lexer::lexString() {
    il::support::SourceLoc loc = currentLoc();
    pos_++;  // Skip opening "
    column_++;

    std::string value;
    while (pos_ < source_.size() && source_[pos_] != '"') {
        if (source_[pos_] == '\\' && pos_ + 1 < source_.size()) {
            // Escape sequence
            pos_++;
            column_++;
            char next = source_[pos_];
            switch (next) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                default: value += next; break;
            }
        } else {
            value += source_[pos_];
        }
        pos_++;
        column_++;
    }

    if (pos_ < source_.size()) {
        pos_++;  // Skip closing "
        column_++;
    }

    return {TokenKind::StringLiteral, value, loc};
}

Token Lexer::lexOperator() {
    il::support::SourceLoc loc = currentLoc();
    char ch = get_char();

    // Two-character operators
    if (pos_ < source_.size()) {
        char next = source_[pos_];
        std::string two{ch, next};

        if (two == "==") {
            pos_++; column_++;
            return {TokenKind::Equal, two, loc};
        } else if (two == "!=") {
            pos_++; column_++;
            return {TokenKind::NotEqual, two, loc};
        }
    }

    // Single-character operators
    switch (ch) {
        case '+': return {TokenKind::Plus, "+", loc};
        case '-': return {TokenKind::Minus, "-", loc};
        case '*': return {TokenKind::Star, "*", loc};
        case '/': return {TokenKind::Slash, "/", loc};
        case '<': return {TokenKind::Less, "<", loc};
        case '>': return {TokenKind::Greater, ">", loc};
        case '(': return {TokenKind::LeftParen, "(", loc};
        case ')': return {TokenKind::RightParen, ")", loc};
        case '{': return {TokenKind::LeftBrace, "{", loc};
        case '}': return {TokenKind::RightBrace, "}", loc};
        case ',': return {TokenKind::Comma, ",", loc};
        case ';': return {TokenKind::Semicolon, ";", loc};
        default:
            return {TokenKind::Error, std::string(1, ch), loc};
    }
}

char Lexer::peek_char() const {
    if (pos_ >= source_.size())
        return '\0';
    return source_[pos_];
}

char Lexer::get_char() {
    char ch = peek_char();
    if (ch != '\0') {
        pos_++;
        column_++;
    }
    return ch;
}
```

**Lexer implementation notes:**

1. **Token structure**: Each token carries its kind (IntLiteral, If, Plus, etc.), the original text, source location,
   and value (for literals). The union holds int64_t or double depending on the literal type.

2. **Keyword table**: Using `std::unordered_map` for O(1) keyword lookup is more maintainable than a giant if-else
   chain. Production lexers often use perfect hashing or tries for even better performance.

3. **Comment handling**: The example shows C++-style line comments (`//`). For block comments (`/* */`), you'd track
   nesting depth and handle EOF inside comments.

4. **String escapes**: The `lexString()` function handles basic escape sequences (\n, \t, \\, \"). Production lexers
   would support Unicode escapes (\uXXXX), hex escapes (\xXX), etc.

5. **Number parsing**: This lexer handles integers and floating-point (with decimal point). Production lexers would add:
    - Scientific notation (1.23e-10)
    - Hexadecimal (0xFF), octal (0o77), binary (0b1010)
    - Type suffixes (42L for long, 3.14f for float)
    - Underscores as separators (1_000_000)

6. **Two-character operators**: The lexer must check for two-character operators (==, !=, <=, >=) before falling back to
   single-character operators (=, !, <, >). Order matters!

7. **Error handling**: Returning `TokenKind::Error` for unknown characters lets the parser report the error with proper
   source location. Production lexers might continue lexing to find more errors.

### Parser Implementation

**Purpose**: The parser builds an Abstract Syntax Tree (AST) from the token stream. It enforces the grammar rules of
your language and detects syntax errors (missing semicolons, unmatched parentheses, etc.).

**Recursive descent**: Each grammar rule becomes a parsing function. For example:

- `parseExpression()` handles expressions
- `parseIfStatement()` handles `if (condition) { ... } else { ... }`
- `parseWhileStatement()` handles `while (condition) { ... }`

The functions call each other recursively, mirroring the grammar's structure.

**Error recovery**: When the parser encounters a syntax error, it uses `resyncAfterError()` to skip tokens until it
finds a safe synchronization point (semicolon, closing brace, keyword). This allows parsing to continue and report
multiple errors.

**File:** `Parser.hpp`

```cpp
#pragma once
#include "Lexer.hpp"
#include "AST.hpp"
#include "support/diagnostics.hpp"
#include <memory>
#include <vector>

class Parser {
public:
    Parser(std::string source, uint32_t fileId,
           il::support::DiagnosticEngine &de);

    std::unique_ptr<Program> parseProgram();

private:
    Lexer lexer_;
    Token current_;
    il::support::DiagnosticEngine &de_;

    // Token manipulation
    Token peek() const { return current_; }
    Token advance();
    bool match(TokenKind kind);
    bool expect(TokenKind kind);

    // Parsing methods
    StmtPtr parseStatement();
    StmtPtr parseIfStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseForStatement();
    StmtPtr parseReturnStatement();
    StmtPtr parseExpressionStatement();

    ExprPtr parseExpression(int minPrecedence = 0);
    ExprPtr parsePrimary();
    ExprPtr parseUnary();
    ExprPtr parseBinary(int minPrecedence);
    ExprPtr parsePostfix(ExprPtr base);

    DeclPtr parseDeclaration();
    DeclPtr parseFunctionDecl();
    DeclPtr parseVariableDecl();

    // Helpers
    int precedence(TokenKind kind) const;
    void emitError(const std::string &message, il::support::SourceLoc loc);
    void resyncAfterError();
};
```

**Precedence Climbing for Expressions:**

**Why precedence climbing?** Expression parsing is tricky because operators have different precedences (multiplication
before addition) and associativities (left-to-right vs right-to-left). Precedence climbing handles this elegantly in a
single recursive function.

**How it works:**

1. Parse the left operand (a primary or unary expression)
2. While we see operators with precedence ≥ minPrecedence:
    - Consume the operator
    - Recursively parse the right operand with precedence `prec + 1` (for left-associativity)
    - Build a BinaryExpr node combining left, operator, right
    - The new BinaryExpr becomes the left operand for the next iteration

**Example**: Parsing `2 + 3 * 4`

1. Parse left = `2`
2. See `+` (prec 5), recursively parse right with minPrecedence = 6
3. In recursive call: parse left = `3`, see `*` (prec 6), parse right = `4`, return `3 * 4`
4. Back in original call: build `2 + (3 * 4)`

Result: `*` binds tighter than `+` automatically due to precedence values.

```cpp
ExprPtr Parser::parseExpression(int minPrecedence) {
    // Step 1: Parse prefix expression (unary or primary)
    // This handles unary operators (-x, !x) and leaf nodes (literals, variables, parens)
    ExprPtr left = parseUnary();
    if (!left) return nullptr;

    // Step 2: Parse binary operators with precedence climbing
    // Keep looping while we see operators with sufficient precedence
    while (true) {
        Token op = peek();
        int prec = precedence(op.kind);

        // Stop if operator has lower precedence than required
        // This is how we enforce "* binds tighter than +"
        if (prec < minPrecedence)
            break;

        advance();  // consume operator

        // Step 3: Recursively parse right operand
        // Right-associative (like ^): use prec
        // Left-associative (like +, *, ==): use prec + 1
        //
        // Why prec + 1? Consider "2 + 3 + 4":
        // - Parse left = 2
        // - See + (prec 5), parse right with minPrecedence = 6
        // - In recursive call: see + (prec 5 < 6), stop and return 3
        // - Build (2 + 3), continue
        // - See + (prec 5), parse right = 4
        // - Build ((2 + 3) + 4)
        // Result: left-associative
        ExprPtr right = parseExpression(prec + 1);
        if (!right) return nullptr;

        // Step 4: Build BinaryExpr node
        auto binary = std::make_unique<BinaryExpr>();
        binary->op = opFromToken(op.kind);
        binary->lhs = std::move(left);
        binary->rhs = std::move(right);
        binary->loc = op.loc;

        // The new binary expression becomes the left operand for next iteration
        left = std::move(binary);
    }

    return left;
}

int Parser::precedence(TokenKind kind) const {
    // Higher numbers = higher precedence (binds tighter)
    // Standard C/C++ precedence:
    // 6: * /         (multiplicative)
    // 5: + -         (additive)
    // 4: < > == !=   (relational/equality)
    // 3: &&          (logical AND)
    // 2: ||          (logical OR)
    // 1: =           (assignment)
    switch (kind) {
        case TokenKind::Star:
        case TokenKind::Slash: return 6;
        case TokenKind::Plus:
        case TokenKind::Minus: return 5;
        case TokenKind::Less:
        case TokenKind::Greater:
        case TokenKind::Equal:
        case TokenKind::NotEqual: return 4;
        default: return 0;  // Not an operator
    }
}
```

### Error Recovery

**Why error recovery?** If the parser stops at the first syntax error, users only see one error at a time. Good error
recovery lets the parser continue and report multiple errors, improving the developer experience.

**Strategy**: When we encounter a syntax error, skip tokens until we reach a "synchronization point" - a place where we
can safely resume parsing. Common sync points:

- Semicolons (statement boundaries)
- Closing braces (block boundaries)
- Keywords like `if`, `while`, `for` (new statement starts)

**Example**: Parsing `x = 1 } y = 2;`

1. Parse `x = 1`
2. Expect semicolon, see `}` → **ERROR**
3. Resync: skip `}`, see `y` → not a sync point, keep going
4. See `=` → not a sync point, keep going
5. See `;` → **SYNC POINT**, resume parsing
6. Successfully parse `y = 2;`
7. Report both errors to user

```cpp
void Parser::resyncAfterError() {
    // Skip tokens until we find a statement boundary
    while (current_.kind != TokenKind::Eof) {
        if (current_.kind == TokenKind::Semicolon ||
            current_.kind == TokenKind::RightBrace ||
            current_.kind == TokenKind::If ||
            current_.kind == TokenKind::While ||
            current_.kind == TokenKind::For ||
            current_.kind == TokenKind::Function) {
            break;
        }
        advance();
    }
}

bool Parser::expect(TokenKind kind) {
    if (current_.kind == kind) {
        advance();
        return true;
    }

    emitError("expected " + tokenKindString(kind) +
              ", got " + tokenKindString(current_.kind),
              current_.loc);
    return false;
}
```

---

## 6. AST Design

The Abstract Syntax Tree (AST) is the in-memory representation of your program's structure after parsing. It's "
abstract" because it omits syntactic details (parentheses, semicolons) and focuses on semantic structure.

**Design principles:**

1. **Type safety**: Use strongly-typed node classes instead of generic "Node" with a switch on type. This leverages
   C++'s type system to catch errors at compile time.

2. **Ownership**: Use `std::unique_ptr` for child nodes (clear ownership), raw pointers for cross-references (no
   ownership).

3. **Immutability**: AST nodes should be created during parsing and not modified afterward. Transformations create new
   nodes.

4. **Hierarchy**: Three main categories:
    - **Expressions**: Produce values (literals, variables, operators, function calls)
    - **Statements**: Perform actions (if, while, return, assignments)
    - **Declarations**: Define entities (functions, variables, classes)

5. **Source locations**: Every node tracks its source location for error reporting.

**Why this design?** The BASIC frontend uses this exact pattern and it scales well to 50+ node types. The pattern makes
it easy to:

- Add new node types (just inherit from Expr/Stmt/Decl)
- Traverse the AST (visitor pattern or manual switch)
- Transform the AST (create new nodes with modified children)

### Node Hierarchy

**File:** `AST.hpp`

```cpp
#pragma once
#include "support/source_location.hpp"
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct Expr;
struct Stmt;
struct Decl;

// Smart pointer aliases
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

// ============================================================================
// EXPRESSION NODES
// ============================================================================

struct Expr {
    enum class Kind {
        IntLiteral, FloatLiteral, StringLiteral, BoolLiteral,
        Variable,
        Unary, Binary,
        Call,
        MemberAccess,
    };

    Kind kind;
    il::support::SourceLoc loc;

    virtual ~Expr() = default;

protected:
    explicit Expr(Kind k) : kind(k) {}
};

struct IntLiteralExpr : Expr {
    IntLiteralExpr() : Expr(Kind::IntLiteral) {}
    int64_t value{0};
};

struct FloatLiteralExpr : Expr {
    FloatLiteralExpr() : Expr(Kind::FloatLiteral) {}
    double value{0.0};
};

struct StringLiteralExpr : Expr {
    StringLiteralExpr() : Expr(Kind::StringLiteral) {}
    std::string value;
};

struct BoolLiteralExpr : Expr {
    BoolLiteralExpr() : Expr(Kind::BoolLiteral) {}
    bool value{false};
};

struct VariableExpr : Expr {
    VariableExpr() : Expr(Kind::Variable) {}
    std::string name;
};

struct UnaryExpr : Expr {
    enum class Op { Neg, Not } op;
    ExprPtr operand;

    UnaryExpr() : Expr(Kind::Unary) {}
};

struct BinaryExpr : Expr {
    enum class Op {
        Add, Sub, Mul, Div,
        Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
        LogicalAnd, LogicalOr,
    } op;

    ExprPtr lhs;
    ExprPtr rhs;

    BinaryExpr() : Expr(Kind::Binary) {}
};

struct CallExpr : Expr {
    std::string callee;
    std::vector<ExprPtr> args;

    CallExpr() : Expr(Kind::Call) {}
};

// ============================================================================
// STATEMENT NODES
// ============================================================================

struct Stmt {
    enum class Kind {
        Expression, Return, If, While, For, Block,
    };

    Kind kind;
    il::support::SourceLoc loc;

    virtual ~Stmt() = default;

protected:
    explicit Stmt(Kind k) : kind(k) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;

    ExprStmt() : Stmt(Kind::Expression) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // nullptr for void return

    ReturnStmt() : Stmt(Kind::Return) {}
};

struct IfStmt : Stmt {
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;  // nullptr if no else

    IfStmt() : Stmt(Kind::If) {}
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    StmtPtr body;

    WhileStmt() : Stmt(Kind::While) {}
};

struct ForStmt : Stmt {
    std::string var;
    ExprPtr init;
    ExprPtr condition;
    ExprPtr increment;
    StmtPtr body;

    ForStmt() : Stmt(Kind::For) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;

    BlockStmt() : Stmt(Kind::Block) {}
};

// ============================================================================
// DECLARATION NODES
// ============================================================================

struct Decl {
    enum class Kind { Function, Variable };

    Kind kind;
    il::support::SourceLoc loc;
    std::string name;

    virtual ~Decl() = default;

protected:
    explicit Decl(Kind k) : kind(k) {}
};

struct Param {
    std::string name;
    std::string typeName;  // "int", "string", etc.
};

struct FunctionDecl : Decl {
    std::vector<Param> params;
    std::string returnTypeName;  // "int", "void", etc.
    StmtPtr body;

    FunctionDecl() : Decl(Kind::Function) {}
};

struct VariableDecl : Decl {
    std::string typeName;
    ExprPtr initializer;  // nullptr if no initializer

    VariableDecl() : Decl(Kind::Variable) {}
};

// ============================================================================
// PROGRAM ROOT
// ============================================================================

struct Program {
    std::vector<DeclPtr> declarations;
};
```

**AST design notes:**

1. **Base classes**: `Expr`, `Stmt`, and `Decl` are abstract base classes. They contain a `Kind` enum for runtime type
   checking (used in switch statements) and a virtual destructor for polymorphic deletion.

2. **Kind enum**: Each node has a `kind` field that identifies its concrete type. This is used for downcasting:
   `if (expr.kind == Expr::Kind::Binary) { auto &bin = static_cast<BinaryExpr&>(expr); ... }`

3. **Smart pointer aliases**: `ExprPtr`, `StmtPtr`, `DeclPtr` are shorthand for `std::unique_ptr<...>`. This makes code
   more readable and simplifies refactoring.

4. **Literal nodes**: Simple struct with a value field. No child nodes.

5. **Operator nodes**: `UnaryExpr` and `BinaryExpr` have an `op` enum and child expressions. The operator type is stored
   in the enum, not as a string (more type-safe).

6. **Control flow nodes**: `IfStmt`, `WhileStmt`, `ForStmt` have condition expressions and body statements. `elseBranch`
   is nullable (nullptr if no else).

7. **Declaration nodes**: `FunctionDecl` has parameters, return type, and body. `VariableDecl` has type and optional
   initializer.

8. **Program root**: The top-level `Program` node owns all declarations. This is the entry point for semantic analysis
   and lowering.

### Visitor Pattern (Optional)

**When to use visitors?** The visitor pattern is useful when you have many operations that traverse the AST (type
checking, code generation, pretty printing, optimization passes). It centralizes the traversal logic instead of
scattering it across switch statements.

**Trade-offs:**

- **Pros**: Clean separation of concerns, easy to add new operations, type-safe
- **Cons**: Boilerplate (one visit method per node type), harder to add new node types

**Alternative**: Manual switch on `kind` is simpler for small frontends (<20 node types). The BASIC frontend uses both
approaches: visitors for complex passes (semantic analysis, lowering), switches for simple operations.

```cpp
// In AST.hpp
struct ExprVisitor {
    virtual ~ExprVisitor() = default;
    virtual void visit(IntLiteralExpr &) = 0;
    virtual void visit(FloatLiteralExpr &) = 0;
    virtual void visit(BinaryExpr &) = 0;
    // ... one method per expression type
};

// In Expr base class
struct Expr {
    // ... existing members ...
    virtual void accept(ExprVisitor &visitor) = 0;
};

// In each expression class
struct BinaryExpr : Expr {
    // ... existing members ...
    void accept(ExprVisitor &visitor) override {
        visitor.visit(*this);
    }
};
```

**Usage example:**

```cpp
class TypeChecker : public ExprVisitor {
    Type currentType_;

public:
    void visit(IntLiteralExpr &expr) override {
        currentType_ = Type::Int;
    }

    void visit(BinaryExpr &expr) override {
        expr.lhs->accept(*this);
        Type lhsTy = currentType_;
        expr.rhs->accept(*this);
        Type rhsTy = currentType_;
        currentType_ = promoteTypes(lhsTy, rhsTy);
    }

    // ... other visit methods ...
};

// Check type of expression
TypeChecker checker;
expr->accept(checker);
Type result = checker.currentType_;
```

---

## 7. Semantic Analysis

Semantic analysis validates the AST's meaning, catching errors that the parser can't detect:

- **Type errors**: Adding string + integer, calling function with wrong argument types
- **Name resolution**: Undefined variables, duplicate declarations
- **Control flow**: Break outside loop, return from void function with value
- **Const correctness**: Assigning to const variables

**Why separate from parsing?** Parsing checks syntax (grammar rules), semantic analysis checks meaning (type rules,
scoping rules). Separating these phases makes the compiler easier to understand and test.

**Two-phase approach:**

1. **First pass**: Collect all declarations (functions, types, constants) into symbol tables
2. **Second pass**: Analyze function bodies, checking types and name resolution

This allows forward references (calling a function defined later in the file).

**Output**: The SemanticAnalyzer builds symbol tables that the lowerer uses to generate IL. It also annotates AST nodes
with type information (optional, but useful for lowering).

### SemanticAnalyzer Structure

**File:** `SemanticAnalyzer.hpp`

```cpp
#pragma once
#include "AST.hpp"
#include "support/diagnostics.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

enum class Type {
    Int, Float, String, Bool, Void, Unknown
};

struct FunctionSignature {
    std::string name;
    std::vector<Type> paramTypes;
    Type returnType;
};

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(il::support::DiagnosticEngine &de);

    void analyze(Program &prog);

    // Query results
    std::optional<Type> lookupVarType(const std::string &name) const;
    const FunctionSignature *lookupFunction(const std::string &name) const;
    bool isVariable(const std::string &name) const;

private:
    il::support::DiagnosticEngine &de_;

    // Symbol tables
    std::unordered_map<std::string, Type> variables_;
    std::unordered_map<std::string, FunctionSignature> functions_;
    std::unordered_set<std::string> constants_;

    // Scope management
    struct Scope {
        std::unordered_set<std::string> symbols;
    };
    std::vector<Scope> scopes_;

    void pushScope();
    void popScope();
    void declareVariable(const std::string &name, Type type, il::support::SourceLoc loc);
    void declareFunction(const std::string &name, const FunctionSignature &sig);

    // Analysis methods
    void analyzeDecl(Decl &decl);
    void analyzeStmt(Stmt &stmt);
    Type analyzeExpr(Expr &expr);

    void analyzeFunctionDecl(FunctionDecl &decl);
    void analyzeVariableDecl(VariableDecl &decl);

    void analyzeIfStmt(IfStmt &stmt);
    void analyzeWhileStmt(WhileStmt &stmt);
    void analyzeReturnStmt(ReturnStmt &stmt);

    Type analyzeBinaryExpr(BinaryExpr &expr);
    Type analyzeCallExpr(CallExpr &expr);

    // Type utilities
    Type parseTypeName(const std::string &typeName);
    bool isCompatible(Type expected, Type actual);
    Type promoteTypes(Type lhs, Type rhs);

    // Error reporting
    void emitError(const std::string &message, il::support::SourceLoc loc);
};
```

**File:** `SemanticAnalyzer.cpp`

```cpp
#include "SemanticAnalyzer.hpp"

SemanticAnalyzer::SemanticAnalyzer(il::support::DiagnosticEngine &de)
    : de_(de) {
    pushScope();  // Global scope
}

void SemanticAnalyzer::analyze(Program &prog) {
    // First pass: collect function signatures
    for (auto &decl : prog.declarations) {
        if (auto *fn = dynamic_cast<FunctionDecl*>(decl.get())) {
            FunctionSignature sig;
            sig.name = fn->name;
            sig.returnType = parseTypeName(fn->returnTypeName);
            for (const auto &param : fn->params)
                sig.paramTypes.push_back(parseTypeName(param.typeName));
            declareFunction(fn->name, sig);
        }
    }

    // Second pass: analyze function bodies
    for (auto &decl : prog.declarations) {
        analyzeDecl(*decl);
    }
}

void SemanticAnalyzer::analyzeDecl(Decl &decl) {
    switch (decl.kind) {
        case Decl::Kind::Function:
            analyzeFunctionDecl(static_cast<FunctionDecl&>(decl));
            break;
        case Decl::Kind::Variable:
            analyzeVariableDecl(static_cast<VariableDecl&>(decl));
            break;
    }
}

void SemanticAnalyzer::analyzeFunctionDecl(FunctionDecl &fn) {
    pushScope();  // Function scope

    // Declare parameters
    for (const auto &param : fn.params) {
        Type ty = parseTypeName(param.typeName);
        declareVariable(param.name, ty, fn.loc);
    }

    // Analyze body
    if (fn.body)
        analyzeStmt(*fn.body);

    popScope();
}

void SemanticAnalyzer::analyzeStmt(Stmt &stmt) {
    switch (stmt.kind) {
        case Stmt::Kind::Expression: {
            auto &exprStmt = static_cast<ExprStmt&>(stmt);
            analyzeExpr(*exprStmt.expr);
            break;
        }
        case Stmt::Kind::Return: {
            auto &retStmt = static_cast<ReturnStmt&>(stmt);
            if (retStmt.value)
                analyzeExpr(*retStmt.value);
            break;
        }
        case Stmt::Kind::If:
            analyzeIfStmt(static_cast<IfStmt&>(stmt));
            break;
        case Stmt::Kind::While:
            analyzeWhileStmt(static_cast<WhileStmt&>(stmt));
            break;
        // ... other statement types
    }
}

Type SemanticAnalyzer::analyzeExpr(Expr &expr) {
    switch (expr.kind) {
        case Expr::Kind::IntLiteral:
            return Type::Int;
        case Expr::Kind::FloatLiteral:
            return Type::Float;
        case Expr::Kind::StringLiteral:
            return Type::String;
        case Expr::Kind::BoolLiteral:
            return Type::Bool;
        case Expr::Kind::Variable: {
            auto &varExpr = static_cast<VariableExpr&>(expr);
            auto ty = lookupVarType(varExpr.name);
            if (!ty) {
                emitError("undefined variable '" + varExpr.name + "'", expr.loc);
                return Type::Unknown;
            }
            return *ty;
        }
        case Expr::Kind::Binary:
            return analyzeBinaryExpr(static_cast<BinaryExpr&>(expr));
        case Expr::Kind::Call:
            return analyzeCallExpr(static_cast<CallExpr&>(expr));
        default:
            return Type::Unknown;
    }
}

Type SemanticAnalyzer::analyzeBinaryExpr(BinaryExpr &expr) {
    Type lhs = analyzeExpr(*expr.lhs);
    Type rhs = analyzeExpr(*expr.rhs);

    switch (expr.op) {
        case BinaryExpr::Op::Add:
        case BinaryExpr::Op::Sub:
        case BinaryExpr::Op::Mul:
        case BinaryExpr::Op::Div:
            // Arithmetic: promote to common type
            if (lhs == Type::String || rhs == Type::String) {
                // Special case: string concatenation for +
                if (expr.op == BinaryExpr::Op::Add)
                    return Type::String;
                emitError("cannot apply arithmetic to strings", expr.loc);
                return Type::Unknown;
            }
            return promoteTypes(lhs, rhs);

        case BinaryExpr::Op::Equal:
        case BinaryExpr::Op::NotEqual:
        case BinaryExpr::Op::Less:
        case BinaryExpr::Op::Greater:
            // Comparison: return bool
            return Type::Bool;

        case BinaryExpr::Op::LogicalAnd:
        case BinaryExpr::Op::LogicalOr:
            // Logical: require bool operands
            if (lhs != Type::Bool) {
                emitError("logical operator requires boolean operand", expr.lhs->loc);
            }
            if (rhs != Type::Bool) {
                emitError("logical operator requires boolean operand", expr.rhs->loc);
            }
            return Type::Bool;

        default:
            return Type::Unknown;
    }
}

Type SemanticAnalyzer::promoteTypes(Type lhs, Type rhs) {
    // Int + Int → Int
    // Int + Float → Float
    // Float + Float → Float
    if (lhs == Type::Float || rhs == Type::Float)
        return Type::Float;
    if (lhs == Type::Int && rhs == Type::Int)
        return Type::Int;
    return Type::Unknown;
}

void SemanticAnalyzer::declareVariable(const std::string &name, Type type,
                                      il::support::SourceLoc loc) {
    // Check if already declared in current scope
    if (!scopes_.empty() && scopes_.back().symbols.count(name)) {
        emitError("redefinition of '" + name + "'", loc);
        return;
    }

    variables_[name] = type;
    if (!scopes_.empty())
        scopes_.back().symbols.insert(name);
}

std::optional<Type> SemanticAnalyzer::lookupVarType(const std::string &name) const {
    auto it = variables_.find(name);
    if (it != variables_.end())
        return it->second;
    return std::nullopt;
}

void SemanticAnalyzer::pushScope() {
    scopes_.push_back(Scope{});
}

void SemanticAnalyzer::popScope() {
    if (!scopes_.empty())
        scopes_.pop_back();
}

Type SemanticAnalyzer::parseTypeName(const std::string &typeName) {
    if (typeName == "int") return Type::Int;
    if (typeName == "float") return Type::Float;
    if (typeName == "string") return Type::String;
    if (typeName == "bool") return Type::Bool;
    if (typeName == "void") return Type::Void;
    return Type::Unknown;
}

void SemanticAnalyzer::emitError(const std::string &message,
                                il::support::SourceLoc loc) {
    // Add diagnostic to engine
    // (Detailed implementation depends on DiagnosticEngine API)
}
```

**Semantic analyzer notes:**

1. **Two-pass design**: First pass collects all function signatures, second pass analyzes bodies. This allows forward
   references.

2. **Scope stack**: `scopes_` is a stack of symbol sets. Each function/block pushes a scope, pops on exit. This
   implements lexical scoping.

3. **Symbol tables**: `variables_` and `functions_` are global maps. For production, use a proper symbol table with
   scope-aware lookup.

4. **Type promotion**: `promoteTypes()` implements the numeric tower (Int + Float → Float). Real languages have more
   complex rules (short + int → int, etc.).

5. **Error accumulation**: Semantic errors are collected, not fatal. This allows reporting multiple errors at once.

6. **Query API**: The lowerer calls `lookupVarType()`, `lookupFunction()`, etc. to get semantic information. This is the
   interface between semantic analysis and lowering.

7. **Missing features**: This example skips:
    - Function overloading
    - Type inference
    - Const/mutability checking
    - Control flow analysis (unreachable code, uninitialized variables)
    - Template/generic types

---

## 8. IL Lowering with IRBuilder

**Purpose**: IL lowering is the final frontend phase that translates the validated AST into Zanna IL. This is where your
language's semantics are encoded as low-level SSA instructions.

**Key challenges:**

- **Control flow**: Translating high-level constructs (if/while/for) into basic blocks and branches
- **SSA generation**: Converting mutable variables into SSA temporaries with alloca/load/store
- **Type mapping**: Translating language types to IL types
- **Runtime calls**: Invoking runtime functions for complex operations (string concat, array access)
- **Expression evaluation**: Emitting arithmetic, comparisons, and conversions

**IRBuilder vs manual IL construction**: The IRBuilder provides a fluent API that hides some IL complexity:

- Automatically reserves SSA temporary IDs
- Tracks current insertion point
- Provides high-level operations (emitCall, emitRet)

For operations not exposed by IRBuilder (arithmetic, loads, stores), you construct `Instr` objects directly.

**Pattern used throughout Zanna**: The lowerer walks the AST recursively, maintaining state (current function, current
block, local variable table) and emitting IL instructions as it goes. The resulting IL module is then verified and
executed.

### IRBuilder API Reference

IRBuilder is the primary interface for constructing IL modules. Include:

```cpp
#include "il/build/IRBuilder.hpp"
#include "il/core/Module.hpp"
#include "il/core/Value.hpp"
#include "il/core/Type.hpp"
```

### Type System

**File:** `il/core/Type.hpp`

```cpp
enum class Type::Kind {
    Void,       // No value
    I1,         // 1-bit boolean
    I16,        // 16-bit integer
    I32,        // 32-bit integer
    I64,        // 64-bit integer
    F64,        // 64-bit float (IEEE 754)
    Ptr,        // Pointer to stack memory
    Str,        // Runtime string handle (managed)
    Error,      // Exception error value
    ResumeTok   // Exception resume token
};

// Construction
Type voidTy(Type::Kind::Void);
Type i64Ty(Type::Kind::I64);
Type f64Ty(Type::Kind::F64);
Type strTy(Type::Kind::Str);
Type ptrTy(Type::Kind::Ptr);
```

### Value Construction

**File:** `il/core/Value.hpp`

```cpp
// SSA temporaries: %t0, %t1, %t2, ...
Value::temp(unsigned id)           // %t{id}

// Constants
Value::constInt(int64_t v)         // Integer literal
Value::constBool(bool v)           // true/false
Value::constFloat(double v)        // Float literal
Value::null()                      // Null pointer

// String constant payloads (used for IL textual constants)
Value::constStr(std::string s)     // string literal payload

// Global references
Value::global(std::string name)    // @{name}
```

> Block addresses (e.g., handler labels) are not represented as `Value` objects. The IRBuilder
> takes `BasicBlock&` parameters directly (`emitResumeLabel`, `cbr`, `br`).

### IRBuilder Core API

```cpp
namespace il::build {

class IRBuilder {
public:
    explicit IRBuilder(Module &m);

    // === External Declarations ===
    Extern &addExtern(const std::string &name, Type ret,
                     const std::vector<Type> &params);

    // === Global Variables and String Constants ===
    Global &addGlobal(const std::string &name, Type type, const std::string &init = "");
    Global &addGlobalStr(const std::string &name, const std::string &value);

    // === Function Creation ===
    Function &startFunction(const std::string &name, Type returnType,
                           const std::vector<Param> &params);

    // === Block Management ===
    BasicBlock &addBlock(Function &fn, const std::string &label);
    BasicBlock &createBlock(Function &fn, const std::string &label,
                           const std::vector<Param> &params = {});  // With parameters
    BasicBlock &insertBlock(Function &fn, size_t idx, const std::string &label);

    void setInsertPoint(BasicBlock &bb);

    // === Control Flow ===
    void br(BasicBlock &dst, const std::vector<Value> &args = {});
    void cbr(Value cond, BasicBlock &trueBlock, const std::vector<Value> &trueArgs,
             BasicBlock &falseBlock, const std::vector<Value> &falseArgs);
    void emitRet(const std::optional<Value> &value, il::support::SourceLoc loc);

    // === Instructions ===
    void emitCall(const std::string &callee, const std::vector<Value> &args,
                  const std::optional<Value> &dst, il::support::SourceLoc loc);
    Value emitConstStr(const std::string &globalName, il::support::SourceLoc loc);

    // === SSA Management ===
    unsigned reserveTempId();
    Value blockParam(BasicBlock &bb, unsigned idx);

    // === Resume (Exception Handling) ===
    void emitResumeSame(Value token, il::support::SourceLoc loc);
    void emitResumeNext(Value token, il::support::SourceLoc loc);
    void emitResumeLabel(Value token, BasicBlock &target, il::support::SourceLoc loc);
};

} // namespace il::build
```

### Manual Instruction Emission

For operations not exposed by IRBuilder (arithmetic, loads, stores, etc.), create instructions directly.
The verifier rejects plain signed-integer arithmetic opcodes (`Opcode::Add`, `Sub`, `Mul`, `SDiv`, `UDiv`,
`SRem`, `URem`); use the overflow-checked variants (`IAddOvf`, `ISubOvf`, `IMulOvf`) and divide-by-zero
checked variants (`SDivChk0`, `UDivChk0`, `SRemChk0`, `URemChk0`) instead. The plain enumerators still
exist in `Opcode.hpp` but only for legacy/internal lowering passes.

`Instr::result` is a `std::optional<unsigned>`, so capture the temp id in a local variable **before**
moving the `Instr` into the block; reading `instr.result` after a `std::move` is undefined.

```cpp
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"

// Example: %result = iadd.ovf %lhs, %rhs
Value emitIAdd(BasicBlock &bb, Value lhs, Value rhs) {
    unsigned id = reserveTempId();
    Instr instr;
    instr.result = id;
    instr.op = Opcode::IAddOvf;          // overflow-checked signed addition
    instr.type = Type(Type::Kind::I64);
    instr.operands.push_back(lhs);
    instr.operands.push_back(rhs);
    instr.loc = currentSourceLoc;        // Track source location

    bb.instructions.push_back(std::move(instr));
    return Value::temp(id);
}

// Example: %result = load i64, %ptr
Value emitLoad(BasicBlock &bb, Type ty, Value ptr) {
    unsigned id = reserveTempId();
    Instr instr;
    instr.result = id;
    instr.op = Opcode::Load;
    instr.type = ty;
    instr.operands.push_back(ptr);
    instr.loc = currentSourceLoc;

    bb.instructions.push_back(std::move(instr));
    return Value::temp(id);
}

// Example: store i64, %ptr, %value
void emitStore(BasicBlock &bb, Type ty, Value ptr, Value value) {
    Instr instr;
    instr.op = Opcode::Store;
    instr.type = Type(Type::Kind::Void);
    instr.operands.push_back(ptr);
    instr.operands.push_back(value);
    instr.loc = currentSourceLoc;

    bb.instructions.push_back(std::move(instr));
    // Store is not a terminator; the block's `terminated` flag is set
    // separately by branch/return emission helpers.
}

// Example: %result = alloca 8
Value emitAlloca(BasicBlock &bb, int64_t bytes) {
    unsigned id = reserveTempId();
    Instr instr;
    instr.result = id;
    instr.op = Opcode::Alloca;
    instr.type = Type(Type::Kind::Ptr);
    instr.operands.push_back(Value::constInt(bytes));
    instr.loc = currentSourceLoc;

    bb.instructions.push_back(std::move(instr));
    return Value::temp(id);
}
```

### Opcode Reference

**File:** `il/core/Opcode.def`

```cpp
// Signed integer arithmetic — verifier ONLY accepts the checked forms below
// (plain Opcode::Add/Sub/Mul/SDiv/UDiv/SRem/URem still exist as enumerators
// for internal lowering passes but are rejected by the IL verifier).
Opcode::IAddOvf, ISubOvf, IMulOvf            // Trap on signed overflow
Opcode::SDivChk0, UDivChk0, SRemChk0, URemChk0  // Trap on divide-by-zero (and overflow for sdiv)

// Bitwise (no overflow semantics, accepted as-is)
Opcode::And, Or, Xor
Opcode::Shl, LShr, AShr

// Floating-point
Opcode::FAdd, FSub, FMul, FDiv

// Comparisons (integer)
Opcode::ICmpEq, ICmpNe
Opcode::SCmpLT, SCmpLE, SCmpGT, SCmpGE  // Signed
Opcode::UCmpLT, UCmpLE, UCmpGT, UCmpGE  // Unsigned

// Comparisons (floating-point)
Opcode::FCmpEQ, FCmpNE, FCmpLT, FCmpLE, FCmpGT, FCmpGE
Opcode::FCmpOrd, FCmpUno                // Ordered / unordered

// Conversions
Opcode::Sitofp                          // Signed int → float
Opcode::Fptosi                          // Float → signed int
Opcode::CastSiToFp, CastUiToFp          // Explicit signed/unsigned int → float
Opcode::CastFpToSiRteChk, CastFpToUiRteChk  // Float → int with rounding/range check
Opcode::CastSiNarrowChk, CastUiNarrowChk    // Narrow with overflow check
Opcode::Zext1                           // i1 → i64 (zero-extend)
Opcode::Trunc1                          // i64 → i1 (truncate)

// Memory
Opcode::Alloca           // Stack allocation
Opcode::Load             // Load from memory
Opcode::Store            // Store to memory
Opcode::GEP              // Get element pointer (offset)
Opcode::AddrOf, GAddr    // Address-of local / global symbol
Opcode::ConstStr, ConstNull, ConstF64   // Materialize constant payloads

// Control flow
Opcode::Br               // Unconditional branch
Opcode::CBr              // Conditional branch
Opcode::SwitchI32        // 32-bit switch dispatch
Opcode::Ret              // Return
Opcode::Call, CallIndirect              // Direct / indirect function call
Opcode::Trap             // Abort execution

// Exception handling
Opcode::EhPush, EhPop, EhEntry
Opcode::ResumeSame, ResumeNext, ResumeLabel
Opcode::TrapKind, TrapErr, TrapFromErr
Opcode::ErrGetKind, ErrGetCode, ErrGetIp, ErrGetLine, ErrGetMsg
```

### Lowerer Implementation

**File:** `Lowerer.hpp`

```cpp
#pragma once
#include "AST.hpp"
#include "SemanticAnalyzer.hpp"
#include "il/build/IRBuilder.hpp"
#include "il/core/Module.hpp"
#include <unordered_map>

class Lowerer {
public:
    explicit Lowerer(il::core::Module &mod);

    void lower(const Program &prog, const SemanticAnalyzer &sema);

private:
    il::core::Module &mod_;
    il::build::IRBuilder builder_;
    const SemanticAnalyzer *sema_{nullptr};

    // Current insertion point
    il::core::BasicBlock *currentBlock_{nullptr};
    il::core::Function *currentFunction_{nullptr};

    // Symbol table (variable name → stack slot)
    std::unordered_map<std::string, unsigned> locals_;
    unsigned nextTempId_{0};

    // String constant pool
    std::unordered_map<std::string, std::string> stringLiterals_;
    unsigned nextStringId_{0};

    // Lowering methods
    void lowerDecl(Decl &decl);
    void lowerFunctionDecl(FunctionDecl &decl);
    void lowerStmt(Stmt &stmt);
    il::core::Value lowerExpr(Expr &expr);

    void lowerIfStmt(IfStmt &stmt);
    void lowerWhileStmt(WhileStmt &stmt);
    void lowerReturnStmt(ReturnStmt &stmt);

    il::core::Value lowerBinaryExpr(BinaryExpr &expr);
    il::core::Value lowerCallExpr(CallExpr &expr);

    // Helpers
    il::core::Type ilType(Type astType);
    std::string getStringLabel(const std::string &value);
    unsigned allocateLocal(const std::string &name);
    il::core::Value loadVariable(const std::string &name);
    void storeVariable(const std::string &name, il::core::Value value);

    // Instruction emission helpers
    il::core::Value emitAlloca(int64_t bytes);
    il::core::Value emitLoad(il::core::Type ty, il::core::Value ptr);
    void emitStore(il::core::Type ty, il::core::Value ptr, il::core::Value value);
    il::core::Value emitBinary(Opcode op, il::core::Type ty,
                              il::core::Value lhs, il::core::Value rhs);
    il::core::Value emitCompare(Opcode op, il::core::Value lhs, il::core::Value rhs);
    il::core::Value emitCoerceToI64(il::core::Value val, Type fromType);
    il::core::Value emitCoerceToF64(il::core::Value val, Type fromType);
};
```

**File:** `Lowerer.cpp`

```cpp
#include "Lowerer.hpp"

using namespace il::core;
using namespace il::build;

Lowerer::Lowerer(Module &mod) : mod_(mod), builder_(mod) {}

void Lowerer::lower(const Program &prog, const SemanticAnalyzer &sema) {
    sema_ = &sema;

    // Lower all function declarations
    for (auto &decl : prog.declarations) {
        if (decl->kind == Decl::Kind::Function) {
            lowerFunctionDecl(static_cast<FunctionDecl&>(*decl));
        }
    }
}

void Lowerer::lowerFunctionDecl(FunctionDecl &decl) {
    // Build parameter list
    std::vector<Param> ilParams;
    for (const auto &param : decl.params) {
        Type astType = sema_->lookupVarType(param.name).value_or(Type::Unknown);
        ilParams.push_back({param.name, ilType(astType)});
    }

    // Create function
    Type retType = Type::Unknown;
    if (decl.returnTypeName == "int") retType = Type::Int;
    else if (decl.returnTypeName == "float") retType = Type::Float;
    else if (decl.returnTypeName == "void") retType = Type::Void;

    Function &fn = builder_.startFunction(decl.name, ilType(retType), ilParams);
    currentFunction_ = &fn;

    // Create entry block
    BasicBlock &entry = builder_.addBlock(fn, "entry");
    currentBlock_ = &entry;
    builder_.setInsertPoint(entry);

    // Allocate stack slots for parameters
    for (size_t i = 0; i < decl.params.size(); ++i) {
        unsigned slotId = allocateLocal(decl.params[i].name);
        Value slot = emitAlloca(8);  // Allocate 8 bytes

        // Store parameter value to stack slot
        Value paramVal = Value::temp(i);  // Parameters are %t0, %t1, ...
        emitStore(ilParams[i].type, slot, paramVal);

        locals_[decl.params[i].name] = slotId;
    }

    // Lower function body
    if (decl.body) {
        lowerStmt(*decl.body);
    }

    // Emit default return if needed
    if (!currentBlock_->terminated) {
        if (retType == Type::Void) {
            builder_.emitRet(std::nullopt, decl.loc);
        } else {
            // Return default value (0 for int, 0.0 for float)
            Value zero = (retType == Type::Float)
                ? Value::constFloat(0.0)
                : Value::constInt(0);
            builder_.emitRet(zero, decl.loc);
        }
        currentBlock_->terminated = true;
    }

    // Reset state
    locals_.clear();
    currentFunction_ = nullptr;
    currentBlock_ = nullptr;
}

void Lowerer::lowerStmt(Stmt &stmt) {
    switch (stmt.kind) {
        case Stmt::Kind::Expression: {
            auto &exprStmt = static_cast<ExprStmt&>(stmt);
            lowerExpr(*exprStmt.expr);  // Discard result
            break;
        }
        case Stmt::Kind::Return:
            lowerReturnStmt(static_cast<ReturnStmt&>(stmt));
            break;
        case Stmt::Kind::If:
            lowerIfStmt(static_cast<IfStmt&>(stmt));
            break;
        case Stmt::Kind::While:
            lowerWhileStmt(static_cast<WhileStmt&>(stmt));
            break;
        case Stmt::Kind::Block: {
            auto &block = static_cast<BlockStmt&>(stmt);
            for (auto &s : block.statements)
                lowerStmt(*s);
            break;
        }
    }
}

Value Lowerer::lowerExpr(Expr &expr) {
    switch (expr.kind) {
        case Expr::Kind::IntLiteral:
            return Value::constInt(static_cast<IntLiteralExpr&>(expr).value);
        case Expr::Kind::FloatLiteral:
            return Value::constFloat(static_cast<FloatLiteralExpr&>(expr).value);
        case Expr::Kind::StringLiteral: {
            auto &strExpr = static_cast<StringLiteralExpr&>(expr);
            std::string label = getStringLabel(strExpr.value);
            return builder_.emitConstStr(label, expr.loc);
        }
        case Expr::Kind::BoolLiteral:
            return Value::constBool(static_cast<BoolLiteralExpr&>(expr).value);
        case Expr::Kind::Variable: {
            auto &varExpr = static_cast<VariableExpr&>(expr);
            return loadVariable(varExpr.name);
        }
        case Expr::Kind::Binary:
            return lowerBinaryExpr(static_cast<BinaryExpr&>(expr));
        case Expr::Kind::Call:
            return lowerCallExpr(static_cast<CallExpr&>(expr));
        default:
            return Value::constInt(0);
    }
}

Value Lowerer::lowerBinaryExpr(BinaryExpr &expr) {
    Value lhs = lowerExpr(*expr.lhs);
    Value rhs = lowerExpr(*expr.rhs);

    Type lhsType = sema_->analyzeExpr(*expr.lhs);
    Type rhsType = sema_->analyzeExpr(*expr.rhs);

    // Promote to common type
    Type resultType = (lhsType == Type::Float || rhsType == Type::Float)
        ? Type::Float : Type::Int;

    if (lhsType != resultType)
        lhs = (resultType == Type::Float)
            ? emitCoerceToF64(lhs, lhsType)
            : emitCoerceToI64(lhs, lhsType);
    if (rhsType != resultType)
        rhs = (resultType == Type::Float)
            ? emitCoerceToF64(rhs, rhsType)
            : emitCoerceToI64(rhs, rhsType);

    const bool isFloat = (resultType == Type::Float);

    switch (expr.op) {
        case BinaryExpr::Op::Add:
            return emitBinary(isFloat ? Opcode::FAdd : Opcode::IAddOvf,
                              ilType(resultType), lhs, rhs);
        case BinaryExpr::Op::Sub:
            return emitBinary(isFloat ? Opcode::FSub : Opcode::ISubOvf,
                              ilType(resultType), lhs, rhs);
        case BinaryExpr::Op::Mul:
            return emitBinary(isFloat ? Opcode::FMul : Opcode::IMulOvf,
                              ilType(resultType), lhs, rhs);
        case BinaryExpr::Op::Div:
            return emitBinary(isFloat ? Opcode::FDiv : Opcode::SDivChk0,
                              ilType(resultType), lhs, rhs);
        case BinaryExpr::Op::Equal:
            return emitCompare(Opcode::ICmpEq, lhs, rhs);
        case BinaryExpr::Op::NotEqual:
            return emitCompare(Opcode::ICmpNe, lhs, rhs);
        case BinaryExpr::Op::Less:
            return emitCompare(Opcode::SCmpLT, lhs, rhs);
        case BinaryExpr::Op::Greater:
            return emitCompare(Opcode::SCmpGT, lhs, rhs);
        default:
            return Value::constInt(0);
    }
}

void Lowerer::lowerIfStmt(IfStmt &stmt) {
    // Create blocks
    BasicBlock &thenBlock = builder_.addBlock(*currentFunction_, "if.then");
    BasicBlock &elseBlock = builder_.addBlock(*currentFunction_, "if.else");
    BasicBlock &endBlock = builder_.addBlock(*currentFunction_, "if.end");

    // Emit condition
    Value cond = lowerExpr(*stmt.condition);
    builder_.cbr(cond, thenBlock, {}, elseBlock, {});
    currentBlock_->terminated = true;

    // Emit then branch
    currentBlock_ = &thenBlock;
    builder_.setInsertPoint(thenBlock);
    lowerStmt(*stmt.thenBranch);
    if (!currentBlock_->terminated) {
        builder_.br(endBlock, {});
        currentBlock_->terminated = true;
    }

    // Emit else branch
    currentBlock_ = &elseBlock;
    builder_.setInsertPoint(elseBlock);
    if (stmt.elseBranch) {
        lowerStmt(*stmt.elseBranch);
    }
    if (!currentBlock_->terminated) {
        builder_.br(endBlock, {});
        currentBlock_->terminated = true;
    }

    // Continue in end block
    currentBlock_ = &endBlock;
    builder_.setInsertPoint(endBlock);
}

void Lowerer::lowerWhileStmt(WhileStmt &stmt) {
    BasicBlock &testBlock = builder_.addBlock(*currentFunction_, "while.test");
    BasicBlock &bodyBlock = builder_.addBlock(*currentFunction_, "while.body");
    BasicBlock &endBlock = builder_.addBlock(*currentFunction_, "while.end");

    // Jump to test
    builder_.br(testBlock, {});
    currentBlock_->terminated = true;

    // Emit test
    currentBlock_ = &testBlock;
    builder_.setInsertPoint(testBlock);
    Value cond = lowerExpr(*stmt.condition);
    builder_.cbr(cond, bodyBlock, {}, endBlock, {});
    currentBlock_->terminated = true;

    // Emit body
    currentBlock_ = &bodyBlock;
    builder_.setInsertPoint(bodyBlock);
    lowerStmt(*stmt.body);
    if (!currentBlock_->terminated) {
        builder_.br(testBlock, {});
        currentBlock_->terminated = true;
    }

    // Continue in end block
    currentBlock_ = &endBlock;
    builder_.setInsertPoint(endBlock);
}

void Lowerer::lowerReturnStmt(ReturnStmt &stmt) {
    if (stmt.value) {
        Value retVal = lowerExpr(*stmt.value);
        builder_.emitRet(retVal, stmt.loc);
    } else {
        builder_.emitRet(std::nullopt, stmt.loc);
    }
    currentBlock_->terminated = true;
}

// === Helper Methods ===

il::core::Type Lowerer::ilType(Type astType) {
    switch (astType) {
        case Type::Int: return il::core::Type(il::core::Type::Kind::I64);
        case Type::Float: return il::core::Type(il::core::Type::Kind::F64);
        case Type::String: return il::core::Type(il::core::Type::Kind::Str);
        case Type::Bool: return il::core::Type(il::core::Type::Kind::I1);
        case Type::Void: return il::core::Type(il::core::Type::Kind::Void);
        default: return il::core::Type(il::core::Type::Kind::Void);
    }
}

std::string Lowerer::getStringLabel(const std::string &value) {
    auto it = stringLiterals_.find(value);
    if (it != stringLiterals_.end())
        return it->second;

    std::string label = "str." + std::to_string(nextStringId_++);
    builder_.addGlobalStr(label, value);
    stringLiterals_[value] = label;
    return label;
}

Value Lowerer::loadVariable(const std::string &name) {
    auto it = locals_.find(name);
    if (it == locals_.end()) {
        // Global variable or error
        return Value::constInt(0);
    }

    Value slot = Value::temp(it->second);
    Type varType = sema_->lookupVarType(name).value_or(Type::Unknown);
    return emitLoad(ilType(varType), slot);
}

void Lowerer::storeVariable(const std::string &name, Value value) {
    auto it = locals_.find(name);
    if (it == locals_.end()) {
        // Allocate new local
        unsigned slotId = allocateLocal(name);
        it = locals_.find(name);
    }

    Value slot = Value::temp(it->second);
    Type varType = sema_->lookupVarType(name).value_or(Type::Unknown);
    emitStore(ilType(varType), slot, value);
}

unsigned Lowerer::allocateLocal(const std::string &name) {
    unsigned id = nextTempId_++;
    locals_[name] = id;
    return id;
}

Value Lowerer::emitAlloca(int64_t bytes) {
    unsigned id = nextTempId_++;
    Instr instr;
    instr.result = id;
    instr.op = Opcode::Alloca;
    instr.type = il::core::Type(il::core::Type::Kind::Ptr);
    instr.operands.push_back(Value::constInt(bytes));
    currentBlock_->instructions.push_back(std::move(instr));
    return Value::temp(id);
}

Value Lowerer::emitLoad(il::core::Type ty, Value ptr) {
    unsigned id = nextTempId_++;
    Instr instr;
    instr.result = id;
    instr.op = Opcode::Load;
    instr.type = ty;
    instr.operands.push_back(ptr);
    currentBlock_->instructions.push_back(std::move(instr));
    return Value::temp(id);
}

void Lowerer::emitStore(il::core::Type ty, Value ptr, Value value) {
    Instr instr;
    instr.op = Opcode::Store;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    instr.operands.push_back(ptr);
    instr.operands.push_back(value);
    currentBlock_->instructions.push_back(std::move(instr));
}

Value Lowerer::emitBinary(Opcode op, il::core::Type ty, Value lhs, Value rhs) {
    unsigned id = nextTempId_++;
    Instr instr;
    instr.result = id;
    instr.op = op;
    instr.type = ty;
    instr.operands.push_back(lhs);
    instr.operands.push_back(rhs);
    currentBlock_->instructions.push_back(std::move(instr));
    return Value::temp(id);
}

Value Lowerer::emitCompare(Opcode op, Value lhs, Value rhs) {
    unsigned id = nextTempId_++;
    Instr instr;
    instr.result = id;
    instr.op = op;
    instr.type = il::core::Type(il::core::Type::Kind::I1);
    instr.operands.push_back(lhs);
    instr.operands.push_back(rhs);
    currentBlock_->instructions.push_back(std::move(instr));
    return Value::temp(id);
}
```

---

## 9. Type System Integration

### Language Type → IL Type Mapping

Define how your language's types map to IL types:

```cpp
il::core::Type Lowerer::ilType(Type astType) {
    switch (astType) {
        case Type::Int:
            return il::core::Type(il::core::Type::Kind::I64);
        case Type::Float:
            return il::core::Type(il::core::Type::Kind::F64);
        case Type::String:
            return il::core::Type(il::core::Type::Kind::Str);
        case Type::Bool:
            return il::core::Type(il::core::Type::Kind::I1);
        case Type::Void:
            return il::core::Type(il::core::Type::Kind::Void);
        default:
            return il::core::Type(il::core::Type::Kind::Void);
    }
}
```

### Type Coercion

Implement implicit conversions:

```cpp
// Int → Float
Value Lowerer::coerceToF64(Value val, Type fromType) {
    if (fromType == Type::Float)
        return val;  // Already float

    if (fromType == Type::Int) {
        unsigned id = nextTempId_++;
        Instr instr;
        instr.result = id;
        instr.op = Opcode::Sitofp;
        instr.type = il::core::Type(il::core::Type::Kind::F64);
        instr.operands.push_back(val);
        currentBlock_->instructions.push_back(std::move(instr));
        return Value::temp(id);
    }

    return val;
}

// Float → Int (truncate)
Value Lowerer::coerceToI64(Value val, Type fromType) {
    if (fromType == Type::Int)
        return val;  // Already int

    if (fromType == Type::Float) {
        unsigned id = nextTempId_++;
        Instr instr;
        instr.result = id;
        instr.op = Opcode::Fptosi;
        instr.type = il::core::Type(il::core::Type::Kind::I64);
        instr.operands.push_back(val);
        currentBlock_->instructions.push_back(std::move(instr));
        return Value::temp(id);
    }

    return val;
}

// Bool → Int (zero-extend)
Value Lowerer::coerceBoolToI64(Value val) {
    unsigned id = nextTempId_++;
    Instr instr;
    instr.result = id;
    instr.op = Opcode::Zext1;
    instr.type = il::core::Type(il::core::Type::Kind::I64);
    instr.operands.push_back(val);
    currentBlock_->instructions.push_back(std::move(instr));
    return Value::temp(id);
}
```

---

## 10. Runtime ABI Reference

### Available Runtime Functions

Zanna provides a C-based runtime library with common functionality.

**Include:**

```cpp
#include "il/runtime/RuntimeSignatures.hpp"
```

### String Operations

```cpp
// Declare in IL (canonical name):
builder.addExtern("Zanna.String.Concat", Type(Type::Kind::Str),
                 {Type(Type::Kind::Str), Type(Type::Kind::Str)});

// Call:
// %result = call @Zanna.String.Concat(%str1, %str2)

// Selected functions (canonical name | legacy alias when ZANNA_RUNTIME_NS_DUAL=ON):
Zanna.String.Concat       | rt_str_concat        : str(str,str)        // Concatenation
Zanna.String.Equals       | rt_str_eq            : i1(str,str)         // Equality
Zanna.String.Cmp          | rt_str_cmp           : i64(str,str)        // <0/0/>0 ordering
Zanna.String.get_Length   | rt_str_len           : i64(str)            // Length in bytes
Zanna.String.Left         | rt_str_left          : str(str,i64)        // Left substring
Zanna.String.Right        | rt_str_right         : str(str,i64)        // Right substring
Zanna.String.MidLen       | rt_str_mid_len       : str(str,i64,i64)    // Substring (start, len)
Zanna.String.ToUpper      | rt_str_ucase         : str(str)            // Uppercase
Zanna.String.ToLower      | rt_str_lcase         : str(str)            // Lowercase
Zanna.String.Chr          | rt_str_chr           : str(i64)            // Codepoint → char
Zanna.String.Asc          | rt_str_asc           : i64(str)            // First char → int
Zanna.String.IndexOf      | rt_str_index_of      : i64(str,str)        // Find substring
Zanna.String.FromI16      | rt_str_i16_alloc     : str(i16)            // i16 → string
Zanna.String.FromI32      | rt_str_i32_alloc     : str(i32)            // i32 → string
Zanna.Text.Fmt.Int             | rt_fmt_int           : str(i64)            // i64 → string
Zanna.Core.Convert.ToStringDouble | rt_f64_to_str : str(f64)           // f64 -> string
```

> **Reference counting note:** `rt_string_ref` / `rt_string_unref` are internal C runtime helpers and are
> NOT exposed to IL frontends. The runtime applies retain/release at the boundaries of the runtime
> functions listed above and at the call/ret/store sites managed by the lowerer; do not call them
> directly from emitted IL.

### Array Operations

These helpers are registered through `Signatures_Arrays.cpp` (not `runtime.def`) and are intended for
frontend-emitted IL that needs raw array storage. They are passed/returned as `ptr` array handles.

```text
// 32-bit integer arrays (i32 element type, but get/set use i64 sign-extended values in the IL signature)
rt_arr_i32_new(i64) -> ptr               // Allocate, length in elements
rt_arr_i32_retain(ptr) -> void           // Retain (refcount++)
rt_arr_i32_release(ptr) -> void          // Release (refcount--)
rt_arr_i32_len(ptr) -> i64               // Length in elements
rt_arr_i32_get(ptr, i64) -> i64          // Get element (sign-extended)
rt_arr_i32_set(ptr, i64, i64) -> void    // Set element
rt_arr_i32_resize(ptr, i64) -> ptr       // Resize, returns possibly relocated handle

// 64-bit integer arrays (mirrors the i32 family)
rt_arr_i64_new / rt_arr_i64_retain / rt_arr_i64_release
rt_arr_i64_len / rt_arr_i64_get / rt_arr_i64_set / rt_arr_i64_resize

// String arrays
rt_arr_str_alloc(i64) -> ptr             // Allocate, length in elements
rt_arr_str_release(ptr, i64) -> void     // Release (length needed for element teardown)
rt_arr_str_len(ptr) -> i64               // Length in elements
rt_arr_str_get(ptr, i64) -> ptr          // Get element (string handle as ptr)
rt_arr_str_put(ptr, i64, ptr) -> void    // Set element (handle as ptr)

// Out-of-bounds panic shared by all arrays
rt_arr_oob_panic(i64, i64) -> void       // (index, length)
```

### OOP: Array Fields (BASIC CLASS)

Array fields declared inside a `CLASS` are represented as pointer-sized handles in the object layout. Lowering supports
both reads and writes via the same runtime helpers used for normal arrays; the only difference is that the base array
handle is loaded from the object field before invoking the helper.

Key pieces in the BASIC frontend:

- Constructor initialization: If an array field declares extents (e.g., `DIM data(8) AS INTEGER`), the constructor
  allocates the array and stores the handle into the field.
    - File: `src/frontends/basic/lower/oop/Lower_OOP_Emit.cpp`
    - Function: `Lowerer::emitClassConstructor`
    - Mapping:
        - Integer arrays → `rt_arr_i32_new(len)`
        - String arrays → `rt_arr_str_alloc(len)`
- Loads: `obj.field(i)` lowers to a load of the array handle from the field followed by `rt_arr_*_get(obj.field, i)`.
    - Files:
        - `src/frontends/basic/lower/Emit_Expr.cpp` (dotted array names in `lowerArrayAccess`)
        - `src/frontends/basic/lower/Lowerer_Expr.cpp` (treat `MethodCallExpr` on field name as array get
          only when the field is declared as an array; guard with `fld->isArray`)
    - Helpers: `rt_arr_i32_get`, `rt_arr_str_get`, `rt_arr_obj_get` (for object-element arrays), with `rt_arr_*_len` for
      bounds checks when emitted.
- Stores: `obj.field(i) = value` lowers to a store into the array referenced by the field.
    - File: `src/frontends/basic/LowerStmt_Runtime.cpp`
    - Paths:
        - `assignArrayElement` handles dotted names, selecting helpers by element type
        - LHS `MethodCallExpr` path synthesizes store for `obj.field(index)`
    - Helpers: `rt_arr_i32_set`, `rt_arr_str_put`, `rt_arr_obj_put` (for object-element arrays) with bounds checks via
      `rt_arr_*_len` + `rt_arr_oob_panic`.
- Layout: Array fields occupy pointer-sized storage so subsequent field offsets are consistent.
    - Files:
        - `src/frontends/basic/lower/oop/Lower_OOP_Scan.cpp` (class layout builder)
        - `src/frontends/basic/Lowerer.hpp` (`ClassLayout` metadata)

Example lowering flow for `obj.field(i) = 42` (integer array field):

1) Load `self` pointer; compute field address via `GEP(self, field.offset)`
2) Load array handle from field pointer
3) Coerce index to i64; compute/emit bounds check using `rt_arr_i32_len`
4) Call `rt_arr_i32_set(handle, index, 42)`

Notes:

- String array fields retain/release element handles using `rt_arr_str_put/rt_arr_str_get` and the standard string
  retain/release hooks.
- Object array fields use `rt_arr_obj_len/get/put` and are not refcounted at the array-handle level; only element
  objects participate in retain/release.
- Single-dimension indexing of array fields is supported; multi-dimension flattening for fields follows the same
  row‑major approach as normal arrays when metadata is available.

### I/O Operations

```text
// Canonical name              | legacy alias       | signature             // notes
Zanna.Terminal.PrintStr        | rt_print_str       : void(str)            // Print string, no newline
Zanna.Terminal.PrintI64        | rt_print_i64       : void(i64)            // Print integer, no newline
Zanna.Terminal.PrintF64        | rt_print_f64       : void(f64)            // Print float, no newline
Zanna.Terminal.Say             | rt_term_say        : void(str)            // Print string + newline
Zanna.Terminal.SayInt          | rt_term_say_i64    : void(i64)            // Print integer + newline
Zanna.Terminal.SayNum          | rt_term_say_f64    : void(f64)            // Print float + newline
Zanna.Terminal.InputLine       | rt_input_line      : str()                // Read line from stdin
Zanna.Terminal.TryReadLine     | rt_term_try_read_line : obj<Zanna.Option>() // Read line, None on EOF
Zanna.Terminal.ReadLineResult  | rt_term_read_line_result : obj<Zanna.Result>() // Read line, Err on EOF
Zanna.Terminal.TryAsk          | rt_term_try_ask    : obj<Zanna.Option>(str) // Prompt + read, None on EOF
Zanna.Terminal.AskResult       | rt_term_ask_result : obj<Zanna.Result>(str) // Prompt + read, Err on EOF
Zanna.Terminal.ReadLine        | rt_term_read_line  : str()                // Compatibility read; prefer TryReadLine/ReadLineResult for EOF
```

There is no dedicated `rt_print_newline`; use one of the `Say*` variants (which append a newline) or
emit `Zanna.Terminal.PrintStr` with `"\n"`.

### Math Operations

```text
Zanna.Math.Sqrt                | rt_sqrt            : f64(f64)             // Square root
Zanna.Math.AbsInt              | rt_abs_i64         : i64(i64)             // Absolute value (int)
Zanna.Math.Abs                 | rt_abs_f64         : f64(f64)             // Absolute value (float)
Zanna.Math.Floor               | rt_floor           : f64(f64)             // Floor
Zanna.Math.Ceil                | rt_ceil            : f64(f64)             // Ceiling
Zanna.Math.Sin                 | rt_sin             : f64(f64)             // Sine
Zanna.Math.Cos                 | rt_cos             : f64(f64)             // Cosine
Zanna.Math.Tan                 | rt_tan             : f64(f64)             // Tangent
Zanna.Math.Atan                | rt_atan            : f64(f64)             // Arctangent
Zanna.Math.Exp                 | rt_exp             : f64(f64)             // Exponential
Zanna.Math.Log                 | rt_log             : f64(f64)             // Natural logarithm
Zanna.Math.Pow                 | rt_math_pow        : f64(f64,f64)         // Power
```

### Example: String Concatenation

```cpp
Value Lowerer::lowerStringConcat(Value lhs, Value rhs) {
    // Declare runtime function (canonical name; legacy rt_str_concat alias
    // is also accepted while ZANNA_RUNTIME_NS_DUAL=ON).
    builder_.addExtern("Zanna.String.Concat", Type(Type::Kind::Str),
                      {Type(Type::Kind::Str), Type(Type::Kind::Str)});

    // Emit call
    unsigned resultId = nextTempId_++;
    builder_.emitCall("Zanna.String.Concat", {lhs, rhs},
                     Value::temp(resultId), currentSourceLoc);

    return Value::temp(resultId);
}
```

---

## 11. Diagnostics and Error Reporting

### DiagnosticEngine API

**File:** `src/support/diagnostics.hpp`

```cpp
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

namespace il::support {

enum class Severity { Note, Warning, Error };

struct Diagnostic {
    Severity severity;    // Note, Warning, Error
    std::string message;  // Human-readable text
    SourceLoc loc;        // Optional source location
    std::string code;     // Optional diagnostic code (e.g., "B1001", "IL001")
};

class DiagnosticEngine {
public:
    void report(Diagnostic d);

    size_t errorCount() const;
    size_t warningCount() const;

    // Access collected diagnostics for inspection.
    const std::vector<Diagnostic> &diagnostics() const;

    // Pretty-print all recorded diagnostics.
    void printAll(std::ostream &os, const SourceManager *sm = nullptr) const;
};

} // namespace il::support
```

### Emitting Diagnostics

```cpp
void Parser::emitError(const std::string &message, il::support::SourceLoc loc) {
    de_.report({il::support::Severity::Error, message, loc, "E001"});
}

void SemanticAnalyzer::emitError(const std::string &message,
                                 il::support::SourceLoc loc) {
    de_.report({il::support::Severity::Error, message, loc, "S001"});
}
```

### Example Error Messages

```cpp
// Parse error
emitError("expected ';' after statement", current_.loc);

// Type error
emitError("cannot apply operator '+' to types 'int' and 'string'", expr.loc);

// Undefined variable
emitError("undefined variable '" + name + "'", expr.loc);

// Argument count mismatch
emitError("function 'foo' expects 2 arguments, got 3", call.loc);
```

---

## 12. Value and Symbol Management

### SSA Value Naming

```cpp
// Temporaries: %t0, %t1, %t2, ...
Value::temp(0)  → "%t0"
Value::temp(1)  → "%t1"

// Globals: @name
Value::global("main")       → "@main"
Value::global("my_string")  → "@my_string"

// Constants: immediate
Value::constInt(42)    → "42"
Value::constFloat(3.14) → "3.14"
Value::constBool(true)  → "true"
Value::null()          → "null"
```

### Variable Storage Pattern

Variables are stored on the stack using `alloca`:

```cpp
// DIM x AS INT in BASIC becomes:
// %t0 = alloca 8          ; Allocate stack slot
// store i64, %t0, 0       ; Initialize to 0

// x = 42 becomes:
// store i64, %t0, 42      ; Store to slot

// y = x becomes:
// %t1 = load i64, %t0     ; Load from x's slot
// store i64, %y_slot, %t1 ; Store to y's slot
```

**Implementation:**

```cpp
struct LocalVariable {
    unsigned slotId;     // Temp ID of the alloca
    Type type;           // Variable type
};

std::unordered_map<std::string, LocalVariable> locals_;

unsigned Lowerer::allocateVariable(const std::string &name, Type type) {
    // Emit alloca
    Value slot = emitAlloca(8);  // 8 bytes for i64/f64/ptr/str

    // Record in symbol table. `Value::id` is a public union field on
    // Value, valid when `slot.kind == Value::Kind::Temp`.
    unsigned slotId = slot.id;
    locals_[name] = {slotId, type};

    return slotId;
}

Value Lowerer::loadVariable(const std::string &name) {
    auto it = locals_.find(name);
    if (it == locals_.end()) {
        // Error: undefined variable
        return Value::constInt(0);
    }

    Value slot = Value::temp(it->second.slotId);
    return emitLoad(ilType(it->second.type), slot);
}

void Lowerer::storeVariable(const std::string &name, Value value) {
    auto it = locals_.find(name);
    if (it == locals_.end()) {
        // Error: undefined variable
        return;
    }

    Value slot = Value::temp(it->second.slotId);
    emitStore(ilType(it->second.type), slot, value);
}
```

---

## 13. Special Features

### Block Arguments (Phi Nodes)

IL uses block arguments instead of explicit phi nodes:

```cpp
// Create block with parameters
std::vector<Param> params = {
    {"i", Type(Type::Kind::I64)},
    {"sum", Type(Type::Kind::I64)}
};
BasicBlock &loopHeader = builder_.createBlock(fn, "loop", params);

// Access parameters
Value i = builder_.blockParam(loopHeader, 0);
Value sum = builder_.blockParam(loopHeader, 1);

// Branch with arguments
builder_.br(loopHeader, {Value::constInt(0), Value::constInt(0)});
```

**Full Example (sum 0..9):**

```cpp
// for (i = 0, sum = 0; i < 10; i++, sum += i)

BasicBlock &entry = builder_.addBlock(fn, "entry");
BasicBlock &loop = builder_.createBlock(fn, "loop",
    {{"i", Type(Type::Kind::I64)}, {"sum", Type(Type::Kind::I64)}});
BasicBlock &done = builder_.addBlock(fn, "done");

// entry: br loop(0, 0)
builder_.setInsertPoint(entry);
builder_.br(loop, {Value::constInt(0), Value::constInt(0)});

// loop:
builder_.setInsertPoint(loop);
Value i = builder_.blockParam(loop, 0);
Value sum = builder_.blockParam(loop, 1);

// if i < 10
Value cond = emitCompare(Opcode::SCmpLT, i, Value::constInt(10));

BasicBlock &body = builder_.addBlock(fn, "body");
builder_.cbr(cond, body, {}, done, {});

// body:
builder_.setInsertPoint(body);
Value i_next = emitBinary(Opcode::IAddOvf, Type(Type::Kind::I64),
                         i, Value::constInt(1));
Value sum_next = emitBinary(Opcode::IAddOvf, Type(Type::Kind::I64), sum, i);
builder_.br(loop, {i_next, sum_next});

// done: return sum
builder_.setInsertPoint(done);
// Need to pass sum through... (more complex, omitted)
```

### Exception Handling (ON ERROR GOTO)

`IRBuilder` does not expose `emitEhPush` / `emitEhPop` directly — those instructions are constructed
manually as `Instr` objects with `Opcode::EhPush` / `Opcode::EhPop`. The handler block must declare
two parameters `(%err:Error, %tok:ResumeTok)` so the runtime can pass the active error/resume token.
The BASIC frontend's helper at `src/frontends/basic/lower/Emit_Control.cpp` (`Lowerer::emitEhPush`)
is a useful template.

```cpp
// ON ERROR GOTO handler
BasicBlock &handler = builder_.createBlock(fn, "handler",
    {{"err", Type(Type::Kind::Error)},
     {"tok", Type(Type::Kind::ResumeTok)}});

// Push handler (manual Instr construction)
{
    Instr push;
    push.op = Opcode::EhPush;
    push.type = Type(Type::Kind::Void);
    push.labels.push_back(handler.label);
    push.brArgs.push_back({});  // EhPush takes no arguments to the handler
    push.loc = loc;
    currentBlock_->instructions.push_back(std::move(push));
}

// ... code that might trap ...

// handler:
builder_.setInsertPoint(handler);
Value tok = builder_.blockParam(handler, 1);  // %tok:ResumeTok
// Handle error, then:
// RESUME NEXT
builder_.emitResumeNext(tok, loc);
```

### ADDFILE Directive (Parse-Time Include)

BASIC frontends can support a simple include directive that splices another BASIC file into the current
program at parse time.

- **Syntax:** `ADDFILE "path.bas"`
- **Scope:** File-scope only (top level). Not permitted inside `SUB`/`FUNCTION`/`CLASS`/`NAMESPACE` bodies.
- **Behavior:**
    - Loads the referenced `.bas` file and parses it as a nested program.
    - Appends included procedures to the current program's procedure list.
    - Inserts included top-level statements where the directive appears, preserving order.
- **Path Resolution:**
    - Relative paths are resolved against the directory of the including file.
    - Absolute paths are supported as-is.
- **Safety:**
    - Cycle detection prevents recursive inclusion of the same file; a diagnostic is emitted.
    - Depth limit (default: 32) guards against runaway recursion; exceeding the limit triggers a diagnostic.
- **Diagnostics:**
    - I/O failures (unreadable/missing files) report the resolved path.
    - Each included file is registered with the SourceManager so diagnostics point to the correct file and line.
- **Notes:**
    - A numeric line label may precede the directive (e.g., `100 ADDFILE "inc.bas"`); the label is recorded
      and the directive still applies.
    - Any trailing tokens on the directive line are ignored up to end-of-line; inline comments are acceptable.

---

## 14. Testing Strategy

### Unit Tests

Test individual components in isolation.

**File:** `src/tests/unit/frontends/yourfrontend/LexerTests.cpp`

```cpp
#include "tests/TestHarness.hpp"
#include "yourfrontend/Lexer.hpp"

TEST(LexerTests, TokenizeNumber) {
    Lexer lexer("42", 0);
    Token tok = lexer.next();

    EXPECT_EQ(tok.kind, TokenKind::IntLiteral);
    EXPECT_EQ(tok.intValue, 42);
}

TEST(LexerTests, TokenizeKeyword) {
    Lexer lexer("if", 0);
    Token tok = lexer.next();

    EXPECT_EQ(tok.kind, TokenKind::If);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
```

**File:** `src/tests/unit/frontends/yourfrontend/ParserTests.cpp`

```cpp
#include "tests/TestHarness.hpp"
#include "yourfrontend/Parser.hpp"

TEST(ParserTests, ParseIfStatement) {
    std::string source = "if x < 10 then return x";
    il::support::DiagnosticEngine de;
    Parser parser(source, 0, de);

    auto prog = parser.parseProgram();
    ASSERT_NE(prog, nullptr);
    ASSERT_EQ(prog->declarations.size(), 0);  // No top-level decls
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
```

### Golden Tests

Compare generated IL against expected output.

**Input:** `src/tests/golden/yourfrontend/hello.src`

```text
print("Hello, World!")
```

**Expected:** `src/tests/golden/yourfrontend/hello.il`

```il
il 0.3.0

extern @Zanna.Terminal.PrintStr(str) -> void

func @main() -> i64 {
entry:
  %t0 = const_str @str.0
  call @Zanna.Terminal.PrintStr(%t0)
  ret 0
}

global const str @str.0 = "Hello, World!"
```

**Test Driver:**

```cpp
TEST_P(GoldenTest, CompareIL) {
    std::string inputPath = GetParam();
    std::string expectedPath = inputPath + ".il";

    // Compile
    auto result = compileFile(inputPath);
    ASSERT_TRUE(result.succeeded());

    // Serialize IL
    std::ostringstream ilStream;
    il::io::Serializer::write(result.module, ilStream);
    std::string actualIL = ilStream.str();

    // Compare
    std::string expectedIL = readFile(expectedPath);
    EXPECT_EQ(normalize(actualIL), normalize(expectedIL));
}
```

### E2E Tests

Run complete programs and check output.

**Input:** `src/tests/e2e/yourfrontend/fibonacci.src`

```text
function fib(n) {
    if n <= 1 then return n
    return fib(n - 1) + fib(n - 2)
}

print(fib(10))
```

**Test:**

```cpp
TEST(E2ETest, Fibonacci) {
    auto result = compileAndRun("src/tests/e2e/yourfrontend/fibonacci.src");

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.stdout, "55\n");
    EXPECT_EQ(result.exitCode, 0);
}
```

---

## 15. Common Pitfalls and Debugging

### Pipeline Dump Flags

When debugging your frontend, use the `--dump-*` CLI flags to inspect output at each compilation stage:

```sh
zanna run --dump-tokens program.zia   # See what the lexer produces
zanna run --dump-ast program.zia      # See the parsed AST
zanna run --dump-il program.zia       # See IL after lowering
zanna run --dump-il-opt program.zia   # See IL after optimization
zanna run --dump-il-passes program.zia # See IL before/after each pass
```

All output goes to stderr. See `docs/tools/debugging.md` §8 for full details.

### 1. Forgetting to Terminate Blocks

**Error:** IL verifier fails with "block not terminated"

**Cause:**

```cpp
builder_.setInsertPoint(block);
// ... emit some instructions ...
// FORGOT: builder_.br(nextBlock, {});
```

**Fix:** Every block must end with a terminator (br, cbr, ret, trap):

```cpp
if (!currentBlock_->terminated) {
    builder_.br(nextBlock, {});
    currentBlock_->terminated = true;
}
```

### 2. SSA Use-Before-Define

**Error:** IL verifier fails with "use of undefined value"

**Cause:**

```cpp
// WRONG: using %t2 before it's defined
%t1 = add %t0, %t2
%t2 = const 42
```

**Fix:** Define values before use:

```cpp
%t2 = const 42
%t1 = add %t0, %t2
```

### 3. Type Mismatches

**Error:** IL verifier fails with "type mismatch"

**Cause:**

```text
; WRONG: i64+f64 has no opcode at all, and plain `add` is verifier-rejected
; for signed integers anyway — use the .ovf form once both sides are i64
%result = iadd.ovf %int_val, %float_val
```

**Fix:** Coerce to a common type *before* the arithmetic, then use the matching opcode family:

```text
%float_int = sitofp %int_val
%result    = fadd %float_int, %float_val
```

### 4. Branch Argument Arity Mismatch

**Error:** IL verifier fails with "argument count mismatch"

**Cause:**

```cpp
// Block expects 2 arguments
BasicBlock &loop = builder_.createBlock(fn, "loop",
    {{"i", Type(Type::Kind::I64)}, {"sum", Type(Type::Kind::I64)}});

// Branch only passes 1
builder_.br(loop, {Value::constInt(0)});  // WRONG
```

**Fix:** Match argument count:

```cpp
builder_.br(loop, {Value::constInt(0), Value::constInt(0)});
```

### 5. Memory Leaks (String/Array References)

**Error:** Memory leaks in long-running programs

**Cause:** Refcounted values (strings, arrays, objects) escape their lowering scope without the
matching retain/release bookkeeping. Frontends do not call retain/release runtime helpers directly —
they emit IL that participates in the runtime's automatic refcount accounting at call/store/return
sites.

**Fix:** Mirror the BASIC frontend's lowering passes (`src/frontends/basic/lower/`), which:

1. Track which temporaries hold refcounted values.
2. Emit balanced retain/release as part of variable assignment, function call boundaries, and
   block parameter handoff.
3. Use the runtime helpers exposed through `runtime.def` (`Zanna.String.*`, `Zanna.Collections.*`)
   rather than the internal `rt_string_ref` / `rt_string_unref` C entry points, which are not
   surfaced as IL externs.

### 6. Debugging Tips

**Enable IL Verification:**

```cpp
#include "zanna/il/Verify.hpp"

auto result = il::verify::Verifier::verify(module);
if (!result) {
    // result.error() returns a const Diag&; .message is a public std::string field.
    std::cerr << "Verification failed: " << result.error().message << "\n";
}
```

**Dump IL to stderr:**

```cpp
il::io::Serializer::write(module, std::cerr);
```

**Trace VM Execution:**

```bash
./zanna front yourfrontend -run --trace=il test.src
./zanna front yourfrontend -run --trace=src test.src
```

**Check Opcode Usage:**
Ensure you're using the right opcode for the operation:

- Signed arithmetic: `iadd.ovf`, `isub.ovf`, `imul.ovf`, `sdiv.chk0`, `srem.chk0` — the plain
  `add`/`sub`/`mul`/`sdiv`/`srem` opcodes still exist in `Opcode.def` for legacy/internal use but
  are rejected by the IL verifier on signed integer types.
- Unsigned division/remainder: `udiv.chk0`, `urem.chk0` (plain `udiv`/`urem` are similarly rejected).
- Floating-point: `fadd`, `fsub`, `fmul`, `fdiv`
- Signed comparison: `scmp_lt`, `scmp_le`, `scmp_gt`, `scmp_ge`
- Unsigned comparison: `ucmp_lt`, `ucmp_le`, `ucmp_gt`, `ucmp_ge`

---

## 16. Reference Materials

### Essential Files to Study

Study the BASIC frontend for patterns:

1. **src/frontends/basic/BasicCompiler.cpp** — Overall pipeline
2. **src/frontends/basic/Lexer.cpp** — Tokenization
3. **src/frontends/basic/Parser.cpp** — Recursive descent parsing
4. **src/frontends/basic/Parser_Expr.cpp** — Expression parsing
5. **src/frontends/basic/ast/ExprNodes.hpp** — AST design
6. **src/frontends/basic/SemanticAnalyzer.hpp** — Type checking
7. **src/frontends/basic/Lowerer.cpp** — IL lowering
8. **src/frontends/basic/Lowerer_Procedure.cpp** — Function lowering
9. **src/frontends/basic/LowerExpr.cpp** — Expression lowering

### IL System Files

10. **src/il/core/Module.hpp** — Module structure
11. **src/il/core/Function.hpp** — Function representation
12. **src/il/core/BasicBlock.hpp** — Block structure
13. **src/il/core/Instr.hpp** — Instruction representation
14. **src/il/core/Opcode.def** — All opcodes
15. **src/il/core/Type.hpp** — Type system
16. **src/il/core/Value.hpp** — Value representation
17. **src/il/build/IRBuilder.hpp** — IRBuilder API
18. **src/il/runtime/RuntimeSignatures.hpp** — Runtime ABI

### Testing Examples

19. **src/tests/unit/test_basic_*.cpp** — BASIC unit tests
20. **src/tests/golden/basic/** — Golden tests
21. **src/tests/zia/** — Zia integration tests

### Documentation

22. **docs/il/il-guide.md** — IL specification
23. **docs/il/il-guide.md#reference** — IL instruction reference
24. **docs/internals/architecture.md** — Zanna architecture
25. **docs/languages/basic-namespaces.md** — Namespace implementation

### Example Programs

26. **examples/embedding/combined.cpp** — Manual IL construction (embedded use)
27. **src/tests/golden/basic/*.bas** — BASIC examples

---

## Summary

This guide has covered everything needed to implement a new frontend:

1. ✅ **Build system integration** — CMakeLists.txt, linking, registration
2. ✅ **Lexer/Parser** — Tokenization, recursive descent, precedence climbing
3. ✅ **AST design** — Node hierarchy, visitor pattern, memory management
4. ✅ **Semantic analysis** — Symbol tables, type checking, validation
5. ✅ **IL lowering** — IRBuilder API, instruction emission, control flow
6. ✅ **Type system** — Language types → IL types, coercion
7. ✅ **Runtime ABI** — Available functions, calling conventions
8. ✅ **Diagnostics** — Error reporting, source locations
9. ✅ **Testing** — Unit tests, golden tests, E2E tests
10. ✅ **Debugging** — Common pitfalls, verification, tracing

**Next Steps:**

1. Study the minimal frontend example (Section 2)
2. Set up build system for your frontend (Section 3)
3. Implement lexer and parser (Section 5)
4. Design AST nodes (Section 6)
5. Implement semantic analysis (Section 7)
6. Implement IL lowering with IRBuilder (Section 8)
7. Add tests (Section 14)
8. Iterate and refine

**Key Principles:**

- **Start small** — Implement minimal subset first
- **Test incrementally** — Write tests for each feature
- **Follow patterns** — Study BASIC frontend for guidance
- **Verify early** — Run IL verifier after each change
- **Leverage runtime** — Use existing runtime functions

Good luck building your frontend!
