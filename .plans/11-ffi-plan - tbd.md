# FFI / Precompiled-Library Ingestion — Sub-Plan (11)

> Owns **object-file readers** (ELF / PE / Mach-O / COFF / `.a` / `.lib` / `.so` / `.dll` / `.dylib`) + **C / C++ minimal header parser** + **ABI catalog** + **name-mangling readers** (Itanium / MSVC) + **extern-decl ingestion** into [HIR](./09-hir-plan%20-%20ok.md) + the [core type lattice](./08.5-substrate-prep-plan%20-%20ok.md). Per the user's mandate: "We will be able to read pre-compiled libraries (FFI)."

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ✅ **MOSTLY DONE 2026-06-02.** FF1-ELF + FF2 + FF3 + FF4 + FF5 + FF6 (Windows / msvcrt.puts) all CLOSED. **First DSS-produced binary that prints**: `examples/c-subset/hello_puts/` calls msvcrt's `puts`, writes "hello\r\n" to captured stdout, exits 42 — all asserted byte-for-byte by examples_runner via Slice 1's stdout pipe. **Remaining**: FF1-PE + FF1-MachO binary readers (anchored — triggered by first PE / macOS corpus needing extern resolution via shipped binaries rather than headers); FF6 cross-host equivalents (Linux/macOS/ARM64) gated on D-LK10-ENTRY-ARM64 + D-LK10-ENTRY-MACHO-EXIT. |
| Predecessors  | ✅ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) (core type lattice — complete). ✅ [`09-hir-plan`](./09-hir-plan%20-%20ok.md) (`ExternFunction` / `ExternGlobal` nodes — HR1–HR11 ✅ 2026-05-26..28: the declarations landed at HR4 and the `FfiMetadata` / `HirFfiMap` side-table FFI ingestion populates is in place HR4–HR5, round-trip-serialized by HR7's `.dsshir` `@ffi(...)`; **HR9 added `externDecl`→ExternFunction/ExternGlobal lowering** (c-subset `externFuncTail` grammar split + `externDecl` semantics rule + `lowerExternDecl`), so the extern *surface* now lowers — the FFI *metadata* population (linkage/ABI/mangling from real binaries/headers) is still THIS plan's job; HR10 added tsql-subset lowering, HR11 ✅ done 2026-05-28 (multi-language CU lowering) — plan 09 complete). |
| Successors    | ⏳ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) LK6 consumes extern symbol declarations for import-table generation. |
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

Full C99 preprocessor is **out of v1** — the header mode accepts typedef + struct + extern + simple macros only. Real headers (glibc / Windows SDK) often need the preprocessor; v1 accepts hand-curated reduced headers shipped under `src/dss-config/ffi-headers/`.

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
| ~~FF2~~ | C header mode + parser reuse | ✅ **CLOSED 2026-06-01.** `src/ffi/c_header_parser.{hpp,cpp}` exposes `readCHeader[FromText,Shipped](...) -> expected<vector<ImportSurface>, HeaderReadError>`. Reuses the c-subset frontend; rejects function bodies / non-extern globals / `#include` / unknown HirKinds each with a distinct F_* code. 9 `HeaderReadErrorKind` variants ↔ 9 F_* codes (0x5008-0x500F) via closed-table dispatch. Shipped `src/dss-config/ffi-headers/libc/{stdio,stdlib}.h`. **Out of scope**: extern initializer detection (cross-tier — D-FF2-3); SourceLocation on HeaderReadError struct (D-FF2-2). Detailed fold history in `memory/project_ff2_*`. **Anchors**: D-FF2-2 SourceLocation struct field (trigger: first field report needing struct-side span); D-FF2-3 `extern int x = 5;` reject (trigger: next cycle touching `cst_to_hir.cpp::lowerExternDecl`); D-FF2-4/5/6 behavioral tests for unreachable arms (each gated on a future c-subset evolution); D-FF1-NEST split `src/ffi/binary_readers/` (trigger: 2nd binary-reader TU). |
| ~~FF3~~ | ABI catalog | ✅ **SUBSTRATE CLOSED 2026-06-01.** `src/ffi/abi/abi_catalog.{hpp,cpp}` exposes `resolveAbi(target, format, reporter) -> expected<AbiTuple, AbiResolveError>`. Closed-table `kAbiCatalog` keys (target.name, format.kind) → (CallConv, cc-name); 6 rows (4 shipped + 2 anchored-but-unshipped). 4 F_Abi* codes at 0x5010-0x5013 via `Count_`-sentinel closed-table. Defensive cc-register-resolvability pass at the FF3 boundary catches paste-error cc rows that bypass schema-loader validate(). **Wiring D-FF3-3** ✅ closed 2026-06-01 (post-fold #5 parallel-tracks cycle): `resolveAbi` now threads from `compileOneTarget` → `compileSingleUnit(ccIndex)` → `allocateRegisters(ccIndex)` → `allocateOneFunc`, replacing the `lir_regalloc.cpp::317` `callingConventionIndex = 0` hardcode. Detailed fold history in `memory/project_ff3_*`. **Out of scope**: layout-side fields (D-FF3-1); programmatic-construction lifetime hardening (D-FF3-2); cross-arch CallConv variant split (D-FF3-4). **Anchors**: D-FF3-1 layout fields (trigger: first non-LP64 target); D-FF3-2 cc-pointer reshape (trigger: FF5 stores AbiTuple past local schema scope); D-FF3-4 distinct CcMSARM64 (trigger: arm64-Windows corpus); D-FF3-5 test rewrite (trigger: apple_arm64 cc ships). |
| ~~FF4~~ | C name mangling (per-platform underscoring) | ✅ **CLOSED 2026-06-01.** `src/ffi/mangling/c_mangle.{hpp,cpp}` exposes `applyCMangling` / `unapplyCMangling` / `unapplyCManglingStrict` / `cFormatAddsLeadingUnderscore`. Closed-table `kCManglingRules` one row per `ObjectFormatKind`, size pinned via `kObjectFormatKindTable.rows.size()`. v1 rules: ELF/PE/Wasm/SPIR-V/Unknown → no decoration; MachO → leading `_`. Strict-mode variant (D-FF4-3) emits `F_MangleMissingExpectedPrefix` (0x5014) on decorated-format input lacking the prefix. Applied at FF5's ingest boundary (D-FF4-Apply closed). Detailed fold history in `memory/project_ff3_ff4_*`. **Out of scope**: 32-bit PE stdcall `@N` suffix (D-FF4-1). **Anchors**: D-FF4-1 PE32 stdcall (trigger: first 32-bit PE target); D-FF4-2 `CManglingRule` struct public hoist (trigger: D-FF4-1, which adds the suffix axis). |
| ~~FF5~~ | `ingest()` + `HirAttribute<FfiMetadata>` | ✅ **CLOSED 2026-06-01** + **EXTENDED 2026-06-02** (FF6 prep — `synthesizeFfiFromSourceDecls` sibling for source-declared externs). `src/ffi/ingest.{hpp,cpp}` exposes BOTH `ingest(sources, externs, target, format, ffiMap, reporter) -> HirIngestResult` (header / binary validated) AND `synthesizeFfiFromSourceDecls(externs, importLibrary, target, format, ffiMap, reporter) -> HirIngestResult` (source-declared — the language grammar's extern decl IS the signature authority). Both share the FF3 abi-resolve gate + FF4 applyCMangling kernel; both produce the same `HirIngestResult` shape. `synthesize` is the canonical c-subset path: per-language `SemanticConfig.externLibraryByFormat: unordered_map<string,string>` (keyed on `objectFormatKindName(format.kind())`) declares the runtime library identity; c-subset ships `pe → msvcrt.dll`, `elf → libc.so.6`, `macho → /usr/lib/libSystem.B.dylib`. New diagnostic `F_FfiNoImportLibraryForFormat = 0x5019` (unsuppressable) fires when the active format has no entry. Composes FF1 + FF2 + FF3 + FF4 at the ingest boundary: reads each `IngestionSource` (variant over BinaryLibrarySource / CHeaderSource / CHeaderDirSource — closes D-FF5-INGESTION-SOURCE), aggregates ImportSurface rows, FF4-unapplies binary-reader rows, matches against caller-supplied `ExternDeclRef[]` (HirNodeId + canonicalName, decoupled from SemanticModel), FF4-applies to produce linker-visible mangledName, populates HirFfiMap. `readCHeaderDirectory` closes D-FF6-HEADER-DIR-READER. FF3 cross-validation gates the (target, format) pair. **Wiring** (2026-06-02): `compileSingleUnit` step 2.5 between HIR and MIR calls `synthesizeFfiFromSourceDecls` over `CstToHirResult.externDecls` (the new `HirExternRecord` accumulator populated by `lowerExternDecl` for both ExternFunction + ExternGlobal arms). 22 tests in `tests/ffi/test_ingest.cpp` (15 ingest + 7 synthesize + happy-path / Mach-O / ELF / ExternGlobal / mixed-validity / config-gap pins). **Out of scope**: end-to-end runtime print (D-FF6-RUNTIME-PRINT — requires CRT init OR ML7 stack args + kernel32 WriteFile). 5 new JSON loader tests in `tests/core/test_grammar_schema.cpp`. **Anchors**: D-CSUBSET-EXTERN-LIBRARY-SYNTAX (per-symbol `extern "<lib>" int foo();` override; trigger: first source language wanting per-extern library override); D-FFI-HEADER-VALIDATION-OPTIONAL (compose `ingest()` BEFORE `synthesize()` to validate source-declared signatures against shipped headers; trigger: first language opting into header-driven sig validation); D-FFI-ABI-GATE-HELPER (extract the FF3+FF4 head block shared by ingest+synthesize; trigger: 3rd caller). |
| ~~FF6~~ | libc smoke test | ✅ **HELLO-PUTS LANDED 2026-06-02 (Windows host).** First DSS-emitted PE binary that calls into msvcrt.dll's `puts`, prints "hello\r\n" to captured stdout, and exits 42 — all asserted byte-for-byte by `examples_runner` (`examples/c-subset/hello_puts/` with `runOn: ["windows"]` + `expectedStdout: "hello\r\n"`). Diagnosis caught the prior cycle's hypothesis was wrong: msvcrt requires NO user-side CRT init for puts (its own `_DllMainCRTStartup` self-inits stdio at DLL attach). The actual 0xC0000005 blocker had TWO layers — (a) user main missing Win64 shadow-space + alignment-bias prologue (closed via D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: new `callPushBytes` cc field + `hasCalls` ML7 scan + `alignedSizeWithBias` unification); (b) MIR→LIR emitting `call` (E8 direct) for the extern `puts` when it should emit `call_indirect_via_extern` (FF 15 indirect-via-IAT) — closed via new `MnemonicSlot::CallIndirectViaExtern` + `externSymbols` set on Lowerer. `hello_puts` is also the first 2-DLL PE binary (kernel32 ExitProcess + msvcrt puts) AND the first DSS binary exercising stdout-pipe capture (D-LK6-2A-MULTI-LIBRARY-PIN ✅ closed; D-FF6-HELLO-PUTS-PROMOTION-PIN ✅ closed; D-FF6-RUNTIME-PRINT ✅ closed). **Remaining out-of-scope**: cross-host (Linux/Mac/ARM64) hello_puts equivalents — gated by D-LK10-ENTRY-ARM64 + D-LK10-ENTRY-POSIX-RUN-HARNESS + the FF1-PE/FF1-MachO binary readers already landed. `printf` with floating-point args + locale-using calls (`fopen`, etc.) gated by D-LK10-CRT-INIT-INVOKE (anchored, NOT blocker for puts). Kernel32-direct print (CRT-free) gated by D-ML7-2.2 stack-args + opaque-pointer typing — anchored D-LK10-KERNEL32-WRITE-PATH. Hard prerequisite D-LK10-ENTRY (the runnable-binary spine) ✅ Stage 1 closed 2026-06-02. |

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
| 1 | Header sources for libc — ship pre-reduced headers or read system headers? | **Pre-reduced headers** in `src/dss-config/ffi-headers/{libc,libsystem,msvcrt,kernel32}/*.h`. Hermetic + small + version-stable. System-header parsing reserved post-v1. |
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
- [x] ✅ ABI catalog drives correct call-instruction sequences (per-platform smoke: hello_puts produces `"hello\r\n"` via msvcrt on Windows host — `printf` variant with `%d` formatter awaits variadic ABI in ML7 extension). (FF3 + FF4 closed 2026-06-01; FF6 hello_puts closed 2026-06-02.)
- [x] ✅ FF6 end-to-end Windows: c-subset program calling `puts` from msvcrt compiles + links + runs correctly. (2026-06-02 — first DSS-produced binary that prints.) Cross-host (Linux/macOS/ARM64) ⏳ gated on D-LK10-ENTRY-ARM64 + D-LK10-ENTRY-MACHO-EXIT + per-host CI runners. Variadic `printf` ⏳ gated on ML7 cycle 2 stack-args closure + variadic ABI substrate.
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
