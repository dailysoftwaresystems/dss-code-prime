# FFI / Precompiled-Library Ingestion — Sub-Plan (11)

> Owns **object-file readers** (ELF / PE / Mach-O / COFF / `.a` / `.lib` / `.so` / `.dll` / `.dylib`) + **C / C++ minimal header parser** + **ABI catalog** + **name-mangling readers** (Itanium / MSVC) + **extern-decl ingestion** into [HIR](./09-hir-plan%20-%20ok.md) + the [core type lattice](./08.5-substrate-prep-plan%20-%20ok.md). Per the user's mandate: "We will be able to read pre-compiled libraries (FFI)."

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ✅ **MOSTLY DONE 2026-06-02.** FF1-ELF + FF2 + FF3 + FF4 + FF5 + FF6 (Windows / msvcrt.puts) all CLOSED. **First DSS-produced binary that prints**: `examples/c-subset/hello_puts/` calls msvcrt's `puts`, writes "hello\r\n" to captured stdout, exits 42 — all asserted byte-for-byte by examples_runner via Slice 1's stdout pipe. **FF11 shipped-lib ingestion ✅ CLOSED** (cycle 21, 2026-06-05 — C-faithful `#include <stdio.h>` runs with NO inline `extern`); **shipped-lib ingestion is now language-AGNOSTIC** — v0.0.2 V2-2 (2026-06-06) replaced the cycle-21 c-subset-source `.h` with a NEUTRAL JSON descriptor `shippedLibs/<platform>/*.json` (universal reader `src/ffi/shipped_lib_descriptor.{hpp,cpp}` + the single shared `parseTypeFromText` type-text codec + semantic-phase symbol injection), closing `D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC` (the inline-extern + quote-include paths UNCHANGED). **shippedLibs build-out (2026-06-08):** the descriptor surface was broadened to the FULL most-used set across all three platforms — `stdio`/`stdlib`/`string`/`ctype`/`math` under each of `windows-x86_64` / `linux-x86_64` / `macos-arm64` (LP64-vs-LLP64 ABI-delta correct: the `long`-bearing `atol`/`strtol`/`strtoul`/`labs`/`fseek`/`ftell` differ Win [i32/u32] vs Linux+macOS [i64/u64]) — and the descriptor schema gained a REQUIRED `header` + optional `standard` **provenance** field (every symbol records which header it comes from; fail-loud `F_ShippedLibDescriptorMalformed` on a missing/empty `header`). `AllShippedDescriptorsDecode` validates every shipped descriptor decodes + every signature parses; the `shipped_include_abs` corpus proves a NON-stdio descriptor (stdlib `abs`) resolves+links+runs end-to-end (exit 42). Variadic functions (`printf`/`scanf`/…) are deliberately EXCLUDED (`D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE` — the `fn(...)` type-text grammar has no variadic marker yet, fails loud); platform auto-select ✅ DISSOLVED 2026-06-09 by Model 3 (`D-FFI-SHIPPED-LIB-PLATFORM-SELECT` — the per-platform dirs collapsed into ONE neutral `shippedLibs/<h>.json` set with a per-format `library` map resolved per-target at `compile_pipeline`; `shippedLibDirs` → `["shippedLibs"]`). Index + ABI-delta + variadic-exclusion notes: `src/dss-config/shippedLibs/README.md`; schema doc: `docs/language-config-spec.md` §12. **Remaining**: FF1-PE + FF1-MachO binary readers (anchored — triggered by first PE / macOS corpus needing extern resolution via shipped binaries rather than headers). **FF6 cross-host `puts` equivalents ✅ DONE — `#include <stdio.h>` + `puts("hello")` RUNS on all 3 OSes × 2 arches** (the string-FFI capstone, 2026-06-09 — `examples/c-subset/shipped_include_puts` → "hello\n"+exit 42 on Win-PE x86_64 + Linux-ELF x86_64+arm64 + macOS-Mach-O arm64; macos-arm64 RUNTIME-PROVEN on real Apple Silicon, run 27212153591 `macos-clang-release` ctest #206 Passed). Unblocked by D-LK10-ENTRY-ARM64 ✅ + D-LK10-ENTRY-MACHO-EXIT ✅ + Model-3 shipped-lib FFI. |
| Predecessors  | ✅ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) (core type lattice — complete). ✅ [`09-hir-plan`](./09-hir-plan%20-%20ok.md) (`ExternFunction` / `ExternGlobal` nodes — HR1–HR11 ✅ 2026-05-26..28: the declarations landed at HR4 and the `FfiMetadata` / `HirFfiMap` side-table FFI ingestion populates is in place HR4–HR5, round-trip-serialized by HR7's `.dsshir` `@ffi(...)`; **HR9 added `externDecl`→ExternFunction/ExternGlobal lowering** (c-subset `externFuncTail` grammar split + `externDecl` semantics rule + `lowerExternDecl`), so the extern *surface* now lowers — the FFI *metadata* population (linkage/ABI/mangling from real binaries/headers) is still THIS plan's job; HR10 added tsql-subset lowering, HR11 ✅ done 2026-05-28 (multi-language CU lowering) — plan 09 complete). |
| Successors    | ✅ (fired) [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) LK6 consumes extern symbol declarations for import-table generation — landed + runtime-proven (PE IAT 2-DLL `hello_puts` / ELF GOT+PLT / Mach-O stubs). |
| Scope         | **Bounded.** FF1–FF6. v1 must read enough to declare libc / libSystem / msvcrt / kernel32 symbols. Full C++ name mangling (Itanium / MSVC) post-v1; C-style first. |

---

## 1. Motivation

A compiler that can't call existing libraries can't build a useful binary. v1 needs at minimum `printf` / `malloc` / `free` / `exit` from libc. Post-v1: full precompiled-library reach (any DLL / SO / dylib / static archive).

Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — `nm` / `dumpbin` / `objdump` are test oracles only.

---

## 2. Design

### 2.1 Files

```
src/ffi/
├── ffi.hpp                          # Public entry point
├── binary_readers/
│   ├── elf_reader.hpp / .cpp        # ELF dynamic + symtab readers
│   ├── pe_reader.hpp / .cpp         # PE imports + exports + .lib archives
│   ├── macho_reader.hpp / .cpp      # Mach-O LC_SYMTAB + LC_DYLD_INFO
│   ├── ar_reader.hpp / .cpp         # Unix `.a` / `.lib` archive container
│   └── reloc_kinds.hpp              # Shared relocation type vocabulary
├── header_parser/
│   ├── c_header_mode.hpp / .cpp     # Reuses c-subset frontend in "header mode"
│   └── header_to_extern.hpp / .cpp  # Extern-decl extraction
├── abi/
│   ├── abi_catalog.hpp / .cpp       # Per (lang × platform) calling-convention table
│   └── target_layout.hpp / .cpp     # Per-target integer / pointer / aggregate layout
├── mangling/
│   ├── itanium_demangle.hpp / .cpp  # C++ Itanium ABI (Linux / macOS)
│   ├── msvc_demangle.hpp / .cpp     # MSVC mangling (Windows)
│   └── c_mangle.hpp / .cpp          # C name (no mangling) + per-platform underscoring
└── ingest.hpp / .cpp                # Public: read + emit HIR ExternFunction/ExternGlobal nodes

tests/ffi/
├── test_elf_reader.cpp
├── test_pe_reader.cpp
├── test_macho_reader.cpp
├── test_ar_reader.cpp
├── test_c_header_mode.cpp
├── test_itanium_demangle.cpp
├── test_msvc_demangle.cpp
└── test_libc_smoke.cpp              # End-to-end: import printf, call it
```

### 2.2 Binary readers

Each format reader produces a uniform `ImportSurface`:

```cpp
struct ImportSurface {
    std::vector<ExternSymbol> exports;       // What this binary exposes
    std::vector<ExternSymbol> undefinedRefs; // What it needs from elsewhere
    std::string moduleName;                  // libc.so.6, msvcrt.dll, libSystem.B.dylib
    BinaryFormat format;                     // ELF / PE / MACHO
    BinaryArch arch;                         // X86_64 / ARM64
    std::optional<std::string> soname;       // ELF DT_SONAME / Mach-O install_name
};

struct ExternSymbol {
    std::string mangledName;                 // As stored in the binary
    std::optional<std::string> demangledName;
    SymbolKind kind;                         // Function / Variable / TLS
    Visibility visibility;                   // Default / Hidden / Protected
    Linkage linkage;                         // Strong / Weak / Common
    std::optional<TypeId> resolvedType;      // If sourced from a header
};
```

ELF reader: walks `.dynsym` + `.dynstr` + `.gnu.hash` / `.hash` + DT_NEEDED entries.
PE reader: walks IAT + ILT + export table (.edata) + import table (.idata). Reads `.lib` static archives + `.dll`.
Mach-O reader: walks LC_SYMTAB + LC_DYSYMTAB + LC_DYLD_INFO_ONLY bind/lazy-bind opcode trie (or LC_DYLD_CHAINED_FIXUPS modern format).
`.a` reader: BSD-style `ar` archive (libc.a, libm.a).

### 2.3 C header parser ("header mode")

Reuse the c-subset frontend in a dedicated mode. Disables function-body parsing; accepts `typedef`, `struct`/`union`/`enum`, function prototypes, `extern` declarations, preprocessor directives that affect declarations (`#define`-only-macros for typed constants).

```c
extern int printf(const char* fmt, ...);
extern void* malloc(size_t size);
extern void  free(void* p);
typedef unsigned long size_t;
```

→ produces HIR `ExternFunction` and `ExternGlobal` nodes with:
- `name` = the declared name (post-mangling)
- `signature` = core lattice `FnSig` with v1 calling convention default (`CcSysV` / `CcMS64` per platform)
- `linkage` = strong default

Full C99 preprocessor is **out of v1** — the header mode accepts typedef + struct + extern + simple macros only. Real headers (glibc / Windows SDK) often need the preprocessor; the original design called for hand-curated reduced headers under `src/dss-config/ffi-headers/`. That tree + the `readCHeaderShipped` access path were removed OPT2 cycle 1 (2026-06-03, commit `8bae225`) as dead-code — production now routes through `synthesizeFfiFromSourceDecls` (source-extern-authority); the `readCHeader` / `readCHeaderFromText` substrate stays as latent capability for the post-v1 system-header parsing scenario.

Open question §4.3: full preprocessor in v1 or v1.x?

### 2.4 ABI catalog

Per (language × platform):

| Tuple | Calling convention | Layout |
|---|---|---|
| C × Linux x86_64 | SysV AMD64 | LP64 |
| C × Linux ARM64 | AAPCS64 | LP64 |
| C × Windows x86_64 | Microsoft x64 | LLP64 |
| C × Windows ARM64 | Microsoft ARM64 | LLP64 |
| C × macOS x86_64 | SysV AMD64 (with quirks) | LP64 |
| C × macOS ARM64 | Apple ARM64 | LP64 |
| C++ × Linux/macOS | Itanium ABI | as C |
| C++ × Windows | MSVC ABI | as C |

Per-tuple: integer / pointer / pointer-alignment sizes; struct field padding rules; va_arg handling; small-aggregate-in-registers thresholds. The ABI catalog drives both extern-decl type lowering and the linker's call instruction sequence.

### 2.5 Name mangling readers

- **C**: pass-through with per-platform underscoring (`_printf` on x86 Mach-O / PE; `printf` on x86_64 ELF / Mach-O / PE).
- **Itanium** (Linux / macOS C++): full demangling of `_Z*` symbols. Standard algorithm; ~800 LOC reference.
- **MSVC** (Windows C++): full demangling of `?*` symbols. Complex; ~1500 LOC reference.
- Both demanglers reverse-runnable (mangle generation) for FFI-export side.

v1 ships C-only (no C++ FFI demangling). Itanium + MSVC demanglers reserved post-v1 unless a v1 language needs C++ interop.

### 2.6 Extern-decl ingestion into HIR

Two sibling producers populate `HirAttribute<FfiMetadata>` (the FFI side-table) — `ingest()` for header / binary-validated externs and `synthesizeFfiFromSourceDecls()` for source-declared externs whose signature is authoritative because the language grammar already emits a complete `extern int puts(const char*);`-shape signature in HIR:

```cpp
HirIngestResult ingest(std::span<IngestionSource const> sources,
                       std::span<ExternDeclRef const>   externs,
                       TargetSchema const&              target,
                       ObjectFormatSchema const&        format,
                       HirFfiMap&                       ffiMap,
                       DiagnosticReporter&              reporter);

HirIngestResult synthesizeFfiFromSourceDecls(
                       std::span<ExternDeclRef const> externs,
                       std::string_view               importLibrary,
                       TargetSchema const&            target,
                       ObjectFormatSchema const&      format,
                       HirFfiMap&                     ffiMap,
                       DiagnosticReporter&            reporter);
```

`ingest()` aggregates ImportSurface rows from each `IngestionSource` (FF1 binary reader or FF2 header parser), FF4-unapplies binary-reader rows, matches by canonical name, FF4-applies to produce the linker-visible decorated name, then `ffiMap.set(externNode, FfiMetadata{...})`. `synthesizeFfiFromSourceDecls()` trusts the source's own extern declaration as the signature authority — no header / binary read; per-format mangling via FF4 + per-language `externLibraryByFormat` lookup. Both produce `HirIngestResult` (`ok()` snapshot + `externsAnnotated` count).

Output: `HirAttribute<FfiMetadata>` populated on each emitted `ExternFunction` / `ExternGlobal` (calling convention, linkage, source library name + soname, mangled name preserved for linker import).

### 2.7 Reverse direction: emit headers for `lib` builds

When `artifactProfile: lib`, emit a `.h` file (C-style) describing exports. Reserved post-v1 unless a v1 customer needs it; mechanism is symmetric (HIR → reverse-AST → text via `10-source-translation-plan` style emission template).

---

## 3. PR breakdown

| PR  | Title                                       | Scope |
|-----|---------------------------------------------|-------|
| FF1 | Binary readers (ELF + PE + Mach-O + ar)     | `ImportSurface` extraction; round-trip tests against objdump / dumpbin / nm as oracles. **ELF half ✅ landed 2026-06-01** (Track B of parallel A+B cycle): new `src/ffi/` directory with `import_surface.hpp` (closed-enum `SymbolKind` / `SymbolVisibility` / `SymbolLinkage`) + `binary_reader.{hpp,cpp}` (format-blind dispatch via magic bytes: ELF / PE / Mach-O). ELF64 LE reader parses `.dynsym` + `.dynstr` via shstrtab lookup; maps STT_FUNC/OBJECT/TLS → SymbolKind, STV_HIDDEN/PROTECTED/INTERNAL → SymbolVisibility, STB_GLOBAL/WEAK/LOCAL → SymbolLinkage; skips STN_UNDEF + locals + empty-name entries. New 7 F_* diagnostic codes at 0x5xxx (F_FileOpenFailed / F_FileEmpty / F_UnknownBinaryFormat / F_UnsupportedBinaryFormat / F_CorruptedBinary / F_UnsupportedElfClass / F_SectionNotFound). PE half (FF1-PE) + Mach-O half (FF1-MachO) anchored as separate triggers — format-blind dispatch already routes to them with `UnsupportedFormat` diagnostics citing the future anchor. 13 tests (round-trip + locals-skipped + 5 failure modes + 2 unsupported-format dispatch + 3 diagnostic round-trip). 129/129 ctest. |
| FF1-PE | PE export-table reader                     | Parse `.edata` (export directory) and `.idata` (import-address-table) from `.dll` / `.lib` archives. Output uniform `ImportSurface` rows. Mirrors FF1-ELF's discipline (format-blind dispatch + F_* diagnostics + closed-enum SymbolKind/Visibility/Linkage). Trigger: first Windows c-subset corpus needing extern resolution against `msvcrt.dll` / `kernel32.dll`. | First PE corpus that needs FFI-resolved externs. |
| FF1-MachO | Mach-O LC_SYMTAB reader                 | Parse `LC_SYMTAB` (n_strx → string table) + `LC_DYSYMTAB` (extern subset) + `LC_DYLD_INFO_ONLY` bind opcodes for delayed-bind externs. Output uniform `ImportSurface` rows. Trigger: first macOS c-subset corpus needing extern resolution against `libSystem.B.dylib`. | First macOS corpus that needs FFI-resolved externs. |
| ~~FF2~~ | C header mode + parser reuse | ✅ **CLOSED 2026-06-01 + PARTIALLY DE-SCOPED 2026-06-03.** `src/ffi/c_header_parser.{hpp,cpp}` exposes `readCHeader[FromText](...) -> expected<vector<ImportSurface>, HeaderReadError>`. Reuses the c-subset frontend; rejects function bodies / non-extern globals / `#include` / unknown HirKinds each with a distinct F_* code. 8 `HeaderReadErrorKind` variants ↔ 8 F_* codes (0x5008-0x500E) via closed-table dispatch. **Shipped-headers de-scope OPT2 cycle 1 (2026-06-03, commit `8bae225`)**: `src/dss-config/ffi-headers/libc/{stdio,stdlib}.h` + `readCHeaderShipped` + `findShippedFfiHeader` + `HeaderReadErrorKind::InvalidShippedPath` (the 9th variant) removed as dead-code — no production caller; sole consumers were self-tests of the headers themselves. Diagnostic codes `C_InvalidShippedFfiHeaderPath` (0xC034) + `F_HeaderInvalidShippedPath` (0x500F) RETIRED (numbers reserved, marked in `parse_diagnostic.hpp`). The `readCHeader` / `readCHeaderFromText` substrate stays as latent capability (FF-latent) — substrate kept compiled + tested for post-v1 system-header parsing or future opt-in header-validation. **Out of scope**: extern initializer detection (cross-tier — D-FF2-3); SourceLocation on HeaderReadError struct (D-FF2-2). Detailed fold history in `memory/project_ff2_*`. **Anchors**: D-FF2-2 SourceLocation struct field (trigger: first field report needing struct-side span); D-FF2-3 `extern int x = 5;` reject (trigger: next cycle touching `cst_to_hir.cpp::lowerExternDecl`); D-FF2-4/5/6 behavioral tests for unreachable arms (each gated on a future c-subset evolution); D-FF1-NEST split `src/ffi/binary_readers/` (trigger: 2nd binary-reader TU). |
| ~~FF3~~ | ABI catalog | ✅ **SUBSTRATE CLOSED 2026-06-01.** `src/ffi/abi/abi_catalog.{hpp,cpp}` exposes `resolveAbi(target, format, reporter) -> expected<AbiTuple, AbiResolveError>`. Closed-table `kAbiCatalog` keys (target.name, format.kind) → (CallConv, cc-name); 6 rows (4 shipped + 2 anchored-but-unshipped). 4 F_Abi* codes at 0x5010-0x5013 via `Count_`-sentinel closed-table. Defensive cc-register-resolvability pass at the FF3 boundary catches paste-error cc rows that bypass schema-loader validate(). **Wiring D-FF3-3** ✅ closed 2026-06-01 (post-fold #5 parallel-tracks cycle): `resolveAbi` now threads from `compileOneTarget` → `compileSingleUnit(ccIndex)` → `allocateRegisters(ccIndex)` → `allocateOneFunc`, replacing the `lir_regalloc.cpp::317` `callingConventionIndex = 0` hardcode. Detailed fold history in `memory/project_ff3_*`. **Out of scope**: layout-side fields (D-FF3-1 — layout half ✅ **CLOSED 2026-06-15 by plan-23 FC6**: the per-`.target.json` `aggregateLayout` params + the generic `type_layout` engine + the `ArenaAttribute<TypeInterner, StructLayout>` side-table; the va_arg half stays OPEN for FC12a); programmatic-construction lifetime hardening (D-FF3-2); cross-arch CallConv variant split (D-FF3-4). **Anchors**: D-FF3-1 layout fields — **layout half ✅ CLOSED 2026-06-15 (FC6)**; va_arg half OPEN (trigger: variadic `<stdarg.h>`, FC12a) [orig trigger: first non-LP64 target]; D-FF3-2 cc-pointer reshape (trigger: FF5 stores AbiTuple past local schema scope); D-FF3-4 distinct CcMSARM64 (trigger: arm64-Windows corpus); ~~D-FF3-5 test rewrite~~ ✅ **CLOSED 2026-06-08** (trigger fired — the `apple_arm64`/`CcApple` cc row shipped in `arm64.target.json` with the macOS-ARM64 exec cycle; the ABI catalog `(arm64, MachO)→CcApple` now resolves, the fail-loud `Arm64MachOFailsLoudUntilAppleArm64CcShipped` test flipped to the happy-path `Arm64MachOResolvesToAppleArm64`; the Apple-ABI divergences pinned at the new `D-FF3-APPLE-ARM64-ABI-DIVERGENCE`). |
| ~~FF4~~ | C name mangling (per-platform underscoring) | ✅ **CLOSED 2026-06-01.** `src/ffi/mangling/c_mangle.{hpp,cpp}` exposes `applyCMangling` / `unapplyCMangling` / `unapplyCManglingStrict` / `cFormatAddsLeadingUnderscore`. Closed-table `kCManglingRules` one row per `ObjectFormatKind`, size pinned via `kObjectFormatKindTable.rows.size()`. v1 rules: ELF/PE/Wasm/SPIR-V/Unknown → no decoration; MachO → leading `_`. Strict-mode variant (D-FF4-3) emits `F_MangleMissingExpectedPrefix` (0x5014) on decorated-format input lacking the prefix. Applied at FF5's ingest boundary (D-FF4-Apply closed). Detailed fold history in `memory/project_ff3_ff4_*`. **Out of scope**: 32-bit PE stdcall `@N` suffix (D-FF4-1). **Anchors**: D-FF4-1 PE32 stdcall (trigger: first 32-bit PE target); D-FF4-2 `CManglingRule` struct public hoist (trigger: D-FF4-1, which adds the suffix axis). |
| ~~FF5~~ | `ingest()` + `HirAttribute<FfiMetadata>` | ✅ **CLOSED 2026-06-01** + **EXTENDED 2026-06-02** (FF6 prep — `synthesizeFfiFromSourceDecls` sibling for source-declared externs). `src/ffi/ingest.{hpp,cpp}` exposes BOTH `ingest(sources, externs, target, format, ffiMap, reporter) -> HirIngestResult` (header / binary validated) AND `synthesizeFfiFromSourceDecls(externs, importLibrary, target, format, ffiMap, reporter) -> HirIngestResult` (source-declared — the language grammar's extern decl IS the signature authority). Both share the FF3 abi-resolve gate + FF4 applyCMangling kernel; both produce the same `HirIngestResult` shape. `synthesize` is the canonical c-subset path: per-language `SemanticConfig.externLibraryByFormat: unordered_map<string,string>` (keyed on `objectFormatKindName(format.kind())`) declares the runtime library identity; c-subset ships `pe → msvcrt.dll`, `elf → libc.so.6`, `macho → /usr/lib/libSystem.B.dylib`. New diagnostic `F_FfiNoImportLibraryForFormat = 0x5019` (unsuppressable) fires when the active format has no entry. Composes FF1 + FF2 + FF3 + FF4 at the ingest boundary: reads each `IngestionSource` (variant over BinaryLibrarySource / CHeaderSource / CHeaderDirSource — closes D-FF5-INGESTION-SOURCE), aggregates ImportSurface rows, FF4-unapplies binary-reader rows, matches against caller-supplied `ExternDeclRef[]` (HirNodeId + canonicalName, decoupled from SemanticModel), FF4-applies to produce linker-visible mangledName, populates HirFfiMap. `readCHeaderDirectory` closes D-FF6-HEADER-DIR-READER. FF3 cross-validation gates the (target, format) pair. **Wiring** (2026-06-02): `compileSingleUnit` step 2.5 between HIR and MIR calls `synthesizeFfiFromSourceDecls` over `CstToHirResult.externDecls` (the new `HirExternRecord` accumulator populated by `lowerExternDecl` for both ExternFunction + ExternGlobal arms). 22 tests in `tests/ffi/test_ingest.cpp` (15 ingest + 7 synthesize + happy-path / Mach-O / ELF / ExternGlobal / mixed-validity / config-gap pins). **Out of scope**: end-to-end runtime print (D-FF6-RUNTIME-PRINT — requires CRT init OR ML7 stack args + kernel32 WriteFile). 5 new JSON loader tests in `tests/core/test_grammar_schema.cpp`. **Anchors**: ~~D-CSUBSET-EXTERN-LIBRARY-SYNTAX~~ ✅ **CLOSED 2026-06-02 (step 13.3a, commit `357ca81`)** — per-symbol library override landed as trailing `stringLiteralExpr` inside `externFuncTail` (between `)` and `;`); c-subset syntax: `extern void* GetStdHandle(int n) "kernel32.dll";`. New fields: `HirExternRecord.libraryOverride` + `ExternDeclRef.libraryOverride`. `synthesizeFfiFromSourceDecls` prefers per-symbol override over format-level default. Source-agnostic by rule-name lookup (`schema().rules().find("stringLiteralExpr")`). Malformed escapes fail-loud via `H_ExternDeclMalformed`. Promoted to anchor `D-CSUBSET-EXTERN-ATTRIBUTE-SYSTEM` for a future 2nd extern-symbol attribute (calling-convention override, ordinal-import). D-FFI-HEADER-VALIDATION-OPTIONAL (compose `ingest()` BEFORE `synthesize()` to validate source-declared signatures against shipped headers; trigger: first language opting into header-driven sig validation); D-FFI-ABI-GATE-HELPER (extract the FF3+FF4 head block shared by ingest+synthesize; trigger: 3rd caller); D-FFI-EXTERN-LIBRARY-OVERRIDE-OPTIONAL (promote `libraryOverride` from sentinel-empty-string to `optional<string>`; trigger: 2nd consumer of the field). **D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC** — ✅ **CLOSED 2026-06-06 (v0.0.2 V2-2).** Delivered language-agnostically: shipped libs are described ONCE in a NEUTRAL JSON descriptor `src/dss-config/shippedLibs/<platform>/*.json` (e.g. `stdio.json`, which REPLACED the cycle-21 c-subset-source `stdio.h` for shipped descriptors), symbols encoding signatures as hir-text type strings (`"fn(ptr<char>) -> i32"`) decoded by the SINGLE shared `parseTypeFromText` codec (extracted from `hir_text.cpp` — `docs/ir-type-text.md`). A universal reader `src/ffi/shipped_lib_descriptor.{hpp,cpp}` parses it; the resolver maps angle `<stdio.h>`→`<stem>.json`; the SEMANTIC phase (before Pass 2) injects the symbols into CU-root scope via the `builtinFunctions` seam (`src/analysis/semantic/semantic_analyzer.cpp`) so the call resolves → externDecls → the UNCHANGED FF5 `synthesizeFfiFromSourceDecls` → linker import. goal-2 = user-decl-wins. Fail-loud `F_ShippedLibDescriptorMalformed`(0x501B) + `F_ShippedLibUnsupportedType`(0x501C), both unsuppressable. ZERO `if (lang == "c")` — `.json` is the neutral form, `.h` was C-coupled by construction (the coupling this anchor forbade). Cited in `src/ffi/shipped_lib_descriptor.{hpp,cpp}` + `src/analysis/semantic/semantic_analyzer.cpp` + `src/core/types/semantic_config.hpp`. ctest 203→204 (new `tests/ffi/test_shipped_lib_descriptor`). Follow-on: `D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE` (a variadic descriptor like `printf`); ~~`D-FFI-SHIPPED-LIB-PLATFORM-SELECT` stays open (the dir still names its platform)~~ → **`D-FFI-SHIPPED-LIB-PLATFORM-SELECT` ✅ DISSOLVED 2026-06-09 (the `#include puts` 3-OS string-FFI capstone, Model 3): the 15 per-platform descriptors collapsed into 5 NEUTRAL `shippedLibs/<h>.json` carrying a per-format `library` map `{pe,elf,macho}` resolved per-target at `compile_pipeline.cpp`; `shippedLibDirs` → the single neutral root `["shippedLibs"]` — there is no per-platform DIR to select anymore (plan-00 §0.2 D12).** — ORIGINAL anchor text retained below for the audit trail. — when shipped-library FFI-surface descriptions are revived (post the OPT2-cycle-1 `8bae225` removal of the C-header `ffi-headers/` tree — for the header-validation opt-in of D-FFI-HEADER-VALIDATION-OPTIONAL, OR a "stdlib symbols without inline `extern` re-declaration" convenience), the descriptor mechanism **MUST be language-agnostic** — C headers are ONE input dialect, NOT the canonical format. The thesis-correct shape: the neutral `ImportSurface` row is the target (it already is — both `binary_readers/` and `c_header_parser` produce it), and shipped descriptors are either (a) a neutral declarative form (e.g. JSON `ImportSurface` rows under `src/dss-config/ffi-libs/<lib>.json`) consumed by ONE universal reader, OR (b) N per-dialect readers (C-header / Rust-sig / `.json`) selected by config, all mapping into the same `ImportSurface`. Bucket-1 declared descriptor + bucket-2 universal ingestion; **zero `if (lang == "c")`** — a future non-C frontend must reach shipped FFI surfaces without expressing them as C. The removed `ffi-headers/libc/*.h` + C-header `readCHeaderShipped` path was C-coupled by construction; this anchor exists so the revival does NOT reinstate that coupling. Trigger: first revival of shipped-library ingestion (D-FFI-HEADER-VALIDATION-OPTIONAL closure OR first non-C-family language needing shipped extern surfaces). Owner: plan 11 FFI. |
| ~~FF6~~ | libc smoke test | ✅ **HELLO-PUTS LANDED 2026-06-02 (Windows host).** First DSS-emitted PE binary that calls into msvcrt.dll's `puts`, prints "hello\r\n" to captured stdout, and exits 42 — all asserted byte-for-byte by `examples_runner` (`examples/c-subset/hello_puts/` with `runOn: ["windows"]` + `expectedStdout: "hello\r\n"`). Diagnosis caught the prior cycle's hypothesis was wrong: msvcrt requires NO user-side CRT init for puts (its own `_DllMainCRTStartup` self-inits stdio at DLL attach). The actual 0xC0000005 blocker had TWO layers — (a) user main missing Win64 shadow-space + alignment-bias prologue (closed via D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: new `callPushBytes` cc field + `hasCalls` ML7 scan + `alignedSizeWithBias` unification); (b) MIR→LIR emitting `call` (E8 direct) for the extern `puts` when it should emit `call_indirect_via_extern` (FF 15 indirect-via-IAT) — closed via new `MnemonicSlot::CallIndirectViaExtern` + `externSymbols` set on Lowerer. `hello_puts` is also the first 2-DLL PE binary (kernel32 ExitProcess + msvcrt puts) AND the first DSS binary exercising stdout-pipe capture (D-LK6-2A-MULTI-LIBRARY-PIN ✅ closed; D-FF6-HELLO-PUTS-PROMOTION-PIN ✅ closed; D-FF6-RUNTIME-PRINT ✅ closed). **Cross-host `puts` equivalents ✅ CLOSED 2026-06-09** (the `#include <stdio.h>` + `puts("hello")` string-FFI capstone — `examples/c-subset/shipped_include_puts` → "hello\n"+exit 42; Linux libc + macOS libSystem both RUN: linux-x86_64 (WSL) + linux-arm64 (qemu + native CI) + macos-arm64 (RUNTIME-PROVEN on Apple Silicon, run 27212153591 ctest #206); unblocked by D-LK10-ENTRY-ARM64 ✅ + D-LK10-ENTRY-MACHO-EXIT ✅ + Model-3 shipped-lib FFI). `printf` with floating-point args + locale-using calls (`fopen`, etc.) gated by D-LK10-CRT-INIT-INVOKE (anchored, NOT blocker for puts). Kernel32-direct print (CRT-free) gated by D-ML7-2.2 stack-args + opaque-pointer typing — anchored D-LK10-KERNEL32-WRITE-PATH. Hard prerequisite D-LK10-ENTRY (the runnable-binary spine) ✅ Stage 1 closed 2026-06-02. |
| FF11 | **Language-agnostic shipped-lib ingestion** | ✅ **CLOSED 2026-06-05 (cycle 21) — C-FAITHFUL SHIPPED HEADERS LANDED (goal-1 + goal-2).** A c-subset program calls a shipped-lib function via `#include <stdio.h>` with NO inline `extern` — exactly like C — compiling to a runnable PE that prints + exits (`examples/c-subset/shipped_include_puts/`, the goal-1 end-to-end proof). **What landed:** (1) the angle-bracket `#include <h>` grammar form + `include-directive`/`header-body` lexer modes — consuming the cycle-20 per-mode capability (`<`→header-path override only in the directive) PLUS a NEW general `popAtNewline` tokenizer capability (a line-scoped mode auto-pops at newline/EOF — C directives, assembly are line-oriented; agnostic, own synthetic test); (2) `SemanticConfig.shippedLibDirs` SYSTEM search path (C's `/usr/include` analogue) + `ImportConfig.systemPathToken`; (3) resolver routes angle→`shippedLibDirs` (HARD `F_ShippedHeaderNotFound`=0x501A, unsuppressable) vs quote→self-dir (soft, unchanged) — config-token-driven, zero `if(lang==…)`; (4) the shipped header (`shippedLibs/windows-x86_64/stdio.h`, c-subset source) is parsed by the language's OWN grammar + merged via the EXISTING include-following → real declaration WITH signature → annotated by the EXISTING FF5 synthesis; (5) **goal-2** (D-FFI-HEADER-VALIDATION-OPTIONAL): a program `extern` conflicting with a shipped-header decl fails loud `S_RedeclaredSymbol` (cross-tree check, deduped exact-once-per-(tree,name)). **ADDITIVE** (user-mandated): the inline-`extern` path (`hello_puts`) + quote-`#include "local.h"` both KEPT green. Agnostic (a 2nd language ships its own headers + declares its own `shippedLibDirs`/modes with ZERO engine change). 3 reviews + self-audit caught a CRITICAL (`F_ShippedHeaderNotFound` missing from the unsuppressable table → fixed + a compile-time `None`-guard) + 2 MEDIUM (goal-2 double-report; modeIntroducedKinds footgun) — all folded. 195/195. **Deferred (anchored):** `D-FFI-SHIPPED-LIB-PLATFORM-SELECT` (target-driven platform-store auto-select — ✅ **since DISSOLVED 2026-06-09 by Model 3, the `#include puts` capstone: neutral `shippedLibs/<h>.json` + per-format `library` map, no per-platform dir to select; plan-00 §0.2 D12**), `D-FFI-ANGLE-INCLUDE-LINE-SCOPED-HEADER` (malformed-header UX polish), `D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC` (the cross-language neutral descriptor — ✅ **CLOSED 2026-06-06, v0.0.2 V2-2**: a language-neutral JSON descriptor `shippedLibs/<platform>/*.json` + universal reader + the single `parseTypeFromText` codec + semantic-phase symbol injection REPLACED the cycle-21 `.h` for shipped descriptors; see the FF5 row above + §0). **Original design + the cycle-20 prerequisite history follow.** 🔓 (was UNBLOCKED cycle 20 when `D-TOKENIZER-CONTEXT-SENSITIVE-LEXING` landed.) The cycle-20 design phase hit a real engine wall: `<` is context-sensitive (`LtOp` in `a < b` expressions — load-bearing across the corpus — vs an include-path opener after `#include`), and the tokenizer's lexeme lookup is CONTEXT-FREE (one global `lexemeTable`), so a naive `<`-pushes-a-mode would break every `<` in the language (the string mode works ONLY because `"` is unambiguous). User chose **Option A — context-sensitive tokenizing** (textbook C) over a localized grammar workaround. So FF11 is sequenced BEHIND a prerequisite **tokenizer-engine cycle — anchor `D-TOKENIZER-CONTEXT-SENSITIVE-LEXING`**: wire the EXISTING-but-unused `GrammarSchema::lookupLexemeInMode` (`grammar_schema.cpp:141-155`) into the main scanner's operator+identifier branches (`tokenizer.cpp:759,773,830`) WITH GLOBAL FALLBACK, + parse per-mode inline `tokens` overrides (close the TODO at `grammar_schema_json.cpp:1810-1818`, which today warns + leaves the per-mode table empty). **Agnostic engine capability** (ANY language's grammar can declare a context-sensitive lexing mode; zero `if(lang==…)`); the FF11 `#include` mode (`#`→pushMode, `<`→header-path token, newline→popMode) is its FIRST consumer. **Gate:** a synthetic-grammar context-sensitive-lexing test (the capability proven via a test grammar, per the unconsumed-substrate discipline) + a FULL CROSS-LANGUAGE REGRESSION (every existing test stays green — a broken tokenizer breaks every language). THEN FF11 (angle-include + shipped headers, design locked below) lands on top — a 2-cycle path the user accepted by choosing A. Keys for the next firing (fresh context): also `import_resolver.cpp:76-101,174-234` (the system-search seam) + `program.cpp:534-615` (thread shippedLibDirs into UnitBuilder) + `parse_diagnostic.hpp` (F_* next slot 0x501A). **The shipped-header DESIGN below stays LOCKED — only the angle-include LEXING was the wall.** ⏳ 🔨 **DESIGN-LOCKED 2026-06-05 (cycle 20, user §B) — C-FAITHFUL SHIPPED HEADERS.** The user chose the C-faithful shipped-header design (option 1) over the prior neutral-store framing, with the standing constraints: 100% config-driven, source/target/linker agnostic, best-long-term, no workaround. **Critical finding (cycle-20 exploration, the plan-lock gate doing its job):** the prior "converged" neutral `ImportSurface` JSON store carries `mangledName`+kind+linkage+**library** but **NO function signature** — so it can only LIBRARY-RESOLVE externs a program ALREADY declared; it CANNOT make `#include <stdio.h>` *declare* `puts` for you. Real C gets the signature from the **header**. So **goal-1 ("call shipped externs without inline re-declaration, exactly like C") = SHIPPED HEADERS merged via a system include path**, NOT the neutral store. **Locked design:** (a) `#include <stdio.h>` — a NEW angle-bracket include grammar form — resolves a per-language header on a config `SemanticConfig.shippedLibDirs` **system search path** (the analogue of C's `/usr/include`; distinct from `"local.h"` = self-dir + includeDirs); (b) the shipped header is parsed **by the language's OWN grammar** and merged via the **EXISTING include-following resolver** (`ConfigDrivenImportResolver.resolveIncludeFollowing`), so the symbol becomes a real declaration **with its signature**; (c) the **EXISTING FF5 `synthesizeFfiFromSourceDecls`** (step 2.5) annotates the library (`externLibraryByFormat` default + the existing per-symbol `"lib.dll"` override the header can carry). **Agnostic by construction:** signatures are inherently language-specific, so the signature SOURCE is a per-language header dialect parsed by that language's grammar — zero `if (lang=="c")`; the search path + the headers are config. **Build sequence:** (1) angle-bracket `#include <...>` grammar form in `c-subset.lang.json` + a system-vs-local distinction in the `imports` config block (today only the quote form exists; `<` lexes as `LtOp`); (2) `SemanticConfig.shippedLibDirs: vector<string>` field (`semantic_config.hpp`) + loader (`grammar_schema_json.cpp`, mirror `externLibraryByFormat`) + c-subset declares its dirs; (3) resolver: the angle form searches `shippedLibDirs` (new `ResolutionContext` field) → resolves the shipped header → merges via the existing `loadFile` path; (4) WIRE `shippedLibDirs` from `SemanticConfig` → `UnitBuilder` → `ResolutionContext` (today production passes NO include dirs — `program.cpp` never calls `addIncludeDir`); (5) shipped header(s) under `src/dss-config/shippedLibs/<platform>/` (c-subset prototypes, e.g. `puts`); (6) **RUNTIME corpus pin** `examples/c-subset/shipped_include_puts/` — `#include <stdio.h>` + `puts("hello")` with **NO inline extern** → compiles + runs + prints (hello_puts minus the decl; the end-to-end goal-1 proof); (7) **goal-2** (D-FFI-HEADER-VALIDATION-OPTIONAL): a program extern conflicting with a shipped-header decl fails loud on signature mismatch. **Closure gates:** runtime corpus runs (goal-1 end-to-end); fail-loud on a missing system header (new `F_*` from 0x501A) + on a goal-2 signature conflict; agnosticism (header via the language's grammar + config search path — no language/format/target identity branch); strict red-on-disable tests. **The neutral cross-language symbol→library catalog (the old converged design) is DEFERRED** — `D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC` stays OPEN for it (a later refinement so a header need not carry the library inline). [SINCE CLOSED ✅ 2026-06-06, v0.0.2 V2-2 — the neutral JSON descriptor landed; see the FF5 row + §0.] If goal-2 grows the cycle too large, goal-1 lands + goal-2 stays pinned (the user's "at least pinned" floor). **Original 2026-06-03 converged design (neutral-store; superseded for goal-1 by the signature finding) retained below for the audit trail.** ⏳ Originally SCHEDULED stepper step 13.8 (2026-06-03). Closes the **D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC** (partial — the library-catalog half stays open) + **D-FFI-HEADER-VALIDATION-OPTIONAL** anchors. Lets a program call shipped-library externs (libc/kernel32/libSystem stdlib symbols) **without inline `extern` re-declaration**, via descriptors the engine ingests — language-agnostically. **Converged design (2026-06-03 feature-dev session):** (1) **Shared neutral store, keyed by PLATFORM not source-language** — `src/dss-config/shippedLibs/<platform>/*.json` (e.g. `linux-x86_64/libc.json`, `windows-x86_64/{kernel32,msvcrt}.json`) — this platform-keyed layout **supersedes** the `D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC` anchor's illustrative `ffi-libs/<lib>.json` path (one store, not two) — holds cross-language platform libs described **once** (libc's surface is identical for C / Rust / a DSL — keying by source-language would duplicate it + re-couple a language-neutral fact, the very thing the anchor forbids). `shippedLibs/<language>/*.json` is reserved ONLY for genuinely language-specific runtimes (a language's own stdlib), not cross-language platform libs. (2) **Language config references, does not own** — new `SemanticConfig.shippedLibDirs: vector<string>` declares an **ordered, deterministic search path** (the integrated repo platform store + the language's own runtime dir if any + optional custom/external dirs). The language *chooses which libs it exposes*; it does not author the descriptions. (3) **Neutral JSON `ImportSurface` format** — a `.json` array of `ImportSurface` rows (mangledName/kind/visibility/linkage; the row's 5th field `libraryPath` is supplied by the descriptor's store location, not duplicated per-row) consumed by ONE universal reader, same `DiagnosticCollector` + `dssShippedLibVersion` + path-pointer-diagnostic loader discipline as `target_schema_json` / `object_format_schema_json`. `.h` stays supported as ONE dialect-by-extension (the kept `c_header_parser`), **not** the canonical format — bucket-1 declared descriptor + bucket-2 universal ingestion, **zero `if (lang == "c")`**. (4) **Lazy resolution** — a descriptor is read only when a program extern is NOT satisfied by its own source declaration; the resolver searches `shippedLibDirs` in order, reads the matching descriptor, and feeds the **existing `ingest()` matcher** (canonical-name index → `ExternDeclRef` match → FF4 mangle → `FfiMetadata` to `HirFfiMap`). **Reuses the entire proven back half verbatim** — `ImportSurface`, `ingest()`'s match/mangle/annotate, `FfiMetadata`, the binary_readers/c_header_parser dialect readers all already exist; FF11 is a discovery front-door + a neutral JSON reader onto them, NOT a new pipeline. Duplicate-symbol policy + deterministic recursive-scan order both already handled by `ingest()` (first-source-wins + `F_FfiIngestDuplicateSymbol` warning) / `readCHeaderDirectory` (alphabetical). **Out of scope** (anchored): per-platform store auto-selection by active `(target, format)` (D-FFI-SHIPPED-LIB-PLATFORM-SELECT); a non-C dialect reader (e.g. `.rs-sig`) lands when its first language arrives (D-FFI-SHIPPED-LIB-DIALECT-READER). |

Post-v1 reserved:
- FF7 Itanium demangler (C++ Linux/macOS)
- FF8 MSVC demangler (Windows)
- FF9 Header parser preprocessor support (`#include`, `#define` with arguments)
- FF10 Reverse-direction header emission

Substrate tier (5-agent review) for FF3 (ABI catalog).

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Header sources for libc — ship pre-reduced headers or read system headers? | **Pre-reduced headers** were the v1 design — shipped through FF2 in `src/dss-config/ffi-headers/{libc,libsystem,msvcrt,kernel32}/*.h`. **Removed OPT2 cycle 1 (2026-06-03)** as no production caller used them (source-extern-authority via `synthesizeFfiFromSourceDecls` won). FF2 parser substrate (`readCHeader` / `readCHeaderFromText`) stays for post-v1 system-header parsing OR opt-in source-signature validation against shipped headers. System-header parsing remains reserved post-v1. |
| 2 | C++ FFI in v1? | **No.** C-only. C++ Itanium / MSVC demangling reserved post-v1. |
| 3 | Preprocessor support in header mode? | **Limited** — typedef + struct + extern + simple `#define`-only-macros. No `#include` chasing (use the pre-reduced headers); no function-like macros. |
| 4 | TLS variable ingestion from binaries? | **Yes** — TLS extern globals emit `ExternGlobal` with `linkage: Tls` flag; linker LK5 handles. |
| 5 | Static archive (`.a` / `.lib`) full vs lazy ingestion? | **Lazy** — only members referenced by the program are pulled. |
| 6 | Weak symbol semantics? | Honored (some libc functions are weak — e.g. `__libc_start_main` aliases). |
| 7 | Versioned symbols (ELF `@GLIBC_2.34`)? | Ingested verbatim; linker LK6 honors the version requirement. |
| 8 | Diagnostic namespace? | `F_*`: `F_UnknownFormat`, `F_CorruptedBinary`, `F_UnresolvedHeader`, `F_AbiMismatch`, `F_MissingSymbol`, `F_NameMangling`. |

---

## 5. Acceptance criteria

- [ ] ELF / PE / Mach-O / ar readers produce `ImportSurface` matching `nm` / `dumpbin` / `objdump` oracles for libc / libSystem / msvcrt across all 6 (OS × arch) targets. (✅ ELF done 2026-06-01; PE/MachO anchored.)
- [x] ✅ C header mode parses pre-reduced libc headers cleanly; extern decls land in HIR with correct calling convention. (FF2 closed 2026-06-01.)
- [x] ✅ ABI catalog drives correct call-instruction sequences (per-platform smoke: hello_puts produces `"hello\r\n"` via msvcrt on Windows host — the `printf`/`%d` variadic path landed with **FC12** — the SysV/Win64/AAPCS64/Apple variadic ABI, see [`23-full-c-plan`](./23-full-c-plan%20-%20tbd.md)). (FF3 + FF4 closed 2026-06-01; FF6 hello_puts closed 2026-06-02.)
- [x] ✅ FF6 end-to-end Windows: c-subset program calling `puts` from msvcrt compiles + links + runs correctly. (2026-06-02 — first DSS-produced binary that prints.) **Cross-host `puts` (2026-06-09, the `#include puts("hello")` 3-OS string-FFI capstone — `examples/c-subset/shipped_include_puts` prints `"hello\n"` + exits 42):** **Linux libc `puts` ✅ RUNS** — RUNTIME-PROVEN LOCALLY on linux-x86_64 (WSL) AND linux-arm64 (qemu-aarch64; `strace` shows `write(1,…)` THEN `exit_group(42)` = libc `exit(3)` flush precedes exit), re-confirmed on the native ubuntu CI legs; **macOS libSystem `puts` ✅ RUNS** — RUNTIME-PROVEN on real Apple Silicon (run 27212153591, `macos-clang-release` ctest #206 `shipped_include_puts` Passed → "hello\n"+exit 42; the FIRST macOS execution of `puts("hello")`→stdout — the Mach-O `_exit`=flushing-C-`exit(3)` + Model-3 libSystem resolution confirmed at runtime). Unblocked by D-LK10-ENTRY-ARM64 ✅ + D-LK10-ENTRY-MACHO-EXIT ✅ + the per-host CI runners (ubuntu-x86_64, ubuntu-24.04-arm, macos-latest) + Model 3 shipped-lib FFI + ELF dynamic `.rodata` + the libc-`exit(3)` hosted-exit spine (plan-00 §0.2 D12+D13). Variadic `printf` ⏳ gated on ML7 cycle 2 stack-args closure + variadic ABI substrate.
- [ ] No `nm` / `dumpbin` / `objdump` invocation in production pipeline (oracles only). (✅ achieved — pipeline uses shipped headers + FFI metadata, no external tool invocation.)

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Pre-reduced libc headers drift from system reality | Medium | Medium | CI runs symbol-existence pin against the actual platform libc on every host runner (read the host binary, assert every header-declared symbol is present). |
| ABI quirks (macOS x86_64 SysV deviations; Win64 vector-arg shadow space) | High | High | Per-tuple golden tests calling a known-good external function and asserting argument register values via a hand-coded "ABI probe" stub. |
| Binary reader bugs allow corrupt FFI ingestion to proceed | Medium | High | Strict validation pass + `F_CorruptedBinary` fail-loud; refuse to ingest unparseable inputs. |
| C++ interop demanded earlier than planned | Medium | Medium | Itanium + MSVC demanglers can land as a focused v1.x PR (FF7/FF8); design is well-known. |
| `.a` / `.lib` archive lazy-loading edge cases (weak symbols, undefined-reference cascades) | Medium | Medium | Implement two-pass: first pass marks all transitive references; second pass pulls members. |

---

## 7. Sequencing

```
08.5 (lattice) ─► 09-hir (extern nodes) ─► FF1 ─► FF2 ─► FF3 ─► FF4 ─► FF5 ─► FF6
                                                                                    │            ▲
                                                                                    ▼            │ (hard prereq)
                                                                          14-linker LK6      14-linker
                                                                          (imports)          D-LK10-ENTRY
                                                                                             (runnable spine)
```

FF7 / FF8 (C++ mangling) and FF9 (preprocessor) are post-v1 follow-ups.
