<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/logo-w.png">
  <source media="(prefers-color-scheme: light)" srcset="img/logo-b.png">
  <img alt="DSS Code Prime — Prime Compiler" src="img/logo-b.png" width="420">
</picture>

# DSS Code Prime

**One hermetic engine that compiles any language to native code for any machine — every byte from source to binary, owned.**

*No LLVM. No GCC. No system assembler or linker.*

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
&nbsp;![Target: C23](https://img.shields.io/badge/target-C23-informational)
&nbsp;![ISAs: x86_64 | arm64](https://img.shields.io/badge/ISAs-x86__64%20%7C%20arm64-informational)
&nbsp;![Formats: ELF | PE | Mach-O](https://img.shields.io/badge/object%20formats-ELF%20%7C%20PE%20%7C%20Mach--O-informational)

</div>

---

## Why DSS

Most compilers are a thin front-end bolted onto a giant shared back-end — LLVM, or GCC. DSS Code Prime inverts that. It is a single, self-contained engine in which **a compilation target is *data, not code***: the CPU instruction set, the calling conventions, the object-file format, and the source language itself all live in JSON configuration that one generic, agnostic engine walks. Adding an architecture or a language is a config-file drop, not a fork of the compiler.

The result is a **hermetic, auditable toolchain**. DSS writes its own machine code and its own PE, ELF, and Mach-O executables directly — no `as`, no `ld`, no `lld`, no `llvm-mc`. Every byte between your source and the running binary is produced by roughly **150,000 lines of code you can read**, not by an opaque, multi-million-line dependency you can only trust.

## What works today

DSS Code Prime already compiles and runs **real, unmodified, production software**:

- **SQLite** — the complete ~270,000-line amalgamation — compiles and runs as a working database on **Windows, Linux, and macOS across both x86_64 and arm64**, with **zero special flags** (runtime-verified on native CI). `sqlite3 --version` → 3.54.0; `SELECT`, and full CRUD, all correct.
- **Codegen verified against GCC.** Across a broad SQL workload, DSS's generated code is byte-for-byte identical to GCC's — down to the last digit of floating-point text. The compiler also audits itself for **silent miscompiles**, the one failure class this project treats as unacceptable.
- **600+ internal tests, 100% green**, backed by a test corpus nearly as large as the engine itself (~143,000 lines).
- **The whole pipeline is in-tree and complete**: tokenizer → parser → semantic analysis → three-tier IR (HIR → MIR → LIR) → register allocation → **its own assembler** (x86_64 + arm64 byte encoding with a round-trip oracle) → **its own linker** (ELF / PE / Mach-O, static and dynamic).

| Capability | Status |
|---|---|
| **Source languages** | `c-subset` (→ full C23, in progress), `tsql-subset`, `toy` — each a `.lang.json` |
| **CPU targets** | `x86_64`, `arm64` — shipped end-to-end (encoder + round-trip oracle) |
| **Object formats** | ELF, PE, Mach-O — shipped (executables + dynamic linking); WASM, SPIR-V — skeletons |
| **Real-world corpus** | SQLite — compiles **and runs** on four OS × ISA targets, zero flags |
| **Codegen fidelity** | byte-identical to GCC on a broad differential workload |

> **Honest status.** DSS is in active development toward full C23 conformance — today it clears ~90% of an empirical, end-to-end C-feature battery (compiled *and executed*, not just parsed). It compiles and *runs* SQLite now; passing SQLite's own upstream test suite is a tracked next milestone. See [`.plans/`](.plans/) for the live, per-cycle status.

## How it works

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
│  link           In-tree linker — object-format writers    │
└──────────────────────────────────────────────────────────┘
    │
    ▼
Native executables · libraries · (WASM / SPIR-V — in progress)
```

The pipeline is **fully config-driven end to end.** A `.lang.json` declares a language's lexer, grammar, semantics, and HIR lowering; a `.target.json` declares a target's opcodes, registers, calling conventions, and encodings; an object-format schema declares the binary container. The shared substrate (`src/{tokenizer,analysis,hir,mir,lir,opt,asm,link,core}`) contains **zero `if (arch/format/language == …)` branches** — that agnosticism is the project's core invariant, enforced on every change.

Three-tier IR: **HIR** (language-neutral, typed) → **MIR** (SSA over a CFG with structured-control-flow markers) → **LIR** (per-target, post-register-allocation). Each tier has its own arena substrate, verifier, and round-trippable text format (`.dsshir` / `.dssir` / `.dsslir`).

## Roadmap & vision

DSS Code Prime is one instance of a larger thesis: **one engine, many languages, many targets, every byte owned.** The road ahead:

- **Full C23** — complete conformance, on every target. This is the current arc; most of the language already works end-to-end.
- **DSS Axis** — a new, first-class systems language of our own design, hosted on the *same* engine as a pure `.lang.json` + lowering. Axis is where "any source language" stops being a demonstration and becomes a language people choose. *(In design.)*
- **Transpilation** — the language-neutral HIR that every front-end already lowers *into* will also be raised back *out* as source, making DSS a universal transpiler as well as a compiler: any input language to any output language through one shared pivot — no per-language-pair translator.
- **More architectures** — RISC-V next, then the long tail — each a `.target.json`, never an engine fork.
- **More formats & platforms** — WASM and SPIR-V from skeleton to shipping; a widening real-world corpus beyond SQLite.
- **Ship-ready packaging** — the same hermetic pipeline will emit *finished, distributable* artifacts, not just raw binaries: native libraries and executables, and complete **Android and iOS app packages** — assembled into their final bundles, **permissioned** (manifests / entitlements) and **code-signed** automatically. Own every byte all the way to the store-ready package.
- **The end state** — a hermetic, fully auditable, reproducible toolchain: build real software on any platform with no opaque, billion-line dependency beneath it — a compiler you can actually read, verify, and trust.

## Key features

- **Any input language** — languages are `.lang.json` configs (lexer / grammar / semantics / HIR-lowering / imports). Same engine, no per-language C++ branches.
- **Any target ISA** — targets are `.target.json` configs (opcodes / registers / calling conventions / terminators / encoding shapes / relocations). x86_64 and arm64 ship end-to-end.
- **Three-tier IR** — HIR → MIR → LIR, each with its own verifier and round-trippable text form.
- **Hermetic toolchain** — own every byte from source to binary. No GAS / MASM / llvm-mc / ld / lld. In-tree assembler and linker, both complete.
- **Fail-loud discipline** — a disabled or unsupported feature raises a real diagnostic; it never silently miscompiles.
- **Cross-platform** — builds natively on Windows, Linux, and macOS; a Docker image gives reproducible builds.

## Usage

The compiler exposes a **program API** with three input modes.

**Project file (`.dss-project.json`)** — a self-contained project definition (full spec: [`docs/project-config-spec.md`](docs/project-config-spec.md)):

```jsonc
{
  "language":        "c-subset",
  "artifactProfile": "cli",
  "targets":         ["x86_64:elf64-x86_64-linux-exec", "x86_64:pe64-x86_64-windows-exec"],
  "sources":         ["src/main.c"],
  "output":          "dist/myprog"
}
```

```bash
dss-code-prime --project myapp.dss-project.json
```

**File list:**

```bash
dss-code-prime --compile src/main.c src/utils.c --language c-subset \
  --target x86_64:elf64-x86_64-linux-exec --target x86_64:pe64-x86_64-windows-exec
```

**Directory scan** (recurses for the language's configured extensions):

```bash
dss-code-prime --dir ./src/ --language c-subset --target x86_64:elf64-x86_64-linux-exec
```

## Defining a language or target

Everything the engine needs is declared in JSON — there is no per-language or per-target C++ to write.

- **A source language** is a `.lang.json` under `src/dss-config/sources/`, declaring its lexer (`tokens`), grammar (`keywords` / `scopes` / `shapes`), semantics (symbol table + type system), and HIR lowering. Shipped references: `c-subset` (a substantial C subset en route to full C23), `tsql-subset` (T-SQL DDL + DML), and `toy` (a small typed language used as a genericity oracle).
- **A target** is a `.target.json` under `src/dss-config/targets/`, declaring its opcode set, register file, calling conventions, terminator kinds, encoding shapes, and relocation taxonomy. Shipped: `x86_64`, `arm64`.
- **An object format** is declared as an `ObjectFormatSchema` the linker walks. Shipped: ELF, PE, Mach-O (WASM and SPIR-V are skeletons).

To add a language or an ISA, drop the config file in — the compiler discovers it by name, no recompilation. The authoritative schemas live in [`docs/language-config-spec.md`](docs/language-config-spec.md) and in the shipped configs themselves.

## Supported targets

Targets are JSON-configured (`src/dss-config/targets/*.target.json`); the substrate is fully target-blind — opcode dispatch, register names, calling conventions, and terminator shapes are all read from the schema.

| Target | OS / Arch | Status |
|---|---|---|
| `x86_64` | Linux / Windows / macOS × x86_64 | **Shipped** — full opcode set, SysV AMD64 + Microsoft x64 calling conventions, byte encoding, round-trip oracle |
| `arm64` | Linux / Windows / macOS / iOS / Android × ARM64 | **Shipped** — AAPCS64, byte encoding, round-trip oracle (MS-ARM64 calling convention deferred) |
| Object formats | per target | **Shipped** — ELF / PE / Mach-O executables + dynamic linking (PE IAT · ELF GOT+PLT · Mach-O `LC_DYLD_INFO_ONLY`); WASM / SPIR-V skeletons |
| `wasm` / `riscv` | Web / embedded | **Planned** — each a config drop over the same engine |

## Project structure

```
src/
├── program/          Public API — project, file list, or directory input
├── core/             Shared substrate (Tree/HIR/MIR/LIR types, schemas, diagnostics)
├── dss-config/       Language + target JSON configs
│   ├── sources/      .lang.json — per-language grammar / semantics / lowering
│   └── targets/      .target.json — per-target opcode / register / ABI
├── tokenizer/        Character stream → token stream
├── analysis/         Lexical → syntactic (CST) → semantic (types, scopes) + multi-file units
├── hir/              High-level IR (typed, language-neutral) + verifier + .dsshir text
├── mir/              Mid-level IR (SSA over CFG) + .dssir text
├── lir/              Low-level IR (per-target, post-regalloc) + regalloc + callconv + .dsslir text
├── opt/              Optimizer passes over MIR
├── asm/              In-tree assembler — shape-keyed byte encoders + round-trip oracle disassembler
├── link/             In-tree linker — ObjectFormatSchema + format-blind engine + ELF/PE/Mach-O/WASM/SPIR-V writers
└── lsp/              Language Server Protocol (stdio JSON-RPC + diagnostics)
```

## Building

**Prerequisites** — a C++23 compiler (MSVC 17.5+, GCC 13+, Clang 16+), CMake 4.0+, and network access on first configure (FetchContent pulls nlohmann/json 3.12.0 + GoogleTest 1.17.0).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Docker** (reproducible build):

```bash
docker compose -f docker/docker-compose.yml up --build
```

**Tests:**

```bash
cd build && ctest --output-on-failure
```

## Contributing

Issues and discussions are open — the [issue forms](.github/ISSUE_TEMPLATE) will guide you. The one hard rule: the shared engine stays **source-, target-, and format-agnostic** (no `if (arch/format/language == …)` in the substrate), and the project **fails loud** rather than ever silently miscompiling. New behavior comes with a test that goes red when it regresses.

## Support the project

If DSS Code Prime is useful to you — or you simply want to see a genuinely independent, auditable toolchain exist — the **Sponsor** button (linking to [dailysoftwaresystems.com](https://dailysoftwaresystems.com/)) funds the work directly. Every contribution extends the runway toward full C23, DSS Axis, and more architectures.

## Contact

Maintained by **Rafael Gasperetti** — [rafaelgasperetti@dailysoftwaresystems.com](mailto:rafaelgasperetti@dailysoftwaresystems.com). For partnerships, sponsorship, or anything else, reach us at [dailysoftwaresystems.com](https://dailysoftwaresystems.com/).

## License

DSS Code Prime — and **DSS Axis**, the forthcoming DSS language — are licensed under the **Apache License, Version 2.0**. See [LICENSE](LICENSE) and [NOTICE](NOTICE). (Previously proprietary; relicensed as open source under Apache 2.0.)

## Documentation

- [Implementation plan](.plans/00-compiler-implementation-plan%20-%20tbd.md) — master plan, sub-plan index, gap catalog
- [Plan 09 — HIR](.plans/09-hir-plan%20-%20ok.md) · [Plan 12 — MIR + LIR](.plans/12-mir-lir-plan%20-%20ok.md) · [Plan 13 — Assembler](.plans/13-assembler-plan%20-%20tbd.md) · [Plan 14 — Linker](.plans/14-linker-plan%20-%20tbd.md)
- `docs/language-config-spec.md` — the `.lang.json` schema · `docs/tree-model.md` — the Tree + arena substrate
