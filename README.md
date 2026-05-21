# DSS Code Prime

A **universal, configurable compiler** written in C++. Define any source language via JSON configuration, compile to any supported target platform — all through a single engine.

## Key Features

- **Any Input Language** — Programming languages are defined via JSON config files describing lexical rules, grammar (BNF), type system, and semantic constraints. No recompilation needed to add a language.
- **Any Target Platform** — Compile to Windows, Linux, macOS, iOS, Android, and Web (WASM) across x86_64, ARM64, and WASM architectures.
- **Classical Compiler Pipeline** — Three-phase analysis (Lexical → Syntactic → Semantic), intermediate representation with optimization passes, and pluggable target code emitters.
- **Cross-Platform** — Builds and runs natively on Windows, Linux, and macOS. Docker image provided for reproducible cross-compilation.

## Architecture

```
User Input (project / files / directory)
    │
    ▼
┌─────────────────────────────────────────────────┐
│  program        Public API & driver              │
│  source-factory Language config loader (JSON)    │
│  tokenizer      Characters → token stream        │
│  analysis       Lexical → Syntactic → Semantic   │
│  gen            IR generation → target emission   │
└─────────────────────────────────────────────────┘
    │
    ▼
Executables / Libraries / WASM modules
```

## Usage

The compiler exposes a **program API** with three input modes:

### Project File (`.dsp`)

A self-contained JSON project definition:

```jsonc
{
  "project": { "name": "MyApp", "version": "1.0.0" },
  "source": {
    "language": "ExampleLang",
    "include": ["src/"],
    "exclude": ["src/tests/"]
  },
  "targets": [
    { "os": "linux",   "arch": "x86_64" },
    { "os": "windows", "arch": "x86_64" },
    { "os": "web",     "arch": "wasm"   }
  ],
  "output": { "directory": "build/" }
}
```

```bash
dss-code-prime --project myapp.dsp
```

### File List

```bash
dss-code-prime --files src/main.exl src/utils.exl --lang ExampleLang --target linux-x86_64 --target web-wasm
```

### Directory Scan

```bash
dss-code-prime --dir ./src/ --lang ExampleLang --target linux-x86_64
```

The compiler scans the directory recursively for files matching the language's configured extensions.

## Defining a Language

Languages are defined in JSON config files under `src/source-config/languages/`. Each file fully describes a language's rules and syntax across three sections: **lexical**, **syntactic**, and **semantic**.

### Example Language Configuration

```jsonc
{
  "language": {
    "name": "ExampleLang",
    "version": "1.0.0",
    "fileExtensions": [".exl", ".example"]
  },

  // ── Lexical rules: how source text is broken into tokens ──
  "lexical": {
    "comments": {
      "singleLine": "//",
      "multiLineStart": "/*",
      "multiLineEnd": "*/"
    },
    "whitespace": {
      "significant": false,          // true for indentation-based languages (e.g. Python)
      "newlineSignificant": false
    },
    "literals": {
      "integer":  { "pattern": "[0-9]+", "suffixes": ["u", "l", "ul"] },
      "float":    { "pattern": "[0-9]+\\.[0-9]+", "suffixes": ["f", "d"] },
      "string":   { "delimiters": ["\""], "escapeChar": "\\", "multiline": false },
      "char":     { "delimiters": ["'"], "escapeChar": "\\" },
      "boolean":  { "trueKeyword": "true", "falseKeyword": "false" },
      "null":     { "keyword": "null" }
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
      { "symbol": "<",  "precedence": 7,  "associativity": "left",  "type": "binary" },
      { "symbol": ">",  "precedence": 7,  "associativity": "left",  "type": "binary" },
      { "symbol": "&&", "precedence": 3,  "associativity": "left",  "type": "binary" },
      { "symbol": "||", "precedence": 2,  "associativity": "left",  "type": "binary" },
      { "symbol": "!",  "precedence": 30, "associativity": "right", "type": "unary_prefix" },
      { "symbol": "++", "precedence": 30, "associativity": "right", "type": "unary_prefix" },
      { "symbol": "--", "precedence": 30, "associativity": "right", "type": "unary_prefix" }
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
      "startPattern": "[a-zA-Z_]",       // first character rules
      "continuePattern": "[a-zA-Z0-9_]", // subsequent characters
      "caseSensitive": true
    }
  },

  // ── Syntactic rules: BNF grammar defining the language structure ──
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
        { "name": "assignment",       "production": "logicOr ('=' assignment)?" },
        { "name": "logicOr",          "production": "logicAnd ('||' logicAnd)*" },
        { "name": "logicAnd",         "production": "equality ('&&' equality)*" },
        { "name": "equality",         "production": "comparison (('==' | '!=') comparison)*" },
        { "name": "comparison",       "production": "addition (('<' | '>' | '<=' | '>=') addition)*" },
        { "name": "addition",         "production": "multiplication (('+' | '-') multiplication)*" },
        { "name": "multiplication",   "production": "unary (('*' | '/') unary)*" },
        { "name": "unary",            "production": "('!' | '-' | '++' | '--') unary | primary" },
        { "name": "primary",          "production": "INTEGER | FLOAT | STRING | BOOLEAN | NULL | IDENTIFIER | '(' expression ')' | functionCall" },
        { "name": "functionCall",     "production": "IDENTIFIER '(' argList? ')'" },
        { "name": "argList",          "production": "expression (',' expression)*" },
        { "name": "type",             "production": "IDENTIFIER ('<' typeList '>')?" },
        { "name": "typeList",         "production": "type (',' type)*" }
      ]
    }
  },

  // ── Semantic rules: type system, scoping, and compile-time constraints ──
  "semantic": {
    "typeSystem": {
      "primitiveTypes": ["int", "float", "double", "string", "bool", "void"],
      "typeInference": true,
      "implicitConversions": [
        { "from": "int",   "to": "float" },
        { "from": "int",   "to": "double" },
        { "from": "float", "to": "double" }
      ]
    },
    "scoping": {
      "model": "block",          // "block" | "function" | "global"
      "allowShadowing": true,    // inner scope can redeclare outer names
      "hoisting": false          // true for JS-like var hoisting
    },
    "rules": [
      { "id": "VAR_DECL_BEFORE_USE",  "description": "Variables must be declared before use",               "severity": "error"   },
      { "id": "TYPE_MISMATCH",        "description": "Assignment type must match or be implicitly convertible", "severity": "error"   },
      { "id": "CONST_REASSIGNMENT",   "description": "Cannot reassign a const variable",                    "severity": "error"   },
      { "id": "RETURN_TYPE_MATCH",    "description": "Return value must match function return type",        "severity": "error"   },
      { "id": "UNUSED_VARIABLE",      "description": "Variable declared but never used",                    "severity": "warning" },
      { "id": "UNREACHABLE_CODE",     "description": "Code after return statement",                         "severity": "warning" }
    ]
  }
}
```

> **Tip:** To add a new language, create a `.lang.json` file following this structure and place it in `src/source-config/languages/`. The compiler picks it up by name — no recompilation required.

See `src/source-config/languages/example.lang.json` for the shipped reference implementation.

## Supported Targets

| Target | OS | Architecture | Output Format |
|---|---|---|---|
| `linux-x86_64` | Linux | x86_64 | ELF |
| `linux-arm64` | Linux | ARM64 | ELF |
| `windows-x86_64` | Windows | x86_64 | PE/COFF (.exe) |
| `macos-x86_64` | macOS | x86_64 | Mach-O |
| `macos-arm64` | macOS | ARM64 (Apple Silicon) | Mach-O |
| `ios-arm64` | iOS | ARM64 | Mach-O |
| `android-arm64` | Android | ARM64 | ELF |
| `web-wasm` | Web | WASM | WebAssembly (.wasm) |

## Project Structure

```
src/
├── program/          Public API — project, file list, or directory input
├── core/             Shared types (Token, AST, IR, Symbol) & utilities
├── source-config/    Language definition JSON files & schema
├── source-factory/   JSON config parser, models & validators
├── tokenizer/        Character stream → token stream
├── analysis/
│   ├── lexical/      Token validation & classification
│   ├── syntactic/    Parser → AST (recursive descent + Pratt)
│   └── semantic/     Symbol table, type checker, scope resolver
└── gen/
    ├── intermediate/ AST → IR, optimization passes
    └── link/         IR → target machine code, linking
```

## Building

### Prerequisites

- C++20 compatible compiler (GCC 12+, Clang 14+, MSVC 2022+)
- CMake 3.20+

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Docker

```bash
docker compose -f docker/docker-compose.yml up --build
```

### Run Tests

```bash
cmake --build build --target test
```

## Documentation

- [Implementation Plan](.plans/00-compiler-implementation-plan - tbd.md) — Full architecture, file structure, module docs, and JSON config spec
