#include "link/format/elf.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift assert
#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
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
constexpr std::uint8_t STT_NOTYPE = 0;
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
constexpr std::uint64_t DT_FLAGS_1 = 0x6ffffffb;
constexpr std::uint64_t DF_1_NOW   = 1;

// Per-machine ELF reloc type for "write resolved symbol VA into GOT
// slot at load time" (dyld semantics).
// x86_64 psABI §4.4.1 — R_X86_64_GLOB_DAT = 6.
// AArch64 ELF psABI §4.6.3 — R_AARCH64_GLOB_DAT = 1025 (0x401).
constexpr std::uint32_t R_X86_64_GLOB_DAT   = 6;
constexpr std::uint32_t R_AARCH64_GLOB_DAT  = 1025;

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

    // Pre-conditions (caller dispatches on these; re-checked here so
    // the helper is callable from future entry paths too —
    // silent-failure H2 + C1 convergence).
    if (module.externImports.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: called with zero "
             "externImports — dynamic-image emission requires at "
             "least one extern; static images route through the "
             "non-dynamic ET_EXEC arm.");
        return {};
    }
    if (module.functions.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: zero functions — ET_EXEC "
             "needs at least one entry function.");
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

    // ── (b) Group externs by library (preserve declaration order)
    std::vector<std::string> libraryOrder;
    std::unordered_map<std::string, std::vector<std::size_t>>
        externsByLib;
    for (std::size_t i = 0; i < module.externImports.size(); ++i) {
        auto const& ext = module.externImports[i];
        auto const it = externsByLib.find(ext.libraryPath);
        if (it == externsByLib.end()) {
            libraryOrder.push_back(ext.libraryPath);
            externsByLib.emplace(ext.libraryPath,
                                 std::vector<std::size_t>{i});
        } else {
            it->second.push_back(i);
        }
    }
    std::size_t const numExterns = module.externImports.size();
    std::size_t const numLibs = libraryOrder.size();
    // Defense-in-depth: the linker validates per-extern non-empty
    // `libraryPath` before dispatch (LK6 cycle 2a substrate), so
    // `numLibs == 0` cannot occur via the linker entry path. This
    // guard makes that invariant explicit in case a future caller
    // bypasses the linker (e.g. a writer-only test harness) — a
    // dynamic image with zero DT_NEEDED entries fails at runtime
    // (dyld has no library to resolve externs from), so failing
    // loud here keeps the contract symmetric across entry paths.
    if (numLibs == 0) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "elf::encodeElfExecDynamic: " +
             std::to_string(numExterns) +
             " extern(s) declared but every `libraryPath` is empty "
             "— zero DT_NEEDED entries would ship; dyld cannot "
             "resolve undefined symbols without a library.");
        return {};
    }

    // ── (c) .interp body (NUL-terminated dynamic-linker path)
    std::vector<std::uint8_t> interp;
    for (char c : elfId.interpreter)
        interp.push_back(static_cast<std::uint8_t>(c));
    interp.push_back(0);

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
        appendDynsymEntry(externNameOff[i],
                          makeStInfo(STB_GLOBAL, STT_NOTYPE),
                          SHN_UNDEF, 0, 0);
    }

    // ── (f) .hash body (DT_HASH single-bucket)
    std::vector<std::uint8_t> hashSec;
    appendU32LE(hashSec, 1);  // nbucket
    appendU32LE(hashSec, static_cast<std::uint32_t>(numExterns + 1));  // nchain
    appendU32LE(hashSec,
        numExterns > 0 ? 1u : 0u);                     // bucket[0]
    appendU32LE(hashSec, 0);                            // chain[0]=STN_UNDEF
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint32_t const next =
            (i + 1 < numExterns) ? static_cast<std::uint32_t>(i + 2) : 0u;
        appendU32LE(hashSec, next);
    }

    // ── (g) .plt body (placeholder; filled after layout)
    // D-LK6-8 closure: stub size is per-machine (x86_64 = 6,
    // ARM64 = 16).
    std::uint16_t const machine = fmt.elf().machine;
    std::uint64_t const pltStubSize = pltStubSizeFor(machine);
    std::vector<std::uint8_t> plt(numExterns * pltStubSize, 0);

    // ── (h) .got body (zero-init; dyld writes resolved fn ptrs)
    std::vector<std::uint8_t> got(numExterns * 8, 0);

    // ── (i) Layout: compute file offsets + VAs ─────────────────
    constexpr std::uint64_t kEhdrSize = 64;
    constexpr std::uint64_t kPhdrSize = 56;
    constexpr std::uint32_t kNumPhdrs = 5;  // PHDR + INTERP + LOAD×2 + DYNAMIC
    std::uint64_t const phtOff = kEhdrSize;
    std::uint64_t const phtSize = kNumPhdrs * kPhdrSize;
    std::uint64_t const interpOff = phtOff + phtSize;
    std::uint64_t const interpVa  = baseImageVa + interpOff;

    std::uint64_t const textOff = pageAlign;   // .text at page boundary
    std::uint64_t const textVa  = secText.virtualAddress;

    // Subsequent sections in PT_LOAD #1; VA = baseImageVa + fileOff
    // (PT_LOAD with fileoff=0 and vaddr=baseImageVa maps verbatim).
    std::uint64_t const pltOff = alignUp(textOff + text.size(), 16);
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
    std::uint64_t const relaDynSz  = numExterns * 24;

    std::uint64_t const ptLoad1End = relaDynOff + relaDynSz;

    // PT_LOAD #2 (R+W) — page-aligned in both file + VA.
    std::uint64_t const ptLoad2Start = alignUp(ptLoad1End, pageAlign);
    std::uint64_t const ptLoad2VaStart = baseImageVa + ptLoad2Start;
    std::uint64_t const gotOff = ptLoad2Start;
    std::uint64_t const gotVa  = ptLoad2VaStart;
    std::uint64_t const gotSz  = got.size();

    std::uint64_t const dynamicOff = alignUp(gotOff + gotSz, 8);
    std::uint64_t const dynamicVa  = baseImageVa + dynamicOff;

    // ── (j) Build .plt bytes (now we have VAs)
    // D-LK6-8 closure: per-machine PLT stub emitter dispatches on
    // `machine`. x86_64 emits 6-byte `FF 25 disp32`; ARM64 emits
    // 16-byte ADRP+LDR+BR+NOP.
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint64_t const stubVa = pltVa + i * pltStubSize;
        std::uint64_t const slotVa = gotVa + i * 8;
        std::size_t   const stubOffset = i * pltStubSize;
        if (!emitPltStub(machine, plt, stubOffset, stubVa, slotVa, reporter)) {
            return {};
        }
    }

    // ── (k) Build .rela.dyn (per-machine GLOB_DAT for each GOT slot)
    // D-LK6-8 closure: R_X86_64_GLOB_DAT = 6 vs R_AARCH64_GLOB_DAT = 1025.
    std::uint32_t const globDatType = globDatTypeFor(machine);
    std::vector<std::uint8_t> relaDyn;
    relaDyn.reserve(relaDynSz);
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint64_t const slotVa = gotVa + i * 8;
        std::uint64_t const rInfo =
            (static_cast<std::uint64_t>(dynsymIdx[i]) << 32)
            | static_cast<std::uint64_t>(globDatType);
        appendU64LE(relaDyn, slotVa);
        appendU64LE(relaDyn, rInfo);
        appendI64LE(relaDyn, 0);
    }

    // ── (l) Build .dynamic
    std::vector<std::uint8_t> dynamicSec;
    auto appendDyn = [&](std::uint64_t tag, std::uint64_t val) {
        appendU64LE(dynamicSec, tag);
        appendU64LE(dynamicSec, val);
    };
    // DT_NEEDED per library, then the resolution-side metadata,
    // then DF_1_NOW for eager binding, then DT_NULL terminator.
    for (std::size_t i = 0; i < numLibs; ++i) {
        appendDyn(DT_NEEDED, libNameOff[i]);
    }
    appendDyn(DT_STRTAB,  dynstrVa);
    appendDyn(DT_STRSZ,   dynstrSz);
    appendDyn(DT_SYMTAB,  dynsymVa);
    appendDyn(DT_SYMENT,  24);
    appendDyn(DT_HASH,    hashVa);
    appendDyn(DT_RELA,    relaDynVa);
    appendDyn(DT_RELASZ,  relaDynSz);
    appendDyn(DT_RELAENT, 24);
    appendDyn(DT_FLAGS_1, DF_1_NOW);
    appendDyn(DT_NULL,    0);
    std::uint64_t const dynamicSz = dynamicSec.size();

    std::uint64_t const ptLoad2End = dynamicOff + dynamicSz;
    std::uint64_t const ptLoad2Size = ptLoad2End - ptLoad2Start;

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
    appendSymtabEntry(0, makeStInfo(STB_LOCAL, STT_SECTION),
                      2 /*shndx=.text*/, 0, 0);
    std::uint32_t const firstNonLocal = 2;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto const& fn = module.functions[i];
        std::string const name =
            std::string{"sym_"} + std::to_string(fn.symbol.v);
        std::uint32_t const nameOff = strtab.add(name);
        appendSymtabEntry(nameOff,
                          makeStInfo(STB_GLOBAL, STT_FUNC),
                          2 /*shndx=.text*/,
                          textVa + funcTextStart[i],
                          fn.bytes.size());
    }

    StringTable shstrtab;
    auto const shsInterp   = shstrtab.add(".interp");
    auto const shsText     = shstrtab.add(".text");
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
                     "' — substrate-invariant violation.");
                return {};
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
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size() + numExterns);
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        symbolVa.emplace(module.functions[i].symbol,
                         textVa + funcTextStart[i]);
    }
    for (std::size_t i = 0; i < numExterns; ++i) {
        // D-LK6-8: extern symbol VA points at the PLT stub. Stub
        // size is per-machine (6 bytes for x86_64; 16 bytes for ARM64).
        auto const [it, inserted] = symbolVa.emplace(
            module.externImports[i].symbol, pltVa + i * pltStubSize);
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
    if (!link::format::applyExecRelocations(
            text, module, funcTextStart, symbolVa,
            targetSchema, textVa,
            "elf::encodeElfExecDynamic", reporter)) {
        return {};
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
    appendPhdrEntry(PT_PHDR,    PF_R,        phtOff,       baseImageVa + phtOff,
                    phtSize,        phtSize,        8);
    appendPhdrEntry(PT_INTERP,  PF_R,        interpOff,    interpVa,
                    interp.size(), interp.size(), 1);
    // PT_LOAD #1 R+X — Ehdr + PHT + .interp + .text + .plt + .dynsym
    //                  + .dynstr + .hash + .rela.dyn
    appendPhdrEntry(PT_LOAD,    PF_X | PF_R, 0,            baseImageVa,
                    ptLoad1End,    ptLoad1End,    pageAlign);
    // PT_LOAD #2 R+W — .got + .dynamic
    appendPhdrEntry(PT_LOAD,    PF_W | PF_R, ptLoad2Start, ptLoad2VaStart,
                    ptLoad2Size,   ptLoad2Size,   pageAlign);
    appendPhdrEntry(PT_DYNAMIC, PF_W | PF_R, dynamicOff,   dynamicVa,
                    dynamicSz,     dynamicSz,     8);

    // Section bodies: pad-to-offset + append per section (simplifier
    // #1 + #4 fold using local padToOffset / appendBytes helpers).
    padToOffset(bytes, interpOff);    appendBytes(bytes, interp);
    padToOffset(bytes, textOff);      appendBytes(bytes, text);
    padToOffset(bytes, pltOff);       appendBytes(bytes, plt);
    padToOffset(bytes, dynsymOff);    appendBytes(bytes, dynsym);
                                       appendBytes(bytes, dynstr);  // align 1
    padToOffset(bytes, hashOff);      appendBytes(bytes, hashSec);
    padToOffset(bytes, relaDynOff);   appendBytes(bytes, relaDyn);
    padToOffset(bytes, ptLoad2Start);                                // PT_LOAD #2 boundary
                                       appendBytes(bytes, got);
    padToOffset(bytes, dynamicOff);   appendBytes(bytes, dynamicSec);
    padToOffset(bytes, symtabOff);    appendBytes(bytes, symtab);
                                       appendBytes(bytes, strtab.view());
                                       appendBytes(bytes, shstrtab.view());
    padToOffset(bytes, shtOff);

    // ── (o) Section Header Table — 13 entries ─────────────────
    //   0: SHT_NULL, 1: .interp, 2: .text, 3: .plt,
    //   4: .dynsym, 5: .dynstr, 6: .hash, 7: .rela.dyn,
    //   8: .got, 9: .dynamic, 10: .symtab, 11: .strtab, 12: .shstrtab
    constexpr std::uint16_t IDX_INTERP   = 1;
    constexpr std::uint16_t IDX_TEXT     = 2;
    constexpr std::uint16_t IDX_DYNSYM   = 4;
    constexpr std::uint16_t IDX_DYNSTR   = 5;
    constexpr std::uint16_t IDX_STRTAB   = 11;
    constexpr std::uint16_t IDX_SHSTRTAB = 12;
    constexpr std::uint16_t kNumSections = 13;
    (void)IDX_INTERP; (void)IDX_TEXT;

    // Designated initializers per `SectionHeader` field — type-design
    // #1 + simplifier #3 fold: 10-arg positional pushShdr lambda
    // dropped (silent u32/u64 swap-bug surface) in favor of named
    // fields at every call site.
    writeSectionHeader(bytes, SectionHeader{});  // SHT_NULL (slot 0)
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsInterp, .type = SHT_PROGBITS, .flags = SHF_ALLOC,
        .addr = interpVa, .offset = interpOff, .size = interp.size(),
        .addr_align = 1});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsText, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR,
        .addr = textVa, .offset = textOff, .size = text.size(),
        .addr_align = 16});
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
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsGot, .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE,
        .addr = gotVa, .offset = gotOff, .size = gotSz,
        .addr_align = 8, .entry_size = 8});
    writeSectionHeader(bytes, SectionHeader{
        .name_offset = shsDynamic, .type = SHT_DYNAMIC, .flags = SHF_ALLOC | SHF_WRITE,
        .addr = dynamicVa, .offset = dynamicOff, .size = dynamicSz,
        .link = IDX_DYNSTR, .addr_align = 8, .entry_size = 16});
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
    auto const entryIdxOpt = link::format::resolveEntryFnIdx(
        module, fmt, "sym_", "elf::encodeElfExecDynamic", reporter);
    if (!entryIdxOpt.has_value()) return {};
    std::size_t const entryFnIdx = *entryIdxOpt;
    std::uint64_t const entryVa = textVa + funcTextStart[entryFnIdx];
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
    appendU16LE(ehdr, 2);  // ET_EXEC
    appendU16LE(ehdr, elfId.machine);
    appendU32LE(ehdr, EV_CURRENT);
    appendU64LE(ehdr, entryVa);
    appendU64LE(ehdr, phtOff);
    appendU64LE(ehdr, shtOff);
    appendU32LE(ehdr, 0);  // e_flags
    appendU16LE(ehdr, static_cast<std::uint16_t>(kEhdrSize));
    appendU16LE(ehdr, static_cast<std::uint16_t>(kPhdrSize));
    appendU16LE(ehdr, static_cast<std::uint16_t>(kNumPhdrs));
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
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"elf::encode: extern imports present ("}
                     + std::to_string(module.externImports.size())
                     + " entries) but format is ET_REL. Externs flow "
                       "to the linker only via ET_EXEC / ET_DYN.");
            return {};
        }
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
    // L1 — these two scans run for BOTH forms (the non-Rodata / reloc
    // checks below precede the `!isExec` dataItems reject), so the
    // message form-qualifier must be computed, not hardcoded ET_EXEC.
    char const* const formKind = isExec ? "(ET_EXEC)" : "(ET_REL)";
    for (auto const& d : module.dataItems) {
        if (d.section != DataSectionKind::Rodata) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("elf::encode {}: AssembledData with "
                             "section={} not yet supported by the ELF "
                             "walker — only Rodata closes at "
                             "D-LK1-ELF-EXEC-DATA-SECTIONS; Data/Bss "
                             "arms remain anchored under "
                             "D-LK4-RODATA-PRODUCER.",
                             formKind, dataSectionKindName(d.section)));
            return {};
        }
        if (!d.relocations.empty()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("elf::encode {}: rodata AssembledData "
                             "SymbolId={{ {} }} carries {} relocation(s) — "
                             "data->data references in .rodata are deferred "
                             "this cycle (the ELF writer patches FUNCTION "
                             "relocations only). Anchored "
                             "D-LK1-ELF-RODATA-DATAITEM-RELOC.",
                             formKind, d.symbol.v, d.relocations.size()));
            return {};
        }
    }
    bool const hasRodata = isExec && !module.dataItems.empty();
    // Reject dataItems on the ET_REL (.o) path loudly — the linker's
    // per-format gate already advertises rodata only on the exec
    // schema, but defend in depth: a hand-built ET_REL module with
    // dataItems would otherwise silently drop them (no .rodata in .o).
    if (!isExec && !module.dataItems.empty()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "elf::encode (ET_REL): AssembledData items are not "
             "emitted into relocatable .o output — rodata in a .o "
             "rides through the symbol+section table, not the "
             "dataItems pipeline. D-LK1-ELF-EXEC-DATA-SECTIONS is "
             "exec-only.");
        return {};
    }
    ObjectFormatSectionInfo const* secRodata =
        hasRodata ? requireSection(fmt, SectionKind::Rodata,
                                   "ELF writer", reporter)
                  : nullptr;
    if (hasRodata && secRodata == nullptr) return {};
    // The schema row's addrAlign is the FLOOR for the section; H1
    // below raises it to the strictest member alignment. Mutable.
    std::uint64_t rodataAlign =
        hasRodata ? std::max<std::uint64_t>(1, secRodata->addrAlign) : 1;
    std::vector<std::uint8_t> rodataBytes;
    // Per-dataItems index → section-relative byte offset (the start of
    // that item's bytes within the concatenated `.rodata` payload).
    // Used below to extend the symbolVa map so a `.text` relocation
    // targeting a rodata SymbolId resolves to
    // `rodataSectionVa + rodataItemOffsets[i]`. Mirrors pe.cpp. u64
    // (NOT u32 like pe.cpp, whose wire fields are u32) — ELF
    // sh_size/sh_offset and the symbolVa values these feed are u64.
    std::vector<std::uint64_t> rodataItemOffsets;
    rodataItemOffsets.reserve(module.dataItems.size());
    if (hasRodata) {
        for (auto const& d : module.dataItems) {
            std::uint64_t const aligned = d.alignment.alignUp(
                static_cast<std::uint64_t>(rodataBytes.size()));
            while (rodataBytes.size() < aligned) rodataBytes.push_back(0);
            rodataItemOffsets.push_back(
                static_cast<std::uint64_t>(rodataBytes.size()));
            rodataBytes.insert(rodataBytes.end(),
                               d.bytes.begin(), d.bytes.end());
        }
        // H1 — section alignment must cover the strictest item (gABI:
        // sh_addralign = max of member alignments) so EVERY item's
        // section-relative offset is also its absolute-VA alignment.
        // The schema row's addrAlign is the floor. (Reachable: a
        // 16-aligned i128/u128 rodata global from the producer —
        // asm.cpp:692 reads primitiveByteSize→16 for I128/U128/F128.)
        // For the single-int corpus max(8,4)=8 → bytes unchanged.
        // D-LK1-ELF-EXEC-DATA-SECTIONS.
        for (auto const& d : module.dataItems)
            rodataAlign = std::max<std::uint64_t>(rodataAlign,
                                                  d.alignment.bytes());
    }

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
        // D-LK1-ELF-EXEC-DATA-SECTIONS: each rodata `AssembledData`
        // item joins the symbolVa map at `rodataSectionVa +
        // rodataItemOffsets[i]`. A `.text` relocation that targets a
        // rodata SymbolId (the code's `lea reg, [rip + g]`) now
        // resolves through the SAME shared `applyExecRelocations`
        // kernel — no rodata loop is added to that kernel; it stays
        // functions-only, and the code-reloc→data-symbol simply needs
        // `g` present in `symbolVa`. Mirrors pe.cpp:875-901. A rodata
        // SymbolId colliding with a function SymbolId is a caller
        // bug (REDEFINITION, not undefined); use the semantically-
        // correct `K_DuplicateDataSymbol` so a test pinning it isn't
        // satisfied by an unrelated undefined-symbol regression.
        if (hasRodata) {
            for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
                auto const& di = module.dataItems[i];
                // M1 — anonymous items (the `SymbolId{}` sentinel:
                // read-only constants / padding, per asm.hpp
                // "Multiple sentinel items are legitimate") are
                // referenced by section offset, NOT by symbol, so
                // they are never reloc targets and must NOT join
                // symbolVa — emplace'ing two `SymbolId{}` items would
                // otherwise false-fire K_DuplicateDataSymbol. (PE
                // pe.cpp:891 carries the SAME latent bug; that parity
                // fix is anchored separately — NOT touched here.)
                if (di.symbol == SymbolId{}) continue;
                std::uint64_t const va =
                    rodataSectionVa + rodataItemOffsets[i];
                if (!symbolVa.emplace(di.symbol, va).second) {
                    emit(reporter,
                         DiagnosticCode::K_DuplicateDataSymbol,
                         std::format("elf::encode (ET_EXEC): rodata "
                                     "SymbolId={{ {} }} collides with "
                                     "another symbol — caller must give "
                                     "each data item a unique SymbolId "
                                     "distinct from function ids.",
                                     di.symbol.v));
                    return {};
                }
            }
        }
        if (!link::format::applyExecRelocations(
                text, module, funcTextStart, symbolVa,
                targetSchema, secText->virtualAddress,
                "elf::encode (ET_EXEC)", reporter)) {
            return {};
        }
    }

    // ── Build .strtab + .symtab ────────────────────────────────
    //
    // Symbol layout: STN_UNDEF (idx 0) → STT_SECTION for .text
    // (LOCAL) → defined function symbols (GLOBAL) → undefined extern
    // symbols (GLOBAL, SHN_UNDEF). `.symtab.sh_info` = index of
    // first non-LOCAL symbol.

    StringTable strtab;
    std::vector<std::uint8_t> symtab;

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
    std::uint32_t const firstNonLocalSymIdx = 2;

    // Map each function's SymbolId to its symtab index (for relocs).
    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;

    // Defined function symbols (GLOBAL + STT_FUNC + shndx=.text).
    for (auto const& f : funcSyms) {
        std::string const symName =
            std::string{"sym_"} + std::to_string(f.symId.v);
        std::uint32_t const nameOff = strtab.add(symName);
        std::uint32_t const idx =
            static_cast<std::uint32_t>(symtab.size() / 24);
        appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_FUNC), 0,
                  /*shndx=.text*/ 1, f.valueInText, f.size);
        symIdxBySymbol.emplace(f.symId, idx);
    }

    // Undefined extern symbols referenced by any relocation but not
    // defined by any function. ET_EXEC has no extern symbols at this
    // point — the cycle-1 reloc-application pass above failed loud on
    // any unresolved target (FFI / dynamic linking is LK6 cycle 2).
    if (!isExec) {
        for (auto const& fn : module.functions) {
            for (auto const& rel : fn.relocations) {
                if (symIdxBySymbol.contains(rel.target)) continue;
                std::string const symName =
                    std::string{"sym_"} + std::to_string(rel.target.v);
                std::uint32_t const nameOff = strtab.add(symName);
                std::uint32_t const idx =
                    static_cast<std::uint32_t>(symtab.size() / 24);
                appendSym(nameOff, makeStInfo(STB_GLOBAL, STT_NOTYPE), 0,
                          SHN_UNDEF, 0, 0);
                symIdxBySymbol.emplace(rel.target, idx);
            }
        }
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
                std::uint32_t const symIdx = it->second;
                std::uint64_t const rOffset = fnStart + rel.offset;
                appendU64LE(relaText, rOffset);
                appendU64LE(relaText, makeRelaInfo(symIdx, fmtReloc->nativeId));
                appendI64LE(relaText, rel.addend);
            }
        }
    }

    // ── Section ordering + .shstrtab ───────────────────────────
    //
    // ET_REL order: SHT_NULL, .text, .rela.text, .symtab, .strtab,
    // .shstrtab. ET_EXEC drops `.rela.text` entirely (no SHT_NULL
    // placeholder): intra-module relocations were applied in-place
    // to `.text` by `applyExecRelocations` above (LK6 cycle 1,
    // closes D-LK1-3); extern relocs (FFI / dynamic linking) are
    // anchored at D-LK6-2 and don't reach this point.
    StringTable shstrtab;
    SectionHeader hNull{};
    SectionHeader hText{};
    SectionHeader hRodata{};
    SectionHeader hRela{};
    SectionHeader hSymtab{};
    SectionHeader hStrtab{};
    SectionHeader hShStrtab{};
    hText.name_offset      = shstrtab.add(secText->name);
    if (hasRodata) {
        hRodata.name_offset = shstrtab.add(secRodata->name);
    }
    if (secRela != nullptr) {
        hRela.name_offset  = shstrtab.add(secRela->name);
    }
    hSymtab.name_offset    = shstrtab.add(secSymtab->name);
    hStrtab.name_offset    = shstrtab.add(secStrtab->name);
    hShStrtab.name_offset  = shstrtab.add(secShStrtab->name);

    // Section indices — IDX_TEXT==1 is pinned (the STT_SECTION sym
    // emitted above hardcodes st_shndx=1). Other indices depend on
    // whether the `.rela.text` slot is present AND whether `.rodata`
    // is present:
    //   ET_REL:               Null(0), Text(1), Rela(2), Symtab(3), Strtab(4), ShStrtab(5).
    //   ET_EXEC (no rodata):  Null(0), Text(1),          Symtab(2), Strtab(3), ShStrtab(4).
    //   ET_EXEC (+ rodata):   Null(0), Text(1), Rodata(2), Symtab(3), Strtab(4), ShStrtab(5).
    // The phantom SHT_NULL placeholder in ET_EXEC was an LK1-cycle-2
    // first draft; architect convergence pulled it out so `readelf
    // -S` doesn't show a blank slot at idx 2 and the index math is
    // honest. `.rodata` (D-LK1-ELF-EXEC-DATA-SECTIONS) sits at index
    // 2 when present, shifting the trailing sections +1.
    constexpr std::uint16_t IDX_TEXT     = 1;
    // `.rodata`, when present, occupies index 2 (see the table above)
    // and shifts every trailing section +1 via `rodataShift`. There is
    // no `IDX_RODATA` constant: the header table is built by ordered
    // `push_back`, not index lookup, so the trailing-section indices
    // are the only ones that need computing.
    std::uint16_t const rodataShift  = hasRodata ? 1u : 0u;
    std::uint16_t const IDX_SYMTAB   = static_cast<std::uint16_t>((isExec ? 2u : 3u) + rodataShift);
    std::uint16_t const IDX_STRTAB   = static_cast<std::uint16_t>((isExec ? 3u : 4u) + rodataShift);
    std::uint16_t const IDX_SHSTRTAB = static_cast<std::uint16_t>((isExec ? 4u : 5u) + rodataShift);

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
        hRodata.addr       = rodataSectionVa;
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

    // ── Layout pass: compute sh_offset for each section ────────
    //
    // ET_REL: [Ehdr] + section bodies + SHT at end.
    // ET_EXEC: [Ehdr] + [PHT (program headers)] + section bodies +
    //          SHT at end. The PHT lives immediately after the Ehdr
    //          so e_phoff = 64 and runtime loaders find it without
    //          a seek.
    constexpr std::uint64_t kEhdrSize = 64;
    constexpr std::uint64_t kProgramHeaderSize = 56;  // Elf64_Phdr
    constexpr std::uint64_t kPtLoadCount = 1;         // cycle-2: just .text
    std::uint64_t const phtSize = isExec ? (kPtLoadCount * kProgramHeaderSize) : 0;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kEhdrSize + phtSize + text.size() + relaText.size()
                  + symtab.size() + strtab.size() + shstrtab.size() + 6 * 64);
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
    if (secRela != nullptr) layoutSection(hRela, relaText);
    layoutSection(hSymtab, symtab);
    layoutSection(hStrtab, strtab.view());
    layoutSection(hShStrtab, shstrtab.view());

    padTo(bytes, 8);  // SHT alignment
    std::uint64_t const shoff = bytes.size();
    // Single source of truth for the section count so future cycles
    // (LK1 ELF executable adding .data/.rodata/.bss, LK6 dynamic
    // linking adding .dynamic/.dynsym/.dynstr) cannot drift between
    // the Ehdr's e_shnum and the actual table size.
    // ET_REL keeps `.rela.text` in slot 2; ET_EXEC drops it entirely
    // (no SHT_NULL placeholder). Section count derives from the
    // actually-emitted slots — same architect B-LK1-2 / D-LK2-5
    // discipline that LK1 cycle 1 + LK2 already adopt.
    std::vector<SectionHeader const*> headers;
    headers.reserve(7);
    headers.push_back(&hNull);
    headers.push_back(&hText);
    // `.rodata` at index 2 when present (D-LK1-ELF-EXEC-DATA-SECTIONS)
    // — this push order is what puts it at index 2 and keeps the
    // `rodataShift` of the trailing IDX_* coherent with the header
    // table ordering.
    if (hasRodata) headers.push_back(&hRodata);
    if (!isExec) headers.push_back(&hRela);
    headers.push_back(&hSymtab);
    headers.push_back(&hStrtab);
    headers.push_back(&hShStrtab);
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
        phdr.reserve(kProgramHeaderSize);
        // The single PT_LOAD covers `.text` and — when present —
        // `.rodata` (D-LK1-ELF-EXEC-DATA-SECTIONS). Both are
        // contiguous on disk and in VA (the layout congruence guard
        // above proves it), so ONE segment maps them. p_flags is the
        // OR of the segment's sections' mapped permissions (NOT the
        // old hardcoded 5): .text(R+X) | .rodata(R) = R+X = 5, so the
        // no-rodata output is byte-identical and the rodata case stays
        // R+X — strictly no new write permission (W^X preserved).
        std::uint32_t pFlags = shFlagsToPFlags(secText->flags);
        if (hasRodata) pFlags |= shFlagsToPFlags(secRodata->flags);
        // p_filesz / p_memsz span .text through the end of .rodata
        // when present (file offsets are contiguous post-layout), else
        // just .text. Derived from the on-disk extent so it tracks the
        // actual emitted bytes.
        std::uint64_t const segByteLen =
            hasRodata ? (hRodata.offset + rodataBytes.size() - hText.offset)
                      : text.size();
        // p_type = PT_LOAD = 1
        appendU32LE(phdr, 1);
        // p_flags — derived above (R / W / X from section sh_flags).
        appendU32LE(phdr, pFlags);
        // p_offset = file offset of .text (segment start)
        appendU64LE(phdr, hText.offset);
        // p_vaddr / p_paddr = virtual address of .text (segment start)
        appendU64LE(phdr, secText->virtualAddress);
        appendU64LE(phdr, secText->virtualAddress);
        // p_filesz / p_memsz = byte length of the segment (.text [+ .rodata])
        appendU64LE(phdr, segByteLen);
        appendU64LE(phdr, segByteLen);
        // p_align — declared by the format schema per (arch × OS).
        // See the `padTo(bytes, pageAlign)` call above; both must
        // use the SAME value or the kernel's congruence check
        // (p_vaddr % p_align == p_offset % p_align) fails.
        appendU64LE(phdr, pageAlign);
        std::memcpy(bytes.data() + kEhdrSize, phdr.data(),
                    kProgramHeaderSize);
    }

    return bytes;
}

} // namespace dss::elf
