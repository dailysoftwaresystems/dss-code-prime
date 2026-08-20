#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/aggregate_layout.hpp"    // BitFieldStrategy (D-CSUBSET-BITFIELD-ABI-EXACT — the per-ABI bit-field rule, FORMAT-determined)
#include "core/types/data_model.hpp"          // DataModel (FC3 c1 — the per-OS width triple)
#include "core/types/grammar_schema.hpp"      // ConfigDiagnostic + LoadResult
#include "core/types/header_name_matching.hpp"  // HeaderNameMatching (D-PP-HEADER-CASE-INSENSITIVE-PE — the per-OS `#include` case rule)
#include "core/types/object_format_kind.hpp"  // ObjectFormatKind + kObjectFormatKindTable
#include "core/types/preprocess_config.hpp"   // PredefinedMacroDef (TF-C97 — the format's data-model predefines)
#include "core/types/section_kind.hpp"        // SectionKind + kSectionKindTable
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"        // SymbolBinding / SymbolVisibility (lifted to core/types for MIR-tier producers)
#include "core/types/target_schema.hpp"       // EnumNameTable<E,N>
#include "link/object_format_backend.hpp"     // D-LINK-…-KIND-IDENTITY-BRANCHES: the format-identity SEAM

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// `ObjectFormatSchema` (plan 14 §2.0 + LK4 substrate). JSON-configured
// object-format descriptor. Mirrors `TargetSchema` for the backend:
// an object format (ELF / PE/COFF / Mach-O / WASM / SPIR-V) is a
// JSON file in `src/dss-config/object-formats/<name>.format.json`,
// NOT C++ code. Adding a new format = drop a new `.format.json`;
// nothing in the linker substrate or engine changes.
//
// The substrate ships the closed-enum vocabulary (section kinds /
// symbol bindings/visibilities / format kinds) + the `relocations[]`
// taxonomy (the format-side half of plan 13 §2.6's reloc unifier:
// format-name → opaque-tag mapping, where the tag is the SAME tag
// the assembler stamps onto `Relocation::kind`).
//
// Per-format JSON rows (sections, dynamic-section entries, load
// commands, etc.) land in their respective LK* cycles — substrate
// holds the contracts.

namespace dss {

// ── Closed-enum vocabulary ────────────────────────────────────────

// One per shipped output format. Substrate-tier — adding a new
// format = JSON file + new enum entry + new walker arm (when the
// engine grows format-specific emission, anchored at LK1+).
//
// `ObjectFormatKind` + helpers extracted to
// `core/types/object_format_kind.hpp` so non-linker layers (FFI
// mangling, semantic-config loader) can speak the closed enum
// without pulling in this header's full 800-LOC substrate. The
// enum + name table + `objectFormatKindName` /
// `objectFormatKindFromName` accessors remain visible here via the
// include above — every existing consumer continues to work
// without touching its `#include`s. Extraction precedent:
// `SectionKind` at the D-LK4-RODATA-SUBSTRATE slice.

// `SectionKind` + helpers extracted to `core/types/section_kind.hpp`
// at the D-LK4-RODATA-SUBSTRATE slice so the upstream assembler can
// tag `AssembledData` outputs with the same vocabulary the linker
// walkers consume. The enum + name table + accessors remain visible
// here via the include above — every existing consumer continues to
// work without touching its `#include`s.

// SymbolBinding + SymbolVisibility lifted to `core/types/symbol_attrs.hpp`
// so MIR-tier producers (the optimizer's DCE pass — D-OPT1-SYMBOL-
// BINDING-VISIBILITY-THREAD) can consume the vocabulary without a
// layer inversion through the link header. The canonical definitions
// + name-tables live there; the file-top `#include` re-exports them
// into the `dss` namespace for every existing link-side consumer. No
// source-compatibility break.
//
// (Include is at file-top — see line 9 area — because symbol_attrs.hpp
// declares its own `namespace dss { ... }` block, so it cannot be
// nested inside the surrounding `namespace dss` here without creating
// a `dss::dss::*` mis-qualification.)

// ── Per-relocation row (format-side half of plan 13 §2.6) ──
//
// The assembler emits a `Relocation{offset, target, kind, addend}`
// where `kind` is the OPAQUE tag declared by the TARGET schema's
// `relocations[]` (plan 13). The linker needs to know which
// PLATFORM-NATIVE relocation NAME that kind maps to (e.g. ELF
// `R_X86_64_PC32`, PE `IMAGE_REL_AMD64_REL32`, Mach-O
// `X86_64_RELOC_BRANCH` are all the same kind in plan 13's reloc
// taxonomy).
//
// `name` is the platform-native name the format engine writes into
// the object file's reloc table. `kind` is the opaque tag the
// assembler stamped — same RelocationKind type as plan 13's
// `TargetRelocationInfo::kind`. The format engine looks up by kind
// (O(1) via `relocationKindIndex`) and writes `name` into the
// output bytes.
//
// `nativeId` is the FORMAT-SPECIFIC numeric tag the format engine
// writes into its relocation record. Each format interprets the
// field differently:
//   * ELF: `r_info` low 32 bits = `r_type`, e.g. R_X86_64_PC32 = 2.
//   * PE:  IMAGE_RELOCATION.Type field, e.g. IMAGE_REL_AMD64_REL32
//     = 4.
//   * Mach-O: STATIC PORTION of `relocation_info.r_info` packed
//     as `(r_type<<28) | (r_length<<25) | (r_pcrel<<24)`. The
//     walker ORs in r_extern (bit 27) + r_symbolnum (bits 0..23)
//     at emit time. Mach-O `validate()` rejects nativeId values
//     that pre-set those walker-owned bits (silent-corruption
//     guard).
//   * WASM: u32 LEB128 reloc type (fits u32 unchanged).
//   * SPIR-V: no traditional relocation table (decoration-based);
//     the field is unused on SPIR-V format schemas.
// Distinct from the universal `kind` (cross-side join key) — `kind`
// is opaque to all formats; `nativeId` is the actual byte value
// embedded in the format's reloc record. Per-row invariant:
// `nativeId != 0` on every relocation row (slot-0 reserved as the
// invalid sentinel — emitting it would write a format-specific
// `R_*_NONE`-shaped record that consumers treat as a no-op,
// silently dropping the patch).
struct DSS_EXPORT ObjectFormatRelocationInfo {
    std::string    name;            // e.g. "R_X86_64_PC32"
    RelocationKind kind{};          // matches assembler-side tag
    std::uint32_t  nativeId = 0;    // ELF r_info type / PE Type /
                                    // Mach-O r_type / WASM reloc type
                                    // — format-specific wire value
    // D-LK-OBJECT-EXTERN-CALL-RELOCATABLE: the "may go through a PLT" variant
    // of this reloc type (e.g. R_X86_64_PLT32=4 for the R_X86_64_PC32=2 rel32
    // call). Emitted INSTEAD of `nativeId` when this reloc targets an UNDEFINED
    // extern in a relocatable object, so a foreign PIE linker resolves the
    // extern through a linker-built PLT — a bare PC32 against an undefined
    // symbol errors under -pie. 0 (default) = no PLT variant (the reloc never
    // names an extern call — e.g. abs64/abs32 data relocs), and the emitter
    // uses `nativeId` unchanged.
    std::uint32_t  pltNativeId = 0;
    // ── DECLARED CALL/BRANCH ROLE (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL) ──
    //
    // True iff this format's NATIVE relocation can only ever target executable
    // code -- i.e. reaching an undefined extern through it PROVES the extern is
    // a FUNCTION, not a data object. An object reader has no other way to
    // classify an extern it meets only as a name.
    //
    // ★ WHY THIS IS DECLARED HERE AND NOT DERIVED FROM THE TARGET'S FORMULA.
    //   The readers used to infer the role from `TargetRelocationInfo::formulaKind`,
    //   testing for the aarch64 CALL26 formula. That is a proxy for the wrong
    //   thing: a formula describes ARITHMETIC, not ROLE. It happens to be
    //   branch-specific on AArch64 only because the branch instruction has its
    //   own encoding. On x86_64 it fails BY CONSTRUCTION -- `S + A - P` is the
    //   identical arithmetic for a call and for a PC-relative data reference,
    //   so `linear` is the HONEST formula there and carries no role at all.
    //   ✔MEASURED 2026-08-20: every macho64-x86_64 archive member that calls
    //   any library function was unreadable for exactly this reason, while the
    //   arm64 sibling read the same source cleanly.
    //
    // ★ WHY THE FORMAT ROW AND NOT THE TARGET ROW. A target document is
    //   PER-CPU and shared across formats. `x86_64.target.json`'s `rel32` is
    //   also ELF's R_X86_64_PC32, which genuinely serves data references too --
    //   declaring a call role there would be a lie about ELF. The FORMAT is
    //   where the ambiguity resolves, because the format is what picks the
    //   native wire relocation: mach-o maps `rel32` to X86_64_RELOC_BRANCH,
    //   which is branch-only, while ELF maps it to a wire type that is not.
    //   ELF x86_64's own call signal is likewise format-side (`pltNativeId`).
    //
    // ⚠ FALSE on a row whose wire type is SHARED with a data use -- ELF
    //   R_X86_64_PC32 is the shipped case (kind 5 `R_X86_64_PC32_UNBIASED`
    //   shares its nativeId). Setting it there would re-commit the same
    //   conflation in a new field.
    //
    // Declared on EVERY document that carries the relocation, including ones
    // no reader consults, because it states what the relocation IS rather than
    // what an artifact kind needs. A fact true in one document and absent from
    // its sibling is how the sibling drifts.
    bool           isCall = false;
    // ── EMISSION ALIAS (D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS) ──
    //
    // True iff this row shares its `nativeId` with another row and exists
    // only so the EMITTER can reach that wire type through a different DSS
    // `kind`. x86_64 is the shipped case: `R_X86_64_PC32` = 2 is BOTH the
    // call-site `rel32` (implicit addend bias -4, because a call's
    // displacement is relative to the instruction END) and the DWARF FDE
    // `initial_location` pointer (bias 0, because it is a DATA word). One
    // ELF wire type, two DSS patch-site semantics.
    //
    // ★ WHY A FLAG AND NOT SIMPLY TWO ROWS. `nativeId → kind` is a REVERSE
    //   map every object READER builds, and it must be a FUNCTION. Two
    //   ordinary rows sharing a nativeId make it ambiguous, and
    //   `elf_object_reader.cpp` fails loud on exactly that — correctly:
    //   ✔MEASURED 2026-08-13, declaring the FDE row as an ordinary second
    //   row made the reader reject EVERY x86_64 ELF object, DSS-produced
    //   AND gcc-produced (3 suites red, one diagnostic). The wire format
    //   carries no bias, so the distinction is real for EMISSION and
    //   meaningless for DECODING; this states that, and `validate()` turns
    //   the reader's runtime discovery into a LOAD-TIME invariant (at most
    //   one non-alias row per nativeId).
    bool           emitOnly = false;
};

// ── THE DECODE SIDE OF `relocations[]`, BUILT ONCE ────────────────
//
// The two lookups every object READER needs, derived from the rows above by
// `ObjectFormatSchema::relocationDecodeTable()` — the one place that knows
// which rows a DECODER may see.
//
// ★ WHY THIS IS A SCHEMA METHOD AND NOT A LOOP INSIDE EACH READER. It WAS a
// loop inside each reader, and two of the three got it wrong in the same way:
// the ELF reader skips `emitOnly` rows, and the Mach-O and COFF readers did
// not. That is the archetype this codebase already named for closed-key
// vocabularies — a rule that must be REMEMBERED per site is a rule that holds
// only where someone remembered it — and the site that forgets is never the
// one being read at the time. The row's meaning is the SCHEMA's to know: it
// declares `emitOnly`, `validate()` enforces what the flag promises, so the
// schema is what answers "which rows decode".
//
// ⓘ THE MACH-O / COFF GAP WAS LATENT AND LOUD, NOT A MISCOMPILE — stated at
//   that precision because overstating it would be its own defect. No shipped
//   Mach-O or PE document declares `emitOnly` (✔MEASURED: 2 of the 24 shipped
//   format documents do, both `elf64-x86_64-linux*`), so nothing decoded
//   wrongly. Had one declared it, `validate()`'s unique-`kind` rule guarantees
//   the alias carries a DIFFERENT kind from the row owning its wire id, so
//   those readers would have hit the ambiguity refusal below and rejected
//   EVERY object of that format. A correctness gap that fails loud, and a
//   capability — one wire type, two emission semantics — silently unavailable
//   to two of the three formats.
struct DSS_EXPORT RelocationDecodeTable {
    // Wire type → the DSS kind it decodes to. A FUNCTION by construction:
    // emission aliases are excluded, and a residual collision is refused.
    std::unordered_map<std::uint32_t, RelocationKind> nativeToKind;
    // The wire types whose presence PROVES the extern they reach is a
    // FUNCTION: every row the format declares `"isCall": true` on, plus every
    // declared `pltNativeId` (a call-through-stub variant can only be a call).
    // DECLARED by the schema, never inferred from the target's arithmetic
    // formula — D-LK-MACHO-ISDATA-NO-CALL-SIGNAL. Legitimately EMPTY for a
    // format that declares neither (every shipped PE document).
    std::unordered_set<std::uint32_t> callSignalNativeIds;
};

// ── Per-section row (plan 14 D-LK4-2) ───────────────────────────
//
// Each format's `sections[]` row maps the UNIVERSAL `SectionKind` (the
// engine speaks this) to the FORMAT-NATIVE section name + structural
// fields (e.g. ELF sh_type / sh_flags / sh_addralign). The substrate
// validates the universal `kind` join key + `name` non-empty +
// `kind` unique cross-row; the format-specific numeric fields are
// stored verbatim and interpreted by the format walker.
//
// **Two-level section naming**: `name` is the section-only id
// (`.text` for ELF/PE, `__text` for Mach-O); `segment` is the
// segment name for formats that use a two-level `(segment,
// section)` tuple. Mach-O sections live inside segments (e.g.
// `__TEXT,__text` = section `__text` in segment `__TEXT`); ELF
// and PE leave `segment` empty. This split was anchored as plan
// 14 §3.1 **D-LK3-1** during LK1; closed by LK3.
struct DSS_EXPORT ObjectFormatSectionInfo {
    SectionKind   kind{};            // universal kind enum
    std::string   name;              // section name
                                     //   ELF/PE: ".text" / ".rdata"
                                     //   Mach-O: "__text" / "__data"
    std::string   segment;           // segment name for two-level
                                     // formats (Mach-O: "__TEXT");
                                     // empty for ELF/PE
    std::uint32_t type = 0;          // ELF sh_type / PE Characteristics
                                     // / Mach-O section_64.flags
    std::uint64_t flags = 0;         // ELF sh_flags / unused on PE
                                     // (validate-rejected) and Mach-O
    std::uint64_t addrAlign = 0;     // ELF sh_addralign / Mach-O
                                     // section_64.align (log2 form);
                                     // unused on PE (validate-rejected)
    std::uint64_t entrySize = 0;     // ELF sh_entsize / unused on
                                     // PE + Mach-O
    std::uint64_t virtualAddress = 0;// ELF sh_addr / Mach-O section_64.addr
                                     // — the virtual address the section
                                     // is loaded at in an executable
                                     // image. Unused for relocatable
                                     // artifacts (default 0); the LK1
                                     // cycle 2 ET_EXEC walker reads this
                                     // to populate sh_addr. PE folds the
                                     // equivalent into its
                                     // IMAGE_OPTIONAL_HEADER (LK2 cycle 2)
                                     // and ignores this field.
};

// ── ELF-specific identity block (loaded only when kind == Elf) ──
//
// Mirrors `Elf64_Ehdr::e_ident[EI_CLASS..EI_OSABI]` + `e_machine`.
// Lives on `ObjectFormatData` so the ELF walker reads the values
// from JSON instead of hardcoding them. Each format's identity
// block is a parallel optional sub-struct on `ObjectFormatData`
// (`elf{}` / `pe{}` / future `macho{}`); the architect-anchored
// reason for NOT using `std::variant` is that variant would couple
// every format's identity definition into a single header that
// every walker transitively includes — re-creating the per-arch
// coupling the shape-keyed-walker split was designed to avoid.
// e_type closed enum (gABI Fig. 4-2). ET_DYN (PIE / .so) is
// declared but rejected by validate() until D-LK1-4 closes
// (LK6 dynamic linking). ET_NONE / ET_CORE etc. are not declared
// here — they have no use case in this substrate.
enum class ElfObjectType : std::uint16_t {
    Rel  = 1,  // ET_REL  — relocatable .o
    Exec = 2,  // ET_EXEC — non-PIE executable
    Dyn  = 3,  // ET_DYN  — PIE or shared library (anchored D-LK1-4)
};

inline constexpr EnumNameTable<ElfObjectType, 3> kElfObjectTypeTable{{{
    { ElfObjectType::Rel,  "rel"  },
    { ElfObjectType::Exec, "exec" },
    { ElfObjectType::Dyn,  "dyn"  },
}}};

[[nodiscard]] constexpr std::string_view
elfObjectTypeName(ElfObjectType t) noexcept {
    return kElfObjectTypeTable.name(t);
}
[[nodiscard]] constexpr std::optional<ElfObjectType>
elfObjectTypeFromName(std::string_view s) noexcept {
    return kElfObjectTypeTable.fromName(s);
}

struct DSS_EXPORT ElfIdentity {
    std::uint8_t   fileClass = 0;    // ELFCLASS64=2 / ELFCLASS32=1
    std::uint8_t   dataEncoding = 0; // ELFDATA2LSB=1 / ELFDATA2MSB=2
    std::uint8_t   osabi = 0;        // ELFOSABI_NONE=0 / ELFOSABI_GNU=3 / …
    std::uint8_t   abiVersion = 0;
    std::uint16_t  machine = 0;      // e_machine: EM_X86_64=62 / EM_AARCH64=183
    // e_type — ET_REL/ET_EXEC walker arms ship at LK1 cycle 1+2;
    // ET_DYN (c150, D-LK1-4 shared-library half) is the base-0,
    // loader-slid image flavor: a `.so` today (entry-less,
    // interpreter-less — validate() enforces that shape); the PIE
    // EXECUTABLE is the anchored follow-up (ET_DYN + PT_INTERP +
    // entry trampoline — the validate() rules relax there, see the
    // D-LK1-4 registry row). Default = Rel preserves LK1 cycle 1
    // schemas unchanged.
    ElfObjectType  objectType = ElfObjectType::Rel;
    // PT_LOAD `p_align` for Exec images. The Linux kernel rejects
    // ELF executables whose `p_align` is smaller than the runtime
    // page size (`ENOEXEC` at `execve()` time). Common values:
    // 0x1000 (4 KB — x86_64, ARM64 with 4K pages); 0x4000 (16 KB —
    // Apple Silicon Asahi configurations); 0x10000 (64 KB — ARM64
    // with CONFIG_ARM64_64K_PAGES, some AWS Graviton kernels).
    // Default 0 surfaces as a load-time validate failure for Exec
    // (D-LK6-3 closure) — the schema author MUST declare it
    // explicitly per (arch × OS). ET_REL leaves this 0 (no
    // program headers, value is unused).
    std::uint64_t  pageAlign = 0;
    // PT_INTERP path — the dynamic linker the kernel `execve()`'s
    // first when loading this executable. Typical Linux x86_64:
    // "/lib64/ld-linux-x86-64.so.2". Required when objectType ==
    // Exec AND `externImports` is non-empty (FFI / LK6 cycle 2b);
    // empty for self-contained executables (LK1 cycle 2) and for
    // ET_REL relocatable objects. The walker emits this string as
    // the `.interp` section's contents and a PT_INTERP program
    // header pointing at it. MUST be empty for ET_DYN today (a
    // `.so` is loaded by an already-running ld.so, never execve'd;
    // validate() rejects) — the PIE-executable follow-up is exactly
    // the ET_DYN + interpreter combination (D-LK1-4 remainder).
    std::string    interpreter;
    // DT_SONAME for an ET_DYN shared library (c150, D-LK1-4): the
    // logical name the dynamic linker records when an executable
    // links against this `.so` (`libfoo.so.1`). OPTIONAL — empty
    // emits NO DT_SONAME (matching `gcc -shared` without
    // `-Wl,-soname`, where the consumer's linker records the file
    // name instead). Config-driven: the schema field is the one
    // knob; the walker never derives a soname from the output file
    // name (the walker emits bytes and does not know it). Only
    // legal on `type: "dyn"` schemas — validate() rejects it on
    // Rel/Exec (dead config there).
    std::string    soname;
    // Eager-vs-lazy dynamic-binding choice. `true` (the v1 stance,
    // plan 14 §5 risk row) emits `DT_FLAGS_1 = DF_1_NOW` so all
    // GOT slots are resolved at load via `R_X86_64_GLOB_DAT` in
    // `.rela.dyn` — simpler kernel surface, no PLT0 resolver stub
    // needed. `false` is the lazy-binding upgrade path anchored at
    // D-LK6-11 (route extern relocs to `.rela.plt` + JUMP_SLOT, emit
    // 16-byte PLT0 resolver-trampoline, init GOT slots to "PLT entry
    // + 6"); v1 walker fails loud `K_FormatLacksImportSupport` on
    // `bindNow == false` until D-LK6-11 lands. Default `true`
    // preserves cycle 2b.2's emitted image for schemas that omit
    // the field.
    bool           bindNow = true;
};

// ── PE/COFF-specific identity block (loaded only when kind == Pe) ──
//
// Mirrors the load-bearing fields of `IMAGE_FILE_HEADER` (PE/COFF
// spec §3.3). `TimeDateStamp` is omitted: deterministic builds
// require it to be 0 and that's the substrate default already.
// `SizeOfOptionalHeader` is omitted from the schema: it's
// COMPUTED at emit time from the optional-header size when
// objectType != Obj (Exec/Dll ship the optional header; Obj
// always writes 0).
//
// `objectType` discriminates between PE/COFF object files
// (`MH_OBJECT`-equivalent — `.obj` relocatable, LK2 cycle 1) and
// executable images (`.exe` for Exec / `.dll` for Dll). Mirrors the
// `ElfObjectType` closed-enum pattern. All three members have walker
// arms: Obj (LK2 cycle 1), Exec (LK2 cycle 2, D-LK2-1), Dll (c152,
// D-LK2-4 — the PE mirror of ELF ET_DYN's c150 `.so`: entry-less
// image, `.edata` exports, complete `.reloc`).
enum class PeObjectType : std::uint16_t {
    Obj  = 1,  // .obj relocatable — LK2 cycle 1 (default; preserves
               //                    LK2 cycle 1 schemas unchanged).
    Exec = 2,  // .exe executable — LK2 cycle 2 (closes D-LK2-1).
    Dll  = 3,  // .dll dynamic library — c152 (closes D-LK2-4 part 1;
               //                        DllMain arm is the pinned
               //                        follow-up D-LK2-DLL-DLLMAIN-ENTRY).
};

inline constexpr EnumNameTable<PeObjectType, 3> kPeObjectTypeTable{{{
    { PeObjectType::Obj,  "obj"  },
    { PeObjectType::Exec, "exec" },
    { PeObjectType::Dll,  "dll"  },
}}};

[[nodiscard]] constexpr std::string_view
peObjectTypeName(PeObjectType t) noexcept {
    return kPeObjectTypeTable.name(t);
}
[[nodiscard]] constexpr std::optional<PeObjectType>
peObjectTypeFromName(std::string_view s) noexcept {
    return kPeObjectTypeTable.fromName(s);
}

struct DSS_EXPORT PeIdentity {
    std::uint16_t machine = 0;          // IMAGE_FILE_MACHINE_AMD64=0x8664
                                        // / I386=0x014C / ARM64=0xAA64
    std::uint16_t characteristics = 0;  // file-level flags; conventionally
                                        // 0 for relocatable .obj
    PeObjectType  objectType = PeObjectType::Obj;
};

// ── PE32+ Optional Header (loaded when PE objectType != Obj) ──
//
// Mirrors `IMAGE_OPTIONAL_HEADER64` (PE/COFF spec §3.4). Only the
// load-bearing fields are declared — every field the Windows loader
// requires for a minimal `.exe`/`.dll` is here; the deluxe data
// directories (debug, security, etc.) are emitted as zero by the
// walker (NumberOfRvaAndSizes=16; the populated entries are Import/
// IAT/Exception/BaseReloc/TLS on exec, plus Export on dll — c152).
//
// Validate() requires all fields populated when objectType is
// Exec/Dll; Obj rejects any non-zero field (config-error trap
// mirroring the ELF ET_REL `virtualAddress=0` symmetry).
struct DSS_EXPORT PeOptionalHeader {
    std::uint16_t magic = 0;                // PE32+=0x20B / PE32=0x10B
    std::uint64_t imageBase = 0;            // preferred load VA
    std::uint32_t sectionAlignment = 0;     // virtual section align (≥ page)
    std::uint32_t fileAlignment = 0;        // raw section align (512..64K)
    std::uint16_t majorOperatingSystemVersion = 0;
    std::uint16_t minorOperatingSystemVersion = 0;
    std::uint16_t majorSubsystemVersion = 0;
    std::uint16_t minorSubsystemVersion = 0;
    std::uint16_t subsystem = 0;            // IMAGE_SUBSYSTEM_WINDOWS_CUI=3
                                            // / WINDOWS_GUI=2
    std::uint16_t dllCharacteristics = 0;   // DYNAMIC_BASE|HIGH_ENTROPY|NX_COMPAT
    std::uint64_t sizeOfStackReserve = 0;
    std::uint64_t sizeOfStackCommit = 0;
    std::uint64_t sizeOfHeapReserve = 0;
    std::uint64_t sizeOfHeapCommit = 0;
    // Authenticode codesign placeholder reservation (plan 14 LK7
    // — closes the PE half). Non-zero requests the walker to:
    //  (1) append `attributeCertReserveSize` zero bytes at the end
    //      of the emitted PE file, 8-byte aligned per PE COFF §5.9.1
    //      (`WIN_CERTIFICATE.dwLength` is u32 8-byte-aligned);
    //  (2) set `IMAGE_DIRECTORY_ENTRY_SECURITY[4]` (data directory
    //      index 4) — its `VirtualAddress` field is OVERLOADED as a
    //      raw FILE OFFSET (NOT an RVA) per PE COFF §3.4.6 (this is
    //      the ONLY data directory with that semantic); the
    //      attribute cert table sits OUTSIDE the loaded image and
    //      is never mapped into the process VA. Size = u32 byte
    //      count (`attributeCertReserveSize`).
    //  Plan 16 (codesign + publish) fills the reserved bytes
    //  post-link with the Authenticode PKCS#7 / RFC 3161 blob.
    //  Default 0 = no reservation, no security directory entry.
    //  Must be a multiple of 8 (PE COFF spec; the walker rejects
    //  any other value at `validate()`).
    std::uint32_t attributeCertReserveSize = 0;
};

// ── Mach-O-specific identity block (loaded only when kind==MachO) ──
//
// Mirrors the load-bearing fields of `mach_header_64` (Apple OS X
// ABI Mach-O File Format Reference). `magic` is fixed at MH_MAGIC_64
// (0xFEEDFACF) and not exposed on the schema — the substrate writes
// it unconditionally for the 64-bit walker. `reserved` is zero by
// definition.
// Mach-O `mach_header_64.filetype` closed enum, mirroring
// ElfObjectType / PeObjectType. Numeric values match
// <mach-o/loader.h>'s MH_OBJECT / MH_EXECUTE / MH_DYLIB. All three
// have walker arms: Object (LK3 cycle 1), Execute (LK3 cycle 2),
// Dylib (c153, D-LK3-3 — the Mach-O mirror of ELF ET_DYN's c150
// `.so` + PE Dll's c152 `.dll`: entry-less image, LC_ID_DYLIB,
// export trie in LC_DYLD_INFO_ONLY, no __PAGEZERO).
enum class MachOObjectType : std::uint32_t {
    Object  = 1,   // MH_OBJECT  — relocatable .o
    Execute = 2,   // MH_EXECUTE — executable image
    Dylib   = 6,   // MH_DYLIB   — dynamic library (c153, D-LK3-3)
};

inline constexpr EnumNameTable<MachOObjectType, 3> kMachOObjectTypeTable{{{
    { MachOObjectType::Object,  "object"  },
    { MachOObjectType::Execute, "execute" },
    { MachOObjectType::Dylib,   "dylib"   },
}}};

[[nodiscard]] constexpr std::string_view
machoObjectTypeName(MachOObjectType t) noexcept {
    return kMachOObjectTypeTable.name(t);
}
[[nodiscard]] constexpr std::optional<MachOObjectType>
machoObjectTypeFromName(std::string_view s) noexcept {
    return kMachOObjectTypeTable.fromName(s);
}

struct DSS_EXPORT MachOIdentity {
    std::uint32_t cputype = 0;       // CPU_TYPE_X86_64=0x01000007
                                     // / CPU_TYPE_ARM64=0x0100000C
    std::uint32_t cpusubtype = 0;    // CPU_SUBTYPE_X86_64_ALL=3
                                     // / CPU_SUBTYPE_ARM64_ALL=0
    MachOObjectType filetype = MachOObjectType::Object;
                                     // LK3 cycle 1 ships MH_OBJECT;
                                     // LK3 cycle 2 adds MH_EXECUTE
                                     // (closes D-LK3-2); c153 adds
                                     // MH_DYLIB (closes D-LK3-3).
    std::uint32_t flags = 0;         // MH_SUBSECTIONS_VIA_SYMBOLS=0x2000
                                     // / MH_PIE=0x200000 (mandatory for
                                     //   modern macOS exec). Optional in
                                     //   the schema; carried VERBATIM into
                                     //   mach_header_64.flags and read by
                                     //   no writer code.
                                     // ★ Every MH_OBJECT format DECLARES
                                     //   0x2000 since 2026-08-20: it is the
                                     //   header half of the pair (with
                                     //   N_ALT_ENTRY in n_desc) that lets a
                                     //   reader tell a file-local body from
                                     //   an interior label — see
                                     //   D-LINK-NONEXTERNAL-DEFINED-SYMBOL-
                                     //   READ-AS-BLOCK-LABEL-NOT-ATOM and
                                     //   the rationale in each object
                                     //   format's `macho.$comment`.
};

// ── Mach-O image block (loaded when filetype is MH_EXECUTE or
//    MH_DYLIB — every loadable image flavor; rejected on MH_OBJECT) ──
//
// Carries the executable-only Mach-O identity fields. Mirrors LK1
// cycle 2's universal pattern (`virtualAddress` on sections,
// `entryPoint` on top-level schema) plus Mach-O-specific load
// commands the walker emits: LC_LOAD_DYLINKER (the dynamic linker
// path — `/usr/lib/dyld` on macOS) and LC_LOAD_DYLIB (each
// declared library the executable needs at load time — libSystem
// at minimum for any process that calls libc/exit).
//
// `pageZeroSize` is the size of the __PAGEZERO segment: a zero-
// protection segment placed at vmaddr=0 of size 0x100000000 (4 GiB)
// on x86_64-darwin / ARM64-darwin. Catches null-pointer derefs
// at the kernel level — the loader rejects MH_EXECUTE without one.
// Declared as a schema field rather than hardcoded because ARM64
// configurations / debug builds may shrink it.
//
// validate() requires `pageZeroSize > 0`, non-empty `dylinkerPath`,
// and at least one `loadDylibs` entry when filetype==MH_EXECUTE.
//
// `loadDylibs` is a `vector<MachODylibRef>` (row type), NOT a
// `vector<string>` — `dylib_command` on the wire has three trailing
// version u32 fields (timestamp, current_version, compatibility_
// version) that the walker writes as zero today but future cycles
// will populate. A row type avoids the parallel-vector anti-pattern
// (type-design O3 fold-in, LK2 cycle 2 + LK3 cycle 2 review).
struct DSS_EXPORT MachODylibRef {
    std::string path;
    // Reserved (zero today; populated when first consumer needs):
    //   std::uint32_t timestamp = 0;
    //   std::uint32_t currentVersion = 0;
    //   std::uint32_t compatibilityVersion = 0;
};

// Ad-hoc code-signature request (D-LK7-ADHOC-CODESIGN-MACHO, increment
// 2/2). When present on a MH_EXECUTE image, the Mach-O walker FILLS the
// LC_CODE_SIGNATURE reservation with a real ad-hoc CodeDirectory +
// SuperBlob (page-hash table over the signed file bytes) so the binary
// satisfies the macOS / Apple-Silicon AMFI loader, which refuses any
// unsigned executable. "Ad-hoc" = no CMS / identity / certificate; the
// CodeDirectory carries the CS_ADHOC flag and dyld trusts the embedded
// page hashes alone. The reservation SIZE is DERIVED from this block
// (`adHocCodeSignatureSize`), so a hand-typed `codeSignatureSize` is not
// needed when `codeSignature` is set.
//
// The two enums are closed (one variant each today) so a typo in the
// JSON fails loud at load (mirrors `ExternCallDispatch`'s closed-enum
// `…FromName` discipline) rather than silently selecting a default
// signing scheme. Additional schemes (e.g. a CMS-signed `Authenticode`-
// equivalent, or SHA-1 legacy slots) add an enum slot + a name-table
// row, never a magic string.
struct DSS_EXPORT MachOCodeSignature {
    enum class Kind : std::uint8_t {
        AdHoc = 1,  // CS_ADHOC CodeDirectory, no CMS / identity
    };
    enum class HashAlgo : std::uint8_t {
        Sha256 = 1,  // CS_HASHTYPE_SHA256 (hashType=2 on the wire)
    };

    Kind          kind          = Kind::AdHoc;
    HashAlgo      hashAlgorithm = HashAlgo::Sha256;
    // Code-slot page size in bytes; one SHA-256 hash per page of the
    // signed region. Must be a power of two (the CodeDirectory stores
    // log2(pageSize) in a single byte). Apple uses 4096 (0x1000)
    // universally for code pages regardless of the VM page size.
    std::uint32_t pageSize      = 4096;
    // CodeDirectory identifier C-string (the `identOffset` payload).
    // Apple uses the bundle id or, for a bare executable, its leaf
    // name. Must be non-empty (the kernel keys the signature on it).
    std::string   identifier;
};

inline constexpr EnumNameTable<MachOCodeSignature::Kind, 1>
kMachOCodeSignatureKindTable{{{
    { MachOCodeSignature::Kind::AdHoc, "adhoc" },
}}};

inline constexpr EnumNameTable<MachOCodeSignature::HashAlgo, 1>
kMachOCodeSignatureHashAlgoTable{{{
    { MachOCodeSignature::HashAlgo::Sha256, "sha256" },
}}};

[[nodiscard]] constexpr std::optional<MachOCodeSignature::Kind>
machoCodeSignatureKindFromName(std::string_view s) noexcept {
    return kMachOCodeSignatureKindTable.fromName(s);
}
[[nodiscard]] constexpr std::optional<MachOCodeSignature::HashAlgo>
machoCodeSignatureHashAlgoFromName(std::string_view s) noexcept {
    return kMachOCodeSignatureHashAlgoTable.fromName(s);
}

// LC_BUILD_VERSION (`build_version_command`). Declares the file's target
// PLATFORM and minimum-OS / SDK versions. Two DIFFERENT consumers need it,
// with different severities:
//   * an IMAGE — modern dyld (macOS 11+ / Apple Silicon, i.e. dyld4)
//     identifies a main executable's platform from this command, and an
//     executable carrying no platform load command (neither LC_BUILD_VERSION
//     nor the legacy LC_VERSION_MIN_MACOSX) is REJECTED at load, a distinct
//     EBADMACHO-class blocker from segment misalignment;
//   * a RELOCATABLE OBJECT — ld64 falls back to guessing and says so, once
//     per input: `ld: warning: no platform load command found in '<tu>.o',
//     assuming: macOS`. Not fatal, but real clang stamps every `.o`, so an
//     object without it is a fidelity gap that puts a warning on every link.
// `minOs`/`sdk` use the on-wire nibble encoding `(major<<16) | (minor<<8) |
// patch` (e.g. 11.0.0 → 0x000B0000). Optional in the schema
// (`image.buildVersion`) — absent → no LC_BUILD_VERSION emitted, by EVERY
// walker that emits one, so a format that declares nothing is byte-identical
// to the pre-LC_BUILD_VERSION layout. The platform is a CLOSED enum
// (name → value) so a typo fails loud at load rather than silently picking a
// platform. (D-LK10-ENTRY-MACHO-EXIT.)
//
// ── THE PLATFORM VOCABULARY IS COMPLETE, NOT macOS-ONLY ───────────────
//
// It shipped with ONE enumerator (`MacOs`), which made every non-macOS Apple
// target — iOS above all — reachable only by editing C++, in a project whose
// whole design premise is that a new target is a `.format.json` edit. The
// mechanism was always agnostic (the loader decodes a NAME through this table
// and `macho.cpp` writes `static_cast<uint32_t>(bv.platform)`; there is no
// platform branch anywhere), so the defect was purely the vocabulary's
// PARTIAL coverage of a CLOSED ABI enum.
//
// ⚠ THIS ADDS NO MECHANISM, and the distinction matters because the bar
// forbids speculative machinery. There is no new code path, no branch, no
// feature — the engine already walks this table generically and writes
// whatever u32 it decodes. These are NAME ROWS for values Apple has already
// fixed in its ABI, exactly like the registered artifact-profile set. Partial
// coverage of a closed vocabulary is the defect; a table row is not a feature.
//
// ✔MEASURED 2026-08-13, both halves, on the operator's Apple Silicon Mac
// (macOS 26.5.2, Xcode SDK MacOSX.sdk, Apple `ld` PROJECT:ld-1267):
//   * THE VALUES come from `<mach-o/loader.h>` itself
//     (`grep 'define PLATFORM_' "$(xcrun --show-sdk-path)/usr/include/mach-o/
//     loader.h"`), which documents 1..24 unconditionally — 13..24 are NOT
//     behind `__OPEN_SOURCE__` or any other guard. All 24 are carried, because
//     stopping at 12 would re-create at a new boundary the exact partial-
//     coverage defect this change exists to remove.
//   * THE SPELLINGS are Apple's own `-platform_version` vocabulary, probed one
//     by one rather than assumed: every name below was fed to
//     `ld -platform_version <name> 1.0 1.0` and ACCEPTED. The hyphen is
//     Apple's, not this project's — `iossimulator` is REJECTED
//     (`ld: -platform_version unknown platform`) while `ios-simulator` is
//     accepted. `maccatalyst` is accepted too; the hyphenated form is chosen
//     so every multi-word row is uniform, which is also this repo's config
//     spelling convention (`leading-underscore`, `case-insensitive`,
//     `register-machine`).
//
// DELIBERATELY ABSENT, both of them real `PLATFORM_*` macros:
//   * `PLATFORM_UNKNOWN` (0) — an LC_BUILD_VERSION that declares no platform
//     is strictly worse than emitting no LC_BUILD_VERSION at all: it makes the
//     file assert "unknown" instead of leaving ld64 to apply its documented
//     assumption. Omitting `image.buildVersion` is the supported way to say
//     nothing.
//   * `PLATFORM_ANY` (0xFFFFFFFF) — a wildcard for MATCHING a platform, not
//     for stamping one into a file a producer writes.
// Neither can be reached by accident: an unlisted name fails loud at schema
// load (C_MalformedJson at /image/buildVersion/platform).
struct DSS_EXPORT MachOBuildVersion {
    // Values ARE Apple's `PLATFORM_*` numbers and are written verbatim into
    // `build_version_command.platform`, so the enumerator values (and the
    // table row order below, which mirrors them) are ABI, not style.
    enum class Platform : std::uint32_t {
        MacOs               = 1,   // PLATFORM_MACOS
        Ios                 = 2,   // PLATFORM_IOS
        TvOs                = 3,   // PLATFORM_TVOS
        WatchOs             = 4,   // PLATFORM_WATCHOS
        BridgeOs            = 5,   // PLATFORM_BRIDGEOS
        MacCatalyst         = 6,   // PLATFORM_MACCATALYST
        IosSimulator        = 7,   // PLATFORM_IOSSIMULATOR
        TvOsSimulator       = 8,   // PLATFORM_TVOSSIMULATOR
        WatchOsSimulator    = 9,   // PLATFORM_WATCHOSSIMULATOR
        DriverKit           = 10,  // PLATFORM_DRIVERKIT
        VisionOs            = 11,  // PLATFORM_VISIONOS
        VisionOsSimulator   = 12,  // PLATFORM_VISIONOSSIMULATOR
        Firmware            = 13,  // PLATFORM_FIRMWARE
        SepOs               = 14,  // PLATFORM_SEPOS
        MacOsExclaveCore    = 15,  // PLATFORM_MACOS_EXCLAVECORE
        MacOsExclaveKit     = 16,  // PLATFORM_MACOS_EXCLAVEKIT
        IosExclaveCore      = 17,  // PLATFORM_IOS_EXCLAVECORE
        IosExclaveKit       = 18,  // PLATFORM_IOS_EXCLAVEKIT
        TvOsExclaveCore     = 19,  // PLATFORM_TVOS_EXCLAVECORE
        TvOsExclaveKit      = 20,  // PLATFORM_TVOS_EXCLAVEKIT
        WatchOsExclaveCore  = 21,  // PLATFORM_WATCHOS_EXCLAVECORE
        WatchOsExclaveKit   = 22,  // PLATFORM_WATCHOS_EXCLAVEKIT
        VisionOsExclaveCore = 23,  // PLATFORM_VISIONOS_EXCLAVECORE
        VisionOsExclaveKit  = 24,  // PLATFORM_VISIONOS_EXCLAVEKIT
    };
    // ⚠ THE DEFAULT IS INERT, NOT A POLICY. `buildVersion` is an
    // `std::optional` on the image, and every consumer gates on
    // `has_value()`, so this member is only ever read after the loader has
    // OVERWRITTEN it from the schema's declared `platform` (a required field
    // — a `buildVersion` object without one fails loud). It is NOT an
    // "assume macOS if unset" fallback: there is no unset-but-emitted state
    // for it to cover. An iOS format gets iOS purely by declaring it.
    Platform      platform = Platform::MacOs;
    std::uint32_t minOs = 0;  // (major<<16)|(minor<<8)|patch
    std::uint32_t sdk   = 0;  // (major<<16)|(minor<<8)|patch
};

inline constexpr EnumNameTable<MachOBuildVersion::Platform, 24>
kMachOBuildVersionPlatformTable{{{
    { MachOBuildVersion::Platform::MacOs,               "macos"                 },
    { MachOBuildVersion::Platform::Ios,                 "ios"                   },
    { MachOBuildVersion::Platform::TvOs,                "tvos"                  },
    { MachOBuildVersion::Platform::WatchOs,             "watchos"               },
    { MachOBuildVersion::Platform::BridgeOs,            "bridgeos"              },
    { MachOBuildVersion::Platform::MacCatalyst,         "mac-catalyst"          },
    { MachOBuildVersion::Platform::IosSimulator,        "ios-simulator"         },
    { MachOBuildVersion::Platform::TvOsSimulator,       "tvos-simulator"        },
    { MachOBuildVersion::Platform::WatchOsSimulator,    "watchos-simulator"     },
    { MachOBuildVersion::Platform::DriverKit,           "driverkit"             },
    { MachOBuildVersion::Platform::VisionOs,            "visionos"              },
    { MachOBuildVersion::Platform::VisionOsSimulator,   "visionos-simulator"    },
    { MachOBuildVersion::Platform::Firmware,            "firmware"              },
    { MachOBuildVersion::Platform::SepOs,               "sepos"                 },
    { MachOBuildVersion::Platform::MacOsExclaveCore,    "macos-exclavecore"     },
    { MachOBuildVersion::Platform::MacOsExclaveKit,     "macos-exclavekit"      },
    { MachOBuildVersion::Platform::IosExclaveCore,      "ios-exclavecore"       },
    { MachOBuildVersion::Platform::IosExclaveKit,       "ios-exclavekit"        },
    { MachOBuildVersion::Platform::TvOsExclaveCore,     "tvos-exclavecore"      },
    { MachOBuildVersion::Platform::TvOsExclaveKit,      "tvos-exclavekit"       },
    { MachOBuildVersion::Platform::WatchOsExclaveCore,  "watchos-exclavecore"   },
    { MachOBuildVersion::Platform::WatchOsExclaveKit,   "watchos-exclavekit"    },
    { MachOBuildVersion::Platform::VisionOsExclaveCore, "visionos-exclavecore"  },
    { MachOBuildVersion::Platform::VisionOsExclaveKit,  "visionos-exclavekit"   },
}}};

// The table's row count IS the vocabulary's size, so it must equal Apple's
// contiguous 1..24 range. Without this an enumerator added without its row
// would compile, load nothing under its name, and silently fall back to row
// 0 ("macos") in `name()` — a wrong platform stamped into a real file.
static_assert(kMachOBuildVersionPlatformTable.rows.size()
                  == static_cast<std::size_t>(
                         MachOBuildVersion::Platform::VisionOsExclaveKit),
              "every PLATFORM_* enumerator 1..24 must carry a name row; the "
              "values are contiguous, so size == the largest enumerator");

[[nodiscard]] constexpr std::optional<MachOBuildVersion::Platform>
machoBuildVersionPlatformFromName(std::string_view s) noexcept {
    return kMachOBuildVersionPlatformTable.fromName(s);
}
[[nodiscard]] constexpr std::string_view
machoBuildVersionPlatformName(MachOBuildVersion::Platform p) noexcept {
    return kMachOBuildVersionPlatformTable.name(p);
}

// Default VM segment page size for Mach-O segment layout (the x86_64 /
// 4 KiB convention). Apple Silicon (arm64) requires 16 KiB (0x4000);
// see `MachOImage::segmentPageSize`. A named constant so validate()'s
// MH_OBJECT "no image block" detection can tell the default apart from
// an explicitly-set value.
inline constexpr std::uint64_t kDefaultMachoSegmentPageSize = 0x1000u;  // 4 KiB

struct DSS_EXPORT MachOImage {
    std::uint64_t pageZeroSize = 0;        // __PAGEZERO vmsize
                                           // (MUST be 0 on MH_DYLIB — a
                                           // dylib carries no __PAGEZERO;
                                           // ld64 likewise rejects
                                           // -pagezero_size on non-main
                                           // links. validate() enforces
                                           // >0 on Execute, ==0 on Dylib.)
    // VM segment page size — the granularity at which every
    // LC_SEGMENT_64's vmaddr / vmsize / fileoff is aligned (and the
    // file is padded between segments). The kernel's mmap-congruence
    // rule is `vmaddr % segmentPageSize == fileoff % segmentPageSize`;
    // misaligned segments are rejected at exec with errno == EBADMACHO
    // ("Malformed Mach-O file"). x86_64-darwin uses 4 KiB (the default);
    // **Apple Silicon (arm64-darwin) REQUIRES 16 KiB (0x4000)** — the
    // dominant cause of EBADMACHO for hand-built arm64 binaries. This is
    // a DISTINCT concept from `pageZeroSize` (the __PAGEZERO vmsize) and
    // from `codeSignature.pageSize` (the code-directory HASH page size,
    // which Apple allows at 4 KiB or 16 KiB independently). Read by
    // `macho.cpp` at both the static (`encodeExec`) and dynamic
    // (`encodeExecDynamic`) layout sites. Declared in `.format.json`
    // (`image.segmentPageSize`) — NOT inferred from the cputype — to
    // keep the writer source/target/format-agnostic. validate() requires
    // a power of two. Default 4 KiB → every pre-arm64 Mach-O schema is
    // byte-identical (D-LK10-ENTRY-MACHO-EXIT).
    std::uint64_t segmentPageSize = kDefaultMachoSegmentPageSize;
    std::string   dylinkerPath;            // LC_LOAD_DYLINKER name
                                           // (Execute-only: dyld is the
                                           // EXECUTABLE's loader; a dylib
                                           // is mapped by the already-
                                           // running dyld. validate()
                                           // requires non-empty on
                                           // Execute, EMPTY on Dylib.)
    std::vector<MachODylibRef> loadDylibs; // each → LC_LOAD_DYLIB
    // D-LK3-3 (c153): the MH_DYLIB LC_ID_DYLIB install name — the
    // identity a CLIENT records at link time and dyld resolves at its
    // load (`@rpath/libdss.dylib` is the modern convention). REQUIRED
    // non-empty on a Dylib schema and REJECTED on every other filetype
    // (dead config there): every ld64-produced MH_DYLIB carries an
    // LC_ID_DYLIB, and dyld's two-level-namespace client binding keys
    // on it — emitting a dylib without one is an unverifiable-without-
    // a-Mac corner this substrate does not ship. Config-driven + honest
    // (the c150 DT_SONAME discipline): the walker NEVER derives the
    // name from the output file name (it emits bytes and does not know
    // it), and an unset field fails loud at validate() rather than
    // silently inventing an identity. The shipped
    // `macho64-arm64-darwin-dylib` schema declares a generic default
    // (the same shipped-schema-identity concession as
    // `codeSignature.identifier`); a differently-named artifact
    // overrides via its own format JSON. Not needed for plain
    // dlopen-by-path (dyld keys that on the path), but clients that
    // LINK against the dylib record this string verbatim.
    std::string   installName;
    // Eager-vs-lazy dynamic-binding choice (parallel to
    // `ElfIdentity.bindNow` — same semantic across ELF + Mach-O).
    // `true` (the v1 stance per plan 14 §5 risk row) emits the
    // immediate `LC_DYLD_INFO_ONLY.bind_off` opcode stream and
    // leaves `lazy_bind_off` zero — every __got slot is resolved at
    // load. `false` is the lazy-binding upgrade path anchored at
    // D-LK6-13 (route extern bindings to `lazy_bind_off` opcode
    // stream resolved on first call via `dyld_stub_binder`). v1
    // walker fails loud `K_FormatLacksImportSupport` citing
    // D-LK6-13 on `bindNow == false`. Default `true` preserves
    // the eager-binding semantics across the format trio.
    bool          bindNow = true;
    // Apple codesign placeholder reservation (plan 14 LK7 —
    // closes the Mach-O half). Non-zero requests the walker to:
    //  (1) reserve `codeSignatureSize` zero bytes at the end of
    //      the `__LINKEDIT` segment (8-byte aligned per Apple's
    //      `cs_blobs.h` — SuperBlob alignment matches `dwLength`'s
    //      symmetric requirement on the PE side);
    //  (2) emit an `LC_CODE_SIGNATURE` (cmd=0x1D, cmdsize=16) load
    //      command pointing `dataoff` at the reserved file offset
    //      and `datasize` at `codeSignatureSize`.
    //  Plan 16 (codesign + publish) fills the reserved bytes
    //  post-link with the Apple Code Directory SuperBlob (CodeDir
    //  + Requirements + Entitlements + CMS signature).
    //  Default 0 = no reservation, no LC_CODE_SIGNATURE load
    //  command. Must be a multiple of 8 (Apple SuperBlob
    //  alignment; the walker rejects any other value at
    //  `validate()`).
    std::uint32_t codeSignatureSize = 0;
    // Ad-hoc code-signature FILL request (D-LK7-ADHOC-CODESIGN-MACHO
    // increment 2/2). When set, the walker DERIVES the reservation size
    // from this block (via `adHocCodeSignatureSize`) — overriding any
    // hand-typed `codeSignatureSize` — and writes a real CodeDirectory +
    // SuperBlob into the reserved region instead of zeroes. When unset,
    // the legacy `codeSignatureSize`-only placeholder path (zero-fill)
    // is preserved unchanged. validate() rejects this block on a
    // MH_OBJECT (like the rest of the image block).
    std::optional<MachOCodeSignature> codeSignature;
    // LC_BUILD_VERSION platform / min-OS / SDK (D-LK10-ENTRY-MACHO-EXIT).
    // When set, BOTH Mach-O exec walkers emit a `build_version_command`
    // so dyld can identify the platform — REQUIRED for a main executable
    // to load on macOS 11+ / Apple Silicon. Absent → no LC_BUILD_VERSION
    // (byte-identical to every pre-arm64 / MH_OBJECT schema). validate()
    // rejects this block on a MH_OBJECT (like the rest of the image).
    std::optional<MachOBuildVersion> buildVersion;
    // Modern dyld binding format (Xcode 12+ / macOS 12+). When
    // `true`, the walker emits `LC_DYLD_CHAINED_FIXUPS` (0x80000034)
    // pointing at a `dyld_chained_fixups_header` + chained-pointer
    // table in `__LINKEDIT`, INSTEAD of the legacy
    // `LC_DYLD_INFO_ONLY` opcode stream. Each `__got` slot becomes
    // a 64-bit packed chained pointer; the exact bitfield depends
    // on the variant emitted (DYLD_CHAINED_PTR_64 v1 substrate
    // target):
    //   * `dyld_chained_ptr_64_bind`: ordinal:24, addend:8,
    //     reserved:19, next:12, bind:1 (bind == 1).
    //   * `dyld_chained_ptr_64_rebase`: target:36, high8:8,
    //     reserved:7, next:12, bind:1 (bind == 0).
    // The `next` field is a 12-bit unit-offset to the next chained
    // pointer on the same page (0 = end of chain). v1 supports
    // BIND-only chains (extern imports → `__got`); rebase support
    // deferred per D-LK6-14-REBASE.
    //
    // D-LK6-14 closure (chained-fixups emitter for DYLD_CHAINED_PTR_64).
    // Requires `bindNow == true` (lazy binding is incompatible with
    // chained fixups in v1; v2 dyld supports lazy via stub trampolines
    // but the walker rejects that combo loud as
    // K_FormatLacksImportSupport). ARM64e pointer-authentication
    // (DYLD_CHAINED_PTR_ARM64E) deferred — D-LK6-14-ARM64E. When that
    // arm lands, it pairs with D-LK6-14-CHAINEDPTR-FORMAT-COUPLING
    // (the pointer_format ↔ bitfield-layout coupling — reciprocal cite
    // at `macho_chained_fixups.hpp::kDyldChainedPtrFormat64`). 32-bit
    // Mach-O chained-fixups deferred — D-LK6-14-32 (paired with
    // FormatGuess::MachO32).
    //
    // Default `false` preserves the legacy LC_DYLD_INFO_ONLY path so
    // shipped formats opt in explicitly. (No shipped format enables it
    // yet — both darwin-exec formats use the legacy opcode-stream bind;
    // the chained-fixups emitter is exercised by unit tests only.)
    bool          useChainedFixups = false;
};

// ── Output container (D-FF1-AR-STATICLIB-DRIVER-WIRING, c171) ──────
//
// WHAT the format's output is PACKAGED as — a single standalone file
// (the default: one relocatable `.o`/`.obj` OR one loadable image) vs
// an `ar` static ARCHIVE bundling N relocatable members a foreign
// linker later pulls + merges (a `.a` / `.lib` static library).
//
// This axis is ORTHOGONAL to `isImageFlavor()` (object-vs-image): an
// archive holds RELOCATABLE OBJECTS (never images — `validate()`
// rejects `container: archive` on any image-flavor schema, since a
// foreign linker cannot pull a member out of a pre-linked image); a
// `single` file can be either an object or an image. The combos:
//   * single + object → one `.o`/`.obj`
//   * single + image  → one `.exe`/`.so`/`.dll`/`.dylib`
//   * archive + object → a `.a`/`.lib` (the staticlib formats)
//   * archive + image  → INVALID (validate-rejected)
//
// **Agnosticism (the §B decision, user Option 1 / format-container):**
// output SHAPE stays 100% FORMAT-driven (cli→exec, lib→shared,
// staticlib→archive are ALL selected by the format the driver resolves,
// exactly as `elf.objectType` selects Rel/Exec/Dyn) — the driver
// dispatches on THIS declared field, NEVER on the `artifactProfile`
// name (`artifact_profile.hpp`'s standing veto). A relocatable format
// that declares `container: "archive"` + `artifactProfiles:
// ["staticlib"]` IS the static-library artifact producer; the driver
// routes it to `linkAndWriteStaticArchive`.
//
// Default `Single` ⇒ every format that omits the field is byte-
// identical (the whole shipped set predates this axis).
enum class ObjectFormatContainer : std::uint8_t {
    Single  = 0,  // one standalone file (object OR image) — the default
    Archive = 1,  // an `ar` static archive of relocatable members (.a/.lib)
};

inline constexpr EnumNameTable<ObjectFormatContainer, 2> kObjectFormatContainerTable{{{
    { ObjectFormatContainer::Single,  "single"  },
    { ObjectFormatContainer::Archive, "archive" },
}}};

[[nodiscard]] constexpr std::string_view
objectFormatContainerName(ObjectFormatContainer c) noexcept {
    return kObjectFormatContainerTable.name(c);
}
[[nodiscard]] constexpr std::optional<ObjectFormatContainer>
objectFormatContainerFromName(std::string_view s) noexcept {
    return kObjectFormatContainerTable.fromName(s);
}

namespace detail {

struct DSS_EXPORT ObjectFormatData {
    ObjectFormatSchemaId id{};
    std::string          name;         // "elf64-x86_64-linux" etc.
    std::string          version;

    // ── D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES ──
    //
    // WAS: `ObjectFormatKind kind = ObjectFormatKind::Elf;` — a field that
    // both (a) let 26 sites in this tier ask "which format is this?" and
    // (b) DEFAULTED TO ELF, so a default-constructed `ObjectFormatData`
    // silently claimed an ELF identity with `elf.machine == 0` (EM_NONE).
    //
    // NOW: the resolved backend, or `nullptr`. Null is the invalid sentinel
    // and it FAILS CLOSED at every reader — `validate()` refuses the
    // document, `isImageFlavor()`/`isExecFlavor()` answer false,
    // `allowsUndefinedImports()` answers false (the STRICT direction), and
    // the linker refuses to walk. There is no default-format path left to
    // fall into. `ObjectFormatSchema{ObjectFormatData}` remains a public,
    // validation-free constructor, so this pointer is the one thing standing
    // between a hand-built struct and the engine; every accessor that reads
    // it is written to survive it being null.
    link::ObjectFormatBackend const* backend = nullptr;

    // ── D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): output container ──
    //
    // OPTIONAL top-level `"container"` ("single" / "archive"; closed
    // enum, loader fails loud on an unknown spelling). `archive` marks
    // a static-library format whose driver output is an `ar` bundle of
    // relocatable members (`.a`/`.lib`) — the driver routes it to
    // `linkAndWriteStaticArchive`, dispatching on THIS field (never the
    // artifactProfile name). Absent ⇒ `Single` (one standalone file),
    // so every pre-c171 format is byte-identical. `validate()` rejects
    // `archive` on any image-flavor schema (a foreign linker cannot
    // pull a member from a pre-linked image).
    ObjectFormatContainer container = ObjectFormatContainer::Single;

    // ── FC3 c1: the data model (per-OS C-family width triple) ─────
    //
    // REQUIRED top-level `"dataModel"` field ("LP64" / "LLP64" /
    // "ILP32" — closed enum, loader fails loud on missing OR unknown;
    // a silent default would bake wrong `long` widths into every
    // semantic analysis run for the format). The OS lives on the
    // FORMAT schema (pe64-*-windows vs elf64-*-linux share a CPU
    // target), so the width contract does too. Consumed by the driver
    // (`buildCuMir` threads it into `analyze()`) — the per-language
    // `coreByDataModel` overrides, the integer-literal ladder, and the
    // shipped-lib descriptor `signatureByDataModel` all resolve
    // against it. The zero default is the INVALID sentinel: a
    // hand-built ObjectFormatData that never set it is rejected by
    // validate() (the loader path always sets it or fails).
    DataModel            dataModel{};

    // ── D-PP-HEADER-CASE-INSENSITIVE-PE: the `#include` header-NAME case rule ──
    //
    // REQUIRED top-level `"headerNameMatching"` field ("case-sensitive" /
    // "case-insensitive"; closed enum, loader fails loud on missing OR
    // unknown). Like `dataModel` it is an OS property carried by the FORMAT
    // (one x86_64 CPU target serves pe64-windows's case-insensitive rule AND
    // elf64-linux's case-sensitive one), and like `dataModel` it is REQUIRED
    // rather than optional-with-default: an optional key would let a future
    // pe/macho format file silently regress to case-sensitive matching, which
    // is the whole failure class the axis closes.
    //
    // Consumed by `core/types/include_path_resolve.hpp`, which does the ASCII
    // folding ITSELF rather than letting `fs::exists` (i.e. the BUILD HOST's
    // filesystem) decide a case question. The zero default is the INVALID
    // sentinel: a hand-built ObjectFormatData that never set it is rejected by
    // validate() (the loader path always sets it or fails).
    HeaderNameMatching   headerNameMatching{};

    // ── D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN: the C-symbol decoration rule ──
    //
    // REQUIRED top-level `"cSymbolDecoration"` BLOCK (`{"scheme": "none"}` /
    // `{"scheme": "leading-underscore"}`; closed enum, loader fails loud on
    // missing OR unknown). How this format decorates a canonical C identifier
    // to obtain the LINKER-visible name. Like `dataModel` and
    // `headerNameMatching` it is a per-(OS × format) fact one CPU target
    // cannot answer — the same x86_64 target decorates under
    // macho64-x86_64-darwin and does not under elf64-x86_64-linux — and like
    // them it is REQUIRED rather than optional-with-default, because the whole
    // defect being closed is a per-format fact with TWO OWNERS that nothing
    // forced to agree. A silent default would simply make the C++ table's
    // answer the winner again, invisibly, for any file that forgot the key.
    //
    // See `CSymbolDecoration` (core/types/object_format_kind.hpp) for the
    // full rationale, the closed-enum-vs-prefix-string decision, and the
    // MEASURED evidence that `_exit` already names a DIFFERENT function on
    // both undecorated formats. The zero-scheme default is the INVALID
    // sentinel: a hand-built ObjectFormatData that never set it is rejected
    // by validate() (the loader path always sets it or fails).
    CSymbolDecoration    cSymbolDecoration{};

    // ── D-CSUBSET-BITFIELD-ABI-EXACT: the per-ABI bit-field PACKING strategy ──
    //
    // C bit-field allocation is genuinely ABI-defined and is determined by the
    // OBJECT FORMAT / OS, not the CPU (one CPU target — x86_64 — serves BOTH
    // ELF-SysV `gnu_packed` AND PE-MS `msvc_straddle`), so the strategy lives on
    // the FORMAT schema, exactly like `dataModel`. OPTIONAL top-level
    // `"bitFieldStrategy"` ("gnu_packed" / "msvc_straddle"; closed enum, loader
    // fails loud on an unknown spelling). Absent ⇒ `None` (the INVALID sentinel
    // here): the layout params then fall back to the TARGET's declared
    // `aggregateLayout.bitFieldStrategy` (back-compat for direct-API / test
    // callers + targets that predate the format-side field) via
    // `effectiveBitFieldStrategy(target, format)`. A format that never lays out a
    // C aggregate (wasm / spirv skeletons) may omit it; the consumer fails loud
    // only when a bit-field is actually laid out under a `None` effective
    // strategy. ELF → gnu_packed, PE → msvc_straddle, Mach-O → gnu_packed (Apple
    // arm64 does NOT diverge from generic AAPCS64 on bit-field packing).
    BitFieldStrategy     bitFieldStrategy = BitFieldStrategy::None;

    // ── FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the per-format `long double` axis ──
    //
    // OPTIONAL top-level `"longDoubleFormat"` ("f64" / "x87-80" / "ieee128";
    // closed enum, loader fails loud on an unknown spelling — the
    // bitFieldStrategy discipline). The representation is OS/format-determined
    // (x86_64 serves BOTH pe64's 64-bit-IEEE `long double` and ELF-SysV's x87
    // 80-bit), so the axis lives here next to `dataModel`. Absent ⇒ `None`
    // (undeclared): a `long double` row is then UNREALIZED at semantic bind —
    // S_LongDoubleFormatUndeclared, never a silent base-core fallback (wasm /
    // spirv skeletons omit it deliberately). Unlike bitFieldStrategy there is
    // NO target-side fallback field — the axis is format-only.
    LongDoubleFormat     longDoubleFormat = LongDoubleFormat::None;

    // ── TF-C97 (D-PP-FORMAT-DATA-MODEL-PREDEFINES): the format's own
    //    predefined macros ─────────────────────────────────────────────
    //
    // OPTIONAL top-level `"predefinedMacros"` — the SAME entry grammar as
    // the language's `preprocess.predefinedMacros` and the target's
    // `predefinedMacros`, parsed by the SHARED `parsePredefinedMacroArray`,
    // merged with both at preprocess time by `mergePredefinedMacros`.
    //
    // ★ WHAT BELONGS HERE, AND WHY IT IS A THIRD FAMILY RATHER THAN A
    // WIDENING OF THE TARGET'S. The macros a C program uses to read this
    // schema's OWN axes — first among them `dataModel`, whose C-visible face
    // is `__LP64__`/`_LP64`. Those are NOT architecture facts: ONE CPU
    // (x86_64) is LP64 under elf64/macho64 and LLP64 under pe64, so a
    // target-side row would be a lie on one of its own formats — exactly the
    // argument that keeps `bitFieldStrategy` and `longDoubleFormat` here and
    // sends `charIsUnsigned` to the target. Both `.target.json` files record
    // the same conclusion in prose, and this key is what finally lets them
    // stop deferring it.
    //
    // ★ WHY THE MACRO NAMES ARE CONFIG DATA AND NOT DERIVED IN THE LOADER.
    // `__LP64__` is a C-FAMILY SPELLING. A loader that synthesized it from
    // `dataModel == Lp64` would hardcode one source language's vocabulary
    // into the object-format tier, which is the source-agnosticism break the
    // whole config-driven design exists to prevent (and it would have no
    // answer at all for a second language that spells the same fact
    // differently). The loader therefore knows only "this format declares
    // some predefines"; WHICH ones is derived — by the config author, in the
    // file, from that file's own `dataModel` value — and never from a format
    // NAME. Nothing in the engine compares a format name to a literal.
    //
    // OPTIONAL; absent ⇒ empty ⇒ the preprocessor's effective list is
    // byte-identical to the pre-TF-C97 (language + target) list. That is the
    // state the LLP64 and ILP32 formats deliberately stay in: defining
    // NEITHER macro is the whole point of putting the channel here, so their
    // silence is a DECLARED answer, not an omission (each such file records
    // it in a `$predefinedMacrosComment`). Per-entry `availableObjectFormats`
    // still applies and is still filtered ONCE downstream.
    std::vector<PredefinedMacroDef> predefinedMacros;

    // ── NOT HERE: bare-`char` signedness (D-TARGET-CHAR-SIGNEDNESS-PER-
    // PLATFORM) ─────────────────────────────────────────────────────────
    // The axis is (processor × platform), and it is declared ENTIRELY on the
    // TARGET — one key, `charIsUnsigned`, carrying its own per-object-format
    // overrides (`{"default": …, "byObjectFormat": {…}}`), resolved by
    // `TargetSchema::charIsUnsigned(ObjectFormatKind)`. A format-side field
    // was tried and removed: `elf` serves BOTH aarch64 (unsigned) and x86_64
    // (signed), so any flat value here is a lie on one of them, and making it
    // honest would force all 24 format files to enumerate CPU architectures —
    // a layering inversion. Do not re-add a `charSignedness` key here; the
    // closed root-key vocabulary in the loader will reject it.

    // Relocations row — same shape as `TargetSchema::relocations[]`
    // so the reloc-taxonomy unifier (plan 13 §2.6) is symmetric.
    std::vector<ObjectFormatRelocationInfo> relocations;
    substrate::TransparentStringMap<std::uint16_t> relocationNameIndex;
    std::unordered_map<RelocationKind, std::uint16_t> relocationKindIndex;

    // Sections row (D-LK4-2). The walker reads sections by
    // SectionKind; `kind` must be unique across rows so the lookup
    // is unambiguous. `name`/`type`/`flags`/`addrAlign`/`entrySize`
    // are format-specific (interpreted by the walker for its format).
    std::vector<ObjectFormatSectionInfo> sections;
    std::unordered_map<SectionKind, std::uint16_t> sectionKindIndex;

    // Per-format identity sub-blocks. Each is populated ONLY when
    // `kind` matches; otherwise zero-defaulted. The walker reads
    // its arm from JSON; validate() enforces the per-kind
    // populated-ness rule.
    ElfIdentity      elf{};
    PeIdentity       pe{};
    PeOptionalHeader peOptionalHeader{};  // populated only when
                                           //   pe.objectType != Obj
    MachOIdentity    macho{};
    MachOImage       machoImage{};        // populated only when
                                           //   macho.filetype is
                                           //   MH_EXECUTE / MH_DYLIB

    // ── Image-side fields (LK1 cycle 2+) ─────────────────────
    //
    // Universal entry-point symbol name for executable artifacts
    // (e.g. "_start" for Linux/glibc, "main" for hand-rolled
    // executables, the macOS LC_MAIN entry-function name for
    // Mach-O executables, the PE AddressOfEntryPoint symbol for
    // Windows .exe). Empty for relocatable artifacts (.o). The
    // walker resolves this against the AssembledModule's symbols
    // to compute the entry virtual address.
    std::string entryPoint;

    // ── D-LK10-ENTRY Slice B (plan 14 §2.13): ProcessExit substrate ──
    //
    // The trampoline emitter (Slice C) reads these two fields to
    // construct a one-function `Lir` module that calls the user
    // entry then issues the OS process-exit request.
    //
    // `processExit`: per-OS exit mechanism (Syscall or ByNameImport),
    // populated from the format JSON's `processExit` block.
    // nullopt = format declared no exit mechanism (`encodeExec` path
    // is then not runnable; the trampoline emitter fails loud).
    // Vocabulary types (`ExitMechanism` + `ProcessExit`) live in
    // `core/types/target_schema.hpp` alongside the other
    // closed-enum schema vocabulary.
    std::optional<ProcessExit> processExit;

    // ── D-RUNTIME-MAIN-ARGC-ARGV (c88): ProcessArgs substrate ──
    //
    // `processArgs`: per-OS program-entry argument mechanism
    // (StackVector today), populated from the format JSON's
    // `processArgs` block. The trampoline emitter reads it to
    // materialize argc/argv into the entry cc's first two integer
    // argument registers BEFORE calling the user entry (and before
    // any ABI-prologue SP adjustment — the stack offsets are
    // defined against the untouched process-entry SP).
    // nullopt = the format declared no argument mechanism: the
    // trampoline passes whatever the registers hold. That is CORRECT on
    // Mach-O, where dyld already delivers argc/argv in the argument
    // registers before any DSS code runs — an ANSWER, not an omission.
    // PE declares the `crt-argv-accessors` mechanism (UCRT-P4); the
    // c111 msvcrt out-parameter mechanism it replaced is retired.
    // Vocabulary types (`ArgsMechanism` + `ProcessArgs`) live in
    // `core/types/target_schema.hpp` beside ProcessExit.
    std::optional<ProcessArgs> processArgs;

    // ── UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION): the RUNTIME LIBRARY
    //    ROLE table ─────────────────────────────────────────────────
    //
    // OPTIONAL top-level `"runtimeLibraries"` — the SINGLE OWNER of "which
    // image plays which runtime role" for this format. Every spine block that
    // needs an image NAMES A ROLE (`processExit.role`, `processArgs.role`,
    // `sehPersonality.role`, `librarySynthesis.role`) and the loader RESOLVES
    // that role here, copying the image into the block's own path field. Those
    // copies are DERIVED-AT-LOAD, not a second owner — the same relationship
    // `sectionKindIndex` has to `sections`.
    //
    // ★★ FAIL LOUD, BOTH DIRECTIONS, AT LOAD (`RuntimeLibraryRole`'s docblock
    // has the measured reasoning): a block naming a role this table does not
    // declare is REFUSED, and a table row no block names is REFUSED too. The
    // first direction stops a silent "some image" resolution; the second stops
    // inert config accumulating — the `charSignedness` rule in this same file,
    // which rejects a second-owner key by name rather than letting it sit.
    //
    // ★ THE MECHANICAL EXIT CRITERION for the pe CRT migration is stated
    // against THIS field: "no pe format file's `runtimeLibraries` table has ANY
    // value naming `msvcrt.dll`", checked across all four pe flavours. That is
    // strictly stronger than a single-CRT-string criterion, because it also
    // catches a future edit that repoints ONE role at the wrong image.
    //
    // Absent ⇒ empty ⇒ this format names no roles anywhere (every relocatable /
    // staticlib / dyn / dll / wasm / spirv format today). Emptiness is not a
    // default; it is what "declares no spine blocks" looks like.
    RuntimeLibraryTable runtimeLibraries;

    // ── UCRT-P4: the unwinder-personality declaration ─────────────
    //
    // OPTIONAL top-level `"sehPersonality"` block (`{"role": …,
    // "mangledName": …}`). The routine an emitted unwind record names as its
    // handler. `std::nullopt` = this format declares NO personality — NOT a
    // silent default: the SEH funclet synthesis pass fails loud the moment a
    // guarded region resolves under a nullopt-personality format, which is
    // exactly what an ELF/Mach-O build carrying `__try` should get. See
    // `SehPersonality` for what two literals in `src/mir` this deletes.
    std::optional<SehPersonality> sehPersonality;

    // ── UCRT-P4 (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE): the REALIZED
    //    ENTRY-VERB set ─────────────────────────────────────────────
    //
    // Top-level `"entryVerbs"` — the program-entry materialization verbs THIS
    // FORMAT CAN ACTUALLY REALIZE. REQUIRED (non-empty) on every exec-flavored
    // format and REJECTED on every other, by the same paired-cluster discipline
    // `processExit` / `entryCallingConvention` already follow: a format that
    // starts a program must say what it can hand the entry, and a format that
    // starts nothing must not carry dead rows.
    //
    // ★★★ THIS FIELD DELIBERATELY CARRIES NO SIGNATURES, and that is the change
    // this cycle made. It used to be `std::vector<EntryShape> entryShapes` —
    // full `(returns, params, verb)` rows — which made the accepted signature
    // set a FORMAT fact. It is not one. `int wmain(int, wchar_t**)` is a
    // SOURCE-LANGUAGE spelling; a linker format has no opinion on how C spells
    // an entry, only on whether it can produce a wide argument vector at all.
    // So the signature moved to its one owner — the language's
    // `DeclarationRule::entryFunctions` mapping — and what stays here is the
    // one question only a format can answer.
    //
    // ⇒ THE ACCEPTED ENTRY SET IS THE INTERSECTION, computed by the engine:
    // a defined function is a program-entry candidate iff its name matches a
    // declared language row, its signature matches that row, and that row's
    // verb is in THIS set. `argc-wargv` appears here only on
    // `pe64-x86_64-windows-exec`, which is the entire reason `wmain` is a
    // candidate on Windows and nowhere else — with no format-identity branch in
    // shared substrate. Before the verb was the intersection key, candidate
    // selection matched NAMES alone and was format-blind (MEASURED: a
    // `wmain`-only source was selected as the ELF entry).
    //
    // EMPTY IS A DECLARED ANSWER, NOT A MISSING DECLARATION: it says "this
    // format starts no program", which is exactly the relocatable / staticlib /
    // library case, and the engine's "does this build need an entry" predicate
    // reads THIS EMPTINESS — never `isExecFlavor()` and never a format name.
    //
    // See `entry_shape.hpp` for the MEASURED defect the pair closes (a 3-param
    // `main` compiling rc=0 with zero diagnostics on pe64 AND elf64 and
    // faulting at run) and for the C23 5.1.2.2.1 + 3.4.1 argument that makes
    // the two config declarations the conformance documentation artifact.
    std::vector<EntryMaterialization> entryVerbs;

    // `entryCallingConvention`: name of the calling convention the
    // trampoline emitter resolves via
    // `target.callingConventionByName(...)` to look up the
    // status-arg register (the cc's `argGprs[0]`) the trampoline
    // moves the user fn's return value into. Per-OS data: PE-Exec =
    // "ms_x64"; ELF/Mach-O-Exec = "sysv_amd64"; ARM64 = "aapcs64".
    //
    // Without this field, the cc selection would be undefined: ML7
    // callconv lowering (the normal cc selector per function
    // attribute / driver flag) is BYPASSED by the trampoline
    // emitter, so the format must declare the active cc explicitly.
    // Empty on relocatable artifacts (.o) and on exec formats that
    // do not declare `processExit`. The format-side validate() rule
    // requires non-empty `entryCallingConvention` whenever
    // `processExit.has_value()` (paired closure — both fields go
    // together).
    std::string entryCallingConvention;

    // ── D-FFI-EXTERN-CALL-DISPATCH: extern-call shape ────────────
    //
    // How a call to an extern import is reached at the CALL SITE for
    // THIS format: `indirect-slot` (PE IAT / Mach-O __got: deref a
    // pointer slot via the target's `call_indirect_via_extern` opcode)
    // vs `direct-plt` (ELF PLT: a plain direct `call` to the linker's
    // PLT stub). See `ExternCallDispatch` (core/types/object_format_kind.hpp)
    // for the full rationale. Consumed by MIR-to-LIR `lowerCall`.
    //
    // `std::nullopt` = the format did not declare a dispatch model — NOT
    // a silent default to either shape: MIR→LIR fails loud iff a module
    // declares extern imports under a nullopt-dispatch format. Enforced at
    // that precise point (lowering), NOT at validate(): a format that never
    // lowers an extern call (a relocatable / WASM / SPIR-V format, or an
    // exec format built for a non-FFI purpose like the codesign fixtures)
    // may legitimately omit it. The shipped exec formats DO declare it; an
    // unknown VALUE still fails loud at load (the loader's enum check).
    std::optional<ExternCallDispatch> externCallDispatch;

    // ── D-LK-EXTERN-DATA-IMPORT: extern-DATA import binding model ───
    //
    // How an imported library DATA OBJECT (libc `stdout`) is bound
    // into the emitted image. `got-indirect` (the ONLY member) = a
    // loader-bound pointer slot holding the library object's ADDRESS:
    // ELF `.got` + R_*_GLOB_DAT, Mach-O `__got` non-lazy pointer, the
    // PE IAT slot. See `DataImportBinding`
    // (core/types/object_format_kind.hpp) for the full rationale —
    // including why the second member, `copy-relocation`, was DELETED
    // rather than left inert.
    //
    // `std::nullopt` = the format declared no data-import binding —
    // NOT a silent default: the linker's pre-walker gate FAILS LOUD
    // on any surviving extern data import under a nullopt-binding
    // format (a data symbol bound through the function-import
    // machinery would read jump-stub bytes as the object's value —
    // the silent-miscompile class). Every shipped IMAGE format —
    // ELF exec/pie/dyn, PE exe/dll, Mach-O exec/dylib — declares
    // "got-indirect"; only the relocatable flavors omit it (they bind
    // no imports at all; a foreign linker resolves their undefined
    // symbols). An unknown VALUE fails loud at load (the closed-enum
    // check — a typo never falls back, and neither does the deleted
    // "copy-relocation" spelling).
    std::optional<DataImportBinding> dataImportBinding;

    // ── D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): extern-ADDRESS
    //     materialization binding ─────────────────────────────────────
    //
    // How code MATERIALIZES the ADDRESS of an undefined/preemptible
    // extern (function OR data) as a LIVE code-form VALUE (an argument,
    // an automatic's initializer, a returned function pointer) under
    // THIS format. `got` = through a GOT slot the FINAL (foreign) linker
    // synthesizes (arm64 ADR_GOT_PAGE + LD64_GOT_LO12_NC), so a foreign
    // default-PIE link of the emitted relocatable `.o`/`.a` accepts it —
    // an absolute page-pair against a preemptible symbol is rejected
    // "when making a shared object". See `ExternAddrBinding`
    // (core/types/object_format_kind.hpp) for the full rationale.
    //
    // `std::nullopt` = the format declared no binding — NOT a silent
    // default: an `&extern` value then materializes via the ordinary lea
    // (a PC-relative rel32 on x86_64 — already foreign-PIE-safe; an
    // absolute page-pair on arm64 — foreign-PIE-safe ONLY for a
    // DSS-linked exec). Only the arm64 relocatable + static-archive
    // formats declare `got`; the DSS-linked exec/pie/dyn formats omit it
    // (they use the c117 DSS-local got-indirect slot path). Consumed by
    // MIR→LIR `lowerGlobalAddr`'s value-form arm. An unknown VALUE fails
    // loud at load (the closed-enum check — the externCallDispatch /
    // dataImportBinding discipline).
    std::optional<ExternAddrBinding> externAddrBinding;

    // ── D-CSUBSET-THREAD-LOCAL (TLS C1): thread-local access block ──
    //
    // HOW code reaches a thread-local object's per-thread copy under
    // THIS format (`"tlsAccess"` in the JSON): the closed access MODEL
    // (`local-exec` / `pe-indexed` / `macho-tlv`) + the per-format
    // VALUES the x86 access sequence needs (segment-override prefix
    // byte, thread-pointer slot displacement). See `TlsAccessInfo`
    // (core/types/object_format_kind.hpp) for the full rationale.
    //
    // `std::nullopt` = the format declared no TLS access model — NOT a
    // silent default: MIR→LIR fails loud (`K_FormatLacksThreadLocalSupport`)
    // on the first thread-local GlobalAddr under a nullopt-tlsAccess
    // format (lowering a thread-local through the ordinary global path
    // would silently produce ONE process-shared copy — a miscompile of
    // the declared storage duration). ELF-Linux exec formats declare
    // `local-exec {0x64, 0}`; PE / Mach-O omit it until their TLS
    // cycles land (their absence IS the fail-loud gate). An unknown
    // VALUE still fails loud at load (the closed-enum check).
    std::optional<TlsAccessInfo> tlsAccess;

    // ── D-CSUBSET-C11-THREADS-MACHO: shipped-library synthesis vehicle ──
    //
    // THIS format's compiler-synthesized-shim vehicle (`"librarySynthesis"`
    // in the JSON): the closed primitive family (`win32` / `pthread`) a
    // synthesized shipped-library shim (today C11 <threads.h>) emits its
    // body over, plus the native library its on-demand helpers import from.
    // See `LibrarySynthesis` (core/types/object_format_kind.hpp).
    //
    // `std::nullopt` = the format declared no synth vehicle. This is NOT a
    // silent default: a format that carries synthesize-tagged shipped
    // symbols but no vehicle fails loud in `synthesizeThreadsShim` (never a
    // silently-assumed vehicle). ELF omits it (glibc exports the C11 thread
    // API directly → the synth recipe map is empty on elf → clean no-op);
    // pe/macho declare it. An unknown VALUE fails loud at load.
    std::optional<LibrarySynthesis> librarySynthesis;

    // ── D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: stack-reserve capability ──
    //
    // Whether THIS format can record a per-PROGRAM stack reserve in the
    // emitted image (`"stackReserveControl"` in the JSON): the closed
    // VEHICLE naming the header field / load command it lands in, plus the
    // declared legal range a request is validated against. See
    // `StackReserveControl` (core/types/object_format_kind.hpp).
    //
    // `std::nullopt` = this format cannot express a stack reserve. NOT a
    // silent default: `linker::link` fails loud
    // (`K_FormatLacksStackReserveControl`) on a project/CLI request against
    // a nullopt format, because silently dropping the request would ship an
    // image whose stack is NOT what the program asked for — the exact
    // knob-that-lies class. Declared by the PE **exec** format only: PE
    // **dll** omits it (the loader ignores a DLL's SizeOfStackReserve), ELF
    // omits it (no image field carries a stack SIZE — `PT_GNU_STACK` is
    // executability; the size is a runtime/`ulimit` property), Mach-O omits
    // it until an `LC_MAIN.stacksize` walker arm lands. An unknown VEHICLE,
    // or one incoherent with `format.kind`, fails loud at load.
    std::optional<StackReserveControl> stackReserveControl;

    // The REMEDY axis of the same anchor (`"stackReserveUnsupportedReason"`):
    // WHY this format cannot carry a stack reserve, as a closed verb the
    // diagnostic turns into an actionable remedy. Mutually exclusive with
    // `stackReserveControl` (declaring both is contradictory config and fails
    // loud at load). OPTIONAL: a format that declares neither still REFUSES a
    // request — just with the generic message. See
    // `StackReserveUnsupportedReason` (core/types/object_format_kind.hpp) for
    // why each of the four verbs exists and what it tells the user to do.
    std::optional<StackReserveUnsupportedReason> stackReserveUnsupportedReason;

    // ── D-LK2-RODATA closure: producer-data-section capability set ──
    //
    // Schema-declared set of `DataSectionKind` values the format's
    // walker accepts on `AssembledModule.dataItems`. The linker's
    // pre-walker gate consults this BEFORE handing the module to
    // the walker — a format whose walker arm has not yet wired
    // (e.g. ELF + Mach-O until D-LK1-RODATA / D-LK3-RODATA close,
    // PE Obj indefinitely because relocatable .obj does not carry
    // rdata sections) declares an empty set and fails loud on any
    // dataItems carrying a section the format does not advertise.
    //
    // **Agnosticism discipline (HARD VETO — standing rule)**: this
    // field replaces the prior `kind == ObjectFormatKind::Pe`
    // format-name branch in `linker.cpp`. The gate is now schema-
    // declared per-format, not enumerated in C++ — adding a fourth
    // format that supports rodata = drop `"supportedDataSections":
    // ["rodata"]` into its JSON, zero linker source changes.
    //
    // Empty by default — only formats that explicitly declare the
    // capability accept dataItems. Image-flavor-only by validate()
    // rule: a relocatable .obj schema declaring rodata support is
    // a config error (relocatable object files emit rodata via
    // .obj symbol tables, not via this capability gate).
    std::vector<DataSectionKind> supportedDataSections;

    // The artifact profiles this format SERVES — i.e. produces a real
    // artifact for (plan 06 AP3). The format-side symmetric twin of the
    // LANGUAGE's `artifactProfiles[]` (AP1, on GrammarSchemaData): AP1
    // declares which profiles a language SUPPORTS; this declares which
    // profiles a format PRODUCES. The driver enforces
    // `project.artifactProfile ∈ format.artifactProfiles` and fails loud
    // (`D_ArtifactProfileFormatMismatch`) otherwise — so a `cli` project
    // pointed at a shared-library format, or a `lib` pointed at an exec
    // format, is rejected at config-load time, not deep in codegen.
    //
    // Empty by default ⇒ the format serves NO profile (fail-CLOSED,
    // consistent with the language-side empty-set reject): a relocatable
    // `.o`/`.obj` format leaves it empty until a `staticlib` archiver
    // ships; only the 5 executable formats declare `["cli"]` today (gui
    // deferred — no gui codegen). A format may only claim a profile it can
    // ACTUALLY emit (declaring an unproducible profile is itself the
    // "nonsense" the gate exists to prevent). Each entry is validated at
    // load against the shared registered vocabulary
    // (`isRegisteredArtifactProfile`, core/types/artifact_profile.hpp).
    // Schema-declared, NOT enumerated in C++ — a new format opts in by
    // dropping `"artifactProfiles": [...]` into its JSON; the gate is a
    // generic set-membership, never an `if (profile == "...")` branch.
    std::vector<std::string> artifactProfiles;

    // Cross-field invariants:
    //   * relocations: kind != 0, kind unique cross-row, name unique
    //     + non-empty, nativeId != 0 when relocations are non-empty.
    //   * sections: kind unique cross-row, name non-empty.
    //   * ELF identity: when kind == Elf, fileClass / dataEncoding /
    //     machine must all be != 0.
    [[nodiscard]] std::vector<ConfigDiagnostic> validate() const;

    // EXEC-flavor predicate — THE single implementation. `validate()`
    // computes its entry-machinery rules from this, and the public
    // `ObjectFormatSchema::isExecFlavor()` forwards to it, so the
    // rule that rejects a config and the predicate a linker gate asks
    // can never drift apart. (It lives on the DATA type rather than
    // on the schema class because `validate()` is a member of the
    // data type and runs BEFORE any `ObjectFormatSchema` exists —
    // duplicating the four arms into the schema class would have
    // created exactly the two-copy divergence this arc is closing.)
    // See the public forwarder for the full semantics + the ET_DYN
    // caveat.
    // ★ THE DERIVATION MOVED TO THE BACKENDS; IT DID NOT COLLAPSE INTO A
    // DECLARED FLAG. The ELF backend still re-derives the ET_DYN PIE shape
    // from ALL FOUR cluster members (interpreter + processExit +
    // entryCallingConvention + processArgs) rather than trusting one of
    // them, for the reason this function has always carried: a hand-built,
    // validate-bypassing `ObjectFormatData` must not be able to fake a PIE
    // by setting a single field, and `ObjectFormatSchema{ObjectFormatData}`
    // is a public constructor that runs no validation. Had this become a
    // declared `"execFlavor": true` key, the single-field fake would be
    // handed straight back. See `elf_backend.cpp`.
    //
    // Null backend ⇒ false: an unresolved format is not an executable.
    [[nodiscard]] bool isExecFlavor() const noexcept {
        return backend != nullptr && backend->isExecFlavor(*this);
    }
};

} // namespace detail

class DSS_EXPORT ObjectFormatSchema {
public:
    explicit ObjectFormatSchema(detail::ObjectFormatData data) noexcept
        : d_(std::move(data)) {}

    ObjectFormatSchema(ObjectFormatSchema const&)            = delete;
    ObjectFormatSchema& operator=(ObjectFormatSchema const&) = delete;
    ObjectFormatSchema(ObjectFormatSchema&&) noexcept        = default;
    ObjectFormatSchema& operator=(ObjectFormatSchema&&) noexcept = default;

    // Lowercase 64-hex SHA-256 of the EXACT document bytes this schema was
    // loaded from — the cache key for the runtime-object cache, which keys on
    // the config documents a build actually loaded.
    //
    // WHY RETAINED RATHER THAN RECOMPUTED. Re-walking `src/dss-config/` from
    // disk to hash it costs ~165 ms per invocation (MEASURED 2026-08-17: 86
    // files, 2,078,133 bytes; I/O-dominated — walk+read 152–160 ms, hash only
    // 9–13 ms), and would be paid on EVERY build. The loaders already hold the
    // bytes; they read them, parse them, and discard them. Digesting them
    // where they already are costs zero extra I/O and happens once per load
    // (loads are memoized in-process), and retaining 32 bytes of digest
    // instead of up to 440 KB of document is what makes retention free.
    //
    // ⚠ EMPTY MEANS UNKNOWN, NEVER "no content". A schema built through a path
    // that does NOT go via `loadFromText` — the public `ObjectFormatData`
    // constructor above, which tests use to inject data directly — has no
    // document bytes to digest and leaves this EMPTY. That is deliberate: an
    // empty digest is a DETECTABLE unknown a cache can refuse to key on,
    // whereas a fabricated or stale one is a silent wrong key. Every file
    // route (`loadShipped` → `loadFromFile` → `loadFromText`) is digested.
    [[nodiscard]] std::string_view     contentDigest() const noexcept {
        return contentDigest_;
    }

    [[nodiscard]] ObjectFormatSchemaId id()      const noexcept { return d_.id; }
    [[nodiscard]] std::string_view     name()    const noexcept { return d_.name; }
    [[nodiscard]] std::string_view     version() const noexcept { return d_.version; }
    // ★ THE BRIDGE ACCESSOR — kept for the identity branches that still live
    // OUTSIDE this cycle's authorized file set (`src/program/compile_pipeline
    // .cpp`, `src/program/program.cpp`, `src/program/cross_validate_target_
    // format.cpp`, `src/ffi/abi/abi_catalog.cpp` — all anchored, all the same
    // species). The schema tier cannot even spell `ObjectFormatKind::Elf` any
    // more — see the compile-error pin at the top of `object_format_schema.cpp`
    // / `_json.cpp`. When those callers close, this accessor goes with them.
    //
    // ⚠ ONE `src/link/` CALLER REMAINS. An earlier draft said "NOTHING in
    // `src/link/` calls it any more"; an independent audit measured otherwise.
    // `linker.cpp` sets `LinkedImage::format = schema.kind()` — a value carried
    // out for consumers and asserted by three tests, never branched on
    // (`writer.cpp` states that as its own rule, and it holds: no `==`, switch
    // or table anywhere in `src/` is keyed on `image.format`).
    // Null backend ⇒ `Unknown`, the project's universal invalid sentinel —
    // never a spurious ELF identity, which is exactly what the old
    // `ObjectFormatKind kind = ObjectFormatKind::Elf` default produced.
    [[nodiscard]] ObjectFormatKind kind() const noexcept {
        return d_.backend != nullptr ? d_.backend->kind()
                                     : ObjectFormatKind::Unknown;
    }
    // The resolved backend, or nullptr when the document declared no
    // resolvable format. `src/link/format/`'s walkers compare THIS against
    // their own singleton to police their public entry points — a pointer
    // identity on an opaque handle, not an enumerator comparison.
    [[nodiscard]] link::ObjectFormatBackend const* backend() const noexcept {
        return d_.backend;
    }
    // D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): the output container —
    // `Archive` iff this format's driver output is an `ar` static
    // library (`.a`/`.lib`) bundling relocatable members. The driver
    // dispatches on this (→ `linkAndWriteStaticArchive`), NOT on the
    // artifactProfile. `Single` (the default) for every image / lone
    // relocatable-object format.
    [[nodiscard]] ObjectFormatContainer container() const noexcept { return d_.container; }
    // True iff `container() == Archive` — a static-library format.
    // Convenience predicate paralleling `isImageFlavor()`; the two are
    // mutually exclusive (validate() rejects `archive` on image flavors).
    [[nodiscard]] bool isStaticArchive() const noexcept {
        return d_.container == ObjectFormatContainer::Archive;
    }
    // FC3 c1: the format's declared data model (LP64 / LLP64 / ILP32).
    // Always a valid member for a loader-produced schema (the field is
    // REQUIRED + closed-enum at load).
    [[nodiscard]] DataModel            dataModel() const noexcept { return d_.dataModel; }
    // D-PP-HEADER-CASE-INSENSITIVE-PE: how an `#include` header NAME is matched
    // against the filesystem for THIS format ("case-sensitive" / "case-
    // insensitive"). Always a valid member for a loader-produced schema (the
    // field is REQUIRED + closed-enum at load). Threaded by the driver into the
    // CU build key, and from there to every include resolver, so `#include` and
    // `__has_include` answer the TARGET's convention on any build host.
    [[nodiscard]] HeaderNameMatching   headerNameMatching() const noexcept {
        return d_.headerNameMatching;
    }
    // D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN: how THIS format decorates a
    // canonical C identifier to obtain the linker-visible name (`none` /
    // `leading-underscore`). Always a REAL scheme for a loader-produced
    // schema — the field is REQUIRED + closed-enum at load, and
    // `validate()` rejects the `Unspecified` sentinel on the in-memory
    // path too, so no caller has to defend against it.
    [[nodiscard]] CSymbolDecoration const& cSymbolDecoration() const noexcept {
        return d_.cSymbolDecoration;
    }
    // D-CSUBSET-BITFIELD-ABI-EXACT: the format's declared bit-field strategy, or
    // `None` if it declared none (the caller falls back to the target's value via
    // `effectiveBitFieldStrategy`). Read by the driver when resolving the
    // `AggregateLayoutParams` it threads into the layout engine.
    [[nodiscard]] BitFieldStrategy     bitFieldStrategy() const noexcept {
        return d_.bitFieldStrategy;
    }
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the format's declared `long double`
    // axis, or `None` if it declared none (the semantic bind then leaves any
    // `long double` row unrealized — S_LongDoubleFormatUndeclared on use).
    // Read by the driver via `effectiveLongDoubleFormat(target, format)`.
    [[nodiscard]] LongDoubleFormat     longDoubleFormat() const noexcept {
        return d_.longDoubleFormat;
    }

    // ── Format-owned predefined macros (TF-C97) ───────────────────
    // The format's `predefinedMacros` rows, in declaration order and
    // UNFILTERED — the per-entry `availableObjectFormats` filter is
    // applied ONCE downstream in `mergePredefinedMacros`, alongside
    // the language's and the target's, so no seed site can drift.
    // Symmetric with `TargetSchema::predefinedMacros()`. EMPTY for a
    // format that declares none (every LLP64/ILP32 format today) ⇒
    // the effective list is the language+target list unchanged.
    [[nodiscard]] std::span<PredefinedMacroDef const>
    predefinedMacros() const noexcept { return d_.predefinedMacros; }

    // Relocation accessors — symmetric with TargetSchema's. The
    // linker calls `relocationByKind(kind)` to find which
    // platform-native NAME to write into the output bytes for a
    // given assembler-side opaque tag.
    [[nodiscard]] std::span<ObjectFormatRelocationInfo const>
    relocations() const noexcept { return d_.relocations; }

    [[nodiscard]] std::size_t
    relocationCount() const noexcept { return d_.relocations.size(); }

    [[nodiscard]] ObjectFormatRelocationInfo const*
    relocationByKind(RelocationKind kind) const noexcept {
        auto it = d_.relocationKindIndex.find(kind);
        if (it == d_.relocationKindIndex.end()) return nullptr;
        return &d_.relocations[it->second];
    }

    [[nodiscard]] ObjectFormatRelocationInfo const*
    relocationByName(std::string_view name) const noexcept {
        auto it = d_.relocationNameIndex.find(name);
        if (it == d_.relocationNameIndex.end()) return nullptr;
        return &d_.relocations[it->second];
    }

    // The DECODE-side view of `relocations()` — see `RelocationDecodeTable`.
    // Every object reader builds its reverse map through this and nothing
    // else, so the `emitOnly` exclusion and the call-signal union have ONE
    // definition to read and to change.
    //
    // Returns the ambiguity as a MESSAGE rather than reporting it: the schema
    // has no reporter, and each reader prefixes its own `<format>::read…`
    // context so the diagnostic still names who was decoding. Unreachable for
    // any document that passed `validate()` (which turns this into a LOAD-time
    // invariant); it stays as the belt-and-suspenders catch for schema data
    // constructed directly, which is how unit tests reach this code.
    [[nodiscard]] std::expected<RelocationDecodeTable, std::string>
    relocationDecodeTable() const;

    // Section accessors — the walker calls `sectionByKind(...)` to
    // resolve format-native name + sh_type / sh_flags / addralign
    // for an emitted section.
    [[nodiscard]] std::span<ObjectFormatSectionInfo const>
    sections() const noexcept { return d_.sections; }

    [[nodiscard]] std::size_t
    sectionCount() const noexcept { return d_.sections.size(); }

    [[nodiscard]] ObjectFormatSectionInfo const*
    sectionByKind(SectionKind kind) const noexcept {
        auto it = d_.sectionKindIndex.find(kind);
        if (it == d_.sectionKindIndex.end()) return nullptr;
        return &d_.sections[it->second];
    }

    // Per-format identity accessors — each is populated only when
    // `kind()` matches that arm. Walker reads the relevant block
    // when it runs.
    [[nodiscard]] ElfIdentity      const& elf()              const noexcept { return d_.elf; }
    [[nodiscard]] PeIdentity       const& pe()               const noexcept { return d_.pe; }
    [[nodiscard]] PeOptionalHeader const& peOptionalHeader() const noexcept { return d_.peOptionalHeader; }
    [[nodiscard]] MachOIdentity    const& macho()            const noexcept { return d_.macho; }
    [[nodiscard]] MachOImage       const& machoImage()       const noexcept { return d_.machoImage; }

    // Cross-format image-flavor predicate. True iff the schema
    // describes an executable / shared-library image (ELF ET_EXEC
    // or ET_DYN, PE Exec/Dll, Mach-O MH_EXECUTE/MH_DYLIB) — i.e. the
    // walker emits a LOAD-TIME-BOUND image (PT_LOAD /
    // IMAGE_OPTIONAL_HEADER / LC_MAIN+LC_LOAD_DYLINKER) rather than
    // relocatable section bytes a LATER linker binds. c150 (D-LK1-4):
    // ET_DYN now counts — a `.so` IS load-time-bound (ld.so maps +
    // relocates it; no later static linker sees its innards), closing
    // the c143-era latent imprecision this predicate carried while no
    // dyn format shipped. c153 (D-LK3-3): Mach-O MH_DYLIB likewise —
    // dyld maps + rebases + binds a dylib at dlopen/load time exactly
    // as it does the main image; the arm is `!= Object` (mirroring
    // ELF `!= Rel` / PE `!= Obj`) so all three formats key the
    // predicate the same way. "Image" does NOT imply "rejects
    // undefined externs" — that policy is the SEPARATE
    // `allowsUndefinedImports()` below (a .so is an image AND may
    // carry undefined symbols).
    // `isExecFlavor()` (below) remains EXEC-only by design (it gates
    // entry/processExit machinery a `.so` / `.dylib` must not
    // declare) — the two predicates diverged at c150 and no longer
    // mirror each other.
    // The 3-arm `switch (d_.kind)` this used to be is now the backends'
    // own `isImageFlavor()`. Null backend ⇒ false (fail-closed).
    [[nodiscard]] bool isImageFlavor() const noexcept {
        return d_.backend != nullptr && d_.backend->isImageFlavor(d_);
    }

    // Cross-format EXEC-flavor predicate. True iff the schema
    // describes a PROGRAM the OS starts — the four arms are ELF
    // ET_EXEC, ELF ET_DYN carrying the full PIE entry cluster, PE
    // PE32+ Exec, and Mach-O MH_EXECUTE. FALSE for every entry-less
    // image (ELF `.so`, PE Dll, Mach-O MH_DYLIB) and for every
    // relocatable. Strictly narrower than `isImageFlavor()` above.
    //
    // Promoted from a `validate()`-local at the D-LK10-ENTRY
    // entry-gate fold so `link/linker.cpp` can ask "does this format
    // need an entry trampoline?" WITHOUT switching on the format
    // kind itself. The closed-verb switch lives INSIDE the schema
    // (the `isImageFlavor()` precedent directly above); the
    // agnosticism bar forbids format-identity branching in the
    // linker, not a closed predicate the schema answers.
    // Single-sourced: forwards to `detail::ObjectFormatData::
    // isExecFlavor()`, the SAME member `validate()` computes its
    // entry-machinery rules from.
    //
    // ★ FOOTGUN FOR FUTURE CALLERS — the ET_DYN arm is NOT
    // independent of `processExit()`. `elfDynPieShape` includes
    // `processExit.has_value()` as one of its four cluster members,
    // so on an ELF ET_DYN schema `isExecFlavor()` IMPLIES
    // `processExit().has_value()` by construction. Consequences:
    //   * a rule of the form "exec-flavored ⇒ must declare
    //     processExit" is a TAUTOLOGY on the dyn arm and enforces
    //     nothing there. ET_DYN's real enforcement is PER TIER, and
    //     saying only the first half of this was the over-claim the
    //     TF-C120 audit caught: AT CONFIG LOAD it is the all-or-none
    //     `clusterCount` rule in object_format_schema.cpp (the `/elf`
    //     PARTIAL-cluster failure) — a `validate()` rule, therefore
    //     ABSENT on the in-memory
    //     `ObjectFormatSchema{ObjectFormatData}` path; IN MEMORY it is
    //     the ELF walker's own half-cluster belt in
    //     `elf::encodeElfExecDynamic` (`isPie != !interpreter.empty()`),
    //     which refuses loud before any entry is resolved (MEASURED by
    //     `EntryGateFold.ElfWalkerRefusesHandBuiltEtDynPartial...` in
    //     tests/link/test_lk10_entry_slice_c.cpp);
    //   * consequently a 3-of-4 ET_DYN missing only `processExit`
    //     is invisible to BOTH this predicate and
    //     `processExit().has_value()` — do not reach for either one
    //     to police that shape;
    //   * do NOT use this predicate to decide anything about a
    //     format that has just had `processExit` edited — the
    //     predicate's own value can move underneath you.
    // The other three arms derive purely from `objectType` /
    // `filetype` and carry no such dependency. (INFERRED from the
    // cluster definition; the tautology is exercised by no test on
    // purpose — a test claiming to cover the dyn arm would be
    // asserting a truth of the predicate's definition, not of the
    // rule.)
    [[nodiscard]] bool isExecFlavor() const noexcept {
        return d_.isExecFlavor();
    }

    // Undefined-extern policy (c150 + c151, D-LK1-4 — the c143
    // gate's third flavor): may the emitted artifact carry a
    // REFERENCED extern that no library binds (an undefined symbol
    // resolved by a LATER binder)?
    //   * relocatable (.o/.obj)  → TRUE — the final (foreign) linker
    //     resolves SHN_UNDEF symbols against sibling objects /
    //     libraries (D-LK-OBJECT-NOLIB-EXTERN-RELOCATABLE, c143);
    //   * ELF ET_DYN (.so)       → TRUE — standard `ld -shared`
    //     semantics: a shared library may reference symbols the
    //     EXECUTABLE (or another loaded object) defines; ld.so
    //     resolves them from the global scope at load;
    //   * executable images      → FALSE — nothing later binds them;
    //     an unresolved reference is a load-time failure, so the
    //     linker rejects LOUD at build time (c143, unchanged).
    // c151: an ELF ET_DYN PIE is an EXECUTABLE — FALSE, exactly like
    // ET_EXEC (ld.so erroring "symbol lookup error" at ./prog time
    // is the deferred-failure class the c143 gate exists to
    // prevent). The PIE discriminator is ENTRY-CLUSTER presence on
    // the dyn schema (validate() pins the cluster all-or-none, so
    // `processExit` presence is a faithful single-member witness) —
    // never a format-name check.
    // Schema-driven (declared objectType + declared entry cluster),
    // never a format-name branch. PE Dll (c152, D-LK2-4) is FALSE —
    // Windows has no ld.so-style deferred global scope for
    // implicitly-linked DLLs: every import binds at load from a NAMED
    // module's export table, so a referenced no-library extern still
    // rejects loud at build time. c153 (D-LK3-3): Mach-O MH_DYLIB is
    // FALSE for the same structural reason — the shipped bind model
    // is the modern TWO-LEVEL namespace (MH_TWOLEVEL; every
    // LC_DYLD_INFO bind opcode names a SPECIFIC dylib ordinal, and
    // the walker fails loud on an import whose library is not in
    // image.loadDylibs), so a library-less undefined has no bind row
    // to ride: unlike ELF's flat global scope there is nothing that
    // resolves it at load. macOS DOES have a flat-namespace opt-in
    // (MH_FLAT + BIND_SPECIAL_DYLIB_FLAT_LOOKUP ordinal -2, or
    // `-undefined dynamic_lookup`), but that is a DIFFERENT bind
    // model the eager two-level machinery deliberately does not
    // ship; a flat-namespace schema knob would flip this arm then.
    //
    // ★ AFTER THE D-LK10-ENTRY ENTRY-GATE FOLD, read the "faithful
    // single-member witness" sentence above precisely: it is the
    // ALL-OR-NONE `clusterCount` rule (object_format_schema.cpp, the
    // `/elf` PARTIAL-cluster failure) that makes `processExit`
    // presence stand for the whole PIE cluster — NOT the new
    // "exec-flavored ⇒ must declare processExit" rule, which is
    // VACUOUS on the ET_DYN arm (`isExecFlavor()`'s dyn arm already
    // contains `processExit.has_value()`; see the footgun note on
    // that accessor). The line below is therefore left keyed on
    // `d_.processExit` rather than rewritten as `!isExecFlavor()`:
    // the two agree on every LOADED schema and diverge only on a
    // hand-built validate-bypassing one (dyn + processExit but no
    // interpreter), where the cluster-member spelling is the one
    // that stays honest about what it read.
    //
    // ★ NULL-BACKEND DIRECTION, STATED BECAUSE IT IS THE ONE PLACE WHERE
    // "fail closed" IS NOT THE SAME AS "return false EVERYWHERE". Every other
    // predicate here answers `false` on a null backend and that IS the strict
    // answer. Here `true` is the PERMISSIVE answer (it lets an unresolved
    // extern through to a later binder), and `!isImageFlavor()` is `true` on a
    // null backend — so the plain relocatable short-circuit would have made an
    // unresolvable schema the most permissive one in the tree. The explicit
    // null test below comes FIRST for exactly that reason.
    [[nodiscard]] bool allowsUndefinedImports() const noexcept {
        if (d_.backend == nullptr) return false;  // unresolved ⇒ strictest
        if (!isImageFlavor()) return true;   // relocatable: later linker resolves
        return d_.backend->allowsUndefinedImports(d_);
    }

    // Image-side entry-point symbol name. Empty for relocatable
    // artifacts; non-empty for executables. The format walker
    // resolves this against `AssembledModule`'s symbol table at
    // emit time to compute the runtime entry virtual address.
    [[nodiscard]] std::string_view entryPoint() const noexcept {
        return d_.entryPoint;
    }

    // ── D-LK10-ENTRY Slice B accessors ──────────────────────────
    [[nodiscard]] std::optional<ProcessExit> const& processExit() const noexcept {
        return d_.processExit;
    }
    [[nodiscard]] std::string_view entryCallingConvention() const noexcept {
        return d_.entryCallingConvention;
    }

    // ── D-RUNTIME-MAIN-ARGC-ARGV accessor ────────────────────────
    // The format's program-entry argument mechanism (StackVector),
    // or nullopt if the format declared none (the trampoline then
    // leaves the argument registers untouched before the user-entry
    // call — the Mach-O LC_MAIN-correct / pre-c88 PE behavior).
    [[nodiscard]] std::optional<ProcessArgs> const& processArgs() const noexcept {
        return d_.processArgs;
    }

    // ── UCRT-P4 accessors ─────────────────────────────────────────
    // The format's ROLE → IMAGE table. Read by the migration's mechanical exit
    // criterion and by tests; the spine blocks themselves already carry their
    // role's resolved image (the loader resolved it), so no engine consumer has
    // to walk this at emit time.
    [[nodiscard]] RuntimeLibraryTable const& runtimeLibraries() const noexcept {
        return d_.runtimeLibraries;
    }
    // The format's unwinder-personality declaration, or nullopt if it declares
    // none. `synthesizeSehFunclets` fails loud on a resolved SEH region under a
    // nullopt personality — never a silently-assumed handler/image pair.
    [[nodiscard]] std::optional<SehPersonality> const&
    sehPersonality() const noexcept { return d_.sehPersonality; }
    // The program-entry materialization VERBS this format realizes. Non-empty on
    // every exec-flavored format, empty on every other (both directions
    // load-enforced), so EMPTY is the engine's "this build needs no program
    // entry" predicate. Candidate selection intersects this set with the source
    // language's declared entry rows; see the field's docblock for why no
    // signature appears here.
    [[nodiscard]] std::span<EntryMaterialization const>
    entryVerbs() const noexcept {
        return d_.entryVerbs;
    }
    // Convenience for the one question every consumer actually asks of the set.
    // A membership helper rather than open-coded `std::find` at each call site:
    // the set is DATA, and the engine must never grow a second way to ask.
    [[nodiscard]] bool realizesEntryVerb(EntryMaterialization v) const noexcept {
        for (auto const d : d_.entryVerbs)
            if (d == v) return true;
        return false;
    }

    // ── D-FFI-EXTERN-CALL-DISPATCH accessor ──────────────────────
    // The format's extern-call shape (`indirect-slot` / `direct-plt`),
    // or nullopt if the format declared none. MIR→LIR `lowerCall`
    // reads this to choose `call_indirect_via_extern` vs plain `call`;
    // a nullopt under a module with extern imports is a fail-loud.
    [[nodiscard]] std::optional<ExternCallDispatch>
    externCallDispatch() const noexcept {
        return d_.externCallDispatch;
    }

    // ── D-LK-EXTERN-DATA-IMPORT accessor ─────────────────────────
    // The format's extern-DATA import binding model
    // (`got-indirect`), or nullopt if the format declared none.
    // The linker's pre-walker gate fails loud on a surviving data
    // import under nullopt; the format walker implements the
    // declared mechanism.
    [[nodiscard]] std::optional<DataImportBinding>
    dataImportBinding() const noexcept {
        return d_.dataImportBinding;
    }

    // ── D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT accessor (TF-C52) ────
    // The format's extern-ADDRESS materialization binding (`got`), or
    // nullopt if the format declared none. MIR→LIR reads this to route
    // an `&extern` VALUE through the arm64 GOT-address macro (adrp:got:
    // + ldr:got_lo12:) instead of an absolute page-pair lea; nullopt =
    // the ordinary lea (foreign-PIE-safe only on x86_64 / a DSS exec).
    [[nodiscard]] std::optional<ExternAddrBinding>
    externAddrBinding() const noexcept {
        return d_.externAddrBinding;
    }

    // ── D-CSUBSET-THREAD-LOCAL accessor (TLS C1) ─────────────────
    // The format's thread-local access block (model + x86 segment
    // prefix + tp-slot displacement), or nullopt if the format
    // declared none. MIR→LIR reads this to lower a thread-local
    // GlobalAddr; a nullopt under a module touching a thread-local
    // is a fail-loud (K_FormatLacksThreadLocalSupport) — never a
    // silent process-shared alias.
    [[nodiscard]] std::optional<TlsAccessInfo>
    tlsAccess() const noexcept {
        return d_.tlsAccess;
    }

    // D-CSUBSET-C11-THREADS-MACHO: the shipped-library synth vehicle this
    // format declared (`win32` / `pthread`), or `std::nullopt` if none.
    // `synthesizeThreadsShim` reads it to emit the right primitive family
    // + import library; a nullopt under a module carrying synthesize-tagged
    // threads symbols is a fail-loud, never a silently-assumed vehicle.
    [[nodiscard]] std::optional<LibrarySynthesis> const&
    librarySynthesis() const noexcept {
        return d_.librarySynthesis;
    }

    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the format's stack-reserve
    // capability (vehicle + declared legal range), or `std::nullopt` if this
    // format cannot express a per-program stack reserve at all. THE
    // capability query the linker gate asks — presence, never a format
    // identity — so PE-exec (declares) and PE-dll (does not) diverge from
    // config alone. A request against a nullopt format is a fail-loud.
    [[nodiscard]] std::optional<StackReserveControl>
    stackReserveControl() const noexcept {
        return d_.stackReserveControl;
    }

    // D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: WHY this format cannot carry a
    // stack reserve, as a closed verb the refusal diagnostic turns into an
    // actionable remedy (`stackReserveUnsupportedRemedy`). `std::nullopt`
    // ⇒ the format declared no reason; the refusal still fires, with the
    // generic message. Never consulted when `stackReserveControl()` has a
    // value — the two are mutually exclusive by load-time validation.
    [[nodiscard]] std::optional<StackReserveUnsupportedReason>
    stackReserveUnsupportedReason() const noexcept {
        return d_.stackReserveUnsupportedReason;
    }

    // ── D-LK2-RODATA producer-data-section capability gate ─────
    //
    // `acceptsDataSection(k)` is true iff this format's walker is
    // wired to emit producer `AssembledData` items of kind `k`.
    // The linker consults this BEFORE invoking the walker, so a
    // format whose walker arm has not landed (or has landed for a
    // strict subset of kinds) fails loud rather than silently
    // dropping bytes. Schema-declared, not enumerated in C++.
    [[nodiscard]] bool acceptsDataSection(DataSectionKind k) const noexcept {
        for (auto const& s : d_.supportedDataSections) {
            if (s == k) return true;
        }
        return false;
    }
    [[nodiscard]] std::span<DataSectionKind const>
    supportedDataSections() const noexcept {
        return d_.supportedDataSections;
    }

    // The artifact profiles this format SERVES (plan 06 AP3). Empty ⇒
    // serves none (fail-closed). The driver checks the project's profile
    // against this set via the shared `artifactProfileSupported` predicate.
    [[nodiscard]] std::span<std::string const>
    artifactProfiles() const noexcept {
        return d_.artifactProfiles;
    }

    // ── Loaders ───────────────────────────────────────────────
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadFromFile(std::filesystem::path const& path);
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadShipped(std::string_view name);
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadFromText(std::string_view jsonText,
                 std::string_view sourceLabel = "<inline>");

private:
    // Lowercase 64-hex SHA-256 of the document bytes — see `contentDigest()`.
    // Written ONLY by `loadFromText` (a static member, so no friend is
    // needed); every other construction path leaves it empty on purpose.
    std::string contentDigest_;

    detail::ObjectFormatData d_;
};

} // namespace dss
