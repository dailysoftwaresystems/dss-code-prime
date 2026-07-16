#include "link/format/elf.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift assert
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"  // isExternallyVisible (ET_DYN exports)
#include "link/format/byte_emit.hpp"
#include "link/format/exec_data_section.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/format/interior_block_symbol_va.hpp"
#include "link/format/object_symbol_names.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ELF64 relocatable (.o) writer — plan 14 LK1 cycle 1.
//
// Byte layout (gABI Ch. 4 + AMD64 psABI §4.4):
//   [0x00]      Elf64_Ehdr (64 B)
//   [0x40]      .text body                  (align 16, already at 64)
//   [...]       pad to 8
//   [...]       .rela.text                  (n_relocs × 24)
//   [...]       pad to 8
//   [...]       .symtab                     (n_syms × 24)
//   [...]       .strtab
//   [...]       .shstrtab
//   [e_shoff]   Section Header Table        (n_sections × 64)
//
// The walker is target-blind in shape — every ELF-specific number
// (machine, class, data encoding, section flags, reloc nativeIds,
// section names) is read from the format schema. The only
// hardcoded structural knowledge is the ELF64 binary record layout.

namespace dss::elf {

namespace {

using dss::report;
using link::format::detail::appendU8;
using link::format::detail::appendU16LE;
using link::format::detail::appendU32LE;
using link::format::detail::appendU64LE;
using link::format::detail::appendI64LE;
using link::format::detail::emit;
using link::format::detail::requireSection;

// ── Elf64 type constants (gABI Ch. 4 + <elf.h>) ─────────────────

constexpr std::uint16_t ET_REL = 1;
constexpr std::uint32_t EV_CURRENT = 1;

// Elf64_Sym st_info encoding (gABI 4.18).
constexpr std::uint8_t STB_LOCAL  = 0;
constexpr std::uint8_t STB_GLOBAL = 1;
constexpr std::uint8_t STB_WEAK   = 2;  // ET_DYN weak exports (c150)
constexpr std::uint8_t STT_NOTYPE = 0;
constexpr std::uint8_t STT_OBJECT = 1;  // data object (copy-reloc dynsym)
constexpr std::uint8_t STT_FUNC   = 2;
constexpr std::uint8_t STT_SECTION = 3;
constexpr std::uint16_t SHN_UNDEF = 0;

// Elf64 sh_type / sh_flags (gABI 4.7-4.8) — used by the dynamic
// walker (cycle 2b.2). Named to match `<elf.h>`; type-design #2
// convergence (LK6 cycle 2b.2 review).
constexpr std::uint32_t SHT_PROGBITS = 1;
constexpr std::uint32_t SHT_SYMTAB   = 2;
constexpr std::uint32_t SHT_STRTAB   = 3;
constexpr std::uint32_t SHT_RELA     = 4;
constexpr std::uint32_t SHT_HASH     = 5;
constexpr std::uint32_t SHT_DYNAMIC  = 6;
constexpr std::uint32_t SHT_DYNSYM   = 11;
constexpr std::uint64_t SHF_WRITE     = 1;
constexpr std::uint64_t SHF_ALLOC     = 2;
constexpr std::uint64_t SHF_EXECINSTR = 4;

// Elf64 p_type / p_flags (gABI Fig. 5-2).
constexpr std::uint32_t PT_LOAD    = 1;
constexpr std::uint32_t PT_DYNAMIC = 2;
constexpr std::uint32_t PT_INTERP  = 3;
constexpr std::uint32_t PT_PHDR    = 6;
// D-CSUBSET-THREAD-LOCAL (TLS C1): the thread-local template segment.
// glibc's ld.so records it as the main module's static-TLS block and
// copies p_filesz bytes + zeroes (p_memsz − p_filesz) more for EVERY
// thread (the main thread included — the CSU runs before main on
// DSS's always-dynamic ELF arm, so no DSS-side arch_prctl is needed).
constexpr std::uint32_t PT_TLS     = 7;
constexpr std::uint32_t PF_X = 1;
constexpr std::uint32_t PF_W = 2;
constexpr std::uint32_t PF_R = 4;

// Map ELF section flags (sh_flags) to program-header permission flags
// (p_flags). gABI: SHF_ALLOC→PF_R (occupies memory ⇒ readable),
// SHF_WRITE→PF_W, SHF_EXECINSTR→PF_X. A PT_LOAD covering several
// sections takes the OR of its members' mapped flags. This replaces
// the previously-hardcoded `p_flags = 5` for the single .text PT_LOAD
// with a derivation from the actual sections' sh_flags (D-LK1-ELF-
// EXEC-DATA-SECTIONS, plan-lock MF-5): .text(6→R+X=5) | .rodata(2→R=4)
// = 5, so the static-exec byte output is unchanged when no rodata is
// present, and stays R+X (5) once SHF_ALLOC-only rodata folds in.
[[nodiscard]] constexpr std::uint32_t
shFlagsToPFlags(std::uint64_t shFlags) noexcept {
    std::uint32_t p = 0;
    if (shFlags & SHF_ALLOC)     p |= PF_R;
    if (shFlags & SHF_WRITE)     p |= PF_W;
    if (shFlags & SHF_EXECINSTR) p |= PF_X;
    return p;
}

// Elf64 d_tag (gABI 5.10).
constexpr std::uint64_t DT_NULL    = 0;
constexpr std::uint64_t DT_NEEDED  = 1;
constexpr std::uint64_t DT_HASH    = 4;
constexpr std::uint64_t DT_STRTAB  = 5;
constexpr std::uint64_t DT_SYMTAB  = 6;
constexpr std::uint64_t DT_RELA    = 7;
constexpr std::uint64_t DT_RELASZ  = 8;
constexpr std::uint64_t DT_RELAENT = 9;
constexpr std::uint64_t DT_STRSZ   = 10;
constexpr std::uint64_t DT_SYMENT  = 11;
// DT_SONAME (gABI 5.10) — the shared library's logical name; the
// d_val is a `.dynstr` offset. Emitted on the ET_DYN arm ONLY when
// the schema declares `elf.soname` (c150, D-LK1-4).
constexpr std::uint64_t DT_SONAME  = 14;
constexpr std::uint64_t DT_FLAGS_1 = 0x6ffffffb;
constexpr std::uint64_t DF_1_NOW   = 1;
// DF_1_PIE (glibc elf.h / binutils) — marks an ET_DYN as a
// position-independent EXECUTABLE, not a shared library. Emitted on
// the PIE sub-mode only (c151, D-LK1-4 PIE half); ground truth: gcc
// 13.3 default-PIE output carries DT_FLAGS_1 = 0x8000001 (`readelf
// -d` shows "Flags: NOW PIE" on tag 0x6ffffffb). Modern kernels/
// tools (execveat protections, readelf's "DYN (Position-Independent
// Executable file)" label) read this bit to distinguish a PIE from
// a `.so`.
constexpr std::uint64_t DF_1_PIE   = 0x08000000;

// Per-machine ELF reloc type for "write resolved symbol VA into GOT
// slot at load time" (dyld semantics).
// x86_64 psABI §4.4.1 — R_X86_64_GLOB_DAT = 6.
// AArch64 ELF psABI §4.6.3 — R_AARCH64_GLOB_DAT = 1025 (0x401).
constexpr std::uint32_t R_X86_64_GLOB_DAT   = 6;
constexpr std::uint32_t R_AARCH64_GLOB_DAT  = 1025;

// Per-machine ELF reloc type for "memcpy the shared library's DATA
// object into the executable's local `.bss` copy at load time" —
// the ET_EXEC extern-data import binding (D-LK-EXTERN-DATA-IMPORT,
// `dataImportBinding: "copy-relocation"` on the format schema).
// x86_64 psABI §4.4.1 — R_X86_64_COPY = 5.
// AArch64 ELF psABI §4.6.3 — R_AARCH64_COPY = 1024 (0x400).
// The loader resolves the symbol NAME in the needed libraries
// (skipping the executable's own definition — ELF_RTYPE_CLASS_COPY
// lookup semantics) and copies min(st_size) bytes to r_offset; every
// other image binds the SAME name to the executable's DEFINED OBJECT
// symbol (interposition), so all references converge on the copy.
constexpr std::uint32_t R_X86_64_COPY   = 5;
constexpr std::uint32_t R_AARCH64_COPY  = 1024;

// Closed-enum machine codes the dynamic walker dispatches on.
// EM_X86_64 = 62 (gABI fig 4-2); EM_AARCH64 = 183 (AArch64 ELF psABI).
// Adding a 3rd ISA (RISC-V = 243, PPC64 = 21, MIPS = 8) requires:
//   * new `R_*_GLOB_DAT` constant
//   * new arm in `pltStubSizeFor` / `globDatTypeFor` / `emitPltStub`
//   * relaxed dispatch guard in `elf::encode`
// All three are localized to this file today; the architect-anchored
// TU split (D-LK6-8 §post-fold #1) becomes warranted when the 3rd
// machine arrives.
constexpr std::uint16_t kEmX86_64  = 62u;
constexpr std::uint16_t kEmAArch64 = 183u;

// Per-machine PLT stub size in bytes.
[[nodiscard]] constexpr std::uint64_t
pltStubSizeFor(std::uint16_t machine) noexcept {
    switch (machine) {
        case kEmX86_64:  return 6u;   // 6-byte `FF 25 disp32`
        case kEmAArch64: return 16u;  // 4×4-byte ADRP+LDR+BR+NOP
    }
    return 0u;  // caller's machine-guard already rejected unknowns
}

// Per-machine GOT-slot relocation type.
[[nodiscard]] constexpr std::uint32_t
globDatTypeFor(std::uint16_t machine) noexcept {
    switch (machine) {
        case kEmX86_64:  return R_X86_64_GLOB_DAT;
        case kEmAArch64: return R_AARCH64_GLOB_DAT;
    }
    return 0u;
}

// Per-machine copy-relocation type (extern DATA imports — c84,
// D-LK-EXTERN-DATA-IMPORT). Same closed-enum dispatch shape as
// `globDatTypeFor`; a 3rd ISA adds its `R_*_COPY` constant + an arm
// here (see the `kEmX86_64` comment block above).
[[nodiscard]] constexpr std::uint32_t
copyRelocTypeFor(std::uint16_t machine) noexcept {
    switch (machine) {
        case kEmX86_64:  return R_X86_64_COPY;
        case kEmAArch64: return R_AARCH64_COPY;
    }
    return 0u;
}

// Per-machine RELATIVE relocation type (c150, D-LK1-4 — the ET_DYN
// base-relative fixup): "write load_base + r_addend into the 64-bit
// slot at r_offset". No symbol lookup — ld.so adds the module's own
// load bias. Every internal absolute pointer slot in a slid image
// (a fn-ptr table entry, a `&global` initializer, a jump-table row)
// carries one of these instead of the exec arm's link-time in-place
// final VA.
// x86_64 psABI §4.4.1 — R_X86_64_RELATIVE = 8.
// AArch64 ELF psABI §4.6.3 — R_AARCH64_RELATIVE = 1027 (0x403).
constexpr std::uint32_t R_X86_64_RELATIVE  = 8;
constexpr std::uint32_t R_AARCH64_RELATIVE = 1027;
[[nodiscard]] constexpr std::uint32_t
relativeRelocTypeFor(std::uint16_t machine) noexcept {
    switch (machine) {
        case kEmX86_64:  return R_X86_64_RELATIVE;
        case kEmAArch64: return R_AARCH64_RELATIVE;
    }
    return 0u;
}

constexpr std::uint8_t makeStInfo(std::uint8_t bind, std::uint8_t type) {
    return static_cast<std::uint8_t>((bind << 4) | (type & 0xF));
}

// Elf64_Rela r_info encoding (gABI 4.13).
constexpr std::uint64_t makeRelaInfo(std::uint32_t symIdx,
                                      std::uint32_t type) {
    return (static_cast<std::uint64_t>(symIdx) << 32)
         | static_cast<std::uint64_t>(type);
}

// Byte-emit helpers + emit() + requireSection() now hoisted to
// `src/link/format/byte_emit.hpp` (substrate shared with PE walker
// and future Mach-O walker — simplifier fold-in #1+#3).

void padTo(std::vector<std::uint8_t>& out, std::uint64_t alignment) {
    if (alignment <= 1) return;
    while (out.size() % alignment != 0) out.push_back(0);
}

// String-table builder hoisted to `src/link/format/string_table.hpp`
// (D-LK4-9 closure, 3rd-consumer trigger from Mach-O). ELF uses
// the NulByte init: byte 0 is the empty-name sentinel.
using link::format::detail::StringTable;

// ── Section header record (in-memory before serialization) ──────

struct SectionHeader {
    std::uint32_t name_offset = 0;   // offset into .shstrtab
    std::uint32_t type = 0;          // sh_type
    std::uint64_t flags = 0;         // sh_flags
    std::uint64_t addr = 0;          // sh_addr (0 in ET_REL)
    std::uint64_t offset = 0;        // sh_offset (filled after layout)
    std::uint64_t size = 0;          // sh_size
    std::uint32_t link = 0;          // sh_link
    std::uint32_t info = 0;          // sh_info
    std::uint64_t addr_align = 0;    // sh_addralign
    std::uint64_t entry_size = 0;    // sh_entsize
};

// File-local helpers used by the dynamic walker (simplifier #1+#4
// convergence). `padToOffset` extends `bytes` to an absolute file
// position with zero-fill (NOT an alignment round-up — that's the
// existing `padTo(out, alignment)`). `appendBytes` collapses the
// `insert(end, X.begin(), X.end())` boilerplate to a named call.
inline void padToOffset(std::vector<std::uint8_t>& out, std::uint64_t offset) {
    if (out.size() < offset) out.resize(offset, 0);
}
inline void appendBytes(std::vector<std::uint8_t>& out,
                         std::span<std::uint8_t const> body) {
    out.insert(out.end(), body.begin(), body.end());
}

// Emit one PLT stub for ARM64 (16 bytes: ADRP + LDR + BR + NOP) per
// AArch64 ELF psABI §4.5 "PLT formats". The 4 instructions are:
//   ADRP x16, page-of(GOT_slot)
//   LDR  x17, [x16, lo12-of(GOT_slot)]
//   BR   x17
//   NOP
// All 4 are 4-byte instructions; total 16 bytes per stub.
//
// PCS contract (AArch64 Procedure Call Standard, ARM IHI 0055
// "Procedure Call Standard for the Arm 64-bit Architecture",
// §6.1.1 / Table 1 "General-purpose registers"): x16 (IP0) and
// x17 (IP1) are designated INTRA-PROCEDURE-CALL SCRATCH registers
// — call-clobbered, sole-legal-scratch for the PLT dispatch
// between the caller's `BL` and the resolved function's entry.
// Any other GPR used here would corrupt the caller's saved state.
// Using x16+x17 is what makes this 4-instruction sequence
// callee-state-preserving by construction. (Citation corrected
// at post-fold #2 — was previously §5.1.2 which is the Data
// Types section in current AAPCS64, not the register-roles
// section.)
//
// Range check: ADRP carries a 21-bit signed page-relative immediate
// (±4 GiB reachable). The LDR's imm12 is 12-bit unsigned scaled by 8
// (0..32760 byte offset within page, 8-byte aligned). The GOT slot
// VA's low-12 must therefore be 8-byte-aligned — a property the
// caller guarantees by construction (GOT slots are 8 bytes each,
// gotVa is page-aligned).
[[nodiscard]] inline bool emitArm64PltStub(
        std::vector<std::uint8_t>& plt,
        std::size_t                stubOffset,
        std::uint64_t              stubVa,
        std::uint64_t              slotVa,
        DiagnosticReporter&        reporter) {
    // ADRP page-pair value: signed 21-bit, computed identically to
    // the R_AARCH64_ADR_PREL_PG_HI21 reloc formula (D-LK6-1
    // Aarch64AdrPrelPgHi21 — kept consistent intentionally so the
    // ADRP encoding in the kernel and the ADRP in the PLT stub match).
    auto const pageOf = [](std::uint64_t v) noexcept -> std::uint64_t {
        return v & ~std::uint64_t{0xFFF};
    };
    std::int64_t const pageDiff =
        static_cast<std::int64_t>(pageOf(slotVa))
      - static_cast<std::int64_t>(pageOf(stubVa));
    std::int64_t const adrpValue = pageDiff >> 12;
    constexpr std::int64_t kAdrpMax =  (std::int64_t{1} << 20) - 1;
    constexpr std::int64_t kAdrpMin = -(std::int64_t{1} << 20);
    if (adrpValue < kAdrpMin || adrpValue > kAdrpMax) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             "elf::encodeElfExecDynamic (ARM64 PLT): ADRP page-pair "
             "value " + std::to_string(adrpValue) +
             " out of signed 21-bit range — PLT and GOT too far "
             "apart (>±4 GiB).");
        return false;
    }

    // LDR imm12: low 12 bits of slotVa, scaled by 8 for 64-bit access.
    std::uint64_t const lo12 = slotVa & std::uint64_t{0xFFF};
    if ((lo12 & 0x7u) != 0u) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             "elf::encodeElfExecDynamic (ARM64 PLT): GOT slot VA "
             "is not 8-byte aligned (low12=0x" +
             std::to_string(lo12) + "). The AArch64 64-bit LDR "
             "encoding requires an 8-byte-aligned offset; the "
             "linker's GOT layout is broken.");
        return false;
    }

    // ADRP x16 encoding: base 0x90000010 (Rd=x16); immlo at bits[30:29],
    // immhi at bits[23:5] holding 21-bit two's-complement page value.
    std::uint32_t const adrpV = static_cast<std::uint32_t>(adrpValue) & 0x1FFFFFu;
    std::uint32_t const immlo = (adrpV & 0x3u) << 29;
    std::uint32_t const immhi = ((adrpV >> 2) & 0x7FFFFu) << 5;
    std::uint32_t const adrp  = 0x90000010u | immlo | immhi;

    // LDR (immediate, unsigned offset) 64-bit: base 0xF9400000;
    // imm12 at bits[21:10] (scaled by 8), Rn=x16 at bits[9:5],
    // Rt=x17 at bits[4:0]. Pre-shifted base 0xF9400211 carries
    // Rn=16 + Rt=17 already.
    std::uint32_t const imm12 = static_cast<std::uint32_t>(lo12 >> 3);
    std::uint32_t const ldr   = 0xF9400211u | (imm12 << 10);

    // BR x17 (unconditional indirect branch); NOP for stub padding
    // so each PLT entry is 16 bytes (cache-line friendly).
    constexpr std::uint32_t kBrX17 = 0xD61F0220u;
    constexpr std::uint32_t kNop   = 0xD503201Fu;

    auto const writeInst = [&](std::size_t off, std::uint32_t inst) {
        plt[off + 0] = static_cast<std::uint8_t>(inst         & 0xFFu);
        plt[off + 1] = static_cast<std::uint8_t>((inst >>  8) & 0xFFu);
        plt[off + 2] = static_cast<std::uint8_t>((inst >> 16) & 0xFFu);
        plt[off + 3] = static_cast<std::uint8_t>((inst >> 24) & 0xFFu);
    };
    writeInst(stubOffset + 0,  adrp);
    writeInst(stubOffset + 4,  ldr);
    writeInst(stubOffset + 8,  kBrX17);
    writeInst(stubOffset + 12, kNop);
    return true;
}

// Emit one PLT stub for x86_64 (6 bytes: `FF 25 disp32`) per
// x86_64 psABI. `jmp [rip+disp32]` jumps indirectly through the GOT
// slot at the PC-relative displacement.
[[nodiscard]] inline bool emitX86_64PltStub(
        std::vector<std::uint8_t>& plt,
        std::size_t                stubOffset,
        std::uint64_t              stubVa,
        std::uint64_t              slotVa,
        DiagnosticReporter&        reporter) {
    std::uint64_t const stubEndVa = stubVa + 6;
    std::int64_t const disp =
        static_cast<std::int64_t>(slotVa)
      - static_cast<std::int64_t>(stubEndVa);
    if (disp < std::numeric_limits<std::int32_t>::min()
     || disp > std::numeric_limits<std::int32_t>::max()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             "elf::encodeElfExecDynamic (x86_64 PLT): PLT→GOT "
             "displacement (" + std::to_string(disp) +
             ") does not fit signed 32-bit — module too large.");
        return false;
    }
    plt[stubOffset + 0] = 0xFFu;
    plt[stubOffset + 1] = 0x25u;
    auto const u = static_cast<std::uint32_t>(disp);
    plt[stubOffset + 2] = static_cast<std::uint8_t>(u & 0xFFu);
    plt[stubOffset + 3] = static_cast<std::uint8_t>((u >>  8) & 0xFFu);
    plt[stubOffset + 4] = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
    plt[stubOffset + 5] = static_cast<std::uint8_t>((u >> 24) & 0xFFu);
    return true;
}

// Per-machine PLT stub dispatch. D-LK6-8 closure (2026-06-01).
[[nodiscard]] inline bool emitPltStub(
        std::uint16_t              machine,
        std::vector<std::uint8_t>& plt,
        std::size_t                stubOffset,
        std::uint64_t              stubVa,
        std::uint64_t              slotVa,
        DiagnosticReporter&        reporter) {
    switch (machine) {
        case kEmX86_64:
            return emitX86_64PltStub(plt, stubOffset, stubVa, slotVa, reporter);
        case kEmAArch64:
            return emitArm64PltStub(plt, stubOffset, stubVa, slotVa, reporter);
    }
    emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
         "elf::encodeElfExecDynamic: machine code " +
         std::to_string(machine) + " has no PLT stub emitter — "
         "caller's machine-guard should have rejected this.");
    return false;
}

void writeSectionHeader(std::vector<std::uint8_t>& out, SectionHeader const& h) {
    appendU32LE(out, h.name_offset);
    appendU32LE(out, h.type);
    appendU64LE(out, h.flags);
    appendU64LE(out, h.addr);
    appendU64LE(out, h.offset);
    appendU64LE(out, h.size);
    appendU32LE(out, h.link);
    appendU32LE(out, h.info);
    appendU64LE(out, h.addr_align);
    appendU64LE(out, h.entry_size);
}

// ── LK6 cycle 2b.2: ELF dynamic-linker image emission ───────────
//
// Closes the walker half of plan 14 §3.1 D-LK6-4. Emits an ET_EXEC
// with PT_INTERP + PT_DYNAMIC + writable PT_LOAD #2 covering
// `.got` + `.dynamic`. Uses **eager binding** (DT_FLAGS_1 =
// DF_1_NOW + R_X86_64_GLOB_DAT in `.rela.dyn`) — simpler than
// lazy binding with PLT0 + R_X86_64_JUMP_SLOT, produces a binary
// dyld resolves before transferring control. PLT stubs are 6-byte
// `FF 25 disp32` (jmp [rip+disp32]) bridging direct `call rel32`
// → indirect dispatch via GOT.
//
// Section layout (and on-disk order):
//   PT_LOAD #1 (R+X, page-aligned VA + file offset):
//     [0]               Ehdr                              (64 B)
//     [64]              Program Header Table  (PT_PHDR + PT_INTERP
//                                              + PT_LOAD#1 + PT_LOAD#2
//                                              + PT_DYNAMIC = 5×56=280)
//     [phtEnd]          .interp     (NUL-terminated path)
//     [pad to pageAlign]
//     [pageAlign]       .text       (sh_addr = secText.virtualAddress)
//     [16-aligned]      .plt        (6 B per extern)
//     [8-aligned]       .dynsym     (24 B per entry — STN_UNDEF + N)
//     [1-aligned]       .dynstr     (NUL + extern names + lib paths)
//     [8-aligned]       .hash       (16 + 4*(N+1) bytes)
//     [8-aligned]       .rela.dyn   (24 B per extern — GLOB_DAT)
//   PT_LOAD #2 (R+W, next pageAlign-aligned VA + file offset):
//     [pageAligned]     .got        (8 B per extern, zero-init —
//                                    dyld writes resolved fn ptrs)
//     [8-aligned]       .dynamic    (16 B per DT_* entry)
//   Non-loaded (after PT_LOAD #2):
//     [8-aligned]       .symtab     (intra-module function symbols)
//     [...]             .strtab
//     [...]             .shstrtab
//     [8-aligned]       Section Header Table (13 entries × 64 B)
//
// PLT-stub-internal `jmp [rip+disp32]` is computed inline (not
// pushed through `applyExecRelocations`) — the kernel sig reshape
// for multi-patch-section is anchored at D-LK6-7 and not needed
// for cycle 2b.2.
[[nodiscard]] std::vector<std::uint8_t>
encodeElfExecDynamic(
    AssembledModule const&         module,
    TargetSchema const&            targetSchema,
    ObjectFormatSchema const&      fmt,
    ObjectFormatSectionInfo const& secText,
    DiagnosticReporter&            reporter) {
    auto const& elfId = fmt.elf();
    std::uint64_t const pageAlign = elfId.pageAlign;
    // c150 (D-LK1-4): the ET_DYN shared-library arm rides THIS same
    // dynamic-image substrate. Divergences from ET_EXEC, each gated
    // on `isDyn` so the exec image stays byte-identical:
    //   (a) e_type = ET_DYN, e_entry = 0 (no entry resolution);
    //   (b) no PT_PHDR / PT_INTERP / `.interp` (loaded by an
    //       already-running ld.so, never execve'd);
    //   (c) base-0 VAs (validate() pins text VA == pageAlign, so
    //       baseImageVa computes to 0; the loader slides);
    //   (d) EXPORTS — every externally-visible defined function +
    //       data global gets a real-named `.dynsym` entry findable
    //       through `.hash`;
    //   (e) internal absolute data slots emit R_*_RELATIVE entries
    //       (base-relative addend) instead of link-final in-place
    //       VAs;
    //   (f) extern DATA imports bind got-indirect (a GOT slot +
    //       GLOB_DAT; copy-relocation is exec-only);
    //   (g) zero externs is LEGAL (a self-contained `.so` still
    //       needs `.dynamic`/`.dynsym`/`.hash` for its exports) and
    //       an extern with an EMPTY libraryPath is LEGAL (undefined,
    //       resolved from ld.so's global scope — no DT_NEEDED row).
    //
    // c151 (D-LK1-4 PIE half): ET_DYN splits into TWO sub-shapes,
    // discriminated by the schema's ENTRY CLUSTER (interpreter +
    // processExit + entryCallingConvention + processArgs — validate()
    // pins all-or-none, so `processExit` presence is a faithful
    // single-member witness):
    //   * `.so` (no cluster): everything above, unchanged.
    //   * PIE  (full cluster): a directly-execve'd EXECUTABLE at a
    //     randomized base — the gcc-default shape. Divergences (b)
    //     and the e_entry half of (a) REVERT to the exec shapes, at
    //     BASE-RELATIVE VAs: PT_PHDR + PT_INTERP + `.interp` come
    //     back, e_entry = the trampoline's base-relative VA (ld.so
    //     jumps to base + e_entry), and DT_FLAGS_1 gains DF_1_PIE
    //     alongside DF_1_NOW. e_type STAYS ET_DYN (a PIE is not a
    //     new object type); (c)-(g) stay the dyn shapes — RELATIVE
    //     rows, symbol-based extern-address rows, got-indirect data
    //     imports, exports (the -rdynamic-like stance; harmless in
    //     an executable and keeps the arm uniform with the `.so`).
    //     `isExecveImage` below names the union "exec OR PIE" for
    //     the entry/interp machinery gates.
    bool const isDyn = elfId.objectType == ElfObjectType::Dyn;
    bool const isPie = isDyn && fmt.processExit().has_value();
    // Belt for hand-built ObjectFormatData that bypassed validate():
    // the walker consumes `elf.interpreter` (PT_INTERP) and the
    // trampoline machinery keyed on `processExit` in lock-step — a
    // half-cluster here would emit a broken image (see validate()'s
    // ET_DYN cluster rule), so re-check the two members this walker
    // actually reads.
    if (isDyn && (isPie != !elfId.interpreter.empty())) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"elf::encodeElfExecDynamic (ET_DYN): schema '"}
                 + std::string{fmt.name()}
                 + "' declares a PARTIAL PIE entry cluster ("
                 + (isPie ? "processExit present but elf.interpreter "
                            "empty -- no loader would resolve the "
                            "trampoline's libc exit import"
                          : "elf.interpreter present but no "
                            "processExit -- no trampoline; e_entry "
                            "would be 0 and the kernel would execute "
                            "header bytes")
                 + "). An ET_DYN schema is a .so (neither) or a PIE "
                   "(both + entryCallingConvention + processArgs); "
                   "validate() enforces this all-or-none -- this "
                   "module bypassed it (D-LK1-4).");
        return {};
    }
    bool const isExecveImage = !isDyn || isPie;

    // Pre-conditions (caller dispatches on these; re-checked here so
    // the helper is callable from future entry paths too —
    // silent-failure H2 + C1 convergence).
    if (!isDyn && module.externImports.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: called with zero "
             "externImports -- ET_EXEC dynamic-image emission requires "
             "at least one extern; static executables route through "
             "the non-dynamic ET_EXEC arm. (ET_DYN accepts zero -- a "
             "self-contained .so still carries export metadata.)");
        return {};
    }
    if (module.functions.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: zero functions -- an ET_EXEC "
             "image needs at least one entry function, and a "
             "function-less (data-only) ET_DYN library has no "
             "shipped producer (D-LK1-4).");
        return {};
    }
    if (!elfId.bindNow) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             "elf::encodeElfExecDynamic: schema requests lazy "
             "binding ('elf.bindNow' = false) but ELF lazy "
             "binding (`.rela.plt` + R_X86_64_JUMP_SLOT + 16-byte "
             "PLT0 resolver-trampoline stub) has not yet landed. "
             "Anchored at plan 14 §3.1 D-LK6-11. Set 'elf.bindNow' "
             "= true (the v1 stance — DF_1_NOW + GLOB_DAT in "
             "`.rela.dyn`) to proceed.");
        return {};
    }
    if (secText.virtualAddress < pageAlign) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"elf::encodeElfExecDynamic: .text "
                         "virtualAddress (0x"} +
             std::to_string(secText.virtualAddress) +
             ") is below pageAlign (0x" +
             std::to_string(pageAlign) +
             ") — baseImageVa would underflow. ET_EXEC needs "
             "headers + PHT + .interp to fit in one page below "
             ".text.");
        return {};
    }
    // baseImageVa is one page below .text (so Ehdr + PHT + .interp
    // fit in the first page of PT_LOAD #1, .text starts at
    // secText.virtualAddress = baseImageVa + pageAlign).
    std::uint64_t const baseImageVa = secText.virtualAddress - pageAlign;
    using link::format::detail::alignUp;

    // ── (a) Build .text + per-function offset table ────────────
    std::vector<std::uint8_t> text;
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        funcTextStart.push_back(text.size());
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
    }
    if (text.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: every AssembledFunction "
             "contributed zero bytes — `.text` is empty; ET_EXEC "
             "requires at least one entry instruction.");
        return {};
    }

    // ── (a.5) Build the data sections for the dynamic image ────────────
    //
    // D-LK1-ELF-EXEC-DATA-SECTIONS (rodata) + D-LK4-DATA-PRODUCER
    // (writable `.data` + zero-fill `.bss`). The producer emits an
    // `AssembledData` per module global: read-only ones (`const`, string
    // literals) → `.rodata`; mutable initialized ones → `.data`; tentative
    // zero-init ones → `.bss`. Each kind is laid out by the SAME shared,
    // kind-parameterized `buildExecDataSection` helper (zero copy, zero
    // `if(format)`); the writer places `.rodata` in the R+X PT_LOAD #1
    // (read-only → W^X preserved) and `.data`+`.bss` in the R+W PT_LOAD #2
    // (mutable). `.bss` contributes to PT_LOAD #2's p_memsz but NOT its
    // p_filesz (zero-fill — no file bytes). An EMPTY matching subset yields
    // an empty layout + zero-size section (no-op, byte-identical to the
    // prior image for the no-data case).
    ObjectFormatSectionInfo const* secRodataDyn =
        fmt.sectionByKind(SectionKind::Rodata);
    ObjectFormatSectionInfo const* secDataDyn =
        fmt.sectionByKind(SectionKind::Data);
    ObjectFormatSectionInfo const* secBssDyn =
        fmt.sectionByKind(SectionKind::Bss);
    std::uint64_t const rodataDynAlignFloor =
        secRodataDyn != nullptr ? secRodataDyn->addrAlign : 1;
    std::uint64_t const dataDynAlignFloor =
        secDataDyn != nullptr ? secDataDyn->addrAlign : 1;
    std::uint64_t const bssDynAlignFloor =
        secBssDyn != nullptr ? secBssDyn->addrAlign : 1;
    // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): allowItemRelocations=true — symbol-
    // address global pointers carry abs64 data→data relocs patched in place below
    // (after symbolVa is built). The layouts are MUTABLE so applyDataItemRelocations
    // can fix up their bytes; the emission below reads the patched bytes.
    auto rodataDynLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Rodata, rodataDynAlignFloor,
        "elf::encodeElfExecDynamic", reporter, /*allowItemRelocations=*/true);
    if (!rodataDynLayoutOpt.has_value()) return {};
    auto& rodataDynLayout = *rodataDynLayoutOpt;
    // D-LK-DYN-RODATA-ITEM-RELOC (c150): in the ET_DYN image, `.rodata`
    // lives in the READ-ONLY PT_LOAD #1 — a reloc-bearing rodata item
    // would need a load-time R_*_RELATIVE write into a non-writable
    // page (DT_TEXTREL territory, rejected by hardened loaders). The
    // shipped producer routes every reloc-bearing CONST global to
    // `relro` (c145), which the dynamic arm merges into the WRITABLE
    // `.data` — so the only rodata+relocs producer today is the LK11
    // cross-CU thunk slot (multi-CU merge). Fail loud rather than emit
    // a TEXTREL image; the fix is relro placement for those slots.
    if (isDyn) {
        for (std::size_t j = 0; j < rodataDynLayout.itemIndices.size(); ++j) {
            auto const& di = module.dataItems[rodataDynLayout.itemIndices[j]];
            if (di.relocations.empty()) continue;
            emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                 std::format("elf::encodeElfExecDynamic (ET_DYN): rodata "
                             "data item #{} (SymbolId={{ {} }}) carries {} "
                             "relocation(s) -- a slid shared library cannot "
                             "patch read-only pages at load (DT_TEXTREL). "
                             "Reloc-bearing const data belongs in `relro` "
                             "(c145); the cross-CU thunk-slot placement is "
                             "the open producer "
                             "(D-LK-DYN-RODATA-ITEM-RELOC).",
                             rodataDynLayout.itemIndices[j], di.symbol.v,
                             di.relocations.size()));
            return {};
        }
    }
    auto dataDynLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Data, dataDynAlignFloor,
        "elf::encodeElfExecDynamic", reporter, /*allowItemRelocations=*/true);
    if (!dataDynLayoutOpt.has_value()) return {};
    auto& dataDynLayout = *dataDynLayoutOpt;
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): a CONST global carrying LOAD-TIME
    // relocations (a const function-pointer table / `int *const p=&x;`) lands in
    // `relro`. In the EXECUTABLE image we FOLD it into `.data` ("treat relro like
    // .data" — the loader writes the resolved target VA into each slot; the
    // read-only-after-relocation hardening of a separate GNU_RELRO segment is a
    // nicety not required for correctness). Merging routes relro through the SAME
    // machinery `.data` already uses (addDataSymbolVas, applyDataItemRelocations,
    // R+W PT_LOAD #2, the `.data` section header) with ZERO parallel-section
    // plumbing through this intricate dynamic arm — and stays BYTE-IDENTICAL for a
    // relro-free module (the merge is a no-op when the relro subset is empty, so
    // the sqlite-dormant / no-data byte-identity guarantees hold). The distinct
    // `.data.rel.ro` + `.rela.data.rel.ro` (gcc's contract) is emitted only by the
    // RELOCATABLE `.o` writer (`encode`, ET_REL). No relro section ROW is declared
    // on the exec schema (relro rides `.data`), so the floor peek yields 1.
    ObjectFormatSectionInfo const* secRelRoDyn =
        fmt.sectionByKind(SectionKind::RelRoConst);
    std::uint64_t const relroDynAlignFloor =
        secRelRoDyn != nullptr ? secRelRoDyn->addrAlign : 1;
    auto relroDynLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::RelRoConst, relroDynAlignFloor,
        "elf::encodeElfExecDynamic", reporter, /*allowItemRelocations=*/true);
    if (!relroDynLayoutOpt.has_value()) return {};
    link::format::mergeFileBackedDataSection(dataDynLayout, *relroDynLayoutOpt);
    auto const bssDynLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Bss, bssDynAlignFloor,
        "elf::encodeElfExecDynamic", reporter);
    if (!bssDynLayoutOpt.has_value()) return {};
    auto const& bssDynLayout = *bssDynLayoutOpt;
    // D-CSUBSET-THREAD-LOCAL (TLS C1): the thread-local template pair —
    // `.tdata` (initialized per-thread template bytes, file-backed) +
    // `.tbss` (zero-fill per-thread extent). Laid out by the SAME shared
    // kind-parameterized helper as every other data section. `.tdata`
    // takes allowItemRelocations=true (CRIT-2's second half: a
    // `thread_local char *msg = "hi";` template slot is patched IN PLACE
    // with the target's absolute VA below — sound for this fixed-base
    // ET_EXEC: every thread's copy starts from the patched template);
    // `.tbss` is zero-fill and can carry none.
    ObjectFormatSectionInfo const* secTdataDyn =
        fmt.sectionByKind(SectionKind::ThreadData);
    ObjectFormatSectionInfo const* secTbssDyn =
        fmt.sectionByKind(SectionKind::ThreadBss);
    std::uint64_t const tdataDynAlignFloor =
        secTdataDyn != nullptr ? secTdataDyn->addrAlign : 1;
    std::uint64_t const tbssDynAlignFloor =
        secTbssDyn != nullptr ? secTbssDyn->addrAlign : 1;
    auto tdataDynLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Tdata, tdataDynAlignFloor,
        "elf::encodeElfExecDynamic", reporter, /*allowItemRelocations=*/true);
    if (!tdataDynLayoutOpt.has_value()) return {};
    auto& tdataDynLayout = *tdataDynLayoutOpt;
    auto const tbssDynLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Tbss, tbssDynAlignFloor,
        "elf::encodeElfExecDynamic", reporter);
    if (!tbssDynLayoutOpt.has_value()) return {};
    auto const& tbssDynLayout = *tbssDynLayoutOpt;
    bool const hasRodataDyn = !rodataDynLayout.empty();
    bool const hasDataDyn   = !dataDynLayout.empty();
    bool const hasBssDyn    = !bssDynLayout.empty();
    bool const hasTdataDyn  = !tdataDynLayout.empty();
    bool const hasTbssDyn   = !tbssDynLayout.empty();
    // hasTls gates EVERY TLS-side emission below (PT_TLS, the phdr-count
    // bump, the layout shift, the section headers) so a no-TLS module's
    // image stays BYTE-IDENTICAL to the pre-TLS walker (the sqlite-dormant
    // guarantee — pinned by NoTlsModuleByteIdenticalToPreTlsShape).
    bool const hasTls = hasTdataDyn || hasTbssDyn;
    // D-LK-DYN-TLS-MODEL (c150): thread-locals in a SHARED LIBRARY need
    // the general-/local-dynamic (or initial-exec) TLS model — the
    // library's block gets a LOADER-assigned module offset, so the
    // link-time local-exec tpoffs this walker computes (valid only for
    // the executable's own PT_TLS block) would silently address another
    // module's slots. The shipped dyn schema advertises no tdata/tbss
    // (the linker's acceptsDataSection gate + the MIR->LIR tlsAccess
    // gate fire first on the real pipeline); this is the walker belt
    // for a hand-built module / a prematurely-opted-in schema.
    if (isDyn && hasTls) {
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             "elf::encodeElfExecDynamic (ET_DYN): module carries "
             "thread-local data items but a shared library's TLS block "
             "has a loader-assigned module offset -- the local-exec "
             "link-time tpoffs this walker computes are exec-only. "
             "General-dynamic/initial-exec TLS for .so output is not "
             "implemented (D-LK-DYN-TLS-MODEL). (A PIE, though it IS "
             "the executable and local-exec would be model-correct for "
             "its own thread-locals, ships without TLS rows until the "
             "PT_TLS-under-slide layout is witnessed -- same anchor.)");
        return {};
    }
    // Each present section's schema row is MANDATORY (the format JSON must
    // declare it — fail loud rather than emit an unnamed section header).
    if (hasRodataDyn && secRodataDyn == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encodeElfExecDynamic: module carries Rodata items but the "
             "format declares no 'rodata' section row.");
        return {};
    }
    if (hasDataDyn && secDataDyn == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encodeElfExecDynamic: module carries Data items but the "
             "format declares no 'data' section row (D-LK4-DATA-PRODUCER).");
        return {};
    }
    if (hasBssDyn && secBssDyn == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encodeElfExecDynamic: module carries Bss items but the "
             "format declares no 'bss' section row (D-LK4-DATA-PRODUCER).");
        return {};
    }
    if (hasTdataDyn && secTdataDyn == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encodeElfExecDynamic: module carries Tdata (thread-local "
             "template) items but the format declares no 'tdata' section row "
             "(D-CSUBSET-THREAD-LOCAL).");
        return {};
    }
    if (hasTbssDyn && secTbssDyn == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encodeElfExecDynamic: module carries Tbss (zero-fill "
             "thread-local) items but the format declares no 'tbss' section "
             "row (D-CSUBSET-THREAD-LOCAL).");
        return {};
    }
    // D-CSUBSET-THREAD-LOCAL: the TARGET must declare its static-TLS layout
    // convention (`"tls"` identity block — Variant I/II + tcbHeaderBytes)
    // before any tpoff can be computed. Absence is the capability signal
    // (belt over the MIR→LIR `tlsbase`-opcode gate, which fires first on
    // the real pipeline); a hand-built module reaching this walker under a
    // TLS-less target fails loud here, never guesses a variant.
    if (hasTls && !targetSchema.tlsIdentity().has_value()) {
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             std::string{"elf::encodeElfExecDynamic: module carries "
                         "thread-local data items but target schema '"}
                 + std::string{targetSchema.name()}
                 + "' declares no 'tls' identity block (variant + "
                   "tcbHeaderBytes) — cannot compute thread-pointer "
                   "offsets (D-CSUBSET-THREAD-LOCAL).");
        return {};
    }
    std::vector<std::uint8_t> const& rodataDyn = rodataDynLayout.bytes;
    std::vector<std::uint8_t> const& dataDynBytes = dataDynLayout.bytes;

    // ── (b) Collect the DISTINCT import libraries → the DT_NEEDED set
    //
    // c87 (D-FFI-MATH-LIBM-DT-NEEDED): one DT_NEEDED per DISTINCT
    // `libraryPath` across ALL import rows — functions AND data. Every
    // import row that reaches this walker is emitted into `.dynsym`
    // and eagerly bound at load (DF_1_NOW), so every row's owning
    // library must be listed or ld.so stops at the first symbol it
    // cannot resolve (the c86 sqlite3 binary carried only libc.so.6
    // while math.json's sqrt/pow/… live in libm.so.6 → "undefined
    // symbol: sqrt" at load). The order is PINNED: lexicographic
    // (byte-wise) over the library names — deterministic AND
    // config-agnostic. First-appearance order (the pre-c87 shape)
    // shifts with CU/merge order; a "libc first" rule would hardcode
    // a library name in the engine. The names themselves come
    // exclusively from config (shipped-lib descriptors' `library`
    // maps, the language's `externLibraryByFormat` default, the
    // format's `processExit.importLibraryPath`) — the engine never
    // invents one.
    // c150 (D-LK1-4): an extern with an EMPTY libraryPath contributes
    // no DT_NEEDED row. On the ET_DYN arm such rows are LEGAL — the
    // c143 undefined-extern gate KEEPS a referenced no-library extern
    // for a `.so` (ld.so resolves it from the global scope: the
    // executable or a sibling library defines it); it still gets its
    // UNDEF `.dynsym` entry + PLT/GOT machinery below. On the exec
    // arm the linker gate rejected such rows before dispatch, so the
    // skip is a no-op there.
    std::vector<std::string> libraryOrder;
    libraryOrder.reserve(module.externImports.size());
    for (auto const& ext : module.externImports) {
        if (!ext.libraryPath.empty()) libraryOrder.push_back(ext.libraryPath);
    }
    std::sort(libraryOrder.begin(), libraryOrder.end());
    libraryOrder.erase(
        std::unique(libraryOrder.begin(), libraryOrder.end()),
        libraryOrder.end());
    std::size_t const numExterns = module.externImports.size();
    std::size_t const numLibs = libraryOrder.size();
    // Defense-in-depth (exec + PIE arms): the linker validates
    // per-extern non-empty `libraryPath` before dispatch (LK6 cycle
    // 2a substrate + the c143 image reject — which the c151 PIE
    // inherits via `allowsUndefinedImports() == false`), so
    // `numLibs == 0` with externs present cannot occur via the
    // linker entry path. This guard makes that invariant explicit in
    // case a future caller bypasses the linker (e.g. a writer-only
    // test harness) — an executable with zero DT_NEEDED entries
    // fails at runtime (the loader has no library to resolve externs
    // from). Only the `.so` sub-shape is exempt: its externs may all
    // be global-scope-resolved (no DT_NEEDED rows — ld.so binds them
    // from the executable / sibling libraries at load). The
    // `numExterns > 0` guard is load-bearing for the PIE only (the
    // exec arm's zero-extern precondition already returned above);
    // a hand-built zero-extern PIE is as legal as a zero-extern .so.
    if ((isPie || !isDyn) && numExterns > 0 && numLibs == 0) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: " +
             std::to_string(numExterns) +
             " extern(s) declared but every `libraryPath` is empty "
             "— zero DT_NEEDED entries would ship; dyld cannot "
             "resolve undefined symbols without a library.");
        return {};
    }

    // ── (b.5) Partition externs: FUNCTION vs DATA imports ──────────
    //
    // c84 (D-LK-EXTERN-DATA-IMPORT): a FUNCTION import binds through
    // the PLT/GOT machinery below (a PLT stub + GOT slot + GLOB_DAT);
    // a DATA import (libc `stdout` — `ExternImport.isData`) binds via
    // an ELF COPY RELOCATION: the exec reserves a `.bss` slot of the
    // object's exact size+alignment, exports the symbol as a DEFINED
    // OBJECT at that slot, and emits one R_*_COPY in `.rela.dyn`; the
    // loader memcpy's the library's object into the slot at startup
    // and ALL references (this exec's code via the normal GlobalAddr
    // path; other images via interposition) converge on the copy —
    // gcc's non-PIE ET_EXEC mechanism, zero new instruction
    // encodings. `externSlot[i]` is the extern's ordinal within its
    // OWN class (PLT/GOT slot for functions; copy-slot for data).
    std::vector<std::size_t> externSlot(numExterns, 0);
    std::size_t numFuncExterns = 0;
    std::size_t numDataExterns = 0;
    for (std::size_t i = 0; i < numExterns; ++i) {
        externSlot[i] = module.externImports[i].isData
                            ? numDataExterns++
                            : numFuncExterns++;
    }
    // c150 (D-LK1-4): the data-import binding is FLAVOR-keyed.
    //   * ET_EXEC → copy-relocation (c84): a `.bss` copy slot + one
    //     R_*_COPY; the exec owns the canonical copy.
    //   * ET_DYN → got-indirect (the c117/c149 model): a GOT slot +
    //     one R_*_GLOB_DAT; ld.so writes the object's address into
    //     the slot and the GotIndirect lowering derefs it. A copy
    //     relocation inside a `.so` is INVALID ELF (copy relocs are
    //     the executable's mechanism; validate() rejects the config).
    bool const hasCopySlots    = !isDyn && numDataExterns > 0;
    bool const hasGotDataSlots =  isDyn && numDataExterns > 0;
    if (numDataExterns > 0) {
        // The linker's pre-walker gate admits data imports only when
        // the schema DECLARES a binding model; each flavor arm here
        // implements exactly one. A mismatched declaration must fail
        // loud, not silently get a slot kind it did not declare.
        auto const binding = fmt.dataImportBinding();
        DataImportBinding const required = isDyn
            ? DataImportBinding::GotIndirect
            : DataImportBinding::CopyRelocation;
        if (!binding.has_value() || *binding != required) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encodeElfExecDynamic: module carries "}
                     + std::to_string(numDataExterns)
                     + " extern DATA import(s) but format '"
                     + std::string{fmt.name()}
                     + "' does not declare 'dataImportBinding': \""
                     + std::string{dataImportBindingName(required)}
                     + "\" -- the data-import mechanism the "
                     + (isDyn ? "ET_DYN" : "ET_EXEC")
                     + " arm implements (D-LK-EXTERN-DATA-IMPORT / "
                       "D-LK1-4).");
            return {};
        }
    }
    // Validate each data import's shape — EXEC (copy-slot) arm only:
    // the copy slot needs a real size (an INCOMPLETE declared type —
    // `extern const char v[];` — carries 0/0: legal ONLY when a
    // sibling CU defines it, in which case the LK11 merge strips the
    // row before this walker runs; one SURVIVING here means a true
    // library import of an unsizeable object — fail loud, an unsized
    // copy slot cannot be reserved) and a power-of-two alignment
    // (layout-derived upstream; re-check so a hand-built module
    // cannot corrupt the slot packing). The ET_DYN got-indirect slot
    // is a pointer — the object's size is irrelevant (an incomplete
    // `extern char v[];` binds fine through the GOT), so the check
    // is skipped there.
    for (std::size_t i = 0; hasCopySlots && i < numExterns; ++i) {
        auto const& ext = module.externImports[i];
        if (!ext.isData) continue;
        if (ext.dataSizeBytes == 0 || ext.dataAlignBytes == 0) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encodeElfExecDynamic: extern DATA "
                             "import '"} + ext.mangledName
                     + "' carries no computable object size/alignment "
                       "(declared with an INCOMPLETE type and no "
                       "defining sibling CU resolved it). A copy-"
                       "relocation slot needs the object's exact size "
                       "— complete the extern's declared type or "
                       "compile it with its defining translation "
                       "unit. D-LK-EXTERN-DATA-IMPORT.");
            return {};
        }
        if ((ext.dataAlignBytes & (ext.dataAlignBytes - 1)) != 0) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encodeElfExecDynamic: extern DATA "
                             "import '"} + ext.mangledName
                     + "' carries a non-power-of-two alignment ("
                     + std::to_string(ext.dataAlignBytes)
                     + ") — the copy-slot packing below would place "
                       "it at a wrong offset. Layout-derived "
                       "alignments are powers of two; this module "
                       "row is corrupt.");
            return {};
        }
    }

    // c84: `.bss` exists for module zero-init globals AND/OR extern-
    // data copy slots (exec arm; the dyn arm's data externs live in
    // the GOT, so only module globals count there).
    bool const hasBssSection = hasBssDyn || hasCopySlots;

    // ── Section indices (hoisted above the dynsym build — c84/c150) ──
    // Computed INCREMENTALLY from the emit order below so adding/
    // removing an optional section ([.interp]/[.rodata]/[.data]/
    // [.bss]) keeps every cross-reference (.link/.info/e_shstrndx)
    // coherent with the actual header table. Hoisted ABOVE the
    // symbol-table builds because (1) the dynsym data-extern patch
    // needs IDX_BSS, (2) the `.symtab` STT_SECTION + function entries
    // need IDX_TEXT (c150 — no longer a hardcoded 2: the dyn image
    // has no `.interp`, shifting `.text` to index 1), and (3) the
    // ET_DYN export patch needs IDX_TEXT/IDX_RODATA/IDX_DATA/IDX_BSS.
    // Emit order:
    //   0 NULL, [.interp — exec only], .text, [.rodata], .plt,
    //   .dynsym, .dynstr, .hash, .rela.dyn, [.tdata], [.tbss],
    //   [.data], .got, .dynamic, [.bss], .symtab, .strtab, .shstrtab
    // (D-CSUBSET-THREAD-LOCAL: `.tdata` right before `.data` matches the
    // file layout — its bytes physically open PT_LOAD #2; `.tbss` (NOBITS,
    // no file bytes) sits beside its template, the gcc pairing.)
    std::uint16_t idxCursor = 0;
    auto nextIdx = [&]() { return idxCursor++; };
    std::uint16_t const IDX_NULL   = nextIdx();  (void)IDX_NULL;
    // `.interp` row exists on every execve'd image (exec AND the
    // c151 PIE sub-mode); only the `.so` skips it.
    std::uint16_t const IDX_INTERP =
        isExecveImage ? nextIdx() : std::uint16_t{0};
    (void)IDX_INTERP;
    std::uint16_t const IDX_TEXT   = nextIdx();  (void)IDX_TEXT;
    std::uint16_t const IDX_RODATA = hasRodataDyn ? nextIdx() : std::uint16_t{0};
    (void)IDX_RODATA;
    std::uint16_t const IDX_PLT    = nextIdx();  (void)IDX_PLT;
    std::uint16_t const IDX_DYNSYM = nextIdx();
    std::uint16_t const IDX_DYNSTR = nextIdx();
    std::uint16_t const IDX_HASH   = nextIdx();  (void)IDX_HASH;
    std::uint16_t const IDX_RELADYN = nextIdx(); (void)IDX_RELADYN;
    std::uint16_t const IDX_TDATA  = hasTdataDyn ? nextIdx() : std::uint16_t{0};
    (void)IDX_TDATA;
    std::uint16_t const IDX_TBSS   = hasTbssDyn ? nextIdx() : std::uint16_t{0};
    (void)IDX_TBSS;
    std::uint16_t const IDX_DATA   = hasDataDyn ? nextIdx() : std::uint16_t{0};
    (void)IDX_DATA;
    std::uint16_t const IDX_GOT    = nextIdx();  (void)IDX_GOT;
    std::uint16_t const IDX_DYNAMIC = nextIdx(); (void)IDX_DYNAMIC;
    std::uint16_t const IDX_BSS    = hasBssSection ? nextIdx() : std::uint16_t{0};
    (void)IDX_BSS;
    std::uint16_t const IDX_SYMTAB = nextIdx();  (void)IDX_SYMTAB;
    std::uint16_t const IDX_STRTAB = nextIdx();
    std::uint16_t const IDX_SHSTRTAB = nextIdx();
    std::uint16_t const kNumSections = idxCursor;

    // ── (b.7) ET_DYN export set (c150, D-LK1-4) ─────────────────
    // Every externally-visible DEFINED symbol — function or data
    // global — gets a real-named `.dynsym` entry so a foreign
    // consumer (`gcc main.c -L. -lfoo`, then ld.so at load) can bind
    // it. The set comes from `module.symbols` (the same real-name
    // rows the ET_REL `.symtab` uses, c139): Local/Hidden/Internal
    // symbols stay out (a `static` function is not part of the
    // library's ABI); Weak exports keep STB_WEAK. Classification is
    // by definition table: a row naming a function exports STT_FUNC
    // with the function's byte size; a row naming a data item
    // exports STT_OBJECT with its section size. st_value/st_shndx
    // are patched after layout (the same two-phase shape as the c84
    // data-extern patch). The exec arm exports nothing — its dynsym
    // stays imports-only, byte-identical to the pre-c150 image.
    struct DynExportRec {
        SymbolId      sym{};
        std::string   name;          // real (already-mangled) source name
        std::uint8_t  info = 0;      // st_info (bind<<4 | type)
        std::uint64_t size = 0;      // st_size
        std::uint32_t nameOff = 0;   // .dynstr offset (filled at (d))
        std::uint32_t dynsymIdx = 0; // entry index (filled at (e))
    };
    std::vector<DynExportRec> dynExports;
    if (isDyn) {
        std::unordered_map<std::uint32_t, std::size_t> funcIdxBySym;
        funcIdxBySym.reserve(module.functions.size());
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            funcIdxBySym.emplace(module.functions[i].symbol.v, i);
        }
        std::unordered_map<std::uint32_t, std::uint64_t> dataSizeBySym;
        dataSizeBySym.reserve(module.dataItems.size());
        for (auto const& di : module.dataItems) {
            if (di.symbol == SymbolId{}) continue;  // anonymous — never exported
            dataSizeBySym.emplace(di.symbol.v, di.sizeInSection());
        }
        for (auto const& ms : module.symbols) {
            if (ms.name.empty()) continue;
            if (!isExternallyVisible(ms.binding, ms.visibility)) continue;
            std::uint8_t const bind =
                ms.binding == SymbolBinding::Weak ? STB_WEAK : STB_GLOBAL;
            if (auto const fit = funcIdxBySym.find(ms.symbol.v);
                fit != funcIdxBySym.end()) {
                dynExports.push_back(DynExportRec{
                    ms.symbol, ms.name, makeStInfo(bind, STT_FUNC),
                    module.functions[fit->second].bytes.size(), 0, 0});
            } else if (auto const dit = dataSizeBySym.find(ms.symbol.v);
                       dit != dataSizeBySym.end()) {
                dynExports.push_back(DynExportRec{
                    ms.symbol, ms.name, makeStInfo(bind, STT_OBJECT),
                    dit->second, 0, 0});
            } else {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"elf::encodeElfExecDynamic (ET_DYN): "
                                 "ModuleSymbol '"} + ms.name
                         + "' (SymbolId #"
                         + std::to_string(ms.symbol.v)
                         + ") names neither a defined function nor a "
                           "data item -- the export table cannot place "
                           "it (producer contract: one ModuleSymbol row "
                           "per DEFINED function/global).");
                return {};
            }
        }
    }

    // ── (c) .interp body (NUL-terminated dynamic-linker path) ──
    // Emitted on every execve'd image: ET_EXEC and the c151 PIE
    // sub-mode (the kernel maps the named loader, which relocates
    // the PIE at a randomized base). The `.so` emits none (mapped by
    // an already-running ld.so; validate() guarantees
    // `elf.interpreter` is empty for the entry-cluster-less dyn
    // shape) — its vector stays EMPTY so the interp body/phdr/
    // section emissions below all no-op there.
    std::vector<std::uint8_t> interp;
    if (isExecveImage) {
        for (char c : elfId.interpreter)
            interp.push_back(static_cast<std::uint8_t>(c));
        interp.push_back(0);
    }

    // ── (d) .dynstr body (NUL + extern names + library paths)
    std::vector<std::uint8_t> dynstr;
    dynstr.push_back(0);
    std::vector<std::uint32_t> externNameOff(numExterns);
    for (std::size_t i = 0; i < numExterns; ++i) {
        externNameOff[i] = static_cast<std::uint32_t>(dynstr.size());
        for (char c : module.externImports[i].mangledName)
            dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }
    std::vector<std::uint32_t> libNameOff(numLibs);
    for (std::size_t i = 0; i < numLibs; ++i) {
        libNameOff[i] = static_cast<std::uint32_t>(dynstr.size());
        for (char c : libraryOrder[i])
            dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }
    // ET_DYN (c150): export names + the optional DT_SONAME string.
    // Both are dyn-only additions — the exec dynstr stays
    // byte-identical (dynExports is empty and soname is
    // validate-rejected on non-dyn schemas).
    for (auto& ex : dynExports) {
        ex.nameOff = static_cast<std::uint32_t>(dynstr.size());
        for (char c : ex.name)
            dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }
    std::uint32_t sonameOff = 0;
    if (isDyn && !elfId.soname.empty()) {
        sonameOff = static_cast<std::uint32_t>(dynstr.size());
        for (char c : elfId.soname)
            dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }

    // ── (e) .dynsym body (STN_UNDEF + N extern symbols)
    std::vector<std::uint8_t> dynsym;
    auto appendDynsymEntry = [&](std::uint32_t nameOff,
                                  std::uint8_t info,
                                  std::uint16_t shndx,
                                  std::uint64_t value,
                                  std::uint64_t size) {
        appendU32LE(dynsym, nameOff);
        appendU8(dynsym, info);
        appendU8(dynsym, 0);
        appendU16LE(dynsym, shndx);
        appendU64LE(dynsym, value);
        appendU64LE(dynsym, size);
    };
    appendDynsymEntry(0, 0, 0, 0, 0);  // STN_UNDEF
    std::vector<std::uint32_t> dynsymIdx(numExterns);
    for (std::size_t i = 0; i < numExterns; ++i) {
        dynsymIdx[i] =
            static_cast<std::uint32_t>(dynsym.size() / 24);
        // A FUNCTION import is UNDEF (the loader resolves it into the
        // GOT via GLOB_DAT). A DATA import (c84 copy-relocation) is a
        // DEFINED OBJECT in THIS executable: st_size is the object's
        // layout-derived size (the loader copies min(st_size) bytes;
        // other images bind to this definition by interposition);
        // st_value (the `.bss` slot VA) + st_shndx (the `.bss` section
        // index) are patched after layout, once both exist.
        bool const isData = module.externImports[i].isData;
        // st_size: the copy-relocation arm (exec) needs the object's
        // size (the loader copies min(st_size) bytes); the dyn
        // got-indirect arm's import stays a plain UNDEF reference
        // (size 0 — no copy happens, the slot holds a pointer).
        appendDynsymEntry(externNameOff[i],
                          makeStInfo(STB_GLOBAL,
                                     isData ? STT_OBJECT : STT_NOTYPE),
                          SHN_UNDEF, 0,
                          (isData && !isDyn)
                              ? module.externImports[i].dataSizeBytes
                              : 0);
    }
    // ET_DYN exports (c150): appended AFTER the imports so import
    // dynsym indices (and the exec image) are unchanged. st_shndx /
    // st_value are patched post-layout; st_size is final now.
    for (auto& ex : dynExports) {
        ex.dynsymIdx = static_cast<std::uint32_t>(dynsym.size() / 24);
        appendDynsymEntry(ex.nameOff, ex.info, SHN_UNDEF, 0, ex.size);
    }

    // ── (f) .hash body (DT_HASH single-bucket)
    //
    // ONE bucket whose chain threads EVERY dynsym entry (imports AND
    // the c150 dyn exports): ld.so hashes the wanted name, lands in
    // bucket 0, and walks the chain comparing names — a single-bucket
    // table is slower than gcc's GNU_HASH but exactly as CORRECT
    // (findability is what the ET_DYN export contract needs). nchain
    // == the dynsym entry count (gABI: chain parallels the symbol
    // table).
    std::size_t const numDynsymEntries = dynsym.size() / 24;
    std::vector<std::uint8_t> hashSec;
    appendU32LE(hashSec, 1);  // nbucket
    appendU32LE(hashSec, static_cast<std::uint32_t>(numDynsymEntries));  // nchain
    appendU32LE(hashSec,
        numDynsymEntries > 1 ? 1u : 0u);               // bucket[0]
    appendU32LE(hashSec, 0);                            // chain[0]=STN_UNDEF
    for (std::size_t i = 1; i < numDynsymEntries; ++i) {
        std::uint32_t const next =
            (i + 1 < numDynsymEntries) ? static_cast<std::uint32_t>(i + 1)
                                       : 0u;
        appendU32LE(hashSec, next);
    }

    // ── (g) .plt body (placeholder; filled after layout)
    // D-LK6-8 closure: stub size is per-machine (x86_64 = 6,
    // ARM64 = 16). c84: FUNCTION imports only — a DATA import gets a
    // `.bss` copy slot (b.5), never a PLT stub (code bytes read as a
    // data value = the silent-miscompile class).
    std::uint16_t const machine = fmt.elf().machine;
    std::uint64_t const pltStubSize = pltStubSizeFor(machine);
    std::vector<std::uint8_t> plt(numFuncExterns * pltStubSize, 0);

    // ── (h) .got body (zero-init; dyld writes resolved fn ptrs) —
    // FUNCTION imports on the exec arm (c84: exec data imports have
    // no GOT slot; the copy slot in `.bss` is their storage). The
    // ET_DYN arm (c150) ALSO gives each DATA import a GOT slot after
    // the function slots — the got-indirect binding: ld.so writes
    // the object's address into the slot (GLOB_DAT) and the
    // GotIndirect lowering derefs it. Slot index for data extern i
    // = numFuncExterns + externSlot[i].
    std::size_t const numGotSlots =
        numFuncExterns + (hasGotDataSlots ? numDataExterns : 0);
    std::vector<std::uint8_t> got(numGotSlots * 8, 0);

    // ── (i) Layout: compute file offsets + VAs ─────────────────
    constexpr std::uint64_t kEhdrSize = 64;
    constexpr std::uint64_t kPhdrSize = 56;
    // EXEC: PHDR + INTERP + LOAD×2 + DYNAMIC, plus PT_TLS ONLY when the
    // module carries thread-local items (D-CSUBSET-THREAD-LOCAL, audit
    // fold HIGH-2): the conditional count keeps every no-TLS image —
    // sqlite included — BYTE-IDENTICAL to the pre-TLS walker (phtSize/
    // interpOff/every downstream offset shifts ONLY when TLS is
    // present).
    // DYN `.so` (c150): LOAD×2 + DYNAMIC only — no PT_INTERP (loaded
    // by an already-running ld.so) and no PT_PHDR (an aux-vector
    // nicety for process images; gcc's `ld -shared` omits it too).
    // PIE (c151): the exec 5 — PT_PHDR + PT_INTERP + LOAD×2 +
    // DYNAMIC (gcc PIE ground truth carries both PHDR and INTERP);
    // TLS-in-dyn (both sub-shapes) was rejected above, so the PIE
    // never takes the 6-phdr TLS arm.
    std::uint32_t const numPhdrs =
        !isExecveImage ? 3u : (hasTls ? 6u : 5u);
    std::uint64_t const phtOff = kEhdrSize;
    std::uint64_t const phtSize = numPhdrs * kPhdrSize;
    std::uint64_t const interpOff = phtOff + phtSize;
    std::uint64_t const interpVa  = baseImageVa + interpOff;

    std::uint64_t const textOff = pageAlign;   // .text at page boundary
    std::uint64_t const textVa  = secText.virtualAddress;

    // `.rodata` immediately after `.text` (both R+X PT_LOAD #1). Aligned
    // to the layout's max item alignment (gABI sh_addralign). Empty when
    // the module has no rodata → zero-size section, `.plt` follows `.text`
    // directly (byte-identical to the pre-rodata image).
    std::uint64_t const rodataAlignDyn =
        hasRodataDyn ? rodataDynLayout.maxAlign : 1;
    std::uint64_t const rodataOff =
        hasRodataDyn ? alignUp(textOff + text.size(), rodataAlignDyn)
                     : textOff + text.size();
    std::uint64_t const rodataVa  = baseImageVa + rodataOff;
    std::uint64_t const rodataSz  = rodataDyn.size();

    // Subsequent sections in PT_LOAD #1; VA = baseImageVa + fileOff
    // (PT_LOAD with fileoff=0 and vaddr=baseImageVa maps verbatim).
    std::uint64_t const pltOff = alignUp(rodataOff + rodataSz, 16);
    std::uint64_t const pltVa  = baseImageVa + pltOff;
    std::uint64_t const pltSize = plt.size();

    std::uint64_t const dynsymOff = alignUp(pltOff + pltSize, 8);
    std::uint64_t const dynsymVa  = baseImageVa + dynsymOff;
    std::uint64_t const dynsymSz  = dynsym.size();

    std::uint64_t const dynstrOff = dynsymOff + dynsymSz;
    std::uint64_t const dynstrVa  = baseImageVa + dynstrOff;
    std::uint64_t const dynstrSz  = dynstr.size();

    std::uint64_t const hashOff = alignUp(dynstrOff + dynstrSz, 8);
    std::uint64_t const hashVa  = baseImageVa + hashOff;
    std::uint64_t const hashSz  = hashSec.size();

    std::uint64_t const relaDynOff = alignUp(hashOff + hashSz, 8);
    std::uint64_t const relaDynVa  = baseImageVa + relaDynOff;
    // c150 (D-LK1-4): on the ET_DYN arm every internal absolute
    // 64-bit data slot (a fn-ptr-table entry, a `&global`
    // initializer, a jump-table row — the abs64 data-item relocs the
    // exec arm patches in place with FINAL VAs) additionally emits
    // one R_*_RELATIVE entry, so the loader can add the slide. The
    // COUNT is fixed by the module (one per data-item relocation in
    // the merged `.data` layout — relro rides it, c145); the CONTENT
    // is assembled post-apply, when the base-relative slot values
    // exist. `.rodata` items were rejected above if reloc-bearing
    // (D-LK-DYN-RODATA-ITEM-RELOC) and `.bss`/tls carry none, so the
    // merged `.data` layout is the complete RELATIVE universe.
    std::size_t numRelativeRelocs = 0;
    if (isDyn) {
        for (std::size_t j = 0; j < dataDynLayout.itemIndices.size(); ++j) {
            numRelativeRelocs +=
                module.dataItems[dataDynLayout.itemIndices[j]]
                    .relocations.size();
        }
    }
    std::uint64_t const relaDynSz  =
        (numExterns + numRelativeRelocs) * 24;

    std::uint64_t const ptLoad1End = relaDynOff + relaDynSz;

    // PT_LOAD #2 (R+W) — page-aligned in both file + VA. Holds the WRITABLE
    // sections: `.data` (file-backed, mutable initialized globals — D-LK4-
    // DATA-PRODUCER) first, then .got + .dynamic (the dynamic-linker
    // writables), then `.bss` (zero-fill — memsz only, NO file bytes) LAST.
    // Keeping mutable globals OUT of PT_LOAD #1 (R+X) preserves W^X. Within
    // the segment file offset and VA advance in lock-step (delta constant);
    // `.bss` is last so its memsz tail extends p_memsz beyond p_filesz
    // without skewing any file-backed section's VA↔offset congruence.
    std::uint64_t const ptLoad2Start = alignUp(ptLoad1End, pageAlign);
    std::uint64_t const ptLoad2VaStart = baseImageVa + ptLoad2Start;

    // ── D-CSUBSET-THREAD-LOCAL (TLS C1, audit folds HIGH-1/HIGH-2) ──
    // `.tdata` is the FIRST file-backed member of PT_LOAD #2 (before
    // `.data`), and PT_TLS points at it: p_offset/p_vaddr = tdata,
    // p_filesz = tdata span, p_memsz = tdata + tbss block. `.tbss`
    // occupies NO file bytes AND NO PT_LOAD memory — unlike `.bss`, the
    // per-thread copies are LOADER-allocated (one per thread, sized by
    // PT_TLS p_memsz); nothing lives at its nominal VA in the process
    // image, so extending PT_LOAD #2's p_memsz over it would reserve
    // dead process-shared memory and desync the loader's TLS block from
    // the segment map.
    //
    // p_align (tlsAlign) = max of the present TLS sections' member
    // alignments (each layout.maxAlign already folds its schema floor).
    // HIGH-1(b): glibc computes each thread's block base as an
    // alignUp(..., p_align)-adjusted address — the link-time tpoffs
    // below are only valid when tdataVa ≡ 0 (mod tlsAlign). tdataOff is
    // alignUp(page-aligned ptLoad2Start, tlsAlign), so that holds
    // whenever tlsAlign ≤ pageAlign; a stricter-than-page TLS alignment
    // has no shipped producer (alignas caps at 256) but would silently
    // break the congruence — fail loud instead.
    std::uint64_t tlsAlignAcc = 1;
    if (hasTdataDyn) tlsAlignAcc = std::max(tlsAlignAcc, tdataDynLayout.maxAlign);
    if (hasTbssDyn)  tlsAlignAcc = std::max(tlsAlignAcc, tbssDynLayout.maxAlign);
    std::uint64_t const tlsAlign = tlsAlignAcc;
    if (hasTls && tlsAlign > pageAlign) {
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             std::format("elf::encodeElfExecDynamic: TLS block alignment "
                         "{} exceeds the page alignment {} — .tdata's VA "
                         "(alignUp of a page-aligned segment start) would "
                         "no longer be congruent to 0 mod p_align, and "
                         "glibc's per-thread block placement would shift "
                         "every link-time tpoff. No shipped producer emits "
                         "this; refusing to emit a silently-misaligned TLS "
                         "block (D-CSUBSET-THREAD-LOCAL).",
                         tlsAlign, pageAlign));
        return {};
    }
    std::uint64_t const tdataOff =
        hasTls ? alignUp(ptLoad2Start, tlsAlign) : ptLoad2Start;
    std::uint64_t const tdataVa   = baseImageVa + tdataOff;
    std::uint64_t const tdataSpan = tdataDynLayout.spanSize;  // 0 if none
    // The per-thread block: tdata template bytes, then the tbss zero-fill
    // part at its OWN alignment within the block (HIGH-1(a)).
    std::uint64_t const tbssBlockBase =
        alignUp(tdataSpan,
                std::max<std::uint64_t>(1, hasTbssDyn ? tbssDynLayout.maxAlign
                                                      : 1));
    std::uint64_t const tlsBlockMemsz =
        hasTbssDyn ? tbssBlockBase + tbssDynLayout.spanSize : tdataSpan;
    // HIGH-1(a) — the gcc-witnessed physics: the thread pointer sits at
    // the alignUp(memsz, p_align) boundary (Variant II), NOT at raw
    // memsz. An _Alignas(32) member makes the two differ (memsz 0x10 →
    // aligned 0x20; tpoff = 4 − 0x20 = −28, not 4 − 0x10).
    std::uint64_t const alignedTlsBlockSize = alignUp(tlsBlockMemsz, tlsAlign);

    // `.data` at the start of PT_LOAD #2 — shifted past `.tdata`'s file
    // bytes when TLS is present (formula unchanged otherwise: the no-TLS
    // byte-identity guarantee).
    std::uint64_t const rwFileCursor =
        hasTls ? tdataOff + tdataSpan : ptLoad2Start;
    std::uint64_t const dataAlignDyn =
        hasDataDyn ? dataDynLayout.maxAlign : 1;
    std::uint64_t const dataOff =
        hasDataDyn ? alignUp(rwFileCursor, dataAlignDyn) : rwFileCursor;
    std::uint64_t const dataVa  = baseImageVa + dataOff;
    std::uint64_t const dataSz  = dataDynLayout.spanSize;

    std::uint64_t const gotOff = alignUp(dataOff + dataSz, 8);
    std::uint64_t const gotVa  = baseImageVa + gotOff;
    std::uint64_t const gotSz  = got.size();

    std::uint64_t const dynamicOff = alignUp(gotOff + gotSz, 8);
    std::uint64_t const dynamicVa  = baseImageVa + dynamicOff;

    // ── (j) Build .plt bytes (now we have VAs)
    // D-LK6-8 closure: per-machine PLT stub emitter dispatches on
    // `machine`. x86_64 emits 6-byte `FF 25 disp32`; ARM64 emits
    // 16-byte ADRP+LDR+BR+NOP. c84: FUNCTION externs only — the stub
    // + GOT slot ordinal is `externSlot[i]` (data externs own no PLT
    // presence at all).
    for (std::size_t i = 0; i < numExterns; ++i) {
        if (module.externImports[i].isData) continue;
        std::size_t const slot = externSlot[i];
        std::uint64_t const stubVa = pltVa + slot * pltStubSize;
        std::uint64_t const slotVa = gotVa + slot * 8;
        std::size_t   const stubOffset = slot * pltStubSize;
        if (!emitPltStub(machine, plt, stubOffset, stubVa, slotVa, reporter)) {
            return {};
        }
    }

    // (k) `.rela.dyn` content is built AFTER the `.bss` layout below
    // (c84): a COPY relocation's r_offset is the data extern's `.bss`
    // copy-slot VA, which does not exist until `.dynamic`'s size (and
    // thus `.bss`'s VA) is known. The SIZE (`relaDynSz` — one 24-byte
    // Elf64_Rela per extern, GLOB_DAT or COPY) was already fixed at
    // layout time above; only the byte CONTENT moves down.
    std::uint32_t const globDatType = globDatTypeFor(machine);
    std::uint32_t const copyType    = copyRelocTypeFor(machine);

    // ── (l) Build .dynamic
    std::vector<std::uint8_t> dynamicSec;
    auto appendDyn = [&](std::uint64_t tag, std::uint64_t val) {
        appendU64LE(dynamicSec, tag);
        appendU64LE(dynamicSec, val);
    };
    // DT_NEEDED per library, [DT_SONAME — dyn, when configured], then
    // the resolution-side metadata, then DF_1_NOW for eager binding,
    // then DT_NULL terminator. The d_ptr entries (STRTAB/SYMTAB/HASH/
    // RELA) carry base-relative VAs on the ET_DYN arm (baseImageVa ==
    // 0); ld.so adds the load bias to every known d_ptr tag.
    for (std::size_t i = 0; i < numLibs; ++i) {
        appendDyn(DT_NEEDED, libNameOff[i]);
    }
    if (sonameOff != 0) {
        appendDyn(DT_SONAME, sonameOff);   // c150 — dyn-only by construction
    }
    appendDyn(DT_STRTAB,  dynstrVa);
    appendDyn(DT_STRSZ,   dynstrSz);
    appendDyn(DT_SYMTAB,  dynsymVa);
    appendDyn(DT_SYMENT,  24);
    appendDyn(DT_HASH,    hashVa);
    // The RELA trio is emitted only when entries exist. The exec arm
    // always has >= 1 (one per extern, externs mandatory there —
    // byte-identical). A dyn image with zero externs AND zero
    // RELATIVE slots legitimately carries no `.rela.dyn` content;
    // a DT_RELA pointing at zero bytes would be dead metadata.
    if (relaDynSz > 0) {
        appendDyn(DT_RELA,    relaDynVa);
        appendDyn(DT_RELASZ,  relaDynSz);
        appendDyn(DT_RELAENT, 24);
    }
    // DF_1_NOW = eager binding (both arms, all shapes). The c151 PIE
    // additionally carries DF_1_PIE — the ET_DYN "this is an
    // executable, not a library" marker (gcc ground truth: FLAGS_1 =
    // NOW PIE on default-PIE output; readelf keys its "(Position-
    // Independent Executable file)" label on it).
    appendDyn(DT_FLAGS_1, isPie ? (DF_1_NOW | DF_1_PIE) : DF_1_NOW);
    appendDyn(DT_NULL,    0);
    std::uint64_t const dynamicSz = dynamicSec.size();

    std::uint64_t const ptLoad2End = dynamicOff + dynamicSz;
    // p_filesz = the FILE-backed span (through .dynamic; `.bss` adds none).
    std::uint64_t const ptLoad2FileSize = ptLoad2End - ptLoad2Start;
    // `.bss` (zero-fill) is the LAST thing in PT_LOAD #2: its VA follows
    // .dynamic, aligned to the bss section alignment, and extends p_memsz
    // beyond p_filesz. No file bytes are emitted for it. D-LK4-DATA-PRODUCER.
    //
    // c84 (D-LK-EXTERN-DATA-IMPORT): the section is the module's own
    // zero-init globals FIRST (the existing bssDynLayout — item offsets
    // unchanged), then one COPY-RELOCATION SLOT per extern DATA import,
    // each at the object's layout-derived alignment. The section exists
    // when EITHER part is non-empty; its alignment is the max of both
    // parts (bssDynLayout.maxAlign already folds the schema floor, even
    // when the module part is empty).
    if (hasBssSection && secBssDyn == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encodeElfExecDynamic: module needs a `.bss` section "
             "(zero-init globals and/or extern-data copy-relocation "
             "slots) but the format declares no 'bss' section row.");
        return {};
    }
    // Copy slots are EXEC-only (c84); the dyn arm's data externs live
    // in the GOT (hasCopySlots is false there), so `bssSpan` stays the
    // module's own zero-init span.
    std::vector<std::uint64_t> copySlotOffset(numExterns, 0);
    std::uint64_t bssSpan = bssDynLayout.spanSize;
    std::uint64_t copyMaxAlign = 1;
    for (std::size_t i = 0; hasCopySlots && i < numExterns; ++i) {
        auto const& ext = module.externImports[i];
        if (!ext.isData) continue;
        bssSpan = alignUp(bssSpan, ext.dataAlignBytes);
        copySlotOffset[i] = bssSpan;
        bssSpan += ext.dataSizeBytes;
        copyMaxAlign = std::max(copyMaxAlign, ext.dataAlignBytes);
    }
    std::uint64_t const bssAlignDyn =
        hasBssSection ? std::max(bssDynLayout.maxAlign, copyMaxAlign) : 1;
    std::uint64_t const bssVa =
        hasBssSection ? alignUp(dynamicVa + dynamicSz, bssAlignDyn) : 0;
    std::uint64_t const bssSz = bssSpan;
    // p_memsz = the in-memory span: through `.bss` when present, else == filesz.
    std::uint64_t const ptLoad2MemSize =
        hasBssSection ? ((bssVa + bssSz) - ptLoad2VaStart)
                      : ptLoad2FileSize;

    // ── (k, moved) Build the EXTERN half of `.rela.dyn` — one
    // Elf64_Rela per extern:
    //   * function → GLOB_DAT against its GOT slot (both arms);
    //   * data, exec arm → COPY against its `.bss` copy slot (c84);
    //   * data, dyn arm  → GLOB_DAT against its GOT slot (c150
    //     got-indirect — ld.so writes the object's address there).
    // The dyn arm's RELATIVE half is assembled AFTER
    // applyDataItemRelocations (its addends are the base-relative
    // slot values that apply writes); the two halves concatenate —
    // RELATIVE first (the gcc/glibc convention) — into `relaDyn`
    // below, sized against relaDynSz.
    std::vector<std::uint8_t> relaExtern;
    relaExtern.reserve(numExterns * 24);
    for (std::size_t i = 0; i < numExterns; ++i) {
        bool const isData = module.externImports[i].isData;
        std::uint64_t const rOffset =
            isData ? (isDyn ? gotVa + (numFuncExterns + externSlot[i]) * 8
                            : bssVa + copySlotOffset[i])
                   : gotVa + externSlot[i] * 8;
        std::uint32_t const rType =
            (isData && !isDyn) ? copyType : globDatType;
        std::uint64_t const rInfo =
            (static_cast<std::uint64_t>(dynsymIdx[i]) << 32)
            | static_cast<std::uint64_t>(rType);
        appendU64LE(relaExtern, rOffset);
        appendU64LE(relaExtern, rInfo);
        appendI64LE(relaExtern, 0);
    }

    // Non-loaded .symtab / .strtab / .shstrtab + SHT.
    std::vector<std::uint8_t> symtab;
    StringTable strtab;
    auto appendSymtabEntry = [&](std::uint32_t nameOff,
                                  std::uint8_t info,
                                  std::uint16_t shndx,
                                  std::uint64_t value,
                                  std::uint64_t size) {
        appendU32LE(symtab, nameOff);
        appendU8(symtab, info);
        appendU8(symtab, 0);
        appendU16LE(symtab, shndx);
        appendU64LE(symtab, value);
        appendU64LE(symtab, size);
    };
    appendSymtabEntry(0, 0, 0, 0, 0);                  // STN_UNDEF
    // shndx = IDX_TEXT (c150 — computed, not the pre-dyn literal 2:
    // the ET_DYN image has no `.interp`, shifting `.text` to 1).
    appendSymtabEntry(0, makeStInfo(STB_LOCAL, STT_SECTION),
                      IDX_TEXT, 0, 0);
    std::uint32_t const firstNonLocal = 2;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto const& fn = module.functions[i];
        std::string const name =
            std::string{"sym_"} + std::to_string(fn.symbol.v);
        std::uint32_t const nameOff = strtab.add(name);
        appendSymtabEntry(nameOff,
                          makeStInfo(STB_GLOBAL, STT_FUNC),
                          IDX_TEXT,
                          textVa + funcTextStart[i],
                          fn.bytes.size());
    }

    StringTable shstrtab;
    // `.interp` name on every execve'd image — exec + the c151 PIE
    // (the `.so` has no PT_INTERP / `.interp` row — gated like the
    // tdata/tbss adds below, so its shstrtab carries no dead name
    // bytes).
    std::uint32_t const shsInterp =
        isExecveImage ? shstrtab.add(".interp") : 0u;
    auto const shsText     = shstrtab.add(".text");
    // `.rodata` section name (schema row when present, else the literal).
    // Added unconditionally to shstrtab (a few unused bytes when no rodata);
    // the SHT entry below is what's gated on `hasRodataDyn`.
    auto const shsRodata   = shstrtab.add(
        secRodataDyn != nullptr ? std::string{secRodataDyn->name} : std::string{".rodata"});
    // `.data` / `.bss` names from the schema rows when present (D-LK4-DATA-
    // PRODUCER) — added unconditionally (a few unused bytes when absent); the
    // SHT entries below are gated on hasDataDyn / hasBssDyn.
    auto const shsData     = shstrtab.add(
        secDataDyn != nullptr ? std::string{secDataDyn->name} : std::string{".data"});
    auto const shsBss      = shstrtab.add(
        secBssDyn != nullptr ? std::string{secBssDyn->name} : std::string{".bss"});
    // `.tdata` / `.tbss` names (D-CSUBSET-THREAD-LOCAL) — added ONLY when
    // present, unlike the unconditional .data/.bss adds above: those
    // predate this cycle and are baked into the no-data baseline, while a
    // new unconditional add would grow .shstrtab on EVERY image and break
    // the no-TLS byte-identity guarantee (the sqlite-dormant claim).
    // secTdataDyn/secTbssDyn are non-null here (the mandatory row guards
    // above failed loud otherwise).
    std::uint32_t const shsTdata =
        hasTdataDyn ? shstrtab.add(std::string{secTdataDyn->name}) : 0u;
    std::uint32_t const shsTbss =
        hasTbssDyn ? shstrtab.add(std::string{secTbssDyn->name}) : 0u;
    auto const shsPlt      = shstrtab.add(".plt");
    auto const shsDynsym   = shstrtab.add(".dynsym");
    auto const shsDynstr   = shstrtab.add(".dynstr");
    auto const shsHash     = shstrtab.add(".hash");
    auto const shsRelaDyn  = shstrtab.add(".rela.dyn");
    auto const shsGot      = shstrtab.add(".got");
    auto const shsDynamic  = shstrtab.add(".dynamic");
    auto const shsSymtab   = shstrtab.add(".symtab");
    auto const shsStrtab   = shstrtab.add(".strtab");
    auto const shsShStrtab = shstrtab.add(".shstrtab");

    std::uint64_t const symtabOff = alignUp(ptLoad2End, 8);
    std::uint64_t const symtabSz  = symtab.size();
    std::uint64_t const strtabOff = symtabOff + symtabSz;
    std::uint64_t const strtabSz  = strtab.size();
    std::uint64_t const shstrtabOff = strtabOff + strtabSz;
    std::uint64_t const shstrtabSz  = shstrtab.size();
    std::uint64_t const shtOff = alignUp(shstrtabOff + shstrtabSz, 8);

    // ── (m) Apply intra-module + extern relocations.
    //
    // Pre-flight: every reloc kind must be declared on the format
    // schema (symmetric with ET_REL + PE — silent-failure C3
    // convergence: kernel only checks target-schema row).
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (fmt.relocationByKind(rel.kind) == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"elf::encodeElfExecDynamic: kind "} +
                     std::to_string(rel.kind.v) +
                     " not declared by ELF format '" +
                     std::string{fmt.name()} +
                     "' -- substrate-invariant violation.");
                return {};
            }
            // D-LK-DYN-TEXT-ABS-RELOC (c150): a slid ET_DYN image
            // cannot carry an ABSOLUTE fixup in `.text` — the page is
            // read-only at load, so patching it would need DT_TEXTREL
            // (deprecated; rejected by hardened loaders), and the
            // link-time value would be wrong under any nonzero slide
            // anyway. Slide-safe kinds: Linear pc-relative (rel32 /
            // riprel32) and every non-Linear instruction formula
            // (the ARM64 page-pair/branch arms — pc-relative by
            // construction despite their pcRelative=false rows).
            // DSS codegen reaches globals rip-relatively, so the
            // shipped pipeline never trips this; it is the belt for
            // an absolute-in-code producer (incl. the tls-tpoff32
            // Linear-absolute kind — TLS-in-.so is already rejected
            // above, D-LK-DYN-TLS-MODEL).
            if (isDyn) {
                auto const* triPre = targetSchema.relocationInfo(rel.kind);
                if (triPre != nullptr
                    && triPre->formulaKind == RelocFormulaKind::Linear
                    && !triPre->pcRelative) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format(
                             "elf::encodeElfExecDynamic (ET_DYN): function "
                             "SymbolId={{ {} }} carries an ABSOLUTE "
                             "relocation (kind {} '{}') in `.text` -- a "
                             "loader-slid shared library cannot patch "
                             "read-only code pages (DT_TEXTREL). Code must "
                             "reach targets pc-relatively "
                             "(D-LK-DYN-TEXT-ABS-RELOC).",
                             fn.symbol.v, rel.kind.v, triPre->name));
                    return {};
                }
            }
        }
    }
    // symbolVa[intra] = textVa + funcOffset; symbolVa[extern] =
    // PLT stub VA. The shared kernel patches REL32 calls
    // (call rel32) → PLT stub for externs, → other function for
    // intra-module. PLT stub then jumps through GOT to resolved fn.
    // emplace().second checked so an extern SymbolId that collides
    // with an intra-module function symbol fails loud (silent-
    // failure C2 convergence — same defect exists in PE walker
    // and is anchored for parallel fold).
    // symbolVa holds one entry per function / extern / data item — an
    // ABSOLUTE VA for everything EXCEPT thread-local items, whose entry is
    // the SIGNED thread-pointer offset bit-cast to u64 (the symbolVa-reuse
    // trick — see addTlsSymbolOffsets; `tlsSymbols` records which entries
    // are tpoffs so the CRIT-1 cross-check below can police every use).
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size() + numExterns
                     + module.dataItems.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        symbolVa.emplace(module.functions[i].symbol,
                         textVa + funcTextStart[i]);
    }
    // D-LK1-ELF-EXEC-DATA-SECTIONS + D-LK4-DATA-PRODUCER (dynamic arm): each
    // NAMED data item (rodata / data / bss) joins symbolVa at its section VA +
    // section-relative offset via the SAME shared `addDataSymbolVas` helper, so
    // a `.text` relocation that targets a data SymbolId (the `lea reg,[rip+g]`
    // load/store of a global) resolves through the shared `applyExecRelocations`
    // kernel. A `.bss` global is reloc-addressable by VA just like a file-backed
    // one. Anonymous items are skipped (M1).
    if (hasRodataDyn
        && !link::format::addDataSymbolVas(
               module.dataItems, rodataDynLayout, rodataVa,
               symbolVa, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    if (hasDataDyn
        && !link::format::addDataSymbolVas(
               module.dataItems, dataDynLayout, dataVa,
               symbolVa, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    if (hasBssDyn
        && !link::format::addDataSymbolVas(
               module.dataItems, bssDynLayout, bssVa,
               symbolVa, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    // D-CSUBSET-THREAD-LOCAL (TLS C1): thread-local items join symbolVa
    // with their VARIANT-KEYED tpoff (NOT a VA) via addTlsSymbolOffsets —
    // they must NEVER also pass through addDataSymbolVas (one entry per
    // symbol; the tls-tpoff32 Linear patch reads THIS value). The .tbss
    // call adds tbssBlockBase so both sections index one contiguous
    // per-thread block. tlsIdentity() is engaged here (the hasTls guard
    // above failed loud otherwise).
    std::unordered_set<SymbolId> tlsSymbols;
    if (hasTdataDyn
        && !link::format::addTlsSymbolOffsets(
               module.dataItems, tdataDynLayout, /*blockBaseOffset=*/0,
               alignedTlsBlockSize, tlsAlign, *targetSchema.tlsIdentity(),
               symbolVa, tlsSymbols, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    if (hasTbssDyn
        && !link::format::addTlsSymbolOffsets(
               module.dataItems, tbssDynLayout, tbssBlockBase,
               alignedTlsBlockSize, tlsAlign, *targetSchema.tlsIdentity(),
               symbolVa, tlsSymbols, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    for (std::size_t i = 0; i < numExterns; ++i) {
        // D-LK6-8: a FUNCTION extern's VA points at its PLT stub (per-
        // machine stub size: 6 bytes x86_64 / 16 bytes ARM64). c84: an
        // EXEC data extern's VA is its `.bss` COPY SLOT — module code
        // references the LOCAL copy through the normal GlobalAddr
        // reloc path; the loader fills the slot from the library's
        // object before entry (R_*_COPY, eager DF_1_NOW binding).
        // c150: a DYN data extern's VA is its GOT SLOT (got-indirect —
        // the lowering lea's the slot and derefs; ld.so fills it via
        // GLOB_DAT), NEVER a thunk/stub VA (a data object is not
        // callable — the PE/Mach-O c117/c149 model).
        bool const isData = module.externImports[i].isData;
        std::uint64_t const va =
            isData ? (isDyn ? gotVa + (numFuncExterns + externSlot[i]) * 8
                            : bssVa + copySlotOffset[i])
                   : pltVa + externSlot[i] * pltStubSize;
        auto const [it, inserted] = symbolVa.emplace(
            module.externImports[i].symbol, va);
        if (!inserted) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"elf::encodeElfExecDynamic: extern "
                             "symbol #"} +
                 std::to_string(module.externImports[i].symbol.v) +
                 " ('" + module.externImports[i].mangledName +
                 "') collides with an intra-module function symbol "
                 "— an extern declared as a local definition would "
                 "silently bypass dyld resolution.");
            return {};
        }
    }
    // D-CSUBSET-COMPUTED-GOTO: synthetic per-block symbols (the `&&label`
    // block-address `lea`s) get their interior-block VAs before relocation
    // resolution — sectionVa = textVa, the SAME base as the function
    // symbols above (block VA = funcVA + blockOffset). The shared helper is
    // identical across ELF/PE/Mach-O.
    if (!link::format::addInteriorBlockSymbolVas(
            module, funcTextStart, textVa, symbolVa,
            "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }

    // ── ★ D-CSUBSET-THREAD-LOCAL walker backstop (audit fold CRIT-1) ──
    // symbolVa is now tpoff-poisoned for `tlsSymbols` members: their
    // entries are SIGNED thread-pointer offsets bit-cast to u64, only
    // meaningful under a tls-flagged relocation kind. Police EVERY use
    // BEFORE any patch is written:
    //
    // (a) NO data-item relocation may target a TLS symbol. A data slot
    //     holds a link-time-constant ADDRESS; a thread-local object has
    //     no such address (its address is tp-relative, one per thread) —
    //     C11 6.6p9 excludes thread storage duration from address
    //     constants. The semantic tier already rejects `&tls_var` in a
    //     static initializer (S_ThreadLocalAddressNotConstant, 0xE048);
    //     this is the walker-tier belt: without it,
    //     applyDataItemRelocations would write the bit-cast NEGATIVE
    //     tpoff verbatim as an abs64 "address" — a silent garbage
    //     pointer.
    //
    // (b) Function relocations must agree with the target row's `tls`
    //     flag BOTH ways: a non-tls kind against a TLS symbol would
    //     embed the bit-cast tpoff as an address; a tls kind against a
    //     non-TLS symbol would embed a VA as a tpoff. Either direction
    //     is the silent-garbage class; both fail loud.
    //
    // Diagnostic code: K_RelocationKindMismatch — the walker's
    // established "this relocation cannot be applied as declared" code;
    // the defect is a reloc-kind↔target-storage-class disagreement, not
    // a missing format capability (0x8015 would mis-blame the format,
    // which DOES support TLS here).
    if (!tlsSymbols.empty()) {
        for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
            for (auto const& rel : module.dataItems[i].relocations) {
                if (tlsSymbols.contains(rel.target)) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format(
                             "elf::encodeElfExecDynamic: data item #{} "
                             "(SymbolId={{ {} }}) carries a relocation "
                             "targeting THREAD-LOCAL symbol #{} — the "
                             "address of a thread-local object is not a "
                             "link-time constant (C11 6.6p9; the semantic "
                             "tier rejects this as 0xE048 "
                             "S_ThreadLocalAddressNotConstant). Patching "
                             "it would embed the bit-cast thread-pointer "
                             "offset as a garbage pointer "
                             "(D-CSUBSET-THREAD-LOCAL, CRIT-1).",
                             i, module.dataItems[i].symbol.v,
                             rel.target.v));
                    return {};
                }
            }
        }
    }
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            auto const* tri = targetSchema.relocationInfo(rel.kind);
            bool const relocIsTls = tri != nullptr && tri->tls;
            bool const targetIsTls = tlsSymbols.contains(rel.target);
            if (relocIsTls != targetIsTls) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format(
                         "elf::encodeElfExecDynamic: function SymbolId={{ "
                         "{} }} relocation (kind {}{}) {} symbol #{} — a "
                         "tls-flagged relocation carries a thread-pointer "
                         "OFFSET and a non-tls one an ADDRESS; mixing "
                         "them embeds the wrong value class silently "
                         "(D-CSUBSET-THREAD-LOCAL, CRIT-1).",
                         fn.symbol.v, rel.kind.v,
                         tri != nullptr
                             ? std::format(" '{}'", tri->name)
                             : std::string{},
                         targetIsTls
                             ? "is not tls-flagged but targets THREAD-LOCAL"
                             : "is tls-flagged but targets NON-thread-local",
                         rel.target.v));
                return {};
            }
        }
    }

    // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): patch each MUTABLE symbol-address
    // global pointer's abs64 data→data reloc IN PLACE with the target's resolved
    // VA (symbolVa is fully built now). ELF ET_EXEC is in-place-final — no
    // `.rela` rows (the VAs are absolute at link time); siteVasOut=nullptr.
    // ET_DYN (c150): the SAME apply writes the BASE-RELATIVE value (baseImageVa
    // == 0, so symbolVa's entries ARE base-relative) and ALSO collects every
    // patched 8-byte site into `relativeSiteVas` — each becomes one
    // R_*_RELATIVE row below whose r_addend equals the slot's value; glibc's
    // ld.so computes load_base + addend into the slot, completing the address.
    // The emission below reads `rodataDynLayout.bytes` / `dataDynLayout.bytes`.
    if (hasRodataDyn
        && !link::format::applyDataItemRelocations(
               rodataDynLayout.bytes, module.dataItems, rodataDynLayout, rodataVa,
               symbolVa, targetSchema, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    std::vector<std::uint64_t> relativeSiteVas;
    if (hasDataDyn
        && !link::format::applyDataItemRelocations(
               dataDynLayout.bytes, module.dataItems, dataDynLayout, dataVa,
               symbolVa, targetSchema, "elf::encodeElfExecDynamic", reporter,
               isDyn ? &relativeSiteVas : nullptr)) {
        return {};
    }
    // ── ET_DYN: assemble `.rela.dyn` = RELATIVE half ++ extern half ──
    // (exec: the extern half alone — RELATIVE stays empty). The
    // RELATIVE addend is READ BACK from the just-patched slot (the
    // apply wrote S + A there; base-relative because baseImageVa==0),
    // so slot bytes and r_addend agree BY CONSTRUCTION — the
    // prelinked-slot convention gcc's ld emits.
    //
    // c150 silent-failure-review CRITICAL fold: a data slot whose reloc
    // targets an EXTERN must NOT take the RELATIVE path. The apply patched
    // it with `symbolVa[extern]` — the GOT SLOT VA (data extern) or the
    // local PLT STUB VA (function extern) — so a RELATIVE row would bake
    // load_base + slot/stub address into the pointer: one indirection off
    // for data (`FILE **pp = &stdout;` pointed at the .so's own GOT slot,
    // witnessed live), and a cross-module identity break for functions
    // (`fp == puts` false in the executable, C11 6.5.9). gcc's shape —
    // emitted here instead — is a SYMBOL-BASED absolute reloc
    // (R_X86_64_64 <dynsym> + rel.addend) with the slot bytes ZEROED;
    // ld.so resolves the symbol across the global scope and writes the
    // real address. One row per site either way, so every count/size
    // invariant below is unchanged. The abs64 native id comes from the
    // reloc's own format row (machine-agnostic — R_AARCH64_ABS64 on an
    // aarch64 dyn schema).
    struct ExternAddrSite {
        std::uint32_t dynsymIdx = 0;
        std::uint32_t nativeId  = 0;
        std::int64_t  addend    = 0;
    };
    std::unordered_map<std::uint64_t, ExternAddrSite> externAddrBySlotVa;
    if (isDyn && hasDataDyn) {
        std::unordered_map<SymbolId, std::size_t> externIdxBySym;
        externIdxBySym.reserve(numExterns);
        for (std::size_t i = 0; i < numExterns; ++i) {
            externIdxBySym.emplace(module.externImports[i].symbol, i);
        }
        for (std::size_t j = 0; j < dataDynLayout.itemIndices.size(); ++j) {
            AssembledData const& di =
                module.dataItems[dataDynLayout.itemIndices[j]];
            for (auto const& rel : di.relocations) {
                auto const extIt = externIdxBySym.find(rel.target);
                if (extIt == externIdxBySym.end()) continue;   // internal
                auto const* fmtReloc = fmt.relocationByKind(rel.kind);
                if (fmtReloc == nullptr) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format(
                             "elf::encodeElfExecDynamic (ET_DYN): data-item "
                             "relocation kind {} targeting extern '{}' is not "
                             "declared by object format '{}' - cannot emit "
                             "the symbol-based absolute reloc.",
                             rel.kind.v,
                             module.externImports[extIt->second].mangledName,
                             fmt.name()));
                    return {};
                }
                std::uint64_t const slotVa = dataVa
                    + dataDynLayout.itemOffsets[j]
                    + static_cast<std::uint64_t>(rel.offset);
                externAddrBySlotVa.insert_or_assign(
                    slotVa, ExternAddrSite{dynsymIdx[extIt->second],
                                           fmtReloc->nativeId, rel.addend});
            }
        }
    }
    std::uint32_t const relativeType = relativeRelocTypeFor(machine);
    std::vector<std::uint8_t> relaDyn;
    relaDyn.reserve(relaDynSz);
    if (isDyn) {
        if (relativeSiteVas.size() != numRelativeRelocs) {
            emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                 std::format("elf::encodeElfExecDynamic (ET_DYN): collected "
                             "{} RELATIVE site(s) but the layout counted {} "
                             "data-item relocation(s) -- DT_RELASZ would "
                             "disagree with the emitted rows.",
                             relativeSiteVas.size(), numRelativeRelocs));
            return {};
        }
        for (std::uint64_t const siteVa : relativeSiteVas) {
            std::uint64_t const slotOff = siteVa - dataVa;
            // Extern-targeted slot: symbol-based row + zeroed slot (the
            // apply's slot/stub VA is UNDONE — the review-fold above).
            if (auto const extSite = externAddrBySlotVa.find(siteVa);
                extSite != externAddrBySlotVa.end()) {
                for (int b = 0; b < 8; ++b) {
                    dataDynLayout.bytes[static_cast<std::size_t>(
                        slotOff + static_cast<std::uint64_t>(b))] = 0;
                }
                appendU64LE(relaDyn, siteVa);
                appendU64LE(relaDyn, makeRelaInfo(extSite->second.dynsymIdx,
                                                  extSite->second.nativeId));
                appendI64LE(relaDyn, extSite->second.addend);
                continue;
            }
            std::uint64_t addend = 0;
            for (int b = 7; b >= 0; --b) {
                addend = (addend << 8)
                       | dataDynLayout.bytes[static_cast<std::size_t>(
                             slotOff + static_cast<std::uint64_t>(b))];
            }
            appendU64LE(relaDyn, siteVa);   // r_offset — the slot itself
            appendU64LE(relaDyn, makeRelaInfo(0, relativeType));
            appendI64LE(relaDyn, static_cast<std::int64_t>(addend));
        }
    }
    appendBytes(relaDyn, relaExtern);
    if (relaDyn.size() != relaDynSz) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             std::format("elf::encodeElfExecDynamic: .rela.dyn content "
                         "({} bytes) disagrees with the laid-out size "
                         "({}) -- the one-Rela-per-extern(+RELATIVE) "
                         "invariant broke; the DT_RELASZ the loader "
                         "reads would be wrong.",
                         relaDyn.size(), relaDynSz));
        return {};
    }
    // D-CSUBSET-THREAD-LOCAL (CRIT-2 second half): patch `.tdata` TEMPLATE
    // slots the same way — a `thread_local char *msg = "hi";` template slot
    // gets the rodata target's ABSOLUTE VA (fixed-base ET_EXEC: the VA is
    // final; every thread's copy starts from the patched template). The
    // CRIT-1 scan above already rejected any TLS-TARGETING reloc, so every
    // reloc reaching this call resolves to a genuine VA.
    if (hasTdataDyn
        && !link::format::applyDataItemRelocations(
               tdataDynLayout.bytes, module.dataItems, tdataDynLayout, tdataVa,
               symbolVa, targetSchema, "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }
    if (!link::format::applyExecRelocations(
            text, module, funcTextStart, symbolVa,
            targetSchema, textVa,
            "elf::encodeElfExecDynamic", reporter)) {
        return {};
    }

    // ── Patch the data externs' dynsym entries (c84, EXEC arm) ──
    // st_value = the `.bss` copy-slot VA; st_shndx = the `.bss`
    // section index — both unknowable at build time (step e). The
    // symbol is thereby a DEFINED OBJECT in this executable: the
    // loader's COPY-reloc lookup skips the exec's own definition to
    // find the library's object (ELF_RTYPE_CLASS_COPY semantics),
    // while every OTHER image binds this name to the exec's copy
    // (interposition) — all references converge on one storage.
    // The ET_DYN arm skips this (hasCopySlots false): its data
    // externs stay plain UNDEF references bound got-indirect.
    for (std::size_t i = 0; hasCopySlots && i < numExterns; ++i) {
        if (!module.externImports[i].isData) continue;
        std::size_t const off =
            static_cast<std::size_t>(dynsymIdx[i]) * 24;
        std::uint64_t const slotVa = bssVa + copySlotOffset[i];
        dynsym[off + 6] = static_cast<std::uint8_t>(IDX_BSS & 0xFF);
        dynsym[off + 7] = static_cast<std::uint8_t>((IDX_BSS >> 8) & 0xFF);
        for (int b = 0; b < 8; ++b) {
            dynsym[off + 8 + b] =
                static_cast<std::uint8_t>((slotVa >> (8 * b)) & 0xFF);
        }
    }

    // ── Patch the ET_DYN exports' dynsym entries (c150) ─────────
    // st_value = the symbol's base-relative VA (baseImageVa == 0, so
    // symbolVa's entry is exactly the value ld.so adds the load base
    // to when resolving); st_shndx = the DEFINING section's index.
    // Placement is re-derived from the same layouts that populated
    // symbolVa (functions → `.text`; data items → the layout whose
    // itemIndices carries them), so value and section can never
    // disagree.
    if (isDyn && !dynExports.empty()) {
        std::unordered_map<std::uint32_t,
                           std::pair<std::uint64_t, std::uint16_t>> place;
        place.reserve(module.functions.size() + module.dataItems.size());
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            place.emplace(module.functions[i].symbol.v,
                          std::make_pair(textVa + funcTextStart[i], IDX_TEXT));
        }
        auto const addLayoutPlaces =
            [&](link::format::ExecDataSectionLayout const& lay,
                std::uint64_t secVa, std::uint16_t secIdx) {
            for (std::size_t j = 0; j < lay.itemIndices.size(); ++j) {
                auto const& di = module.dataItems[lay.itemIndices[j]];
                if (di.symbol == SymbolId{}) continue;   // anonymous
                place.emplace(di.symbol.v,
                              std::make_pair(secVa + lay.itemOffsets[j],
                                             secIdx));
            }
        };
        if (hasRodataDyn) addLayoutPlaces(rodataDynLayout, rodataVa, IDX_RODATA);
        if (hasDataDyn)   addLayoutPlaces(dataDynLayout,   dataVa,   IDX_DATA);
        if (hasBssDyn)    addLayoutPlaces(bssDynLayout,    bssVa,    IDX_BSS);
        for (auto const& ex : dynExports) {
            auto const it = place.find(ex.sym.v);
            if (it == place.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"elf::encodeElfExecDynamic (ET_DYN): "
                                 "export '"} + ex.name
                         + "' (SymbolId #" + std::to_string(ex.sym.v)
                         + ") landed in no emitted section -- the "
                           "export classification (b.7) and the "
                           "section layouts disagree; walker bug.");
                return {};
            }
            std::size_t const off =
                static_cast<std::size_t>(ex.dynsymIdx) * 24;
            std::uint16_t const shndx = it->second.second;
            std::uint64_t const va    = it->second.first;
            dynsym[off + 6] = static_cast<std::uint8_t>(shndx & 0xFF);
            dynsym[off + 7] = static_cast<std::uint8_t>((shndx >> 8) & 0xFF);
            for (int b = 0; b < 8; ++b) {
                dynsym[off + 8 + b] =
                    static_cast<std::uint8_t>((va >> (8 * b)) & 0xFF);
            }
        }
    }

    // ── (n) Emit bytes ────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(shtOff + 13 * 64);

    // Ehdr at [0..64] — fill at end (need shtOff first); start with 64 zero bytes.
    bytes.resize(kEhdrSize, 0);

    // Program Header Table at [64..64+phtSize].
    auto appendPhdrEntry = [&](std::uint32_t pType,
                                std::uint32_t pFlags,
                                std::uint64_t pOffset,
                                std::uint64_t pVaddr,
                                std::uint64_t pFilesz,
                                std::uint64_t pMemsz,
                                std::uint64_t pAlign) {
        appendU32LE(bytes, pType);
        appendU32LE(bytes, pFlags);
        appendU64LE(bytes, pOffset);
        appendU64LE(bytes, pVaddr);
        appendU64LE(bytes, pVaddr);   // p_paddr
        appendU64LE(bytes, pFilesz);
        appendU64LE(bytes, pMemsz);
        appendU64LE(bytes, pAlign);
    };
    // PT_PHDR + PT_INTERP: every execve'd image — exec AND the c151
    // PIE (at base-relative VAs there: baseImageVa == 0, ld.so adds
    // the slide). Only the `.so` carries neither; see the numPhdrs
    // comment above.
    if (isExecveImage) {
        appendPhdrEntry(PT_PHDR,    PF_R,        phtOff,       baseImageVa + phtOff,
                        phtSize,        phtSize,        8);
        appendPhdrEntry(PT_INTERP,  PF_R,        interpOff,    interpVa,
                        interp.size(), interp.size(), 1);
    }
    // PT_LOAD #1 R+X — Ehdr + PHT + [.interp] + .text + .plt + .dynsym
    //                  + .dynstr + .hash + .rela.dyn
    appendPhdrEntry(PT_LOAD,    PF_X | PF_R, 0,            baseImageVa,
                    ptLoad1End,    ptLoad1End,    pageAlign);
    // PT_LOAD #2 R+W — [.tdata] + [.data] + .got + .dynamic + [.bss].
    // p_filesz covers the file-backed sections (INCLUDING the `.tdata`
    // template bytes at the segment head — they must be mapped for the
    // loader to copy them per-thread); p_memsz additionally covers `.bss`
    // (zero-fill, no file bytes) so the loader reserves + zeroes the bss
    // span at load. `.tbss` contributes to NEITHER (D-CSUBSET-THREAD-
    // LOCAL: the per-thread copies live in loader-allocated TLS blocks
    // sized by PT_TLS p_memsz, not in this segment). D-LK4-DATA-PRODUCER.
    appendPhdrEntry(PT_LOAD,    PF_W | PF_R, ptLoad2Start, ptLoad2VaStart,
                    ptLoad2FileSize, ptLoad2MemSize, pageAlign);
    appendPhdrEntry(PT_DYNAMIC, PF_W | PF_R, dynamicOff,   dynamicVa,
                    dynamicSz,     dynamicSz,     8);
    // PT_TLS (D-CSUBSET-THREAD-LOCAL, audit fold HIGH-2) — present ONLY
    // when the module carries thread-local items (numPhdrs bumped to 6
    // above; every no-TLS image stays byte-identical). Points at the
    // `.tdata` template inside PT_LOAD #2: p_filesz = the initialized
    // template bytes, p_memsz = template + tbss block (the loader
    // allocates p_memsz per thread, copies p_filesz, zeroes the rest),
    // p_align = the TLS block alignment the tpoff formulas above assumed.
    if (hasTls) {
        appendPhdrEntry(PT_TLS, PF_R, tdataOff, tdataVa,
                        tdataSpan, tlsBlockMemsz, tlsAlign);
    }

    // Section bodies: pad-to-offset + append per section (simplifier
    // #1 + #4 fold using local padToOffset / appendBytes helpers).
    // `.interp` bytes: execve'd images only — exec + PIE (the `.so`
    // `interp` vector is empty by construction; skip the pad too —
    // nothing sits between the PHT and the page-aligned `.text`
    // there).
    if (isExecveImage) { padToOffset(bytes, interpOff); appendBytes(bytes, interp); }
    padToOffset(bytes, textOff);      appendBytes(bytes, text);
    if (hasRodataDyn) { padToOffset(bytes, rodataOff); appendBytes(bytes, rodataDyn); }
    padToOffset(bytes, pltOff);       appendBytes(bytes, plt);
    padToOffset(bytes, dynsymOff);    appendBytes(bytes, dynsym);
                                       appendBytes(bytes, dynstr);  // align 1
    padToOffset(bytes, hashOff);      appendBytes(bytes, hashSec);
    padToOffset(bytes, relaDynOff);   appendBytes(bytes, relaDyn);
    padToOffset(bytes, ptLoad2Start);                                // PT_LOAD #2 boundary
    // `.tdata` (thread-local template) opens PT_LOAD #2 (D-CSUBSET-THREAD-
    // LOCAL) — its (possibly reloc-patched) bytes precede `.data`'s.
    // `.tbss` emits NO file bytes (zero-fill template extent).
    if (hasTdataDyn) { padToOffset(bytes, tdataOff); appendBytes(bytes, tdataDynLayout.bytes); }
    // `.data` (mutable initialized globals) follows.
    // `.bss` emits NO file bytes (zero-fill) so it is absent from this body
    // pass — its size lives only in the section header + p_memsz.
    if (hasDataDyn) { padToOffset(bytes, dataOff); appendBytes(bytes, dataDynBytes); }
    padToOffset(bytes, gotOff);       appendBytes(bytes, got);
    padToOffset(bytes, dynamicOff);   appendBytes(bytes, dynamicSec);
    padToOffset(bytes, symtabOff);    appendBytes(bytes, symtab);
                                       appendBytes(bytes, strtab.view());
                                       appendBytes(bytes, shstrtab.view());
    padToOffset(bytes, shtOff);

    // ── (o) Section Header Table ──────────────────────────────
    //   0: SHT_NULL, 1: .interp, 2: .text, [.rodata,] .plt,
    //   .dynsym, .dynstr, .hash, .rela.dyn, .got, .dynamic,
    //   .symtab, .strtab, .shstrtab
    //
    // D-LK1-ELF-EXEC-DATA-SECTIONS (dynamic arm): `.rodata`, WHEN present,
    // occupies index 3 (after `.text`@2, before `.plt`) — mirroring the
    // static ET_EXEC arm's `.rodata`@idx2 insertion. The incremental
    // IDX_* computation (hoisted ABOVE the emit step — c84, so the
    // dynsym data-extern patch can stamp st_shndx=IDX_BSS before the
    // body is appended) keeps every cross-reference coherent; the
    // header-table writes below follow the SAME emit order.

    // Designated initializers per `SectionHeader` field — type-design
    // #1 + simplifier #3 fold: 10-arg positional pushShdr lambda
    // dropped (silent u32/u64 swap-bug surface) in favor of named
    // fields at every call site.
    writeSectionHeader(bytes, SectionHeader{});  // SHT_NULL (slot 0)
    // `.interp` row: execve'd images — exec + the c151 PIE (the
    // `.so` header table goes straight from SHT_NULL to `.text`;
    // IDX_* above matched this).
    if (isExecveImage) {
        writeSectionHeader(bytes, SectionHeader{
            .name_offset = shsInterp, .type = SHT_PROGBITS, .flags = SHF_ALLOC,
            .addr = interpVa, .offset = interpOff, .size = interp.size(),
            .addr_align = 1});
    }
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsText, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR,
        .addr = textVa, .offset = textOff, .size = text.size(),
        .addr_align = 16});
    // `.rodata` (SHF_ALLOC, R) — present only when the module carries
    // rodata; folds into the R+X PT_LOAD #1. Read-only data, NO
    // SHF_EXECINSTR / SHF_WRITE.
    if (hasRodataDyn) {
        writeSectionHeader(bytes, SectionHeader{
            .name_offset = shsRodata, .type = SHT_PROGBITS, .flags = SHF_ALLOC,
            .addr = rodataVa, .offset = rodataOff, .size = rodataSz,
            .addr_align = rodataAlignDyn});
    }
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsPlt, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR,
        .addr = pltVa, .offset = pltOff, .size = pltSize,
        .addr_align = 16});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynsym, .type = SHT_DYNSYM, .flags = SHF_ALLOC,
        .addr = dynsymVa, .offset = dynsymOff, .size = dynsymSz,
        .link = IDX_DYNSTR, .info = 1 /*first non-local symtab idx*/,
        .addr_align = 8, .entry_size = 24});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynstr, .type = SHT_STRTAB, .flags = SHF_ALLOC,
        .addr = dynstrVa, .offset = dynstrOff, .size = dynstrSz,
        .addr_align = 1});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsHash, .type = SHT_HASH, .flags = SHF_ALLOC,
        .addr = hashVa, .offset = hashOff, .size = hashSz,
        .link = IDX_DYNSYM, .addr_align = 8, .entry_size = 4});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsRelaDyn, .type = SHT_RELA, .flags = SHF_ALLOC,
        .addr = relaDynVa, .offset = relaDynOff, .size = relaDynSz,
        .link = IDX_DYNSYM, .addr_align = 8, .entry_size = 24});
    // `.tdata` / `.tbss` (D-CSUBSET-THREAD-LOCAL): sh_type / sh_flags read
    // from the SCHEMA ROWS (SHT_PROGBITS / SHT_NOBITS, both
    // SHF_WRITE|SHF_ALLOC|SHF_TLS = 0x403), never hardcoded. `.tbss`'s
    // sh_addr = tdataVa + tdataSpan is NOMINAL — the gcc overlap
    // convention: a NOBITS TLS section occupies no process-image memory
    // (the loader materializes per-thread copies from PT_TLS), so its
    // "address" merely documents the template ordering and may overlap
    // whatever follows; sh_offset likewise points just past the template
    // bytes without consuming file space.
    if (hasTdataDyn) {
        writeSectionHeader(bytes, SectionHeader{
            .name_offset = shsTdata,
            .type = secTdataDyn->type,
            .flags = secTdataDyn->flags,
            .addr = tdataVa, .offset = tdataOff, .size = tdataSpan,
            .addr_align = tdataDynLayout.maxAlign});
    }
    if (hasTbssDyn) {
        writeSectionHeader(bytes, SectionHeader{
            .name_offset = shsTbss,
            .type = secTbssDyn->type,
            .flags = secTbssDyn->flags,
            .addr = tdataVa + tdataSpan, .offset = tdataOff + tdataSpan,
            .size = tbssDynLayout.spanSize,
            .addr_align = tbssDynLayout.maxAlign});
    }
    // `.data` (SHF_ALLOC | SHF_WRITE) — mutable initialized globals, file-
    // backed, in the R+W PT_LOAD #2. sh_type / sh_flags / sh_addralign come
    // from the SCHEMA ROW (NOT hardcoded — `secDataDyn->type` = SHT_PROGBITS,
    // `->flags` = SHF_ALLOC|SHF_WRITE), keeping the writer format-agnostic.
    // D-LK4-DATA-PRODUCER.
    if (hasDataDyn) {
        writeSectionHeader(bytes, SectionHeader{
            .name_offset = shsData,
            .type = secDataDyn->type,
            .flags = secDataDyn->flags,
            .addr = dataVa, .offset = dataOff, .size = dataSz,
            .addr_align = dataAlignDyn});
    }
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsGot, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE,
        .addr = gotVa, .offset = gotOff, .size = gotSz,
        .addr_align = 8, .entry_size = 8});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynamic, .type = SHT_DYNAMIC, .flags = SHF_ALLOC | SHF_WRITE,
        .addr = dynamicVa, .offset = dynamicOff, .size = dynamicSz,
        .link = IDX_DYNSTR, .addr_align = 8, .entry_size = 16});
    // `.bss` (SHT_NOBITS, SHF_ALLOC | SHF_WRITE) — zero-fill mutable globals
    // and/or extern-data COPY-relocation slots (c84). sh_type / sh_flags from
    // the SCHEMA ROW (`secBssDyn->type` = SHT_NOBITS). sh_offset points just
    // past the file-backed data (conventional for NOBITS — no file bytes are
    // consumed); sh_size is the zero-fill memory extent (module globals +
    // copy slots). D-LK4-DATA-PRODUCER + D-LK-EXTERN-DATA-IMPORT.
    if (hasBssSection) {
        writeSectionHeader(bytes, SectionHeader{
            .name_offset = shsBss,
            .type = secBssDyn->type,
            .flags = secBssDyn->flags,
            .addr = bssVa, .offset = ptLoad2End, .size = bssSz,
            .addr_align = bssAlignDyn});
    }
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsSymtab, .type = SHT_SYMTAB,
        .offset = symtabOff, .size = symtabSz,
        .link = IDX_STRTAB, .info = firstNonLocal,
        .addr_align = 8, .entry_size = 24});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsStrtab, .type = SHT_STRTAB,
        .offset = strtabOff, .size = strtabSz, .addr_align = 1});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsShStrtab, .type = SHT_STRTAB,
        .offset = shstrtabOff, .size = shstrtabSz, .addr_align = 1});

    // ── (p) Fill in Elf64_Ehdr ─────────────────────────────────
    //
    // Resolve entry function index from fmt.entryPoint() —
    // symmetric with the existing ET_EXEC arm (silent-failure
    // HIGH-1 fold from LK1 cycle 2; code-reviewer #1 regression
    // catch on LK6 cycle 2b.2 review). Empty entryPoint defaults
    // to functions[0]; non-empty resolves by synthesized
    // `sym_<id>` name today (real-name resolution closes with
    // D-LK1-1 / LK7).
    // D-LK10-ENTRY Slice C audit fold: shared resolver.
    // ET_DYN `.so` (c150): a shared library has NO entry — e_entry =
    // 0 and the resolver is not consulted (nothing downstream reads
    // an entry: the trampoline was never injected — the schema
    // declares no processExit — and ld.so ignores e_entry on
    // DT_NEEDED objects).
    // ET_DYN PIE (c151) + ET_EXEC: the resolver runs. Empty
    // entryPoint defaults to functions[0] — the linker-prepended
    // `_start` trampoline (keyed on processExit presence, the same
    // cluster member as `isPie`). On the PIE the resulting entryVa
    // is BASE-RELATIVE (textVa = pageAlign, baseImageVa == 0); ld.so
    // transfers control to load_base + e_entry — the gcc PIE shape
    // (Entry 0x1040-class values in readelf -h).
    std::uint64_t entryVa = 0;
    if (isExecveImage) {
        auto const entryIdxOpt = link::format::resolveEntryFnIdx(
            module, fmt, "sym_", "elf::encodeElfExecDynamic", reporter);
        if (!entryIdxOpt.has_value()) return {};
        entryVa = textVa + funcTextStart[*entryIdxOpt];
    }
    std::vector<std::uint8_t> ehdr;
    ehdr.reserve(kEhdrSize);
    ehdr.push_back(0x7F); ehdr.push_back('E');
    ehdr.push_back('L');  ehdr.push_back('F');
    ehdr.push_back(elfId.fileClass);
    ehdr.push_back(elfId.dataEncoding);
    ehdr.push_back(static_cast<std::uint8_t>(EV_CURRENT));
    ehdr.push_back(elfId.osabi);
    ehdr.push_back(elfId.abiVersion);
    for (int i = 0; i < 7; ++i) ehdr.push_back(0);
    appendU16LE(ehdr, isDyn ? 3 : 2);  // e_type: ET_DYN / ET_EXEC
    appendU16LE(ehdr, elfId.machine);
    appendU32LE(ehdr, EV_CURRENT);
    appendU64LE(ehdr, entryVa);
    appendU64LE(ehdr, phtOff);
    appendU64LE(ehdr, shtOff);
    appendU32LE(ehdr, 0);  // e_flags
    appendU16LE(ehdr, static_cast<std::uint16_t>(kEhdrSize));
    appendU16LE(ehdr, static_cast<std::uint16_t>(kPhdrSize));
    appendU16LE(ehdr, static_cast<std::uint16_t>(numPhdrs));
    appendU16LE(ehdr, 64);  // sizeof(Elf64_Shdr)
    appendU16LE(ehdr, kNumSections);
    appendU16LE(ehdr, IDX_SHSTRTAB);
    std::memcpy(bytes.data(), ehdr.data(), kEhdrSize);

    return bytes;
}

} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    // `targetSchema` is consumed by the ET_EXEC reloc-application
    // path (LK6 cycle 1) — the structured formula on each
    // `TargetRelocationInfo` (pcRelative + addendBias + widthBytes)
    // tells the walker how to compute and write patches. ET_REL
    // mode emits Rela records and doesn't need it.

    auto const& fmt = objectFormatSchema;
    if (fmt.kind() != ObjectFormatKind::Elf) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"elf::encode called with non-ELF format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }
    // c150 + c151 (D-LK1-4): ET_DYN routes to the dynamic-image
    // walker UNCONDITIONALLY — both sub-shapes need `.dynamic` /
    // `.dynsym` / `.hash` even with zero extern imports (a `.so` for
    // its EXPORTS; a PIE for its loader metadata), so the exec arm's
    // externs-only entry condition below does not apply. The walker
    // discriminates `.so` vs PIE internally by the schema's entry
    // cluster (`isPie` — processExit presence; validate() pinned the
    // cluster all-or-none, and the walker re-checks the two members
    // it consumes). The machine guard mirrors the exec arm's (the
    // PLT/GLOB_DAT emitters are per-machine).
    if (fmt.elf().objectType == ElfObjectType::Dyn) {
        std::uint16_t const elfMachine = fmt.elf().machine;
        if (elfMachine != kEmX86_64 && elfMachine != kEmAArch64) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encode: ET_DYN output but ELF "
                             "e_machine="}
                     + std::to_string(elfMachine)
                     + " has no PLT/GLOB_DAT emitter yet. Supported "
                       "machines: x86_64 (62), ARM64 (183) -- add the "
                       "per-machine arms (pltStubSizeFor / "
                       "globDatTypeFor / relativeRelocTypeFor / "
                       "emitPltStub) for a new ISA.");
            return {};
        }
        auto const* secTextDyn =
            requireSection(fmt, SectionKind::Text, "ELF dynamic writer",
                           reporter);
        if (!secTextDyn) return {};
        return encodeElfExecDynamic(module, targetSchema, fmt,
                                     *secTextDyn, reporter);
    }
    // ELF dynamic-linker import-table emission — substrate landed
    // at LK6 cycle 2b.1 (PT_INTERP path field); walker emission
    // closed at LK6 cycle 2b.2 (this dispatch — D-LK6-4 closed).
    // When the module carries externImports, route to
    // `encodeElfExecDynamic` for the full image with .interp +
    // .dynsym + .dynstr + .hash + .rela.dyn + .plt + .got +
    // .dynamic + PT_INTERP + PT_DYNAMIC + writable PT_LOAD #2.
    // Eager binding (DF_1_NOW + R_X86_64_GLOB_DAT) — lazy binding
    // is anchored separately (architect Obs 2 future cycle).
    if (!module.externImports.empty()) {
        bool const isExecEarly =
            (fmt.elf().objectType == ElfObjectType::Exec);
        if (!isExecEarly) {
            // D-LK-OBJECT-EXTERN-CALL-RELOCATABLE: an ET_REL object CAN carry
            // extern imports IF the format declares an `externCallDispatch` —
            // the externs become SHN_UNDEF `.symtab` entries + `.rela.text`
            // relocs that the FINAL (foreign) linker resolves (it synthesizes
            // the PLT/GOT), NOT DSS. A format with NO `externCallDispatch`
            // still rejects (extern calls have no defined relocation form). On
            // the dispatch-present path we FALL THROUGH to the normal ET_REL
            // writer below — the exec-only dynamic-image emission is skipped.
            if (!fmt.externCallDispatch().has_value()) {
                emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                     std::string{"elf::encode: extern imports present ("}
                         + std::to_string(module.externImports.size())
                         + " entries) but the ET_REL format declares no "
                           "`externCallDispatch` - extern calls have no "
                           "defined relocation form. Declare it (e.g. "
                           "\"direct-plt\") to emit undefined-extern symbols "
                           "+ relocations for the final linker.");
                return {};
            }
            // D-LK-OBJECT-DATA-EXTERN-RELOCATABLE (c144): BOTH function-call
            // and DATA externs are emitted in a relocatable object. A function
            // `call` → SHN_UNDEF symbol + PLT32 reloc; a DATA reference (e.g.
            // sqlite `out = stdout`) → the SAME SHN_UNDEF symbol + a plain PC32
            // reloc (the .rela.text loop below excludes data from
            // externCallTargets, so it emits nativeId/PC32, never
            // pltNativeId/PLT32 — a data symbol bound through a PLT stub would
            // read jump-stub bytes as the object's value). This is EXACTLY what
            // gcc emits for `extern FILE *stdout` in a `.o`: a NOTYPE UND
            // symbol + R_X86_64_PC32, even under default-PIE — the FINAL linker
            // binds it by copy-relocation when the `.o` links into an
            // EXECUTABLE (the DSS `.o` consumer today: sqlite's testfixture).
            // The one case a data extern would instead need a GOT-indirect
            // binding (R_X86_64_GOTPCREL) — the `.o` linked into a SHARED
            // LIBRARY — is NOT a silent miscompile: ld itself fails loud
            // ("relocation R_X86_64_PC32 against undefined symbol `stdout' can
            // not be used when making a shared object; recompile with -fPIC").
            // A `.o`→`.so` consumer + a got-indirect data binding for the
            // relocatable ELF format is the pinned future trigger. So both
            // extern kinds FALL THROUGH to the normal ET_REL writer below.
        } else {
            if (fmt.elf().interpreter.empty()) {
                emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                     std::string{"elf::encode: extern imports present ("}
                         + std::to_string(module.externImports.size())
                         + " entries) but `elf.interpreter` is empty — "
                           "declare a PT_INTERP path on the schema (e.g. "
                           "'/lib64/ld-linux-x86-64.so.2') for a loadable "
                           "dynamic image.");
                return {};
            }
            // Machine-dispatch guard (D-LK6-8 closed 2026-06-01).
            // `encodeElfExecDynamic` now dispatches per-machine to
            // `emitPltStub` + `globDatTypeFor` + `pltStubSizeFor`.
            // Supported: x86_64 (62), ARM64 (183). Other machines fail
            // loud — adding RISC-V (243) / PPC64 (21) / MIPS (8) means
            // adding the per-machine arms in this file (see top-level
            // comment near `kEmX86_64` / `kEmAArch64`).
            std::uint16_t const elfMachine = fmt.elf().machine;
            if (elfMachine != kEmX86_64 && elfMachine != kEmAArch64) {
                emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                     std::string{"elf::encode: extern imports present but "}
                         + "ELF e_machine=" + std::to_string(elfMachine)
                         + " has no PLT stub emitter yet. Supported "
                           "machines: x86_64 (62), ARM64 (183). Other "
                           "ISAs (RISC-V 243, PPC64 21, MIPS 8) are "
                           "anchored as future work — add a row to "
                           "pltStubSizeFor / globDatTypeFor / emitPltStub.");
                return {};
            }
            // Section schema lookup mirrors the existing path so the
            // dynamic helper inherits the same K_NoMatchingObjectFormat
            // guard.
            auto const* secTextDyn =
                requireSection(fmt, SectionKind::Text, "ELF dynamic writer",
                               reporter);
            if (!secTextDyn) return {};
            return encodeElfExecDynamic(module, targetSchema, fmt,
                                         *secTextDyn, reporter);
        }
    }

    // Route between ET_REL and ET_EXEC based on the schema's
    // declared objectType. ET_REL keeps its .rela.text-bearing
    // layout; ET_EXEC applies relocations in-place to `.text`
    // (LK6 cycle 1 — see `applyExecRelocations` below) and adds a
    // PT_LOAD program header.
    bool const isExec = (fmt.elf().objectType == ElfObjectType::Exec);

    // Resolve the section schema rows we need. ET_REL requires
    // RelocTable (.rela.text); ET_EXEC doesn't (relocs are either
    // applied or rejected). Both modes share text/symtab/strtab/
    // shstrtab.
    auto const* secText      = requireSection(fmt, SectionKind::Text,      "ELF writer", reporter);
    auto const* secRela      = isExec ? nullptr
                                       : requireSection(fmt, SectionKind::RelocTable, "ELF writer", reporter);
    auto const* secSymtab    = requireSection(fmt, SectionKind::Symtab,    "ELF writer", reporter);
    auto const* secStrtab    = requireSection(fmt, SectionKind::Strtab,    "ELF writer", reporter);
    auto const* secShStrtab  = requireSection(fmt, SectionKind::ShStrtab,  "ELF writer", reporter);
    if (!secText || (!isExec && !secRela) || !secSymtab || !secStrtab || !secShStrtab) {
        return {};
    }

    // ── Build .rodata bytes from AssembledData.dataItems ───────────
    //
    // D-LK1-ELF-EXEC-DATA-SECTIONS (rodata-only scope): the ET_EXEC
    // walker emits a loadable `.rodata` section when the module
    // carries any `AssembledData` item with `section ==
    // DataSectionKind::Rodata`. Mirrors the PE walker's `.rdata` arm
    // (pe.cpp:655-714) precisely: per-item bytes are placed at the
    // item's `Alignment` (padding zero-filled); the section's VA is
    // computed contiguously after `.text`. This rodata is folded
    // into the SAME R+X `.text` PT_LOAD (SHF_ALLOC only — strictly
    // less permissive than writable; W^X preserved).
    //
    // Discipline pin (mirrors the PE fold): the non-Rodata fail-loud
    // below runs UNCONDITIONALLY across `module.dataItems` (NOT
    // guarded by a hasRodata bool), so a module carrying ONLY
    // Data/Bss items cannot slip past the gate via a dropped scan.
    // Data/Bss arms remain anchored under D-LK4-RODATA-PRODUCER.
    // A data item carrying its OWN relocations (data→data references —
    // a vtable / pointer table) is deferred this cycle: this writer
    // does not yet patch dataItem relocations into `.rodata`
    // (D-LK1-ELF-RODATA-DATAITEM-RELOC). `int g=42;` produces neither.
    using link::format::detail::alignUp;
    // ── Validate + lay out `.rodata` via the shared exec data-section
    // substrate (`exec_data_section.hpp`) — the SAME helper the Mach-O
    // `__const` writer uses, so the per-item validation (Rodata-only +
    // no-data->data-relocs) + the byte layout + the H1 section-align
    // raise are single-sourced (no copy in two walkers). The
    // `writerName` carries the ET_EXEC/ET_REL qualifier so the format-
    // neutral diagnostics still pinpoint the ELF form. The validation
    // runs for BOTH forms (it precedes the `!isExec` reject below), so
    // a module carrying ONLY Data/Bss items cannot slip past.
    // D-LK1-ELF-EXEC-DATA-SECTIONS.
    std::string const elfDataWriterName =
        isExec ? "elf::encode (ET_EXEC)" : "elf::encode (ET_REL)";
    // D-CSUBSET-THREAD-LOCAL (audit fold LOW-b): thread-local items are
    // handled ONLY by the DYNAMIC walker arm (encodeElfExecDynamic — PT_TLS
    // + tpoff symbolVa). This static ET_EXEC / ET_REL arm has no TLS block
    // emission; laying a Tdata/Tbss item out here would silently produce a
    // process-shared alias (and no PT_TLS for the loader). Unreachable via
    // the shipped pipeline (DSS ELF exes always import libc `exit` → the
    // dynamic arm; the linker's acceptsDataSection gate fires first for
    // non-opted-in formats) — this is the anti-static-alias belt for a
    // hand-built module or a future format JSON opting in prematurely.
    // A freestanding no-libc profile would land TLS here (the thread_local
    // arc design's fork C: it needs DSS-side arch_prctl synthesis, since
    // no ld.so runs to process PT_TLS).
    for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
        auto const  s = module.dataItems[i].section;
        if (s != DataSectionKind::Tdata && s != DataSectionKind::Tbss)
            continue;
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             std::format("{}: AssembledData item #{} is thread-local ({}) "
                         "but the static ELF arm emits no PT_TLS/TLS "
                         "block — thread-locals are supported only on the "
                         "ELF DYNAMIC arm (D-CSUBSET-THREAD-LOCAL; a "
                         "no-libc static-exec profile is a future fork of "
                         "that arc).",
                         elfDataWriterName, i, dataSectionKindName(s)));
        return {};
    }
    // Floor = the schema's section addrAlign when the row exists (peeked
    // WITHOUT a diagnostic; the mandatory fail-loud `requireSection` on the
    // exec path below is what enforces the row).
    ObjectFormatSectionInfo const* secRodataPeek =
        fmt.sectionByKind(SectionKind::Rodata);
    ObjectFormatSectionInfo const* secDataPeek =
        fmt.sectionByKind(SectionKind::Data);
    ObjectFormatSectionInfo const* secBssPeek =
        fmt.sectionByKind(SectionKind::Bss);
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): the relro (relocated-read-only
    // const) section peek — the ET_REL `.data.rel.ro` row (present on the `.o`
    // schema; absent on the exec schema, where relro rides `.data`).
    ObjectFormatSectionInfo const* secRelRoPeek =
        fmt.sectionByKind(SectionKind::RelRoConst);
    std::uint64_t const rodataAlignFloor =
        secRodataPeek != nullptr ? secRodataPeek->addrAlign : 1;
    std::uint64_t const dataAlignFloor =
        secDataPeek != nullptr ? secDataPeek->addrAlign : 1;
    std::uint64_t const bssAlignFloor =
        secBssPeek != nullptr ? secBssPeek->addrAlign : 1;
    std::uint64_t const relroAlignFloor =
        secRelRoPeek != nullptr ? secRelRoPeek->addrAlign : 1;
    // Lay out each data section kind via the shared kind-parameterized helper
    // (D-LK1-ELF-EXEC-DATA-SECTIONS for rodata; D-LK4-DATA-PRODUCER for the
    // writable `.data` + zero-fill `.bss`; D-LK-RELRO-CONST-DATA-RELOCATABLE for
    // the reloc-bearing const `relro`). `.data` + `relro` ALLOW item relocations
    // (a reloc-bearing MUTABLE pointer global lands in `.data`; a reloc-bearing
    // CONST one in `relro`) — ET_EXEC applies them in place below; ET_REL emits
    // `.rela.data` / `.rela.data.rel.ro`. `.rodata` does NOT allow them (a
    // reloc-bearing rodata item is a producer-contract breach — kept failing loud,
    // the RodataDataItemWithRelocationFailsLoud pin).
    auto const rodataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Rodata, rodataAlignFloor,
        elfDataWriterName, reporter);
    if (!rodataLayoutOpt.has_value()) return {};
    auto const& rodataLayout = *rodataLayoutOpt;
    auto dataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Data, dataAlignFloor,
        elfDataWriterName, reporter, /*allowItemRelocations=*/true);
    if (!dataLayoutOpt.has_value()) return {};
    auto& dataLayout = *dataLayoutOpt;
    auto const bssLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Bss, bssAlignFloor,
        elfDataWriterName, reporter);
    if (!bssLayoutOpt.has_value()) return {};
    auto const& bssLayout = *bssLayoutOpt;
    auto relroLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::RelRoConst, relroAlignFloor,
        elfDataWriterName, reporter, /*allowItemRelocations=*/true);
    if (!relroLayoutOpt.has_value()) return {};
    auto& relroLayout = *relroLayoutOpt;
    // ET_EXEC: FOLD relro into `.data` (the exec decision — "treat relro like
    // .data"; the loader writes the resolved target VA into each slot, then a
    // foreign linker's GNU_RELRO seals it — a hardening nicety, not required for
    // correctness). This static ET_EXEC arm handles externless modules; the
    // reloc-bearing relro/data items are patched IN PLACE below
    // (applyDataItemRelocations). ET_REL keeps relro a DISTINCT `.data.rel.ro`
    // section + emits `.rela.data.rel.ro` (gcc's contract).
    if (isExec) {
        link::format::mergeFileBackedDataSection(dataLayout, relroLayout);
    }

    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE: data sections are emitted for BOTH
    // ET_EXEC and ET_REL. ET_EXEC binds them into loaded PT_LOAD segments with
    // computed VAs; ET_REL emits them with sh_addr=0 + section-relative data
    // symbols the final linker binds. `buildExecDataSection` (called
    // unconditionally below) already produced the layouts format-blind; the
    // only ET_REL-vs-exec differences are downstream (no VA math, no PT_LOAD,
    // relocs emitted into `.rela.text` rather than applied in place).
    bool const hasRodata = !rodataLayout.empty();
    bool const hasData   = !dataLayout.empty();
    bool const hasBss    = !bssLayout.empty();
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): relro is a SEPARATE `.data.rel.ro`
    // section ONLY in ET_REL. In ET_EXEC it was FOLDED into `.data` above
    // (mergeFileBackedDataSection), so `hasRelRo` stays false there — no separate
    // section, no `.rela.data.rel.ro`; the merged items ride `.data`.
    bool const hasRelRo  = !isExec && !relroLayout.empty();
    // D-LK-OBJECT-EXTERN-SYMBOL-NAMES (secondary): an empty `.note.GNU-stack`
    // marks the ET_REL object's stack as NON-executable, silencing the
    // `ld: missing .note.GNU-stack section implies executable stack` warning
    // when a foreign linker consumes it. Schema-DRIVEN + graceful: emitted
    // only when the format declares a `note` section (so a format opts out by
    // omitting the row — no hard `requireSection`), and only for ET_REL (an
    // executable carries stack policy in a PT_GNU_STACK program header, not a
    // section). `secNote` may be null; the peek never fails loud.
    auto const* secNote = fmt.sectionByKind(SectionKind::Note);
    bool const hasNote  = !isExec && (secNote != nullptr);
    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE: ET_REL now EMITS dataItems (a
    // global → `.rodata`/`.data`/`.bss` + a section-relative `.symtab` symbol +
    // `.rela.text` relocs). The deferred cases still fail loud upstream: an
    // unadvertised section kind (TLS `tdata`/`tbss`) is caught by the linker's
    // `acceptsDataSection` gate + this arm's earlier TLS reject. A data item
    // carrying its OWN relocations (a pointer-initializer) is now SUPPORTED:
    // `.data`/`relro` are laid out with allowItemRelocations=true above; ET_REL
    // emits its `.rela.data` / `.rela.data.rel.ro`, ET_EXEC applies it in place
    // (D-LK-RELRO-CONST-DATA-RELOCATABLE). A reloc-bearing `.rodata` item still
    // fails loud (allow=false — the RodataDataItemWithRelocationFailsLoud pin).
    // On the exec path each PRESENT section's row is MANDATORY — fail loud
    // (the format JSON must declare it). The rows also feed the section-header
    // sh_type / sh_flags / name below (all read from the schema, never hardcoded).
    ObjectFormatSectionInfo const* secRodata =
        hasRodata ? requireSection(fmt, SectionKind::Rodata,
                                   "ELF writer", reporter)
                  : nullptr;
    if (hasRodata && secRodata == nullptr) return {};
    ObjectFormatSectionInfo const* secData =
        hasData ? requireSection(fmt, SectionKind::Data, "ELF writer", reporter)
                : nullptr;
    if (hasData && secData == nullptr) return {};
    ObjectFormatSectionInfo const* secBss =
        hasBss ? requireSection(fmt, SectionKind::Bss, "ELF writer", reporter)
               : nullptr;
    if (hasBss && secBss == nullptr) return {};
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): the `.data.rel.ro` row is
    // MANDATORY when an ET_REL object carries relro items (the `.o` schema
    // declares it). ET_EXEC merged relro into `.data`, so hasRelRo is false and
    // this peek is skipped there.
    ObjectFormatSectionInfo const* secRelRo =
        hasRelRo ? requireSection(fmt, SectionKind::RelRoConst, "ELF writer",
                                  reporter)
                 : nullptr;
    if (hasRelRo && secRelRo == nullptr) return {};
    std::uint64_t const rodataAlign = hasRodata ? rodataLayout.maxAlign : 1;
    std::vector<std::uint8_t> const& rodataBytes = rodataLayout.bytes;

    // ── Build .text + per-function symbols ─────────────────────
    //
    // Concatenate every AssembledFunction's bytes into one .text
    // section, recording each function's start offset for its
    // symbol's `st_value`.
    std::vector<std::uint8_t> text;
    struct FuncSymRecord {
        SymbolId     symId{};
        std::uint64_t valueInText = 0;
        std::uint64_t size = 0;
    };
    std::vector<FuncSymRecord> funcSyms;
    funcSyms.reserve(module.functions.size());

    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());

    for (auto const& fn : module.functions) {
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
        funcSyms.push_back({fn.symbol, start,
                            static_cast<std::uint64_t>(fn.bytes.size())});
    }

    // .rodata runtime VA — contiguous after `.text`, rounded up to
    // the rodata section's addralign (D-LK1-ELF-EXEC-DATA-SECTIONS).
    // Computed HERE (now that `text.size()` is known) so it is in
    // scope for BOTH the symbolVa map below AND the file-layout pass
    // further down. The file-offset congruence (rodata-from-text on
    // disk == rodata-from-text in VA) is asserted at layout time.
    std::uint64_t const rodataSectionVa =
        hasRodata
            ? secText->virtualAddress + alignUp(text.size(), rodataAlign)
            : 0;

    // Writable data segment (R+W PT_LOAD #2) for `.data` + `.bss` — D-LK4-
    // DATA-PRODUCER. Kept in a SEPARATE page-aligned segment from the R+X
    // text/rodata so mutable globals never share a page with executable code
    // (W^X). The segment starts one page above the end of the read-only
    // sections' VA span; `.data` (file-backed) first, then `.bss` (zero-fill,
    // memsz-only) last. VAs are congruent with file offsets (delta == the
    // read-only span rounded to a page) — the layout pass below asserts it.
    std::uint64_t const pageAlignStatic = fmt.elf().pageAlign;
    std::uint64_t const dataAlign = hasData ? dataLayout.maxAlign : 1;
    std::uint64_t const bssAlign  = hasBss ? bssLayout.maxAlign : 1;
    std::uint64_t const dataSize  = dataLayout.spanSize;
    std::uint64_t const bssSize   = bssLayout.spanSize;
    bool const hasWritableSeg = hasData || hasBss;
    // End of the read-only VA span (text + optional rodata).
    std::uint64_t const roSpanEndVa =
        hasRodata ? rodataSectionVa + rodataBytes.size()
                  : secText->virtualAddress + text.size();
    std::uint64_t const writableSegVa =
        hasWritableSeg ? alignUp(roSpanEndVa, pageAlignStatic) : 0;
    std::uint64_t const dataSectionVa =
        hasData ? alignUp(writableSegVa, dataAlign) : 0;
    std::uint64_t const bssSectionVa =
        hasBss ? alignUp((hasData ? dataSectionVa + dataSize : writableSegVa),
                         bssAlign)
               : 0;

    // ── ET_EXEC: apply intra-module relocations in-place ───────
    //
    // Delegated to the shared `applyExecRelocations` kernel in
    // `link/format/exec_reloc_apply.hpp` — same helper consumed by
    // PE PE32+ and Mach-O MH_EXECUTE walkers. Format-specific input
    // is the `sectionVa` (ELF: `secText->virtualAddress`) and the
    // diagnostic prefix.
    if (isExec) {
        // Build absolute symbol-VA map: for every function, its
        // runtime VA is `secText->virtualAddress + offsetInText`.
        // ELF cycle 2a has no extern imports (anchored D-LK6-4);
        // when externs land, they extend this same map with GOT
        // / PLT slot VAs.
        std::unordered_map<SymbolId, std::uint64_t> symbolVa;
        symbolVa.reserve(module.functions.size()
                         + module.dataItems.size());
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            symbolVa.emplace(module.functions[i].symbol,
                             secText->virtualAddress + funcTextStart[i]);
        }
        // D-LK1-ELF-EXEC-DATA-SECTIONS: each NAMED rodata
        // `AssembledData` item joins the symbolVa map at
        // `rodataSectionVa + rodataItemOffsets[i]` via the shared
        // `addDataSymbolVas` substrate (the SAME helper the Mach-O
        // `__const` writer uses). A `.text` relocation that targets a
        // rodata SymbolId (the code's `lea reg, [rip + g]`) now
        // resolves through the SAME shared `applyExecRelocations`
        // kernel — no rodata loop is added to that kernel; it stays
        // functions-only. Anonymous `SymbolId{}` items are skipped (M1
        // — offset-referenced, never reloc targets). A rodata SymbolId
        // colliding with a function SymbolId is a caller bug
        // (REDEFINITION); the helper emits K_DuplicateDataSymbol.
        if (hasRodata
            && !link::format::addDataSymbolVas(
                   module.dataItems, rodataLayout, rodataSectionVa,
                   symbolVa, "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
        // D-LK4-DATA-PRODUCER: `.data` + `.bss` globals join symbolVa at their
        // section VA so a `.text` load/store reloc resolves to the writable VA.
        if (hasData
            && !link::format::addDataSymbolVas(
                   module.dataItems, dataLayout, dataSectionVa,
                   symbolVa, "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
        if (hasBss
            && !link::format::addDataSymbolVas(
                   module.dataItems, bssLayout, bssSectionVa,
                   symbolVa, "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
        // D-CSUBSET-COMPUTED-GOTO: synthetic per-block symbols get their
        // interior-block VAs before relocation resolution — sectionVa =
        // secText->virtualAddress, the SAME base as the function symbols
        // above. This static ET_EXEC path handles externless modules (the
        // dynamic path requires non-empty externImports); the dynamic path
        // calls the same helper.
        if (!link::format::addInteriorBlockSymbolVas(
                module, funcTextStart, secText->virtualAddress, symbolVa,
                "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
        if (!link::format::applyExecRelocations(
                text, module, funcTextStart, symbolVa,
                targetSchema, secText->virtualAddress,
                "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
        // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): `.data` now carries reloc-
        // bearing items — a MUTABLE pointer global's abs64, and the relro CONST
        // pointers FOLDED in above. Patch each in place with its target's
        // absolute VA via the SAME shared kernel the dynamic arm + PE/Mach-O use.
        // (`.rodata` carries none — allow=false; `.bss` is zero-fill.) A static
        // ET_EXEC is non-PIE with resolved VAs, so there is no `.rela`/base-reloc
        // table — `siteVasOut` is nullptr.
        if (hasData
            && !link::format::applyDataItemRelocations(
                   dataLayout.bytes, module.dataItems, dataLayout,
                   dataSectionVa, symbolVa, targetSchema,
                   "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
    }

    // ── Build .strtab + .symtab ────────────────────────────────
    //
    // Symbol layout: STN_UNDEF (idx 0) → STT_SECTION for .text
    // (LOCAL) → defined function symbols (GLOBAL) → defined DATA symbols
    // (GLOBAL, STT_OBJECT, D-LK-OBJECT-DATA-SECTION-RELOCATABLE) → undefined
    // extern symbols (GLOBAL, SHN_UNDEF). `.symtab.sh_info` = index of first
    // non-LOCAL symbol.
    //
    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE: the data-section header indices,
    // computed HERE (before the symtab) so a data symbol's `st_shndx` names its
    // section. Data sections sit right after `.text`(1) in rodata→data→relro→bss
    // order — the SAME order the `nextIdxS()` cursor + the `headers` push use
    // below — so each index is a running count from 2. (Kept in lockstep with
    // that cursor; the golden ET_REL byte tests pin the resulting layout.)
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): `.data.rel.ro` slots between
    // `.data` and `.bss` (hasRelRo is ET_REL-only — ET_EXEC merged it into
    // `.data`, so these indices are unshifted there).
    std::uint16_t const IDX_RODATA = hasRodata ? 2u : 0u;
    std::uint16_t const IDX_DATA   =
        hasData ? static_cast<std::uint16_t>(2u + (hasRodata ? 1u : 0u)) : 0u;
    std::uint16_t const IDX_RELRO  =
        hasRelRo ? static_cast<std::uint16_t>(
                       2u + (hasRodata ? 1u : 0u) + (hasData ? 1u : 0u))
                 : 0u;
    std::uint16_t const IDX_BSS    =
        hasBss ? static_cast<std::uint16_t>(
                     2u + (hasRodata ? 1u : 0u) + (hasData ? 1u : 0u)
                     + (hasRelRo ? 1u : 0u))
               : 0u;

    StringTable strtab;
    std::vector<std::uint8_t> symtab;

    // Real source-level C names for the externally-visible defined functions
    // (D-LK-OBJECT-EXTERN-SYMBOL-NAMES) AND the undefined extern references
    // (D-LK-OBJECT-EXTERN-CALL-RELOCATABLE), so a foreign linker resolves both
    // by name; `sym_<id>` fallback for static/local/synthesized symbols. Built
    // once from `module` for O(1) per-symbol lookup below.
    link::format::ObjectSymbolNames const objNames{module};

    // Helper: emit one Elf64_Sym record (24 bytes).
    auto appendSym = [&](std::uint32_t nameOff, std::uint8_t info,
                          std::uint8_t other, std::uint16_t shndx,
                          std::uint64_t value, std::uint64_t size) {
        appendU32LE(symtab, nameOff);
        appendU8(symtab, info);
        appendU8(symtab, other);
        appendU16LE(symtab, shndx);
        appendU64LE(symtab, value);
        appendU64LE(symtab, size);
    };

    // Symbol index 0: STN_UNDEF (24 zero bytes).
    appendSym(0, 0, 0, 0, 0, 0);

    // Index 1: STT_SECTION for .text — the relocation base most
    // ELF consumers expect for section-relative references. Its
    // st_shndx points at the .text section index; we pin that to
    // IDX_TEXT (=1) by the section ordering below.
    appendSym(0, makeStInfo(STB_LOCAL, STT_SECTION),
              0, /*shndx=.text*/ 1, 0, 0);

    // Map each SymbolId to its symtab index (for relocs).
    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;

    // D-CSUBSET-COMPUTED-GOTO: synthetic per-block symbols (the `&&label`
    // block-address `lea` relocation sources) are intra-module DEFINED
    // LOCAL symbols pointing at an interior `.text` offset. In ET_REL they
    // need a real `.symtab` entry — STB_LOCAL, STT_NOTYPE, st_shndx=.text,
    // st_value = the block's byte offset within `.text` (funcTextStart[fi]
    // + blockByteOffset). They MUST precede the GLOBAL function symbols
    // (ELF requires all LOCAL symbols first; `.symtab.sh_info` = the first
    // non-LOCAL index), and registering them here makes the `.rela.text`
    // loop resolve the block-address relocation to this defined LOCAL
    // rather than the SHN_UNDEF extern-fallback below (which would emit a
    // bogus undefined-global and break the object at final link). The
    // ET_EXEC arm resolves these in-place via `addInteriorBlockSymbolVas`
    // and emits no `.rela.text`/extern symbols, so this block is
    // ET_REL-only by being inside the symtab/rela construction the
    // ET_EXEC arm shares — but a block symbol can only arise on a function
    // that took a block address, and the extern fallback below is
    // `!isExec`-gated, so an ET_EXEC build never reaches the mis-handling.
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        for (auto const& bs : module.functions[fi].blockSymbols) {
            std::string const symName =
                std::string{"sym_"} + std::to_string(bs.symbol.v);
            std::uint32_t const nameOff = strtab.add(symName);
            std::uint32_t const idx =
                static_cast<std::uint32_t>(symtab.size() / 24);
            appendSym(nameOff, makeStInfo(STB_LOCAL, STT_NOTYPE), 0,
                      /*shndx=.text*/ 1,
                      funcTextStart[fi] + bs.blockByteOffset, 0);
            symIdxBySymbol.emplace(bs.symbol, idx);
        }
    }
    // `.symtab.sh_info` = index of the first non-LOCAL symbol = the count
    // of the LOCAL prefix (UNDEF + STT_SECTION + every block symbol).
    std::uint32_t const firstNonLocalSymIdx =
        static_cast<std::uint32_t>(symtab.size() / 24);

    // Defined function symbols (GLOBAL + STT_FUNC + shndx=.text). In an
    // ET_REL object an externally-visible symbol gets its real C name
    // (D-LK-OBJECT-EXTERN-SYMBOL-NAMES) so a foreign linker resolves it; a
    // static/local one keeps `sym_<id>` (binding stays GLOBAL — the `may stay
    // internal` name carve-out). ET_EXEC keeps the synthesized `sym_<id>`
    // form UNCHANGED — its entry-point resolution matches the schema's
    // `entryPoint` string against that reconstructed name (D-LK1-1), and no
    // foreign toolchain ever re-links a DSS executable, so real names there
    // are an unfired, separate concern.
    for (auto const& f : funcSyms) {
        std::string const symName =
            isExec ? std::string{"sym_"} + std::to_string(f.symId.v)
                   : objNames.definedName(f.symId, "sym_");
        std::uint32_t const nameOff = strtab.add(symName);
        std::uint32_t const idx =
            static_cast<std::uint32_t>(symtab.size() / 24);
        appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_FUNC), 0,
                  /*shndx=.text*/ 1, f.valueInText, f.size);
        symIdxBySymbol.emplace(f.symId, idx);
    }

    // Defined DATA symbols (GLOBAL + STT_OBJECT, SECTION-RELATIVE) — ET_REL
    // only (D-LK-OBJECT-DATA-SECTION-RELOCATABLE). A global lands in
    // `.rodata`/`.data`/`.bss`; its symtab entry names the section (st_shndx)
    // + section-relative offset (st_value) + size the FINAL linker binds, so a
    // `.text`→global reloc resolves to a DEFINED symbol rather than being
    // misclassified as an undefined extern by the loop below. Externally-
    // visible → real name; static → `sym_<id>` (same carve-out as functions).
    // ET_EXEC resolves data symbols via `addDataSymbolVas` (absolute VAs) and
    // emits none into this symtab. MUST precede the extern-fallback loop.
    if (!isExec) {
        auto emitDataSyms =
            [&](link::format::ExecDataSectionLayout const& layout,
                std::uint16_t sectionIdx) {
                for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
                    AssembledData const& di =
                        module.dataItems[layout.itemIndices[j]];
                    if (di.symbol == SymbolId{}) continue;   // anonymous item
                    // A data global's SymbolId must be unique. A collision with
                    // an already-emitted (function / block / data) symbol is a
                    // producer-contract breach — fail loud (symmetry with the
                    // exec `addDataSymbolVas` K_DuplicateDataSymbol), never a
                    // silent skip that would bind a `.text`→data reloc to the
                    // WRONG symbol.
                    if (symIdxBySymbol.contains(di.symbol)) {
                        emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                             "elf::encode (ET_REL): data SymbolId={ "
                                 + std::to_string(di.symbol.v)
                                 + " } collides with an already-emitted symbol "
                                   "- a data global's SymbolId must be unique.");
                        continue;
                    }
                    std::string const symName =
                        objNames.definedName(di.symbol, "sym_");
                    std::uint32_t const nameOff = strtab.add(symName);
                    std::uint32_t const idx =
                        static_cast<std::uint32_t>(symtab.size() / 24);
                    appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_OBJECT), 0,
                              sectionIdx, layout.itemOffsets[j],
                              di.sizeInSection());
                    symIdxBySymbol.emplace(di.symbol, idx);
                }
            };
        if (hasRodata) emitDataSyms(rodataLayout, IDX_RODATA);
        if (hasData)   emitDataSyms(dataLayout, IDX_DATA);
        if (hasRelRo)  emitDataSyms(relroLayout, IDX_RELRO);   // c145
        if (hasBss)    emitDataSyms(bssLayout, IDX_BSS);
    }

    // Undefined extern symbols referenced by any relocation but not
    // defined by any function. ET_EXEC has no extern symbols at this
    // point — the cycle-1 reloc-application pass above failed loud on
    // any unresolved target (FFI / dynamic linking is LK6 cycle 2).
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): scan DATA-ITEM relocations too
    // — a relro/`.data` const pointer table whose target is an UNDEFINED extern
    // (`int *const p = &extern_var;`) needs an SHN_UNDEF `.symtab` entry so the
    // `.rela.data.rel.ro` / `.rela.data` emission below resolves its symIdx (the
    // final linker binds it). Only `.data`/relro items carry relocs here (a
    // reloc-bearing `.rodata` item already failed `buildExecDataSection`).
    if (!isExec) {
        auto emitExternForReloc = [&](Relocation const& rel) {
            if (symIdxBySymbol.contains(rel.target)) return;
            // Real import name (D-LK-OBJECT-EXTERN-CALL-RELOCATABLE) so a
            // foreign linker resolves the extern; `sym_<id>` fallback for a
            // reloc target that is neither defined nor a known import.
            std::string const symName =
                objNames.externName(rel.target, "sym_");
            std::uint32_t const nameOff = strtab.add(symName);
            std::uint32_t const idx =
                static_cast<std::uint32_t>(symtab.size() / 24);
            appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_NOTYPE), 0,
                      SHN_UNDEF, 0, 0);
            symIdxBySymbol.emplace(rel.target, idx);
        };
        for (auto const& fn : module.functions)
            for (auto const& rel : fn.relocations) emitExternForReloc(rel);
        for (auto const& di : module.dataItems)
            for (auto const& rel : di.relocations) emitExternForReloc(rel);
    }

    // ── Build .rela.text ───────────────────────────────────────
    //
    // For each AssembledFunction's relocations, compute the absolute
    // offset within .text (the function's range start + the local
    // offset) and translate `kind → nativeId` via the format schema.
    // ET_EXEC applies relocations in-place (above) and emits no
    // .rela.text — the build loop is skipped entirely.

    std::vector<std::uint8_t> relaText;
    if (!isExec) {
        // D-LK-OBJECT-EXTERN-CALL-RELOCATABLE: undefined-extern FUNCTION
        // targets (built once) — a rel32 CALL to one emits the PLT-capable
        // reloc variant so a foreign PIE link resolves it through a
        // linker-built PLT. D-LK-OBJECT-DATA-EXTERN-RELOCATABLE (c144): DATA
        // externs are EXCLUDED — a data reference is not a call and must emit
        // plain PC32 (the copy-relocation shape gcc emits for `extern FILE
        // *stdout`), never PLT32; PLT32 would bind the data symbol to a
        // linker-built PLT stub and the code would read jump-stub bytes as
        // the object's value (the silent miscompile the image-path
        // K_FormatLacksImportSupport reject guards against, here prevented in
        // the relocatable writer).
        std::unordered_set<SymbolId> externCallTargets;
        externCallTargets.reserve(module.externImports.size());
        for (auto const& e : module.externImports) {
            if (e.isData) continue;
            externCallTargets.insert(e.symbol);
        }
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            auto const& fn = module.functions[fi];
            std::uint64_t const fnStart = funcTextStart[fi];
            for (auto const& rel : fn.relocations) {
                auto const* fmtReloc = fmt.relocationByKind(rel.kind);
                if (fmtReloc == nullptr) {
                    // Should have been caught by `link()` substrate
                    // but re-check defensively — silent skip is
                    // exactly the failure class the substrate
                    // discipline rejects.
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         "elf::encode: relocation kind "
                             + std::to_string(rel.kind.v)
                             + " not declared by ELF format '"
                             + std::string{fmt.name()} + "'");
                    continue;
                }
                auto it = symIdxBySymbol.find(rel.target);
                if (it == symIdxBySymbol.end()) {
                    emit(reporter, DiagnosticCode::K_SymbolUndefined,
                         "elf::encode: relocation target symbol #"
                             + std::to_string(rel.target.v)
                             + " has no symtab entry");
                    continue;
                }
                // D-LK-OBJECT-RELOC-ADDEND-CROSSTOOLCHAIN: the ELF RELA
                // r_addend a FOREIGN linker (gcc's ld) reads is the FULL psABI
                // implicit addend. DSS splits that as `Relocation::addend` +
                // the target schema's `addendBias` (applied together only by
                // DSS's OWN in-place exec applier, `applyExecRelocations`,
                // which never emits `.rela`). A `.o` is consumed exclusively by
                // an external linker (DSS re-links AssembledModules, never its
                // own emitted `.o`), so we must BAKE the bias in here: for a
                // `call`/`lea` rel32 (R_X86_64_PC32, addendBias=-4) → r_addend
                // -4 (a rel32 field is relative to the instruction END, 4 bytes
                // past the reloc offset), matching exactly what gcc emits; for
                // abs64/abs32 (addendBias=0) → r_addend unchanged. Without this
                // gcc resolved a call to sym+4 (mid-instruction) → SIGSEGV.
                auto const* triReloc = targetSchema.relocationInfo(rel.kind);
                if (triReloc == nullptr) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         "elf::encode: relocation kind "
                             + std::to_string(rel.kind.v)
                             + " has no TargetRelocationInfo on target schema '"
                             + std::string{targetSchema.name()}
                             + "' - cannot compute the psABI r_addend.");
                    continue;
                }
                std::uint32_t const symIdx = it->second;
                // D-LK-OBJECT-EXTERN-CALL-RELOCATABLE: a rel32 CALL to an
                // UNDEFINED extern emits the PLT-capable variant (PLT32) so a
                // foreign PIE link routes it through a linker-built PLT — a bare
                // PC32 against an undefined symbol errors under -pie. Same
                // S+A-P formula + -4 addend; intra-module (defined) call
                // targets keep plain PC32 (pltNativeId is used only when the
                // target is an extern FUNCTION import — data externs are
                // excluded from externCallTargets and keep plain PC32).
                std::uint32_t const emittedNativeId =
                    (fmtReloc->pltNativeId != 0
                     && externCallTargets.contains(rel.target))
                        ? fmtReloc->pltNativeId
                        : fmtReloc->nativeId;
                std::uint64_t const rOffset = fnStart + rel.offset;
                appendU64LE(relaText, rOffset);
                appendU64LE(relaText, makeRelaInfo(symIdx, emittedNativeId));
                appendI64LE(relaText, rel.addend + triReloc->addendBias);
            }
        }
    }

    // ── Build .rela.data / .rela.data.rel.ro ───────────────────
    //
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): a DATA section whose items carry
    // their OWN relocations (a const/mutable pointer table — sqlite's VFS method
    // tables + `aSyscall[]`) gets a `.rela.<section>` mirroring `.rela.text`. Each
    // item's relocations emit at `r_offset = itemSectionOffset + rel.offset`
    // against the item's target `.symtab` entry. UNLIKE `.rela.text`, a data
    // relocation is NEVER a call, so it always emits the plain `nativeId` (never
    // the PLT variant — a data slot bound through a PLT stub would read jump-stub
    // bytes as the pointer's value). r_addend bakes the psABI bias
    // (D-LK-OBJECT-RELOC-ADDEND-CROSSTOOLCHAIN) exactly like `.rela.text`; for an
    // abs64 pointer (addendBias 0) it is the item's stored addend. ET_REL-only.
    auto buildDataRela =
        [&](link::format::ExecDataSectionLayout const& layout)
        -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> rela;
        for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
            AssembledData const& di = module.dataItems[layout.itemIndices[j]];
            std::uint64_t const itemOff = layout.itemOffsets[j];
            for (auto const& rel : di.relocations) {
                auto const* fmtReloc = fmt.relocationByKind(rel.kind);
                if (fmtReloc == nullptr) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         "elf::encode: data relocation kind "
                             + std::to_string(rel.kind.v)
                             + " not declared by ELF format '"
                             + std::string{fmt.name()} + "'");
                    continue;
                }
                auto const it = symIdxBySymbol.find(rel.target);
                if (it == symIdxBySymbol.end()) {
                    emit(reporter, DiagnosticCode::K_SymbolUndefined,
                         "elf::encode: data relocation target symbol #"
                             + std::to_string(rel.target.v)
                             + " has no symtab entry");
                    continue;
                }
                auto const* triReloc = targetSchema.relocationInfo(rel.kind);
                if (triReloc == nullptr) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         "elf::encode: data relocation kind "
                             + std::to_string(rel.kind.v)
                             + " has no TargetRelocationInfo on target schema '"
                             + std::string{targetSchema.name()} + "'");
                    continue;
                }
                appendU64LE(rela, itemOff + rel.offset);
                appendU64LE(rela, makeRelaInfo(it->second, fmtReloc->nativeId));
                appendI64LE(rela, rel.addend + triReloc->addendBias);
            }
        }
        return rela;
    };
    // `.rela.data` for reloc-bearing MUTABLE `.data` items; `.rela.data.rel.ro`
    // for the const relro items. Both ET_REL-only (on the exec path relro was
    // merged into `.data` + applied in place, so these layouts hold no relocs).
    std::vector<std::uint8_t> relaData;
    std::vector<std::uint8_t> relaRelRo;
    if (!isExec) {
        if (hasData)  relaData  = buildDataRela(dataLayout);
        if (hasRelRo) relaRelRo = buildDataRela(relroLayout);
    }
    bool const hasRelaData  = !relaData.empty();
    bool const hasRelaRelRo = !relaRelRo.empty();

    // ── Section ordering + .shstrtab ───────────────────────────
    //
    // ET_REL order: SHT_NULL, .text, .rela.text, .symtab, .strtab,
    // .shstrtab, [.note.GNU-stack]. `.note.GNU-stack` (when the schema
    // declares it) is appended LAST — after .shstrtab — so every existing
    // section index and e_shstrndx stay put (a zero-size marker's position
    // is irrelevant; e_shstrndx names .shstrtab explicitly). ET_EXEC drops
    // `.rela.text` entirely (no SHT_NULL placeholder): intra-module
    // relocations were applied in-place to `.text` by `applyExecRelocations`
    // above (LK6 cycle 1, closes D-LK1-3); extern relocs (FFI / dynamic
    // linking) are anchored at D-LK6-2 and don't reach this point.
    StringTable shstrtab;
    SectionHeader hNull{};
    SectionHeader hText{};
    SectionHeader hRodata{};
    SectionHeader hData{};
    SectionHeader hBss{};
    SectionHeader hRelRo{};        // c145: .data.rel.ro (ET_REL)
    SectionHeader hRela{};
    SectionHeader hRelaData{};     // c145: .rela.data (ET_REL)
    SectionHeader hRelaRelRo{};    // c145: .rela.data.rel.ro (ET_REL)
    SectionHeader hSymtab{};
    SectionHeader hStrtab{};
    SectionHeader hShStrtab{};
    SectionHeader hNote{};
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): derive the `.rela.<section>` names
    // for the data relocation tables. ELF names an SHT_RELA table the format's
    // rela-prefix + the section it applies to; recover the prefix from the
    // `.rela.text` row (secRela) by stripping the `.text` suffix — schema-driven,
    // no hardcoded `.rela` literal (a `.rel`/SHT_REL format derives `.rel` the
    // same way). ET_REL only (secRela is null on the exec path).
    std::string relaDataName;
    std::string relaRelRoName;
    if (secRela != nullptr) {
        std::string_view relaPrefix{secRela->name};
        std::string_view const textName{secText->name};
        if (relaPrefix.size() > textName.size()
            && relaPrefix.substr(relaPrefix.size() - textName.size())
                   == textName) {
            relaPrefix =
                relaPrefix.substr(0, relaPrefix.size() - textName.size());
        }
        if (hasData)
            relaDataName = std::string{relaPrefix} + std::string{secData->name};
        if (hasRelRo)
            relaRelRoName =
                std::string{relaPrefix} + std::string{secRelRo->name};
    }
    hText.name_offset      = shstrtab.add(secText->name);
    if (hasRodata) {
        hRodata.name_offset = shstrtab.add(secRodata->name);
    }
    if (hasData) {
        hData.name_offset  = shstrtab.add(secData->name);
    }
    if (hasRelRo) {
        hRelRo.name_offset = shstrtab.add(secRelRo->name);
    }
    if (hasBss) {
        hBss.name_offset   = shstrtab.add(secBss->name);
    }
    if (secRela != nullptr) {
        hRela.name_offset  = shstrtab.add(secRela->name);
    }
    if (hasRelaData) {
        hRelaData.name_offset = shstrtab.add(relaDataName);
    }
    if (hasRelaRelRo) {
        hRelaRelRo.name_offset = shstrtab.add(relaRelRoName);
    }
    hSymtab.name_offset    = shstrtab.add(secSymtab->name);
    hStrtab.name_offset    = shstrtab.add(secStrtab->name);
    hShStrtab.name_offset  = shstrtab.add(secShStrtab->name);
    if (hasNote) {
        hNote.name_offset  = shstrtab.add(secNote->name);
    }

    // Section indices — IDX_TEXT==1 is pinned (the STT_SECTION sym
    // emitted above hardcodes st_shndx=1). Other indices depend on which
    // optional sections are present. Since D-LK-OBJECT-DATA-SECTION-
    // RELOCATABLE, data sections appear on BOTH forms (ET_REL now carries
    // data + `.rela.text` together):
    //   ET_REL (no data):     Null(0), Text(1), Rela(2), Symtab(3), Strtab(4), ShStrtab(5)[, Note(6)].
    //   ET_REL (+ .data):     Null(0), Text(1), Data(2), Rela(3), Symtab(4), Strtab(5), ShStrtab(6)[, Note(7)].
    //   ET_EXEC (no rodata):  Null(0), Text(1),          Symtab(2), Strtab(3), ShStrtab(4).
    //   ET_EXEC (+ rodata):   Null(0), Text(1), Rodata(2), Symtab(3), Strtab(4), ShStrtab(5).
    // The phantom SHT_NULL placeholder in ET_EXEC was an LK1-cycle-2
    // first draft; architect convergence pulled it out so `readelf
    // -S` doesn't show a blank slot at idx 2 and the index math is
    // honest. `[.rodata]/[.data]/[.bss]` sit at index 2.. (in that order)
    // when present, shifting the trailing sections; `.note.GNU-stack` (ET_REL)
    // is appended LAST.
    constexpr std::uint16_t IDX_TEXT     = 1;
    // Trailing-section indices computed INCREMENTALLY from the emit order so the
    // optional sections ([.rodata]/[.data]/[.bss], then [.rela.text] on ET_REL)
    // each shift the rest coherently without a hand-maintained per-section
    // constant. Emit order (BOTH forms):
    //   0 NULL, 1 .text, [.rodata], [.data], [.bss], [.rela.text (ET_REL)],
    //   .symtab, .strtab, .shstrtab, [.note.GNU-stack (ET_REL, last)].
    // The data indices themselves were captured EARLY (IDX_RODATA/IDX_DATA/
    // IDX_BSS, before the symtab) for the data symbols' st_shndx; here the
    // cursor only advances past them.
    std::uint16_t idxCursorS = 2;  // after NULL(0) + .text(1)
    auto nextIdxS = [&]() { return idxCursorS++; };
    // ORDER MUST MATCH the `headers` push order below:
    //   NULL, .text, [.rodata], [.data], [.bss], [.rela.text (ET_REL)],
    //   .symtab, .strtab, .shstrtab, [.note.GNU-stack].
    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE: data sections are bumped BEFORE
    // `.rela.text` (fixing the prior cursor, which bumped `.rela.text` first —
    // harmless while data was exec-only + rela was ET_REL-only [mutually
    // exclusive], but WRONG now that an ET_REL object carries BOTH). Their
    // indices were already captured as IDX_RODATA/IDX_DATA/IDX_BSS earlier (the
    // data-symtab loop needs them before this point); here we only advance the
    // cursor so IDX_SYMTAB/IDX_STRTAB/IDX_SHSTRTAB land after them.
    if (hasRodata) { (void)nextIdxS(); }
    if (hasData)   { (void)nextIdxS(); }
    if (hasRelRo)  { (void)nextIdxS(); }           // .data.rel.ro (ET_REL) c145
    if (hasBss)    { (void)nextIdxS(); }
    if (!isExec) { (void)nextIdxS(); }            // .rela.text slot (ET_REL)
    // c145: `.rela.data` / `.rela.data.rel.ro` follow `.rela.text` (ET_REL only,
    // each present only when its data section carries reloc-bearing items).
    if (hasRelaData)  { (void)nextIdxS(); }
    if (hasRelaRelRo) { (void)nextIdxS(); }
    std::uint16_t const IDX_SYMTAB   = nextIdxS();
    std::uint16_t const IDX_STRTAB   = nextIdxS();
    std::uint16_t const IDX_SHSTRTAB = nextIdxS();

    hText.type       = secText->type;
    hText.flags      = secText->flags;
    hText.addr_align = secText->addrAlign;
    hText.entry_size = secText->entrySize;
    hText.size       = text.size();
    // sh_addr — ET_EXEC fills from schema's virtualAddress; ET_REL
    // leaves it 0 (unbound in .o).
    hText.addr       = isExec ? secText->virtualAddress : 0;

    // .rodata section header (D-LK1-ELF-EXEC-DATA-SECTIONS). sh_type
    // / sh_flags / sh_addralign are read from the schema row (NOT
    // hardcoded — `secRodata->flags` is the config sh_flags=SHF_ALLOC).
    // sh_addr = the contiguous-after-.text VA computed above; sh_offset
    // is filled by layoutSection below.
    if (hasRodata) {
        hRodata.type       = secRodata->type;
        hRodata.flags      = secRodata->flags;
        hRodata.addr_align = rodataAlign;
        hRodata.entry_size = secRodata->entrySize;
        hRodata.size       = rodataBytes.size();
        hRodata.addr       = isExec ? rodataSectionVa : 0;  // ET_REL: unbound
    }
    // `.data` / `.bss` section headers (D-LK4-DATA-PRODUCER). sh_type /
    // sh_flags from the SCHEMA ROW (NOT hardcoded — `.data` = SHT_PROGBITS +
    // SHF_ALLOC|SHF_WRITE; `.bss` = SHT_NOBITS + SHF_ALLOC|SHF_WRITE). sh_addr
    // = the writable-segment VA computed above; sh_offset is filled by the
    // layout pass below (`.bss` gets the just-past-data file offset — NOBITS
    // consumes no file bytes).
    if (hasData) {
        hData.type       = secData->type;
        hData.flags      = secData->flags;
        hData.addr_align = dataAlign;
        hData.entry_size = secData->entrySize;
        hData.size       = dataSize;
        hData.addr       = isExec ? dataSectionVa : 0;  // ET_REL: unbound
    }
    if (hasBss) {
        hBss.type        = secBss->type;
        hBss.flags       = secBss->flags;
        hBss.addr_align  = bssAlign;
        hBss.entry_size  = secBss->entrySize;
        hBss.size        = bssSize;
        hBss.addr        = isExec ? bssSectionVa : 0;  // ET_REL: unbound
    }
    // `.data.rel.ro` section header (D-LK-RELRO-CONST-DATA-RELOCATABLE, c145).
    // sh_type / sh_flags from the SCHEMA ROW (SHT_PROGBITS + SHF_ALLOC|SHF_WRITE,
    // like `.data`). ET_REL only — sh_addr=0 (ET_EXEC merged relro into `.data`,
    // so hasRelRo is false there). Its OWN relocations live in `.rela.data.rel.ro`.
    if (hasRelRo) {
        hRelRo.type       = secRelRo->type;
        hRelRo.flags      = secRelRo->flags;
        hRelRo.addr_align = relroLayout.maxAlign;
        hRelRo.entry_size = secRelRo->entrySize;
        hRelRo.size       = relroLayout.bytes.size();
        hRelRo.addr       = 0;  // ET_REL: unbound (never reached with isExec)
    }

    if (secRela != nullptr) {
        hRela.type       = secRela->type;
        hRela.flags      = secRela->flags;
        hRela.addr_align = secRela->addrAlign;
        hRela.entry_size = secRela->entrySize;  // 24 for Elf64_Rela
        hRela.link       = IDX_SYMTAB;
        hRela.info       = IDX_TEXT;
        hRela.size       = relaText.size();
    } else {
        // ET_EXEC: slot remains SHT_NULL (all zeros). Section index
        // stays in the header table to preserve IDX_* parity.
    }
    // `.rela.data` / `.rela.data.rel.ro` (c145): same SHT_RELA type/flags/align/
    // entrysize as `.rela.text` (all read from the secRela schema row — SHT_RELA
    // properties are format-owned), but sh_info names the TARGET data section
    // (IDX_DATA / IDX_RELRO) whose items these relocations patch. sh_link =
    // .symtab. ET_REL only; each present only when its blob is non-empty.
    if (hasRelaData) {
        hRelaData.type       = secRela->type;
        hRelaData.flags      = secRela->flags;
        hRelaData.addr_align = secRela->addrAlign;
        hRelaData.entry_size = secRela->entrySize;
        hRelaData.link       = IDX_SYMTAB;
        hRelaData.info       = IDX_DATA;
        hRelaData.size       = relaData.size();
    }
    if (hasRelaRelRo) {
        hRelaRelRo.type       = secRela->type;
        hRelaRelRo.flags      = secRela->flags;
        hRelaRelRo.addr_align = secRela->addrAlign;
        hRelaRelRo.entry_size = secRela->entrySize;
        hRelaRelRo.link       = IDX_SYMTAB;
        hRelaRelRo.info       = IDX_RELRO;
        hRelaRelRo.size       = relaRelRo.size();
    }

    hSymtab.type       = secSymtab->type;
    hSymtab.flags      = secSymtab->flags;
    hSymtab.addr_align = secSymtab->addrAlign;
    hSymtab.entry_size = secSymtab->entrySize;  // 24 for Elf64_Sym
    hSymtab.link       = IDX_STRTAB;
    hSymtab.info       = firstNonLocalSymIdx;
    hSymtab.size       = symtab.size();

    hStrtab.type       = secStrtab->type;
    hStrtab.flags      = secStrtab->flags;
    hStrtab.addr_align = std::max<std::uint64_t>(1, secStrtab->addrAlign);
    hStrtab.entry_size = secStrtab->entrySize;
    hStrtab.size       = strtab.size();

    hShStrtab.type       = secShStrtab->type;
    hShStrtab.flags      = secShStrtab->flags;
    hShStrtab.addr_align = std::max<std::uint64_t>(1, secShStrtab->addrAlign);
    hShStrtab.entry_size = secShStrtab->entrySize;
    hShStrtab.size       = shstrtab.size();

    // `.note.GNU-stack` — empty (size 0) SHT_PROGBITS with sh_flags=0 (NO
    // SHF_EXECINSTR): its mere presence tells `ld` the object needs no
    // executable stack. type/flags/align all from the schema row (not
    // hardcoded), matching `.section .note.GNU-stack,"",@progbits`.
    if (hasNote) {
        hNote.type       = secNote->type;
        hNote.flags      = secNote->flags;
        hNote.addr_align = std::max<std::uint64_t>(1, secNote->addrAlign);
        hNote.entry_size = secNote->entrySize;
        hNote.size       = 0;
    }

    // ── Layout pass: compute sh_offset for each section ────────
    //
    // ET_REL: [Ehdr] + section bodies + SHT at end.
    // ET_EXEC: [Ehdr] + [PHT (program headers)] + section bodies +
    //          SHT at end. The PHT lives immediately after the Ehdr
    //          so e_phoff = 64 and runtime loaders find it without
    //          a seek.
    constexpr std::uint64_t kEhdrSize = 64;
    constexpr std::uint64_t kProgramHeaderSize = 56;  // Elf64_Phdr
    // PT_LOAD #1 = R+X (.text [+ .rodata]); PT_LOAD #2 = R+W ([.data] [+ .bss])
    // when the module has writable globals (D-LK4-DATA-PRODUCER). W^X: the
    // writable segment is a SEPARATE PT_LOAD so mutable data never shares a page
    // with executable code.
    std::uint64_t const kPtLoadCount = 1 + (hasWritableSeg ? 1u : 0u);
    std::uint64_t const phtSize = isExec ? (kPtLoadCount * kProgramHeaderSize) : 0;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kEhdrSize + phtSize + text.size() + relaText.size()
                  + symtab.size() + strtab.size() + shstrtab.size() + 7 * 64);
    bytes.resize(kEhdrSize + phtSize);  // placeholder; rewritten below

    // Single layout lambda — `vector<uint8_t> const&` decays to
    // `span<uint8_t const>` so both the in-memory section bodies
    // (text / relaText / symtab) and the StringTable views share
    // one code path.
    auto layoutSection = [&](SectionHeader& h, std::span<std::uint8_t const> body) {
        if (h.addr_align > 1) padTo(bytes, h.addr_align);
        h.offset = bytes.size();
        bytes.insert(bytes.end(), body.begin(), body.end());
    };

    // For ET_EXEC: pad .text's file offset up to the PT_LOAD page
    // alignment declared on the format schema (`fmt.elf().pageAlign`,
    // e.g. 0x1000 for x86_64 Linux / ARM64-4K, 0x4000 for Apple
    // Silicon Asahi, 0x10000 for ARM64-64K). The Linux kernel
    // enforces `p_vaddr % p_align == p_offset % p_align` on every
    // PT_LOAD — execve() fails with ENOEXEC if violated (silent
    // from the toolchain's POV). Cycle 1 ET_REL doesn't have
    // program headers and is unaffected. `validate()` requires
    // non-zero `pageAlign` for Exec (D-LK6-3) so the field is
    // never absent here.
    std::uint64_t const pageAlign = fmt.elf().pageAlign;
    if (isExec) padTo(bytes, pageAlign);
    layoutSection(hText, text);
    // `.rodata` immediately after `.text` (D-LK1-ELF-EXEC-DATA-
    // SECTIONS). The on-disk rodata-from-text delta MUST equal the
    // VA rodata-from-text delta so the single PT_LOAD's file<->mem
    // mapping is congruent: hText.offset is page-aligned (already
    // rodataAlign-aligned since rodataAlign | pageAlign for the
    // shipped 8|4096 case), and layoutSection pads rodata to
    // rodataAlign — so `hRodata.offset - hText.offset ==
    // alignUp(text.size(), rodataAlign)`, matching the early
    // `rodataSectionVa`. Defend that congruence with a fail-loud
    // guard (a non-divisor rodataAlign or future layout change would
    // otherwise silently desync VA from file offset → the loader
    // maps the wrong bytes at the global's runtime address).
    if (hasRodata) {
        layoutSection(hRodata, rodataBytes);
        // ET_REL has no VA — the file/VA congruence check (a PT_LOAD invariant)
        // is exec-only. D-LK-OBJECT-DATA-SECTION-RELOCATABLE.
        if (isExec) {
            std::uint64_t const fileDelta = hRodata.offset - hText.offset;
            if (secText->virtualAddress + fileDelta != rodataSectionVa) {
                emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("elf::encode (ET_EXEC): .rodata file/VA "
                                 "congruence broken — textVa({}) + "
                                 "fileDelta({}) != rodataSectionVa({}). The "
                                 "single PT_LOAD requires the on-disk "
                                 ".rodata-from-.text offset to equal the VA "
                                 "offset. D-LK1-ELF-EXEC-DATA-SECTIONS.",
                                 secText->virtualAddress, fileDelta,
                                 rodataSectionVa));
                return {};
            }
        }
    }
    // `.data` + `.bss` (D-LK4-DATA-PRODUCER) — the WRITABLE segment, page-
    // aligned above the read-only sections so they form a SEPARATE R+W PT_LOAD
    // (W^X). `.data` is file-backed; `.bss` consumes NO file bytes (its sh_offset
    // points just past `.data` but the layout pass appends nothing). Each
    // section's file/VA congruence is asserted fail-loud.
    // ET_REL packs data compactly (section alignment only); the page-boundary
    // padding is a PT_LOAD (exec) concern. D-LK-OBJECT-DATA-SECTION-RELOCATABLE.
    if (isExec && hasWritableSeg) {
        padTo(bytes, pageAlign);   // PT_LOAD #2 page boundary (file + VA)
    }
    if (hasData) {
        layoutSection(hData, dataLayout.bytes);
        if (isExec) {   // exec-only file/VA congruence (PT_LOAD invariant)
            std::uint64_t const fileDelta = hData.offset - hText.offset;
            if (secText->virtualAddress + fileDelta != dataSectionVa) {
                emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("elf::encode (ET_EXEC): .data file/VA congruence "
                                 "broken — textVa({}) + fileDelta({}) != "
                                 "dataSectionVa({}). D-LK4-DATA-PRODUCER.",
                                 secText->virtualAddress, fileDelta, dataSectionVa));
                return {};
            }
        }
    }
    // `.data.rel.ro` (c145) — file-backed, packed at its section alignment right
    // after `.data`, before the zero-fill `.bss` tail. ET_REL only (relro was
    // merged into `.data` on the exec path, so hasRelRo is false there → no VA
    // congruence concern).
    if (hasRelRo) {
        layoutSection(hRelRo, relroLayout.bytes);
    }
    if (hasBss) {
        // NOBITS: record sh_offset at the current cursor (conventional — points
        // just past .data) WITHOUT appending bytes; sh_size is the zero-fill
        // extent. Its sh_addr (set above) is what the loader zero-fills.
        if (hBss.addr_align > 1) padTo(bytes, hBss.addr_align);
        hBss.offset = bytes.size();
    }
    if (secRela != nullptr) layoutSection(hRela, relaText);
    // `.rela.data` / `.rela.data.rel.ro` (c145) follow `.rela.text` (ET_REL only).
    if (hasRelaData)  layoutSection(hRelaData, relaData);
    if (hasRelaRelRo) layoutSection(hRelaRelRo, relaRelRo);
    layoutSection(hSymtab, symtab);
    layoutSection(hStrtab, strtab.view());
    layoutSection(hShStrtab, shstrtab.view());
    // `.note.GNU-stack` last — a zero-length body, so it just records a valid
    // sh_offset (no bytes appended). Keeping it after .shstrtab is what leaves
    // every existing section index + e_shstrndx untouched.
    if (hasNote) layoutSection(hNote, std::span<std::uint8_t const>{});

    padTo(bytes, 8);  // SHT alignment
    std::uint64_t const shoff = bytes.size();
    // Single source of truth for the section count so future cycles
    // (LK1 ELF executable adding .data/.rodata/.bss, LK6 dynamic
    // linking adding .dynamic/.dynsym/.dynstr) cannot drift between
    // the Ehdr's e_shnum and the actual table size.
    // ET_REL keeps `.rela.text` (after any `.rodata`/`.data`/`.bss` —
    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE); ET_EXEC drops it entirely (no
    // SHT_NULL placeholder). Section count derives from the actually-emitted
    // slots — same architect B-LK1-2 / D-LK2-5 discipline that LK1 cycle 1 +
    // LK2 already adopt.
    std::vector<SectionHeader const*> headers;
    headers.reserve(8);
    headers.push_back(&hNull);
    headers.push_back(&hText);
    // `.rodata` at index 2 when present (D-LK1-ELF-EXEC-DATA-SECTIONS)
    // — this push order is what puts it at index 2 and keeps the
    // `rodataShift` of the trailing IDX_* coherent with the header
    // table ordering.
    if (hasRodata) headers.push_back(&hRodata);
    // `.data` / `.bss` (D-LK4-DATA-PRODUCER) follow `.rodata` in the header
    // table (exec-only) — this push order matches the incremental IDX_* math.
    if (hasData) headers.push_back(&hData);
    // `.data.rel.ro` (c145) between `.data` and `.bss` (ET_REL only).
    if (hasRelRo) headers.push_back(&hRelRo);
    if (hasBss) headers.push_back(&hBss);
    if (!isExec) headers.push_back(&hRela);
    // `.rela.data` / `.rela.data.rel.ro` (c145) after `.rela.text` (ET_REL only).
    if (hasRelaData) headers.push_back(&hRelaData);
    if (hasRelaRelRo) headers.push_back(&hRelaRelRo);
    headers.push_back(&hSymtab);
    headers.push_back(&hStrtab);
    headers.push_back(&hShStrtab);
    // `.note.GNU-stack` appended LAST (after .shstrtab) so it grows only
    // `e_shnum` — every prior index + `e_shstrndx` are unchanged.
    if (hasNote) headers.push_back(&hNote);
    std::uint16_t const sectionCount =
        static_cast<std::uint16_t>(headers.size());
    for (auto const* h : headers) writeSectionHeader(bytes, *h);

    // ── Elf64_Ehdr (overwrite the leading 64 zero bytes) ───────
    //
    // ET_REL: e_type = 1, e_entry = 0, e_phoff = 0, e_phnum = 0.
    // ET_EXEC: e_type = 2, e_entry = virtualAddress + entry-fn
    // offset (cycle 2: entry function = module.functions[0] at
    // offset 0 in .text), e_phoff = sizeof(Ehdr) = 64, e_phnum = 1.
    auto const& id = fmt.elf();
    std::uint16_t const eType =
        static_cast<std::uint16_t>(id.objectType);
    std::uint64_t eEntry = 0;
    std::uint64_t ePhoff = 0;
    std::uint16_t ePhnum = 0;
    std::uint16_t ePhentsize = 0;
    if (isExec) {
        // e_entry: virtual address of the entry instruction.
        //
        // Resolution order:
        //   * Empty `entryPoint`: use the first function. Cycle-2
        //     default convention until real names land.
        //   * Non-empty `entryPoint`: look up the named function in
        //     the module. Function names are synthesized as
        //     `sym_<id>` today (D-LK1-1 anchored — real names from
        //     HIR→LIR→AssembledFunction land at LK7); the lookup
        //     accepts that synthesized form. Unknown name = fail
        //     loud K_SymbolUndefined.
        if (module.functions.empty()) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 "ELF ET_EXEC writer: cannot derive e_entry — the "
                 "AssembledModule has zero functions; ET_EXEC requires "
                 "at least one function to serve as the entry point");
            return {};
        }
        // D-LK10-ENTRY Slice C audit fold: shared resolver.
        auto const entryIdxOptInner = link::format::resolveEntryFnIdx(
            module, fmt, "sym_", "ELF ET_EXEC writer", reporter);
        if (!entryIdxOptInner.has_value()) return {};
        std::size_t const entryFnIdx = *entryIdxOptInner;
        std::uint64_t const entryOffsetInText = funcTextStart[entryFnIdx];
        eEntry = secText->virtualAddress + entryOffsetInText;
        ePhoff = kEhdrSize;
        ePhnum = static_cast<std::uint16_t>(kPtLoadCount);
        ePhentsize = static_cast<std::uint16_t>(kProgramHeaderSize);
    }

    std::vector<std::uint8_t> ehdr;
    ehdr.reserve(kEhdrSize);
    // e_ident
    ehdr.push_back(0x7F); ehdr.push_back('E');
    ehdr.push_back('L');  ehdr.push_back('F');
    ehdr.push_back(id.fileClass);
    ehdr.push_back(id.dataEncoding);
    ehdr.push_back(static_cast<std::uint8_t>(EV_CURRENT));
    ehdr.push_back(id.osabi);
    ehdr.push_back(id.abiVersion);
    for (int i = 0; i < 7; ++i) ehdr.push_back(0);  // EI_PAD
    // e_type, e_machine, e_version
    appendU16LE(ehdr, eType);
    appendU16LE(ehdr, id.machine);
    appendU32LE(ehdr, EV_CURRENT);
    // e_entry, e_phoff, e_shoff
    appendU64LE(ehdr, eEntry);
    appendU64LE(ehdr, ePhoff);
    appendU64LE(ehdr, shoff);
    // e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx
    appendU32LE(ehdr, 0);
    appendU16LE(ehdr, static_cast<std::uint16_t>(kEhdrSize));
    appendU16LE(ehdr, ePhentsize);
    appendU16LE(ehdr, ePhnum);
    appendU16LE(ehdr, 64);  // sizeof(Elf64_Shdr)
    appendU16LE(ehdr, sectionCount);
    appendU16LE(ehdr, IDX_SHSTRTAB);
    std::memcpy(bytes.data(), ehdr.data(), kEhdrSize);

    // ── PT_LOAD program header (ET_EXEC only) ──────────────────
    //
    // One PT_LOAD covering the .text region with R|X permissions.
    // ET_EXEC requires at least one PT_LOAD; the runtime loader
    // uses it to map the segment into the process address space.
    // PT_PHDR (pointing at the program header table itself) is
    // conventional but optional for STATIC executables — Linux's
    // kernel ELF loader accepts ET_EXEC without it. **PT_PHDR
    // becomes REQUIRED as soon as PT_INTERP appears**: the dynamic
    // loader uses PT_PHDR to locate the program headers in the
    // mapped process image (otherwise it has no way to find them).
    // Cycle 2 ships only PT_LOAD; PT_PHDR / PT_INTERP / PT_DYNAMIC
    // arrive together with LK6 dynamic linking.
    if (isExec) {
        std::vector<std::uint8_t> phdr;
        phdr.reserve(kPtLoadCount * kProgramHeaderSize);
        auto appendPhdr = [&](std::uint32_t pFlags, std::uint64_t pOffset,
                               std::uint64_t pVaddr, std::uint64_t pFilesz,
                               std::uint64_t pMemsz) {
            appendU32LE(phdr, 1);             // p_type = PT_LOAD
            appendU32LE(phdr, pFlags);
            appendU64LE(phdr, pOffset);
            appendU64LE(phdr, pVaddr);
            appendU64LE(phdr, pVaddr);        // p_paddr
            appendU64LE(phdr, pFilesz);
            appendU64LE(phdr, pMemsz);
            appendU64LE(phdr, pageAlign);     // p_align (kernel congruence)
        };
        // PT_LOAD #1 (R+X) covers `.text` and — when present — `.rodata`
        // (D-LK1-ELF-EXEC-DATA-SECTIONS). Both are contiguous on disk + VA
        // (the layout congruence guard above proves it). p_flags is the OR of
        // the segment's sections' mapped permissions: .text(R+X) | .rodata(R)
        // = R+X (W^X preserved). Span derived from the on-disk extent.
        std::uint32_t pFlags1 = shFlagsToPFlags(secText->flags);
        if (hasRodata) pFlags1 |= shFlagsToPFlags(secRodata->flags);
        std::uint64_t const seg1ByteLen =
            hasRodata ? (hRodata.offset + rodataBytes.size() - hText.offset)
                      : text.size();
        appendPhdr(pFlags1, hText.offset, secText->virtualAddress,
                   seg1ByteLen, seg1ByteLen);
        // PT_LOAD #2 (R+W) covers `.data` (file-backed) + `.bss` (zero-fill).
        // p_flags = OR of .data/.bss sh_flags → R+W. p_filesz spans the file-
        // backed `.data`; p_memsz additionally covers `.bss` so the loader
        // reserves + zeroes it. D-LK4-DATA-PRODUCER.
        if (hasWritableSeg) {
            std::uint32_t pFlags2 = 0;
            if (hasData) pFlags2 |= shFlagsToPFlags(secData->flags);
            if (hasBss)  pFlags2 |= shFlagsToPFlags(secBss->flags);
            std::uint64_t const seg2Off =
                hasData ? hData.offset : hBss.offset;
            std::uint64_t const seg2Va = writableSegVa;
            std::uint64_t const seg2FileSz =
                hasData ? (hData.offset + dataSize - seg2Off) : 0;
            std::uint64_t const seg2MemEnd =
                hasBss ? (bssSectionVa + bssSize)
                       : (dataSectionVa + dataSize);
            std::uint64_t const seg2MemSz = seg2MemEnd - seg2Va;
            appendPhdr(pFlags2, seg2Off, seg2Va, seg2FileSz, seg2MemSz);
        }
        std::memcpy(bytes.data() + kEhdrSize, phdr.data(), phdr.size());
    }

    return bytes;
}

} // namespace dss::elf
