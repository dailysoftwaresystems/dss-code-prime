# FFI / Precompiled-Library Ingestion — Sub-Plan (11)

> Owns **object-file readers** (ELF / PE / Mach-O / COFF / `.a` / `.lib` / `.so` / `.dll` / `.dylib`) + **C / C++ minimal header parser** + **ABI catalog** + **name-mangling readers** (Itanium / MSVC) + **extern-decl ingestion** into [HIR](./09-hir-plan%20-%20ok.md) + the [core type lattice](./08.5-substrate-prep-plan%20-%20ok.md). Per the user's mandate: "We will be able to read pre-compiled libraries (FFI)."

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-critical. Without FFI we can't call libc → no `printf` → no useful binary. |
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

`ingest()` is the public entry point:

```cpp
HirIngestResult ingest(std::vector<std::filesystem::path> libraries,
                       std::vector<std::filesystem::path> headers,
                       TargetTriple target,
                       CompilationUnit& cu);
```

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
| ~~FF2~~ | C header mode + parser reuse | ✅ **CLOSED 2026-06-01** (post-fold #2 heavy — 7-agent second-pass folds). `src/ffi/c_header_parser.{hpp,cpp}` exposes `readCHeader[FromText,Shipped](...) -> std::expected<vector<ImportSurface>, HeaderReadError>` — thin orchestration over the c-subset frontend (tokenize + parse + semantic + lower-to-HIR), walking the resulting HIR module's `moduleDecls` and emitting one `ImportSurface` per `ExternFunction` / `ExternGlobal`. Typedef absorbed into type system (no row); function body / non-extern global / `#include` / future HirKinds each fail loud with a distinct F_* code (the residual default arm surfaces the numeric `HirKind` value in the diagnostic detail). **9 closed-enum `HeaderReadErrorKind` variants** mapped to **9 F_* codes** (`0x5008-0x500F`) via single closed-table `kHeaderReadErrorTable` (replaces two parallel switches per simplifier #2 fold, mirrors `kTargetArchMachineCodes`). Underlying c-subset diagnostics (P_*/L_*/S_*/H_*) + ConfigDiagnostics (C_*) drain to the caller's reporter; the first underlying cause is INLINED into the FF2 wrap so the verdict stays self-sufficient under `--suppress=C_*` (silent-failure C2 fold). Source span attribution on every FF2-layer rejection via `HirSourceMap::tryGet`. `slurpFile` checks `in.bad() \|\| ss.bad()` for mid-read I/O + output-side OOM truncation (silent-failure C2+C3). `forwardConfigDiagnostics` hoisted to `grammar_schema.hpp` (silent-failure C1). `findShippedFfiHeader` uses `fs::path::is_absolute()` + per-component `..` check — rejects POSIX absolute, Windows `C:\` drive letter, UNC paths, leading `.`, and embedded `..` components (silent-failure CRITICAL-1 closed Windows-absolute bypass). New `C_InvalidShippedFfiHeaderPath` (0xC034) distinct from `C_InvalidLanguageName`. New `HeaderReadErrorKind::InvalidShippedPath` (kind 8) + `F_HeaderInvalidShippedPath` (0x500F) split caller-API path bugs from genuine file-not-found (silent-failure H2). `loweringResult` nullptr-checked before `->ok` deref (silent-failure H1). `cSignature` field DROPPED from `ImportSurface` (type-design Q6: no producer, no consumer; D-FF2-1 will re-add as `optional<FnSigTypeId>` if FF3 surfaces a row-attached need). 33 tests + 131/131 ctest. **Anchored sub-rows**: **~~D-FF2-1~~** ✅ **EVALUATED 2026-06-01 BY FF3, TRIGGER NOT MET — ANCHOR RETIRED.** FF3 resolves typed signatures off the HIR side-table (`HirAttribute<FfiMetadata>` → MIR pre-pass), not via a field on the `ImportSurface` row. `cSignature` stays permanently dropped from the row shape, **D-FF2-2** `HeaderReadError` struct field for `SourceLocation` (today: span lives on the ParseDiagnostic only; trigger: first field report where the struct-side caller needs the span without rep traversal), **D-FF2-3** cross-tier `extern int x = 5;` initializer reject (proper fix is in `cst_to_hir.cpp::lowerExternDecl` OR `semantic_analyzer`'s externDecl handling; trigger: next cycle touching either TU; F_*/kind to be added paired with the fix), **D-FF2-4** behavioral test for `forwardConfigDiagnostics` in FF2's GrammarLoadFailed arm (trigger: when broken-grammar test seam lands), **D-FF2-5** behavioral test for `HeaderHasUnsupportedTopLevel` (trigger: c-subset grows a non-Skip top-level production), **D-FF2-6** behavioral test for `InternalInvariant` (trigger: c-subset frontend grows a recoverable Error path at module scope), **D-FF1-NEST** split `src/ffi/binary_readers/` subdirectory (trigger: second binary-reader TU lands), **D-FF5-INGESTION-SOURCE** `IngestionSource` variant in `ingest.hpp` (trigger: FF5 session), **D-FF6-HEADER-DIR-READER** `readCHeaderDirectory` for multi-file libraries (trigger: first library needing > 3 header files). |
| ~~FF3~~ | ABI catalog | ✅ **SUBSTRATE CLOSED 2026-06-01 (post-fold #2 heavy).** `src/ffi/abi/abi_catalog.{hpp,cpp}` exposes `resolveAbi(target, format, reporter) -> expected<AbiTuple, AbiResolveError>` where `AbiTuple = { CallConv, TargetCallingConvention const* }`. Closed-table `kAbiCatalog` keys 6 (target.name, format.kind) tuples → (CallConv, expected cc-name); 4 currently shipped (x86_64+Elf/Pe/MachO, arm64+Elf) + 2 anchored-but-unshipped (arm64+Pe → ms_arm64 [PLACEHOLDER CallConv per D-FF3-4], arm64+MachO → apple_arm64). `consteval` uniqueness on (targetName, formatKind). Closed-table error-kind mapping (3 F_Abi* codes at 0x5010-0x5012). `TargetSchema::callingConventionByName` (O(1) hashmap) used for cc lookup. WASM (CcWasm) + SPIR-V (CcSpirv) abi-models return cc=nullptr; defensive `FormatAbiModelMismatch` arm. 16 tests (4 shipped happy paths + 2 anchored-cc-not-shipped + 2 abi-model dispatch + 2 abi-model mismatch defensive + 1 unknown-target + catalog count + 6 round-trip). **SUBSTRATE ONLY** — the silent-failure surface where `src/lir/lir_regalloc.cpp::317` hardcodes `callingConventionIndex = 0` REMAINS OPEN until **D-FF3-3** wires `resolveAbi` into `compileOneTarget` → `compileSingleUnit` → `allocateRegisters`. Anchored sub-rows: **D-FF3-1** layout-side (pointer size, integer sizes, struct padding, va_arg, small-aggregate thresholds — trigger first non-LP64 target); **D-FF3-2** `AbiTuple::cc` raw-pointer → cc index reshape with `shared_ptr<TargetSchema const>` co-ownership (trigger: FF5 ingest stores AbiTuple past local schema scope); **D-FF3-3** wire `resolveAbi` into `compileOneTarget` → `compileSingleUnit` → `allocateRegisters`; delete `lir_regalloc.cpp::317` `callingConventionIndex = 0` hardcode (trigger: FIRST `--target=...:pe64-x86_64-windows` e2e test OR FF6 libc smoke test, whichever comes first — neither needs FF5 ingest if the corpus declares externs explicitly); **D-FF3-4** distinct `CallConv::CcMSARM64` variant pairing with `ms_arm64` cc name (trigger: arm64-Windows corpus); **D-FF3-5** test rewrite when `apple_arm64` cc row ships in arm64.target.json; **~~D-FF3-Coherence~~** ✅ **EVALUATED 2026-06-01, ANCHOR RETIRED.** The "silent-failure" surface this anchor was designed to close does NOT exist: `TargetSchemaData::validate()` at `src/core/types/target_schema.cpp:901-908` already rejects any cc row whose `argGprs`/`argFprs`/`returnGprs`/`returnFprs`/`callerSaved`/`calleeSaved`/`linkRegister`/`stackPointer` names don't all resolve in `registerIndex`. The loader at `target_schema_json.cpp:903-908` forwards `validate()` failures as ConfigDiagnostics, causing `loadFromText` to return `std::unexpected`. A target.json with a paste-error cc cannot load; FF3 never sees it. Two regression-trip tests pin this contract: `SchemaLoaderRejectsPasteErrorRegistersInCc` + `SchemaLoaderRejectsCcRoleClassMismatch` in `tests/ffi/test_abi_catalog.cpp`. If a future cycle adds programmatic schema construction bypassing JSON (e.g. for fuzzing or test fixtures), this anchor's trigger reopens — re-add an FF3-tier defensive pass at that time. |
| ~~FF4~~ | C name mangling (per-platform underscoring) | ✅ **SUBSTRATE CLOSED 2026-06-01 (post-fold #2 heavy).** `src/ffi/mangling/c_mangle.{hpp,cpp}` exposes `applyCMangling(canonical, format) -> string`, `unapplyCMangling(decorated, format) -> string`, `cFormatAddsLeadingUnderscore(format) -> bool`. Closed-table `kCManglingRules` one row per `ObjectFormatKind` variant — size pinned via `kObjectFormatKindTable.rows.size()` (existing canonical variant-count anchor), order pinned via `consteval kCManglingRulesAlignedWithEnum`. v1 decoration rules: ELF/PE/Wasm/SPIR-V/Unknown → no decoration; MachO → leading underscore (`_printf`; arch-agnostic since the convention is the same on 32-bit and 64-bit Mach-O). Symmetric apply/unapply pure functions; conservative unapply for missing-prefix inputs. No new diagnostic codes (FF4 is pure data transform; no failure modes for known formats). 21 tests including enum-driven round-trip + `AllFormatKindsHaveExplicitRulePin`. Post-fold #3 also added `AbiResolveErrorKind::Count_` sentinel (closes silent-failure H3: `LastVariant + 1u` would silently accept a new variant appended without a row) and consolidated 5 `countCode` test-helper consumers into `tests/test_support/diagnostic_count.hpp`. **SUBSTRATE ONLY** — neither FF2 (header parser) nor FF1 (binary readers) calls FF4 today; the FFI ingest boundary (FF5) is the right tier to apply per-format decoration after canonical names land in `HirFfiMap`. Anchored sub-rows: **D-FF4-1** 32-bit PE cdecl `_func` / stdcall `_func@N` (trigger: first 32-bit PE target); **D-FF4-2** hoist `CManglingRule` struct from anon namespace to public header (trigger: D-FF4-1 close, which adds a second decoration axis [suffix] beyond the boolean underscore flag); **~~D-FF4-3~~** ✅ **CLOSED 2026-06-01 (post-fold #3 parallel-tracks cycle).** New `unapplyCManglingStrict(decoratedName, format, reporter) -> std::expected<std::string, MangleError>` in `src/ffi/mangling/c_mangle.{hpp,cpp}`. Returns the canonical undecorated name on success; emits `F_MangleMissingExpectedPrefix` (0x5014) + `MangleErrorKind::MissingExpectedPrefix` on a decorated-format input lacking the per-format prefix (e.g. Mach-O symbol passed in as `printf` instead of `_printf`). For no-decoration formats (ELF/PE/Wasm/SPIR-V/Unknown), strict mode is structurally a no-op success path. Empty input → empty success. Closed-table `kMangleErrorTable` keyed on `MangleErrorKind::Count_` sentinel — matches the codebase pattern from `kAbiResolveErrorTable`. 7 new tests including enum-driven round-trip across every `ObjectFormatKind` variant via `kObjectFormatKindTable.rows`. Used by FF5 ingest where the format-kind is authoritative; the existing `unapplyCMangling` stays conservative for callers without authoritative format context; **D-FF4-Apply** apply FF4 at the FFI ingest boundary (FF2/FF1 → HirFfiMap thread-through, trigger: FF5 integration cycle). |
| FF5 | `ingest()` + `HirAttribute<FfiMetadata>`    | Public entry point; CU integration; linker hand-off. **Hand-off half ✅ landed 2026-05-30 (LK6 cycle 2d)**: the HIR `HirAttribute<FfiMetadata>` → MIR pre-pass (`collectExterns`) → MIR/LIR side-tables → assembler → linker thread-through closed via plan 14 D-LK6-6. Tests build the FFI map manually through `HirFfiMap::set(...)`. **Ingestion half ⏳ pending FF1/FF2**: `ingest(libraries, ...)` reading `.so` / `.dll` / `.dylib` / `.a` / `.lib` headers to POPULATE the FFI map for real corpora — gated on FF1 (binary readers) + FF2 (C header parser). |
| FF6 | libc smoke test                             | End-to-end: declare `extern printf(...)`; compile + link + run a c-subset program that calls it; assert correct output on all 6 (OS × arch) targets. |

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

- [ ] ELF / PE / Mach-O / ar readers produce `ImportSurface` matching `nm` / `dumpbin` / `objdump` oracles for libc / libSystem / msvcrt across all 6 (OS × arch) targets.
- [ ] C header mode parses pre-reduced libc headers cleanly; extern decls land in HIR with correct calling convention.
- [ ] ABI catalog drives correct call-instruction sequences (per-platform smoke: `printf("hello %d\n", 42)` produces correct stdout via a native binary).
- [ ] FF6 end-to-end: c-subset program calling `printf` from libc compiles + links + runs correctly on all 6 (OS × arch) targets.
- [ ] No `nm` / `dumpbin` / `objdump` invocation in production pipeline (oracles only).

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
                                                                                    │
                                                                                    ▼
                                                                          14-linker LK6 (imports)
```

FF7 / FF8 (C++ mangling) and FF9 (preprocessor) are post-v1 follow-ups.
