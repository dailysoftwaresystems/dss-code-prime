#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/grammar_schema.hpp"      // ConfigDiagnostic + LoadResult
#include "core/types/object_format_kind.hpp"  // ObjectFormatKind + kObjectFormatKindTable
#include "core/types/section_kind.hpp"        // SectionKind + kSectionKindTable
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"        // SymbolBinding / SymbolVisibility (lifted to core/types for MIR-tier producers)
#include "core/types/target_schema.hpp"       // EnumNameTable<E,N>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
    // ET_DYN anchored at D-LK1-4 paired with LK6 dynamic linking.
    // `validate()` rejects values outside {Rel, Exec}; ET_DYN
    // accepted at load but rejected at walker dispatch until
    // D-LK1-4 closes. Default = Rel preserves LK1 cycle 1
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
    // header pointing at it.
    std::string    interpreter;
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
// executable images (`.exe` for Exec / `.dll` for Dll, LK2 cycle
// 2). Mirrors the `ElfObjectType` closed-enum pattern. Dll is
// declared but rejected by validate() until a future cycle ships
// the .dll arm (anchored at plan 14 §3.1 — same shape as ELF
// ET_DYN's D-LK1-4 anchor).
enum class PeObjectType : std::uint16_t {
    Obj  = 1,  // .obj relocatable — LK2 cycle 1 (default; preserves
               //                    LK2 cycle 1 schemas unchanged).
    Exec = 2,  // .exe executable — LK2 cycle 2 (closes D-LK2-1).
    Dll  = 3,  // .dll dynamic library — anchored, not yet implemented.
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

// ── PE32+ Optional Header (loaded only when PE objectType==Exec) ──
//
// Mirrors `IMAGE_OPTIONAL_HEADER64` (PE/COFF spec §3.4). Only the
// load-bearing fields are declared — every field the Windows loader
// requires for a minimal `.exe` is here; the deluxe data
// directories (debug, security, etc.) are emitted as zero by the
// walker (NumberOfRvaAndSizes=16, all entries zero — minimum
// loadable image).
//
// Validate() requires all fields populated when objectType==Exec;
// Obj rejects any non-zero field (config-error trap mirroring the
// ELF ET_REL `virtualAddress=0` symmetry).
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
// <mach-o/loader.h>'s MH_OBJECT / MH_EXECUTE / MH_DYLIB. The walker
// supports the first two; Dylib is declared but rejected by
// validate() until a future cycle ships the .dylib arm (anchored
// at plan 14 §3.1 D-LK3-3).
enum class MachOObjectType : std::uint32_t {
    Object  = 1,   // MH_OBJECT  — relocatable .o
    Execute = 2,   // MH_EXECUTE — executable image
    Dylib   = 6,   // MH_DYLIB   — dynamic library (anchored D-LK3-3)
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
                                     // (closes D-LK3-2). MH_DYLIB
                                     // anchored at D-LK3-3.
    std::uint32_t flags = 0;         // MH_SUBSECTIONS_VIA_SYMBOLS=0x2000
                                     // / MH_PIE=0x200000 (mandatory for
                                     //   modern macOS exec). Optional;
                                     //   0 is legal for a minimal .o.
};

// ── Mach-O image block (loaded only when filetype==MH_EXECUTE) ──
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

struct DSS_EXPORT MachOImage {
    std::uint64_t pageZeroSize = 0;        // __PAGEZERO vmsize
    std::string   dylinkerPath;            // LC_LOAD_DYLINKER name
    std::vector<MachODylibRef> loadDylibs; // each → LC_LOAD_DYLIB
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
    // shipped formats opt in explicitly (currently the new
    // `macho64-arm64-darwin-exec.format.json` enables it).
    bool          useChainedFixups = false;
};

namespace detail {

struct DSS_EXPORT ObjectFormatData {
    ObjectFormatSchemaId id{};
    std::string          name;         // "elf64-x86_64-linux" etc.
    std::string          version;
    ObjectFormatKind     kind = ObjectFormatKind::Elf;

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
                                           //   macho.filetype == MH_EXECUTE

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

    // Cross-field invariants:
    //   * relocations: kind != 0, kind unique cross-row, name unique
    //     + non-empty, nativeId != 0 when relocations are non-empty.
    //   * sections: kind unique cross-row, name non-empty.
    //   * ELF identity: when kind == Elf, fileClass / dataEncoding /
    //     machine must all be != 0.
    [[nodiscard]] std::vector<ConfigDiagnostic> validate() const;
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

    [[nodiscard]] ObjectFormatSchemaId id()      const noexcept { return d_.id; }
    [[nodiscard]] std::string_view     name()    const noexcept { return d_.name; }
    [[nodiscard]] std::string_view     version() const noexcept { return d_.version; }
    [[nodiscard]] ObjectFormatKind     kind()    const noexcept { return d_.kind; }

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
    // describes an executable / shared-library image (ELF ET_EXEC,
    // PE Exec/Dll, Mach-O MH_EXECUTE) — i.e. the walker emits an
    // image header (PT_LOAD / IMAGE_OPTIONAL_HEADER / LC_MAIN+
    // LC_LOAD_DYLINKER) rather than relocatable section bytes.
    // Mirrors the `isExecFlavor` rule inside `validate()` (the
    // terminal cross-format Text-virtualAddress gate); exposing it
    // as an accessor lets walker code branch on "am I image-side?"
    // without duplicating the disjunction (type-design O1 fold-in,
    // LK2 cycle 2 + LK3 cycle 2 post-audit).
    [[nodiscard]] bool isImageFlavor() const noexcept {
        switch (d_.kind) {
            case ObjectFormatKind::Elf:
                return d_.elf.objectType == ElfObjectType::Exec;
            case ObjectFormatKind::Pe:
                return d_.pe.objectType != PeObjectType::Obj;
            case ObjectFormatKind::MachO:
                return d_.macho.filetype == MachOObjectType::Execute;
            default:
                return false;
        }
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

    // ── D-FFI-EXTERN-CALL-DISPATCH accessor ──────────────────────
    // The format's extern-call shape (`indirect-slot` / `direct-plt`),
    // or nullopt if the format declared none. MIR→LIR `lowerCall`
    // reads this to choose `call_indirect_via_extern` vs plain `call`;
    // a nullopt under a module with extern imports is a fail-loud.
    [[nodiscard]] std::optional<ExternCallDispatch>
    externCallDispatch() const noexcept {
        return d_.externCallDispatch;
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

    // ── Loaders ───────────────────────────────────────────────
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadFromFile(std::filesystem::path const& path);
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadShipped(std::string_view name);
    static LoadResult<std::shared_ptr<ObjectFormatSchema>>
    loadFromText(std::string_view jsonText,
                 std::string_view sourceLabel = "<inline>");

private:
    detail::ObjectFormatData d_;
};

} // namespace dss
