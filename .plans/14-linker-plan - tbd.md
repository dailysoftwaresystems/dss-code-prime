# In-tree Linker — Sub-Plan (14)

> Owns the **object-format engine** (one language-blind C++ engine that reads JSON-configured format schemas) and the **linker engine** (symbol resolution, relocation application, section layout, per-platform metadata). Consumes (bytes, relocations, symbols) from the [in-tree assembler](./13-assembler-plan%20-%20tbd.md); produces final-form binaries.
>
> Per the user's mandate ("we will be the process from source to targets"), **no `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` invocation**. This is the single largest backend chunk of v1.
>
> **Universal-compiler thesis extension (rev 3 — 2026-05-29).** Object formats are **100% config-driven** — mirroring [`GrammarSchema`](./05-parser-plan%20-%20ok.md) on the frontend and [`TargetSchema`](./12-mir-lir-plan%20-%20ok.md) on the backend ISA tier. Each object format (ELF, PE/COFF, Mach-O, WASM, SPIR-V) is a JSON file in `src/dss-config/object-formats/<name>.format.json` declaring section taxonomy, symbol-table layout, relocation kinds, per-format metadata sections, and (where applicable) dynamic-linking constructs. ONE language-blind C++ engine reads the JSON to emit bytes — there is NO per-format C++ writer. Adding a new object format = drop a new JSON file, no engine edits. Honors thesis decision #4 ("config-driven everything") end-to-end.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-critical. Largest single chunk of backend work. **Plan rev 3 (2026-05-29): object formats are JSON-configured** (mirrors `GrammarSchema` + `TargetSchema` pattern) — one engine, no per-format C++. |
| Predecessors  | ⏳ [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) (bytes + relocations). ⏳ [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) (extern symbol declarations from precompiled libs). |
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
| `*.target.json` `relocations[]` | The **formula+tag**: opaque `uint32_t tag → { isPCRelative, width, addendWidth, ... }` | Assembler (plan 13) emits the tag; linker applies the formula via `relocation_apply.cpp` |
| `*.format.json` `relocations[]` (this plan) | The **format-name → tag** mapping: e.g. `"R_X86_64_PC32" → tag 1`, `"IMAGE_REL_AMD64_REL32" → tag 1`, `"X86_64_RELOC_BRANCH" → tag 1` (all three are PC-relative 32-bit signed — one formula, three names) | Linker (LK6 reloc-apply) uses the format name when writing the object file's reloc table |

Same opaque `uint32_t` tag joins both sides. **No per-(arch×format) C++ enum anywhere.** Cross-referenced from plan 13 §2.6 — AS4 (target-schema reloc rows) and LK6 (format-schema name→tag mapping) land in the same review window so the integer assignments don't drift. The cross-cycle "linker engine mismatch with assembler relocation kinds" risk (rev 1/2's §6 High/High) is closed structurally by both sides reading from the same opaque-tag namespace.

Loader pattern mirrors `TargetSchema::loadShipped`/`GrammarSchema::loadShipped`:

```cpp
auto fmt = ObjectFormatSchema::loadShipped("elf");
LinkResult bin = link(input, *fmt, /*architecture=*/"x86_64", reporter);
```

`O_*` diagnostic family at 0xC00x for object-format validation (mirrors `L_*` 0xB00x for LIR, `I_*` 0xA00x for MIR, `H_*` 0xF00x for HIR).

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

`K_*` (K for "linK") — `K_SymbolUndefined`, `K_SymbolRedefined`, `K_RelocationOverflow`, `K_RelocationKindMismatch`, `K_SectionOverlap`, `K_ImportUnresolved`, `K_InvalidLoadCommand` (Mach-O), `K_InvalidPeHeader`, `K_InvalidElfHeader`, `K_TlsModelUnsupported`. The engine emits these directly; the schema's `ObjectFormatSchema::validate()` separately emits `O_*` diagnostics at 0xC00x for malformed/incomplete format JSON.

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
| LK1 | `elf.format.json` + engine ELF support           | Linux x86_64 + ARM64; executables + shared libs; PLT/GOT; GNU_HASH; PT_GNU_RELRO. JSON-declared section/symbol/reloc/dynamic schema; engine reads it (no per-format C++). |
| LK2 | `pe.format.json` + engine PE support             | Windows x86_64 + ARM64; exe + DLL; subsystem flag per artifactProfile; base relocations; .idata IAT. JSON-declared section table + import/export schema; engine reads it. |
| LK3 | `macho.format.json` + engine Mach-O support      | macOS x86_64 + ARM64; executables + dylibs; chained-fixups path; LC_DYLD_INFO legacy path; LC_CODE_SIGNATURE placeholder. JSON-declared load-command + chained-fixups schema; engine reads it. |
| LK4 | `object_writer` engine                           | ONE format-blind engine reading `ObjectFormatSchema`: symbol resolution + relocation application + section layout + per-format metadata emission. NO format-specific dispatch table or per-format C++ writer — all format knowledge in JSON. |
| LK5 | TLS lowering                                     | Per §2.8 table; initial-exec model on every platform. |
| LK6 | Dynamic linking + imports                        | PE IAT, ELF GOT/PLT, Mach-O bind opcodes. FFI imports from `11-ffi-plan` arrive here. |
| LK7 | Codesign hook                                    | Mach-O LC_CODE_SIGNATURE placeholder + PE attribute-cert reservation, both filled by `16-codesign-publish-plan`. |
| LK8 | `wasm.format.json` + engine WASM support (skeleton) | Module header + section framework; JSON-declared; full impl in `18-wasm-plan`. |
| LK9 | `spirv.format.json` + engine SPIR-V support (skeleton) | Module header + section framework; JSON-declared; full impl in `17-shader-gpu-plan`. |
| LK10| End-to-end "hello world" integration on all 6 (OS × arch) | The hermetic-acceptance gate: build c-subset corpus on a CI runner with NO system linker installed; produced binary runs + prints expected output. |
| LK11| **Cross-CU linking** (v1.x)                       | Lifts the v1 single-CU-per-image assumption. Engine's symbol-table merge (§2.2 flow item 1) becomes real; cross-CU symbol resolution + relocation application; CU-boundary diagnostics under `K_CrossCu*`. **Couples with** [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) **CU6**. v1.x — triggers per §2.12. |

Substrate tier (5-agent review) for LK4 (format-blind engine + `ObjectFormatSchema` substrate) + LK7 (codesign-hook contract). Feature tier for per-format JSON onboarding (LK1/LK2/LK3/LK8/LK9 — each PR ships one `*.format.json` + any engine support its schema declarations need).

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
