# DSS Code Prime

A **universal, configurable compiler** written in C++. Define any source language via JSON configuration, compile to any target ISA via JSON configuration — all through a single engine.

> **Status** — Frontend (lexer / parser / semantic / HIR) is complete. MIR + LIR closed end-to-end (HIR→MIR lowering, register allocation, calling-convention lowering, full IR text round-trip). **In-tree assembler complete end-to-end** (AS1–AS6 landed 2026-05-29) — x86_64 + ARM64 byte encoding via shape-keyed walkers, round-trip oracle disassembler, relocation taxonomy, source-map stamping. **In-tree linker complete end-to-end** (LK1–LK10 landed 2026-05-30) — `ObjectFormatSchema` + format-blind engine + per-format writers for ELF / PE / Mach-O / WASM (skeleton) / SPIR-V (skeleton), executable image paths (ET_EXEC / .exe / MH_EXECUTE), dynamic linking (PE IAT / ELF GOT+PLT / Mach-O LC_DYLD_INFO_ONLY), codesign placeholders (LK7), file emission (`linker::writeImage`) + driver pipeline wiring (`Program::compileFiles` / `compileDirectory`). See `.plans/00-compiler-implementation-plan - tbd.md` for the live status snapshot.

## Key Features

- **Any Input Language** — Languages are defined via `.lang.json` configs (lexer / parser grammar / semantics / HIR-lowering / imports). Same engine, no per-language C++ branches. Shipped reference configs: c-subset, tsql-subset, toy.
- **Any Target ISA** — Compile targets are `.target.json` configs (opcode set, register file, calling conventions, terminator kinds, encoding shapes, relocation taxonomy). Same engine, no per-target C++ branches. **x86_64 + ARM64 both ship** end-to-end through the assembler (byte encoding + round-trip oracle).
- **Three-tier IR** — HIR (language-neutral, typed) → MIR (SSA over CFG with structured-CF markers) → LIR (per-target, post-regalloc). Each tier has its own arena substrate, verifier, and round-trippable text format (`.dsshir` / `.dssir` / `.dsslir`).
- **Hermetic toolchain** — Own every byte from source to binary. No GAS / MASM / llvm-mc / ld / lld invocation. In-tree assembler (plan 13 ✅) + in-tree linker (plan 14 LK1–LK10 ✅) both complete end-to-end. Driver pipeline (`Program::compileFiles` / `compileDirectory`) routes source → HIR → MIR → LIR → ASM → link → on-disk artifact through the unified config-driven substrate.
- **Cross-Platform** — Builds natively on Windows, Linux, macOS. Docker image for reproducible builds.

## Architecture

```
User Input (project / files / directory)
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│  program        Public API & driver pipeline             │
│  dss-config     Language + target + format JSON configs  │
│  tokenizer      Characters → token stream                 │
│  analysis       Lexical → Syntactic → Semantic            │
│  hir            High-level IR (typed, language-neutral)   │
│  mir            Mid-level IR (SSA over CFG)               │
│  lir            Low-level IR (per-target, JSON-driven)    │
│  asm            In-tree assembler — byte encoding         │
│  link           In-tree linker — object format writers    │
└──────────────────────────────────────────────────────────┘
    │
    ▼
Executables / Libraries / WASM modules
```

The pipeline is **fully config-driven** end-to-end: the engine has zero per-language C++ branches. A `.lang.json` declares lexer/grammar/semantics/HIR-lowering, and a `.target.json` declares opcode/register/calling-convention vocabulary. Adding a new source language OR target ISA is a config-file drop, not an engine change.

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

Languages are defined in JSON config files under `src/dss-config/sources/`. Each file describes a language's rules across four top-level blocks: `tokens` (lexer), `keywords` / `scopes` / `shapes` (parser grammar v2), `semantics` (symbol table + type system, schema v4), `hirLowering` (CST→HIR projection), and optional `imports` for cross-file resolution.

Shipped reference configs:

- `src/dss-config/sources/c-subset.lang.json` — a substantial C subset (declarations, control flow, pointers, structs/unions/enums, designated initializers, `#include`)
- `src/dss-config/sources/tsql-subset.lang.json` — T-SQL DDL + DML
- `src/dss-config/sources/toy.lang.json` — a small typed expression language used as a genericity oracle

The current schema spec lives in `docs/language-config-spec.md` (config format v4). The example below is a **simplified illustration of the v1 design intent** — the shipped schema is more compact and uses different block names; read the spec + a shipped config for the real shape.

### Example Language Configuration (v1 design — illustrative only)

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

> **Tip:** To add a new language, drop a `.lang.json` file under `src/dss-config/sources/` and the compiler picks it up by name — no recompilation required. Same discipline for adding a new target ISA: drop a `.target.json` under `src/dss-config/targets/`.

## Supported Targets

Targets are JSON-configured (`src/dss-config/targets/*.target.json`). The substrate is fully target-blind: opcode dispatch, register names, calling conventions, terminator shapes — all read from the target schema. Adding a new ISA is a config-file drop, zero C++.

| Target | OS / Arch | Status |
|---|---|---|
| `x86_64` | Linux / Windows / macOS × x86_64 | Shipped — full opcode set + SysV AMD64 + Microsoft x64 calling conventions + byte encoding (`x86-variable` walker) + round-trip oracle |
| `arm64` | Linux / Windows / macOS / iOS / Android × ARM64 | Shipped — AAPCS64 + binary ops + byte encoding (`fixed32` walker) + round-trip oracle. MS-ARM64 calling convention deferred (D-AS3-5) |
| `wasm` | Web | Reserved — plan 18; consumes MIR with structured-CF markers |
| Object formats (ELF / PE / Mach-O / WASM / SPIR-V) | per target | ✅ Shipped — `ObjectFormatSchema` + format-blind linker engine + per-format writers (`elf.cpp` / `pe.cpp` / `macho.cpp` / `wasm.cpp` skeleton / `spirv.cpp` skeleton). Executable image paths (ET_EXEC, PE EXE, Mach-O MH_EXECUTE); dynamic linking (PE IAT / ELF GOT+PLT / Mach-O LC_DYLD_INFO_ONLY); codesign placeholders. Plan 14 LK1–LK10 ✅. |

## Project Structure

```
src/
├── program/          Public API — project, file list, or directory input
├── core/             Shared types (Tree/HIR/MIR/LIR substrate, schemas, diagnostics)
├── dss-config/       Language + target JSON configs
│   ├── sources/      .lang.json — per-language grammar/semantics/lowering
│   └── targets/      .target.json — per-target opcode/register/ABI
├── tokenizer/        Character stream → token stream
├── analysis/
│   ├── syntactic/    Parser → CST (recursive descent + Pratt walker + LSP)
│   ├── semantic/     Symbol table, type checker, scope resolver
│   └── compilation_unit/  Multi-file CU + cross-file import resolution
├── hir/              High-level IR (typed, language-neutral) + verifier + .dsshir text
├── mir/              Mid-level IR (SSA over CFG, structured-CF markers) + .dssir text
├── lir/              Low-level IR (per-target, post-regalloc) + .dsslir text + regalloc + callconv
├── asm/              In-tree assembler — shape-keyed byte encoders + round-trip oracle disassembler (plan 13 ✅)
├── link/             In-tree linker — ObjectFormatSchema + format-blind engine + ELF/PE/MachO/WASM/SPIR-V writers + file emission (plan 14 LK1–LK10 ✅)
└── lsp/              Language Server Protocol (stdio JSON-RPC + diagnostics)
```

The IR layering is HIR → MIR → LIR. HIR is the language-neutral pivot (CST→HIR lowering is config-driven, no per-language C++); MIR is SSA over CFG with structured-CF markers preserved; LIR is target-specific (JSON-configured) with virtual + physical registers. Each layer ships its own arena substrate, verifier, and round-trippable text format.

## Building

### Prerequisites

- C++23-capable compiler (MSVC 17.5+, GCC 13+, Clang 16+)
- CMake 4.0+
- Network access on first configure (FetchContent pulls nlohmann/json 3.12.0 + GoogleTest 1.17.0)

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
cd build && ctest --output-on-failure
```

## Documentation

- [Implementation Plan](.plans/00-compiler-implementation-plan%20-%20tbd.md) — Master plan, sub-plan index, gap catalog
- [Plan 09 — HIR](.plans/09-hir-plan%20-%20ok.md) — High-level IR (language-neutral pivot)
- [Plan 12 — MIR + LIR](.plans/12-mir-lir-plan%20-%20ok.md) — Mid + low-level IR
- [Plan 12.5 — Const-eval](.plans/12.5-const-eval-plan%20-%20ok.md) — Shared constants-evaluation engine
- [Plan 13 — Assembler](.plans/13-assembler-plan%20-%20tbd.md) — In-tree assembler (AS1–AS6 ✅ closed end-to-end)
- [Plan 14 — Linker](.plans/14-linker-plan%20-%20tbd.md) — In-tree linker (LK1–LK10 ✅ closed end-to-end; ELF/PE/MachO + WASM/SPIR-V skeletons + driver pipeline)
- `docs/language-config-spec.md` — Current `.lang.json` schema (v4)
- `docs/tree-model.md` — Tree + arena substrate
