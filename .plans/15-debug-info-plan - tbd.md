# Debug-Info — Sub-Plan (15)

> Owns **DWARF 5** writer (ELF + Mach-O) and **PDB** writer (PE), plus the source-position preservation chain that threads `SourceSpan` from CST through HIR / MIR / LIR / bytes. Without this layer, `gdb` / `lldb` / WinDbg / Visual Studio cannot step through our binaries. Per the hermetic invariant, no `dsymutil` / `mspdb*` invocations — we own the writers.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 acceptance includes "step through line numbers"; locals reserved post-v1. |
| Predecessors  | ✅ [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) (byte-offset stability — AS1–AS6 closed 2026-05-29). ✅ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) (section placement — LK1–LK10 closed 2026-05-30). |
| Successors    | [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) codesigns include debug sections. |
| Scope         | **Bounded.** DB1–DB12. v1: line numbers via DWARF + PDB; locals (variable lifetimes) post-v1. CFI / unwind in v1 (required for crash + profilers). |

---

## 1. Motivation

Production-grade tooling requires debug info. Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no `dsymutil` / `mspdb*`. Source-position fidelity through CST → HIR → MIR → LIR → bytes is the data spine.

---

## 2. Design

### 2.1 Files

```
src/debuginfo/
├── source_map.hpp / .cpp        # SourceMap: per-instruction → SourceSpan
├── dwarf/
│   ├── dwarf5_writer.hpp / .cpp # DB2: skeleton + abbrev + str + info
│   ├── dwarf5_types.hpp / .cpp  # DB3: Universal type→DIE mapper over TypeInterner + optional debugInfo facet
│   ├── dwarf5_line.hpp / .cpp   # DB-line: Line-program state machine (bucket-2 universal emitter)
│   ├── dwarf5_cfi.hpp / .cpp    # DB4: .eh_frame / .debug_frame CFI (bucket-2 universal stream)
│   └── dwarf5_locs.hpp / .cpp   # DB-loc: .debug_loclists (post-v1)
├── pdb/
│   ├── pdb_msf.hpp / .cpp       # DB5: MSF container (stream-of-streams)
│   ├── pdb_tpi.hpp / .cpp       # DB6: type / id streams (TPI + IPI)
│   ├── pdb_dbi.hpp / .cpp       # DB7: DBI + module + lines streams
│   ├── pdb_gsi.hpp / .cpp       # Global/Public symbol streams
│   └── pdb_pdata.hpp / .cpp     # DB8: Win64 SEH .pdata / .xdata (bucket-2 universal stream)
└── macho/
    └── macho_unwind.hpp / .cpp  # DB9: __TEXT,__unwind_info compact unwind
```

**Removed from the rev-1/2 sketch** (was: `src/debuginfo/lang/toy_debug.cpp` + `c_subset_debug.cpp` + `tsql_subset_debug.cpp` — per-language `.cpp` for type→DIE mapping). Per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) Decision #4's three-bucket rule (clarified 2026-05-29): a per-language `.cpp` tree IS bucket-3 identity-branching, which the thesis forbids. The replacement is a single bucket-2 universal mapper:

- **`dwarf5_types.hpp / .cpp`** consumes `TypeInterner` directly. Every core `TypeKind` (Bool/I*/U*/F*/Char/Byte/Void/Struct/Union/Tuple/Array/Slice/Enum/Ptr/Ref/FnPtr/FnSig/…) has a deterministic DW_TAG mapping declared once.
- **Per-language extension types** (C# Boxed/Delegate/GcRef, T-SQL Varchar<N>/RowType, VHDL Std_Logic, shader Sampler/Texture, etc.) declare their DW_TAG choice in an OPTIONAL `debugInfo` facet on the source `.lang.json` (bucket-1: `{ extensionKindName: "VarcharN", dwarfTag: "DW_TAG_string_type", encoding: ... }`). A language with no `debugInfo` facet falls back to a generic DW_TAG_unspecified_type for its extensions.
- The mapper has **zero `if (schema.name() == ...)` branches**. New language onboarding = new `debugInfo` facet (or zero work if the core lattice suffices).

**Bucket-2 streams** named explicitly here so the substrate doesn't drift into per-format `.cpp`:
- `dwarf5_line.cpp` — DWARF 5 line-program state machine (universal opcode encoder over `DW_LNS_*` / `DW_LNE_*` / special-opcode tables; opcode tables are JSON-declared in `debug-info.format.json` if any per-platform tweaks are needed; v1 standard DWARF 5 has no per-platform variation, so the tables can be compile-time).
- `dwarf5_cfi.cpp` — `.eh_frame` / `.debug_frame` CIE/FDE encoder; CFI opcodes (`DW_CFA_*`) are a stream emitter, target-blind once the target's register-number assignment is read from `*.target.json`'s `debugInfo.dwarfRegisters[]` facet.
- `pdb_pdata.cpp` — Win64 SEH unwind-info encoder; per-arch `UnwindCode` opcode set is JSON-declared (x86_64 SET_FPREG / SAVE_NONVOL / ALLOC_LARGE / ALLOC_SMALL / PUSH_MACHFRAME etc.; ARM64 has its own packed set).
- `macho_unwind.cpp` — Apple `__unwind_info` compact-encoding emitter; per-arch encoding constants in JSON, the page-walker is universal.

### 2.2 Source-position preservation chain

Every node carries the originating `SourceSpan`:

- **CST**: already carries via `Token::span` + `Node::span`.
- **HIR**: `HirSourceMap` (`HirAttribute<HirSourceLoc>` — bundles `BufferId` + `SourceSpan`) populated during CST→HIR (HR5 ✅ 2026-05-26; per `09-hir-plan` §2.6).
- **MIR**: `MirAttribute<SourceSpan>` populated during HIR→MIR (per `12-mir-lir-plan`).
- **LIR**: `LirAttribute<SourceSpan>` populated during MIR→LIR.
- **Bytes**: assembler emits `(byte_offset → LirInstId)` map (per `13-assembler-plan` §2.8).

Chained: `byte_offset → LirInstId → SourceSpan` via two attribute lookups. The `SourceMap` type packages this chain into a `byte_offset → (file_id, line, col)` table consumed by both DWARF and PDB writers.

### 2.3 DWARF 5

Sections written (per `14-linker-plan` ELF / Mach-O writer integration):

| Section | Purpose |
|---|---|
| `.debug_info` | DIE tree: compile units, subprograms, types, variables |
| `.debug_abbrev` | DIE shape templates |
| `.debug_str` / `.debug_line_str` | Interned strings |
| `.debug_line` | Line-number state machine |
| `.debug_aranges` | Address range index |
| `.debug_loclists` | Variable location lists (post-v1) |
| `.debug_rnglists` | Non-contiguous range tracking |
| `.eh_frame` | CFI for stack unwind (always-on; required for profilers + exception handlers even if our v1 languages don't throw) |

DIE types emitted v1:

- `DW_TAG_compile_unit` — root
- `DW_TAG_subprogram` — functions
- `DW_TAG_formal_parameter` — params (location attribute reserved post-v1)
- `DW_TAG_variable` — globals + locals (location post-v1)
- `DW_TAG_base_type` — primitives (mapped from core lattice via `DW_ATE_*`)
- `DW_TAG_pointer_type`, `DW_TAG_array_type`
- `DW_TAG_structure_type`, `DW_TAG_union_type`, `DW_TAG_enumeration_type`
- `DW_TAG_member`, `DW_TAG_enumerator`
- `DW_TAG_typedef`, `DW_TAG_const_type`, `DW_TAG_volatile_type`

Line program: standard DWARF 5 state machine (`DW_LNS_copy`, `DW_LNS_advance_pc`, `DW_LNS_advance_line`, etc.). Reuses `SourceBuffer` file mapping for file IDs.

**Mach-O specifics**: DWARF sections in `__DWARF` segment; we emit inline (no `dsymutil` linked-DWARF; users can run dsymutil externally if they want bundled `.dSYM`).

### 2.4 PDB

Microsoft Program Database. MSF (Multi-Stream File) container layout:

- Stream 1: PDB info (version, signature, age, NamedStream table)
- Stream 2: TPI — type index records (`LF_POINTER`, `LF_ARRAY`, `LF_STRUCTURE`, `LF_CLASS`, `LF_UNION`, `LF_ENUM`, `LF_PROCEDURE`, `LF_ARGLIST`, `LF_FIELDLIST`, `LF_MEMBER`, `LF_ENUMERATE`)
- Stream 3: DBI — debug information stream (module list, section contributions)
- Stream 4: IPI — id index (related to TPI)
- Per-module streams: per-CU symbols + lines

Symbol records (`S_*`): `S_GPROC32` / `S_LPROC32` (functions), `S_LDATA32` / `S_GDATA32` (variables), `S_REGREL32` (param/local at frame-pointer offset, post-v1), `S_FRAMEPROC`, `S_END`.

Line info via `DEBUG_S_LINES` subsections: per-function `(line → address)` mapping.

PE integration: PE's `IMAGE_DEBUG_DIRECTORY` carries an `IMAGE_DEBUG_TYPE_CODEVIEW` entry containing the PDB GUID + age + path; the PDB file lives beside the EXE/DLL (sidecar).

Reference: LLVM's PDB writer (study, do not link). Our impl ~3-5k LOC.

### 2.5 CFI / unwind info

Required even when no exceptions: profilers + crash dumps walk the stack via these tables.

| Platform | Format |
|---|---|
| ELF | `.eh_frame` (DWARF CFI: FDE/CIE) |
| PE x86_64 | `.pdata` + `.xdata` (Win64 SEH UNWIND_INFO) |
| PE ARM64 | `.pdata` + `.xdata` (`_IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY`) |
| Mach-O | `__TEXT,__unwind_info` (compact-unwind) + `__TEXT,__eh_frame` (DWARF CFI fallback) |

Stack frame prologue/epilogue from `12-mir-lir-plan` LIR materialization drives the CFI/SEH content.

### 2.6 Per-language type DIE mapping

Each shipped language has a `<lang>_debug.cpp` that maps:
- Core lattice → standard DIE shapes (universal)
- Language extension types → nominal `DW_TAG_typedef` / `DW_TAG_structure_type` (per-language registration; see `08.5-substrate-prep-plan` §2.2)

For PDB the equivalent: extension types → `LF_STRUCTURE` records with language-qualified names.

### 2.7 Diagnostic namespace

`B_*` (B for "debug" — `D_*` is reserved for the driver) — `B_DwarfVerifyFailed`, `B_PdbStreamCorrupt`, `B_CfiInconsistent`, `B_LineProgramOverflow`, `B_SourceSpanMissing` (an emitted LIR instruction had no `LirAttribute<SourceSpan>`), `B_TypeDieMissing` (HIR type with no DWARF/PDB mapping for the target language).

Note on naming collision: PDB CodeView **wire-format constants** in §2.4 use `S_GPROC32` / `S_LPROC32` / etc. — those are CodeView record-type identifiers from Microsoft's public spec, NOT diagnostic codes. Diagnostic codes in this plan are exclusively `B_*`. Disambiguation enforced by the code-review pattern: anything in a `DSS_DIAGNOSTIC(...)` macro must start with `B_`.

### 2.8 Source-name preservation through transpilation

When source language A is transpiled to language B and then compiled (per `10-source-translation-plan`), debug info points at A. Implementation: `HirAttribute<TranspileOrigin>` carries the original `SourceSpan`; debug-info writer prefers `TranspileOrigin.sourceSpan` over the post-transpile span.

---

## 3. PR breakdown

| PR  | Title                                          | Scope |
|-----|------------------------------------------------|-------|
| DB1 | Source-position map plumbing (HIR/MIR/LIR/bytes) | Touch points in 09, 12, 13. `SourceMap` type assembled in this PR. |
| DB2 | DWARF 5 base                                   | `.debug_str` + `.debug_abbrev` + `.debug_info` skeleton + `.debug_line` state machine. Skeleton subprograms. |
| DB3 | DWARF type DIEs                                | Core lattice → DW_TAG_*. Per-language `<lang>_debug.cpp` glue. |
| DB4 | DWARF unwind (`.eh_frame` CFI)                 | FDE/CIE generation from LIR prologue/epilogue. |
| DB5 | PDB MSF container                              | Stream-of-streams layout, header, NamedStreams. |
| DB6 | PDB type records (TPI / IPI)                   | Per-language type-record emission. |
| DB7 | PDB symbols + lines (DBI + per-module)         | `S_GPROC32` / `S_LPROC32` + DEBUG_S_LINES. |
| DB8 | Win64 SEH `.pdata` / `.xdata`                  | UNWIND_INFO from LIR prologue/epilogue. |
| DB9 | Mach-O compact-unwind                          | `__TEXT,__unwind_info` from LIR prologue/epilogue. |
| DB10| Per-language debug glue                        | toy / c-subset / tsql-subset type-DIE mappings. |
| DB11| Round-trip tests via oracles                   | `llvm-dwarfdump --verify` / `llvm-pdbutil pretty` as TEST oracles; golden text comparison. |
| DB12| End-to-end debugger smoke                      | Scripted gdb / lldb / cdb step-through per CI runner; assert expected line stops. |

Substrate tier for DB1 (source-position contract). Feature tier for writers.

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | DWARF version: 4 or 5? | **5.** Supported by gdb 9+, lldb 12+. |
| 2 | Variable lifetime tracking (locals) in v1? | **No** — line numbers only. Locals → post-v1 (G-533). |
| 3 | macOS `.dSYM` bundle? | **No** — embed DWARF inline in Mach-O. Bundle generation reserved post-v1. |
| 4 | Split DWARF (`.dwo`)? | **Post-v1.** |
| 5 | PDB embed vs sidecar? | **Sidecar `.pdb`** (Microsoft convention). |
| 6 | PDB GUID strategy? | **BLAKE3 of section contents** (16 bytes) + monotonic age counter (matches `14-linker-plan` build-id philosophy). |
| 7 | DWARF line-program for transpiled source — point at original or transpiled file? | **Original.** Per `HirAttribute<TranspileOrigin>`. Debugger lands on user-authored line. |
| 8 | Debug info for shader (SPIR-V) and WASM? | **Out of v1.** SPIR-V `OpLine`-based debug + WASM DWARF-in-custom-section reserved with their own plans. |

---

## 5. Acceptance criteria

- [ ] c-subset corpus binaries with `-g` produce DWARF that passes `llvm-dwarfdump --verify` on Linux x86_64 + ARM64.
- [ ] c-subset corpus binaries with `-g` produce DWARF on Mach-O that lldb can step through on macOS x86_64 + ARM64.
- [ ] c-subset corpus binaries with `-g` produce PDB that passes `llvm-pdbutil pretty` on Windows x86_64 + ARM64.
- [ ] Scripted debugger step-through reaches every expected line on each CI runner.
- [ ] CFI / unwind tables let a synthetic crash (segfault) produce a correct backtrace on every platform.
- [ ] All emission is hermetic — no `dsymutil` / `mspdb*` invocation.
- [ ] Deterministic output: rebuilding produces byte-identical `.debug_*` sections and `.pdb` files.

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| PDB format complexity (LLVM impl ~10k LOC) | High | High | Phased delivery (MSF first / TPI second / DBI+lines third); test oracle pinning at each step. |
| DWARF CFI correctness (mis-emitted unwind silently breaks profilers + exception handlers) | High | High | Per-function unwind golden tests + crash-on-line debugger smoke test in CI from DB4 day one. |
| Per-arch unwind divergence | Medium | Medium | Separate PRs DB8 (Win64 SEH) + DB9 (Mach-O compact-unwind); platform-specific test runners. |
| Source-position map breaks during optimizer passes | Medium | High | Verifier in MIR / LIR rejects instructions missing `SourceSpan`; optimizer must preserve or explicitly mark "synthetic, no source." |

---

## 7. Sequencing

```
12-mir-lir + 13-assembler ─► DB1 (source-position chain)
                                    │
                         ┌──────────┼─────────────────┐
                         ▼          ▼                 ▼
                        DB2/DB3   DB5/DB6/DB7      (per-lang DB10)
                        (DWARF)    (PDB)
                         │          │
                         ▼          ▼
                        DB4        DB8                DB9
                       (CFI ELF)  (SEH PE)        (Mach-O compact)
                         │          │                 │
                         └──────────┼─────────────────┘
                                    ▼
                                  DB11 ─► DB12 (debugger smoke)
                                            │
                                            ▼
                               16-codesign-publish (sections covered by signature)
```
