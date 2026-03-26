# DSS Code Prime — Universal Compiler Implementation Plan

## 1. Vision & Overview

**DSS Code Prime** is a universal, configurable compiler written in C++. Its core design principle is that **both the source language and the target platform are configurable**, making it a single compiler engine capable of compiling _any_ defined language to _any_ supported target.

### Key Design Goals

| Goal | Description |
|---|---|
| **Language-Agnostic Input** | Any programming language can be defined via a JSON configuration file that describes its full syntax, grammar, type system, and semantics. |
| **Multi-Target Output** | Compile to Windows, Linux, macOS, iOS, Android, and Web (WASM) — each with their accessible processor architectures (x86_64, ARM64, RISC-V, WASM, etc.). |
| **Three-Phase Analysis** | Classical compiler pipeline: Lexical → Syntactic → Semantic analysis. |
| **Portable Build** | Runs natively on Windows, Linux, and macOS. Docker image provided for reproducible cross-compilation environments. |
| **Extensible Architecture** | New languages are added via config files; new targets via pluggable backend modules. |

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          DSS Code Prime                                  │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │                        program (Public API)                        │  │
│  │  Receives: project file | file list | directory path               │  │
│  │  Receives: source language name                                    │  │
│  │  Receives: target platform(s) — one or many                        │  │
│  │  Resolves source files by extension, dispatches compilation        │  │
│  └───────────────────────────┬────────────────────────────────────────┘  │
│                              │                                           │
│                              ▼                                           │
│  ┌──────────────┐   ┌───────────────────────┐   ┌───────────────┐       │
│  │ source-config │──▶│   source-factory      │──▶│  core/types   │       │
│  │  (JSON files) │   │ (parser + validator)  │   │  (in-memory   │       │
│  └──────────────┘   └───────────────────────┘   │   lang model) │       │
│                                                  └──────┬────────┘       │
│                                                         │                │
│  ┌──────────────────────────────────────────────────────┘                │
│  │                                                                       │
│  ▼                                                                       │
│  ┌────────────┐   ┌──────────────────────────────────┐                   │
│  │ tokenizer  │──▶│           analysis                │                   │
│  │ (char→tok) │   │  ┌─────────┬──────────┬────────┐ │                   │
│  └────────────┘   │  │ lexical │syntactic │semantic│ │                   │
│                    │  │ (rules  │(parser + │(types, │ │                   │
│                    │  │  check) │ AST)     │ scope) │ │                   │
│                    │  └─────────┴──────────┴────────┘ │                   │
│                    └──────────────┬───────────────────┘                   │
│                                  │                                       │
│                                  ▼                                       │
│                    ┌──────────────────────────────────┐                   │
│                    │             gen                   │                   │
│                    │  ┌──────────────┬──────────────┐ │                   │
│                    │  │ intermediate │    link       │ │                   │
│                    │  │ (IR gen +    │ (target emit  │ │                   │
│                    │  │  optimize)   │  + linking)   │ │                   │
│                    │  └──────────────┴──────────────┘ │                   │
│                    └──────────────────────────────────┘                   │
└──────────────────────────────────────────────────────────────────────────┘
```

### Data Flow Summary

```
User Input (via program API)
    │  ─ project file (.dsp), file list, or directory path
    │  ─ source language name (e.g. "ExampleLang")
    │  ─ target platform(s) (e.g. ["linux-x86_64", "web-wasm"])
    │
    ▼
[program] ── resolves input → discovers source files by extension
    │         loads language config via source-factory
    │         iterates targets, dispatches per-file compilation
    │
    ▼ (for each source file × each target)
[source-factory] ── loads language definition from JSON
    │
    ▼
[tokenizer] ── raw characters → token stream
    │
    ▼
[analysis/lexical] ── validates tokens against language rules
    │
    ▼
[analysis/syntactic] ── token stream → Abstract Syntax Tree (AST)
    │
    ▼
[analysis/semantic] ── type checking, scope resolution, semantic validation
    │
    ▼
[gen/intermediate] ── AST → Intermediate Representation (IR), IR optimization
    │
    ▼
[gen/link] ── IR → target-specific machine code, linking, output binary
    │
    ▼
Executable / Library / WASM module (per target)
```

---

## 3. Project File Structure

```
dss-code-prime/
│
├── .plans/
│   └── compiler-implementation-plan.md      # This document
│
├── CMakeLists.txt                           # Root CMake build (cross-platform)
├── README.md
├── .gitignore
│
├── docker/
│   ├── Dockerfile                           # Multi-stage build image
│   ├── docker-compose.yml                   # Dev/CI compose file
│   └── toolchains/                          # CMake toolchain files for cross-compile
│       ├── linux-x86_64.cmake
│       ├── linux-arm64.cmake
│       ├── windows-x86_64.cmake
│       ├── macos-x86_64.cmake
│       ├── macos-arm64.cmake
│       ├── ios-arm64.cmake
│       ├── android-arm64.cmake
│       └── web-wasm.cmake
│
├── docs/
│   ├── architecture.md                      # Detailed architecture documentation
│   ├── language-config-spec.md              # JSON config specification
│   └── target-config-spec.md                # Target platform specification
│
├── libs/                                    # Third-party dependencies
│   └── README.md                            # Dependency documentation
│
├── src/
│   ├── CMakeLists.txt                       # Src-level CMake
│   ├── main.cpp                             # CLI entry point (thin — delegates to program)
│   │
│   ├── program/                             # ── Public API / Driver ──
│   │   ├── CMakeLists.txt
│   │   ├── program.hpp                      # Public API: the open interface for compilation
│   │   ├── program.cpp                      # Orchestrates input resolution + multi-target compilation
│   │   ├── input_resolver.hpp               # Resolves project / file list / directory → source files
│   │   ├── input_resolver.cpp
│   │   ├── project_file.hpp                 # Parses .dsp project file (project definition)
│   │   ├── project_file.cpp
│   │   ├── compilation_request.hpp          # Request model: source lang, files, targets
│   │   └── compilation_result.hpp           # Result model: per-target output paths + diagnostics
│   │
│   ├── core/                                # ── Shared types & utilities ──
│   │   ├── CMakeLists.txt
│   │   ├── compiler.hpp                     # Top-level compiler orchestrator interface
│   │   ├── compiler.cpp                     # Orchestrates the full pipeline
│   │   ├── types/
│   │   │   ├── token.hpp                    # Token type (kind, value, location)
│   │   │   ├── ast.hpp                      # AST node definitions (base + variants)
│   │   │   ├── ir.hpp                       # Intermediate Representation node types
│   │   │   ├── symbol.hpp                   # Symbol table entry type
│   │   │   ├── source_location.hpp          # File/line/col position tracking
│   │   │   └── target_info.hpp              # Target OS + processor descriptor
│   │   ├── error/
│   │   │   ├── error.hpp                    # Error/Warning/Info diagnostic type
│   │   │   ├── error.cpp
│   │   │   ├── error_reporter.hpp           # Collects and formats diagnostics
│   │   │   └── error_reporter.cpp
│   │   └── utils/
│   │       ├── file_io.hpp                  # Cross-platform file reading utilities
│   │       ├── file_io.cpp
│   │       ├── string_utils.hpp             # String manipulation helpers
│   │       └── string_utils.cpp
│   │
│   ├── source-config/                       # ── Language Definition Files ──
│   │   ├── README.md                        # How to write a language config
│   │   ├── schemas/
│   │   │   └── language-schema.json         # JSON Schema for validation
│   │   └── languages/
│   │       └── example.lang.json            # Example language definition
│   │
│   ├── source-factory/                # ── Config Parser & Loader ──
│   │   ├── CMakeLists.txt
│   │   ├── config_reader.hpp                # Public API: load(path) → LanguageConfig
│   │   ├── config_reader.cpp                # JSON parsing + hydration into models
│   │   ├── models/
│   │   │   ├── language_config.hpp          # Root model: holds full language definition
│   │   │   ├── token_definition.hpp         # Defines a token kind (regex, keyword list, etc.)
│   │   │   ├── grammar_rule.hpp             # BNF/PEG-style production rule model
│   │   │   ├── type_system_config.hpp       # Primitive types, type rules, coercion
│   │   │   ├── operator_definition.hpp      # Operators: symbol, precedence, associativity
│   │   │   └── semantic_rule.hpp            # Semantic constraints (e.g. "variables must be declared")
│   │   └── validators/
│   │       ├── config_validator.hpp          # Validates loaded config for completeness/consistency
│   │       └── config_validator.cpp
│   │
│   ├── tokenizer/                           # ── Tokenization (char stream → tokens) ──
│   │   ├── CMakeLists.txt
│   │   ├── tokenizer.hpp                    # Public API: tokenize(source, lang_config) → TokenStream
│   │   ├── tokenizer.cpp                    # Core tokenization engine (driven by token definitions)
│   │   ├── token_stream.hpp                 # Iterable token container with peek/advance
│   │   ├── token_stream.cpp
│   │   ├── source_reader.hpp                # Buffered character reader with location tracking
│   │   └── source_reader.cpp
│   │
│   ├── analysis/                            # ── Three-Phase Analysis ──
│   │   ├── CMakeLists.txt
│   │   │
│   │   ├── lexical/                         # Phase 1: Lexical Analysis
│   │   │   ├── lexer.hpp                    # Public API: validates & classifies token stream
│   │   │   ├── lexer.cpp                    # Applies lexical rules from language config
│   │   │   ├── lexical_rules.hpp            # Rule engine: keyword matching, literal validation
│   │   │   └── lexical_rules.cpp
│   │   │
│   │   ├── syntactic/                       # Phase 2: Syntactic Analysis (Parsing)
│   │   │   ├── parser.hpp                   # Public API: parse(tokens, grammar) → AST
│   │   │   ├── parser.cpp                   # Recursive descent / table-driven parser
│   │   │   ├── grammar.hpp                  # Runtime grammar representation (from config)
│   │   │   ├── grammar.cpp                  # Grammar loading and first/follow set computation
│   │   │   ├── ast_builder.hpp              # Constructs AST nodes during parsing
│   │   │   └── ast_builder.cpp
│   │   │
│   │   └── semantic/                        # Phase 3: Semantic Analysis
│   │       ├── semantic_analyzer.hpp        # Public API: analyze(AST) → annotated AST
│   │       ├── semantic_analyzer.cpp        # Orchestrates all semantic passes
│   │       ├── symbol_table.hpp             # Scoped symbol table (variables, functions, types)
│   │       ├── symbol_table.cpp
│   │       ├── type_checker.hpp             # Type inference and checking engine
│   │       ├── type_checker.cpp
│   │       ├── scope_resolver.hpp           # Scope entry/exit, name resolution
│   │       └── scope_resolver.cpp
│   │
│   └── gen/                                 # ── Code Generation ──
│       ├── CMakeLists.txt
│       │
│       ├── intermediate/                    # IR Generation & Optimization
│       │   ├── ir_generator.hpp             # Public API: generate(AST) → IR
│       │   ├── ir_generator.cpp             # Walks annotated AST, emits IR nodes
│       │   ├── ir_node.hpp                  # IR instruction set (three-address code style)
│       │   ├── ir_optimizer.hpp             # Optimization passes (constant folding, DCE, etc.)
│       │   └── ir_optimizer.cpp
│       │
│       └── link/                            # Target Code Emission & Linking
│           ├── linker.hpp                   # Public API: link(IR, target) → output binary
│           ├── linker.cpp                   # Resolves symbols, invokes target emitter
│           ├── target_config.hpp            # Target descriptor (OS, arch, ABI)
│           ├── target_config.cpp
│           └── targets/                     # Per-target code emitters
│               ├── target_base.hpp          # Abstract base class for all targets
│               ├── target_windows_x86_64.hpp
│               ├── target_windows_x86_64.cpp
│               ├── target_linux_x86_64.hpp
│               ├── target_linux_x86_64.cpp
│               ├── target_linux_arm64.hpp
│               ├── target_linux_arm64.cpp
│               ├── target_macos_x86_64.hpp
│               ├── target_macos_x86_64.cpp
│               ├── target_macos_arm64.hpp
│               ├── target_macos_arm64.cpp
│               ├── target_ios_arm64.hpp
│               ├── target_ios_arm64.cpp
│               ├── target_android_arm64.hpp
│               ├── target_android_arm64.cpp
│               ├── target_web_wasm.hpp
│               └── target_web_wasm.cpp
│
└── tests/                                   # ── Test Suite (mirrors src/) ──
    ├── CMakeLists.txt
    ├── program/
    │   ├── test_program.cpp
    │   ├── test_input_resolver.cpp
    │   └── test_project_file.cpp
    ├── core/
    │   ├── test_error_reporter.cpp
    │   └── test_types.cpp
    ├── source-factory/
    │   ├── test_config_reader.cpp
    │   └── test_config_validator.cpp
    ├── tokenizer/
    │   ├── test_tokenizer.cpp
    │   ├── test_token_stream.cpp
    │   └── test_source_reader.cpp
    ├── analysis/
    │   ├── lexical/
    │   │   └── test_lexer.cpp
    │   ├── syntactic/
    │   │   ├── test_parser.cpp
    │   │   └── test_grammar.cpp
    │   └── semantic/
    │       ├── test_semantic_analyzer.cpp
    │       ├── test_symbol_table.cpp
    │       └── test_type_checker.cpp
    └── gen/
        ├── intermediate/
        │   ├── test_ir_generator.cpp
        │   └── test_ir_optimizer.cpp
        └── link/
            └── test_linker.cpp
```

---

## 4. Module Detailed Documentation

---

### 4.1 `program/` — Public API & Driver

The **program** module is the open API layer — the entry point that external consumers (CLI, IDE integrations, CI pipelines) use to invoke compilation. It handles **what** to compile and **where** to output, then delegates the **how** to the compiler pipeline.

#### 4.1.1 Input Modes

The API accepts three input modes:

| Mode | Description | Example |
|---|---|---|
| **Project file** | A `.dsp` (DSS Project) JSON file that declares source language, source directories/files, targets, and output configuration. | `program.compile("myapp.dsp")` |
| **File list** | An explicit list of source file paths, plus language name and target(s). | `program.compile(files, "ExampleLang", targets)` |
| **Directory** | A directory path — the program scans it recursively for files matching the language's `fileExtensions` from the config. | `program.compile("./src/", "ExampleLang", targets)` |

#### 4.1.2 `program.hpp/.cpp` — Public API

```cpp
class Program {
public:
    Program();
    ~Program();

    /// Compile from a .dsp project file (self-contained — language, files, targets defined inside).
    CompilationResult compileProject(const std::string& projectFilePath);

    /// Compile an explicit list of source files for the given language to the given target(s).
    CompilationResult compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<TargetInfo>& targets
    );

    /// Compile all matching source files found in a directory (recursive scan by extension).
    CompilationResult compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<TargetInfo>& targets
    );

private:
    /// Shared implementation: compile resolved files for each target.
    CompilationResult compileResolved(
        const std::vector<std::string>& resolvedFiles,
        const LanguageConfig& langConfig,
        const std::vector<TargetInfo>& targets
    );
};
```

**Behavior:**
- Loads the language config once (via `source-factory`), reuses it for all files
- For each target in the targets list, runs the full pipeline (tokenize → analyze → gen) per source file
- Collects per-file, per-target results into a single `CompilationResult`
- Thread-safe: files can be compiled in parallel (future optimization)

#### 4.1.3 `input_resolver.hpp/.cpp` — Input Resolution

```cpp
class InputResolver {
public:
    /// Resolve a directory to a list of source files matching the given extensions.
    static std::vector<std::string> resolveDirectory(
        const std::string& directoryPath,
        const std::vector<std::string>& fileExtensions,
        bool recursive = true
    );

    /// Validate that all files in a list exist and are readable.
    static std::vector<std::string> validateFiles(const std::vector<std::string>& filePaths);

    /// Resolve a mixed input (could be a file, directory, or glob pattern).
    static std::vector<std::string> resolve(
        const std::string& input,
        const std::vector<std::string>& fileExtensions
    );
};
```

**Responsibilities:**
- Recursively scans directories for files matching configured extensions (e.g. `.exl`, `.example`)
- Validates file existence and read permissions
- Supports glob patterns (e.g. `src/**/*.exl`)
- Returns sorted, deduplicated absolute paths
- Cross-platform (Windows `\` vs POSIX `/`)

#### 4.1.4 `project_file.hpp/.cpp` — Project File Parser

A `.dsp` (DSS Project) file is a JSON document that bundles all compilation parameters:

```jsonc
{
  "project": {
    "name": "MyApplication",
    "version": "1.0.0"
  },
  "source": {
    "language": "ExampleLang",
    "include": [
      "src/",
      "lib/helpers.exl"
    ],
    "exclude": [
      "src/tests/",
      "src/deprecated/"
    ]
  },
  "targets": [
    { "os": "linux",   "arch": "x86_64" },
    { "os": "windows", "arch": "x86_64" },
    { "os": "web",     "arch": "wasm"   }
  ],
  "output": {
    "directory": "build/",
    "nameTemplate": "${project.name}-${target.os}-${target.arch}"
  }
}
```

```cpp
class ProjectFile {
public:
    /// Parse a .dsp project file.
    static ProjectFile load(const std::string& projectFilePath);

    std::string projectName;
    std::string projectVersion;
    std::string languageName;
    std::vector<std::string> includePaths;   // files and/or directories
    std::vector<std::string> excludePaths;   // exclusion patterns
    std::vector<TargetInfo> targets;
    std::string outputDirectory;
    std::string nameTemplate;
};
```

#### 4.1.5 `compilation_request.hpp` — Request Model

```cpp
struct CompilationRequest {
    std::vector<std::string> sourceFiles;    // Resolved absolute paths
    std::string languageName;                // Language config to load
    std::vector<TargetInfo> targets;         // One or more targets
    std::string outputDirectory;             // Where to write outputs
    std::string nameTemplate;                // Output naming pattern (optional)
};
```

#### 4.1.6 `compilation_result.hpp` — Result Model

```cpp
struct FileTargetResult {
    std::string sourceFile;
    TargetInfo target;
    bool success;
    std::string outputPath;                  // Path to generated binary (on success)
    std::vector<Error> diagnostics;          // Errors, warnings, info for this file+target
};

struct CompilationResult {
    bool overallSuccess;                     // True if all files × all targets succeeded
    std::vector<FileTargetResult> results;   // Per-file, per-target results
    std::vector<Error> globalDiagnostics;    // Diagnostics not tied to a specific file

    /// Convenience: get all results for a specific target.
    std::vector<FileTargetResult> resultsForTarget(const TargetInfo& target) const;

    /// Convenience: get all failed results.
    std::vector<FileTargetResult> failures() const;
};
```

---

### 4.2 `core/` — Shared Types & Utilities

The **core** module contains types and utilities shared across every stage of the compiler. Nothing in `core/` depends on any other `src/` module — it is the foundation layer.

#### 4.2.1 `core/compiler.hpp/.cpp` — Compiler Orchestrator

The top-level class that wires the entire pipeline together:

```cpp
class Compiler {
public:
    /// Construct with a language config path and target specification.
    Compiler(const std::string& langConfigPath, const TargetInfo& target);

    /// Compile a source file, returning diagnostics and (on success) the output path.
    CompileResult compile(const std::string& sourceFilePath);

private:
    std::unique_ptr<LanguageConfig> langConfig_;
    TargetInfo target_;
    ErrorReporter reporter_;
};
```

**Responsibilities:**
- Loads the language configuration via `source-factory`
- Orchestrates: Tokenize → Lex → Parse → Semantic → IR Gen → Optimize → Link
- Collects and returns diagnostics from all phases

#### 4.2.2 `core/types/` — Fundamental Data Types

| File | Purpose |
|---|---|
| `token.hpp` | `Token` struct: `{ TokenKind kind; std::string lexeme; SourceLocation loc; }` — the universal token representation used from tokenizer through parser. |
| `ast.hpp` | AST node hierarchy. Base `ASTNode` with variants: `LiteralNode`, `BinaryExprNode`, `UnaryExprNode`, `IdentifierNode`, `FunctionDeclNode`, `BlockNode`, `IfNode`, `WhileNode`, `ReturnNode`, etc. Uses a visitor pattern for traversal. |
| `ir.hpp` | IR instruction types: `IRInstruction` base with `IRBinaryOp`, `IRUnaryOp`, `IRLoad`, `IRStore`, `IRCall`, `IRBranch`, `IRLabel`, `IRReturn`, etc. Three-address code style. |
| `symbol.hpp` | `Symbol` struct: `{ std::string name; TypeInfo type; ScopeLevel scope; SourceLocation declLoc; bool isInitialized; }` |
| `source_location.hpp` | `SourceLocation` struct: `{ std::string filePath; size_t line; size_t column; }` — used in every diagnostic message. |
| `target_info.hpp` | `TargetInfo` struct: `{ TargetOS os; TargetArch arch; std::string abi; }` — describes the compilation target. |

#### 4.2.3 `core/error/` — Diagnostic System

A centralized error reporting system used by every compiler phase:

- **`Error`**: Holds severity (Error/Warning/Info/Hint), message, source location, and an error code.
- **`ErrorReporter`**: Accumulates diagnostics, supports formatted output, can abort on first error or collect all.

#### 4.2.4 `core/utils/` — Cross-Platform Utilities

- **`file_io`**: Read file to string, check existence, resolve paths (abstracts Windows/POSIX differences).
- **`string_utils`**: UTF-8 aware helpers, escape sequences, string trimming.

---

### 4.3 `source-config/` — Language Definition Files (JSON)

This directory contains the **JSON configuration files** that define programming languages. Each file fully describes a language's lexical, syntactic, and semantic rules — the compiler engine reads these at startup.

#### 4.3.1 JSON Schema Structure (`schemas/language-schema.json`)

The schema defines the expected shape of every language config file. Top-level sections:

```jsonc
{
  "$schema": "language-schema.json",
  "language": {
    "name": "ExampleLang",
    "version": "1.0.0",
    "fileExtensions": [".exl", ".example"]
  },

  "lexical": {
    "comments": {
      "singleLine": "//",
      "multiLineStart": "/*",
      "multiLineEnd": "*/"
    },
    "whitespace": {
      "significant": false,
      "newlineSignificant": false
    },
    "literals": {
      "integer": { "pattern": "[0-9]+", "suffixes": ["u", "l", "ul"] },
      "float": { "pattern": "[0-9]+\\.[0-9]+", "suffixes": ["f", "d"] },
      "string": { "delimiters": ["\""], "escapeChar": "\\", "multiline": false },
      "char": { "delimiters": ["'"], "escapeChar": "\\" },
      "boolean": { "trueKeyword": "true", "falseKeyword": "false" },
      "null": { "keyword": "null" }
    },
    "keywords": [
      "if", "else", "while", "for", "return", "function",
      "var", "const", "class", "import", "export"
    ],
    "operators": [
      { "symbol": "+",  "precedence": 10, "associativity": "left",  "type": "binary" },
      { "symbol": "-",  "precedence": 10, "associativity": "left",  "type": "binary" },
      { "symbol": "*",  "precedence": 20, "associativity": "left",  "type": "binary" },
      { "symbol": "/",  "precedence": 20, "associativity": "left",  "type": "binary" },
      { "symbol": "=",  "precedence": 1,  "associativity": "right", "type": "binary" },
      { "symbol": "==", "precedence": 5,  "associativity": "left",  "type": "binary" },
      { "symbol": "!=", "precedence": 5,  "associativity": "left",  "type": "binary" },
      { "symbol": "!",  "precedence": 30, "associativity": "right", "type": "unary_prefix" },
      { "symbol": "++", "precedence": 30, "associativity": "right", "type": "unary_prefix" }
    ],
    "delimiters": {
      "statementTerminator": ";",
      "blockStart": "{",
      "blockEnd": "}",
      "parenStart": "(",
      "parenEnd": ")",
      "bracketStart": "[",
      "bracketEnd": "]",
      "separator": ","
    },
    "identifiers": {
      "startPattern": "[a-zA-Z_]",
      "continuePattern": "[a-zA-Z0-9_]",
      "caseSensitive": true
    }
  },

  "syntactic": {
    "grammar": {
      "format": "BNF",
      "rules": [
        { "name": "program",          "production": "statement*" },
        { "name": "statement",        "production": "variableDecl | functionDecl | expressionStmt | ifStmt | whileStmt | returnStmt | block" },
        { "name": "variableDecl",     "production": "('var' | 'const') IDENTIFIER (':' type)? ('=' expression)? ';'" },
        { "name": "functionDecl",     "production": "'function' IDENTIFIER '(' paramList? ')' (':' type)? block" },
        { "name": "paramList",        "production": "param (',' param)*" },
        { "name": "param",            "production": "IDENTIFIER ':' type" },
        { "name": "block",            "production": "'{' statement* '}'" },
        { "name": "ifStmt",           "production": "'if' '(' expression ')' block ('else' (ifStmt | block))?" },
        { "name": "whileStmt",        "production": "'while' '(' expression ')' block" },
        { "name": "returnStmt",       "production": "'return' expression? ';'" },
        { "name": "expressionStmt",   "production": "expression ';'" },
        { "name": "expression",       "production": "assignment" },
        { "name": "assignment",       "production": "equality ('=' assignment)?" },
        { "name": "equality",         "production": "comparison (('==' | '!=') comparison)*" },
        { "name": "comparison",       "production": "addition (('<' | '>' | '<=' | '>=') addition)*" },
        { "name": "addition",         "production": "multiplication (('+' | '-') multiplication)*" },
        { "name": "multiplication",   "production": "unary (('*' | '/') unary)*" },
        { "name": "unary",            "production": "('!' | '-' | '++') unary | primary" },
        { "name": "primary",          "production": "INTEGER | FLOAT | STRING | BOOLEAN | NULL | IDENTIFIER | '(' expression ')' | functionCall" },
        { "name": "functionCall",     "production": "IDENTIFIER '(' argList? ')'" },
        { "name": "argList",          "production": "expression (',' expression)*" },
        { "name": "type",             "production": "IDENTIFIER ('<' typeList '>')?" },
        { "name": "typeList",         "production": "type (',' type)*" }
      ]
    }
  },

  "semantic": {
    "typeSystem": {
      "primitiveTypes": ["int", "float", "double", "string", "bool", "void"],
      "typeInference": true,
      "implicitConversions": [
        { "from": "int", "to": "float" },
        { "from": "int", "to": "double" },
        { "from": "float", "to": "double" }
      ]
    },
    "scoping": {
      "model": "block",
      "allowShadowing": true,
      "hoisting": false
    },
    "rules": [
      { "id": "VAR_DECL_BEFORE_USE",     "description": "Variables must be declared before use", "severity": "error" },
      { "id": "TYPE_MISMATCH",            "description": "Assignment type must match or be implicitly convertible", "severity": "error" },
      { "id": "CONST_REASSIGNMENT",       "description": "Cannot reassign a const variable", "severity": "error" },
      { "id": "RETURN_TYPE_MATCH",        "description": "Return value must match function return type", "severity": "error" },
      { "id": "UNUSED_VARIABLE",          "description": "Variable declared but never used", "severity": "warning" },
      { "id": "UNREACHABLE_CODE",         "description": "Code after return statement", "severity": "warning" }
    ]
  }
}
```

#### 4.3.2 `languages/` — Shipped Language Definitions

Initially ships with `example.lang.json` as a reference implementation. Future language configs (e.g., a C-subset, a scripting language) can be added here.

---

### 4.4 `source-factory/` — Config Parser & Loader

Reads a `.lang.json` file and hydrates it into a strongly-typed C++ object model.

#### 4.4.1 `config_reader.hpp/.cpp`

```cpp
class ConfigReader {
public:
    /// Load and parse a language config file. Throws on I/O or parse error.
    static std::unique_ptr<LanguageConfig> load(const std::string& configFilePath);

private:
    static void parseLexicalSection(const json& j, LanguageConfig& config);
    static void parseSyntacticSection(const json& j, LanguageConfig& config);
    static void parseSemanticSection(const json& j, LanguageConfig& config);
};
```

**JSON library:** [nlohmann/json](https://github.com/nlohmann/json) (header-only, MIT license).

#### 4.4.2 `models/` — In-Memory Language Model

| File | Class | Key Fields |
|---|---|---|
| `language_config.hpp` | `LanguageConfig` | `name`, `version`, `fileExtensions`, `lexical`, `syntactic`, `semantic` — root container. |
| `token_definition.hpp` | `TokenDefinition` | `kind` (keyword, operator, literal, etc.), `pattern` (regex), `value` (exact string). |
| `grammar_rule.hpp` | `GrammarRule` | `name`, `production` (string), `alternatives` (parsed list of symbol sequences). |
| `type_system_config.hpp` | `TypeSystemConfig` | `primitiveTypes`, `typeInference` flag, `implicitConversions` list. |
| `operator_definition.hpp` | `OperatorDefinition` | `symbol`, `precedence`, `associativity`, `type` (binary/unary_prefix/unary_postfix). |
| `semantic_rule.hpp` | `SemanticRule` | `id`, `description`, `severity` (error/warning). |

#### 4.4.3 `validators/` — Config Validation

- **`ConfigValidator`**: Checks that the loaded config is internally consistent:
  - All grammar non-terminals are defined
  - Operator precedences don't conflict
  - Keywords don't collide with identifier patterns
  - Semantic rules reference valid types
  - All required sections are present

---

### 4.5 `tokenizer/` — Character Stream to Token Stream

The tokenizer is the **first stage** of the pipeline. It reads raw source characters and emits a stream of `Token` objects based on the loaded language config's lexical definitions.

#### 4.5.1 `tokenizer.hpp/.cpp`

```cpp
class Tokenizer {
public:
    /// Construct tokenizer for a given language config.
    explicit Tokenizer(const LanguageConfig& config);

    /// Tokenize an entire source file into a token stream.
    TokenStream tokenize(const std::string& sourceFilePath);

    /// Tokenize from a string (for testing / REPL).
    TokenStream tokenizeString(const std::string& source, const std::string& fileName = "<string>");

private:
    const LanguageConfig& config_;

    Token nextToken(SourceReader& reader);
    Token readStringLiteral(SourceReader& reader);
    Token readNumberLiteral(SourceReader& reader);
    Token readIdentifierOrKeyword(SourceReader& reader);
    Token readOperatorOrDelimiter(SourceReader& reader);
    void skipWhitespaceAndComments(SourceReader& reader);
};
```

**How it works:**
1. Uses `SourceReader` to consume characters one at a time
2. Matches characters against the language config's patterns (longest match wins)
3. Differentiates keywords from identifiers by checking the keyword list
4. Handles string escapes, multi-line strings, nested comments per config
5. Tracks source location for every token

#### 4.5.2 `token_stream.hpp/.cpp`

```cpp
class TokenStream {
public:
    const Token& peek() const;           // Look at current token
    const Token& peekAhead(size_t n);    // Look ahead n tokens
    Token advance();                      // Consume and return current token
    bool match(TokenKind kind);           // Consume if matches, return true/false
    Token expect(TokenKind kind);         // Consume if matches, else error
    bool isAtEnd() const;
    SourceLocation currentLocation() const;

private:
    std::vector<Token> tokens_;
    size_t position_ = 0;
};
```

#### 4.5.3 `source_reader.hpp/.cpp`

Buffered character reader with:
- `peek()` / `advance()` / `isAtEnd()` interface
- Automatic line/column tracking
- UTF-8 aware character consumption

---

### 4.6 `analysis/` — Three-Phase Analysis

---

#### 4.6.1 `analysis/lexical/` — Phase 1: Lexical Analysis

While the tokenizer splits characters into tokens, the **lexical analyzer** validates and enriches them using language-specific rules.

```cpp
class Lexer {
public:
    explicit Lexer(const LanguageConfig& config, ErrorReporter& reporter);

    /// Validate and enrich the token stream. Returns validated stream.
    TokenStream analyze(TokenStream&& rawTokens);

private:
    LexicalRules rules_;
    ErrorReporter& reporter_;
};
```

**Responsibilities:**
- Validates that all tokens match allowed patterns (rejects malformed literals, unknown symbols)
- Classifies ambiguous tokens (e.g., `-` as unary vs binary based on context)
- Validates string escape sequences
- Validates numeric literal suffixes and ranges
- Reports lexical errors with precise source locations

**`LexicalRules`** — Engine that compiles the language config's lexical section into efficient matchers (regex or DFA-based).

---

#### 4.6.2 `analysis/syntactic/` — Phase 2: Syntactic Analysis (Parsing)

Transforms the validated token stream into an **Abstract Syntax Tree (AST)**.

```cpp
class Parser {
public:
    Parser(const LanguageConfig& config, ErrorReporter& reporter);

    /// Parse token stream into an AST.
    std::unique_ptr<ASTNode> parse(TokenStream& tokens);

private:
    Grammar grammar_;
    ASTBuilder builder_;
    ErrorReporter& reporter_;

    // Recursive descent methods (generated from grammar rules)
    std::unique_ptr<ASTNode> parseRule(const std::string& ruleName, TokenStream& tokens);
    std::unique_ptr<ASTNode> parseExpression(TokenStream& tokens, int minPrecedence);
};
```

**How it works:**
- The `Grammar` class loads the BNF rules from the language config and computes FIRST/FOLLOW sets
- The parser uses **recursive descent** for statement-level constructs
- Uses **Pratt parsing** (precedence climbing) for expressions, driven by operator definitions from the config
- Error recovery via synchronization tokens (e.g., skip to next `;` or `}`)

**`ASTBuilder`** — Factory for creating AST nodes. Attaches source locations and parent pointers.

**`Grammar`** — Runtime grammar representation:
- Parses BNF production strings into structured alternatives
- Computes FIRST and FOLLOW sets for conflict detection
- Validates grammar is unambiguous (or flags ambiguities for Pratt resolution)

---

#### 4.6.3 `analysis/semantic/` — Phase 3: Semantic Analysis

Validates the AST for **meaning** — type correctness, scope rules, and language constraints.

```cpp
class SemanticAnalyzer {
public:
    SemanticAnalyzer(const LanguageConfig& config, ErrorReporter& reporter);

    /// Perform all semantic passes on the AST. Annotates AST in-place.
    void analyze(ASTNode& root);

private:
    SymbolTable symbolTable_;
    TypeChecker typeChecker_;
    ScopeResolver scopeResolver_;
    ErrorReporter& reporter_;

    void resolveNames(ASTNode& node);    // Pass 1: Name resolution
    void checkTypes(ASTNode& node);      // Pass 2: Type checking
    void checkSemanticRules(ASTNode& node); // Pass 3: Custom rules from config
};
```

**Sub-components:**

| Component | Purpose |
|---|---|
| **`SymbolTable`** | Scoped hash map of symbols. Supports nested scopes (push/pop). Stores variable types, function signatures, class definitions. |
| **`TypeChecker`** | Infers expression types bottom-up. Validates assignments, function calls, returns. Applies implicit conversions from config. |
| **`ScopeResolver`** | Manages scope stack. Resolves identifiers to their declarations. Enforces scoping model (block/function/global) per config. Detects use-before-declare. |

**Semantic passes (in order):**
1. **Name Resolution** — Walk AST, register declarations in symbol table, resolve references
2. **Type Checking** — Infer and check types for all expressions and statements
3. **Custom Rule Checking** — Apply semantic rules from the config (unused variables, unreachable code, const enforcement)

---

### 4.7 `gen/` — Code Generation

---

#### 4.7.1 `gen/intermediate/` — IR Generation & Optimization

Transforms the semantically-validated AST into a **target-independent Intermediate Representation**.

```cpp
class IRGenerator {
public:
    explicit IRGenerator(ErrorReporter& reporter);

    /// Generate IR from a semantically-annotated AST.
    IRProgram generate(const ASTNode& root);

private:
    std::vector<IRInstruction> instructions_;
    size_t tempCounter_ = 0;
    size_t labelCounter_ = 0;

    IRValue generateExpression(const ASTNode& expr);
    void generateStatement(const ASTNode& stmt);
    std::string newTemp();     // Generate unique temporary variable
    std::string newLabel();    // Generate unique label
};
```

**IR Design** — Three-address code with the following instructions:
- `BINARY_OP dest, left, op, right` — Arithmetic/logic operations
- `UNARY_OP dest, op, operand` — Unary operations
- `LOAD dest, source` — Load variable value
- `STORE dest, source` — Store to variable
- `CALL dest, function, args...` — Function call
- `PARAM value` — Push function parameter
- `LABEL name` — Label for jumps
- `JUMP label` — Unconditional jump
- `JUMP_IF cond, label` — Conditional jump
- `JUMP_IF_NOT cond, label` — Conditional jump (negated)
- `RETURN value?` — Function return
- `FUNC_BEGIN name` / `FUNC_END` — Function boundaries
- `ALLOC dest, type` — Allocate stack space

**`IROptimizer`** — Applies target-independent optimizations:
- Constant folding (evaluate compile-time expressions)
- Constant propagation
- Dead code elimination (remove unreachable instructions)
- Common subexpression elimination
- Copy propagation

```cpp
class IROptimizer {
public:
    /// Apply all optimization passes to the IR program.
    void optimize(IRProgram& program);

    /// Apply a specific optimization pass.
    void constantFolding(IRProgram& program);
    void deadCodeElimination(IRProgram& program);
    void copyPropagation(IRProgram& program);
};
```

---

#### 4.7.2 `gen/link/` — Target-Specific Emission & Linking

Transforms the optimized IR into **target-specific machine code** and produces the final output (executable, library, or WASM module).

```cpp
class Linker {
public:
    Linker(const TargetInfo& target, ErrorReporter& reporter);

    /// Emit target code and link into final output.
    LinkResult link(const IRProgram& program, const std::string& outputPath);

private:
    std::unique_ptr<TargetBase> target_;
    ErrorReporter& reporter_;
};
```

**`TargetBase`** — Abstract base class for all target emitters:

```cpp
class TargetBase {
public:
    virtual ~TargetBase() = default;

    /// Emit target-specific code from IR.
    virtual std::vector<uint8_t> emit(const IRProgram& program) = 0;

    /// Get the file extension for this target's output.
    virtual std::string outputExtension() const = 0;

    /// Get the target's pointer size in bytes.
    virtual size_t pointerSize() const = 0;

    /// Get the target's endianness.
    virtual Endianness endianness() const = 0;
};
```

**Supported Targets:**

| Target Class | OS | Architecture | Output Format |
|---|---|---|---|
| `TargetWindowsX86_64` | Windows | x86_64 | PE/COFF (.exe) |
| `TargetLinuxX86_64` | Linux | x86_64 | ELF |
| `TargetLinuxARM64` | Linux | ARM64 | ELF |
| `TargetMacOSX86_64` | macOS | x86_64 | Mach-O |
| `TargetMacOSARM64` | macOS | ARM64 (Apple Silicon) | Mach-O |
| `TargetIOSARM64` | iOS | ARM64 | Mach-O |
| `TargetAndroidARM64` | Android | ARM64 | ELF (Android NDK) |
| `TargetWebWASM` | Web | WASM | WebAssembly (.wasm) |

**`TargetConfig`** — Maps `TargetInfo` (OS + arch) to the correct `TargetBase` subclass. Factory pattern.

---

## 5. Docker Configuration

### 5.1 `docker/Dockerfile`

Multi-stage image providing:
- **Build stage**: GCC/Clang, CMake, Ninja, nlohmann-json headers
- **Cross-compile stage**: Target toolchains (MinGW for Windows, Android NDK, Emscripten for WASM, osxcross for macOS)
- **Runtime stage**: Minimal image with just the compiler binary

### 5.2 `docker/toolchains/`

CMake toolchain files for each cross-compilation target. Each file sets:
- `CMAKE_SYSTEM_NAME` / `CMAKE_SYSTEM_PROCESSOR`
- Compiler paths (`CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`)
- Sysroot paths
- ABI flags

---

## 6. Build System

**CMake** is the build system, with the following structure:

- Root `CMakeLists.txt` sets C++20 standard, detects platform, includes subdirectories
- Each `src/` subdirectory has its own `CMakeLists.txt` producing a static library
- Libraries link: `core` ← `source-factory` ← `tokenizer` ← `analysis` ← `gen` ← `program`
- `main.cpp` links `program` (which transitively pulls everything) into the `dss-code-prime` executable
- `tests/CMakeLists.txt` uses **Google Test** (fetched via CMake `FetchContent`)

---

## 7. Third-Party Dependencies

| Library | Purpose | License |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing for language configs | MIT |
| [Google Test](https://github.com/google/googletest) | Unit testing framework | BSD-3 |
| [fmt](https://github.com/fmtlib/fmt) | String formatting (optional, or use C++20 `std::format`) | MIT |
| [spdlog](https://github.com/gabime/spdlog) | Logging (optional) | MIT |

---

## 8. Implementation Phases (Todos)

| # | ID | Title | Dependencies | Description |
|---|---|---|---|---|
| 1 | `scaffold-project` | Scaffold project & build system | — | Create CMakeLists.txt files, directory structure, Docker setup, and verify the project builds as an empty shell on all platforms. |
| 2 | `core-types` | Implement core types | scaffold-project | Implement Token, ASTNode hierarchy, IR types, Symbol, SourceLocation, TargetInfo, Error, ErrorReporter, file I/O utils. |
| 3 | `source-config-schema` | Design language config schema | scaffold-project | Finalize and document the JSON schema. Create the example language config file. |
| 4 | `source-factory` | Implement source factory | core-types, source-config-schema | Implement ConfigReader, all model classes, and ConfigValidator. Parse JSON into the in-memory model. |
| 5 | `tokenizer` | Implement tokenizer | core-types, source-factory | Implement Tokenizer, TokenStream, SourceReader. Tokenize source files based on loaded language config. |
| 6 | `analysis-lexical` | Implement lexical analysis | tokenizer | Implement Lexer and LexicalRules. Validate and classify the raw token stream. |
| 7 | `analysis-syntactic` | Implement syntactic analysis | analysis-lexical | Implement Parser, Grammar, ASTBuilder. Parse tokens into AST using grammar from config. |
| 8 | `analysis-semantic` | Implement semantic analysis | analysis-syntactic | Implement SemanticAnalyzer, SymbolTable, TypeChecker, ScopeResolver. Validate AST semantics. |
| 9 | `gen-intermediate` | Implement IR generation | analysis-semantic | Implement IRGenerator and IROptimizer. Transform AST to IR and apply optimizations. |
| 10 | `gen-link` | Implement code emission & linking | gen-intermediate | Implement Linker, TargetBase, and initial targets (Linux x86_64 as first). Emit machine code and produce binaries. |
| 11 | `program-api` | Implement program API & driver | core-types, gen-link | Implement Program (public API), InputResolver (directory scan, file list, glob), ProjectFile (.dsp parser), CompilationRequest/Result models. Multi-target dispatch. |
| 12 | `targets-expand` | Expand target support | gen-link | Implement remaining targets: Windows, macOS, iOS, Android, Web/WASM. |
| 13 | `testing` | Comprehensive test suite | all above | Write unit tests for every module. Integration tests compiling example programs end-to-end. |
| 14 | `docker-setup` | Docker & CI setup | scaffold-project | Finalize Dockerfile, compose, toolchain files. Set up CI/CD pipeline for automated builds and tests. |

---

## 9. Open Questions & Notes

- **Grammar format**: BNF chosen for readability; PEG is an alternative if ambiguity becomes an issue. To be decided per-language config.
- **IR level**: Starting with a simple three-address code IR. May evolve to SSA form for better optimization.
- **Register allocation**: Deferred to target emitters. Initial approach: naive stack-based allocation, optimize later.
- **Standard library**: Languages may need runtime support (memory allocation, I/O). This is out of scope for v1 — will be addressed as a separate "runtime" module.
- **LLVM backend**: An alternative to hand-written target emitters. Could be added as an optional backend in future.
- **Incremental compilation**: Not in v1 scope. Full recompilation per invocation.
- **Error recovery**: Parser will implement panic-mode recovery (sync to statement boundaries). More sophisticated recovery later.
