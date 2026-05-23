# In-tree Linker — Sub-Plan (14)

> Owns the **per-format object writers** (ELF, PE/COFF, Mach-O, WASM, SPIR-V) and the **linker engine** (symbol resolution, relocation application, section layout, per-platform metadata). Consumes (bytes, relocations, symbols) from the [in-tree assembler](./13-assembler-plan%20-%20tbd.md); produces final-form binaries.
>
> Per the user's mandate ("we will be the process from source to targets"), **no `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` invocation**. This is the single largest backend chunk of v1.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-critical. Largest single chunk of backend work. |
| Predecessors  | ⏳ [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) (bytes + relocations). ⏳ [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) (extern symbol declarations from precompiled libs). |
| Successors    | ⏳ [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) (LC_CODE_SIGNATURE / PE security directory placeholders filled post-link). ⏳ [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) (debug sections placed alongside code). |
| Scope         | **Bounded.** v1: LK1–LK10. v1 acceptance: link c-subset corpus → ELF / PE / Mach-O on every {OS × arch}. WASM (LK8) + SPIR-V (LK9) post-v1 skeletons. **v1.x: LK11** — cross-CU linking (couples with [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) CU6). |

---

## 1. Motivation

Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no `ld` / `link.exe` / `ld64` invocation. Three payoffs:

1. **Apple-host-free local dev.** Mach-O writer in-tree eliminates the `osxcross`/`xcrun` dependency.
2. **WASM + SPIR-V unification.** Same plumbing covers native, browser, GPU.
3. **Reproducibility.** Deterministic build-id; no embedded timestamps/paths/version-strings.

---

## 2. Design

### 2.1 Files

```
src/link/
├── linker.hpp / .cpp            # Engine: resolve + relocate + lay out
├── symbol_table.hpp / .cpp      # Defined / undefined / weak / local / hidden
├── section.hpp                  # SectionFlags, SectionKind, layout primitives
├── relocation_apply.hpp / .cpp  # Per (arch × format) relocation mutators
├── tls.hpp / .cpp               # Per-platform TLS lowering
├── objfmt/
│   ├── elf/
│   │   ├── elf_writer.hpp / .cpp   # LK1
│   │   └── elf_dynamic.hpp / .cpp  # .dynamic / GNU_HASH / .interp
│   ├── pe/
│   │   ├── pe_writer.hpp / .cpp    # LK2
│   │   └── pe_imports.hpp / .cpp   # .idata / IAT / .pdata / .xdata
│   ├── macho/
│   │   ├── macho_writer.hpp / .cpp # LK3
│   │   ├── macho_chained.hpp/.cpp  # Chained fixups (Apple Silicon)
│   │   └── macho_lc_dyld.hpp/.cpp  # LC_DYLD_INFO bind/lazy/rebase opcodes
│   ├── wasm/
│   │   └── wasm_writer.hpp / .cpp  # LK8 (skeleton)
│   └── spirv/
│       └── spirv_writer.hpp / .cpp # LK9 (skeleton)
└── build_id.hpp / .cpp          # BLAKE3-based deterministic build-id
```

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

`K_*` (K for "linK") — `K_SymbolUndefined`, `K_SymbolRedefined`, `K_RelocationOverflow`, `K_RelocationKindMismatch`, `K_SectionOverlap`, `K_ImportUnresolved`, `K_InvalidLoadCommand` (Mach-O), `K_InvalidPeHeader`, `K_InvalidElfHeader`, `K_TlsModelUnsupported`. Per-format writers also emit `K_*` codes scoped to their format.

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
| LK1 | ELF writer                                       | Linux x86_64 + ARM64; executables + shared libs; PLT/GOT; GNU_HASH; PT_GNU_RELRO. |
| LK2 | PE/COFF writer                                   | Windows x86_64 + ARM64; exe + DLL; subsystem flag per artifactProfile; base relocations; .idata IAT. |
| LK3 | Mach-O writer                                    | macOS x86_64 + ARM64; executables + dylibs; chained-fixups path; LC_DYLD_INFO legacy path; LC_CODE_SIGNATURE placeholder. |
| LK4 | Linker engine                                    | Symbol resolution + relocation application + section layout. Format-agnostic; calls into LK1/LK2/LK3 writers. |
| LK5 | TLS lowering                                     | Per §2.8 table; initial-exec model on every platform. |
| LK6 | Dynamic linking + imports                        | PE IAT, ELF GOT/PLT, Mach-O bind opcodes. FFI imports from `11-ffi-plan` arrive here. |
| LK7 | Codesign hook                                    | Mach-O LC_CODE_SIGNATURE placeholder + PE attribute-cert reservation, both filled by `16-codesign-publish-plan`. |
| LK8 | WASM writer (skeleton)                           | Module header + section framework; full impl in `18-wasm-plan`. |
| LK9 | SPIR-V writer (skeleton)                         | Module header + section framework; full impl in `17-shader-gpu-plan`. |
| LK10| End-to-end "hello world" integration on all 6 (OS × arch) | The hermetic-acceptance gate: build c-subset corpus on a CI runner with NO system linker installed; produced binary runs + prints expected output. |
| LK11| **Cross-CU linking** (v1.x)                       | Lifts the v1 single-CU-per-image assumption. Engine's symbol-table merge (§2.2 flow item 1) becomes real; cross-CU symbol resolution + relocation application; CU-boundary diagnostics under `K_CrossCu*`. **Couples with** [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) **CU6**. v1.x — triggers per §2.12. |

Substrate tier (5-agent review) for LK4 (engine) + LK7 (codesign-hook contract). Feature tier for writers.

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
| 8 | Linker scripts? | **No.** We hardcode per-format layout templates; user-provided linker scripts reserved post-v1. |

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
| Mach-O chained-fixups format complexity (intricate bind opcode trie) | High | High | LK3 lands behind a thin shim with both legacy LC_DYLD_INFO and chained paths; integration test on Apple Silicon CI runner before LK3 ships. |
| PE IAT layout intricacies (32-bit vs 64-bit thunks, hint/name tables) | Medium | High | Per-import golden bytes pinned against dumpbin oracle output. |
| ELF dynamic linking edge cases (PLT lazy-binding races, GNU_HASH layout) | Medium | High | Lazy-binding deferred to post-v1; v1 uses immediate binding (BIND_NOW). |
| TLS model bugs surface only at multi-threaded runtime | Medium | Critical | LK5 acceptance includes a multi-threaded smoke test per platform. |
| WASM / SPIR-V skeleton drifts from full plans (18 / 17) | Low | Medium | LK8 / LK9 are skeleton-only; full impl is plan-owned; cross-link discipline enforced. |
| Linker engine mismatch with assembler relocation kinds | High | High | AS4 (assembler) and LK4 (engine) co-reviewed; relocation taxonomy lives in shared `relocation.hpp`. |

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
