# In-tree Linker — Sub-Plan (14)

> Owns the **object-format engine** (one language-blind C++ engine that reads JSON-configured format schemas) and the **linker engine** (symbol resolution, relocation application, section layout, per-platform metadata). Consumes (bytes, relocations, symbols) from the [in-tree assembler](./13-assembler-plan%20-%20tbd.md); produces final-form binaries.
>
> Per the user's mandate ("we will be the process from source to targets"), **no `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` invocation**. This is the single largest backend chunk of v1.
>
> **Universal-compiler thesis extension (rev 3 — 2026-05-29).** Object formats are **100% config-driven** — mirroring [`GrammarSchema`](./05-parser-plan%20-%20ok.md) on the frontend and [`TargetSchema`](./12-mir-lir-plan%20-%20ok.md) on the backend ISA tier. Each object format (ELF, PE/COFF, Mach-O, WASM, SPIR-V) is a JSON file in `src/dss-config/object-formats/<name>.format.json` declaring section taxonomy, symbol-table layout, relocation kinds, per-format metadata sections, and (where applicable) dynamic-linking constructs. ONE language-blind C++ engine reads the JSON to emit bytes — there is NO per-format C++ writer. Adding a new object format = drop a new JSON file, no engine edits. Honors thesis decision #4 ("config-driven everything") end-to-end.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **in flight.** LK4 substrate ✅ landed 2026-05-29 on `feature/as-1` — `ObjectFormatSchema{Unknown=0,Elf,Pe,MachO,Wasm,Spirv}` + format-blind `link(AssembledModule, TargetSchema, ObjectFormatSchema, reporter) → LinkedImage` engine + cross-side reloc-taxonomy unifier (`src/core/substrate/relocation_table.hpp` shared with TargetSchema) + `K_*` diagnostic family at 0x8xxx. Per-format byte emission (LK1 ELF / LK2 PE / LK3 Mach-O) + LK5–LK10 ⏳ pending. **Plan rev 3 (2026-05-29): object formats are JSON-configured** (mirrors `GrammarSchema` + `TargetSchema` pattern) — one engine, no per-format C++. |
| Predecessors  | ✅ [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) (bytes + relocations) — AS1–AS6 closed 2026-05-29. ⏳ [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) (extern symbol declarations from precompiled libs). |
| Successors    | ⏳ [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) (LC_CODE_SIGNATURE / PE security directory placeholders filled post-link). ⏳ [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) (debug sections placed alongside code). |
| Scope         | **Bounded.** v1: LK1–LK10. v1 acceptance: link c-subset corpus → ELF / PE / Mach-O on every {OS × arch}. WASM (LK8) + SPIR-V (LK9) post-v1 skeletons. **v1.x: LK11** — cross-CU linking (couples with [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) CU6). |

---

## 1. Motivation

Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no `ld` / `link.exe` / `ld64` invocation. Four payoffs:

1. **Apple-host-free local dev.** Mach-O emission via the in-tree config-driven engine eliminates the `osxcross`/`xcrun` dependency.
2. **WASM + SPIR-V unification.** Same plumbing covers native, browser, GPU.
3. **Reproducibility.** Deterministic build-id; no embedded timestamps/paths/version-strings.
4. **Config-driven object formats.** Every supported format (ELF / PE / Mach-O / WASM / SPIR-V) is a JSON file, not C++. The engine is target-blind + format-blind; format-specific knowledge lives in `*.format.json` schemas validated at load time. Same thesis decision #4 discipline that makes the frontend onboard a new source language via one JSON file (`GrammarSchema`) and the backend ISA tier onboard a new processor via one JSON file (`TargetSchema`).

---

## 2. Design

### 2.0 Config-driven object formats (rev 3)

Mirroring `src/dss-config/sources/*.lang.json` (frontend) and `src/dss-config/targets/*.target.json` (backend ISA), each object format is one JSON file under `src/dss-config/object-formats/`:

- `elf.format.json` — ELF64 little-endian, section taxonomy (`.text`/`.data`/`.rodata`/`.bss`/`.debug_*`/…), program-header types (PT_LOAD/PT_DYNAMIC/PT_INTERP/PT_GNU_STACK/PT_GNU_RELRO), dynamic-section entries (DT_NEEDED/DT_HASH/DT_GNU_HASH/DT_RELA/…), relocation kinds (R_X86_64_64/R_AARCH64_ABS64/…), build-id placement (`.note.gnu.build-id`).
- `pe.format.json` — Section table (file/virtual alignment), import table (`.idata` + IAT), unwind info (`.pdata` + `.xdata`), exception/debug directory entries, signing window (`IMAGE_DIRECTORY_ENTRY_SECURITY` placeholder).
- `macho.format.json` — Segments (file/virtual alignment), load commands (LC_SEGMENT_64, LC_DYLD_INFO, LC_DYSYMTAB, LC_FUNCTION_STARTS, LC_UUID, LC_CODE_SIGNATURE placeholder), chained fixups (Apple Silicon), bind/lazy/rebase opcode tables.
- `wasm.format.json` — Module sections (type/import/function/table/memory/global/export/start/elem/code/data), name section convention, custom section taxonomy.
- `spirv.format.json` — Module header (magic/version/generator/bound/schema), section ordering convention (capability/extension/extinstimport/memorymodel/entrypoint/executionmode/...), debug info layout.

The schema vocabulary is universal: every format declares its `sections[]` (kind + flags + alignment), its `symbolTable` (entry layout, kinds, binding/visibility encodings), its `relocations[]` (kind name → operator + bit-width + signed-ness + applies-to-section-kind), its `metadataSections[]` (build-id / signing window / debug-id placement), and its `imports`/`exports` model (for formats that have one). The ENGINE is one set of headers/cpp files under `src/link/` reading `ObjectFormatSchema`.

**The bucket-1 vs bucket-2 split** (per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) Decision #4's three-bucket rule, clarified 2026-05-29):

- **Bucket 1 — declarative layout (JSON):** section/symbol/reloc/field byte layouts; section kind taxonomy; relocation formulas (operator + bit-width + sign); load-command / program-header field tables; chained-fixups data-page descriptors; build-id placement; signing-window placement. The schema says "these bytes go in these slots."
- **Bucket 2 — universal algorithm over declared vocabulary (one C++ engine):** Mach-O bind / lazy-bind / rebase opcode tries (variable-length opcode streams keyed by per-segment data pages, encoded as JSON-declared opcode tables walked by ONE stream emitter); chained-fixups page-encoding walker; ELF GNU_HASH bucket computation; GOT / PLT slot synthesis; relocation application (mutator dispatch via `schema.relocationKind(name)` → bucket-1 formula); per-format metadata section emission. The engine walks the JSON vocabulary and emits the bytes; **no `if (format == "macho")` branch anywhere in the engine**.

Bucket-3 drift (per-format `.cpp` directories) is the failure mode this plan explicitly rejected — see §2.1 below for the deleted `objfmt/elf/`, `objfmt/pe/`, `objfmt/macho/` trees.

**Shared with plan 13 (assembler) — the relocation-taxonomy unifier.** Two-schema decomposition of the (arch × format) relocation matrix, joined by an opaque `uint32_t` tag:

| Schema | Owns | Consumed by |
|---|---|---|
| `*.target.json` `relocations[]` | The **formula+tag**: opaque `uint32_t tag → { isPCRelative, width, addendWidth, ... }`. **✅ target-side substrate landed 2026-05-29 (AS1 cycle 1)**: `TargetRelocationInfo { name; RelocationKind kind; formula }` (`RelocationKind` strong-id), `TargetSchema::relocations()` / `relocationInfo(RelocationKind)` / `relocationByName(name)` accessors, parallel `relocationKindIndex` for O(1) lookup, validate() rules (unique non-zero `kind`, unique non-empty `name`, type-strict `formula`). **✅ assembler-side emission landed 2026-05-29 (AS4 combined cycle)**: both walkers (`x86_variable` + `fixed32`) emit `Relocation` entries when a `SymbolRef` operand reaches a symbol-bearing slot (`Disp32` / `Imm26`); per-wire `relocationKind` declared in JSON and resolved at load time. x86_64 ships `rel32` + `abs64` + `abs32`; arm64 ships `call26` + `adr_prel_pg_hi21` + `abs64`. AssembledFunction.relocations now populates end-to-end through the c-subset corpus path. Decomposition into the richer `{isPCRelative, width, addendWidth, ...}` row is still owed by **LK6 / D-AS4-4** (current substrate stores `formula` as opaque text — sufficient for documentation cross-reference + plan 14's formula-string-to-formula-enum mapping decision). Wire-declared addend bias is anchored at plan 13 §3.1 D-AS4-4. | Assembler (plan 13) emits the tag; linker applies the formula via `relocation_apply.cpp` |
| `*.format.json` `relocations[]` (this plan) | The **format-name → tag** mapping: e.g. `"R_X86_64_PC32" → tag 1`, `"IMAGE_REL_AMD64_REL32" → tag 1`, `"X86_64_RELOC_BRANCH" → tag 1` (all three are PC-relative 32-bit signed — one formula, three names) | Linker (LK6 reloc-apply) uses the format name when writing the object file's reloc table |

Same opaque `uint32_t` tag joins both sides. **No per-(arch×format) C++ enum anywhere.** Cross-referenced from plan 13 §2.6 — AS4 (target-schema reloc rows) and LK6 (format-schema name→tag mapping) land in the same review window so the integer assignments don't drift. The cross-cycle "linker engine mismatch with assembler relocation kinds" risk (rev 1/2's §6 High/High) is closed structurally by both sides reading from the same opaque-tag namespace.

Loader pattern mirrors `TargetSchema::loadShipped`/`GrammarSchema::loadShipped`:

```cpp
auto fmt = ObjectFormatSchema::loadShipped("elf");
LinkResult bin = link(input, *fmt, /*architecture=*/"x86_64", reporter);
```

`O_*` diagnostic family at **0x5xxx** for object-format validation (per the central nibble registry in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.2 — the shipped `parse_diagnostic.cpp` has explicitly reserved 0x5xxx for `O_*` since the cross-plan diagnostic-band cleanup). Earlier rev claimed `0xC00x` which silently collided with the shipped `C_*` config family (0xC001..0xC033 — already in production). The corrected allocation mirrors `L_*` 0xBxxx for LIR, `I_*` 0xAxxx for MIR, `H_*` 0xFxxx for HIR.

**Cross-cutting consequence**: the v1 acceptance for "link c-subset to ELF/PE/Mach-O across {OS × arch}" is met by **3 JSON files + 1 engine**, not 3 separate C++ writers. Adding a new format (e.g., COFF for embedded; XCOFF for AIX; a-out for retro) = new JSON file, no engine edits.

### 2.1 Files

```
src/link/
├── linker.hpp / .cpp                   # Engine: resolve + relocate + lay out
├── object_format_schema.hpp / .cpp / _json.cpp
│                                       # ObjectFormatSchema + loadShipped (mirrors TargetSchema)
├── object_format_data.hpp              # detail::ObjectFormatData PODs (sections / symbols / relocs / metadata)
├── object_writer.hpp / .cpp            # ONE engine reading ObjectFormatSchema; emits bytes for any declared format
├── symbol_table.hpp / .cpp             # Defined / undefined / weak / local / hidden — format-blind
├── section.hpp                         # SectionFlags, SectionKind, layout primitives — format-blind
├── relocation_apply.hpp / .cpp         # Mutator dispatch via shared TargetSchema::relocationInfo(tag) — same opaque schema-tag the assembler emits per plan 13 §2.6 (NO per-(arch×format) C++ enum)
├── tls.hpp / .cpp                      # Per-platform TLS lowering — config-driven via schema's TLS model
└── build_id.hpp / .cpp                 # BLAKE3-based deterministic build-id; placement schema-driven

src/dss-config/object-formats/
├── elf.format.json                     # LK1 — ELF64 LE; section/symbol/reloc/dynamic schema
├── pe.format.json                      # LK2 — PE/COFF; section table + import/export + unwind schema
├── macho.format.json                   # LK3 — Mach-O 64; load-command + chained-fixups schema
├── wasm.format.json                    # LK8 (post-v1 skeleton)
└── spirv.format.json                   # LK9 (post-v1 skeleton)
```

The old `src/link/objfmt/elf/`, `objfmt/pe/`, `objfmt/macho/`, `objfmt/wasm/`, `objfmt/spirv/` per-format-writer C++ trees (rev 1/2 sketch) are DELETED — collapsed into `*.format.json` schemas + the single `object_writer.hpp` engine.

### 2.2 Engine

Input per CU:
- Section bytes (one buffer per logical section: `.text`, `.data`, `.rodata`, `.bss`, `.debug_*`, …)
- Relocation list (offset, target symbol, kind, addend)
- Symbol table (name, binding, visibility, section, offset, size)

Output per CU:
- One image file in the target format (ELF / PE / Mach-O / WASM / SPIR-V).

Engine flow:

1. **Merge symbol tables** across all input CUs. **v1 is single-CU per image** (per [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) §2.4); the merge logic stubs to identity. Multi-CU input lands in **LK11** (v1.x), coupled with CU6 — see §2.12.
2. **Resolve symbols.** Each undefined ref resolves to either a defined symbol in the same image (intra-module) or to an FFI import (entry in the image's import table).
3. **Lay out sections.** Apply per-format conventions (ELF: page-aligned segments per PT_LOAD; PE: section table with file/virtual alignment; Mach-O: segments with file/virtual alignment).
4. **Apply relocations.** Per (arch × format) mutator from `13-assembler-plan` §2.5 taxonomy.
5. **Emit per-format metadata.** ELF dynamic, PE imports/exports, Mach-O LC_DYLD_INFO, etc.
6. **Compute build-id.** BLAKE3 over section contents → emitted to `.note.gnu.build-id` (ELF), `LC_UUID` (Mach-O), `IMAGE_DEBUG_DIRECTORY` (PE).
7. **Reserve signing window** for Mach-O / PE per [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md).

### 2.3 ELF (Linux, Android)

- ELF64 little-endian; class ELF64; OS/ABI = SYSV; e_machine = `EM_X86_64` / `EM_AARCH64`.
- Executables: program headers (PT_LOAD × N, PT_DYNAMIC, PT_INTERP, PT_GNU_STACK, PT_GNU_RELRO), DT_NEEDED for dynamic deps.
- Shared libs: same shape, no PT_INTERP.
- Sections: `.text`, `.rodata`, `.data`, `.bss`, `.symtab`, `.strtab`, `.dynsym`, `.dynstr`, `.rela.dyn` / `.rela.plt`, `.dynamic`, `.gnu.hash`, `.note.gnu.build-id`, `.eh_frame`, `.debug_*`.
- PLT/GOT synthesis for dynamic calls.
- GNU_HASH for dynamic symbol lookup (faster than SysV hash).

### 2.4 PE/COFF (Windows)

- DOS stub + PE header (`PE\0\0` + IMAGE_FILE_HEADER + IMAGE_OPTIONAL_HEADER64).
- Subsystem per `06-artifact-profile`: CLI → `IMAGE_SUBSYSTEM_WINDOWS_CUI`, GUI → `IMAGE_SUBSYSTEM_WINDOWS_GUI`, lib → DLL bit set in characteristics.
- Section table + raw section data: `.text`, `.rdata` (read-only data + import directory), `.data`, `.bss`, `.pdata` (function table for unwind), `.xdata` (unwind info), `.reloc` (base relocations).
- `.idata` for dynamic imports; IAT (Import Address Table) and ILT (Import Lookup Table) pairs per imported DLL.
- `.edata` for exports (lib only).
- Base relocations (R_X86_64_RELATIVE-equivalent: `IMAGE_REL_BASED_DIR64`).
- `IMAGE_DIRECTORY_ENTRY_SECURITY` placeholder reserved for `16-codesign-publish-plan` (Authenticode signature appended post-link).

### 2.5 Mach-O (macOS, iOS)

- mach_header_64; magic `MH_MAGIC_64`; CPU type `CPU_TYPE_X86_64` / `CPU_TYPE_ARM64`; filetype `MH_EXECUTE` / `MH_DYLIB`.
- Load commands: LC_SEGMENT_64 × N (`__TEXT`, `__DATA_CONST`, `__DATA`, `__LINKEDIT`), LC_SYMTAB, LC_DYSYMTAB, LC_LOAD_DYLIB × N (deps), LC_LOAD_DYLINKER, LC_UUID, LC_BUILD_VERSION (Apple Silicon), LC_FUNCTION_STARTS, LC_DATA_IN_CODE, LC_CODE_SIGNATURE (placeholder).
- LC_DYLD_INFO_ONLY for x86_64 Mach-O (legacy bind/lazy-bind/rebase/export opcode trie).
- **Chained fixups** (newer macOS 11+, all iOS): LC_DYLD_CHAINED_FIXUPS. Required for Apple Silicon binaries. v1 emits both modes; selects based on target.
- Mach-O fat-archive (`MH_UNIVERSAL`) — reserved post-v1; v1 ships one binary per arch.

### 2.6 WASM (web) — skeleton in v1, full in v1.x

See [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md). Linker duties = merge function/table indices, dedupe types, generate imports/exports.

### 2.7 SPIR-V (GPU) — skeleton in v1, full in v1.x

See [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md). Linker duties = emit module-level capability/extension/memory-model headers + per-entry-point `OpEntryPoint` decls + interface variables.

### 2.8 TLS lowering

| Platform | TLS access mechanism |
|---|---|
| Linux x86_64 | `%fs:` segment; `R_X86_64_TPOFF32` relocation; static / initial-exec / general-dynamic / local-dynamic models per ELF TLS ABI |
| Linux ARM64 | `tpidr_el0` register; `R_AARCH64_TLSDESC_*` relocations |
| Windows x86_64 | TEB at `%gs:0x58`; `IMAGE_REL_BASED_DIR64` + `_tls_index` lookup |
| Windows ARM64 | Same TEB model via TPIDR_EL0 + `_tls_index` |
| macOS x86_64 | `_thread_vars` indirection via `__thread_vars` Mach-O section |
| macOS ARM64 | Same via TPIDR_EL0 + `__thread_vars` |

v1: implement initial-exec model for static binaries; general-dynamic reserved post-v1.

### 2.9 Static vs dynamic linking

- **Static**: every symbol resolves within the image; no `.dynamic` / `.idata`; output is one self-contained binary. libc still typically dynamic on Linux/macOS; static glibc is reserved post-v1.
- **Dynamic**: symbols may resolve to imports; per-format mechanism (ELF DT_NEEDED + PLT/GOT; PE IAT; Mach-O LC_LOAD_DYLIB + bind opcodes). v1 default for `cli` artifactProfile.

### 2.10 Diagnostic namespace

`K_*` (K for "linK") — `K_SymbolUndefined`, `K_SymbolRedefined`, `K_RelocationOverflow`, `K_RelocationKindMismatch`, `K_SectionOverlap`, `K_ImportUnresolved`, `K_InvalidLoadCommand` (Mach-O), `K_InvalidPeHeader`, `K_InvalidElfHeader`, `K_TlsModelUnsupported`. The engine emits these directly; the schema's `ObjectFormatSchema::validate()` separately emits `O_*` diagnostics at 0x5xxx for malformed/incomplete format JSON.

### 2.12 Cross-CU linking (v1.x — coupled with `08-compilation-unit-plan` CU6)

v1 ships **one `CompilationUnit` per output image**. The engine's "merge symbol tables across all input CUs" step (§2.2 flow item 1) is therefore an identity operation in v1 — there is only one CU to merge.

**LK11** (v1.x) lifts that assumption to enable:

- **Shared libraries / DLLs consumed by a separate `cli`** — each lib is a CU; the executable's CU resolves symbols against the libs' CUs at link time (not at FFI ingestion time, which is LK6 territory).
- **Incremental compilation** — each translation unit becomes a separate CU; LK11 combines them.

Substrate already in place by the time LK11 ships:

- `CompilationUnitId` provenance ([CU1](./08-compilation-unit-plan%20-%20tbd.md) L2) — every symbol's defining CU is identifiable.
- Cross-CU `NodeId` guard ([CU3](./08-compilation-unit-plan%20-%20tbd.md) D3) — wrong-CU `NodeAttribute<SymbolId>` access is fatal at substrate level.
- CU's `crossRefs` table ([CU3/CU4](./08-compilation-unit-plan%20-%20tbd.md)) — pre-resolved inter-tree edges; LK11 extends the same data model to inter-CU edges.

**Trigger** (when LK11 unblocks): the first artifact profile that requires multiple CUs in one image — typically when [`06-artifact-profile-plan`](./06-artifact-profile-plan%20-%20tbd.md) `lib`/`staticlib` outputs need to be consumed by a separate `cli` project, OR when incremental rebuild enters scope. Until then LK11 is reserved scope, and the v1 single-CU contract holds end-to-end.

**Out of scope for LK11**: shared symbol-table semantics across **different processes** (process-boundary IPC / DLL hot-reload). Those are runtime concerns owned by [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md).

### 2.11 Build-id

BLAKE3-256 hash of (section contents excluding the build-id field itself, in canonical order). Emitted to:
- ELF: `.note.gnu.build-id` (20 bytes for compat with GNU tooling; truncated SHA-1-shaped slot)
- Mach-O: `LC_UUID` (16 bytes)
- PE: `IMAGE_DEBUG_DIRECTORY` entry of type `IMAGE_DEBUG_TYPE_CODEVIEW` (16-byte GUID + monotonic age)

---

## 3. PR breakdown

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| ~~LK1 cycle 1~~ ✅ **relocatable .o landed 2026-05-29** | `elf.format.json` + ELF writer walker — first per-format byte-emission walker plugged into the format-blind linker engine. | **ET_REL minimal valid .o end-to-end.** New `src/dss-config/object-formats/elf64-x86_64-linux.format.json` — first shipped object-format JSON; declares ELF identity (class=ELFCLASS64, data=ELFDATA2LSB, machine=EM_X86_64=62), 5 sections (.text/.rela.text/.symtab/.strtab/.shstrtab) with sh_type/sh_flags/sh_addralign/sh_entsize, 3 relocations (R_X86_64_PC32=2, R_X86_64_64=1, R_X86_64_32=10) with format-side `nativeId`. New `ObjectFormatSectionInfo{kind,name,type,flags,addrAlign,entrySize}` schema row (closes D-LK4-2). `ObjectFormatRelocationInfo` grew `nativeId:u32` (validated `!= 0`). New `ElfIdentity{fileClass,dataEncoding,osabi,abiVersion,machine}` block on `ObjectFormatData` (populated only when `kind == Elf`; `validate()` requires non-zero class/data/machine). New `SectionKind::ShStrtab` enum entry (distinct from `Strtab` — section-name vs symbol-name table; `EnumNameTable` size 12). New `src/link/format/elf.{hpp,cpp}` ELF writer walker — emits Elf64_Ehdr + section header table + .text + .rela.text + .symtab + .strtab + .shstrtab byte-for-byte per gABI Ch. 4 + AMD64 psABI §4.4. `e_shnum` derived from headers array size (architect B-LK1-2 convergence — pre-fix was hardcoded literal `6`, would silently corrupt when sections grow). `K_NoMatchingObjectFormat=0x8003` re-added with full LK1 consumer enumeration in parse_diagnostic.hpp docstring. `link()` dispatch shell got format-keyed closed-enum switch over `ObjectFormatKind` — `Elf` arm calls `elf::encode`; `Unknown`/`Pe`/`MachO`/`Wasm`/`Spirv` arms fire `K_NoMatchingObjectFormat`. New `linkagePassed` snapshot-vs-current-errorCount gate in `link()` — skips walker dispatch when cross-reference unifier emitted any K_*; resets `resolvedFuncCount=0` to prevent ok() false-positive (architect Decision 4 convergence). Closes D-LK4-1 (ELF arm) + D-LK4-2. **7-agent review fold-in (inline)**: architect B-LK1-2 (e_shnum hardcode) → derived from headers array; architect Decision 4 + comment-analyzer (resolvedFuncCount inconsistency) → reset on linkage failure; silent-failure CRITICAL-2 + HIGH-3 (D-AS5-3 walker silent-skip) → fail-loud + hasModRm/hasImm32 guards; convention #1 (`ShStrtab` missing from enum-name round-trip test) → added; convention #2 + comment-analyzer (K_NoMatchingObjectFormat under-described) → 4-scenario docstring; comment-analyzer (fabricated "LK1 cycle 2" anchor + stale linker.hpp doc + misleading elf.hpp failure-channel claim) → rewritten; test-analyzer Gaps 3/4/5 → 6 new tests (`LinkagePassedGateSkipsWalkerOnSymbolUndefined`, Pe/MachO/Wasm/Spirv `K_NoMatchingObjectFormat` dispatch arm pins, D-AS5-3 `SymbolBearingSlotsReturnNullopt` + `Imm32SlotsCarryConcreteValue`). 14 new LK1 + D-AS5-3 tests. 113/113 ctest. **New deferred items**: D-LK1-1 (real symbol-name thread → LK7), D-LK3-1 (Mach-O segment+section split → LK3). |
| LK1 cycle 2 | `elf.format.json` ET_EXEC + program headers + ARM64 EM_AARCH64 row | ET_EXEC e_type, program headers, segment layout for ELF executable; second-arch onboarding (drop arm64-linux.format.json). |
| LK2 | `pe.format.json` + engine PE support             | Windows x86_64 + ARM64; exe + DLL; subsystem flag per artifactProfile; base relocations; .idata IAT. JSON-declared section table + import/export schema; engine reads it. |
| LK3 | `macho.format.json` + engine Mach-O support      | macOS x86_64 + ARM64; executables + dylibs; chained-fixups path; LC_DYLD_INFO legacy path; LC_CODE_SIGNATURE placeholder. JSON-declared load-command + chained-fixups schema; engine reads it. |
| ~~LK4 substrate~~ ✅ **substrate slice landed 2026-05-29** | `object_writer` engine substrate + `ObjectFormatSchema` | **Substrate-tier slice done. Format walkers (ELF/PE/Mach-O) anchored at LK1+.** New `src/link/object_format_schema.{hpp,cpp,_json.cpp}` mirrors `TargetSchema`'s shape — JSON-configured per-format descriptor at `src/dss-config/object-formats/<name>.format.json` (no shipped files yet; LK1+ adds them). Closed-enum vocabulary via `EnumNameTable<E,N>`: `ObjectFormatKind{Unknown=0,Elf,Pe,MachO,Wasm,Spirv}`, `SectionKind{Text,Rodata,Data,Bss,Symtab,Strtab,RelocTable,Dynamic,Note,Debug,Custom}`, `SymbolBinding{Local,Global,Weak}`, `SymbolVisibility{Default,Hidden,Protected,Internal}`. `ObjectFormatRelocationInfo{name, kind}` — format-side half of plan 13 §2.6 reloc-taxonomy unifier (the SAME opaque `RelocationKind` tag the assembler stamps; format owns the platform-native name like `R_X86_64_PC32` / `IMAGE_REL_AMD64_REL32`). New `ObjectFormatSchemaId` strong-id minted via shared `substrate::mintMonotonicId`. JSON loader `dssObjectFormatVersion=1` + `format.{name,version,kind}` + `relocations[{name,kind}]` with same discipline as TargetSchema (kind != 0, unique kind cross-row, unique name, fail-loud on duplicate / zero / non-integer kind / unknown kind name). New `K_*` diagnostic family at 0x8xxx (plan 00 §0.3 update): `K_SymbolUndefined=0x8001`, `K_RelocationKindMismatch=0x8002` (substrate-tier; per-format codes including `K_NoMatchingObjectFormat` for artifact-profile dispatch land alongside their LK* cycles, not as substrate dead-code). New `src/link/linker.{hpp,cpp}`: `link(AssembledModule, TargetSchema, ObjectFormatSchema, reporter) → LinkedImage`. `LinkedImage{format, bytes, expectedFuncCount, resolvedFuncCount, ok()}` — same parallel-index discipline that AssembledModule + LirAllocation use (`ok()` derived from `expectedFuncCount > 0 && resolved == expected`). Cycle-scope behavior: walks every relocation, fails loud if `kind` is missing from EITHER side (3-agent convergence: diagnostic message names BOTH sides when both miss — naming one would force a re-link to discover the other), fails loud on unknown symbol target INDEPENDENTLY of kind resolution (silent-failure C2 fix: BOTH diagnostics surface in one pass). Per-format byte emission (ELF/PE/Mach-O writers) is the D-LK4-1 scope. **7-agent review fold-in (inline)**: 3-agent convergence on joint-side reporting + silent-failure-hunter C2 on combined-error single-pass + type-design 85% on `Unknown=0` invalid-sentinel + comment-analyzer on `K_SymbolUndefined` doc accuracy + simplifier on `lir_pass_util::report` reuse (linker.cpp no longer reimplements the report shape) + test-analyzer on partial-resolution multi-func test + architect on D-LK4-3 FFI co-trigger (anchored). 18 LK4 tests: 5 enum + default-sentinel pin, 9 JSON loader happy/sad paths (missing/wrong version, unknown kind, duplicate name/kind, zero kind, non-array relocations, Unknown-sentinel rejected by validate), 9 linker tests (empty/intra-CU/no-relocs/unknown symbol/missing-on-format/missing-everywhere/partial multi-func/joint-side message/combined-error single-pass). 111/111 ctest. |
| LK4 cont. | format walkers + section/symbol emission        | Plug per-format byte-emission walkers into the substrate engine. (Slated for LK1+ as those format JSONs ship — substrate is structurally ready.) |
| LK5 | TLS lowering                                     | Per §2.8 table; initial-exec model on every platform. |
| LK6 | Dynamic linking + imports                        | PE IAT, ELF GOT/PLT, Mach-O bind opcodes. FFI imports from `11-ffi-plan` arrive here. |
| LK7 | Codesign hook                                    | Mach-O LC_CODE_SIGNATURE placeholder + PE attribute-cert reservation, both filled by `16-codesign-publish-plan`. |
| LK8 | `wasm.format.json` + engine WASM support (skeleton) | Module header + section framework; JSON-declared; full impl in `18-wasm-plan`. |
| LK9 | `spirv.format.json` + engine SPIR-V support (skeleton) | Module header + section framework; JSON-declared; full impl in `17-shader-gpu-plan`. |
| LK10| End-to-end "hello world" integration on all 6 (OS × arch) | The hermetic-acceptance gate: build c-subset corpus on a CI runner with NO system linker installed; produced binary runs + prints expected output. |
| LK11| **Cross-CU linking** (v1.x)                       | Lifts the v1 single-CU-per-image assumption. Engine's symbol-table merge (§2.2 flow item 1) becomes real; cross-CU symbol resolution + relocation application; CU-boundary diagnostics under `K_CrossCu*`. **Couples with** [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) **CU6**. v1.x — triggers per §2.12. |

Substrate tier (5-agent review) for LK4 (format-blind engine + `ObjectFormatSchema` substrate) + LK7 (codesign-hook contract). Feature tier for per-format JSON onboarding (LK1/LK2/LK3/LK8/LK9 — each PR ships one `*.format.json` + any engine support its schema declarations need).

---

## 3.1 Deferred-items registry (LK)

Mirrors plan 12 §3.1 / plan 13 §3.1. Every deferred item has an explicit owner + trigger. Items struck through when their closing commit lands.

| # | Deferred item | Why deferred (not a silent gap) | Owner / closure | Trigger |
|---|---|---|---|---|
| D-LK4-1 | **Per-format byte-emission walkers** — `LinkedImage::bytes` is currently empty; LK4 substrate ships the dispatch shell only. ELF / PE / Mach-O writers must walk `ObjectFormatSchema::sections` (which doesn't exist yet — that schema row is also LK1+) and emit per-format section headers, symbol-table layout, relocation tables, dynamic entries. | **LK1 (`elf.format.json` + ELF writer)** opens the walker substrate; LK2 (PE) + LK3 (Mach-O) plug in additional shape-keyed arms. The shape-keyed dispatch pattern follows plan 13 §2.4 (encoder shapes) and ML5 cycle 2a's target-blind pivot. | First per-format integration test. |
| D-LK4-2 | **Section / symbol-table JSON schema rows.** `ObjectFormatSchema` ships `relocations[]` (the format-side half of plan 13 §2.6's reloc unifier) but does NOT yet declare `sections[]` (mapping `SectionKind → platform-native section name + flags`), `dynamic[]` (ELF dynamic-section entries, PE .idata/.edata templates, Mach-O LC_* load commands), or `symbolTable.layout` (binding/visibility mapping per-format). Substrate ships the closed-enum vocabulary the JSON will declare (`SectionKind`, `SymbolBinding`, `SymbolVisibility`); the loader expansion + accessors land alongside LK1's ELF schema. | **LK1** alongside the first shipped `.format.json` file. The schema additions land in the same PR as their first consumer (one writer + one JSON file). | First shipped `*.format.json` file. |
| D-LK4-3 | **Symbol-namespace promotion: `SymbolId` → `(CuId, SymbolId)`.** v1 substrate assumes single-CU: `link()` indexes `module.functions[].symbol` into a single `unordered_set<SymbolId>` (`linker.cpp:35`) and resolves all relocs against that set. `SymbolId` is per-arena, so two CUs can mint the same integer for different symbols. Triggers in TWO independent ways (architect convergence): (a) cross-CU linking — multiple CUs in one image; (b) **FFI import resolution** — an `extern` symbol from a precompiled library arrives as a `Relocation::target` that no `AssembledFunction` declares, so LK4 today fires `K_SymbolUndefined` on every FFI ref. | **EITHER LK6** (dynamic linking + imports — FFI table arrives) **OR LK11** (cross-CU linking), whichever lands first. The compound-key promotion + the import-table consultation are one PR's worth of work and should NOT split. | First FFI import to a precompiled lib, OR first multi-CU image artifact. |
| D-LK4-4 | **`LinkedImage` strong-id (LinkedImageId).** v1 substrate uses `LinkedImage` as a value type; no monotonic id. Future incremental linking (open question #3) + LSP "show linked image" introspection would want O(1) identity. | **LK11** OR a dedicated cycle when incremental linking lands (open question #3 — post-v1). | First incremental-link cache lookup. |
| D-LK4-5 | **Wire-declared relocation addend bias** — same anchor as plan 13 D-AS4-4. Assembler currently hardcodes `Relocation::addend = 0` at the walker emit sites; linker substrate respects whatever the assembler stamped. Future ISAs whose linker formula expects a non-zero addend will need both sides updated together. | **Plan 13 D-AS4-4** is the canonical anchor (assembler-side change first); LK substrate gains formula-application logic in **LK6** (dynamic linking + imports — the first reloc actually applied to bytes). | First non-zero-addend formula. |
| ~~D-LK4-7~~ ✅ **closed 2026-05-29 same session as LK4 substrate (+ 7-agent re-review fold)** | Cross-side shared substrate for `relocations[]` loader + validator + `DiagnosticCollector`. | Closed by `src/core/substrate/{diagnostic_collector,relocation_table}.hpp` — template `loadRelocationsTable<RowT>` (+ no-extension overload) + `validateRelocationsTable<RowT>` consumed by both `target_schema_json.cpp` and `object_format_schema_json.cpp`; `DiagnosticCollector` consumed by 3 JSON loaders (target / grammar / object-format). Plan 13 §2.6 reloc-taxonomy unifier is now identical-by-construction on both sides. **7-agent re-review fold-in (post-audit)**: 4-agent convergence on `extendRow` doc-vs-code contract → comment fixed + target-side returns false after emit on bad formula (no partial row escapes indices); 2-agent convergence on public `diagnostics` field hazard → encapsulated as private + `emitRaw(ConfigDiagnostic&&)` + `release() &&` rvalue-extract (3 callsites migrated, day-1 back-door closed); silent-failure HIGH on loader-side dup-kind silent first-wins → defense-in-depth loader detection added (validator becomes belt-and-suspenders); 3-agent convergence on empty-extension boilerplate → no-extension overload; type-design LOW on `concept relocation_row<T>` → added (better template errors + self-documents the {name, kind} contract); comment-analyzer on stale provenance + "enforced upstream" wording → fixed; test-analyzer HIGH on `extendRow == false` skip branch dead → new `tests/core/substrate/test_relocation_table.cpp` with 9 substrate tests covering the skip contract + loader-side dup-kind + zero-kind cross-pass split + DiagnosticCollector encapsulation. −170 LOC net for the substrate hoist itself; the review fold adds back ~70 LOC of encapsulation + tests. 112/112 ctest. | — (closed) |
| D-LK4-6 | **`ObjectFormatRelocationInfo` enrichment** — currently `{name, kind, nativeId}` only. Per-format reloc rows likely need `width: bits`, `offsetWithinSection: bits`, `applyAlignment: bits`, `requiresGot/Plt/Tls: bool`. Format-side counterpart to plan 13's eventual `TargetRelocationInfo` decomposition. | **LK6** (dynamic linking + imports — the cycle that actually applies a formula). The PR enriches both `TargetRelocationInfo` and `ObjectFormatRelocationInfo` in parallel + the JSON loader on both sides + validate() pairing. | First reloc actually applied to output bytes. |
| D-LK1-1 | **Real symbol-name thread through HIR → MIR → LIR → AssembledFunction.** Today `AssembledFunction.symbol` is just a `SymbolId`; the ELF writer synthesizes section-local names like `"sym_42"`. Hermetic linking against another `.o` we produce works (we resolve by SymbolId index), but consumers like GNU `objdump`/`readelf` / a system linker / a debugger see anonymized names and cross-toolchain interop breaks. The name is already known at HIR-decl time; the thread needs to flow through the IR layers + sit on `AssembledFunction` as `std::string symbolName`. | **LK7** (codesign + publish) is the natural trigger — that's the first cycle whose acceptance criterion requires the produced binary to interoperate with platform tooling (codesign reads symbol names; signtool's hash machinery consults debug info). | First cross-toolchain interop test (system linker reading our `.o`, debugger reading our DWARF). |
| D-LK3-1 | **Mach-O segment+section two-level naming.** `ObjectFormatSectionInfo::name` is a single string (e.g. `.text` for ELF). Mach-O sections live inside segments — `__TEXT,__text` is one section in the `__TEXT` segment. The single-string field doesn't carry the segment relationship. | **LK3** (`macho.format.json` + Mach-O writer). The JSON row gains a `segment` field; the substrate `ObjectFormatSectionInfo` grows `std::string segment` (empty when format doesn't use segments). | First Mach-O writer cycle. |

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | `--as-needed` / unused-import elision? | **No** — link every declared FFI import. Elision reserved post-v1. |
| 2 | Mach-O universal binaries (multi-arch one file)? | **Post-v1.** v1 ships one binary per arch. |
| 3 | Incremental linking? | **Post-v1.** |
| 4 | LTO / link-time IR-level optimization? | **Post-v1.** |
| 5 | Section ordering policy? | Page-aligned segments per format conventions (ELF: text/rodata/data/bss; PE: text/rdata/data/idata/edata/reloc; Mach-O: __TEXT/__DATA_CONST/__DATA/__LINKEDIT). |
| 6 | Build-id input space? | BLAKE3 of section contents. Deterministic. |
| 7 | DT_RPATH / DT_RUNPATH for ELF? | Honored if specified in project config; default unset. Mach-O equivalent: `@rpath` in LC_RPATH (same logic). |
| 8 | Linker scripts? | **No.** Layout templates live in the per-format JSON schema (`*.format.json`); user-provided linker scripts reserved post-v1. |

---

## 5. Acceptance criteria

- [ ] ELF writer output passes `readelf -a` / `objdump -d` clean on Linux x86_64 + ARM64.
- [ ] PE writer output passes `dumpbin /HEADERS /ALL` clean on Windows x86_64 + ARM64.
- [ ] Mach-O writer output passes `otool -l -L -h` clean on macOS x86_64 + ARM64.
- [ ] Mach-O codesign-verify-style structural check passes after `16-codesign-publish-plan` fills the LC_CODE_SIGNATURE.
- [ ] c-subset corpus binaries run + print expected output on all 6 (OS × arch) v1 targets.
- [ ] **Hermetic gate**: CI runner with NO `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` installed builds the c-subset corpus end-to-end (LK10).
- [ ] BLAKE3 build-id is deterministic: rebuilding the same source twice produces byte-identical output.

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Mach-O chained-fixups format complexity (intricate bind opcode trie) | High | High | **Classified as bucket-2 universal-algorithm** per §2.0 (the bind / lazy-bind / rebase opcode trie is one stream emitter walking a JSON-declared opcode vocabulary, NOT a per-format C++ tree). LK3 lands the JSON declaration + the engine's trie-emit pass; integration test on Apple Silicon CI runner pins the byte output against an oracle (real `dyld_info` dump) before LK3 ships. Same shape for chained-fixups page encoding (LC_DYLD_CHAINED_FIXUPS) — chain templates declared in JSON, the page-walker is universal. |
| PE IAT layout intricacies (32-bit vs 64-bit thunks, hint/name tables) | Medium | High | Per-import golden bytes pinned against dumpbin oracle output. |
| ELF dynamic linking edge cases (PLT lazy-binding races, GNU_HASH layout) | Medium | High | Lazy-binding deferred to post-v1; v1 uses immediate binding (BIND_NOW). |
| TLS model bugs surface only at multi-threaded runtime | Medium | Critical | LK5 acceptance includes a multi-threaded smoke test per platform. |
| WASM / SPIR-V skeleton drifts from full plans (18 / 17) | Low | Medium | LK8 / LK9 are skeleton-only; full impl is plan-owned; cross-link discipline enforced. |
| ~~Linker engine mismatch with assembler relocation kinds~~ | Low | Low | **Resolved rev 3 via plan 13 §2.6 unifier (2026-05-29):** relocation taxonomy lives as a `relocations[]` facet on `*.target.json` (NOT a separate `relocation.hpp` enum). Assembler emits opaque `uint32_t kind = tag`; linker resolves via `schema.relocationInfo(tag)`. Both consume the same `TargetSchema` instance — there's no two-side enum to drift. The object-format-specific reloc *names* (e.g. `R_X86_64_PC32` vs `IMAGE_REL_AMD64_REL32`) live in `*.format.json` as `format-name → schema-tag` mappings, so a single bucket-1 formula serves every format that supports it. |

---

## 7. Sequencing

```
13-assembler (AS4) ─► LK4 (engine)
                         │
              ┌──────────┼──────────┐
              ▼          ▼          ▼
             LK1        LK2        LK3 ─► LK7 ─► 16-codesign-publish
            (ELF)      (PE)      (Mach-O)
              │          │          │
              └──────────┼──────────┘
                         ▼
                       LK5 ─► LK6 ─► LK10
                                       │
                          ┌────────────┼────────────┐
                          ▼            ▼            ▼
                         LK8         LK9         15-debug-info
                        (WASM)    (SPIR-V)
```

LK1/LK2/LK3 are parallel. LK4 ties them together. LK5 + LK6 finish the substrate. LK10 is the hermetic acceptance gate. LK8/LK9 are skeletons unblocking 17/18. **LK11** (v1.x) ships post-LK10 — couples with [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) CU6 to lift the single-CU-per-image assumption (see §2.12); trigger-gated, not on the v1 critical path.
