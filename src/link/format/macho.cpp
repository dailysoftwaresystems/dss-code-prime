#include "link/format/macho.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"
#include "link/format/exec_data_section.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/format/macho_chained_fixups.hpp"
#include "link/format/macho_codesign.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Mach-O 64-bit writer — plan 14 LK3 cycle 1 (.o) + cycle 2 (.exe).
//
// MH_OBJECT (.o) byte layout (Apple OS X ABI Mach-O File Format
// Reference + <mach-o/loader.h> + <mach-o/nlist.h> + <mach-o/reloc.h>):
//   [0x00]      mach_header_64                  (32 B)
//   [0x20]      LC_SEGMENT_64 + section_64[N]   (72 + 80*N B)
//   [...]       LC_SYMTAB                       (24 B)
//   [...]       __text bytes
//   [...]       per-section relocation_info[]   (8 B each)
//   [symoff]    nlist_64[]                      (16 B each)
//   [stroff]    string table (NUL-seeded)
//
// MH_EXECUTE (.exe) byte layout (LK3 cycle 2, closes D-LK3-2's
// MH_EXECUTE half — D-LK3-3 anchors MH_DYLIB):
//   [0x00]      mach_header_64                  (32 B)
//   [0x20]      LC_SEGMENT_64 __PAGEZERO        (72 B, 0 sections)
//   [...]       LC_SEGMENT_64 __TEXT + sec_64   (72 + 80*N B)
//   [...]       LC_LOAD_DYLINKER                (≥ 12 B, padded to 8)
//   [...]       LC_MAIN                         (24 B)
//   [...]       LC_LOAD_DYLIB[] (one per lib)   (≥ 24 B each, padded to 8)
//   [...]       LC_SYMTAB                       (24 B)
//   [...]       (pad to page)
//   [...]       __text bytes (loaded at __TEXT.vmaddr)
//   [symoff]    nlist_64[]   (16 B each — pointed to by LC_SYMTAB.symoff)
//   [stroff]    string table (NUL-seeded — pointed to by LC_SYMTAB.stroff)
//
// The walker is target-blind in shape — every Mach-O-specific
// number (cputype, cpusubtype, filetype, section flags, reloc
// nativeId, section/segment names, dylinker / dylib paths) is
// read from the format schema. Only the binary record layout is
// hardcoded.

namespace dss::macho {

namespace {

using dss::report;
using link::format::detail::alignUp;
using link::format::detail::appendU8;
using link::format::detail::appendU16LE;
using link::format::detail::appendU32LE;
using link::format::detail::appendU64LE;
using link::format::detail::emit;
using link::format::detail::requireSection;
using link::format::detail::StringTable;

// ── Mach-O constants ────────────────────────────────────────────

constexpr std::uint32_t MH_MAGIC_64 = 0xFEEDFACFu;
constexpr std::uint32_t LC_SEGMENT_64 = 0x19u;
constexpr std::uint32_t LC_SYMTAB     = 0x02u;
constexpr std::int32_t  kVmProtRwx    = 7;   // R|W|X

// nlist_64.n_type bits (<mach-o/nlist.h>)
constexpr std::uint8_t N_EXT  = 0x01;
constexpr std::uint8_t N_UNDF = 0x00;
constexpr std::uint8_t N_SECT = 0x0E;

constexpr std::size_t kMachHeader64Size   = 32;
constexpr std::size_t kSegmentCommand64Size = 72;
constexpr std::size_t kSection64Size      = 80;
constexpr std::size_t kSymtabCommandSize  = 24;
constexpr std::size_t kNlist64Size        = 16;
constexpr std::size_t kRelocationInfoSize = 8;

// ── Fixed-width name field (16 chars, NUL-padded) ──────────────

void appendName16(std::vector<std::uint8_t>& out, std::string_view name) {
    std::array<std::uint8_t, 16> buf{};  // zero-initialized
    std::size_t const n = std::min(name.size(), std::size_t{16});
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<std::uint8_t>(name[i]);
    }
    for (auto b : buf) out.push_back(b);
}

void padTo(std::vector<std::uint8_t>& out, std::uint64_t alignment) {
    if (alignment <= 1) return;
    while (out.size() % alignment != 0) out.push_back(0);
}

// LC_* command constants (<mach-o/loader.h>) shared between the
// static (encodeExec) and dynamic (encodeExecDynamic) arms.
constexpr std::uint32_t LC_LOAD_DYLINKER  = 0x0E;
constexpr std::uint32_t LC_MAIN           = 0x80000028u;  // |= LC_REQ_DYLD
constexpr std::uint32_t LC_LOAD_DYLIB     = 0x0C;
constexpr std::uint32_t LC_DYLD_INFO_ONLY      = 0x80000022u;  // |= LC_REQ_DYLD
constexpr std::uint32_t LC_DYLD_CHAINED_FIXUPS = 0x80000034u;  // |= LC_REQ_DYLD — D-LK6-14
constexpr std::uint32_t LC_DYSYMTAB            = 0x0B;
constexpr std::uint32_t LC_CODE_SIGNATURE = 0x1D;
constexpr std::uint32_t LC_BUILD_VERSION  = 0x32;  // build_version_command
// linkedit_data_command shape (16 bytes): cmd / cmdsize / dataoff /
// datasize. Both LC_CODE_SIGNATURE and LC_DYLD_CHAINED_FIXUPS use
// this exact layout — the alias documents the shared shape so
// future linkedit-data-command LCs can reuse the constant and each
// emission site is site-specific about WHICH LC is being sized.
// (Cross-read: type-design Q3 + simplifier #2 voted collapse;
// code-architect Q6 voted keep — compromise = keep both names for
// site-specific clarity + static_assert pins the wire-shared shape
// so a future Apple change to one struct doesn't diverge silently.)
constexpr std::size_t   kCodeSigCommandSize = 16;
constexpr std::size_t   kLinkeditDataCommandSize = 16;
// build_version_command (<mach-o/loader.h>): cmd / cmdsize / platform /
// minos / sdk / ntools — 6 × u32 = 24 bytes with ntools = 0 (no trailing
// build_tool_version records). Modern dyld (macOS 11+ / Apple Silicon)
// identifies the executable's PLATFORM from this command; a main
// executable without it (nor the legacy LC_VERSION_MIN_MACOSX) is
// rejected at load. (D-LK10-ENTRY-MACHO-EXIT.)
constexpr std::size_t   kBuildVersionCommandSize = 24;
static_assert(kCodeSigCommandSize == kLinkeditDataCommandSize,
              "linkedit_data_command shape is wire-frozen at 16 bytes; "
              "both LC_CODE_SIGNATURE and LC_DYLD_CHAINED_FIXUPS use it.");
constexpr std::uint64_t kLoadCmdAlign     = 8;  // pad LCs to 8 bytes

// Emit one `build_version_command` (LC_BUILD_VERSION, 24 bytes, ntools=0)
// — the SINGLE chokepoint shared by both the static (encodeExec) and
// dynamic (encodeExecDynamic) arms, so the 6-u32 wire layout is written
// in exactly one place. platform / minos / sdk are little-endian u32s;
// minos/sdk carry the (major<<16)|(minor<<8)|patch nibble encoding the
// schema loader produced. (D-LK10-ENTRY-MACHO-EXIT.)
void appendBuildVersionCommand(std::vector<std::uint8_t>& out,
                               MachOBuildVersion const& bv) {
    appendU32LE(out, LC_BUILD_VERSION);
    appendU32LE(out,
                static_cast<std::uint32_t>(kBuildVersionCommandSize));
    appendU32LE(out, static_cast<std::uint32_t>(bv.platform));
    appendU32LE(out, bv.minOs);
    appendU32LE(out, bv.sdk);
    appendU32LE(out, 0u);  // ntools = 0 (no trailing build_tool_version)
}

// section_64.flags (S_*) — <mach-o/loader.h>
constexpr std::uint32_t S_NON_LAZY_SYMBOL_POINTERS = 0x6;
constexpr std::uint32_t S_SYMBOL_STUBS             = 0x8;
constexpr std::uint32_t S_ATTR_SOME_INSTRUCTIONS   = 0x00000400u;
constexpr std::uint32_t S_ATTR_PURE_INSTRUCTIONS   = 0x80000000u;

// dyld bind opcodes (<mach-o/loader.h>) — consumed by the dynamic
// arm's bind-stream emitter.
constexpr std::uint8_t BIND_OPCODE_DONE                           = 0x00;
constexpr std::uint8_t BIND_OPCODE_SET_DYLIB_ORDINAL_IMM          = 0x10;
constexpr std::uint8_t BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB         = 0x20;
constexpr std::uint8_t BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM  = 0x40;
constexpr std::uint8_t BIND_OPCODE_SET_TYPE_IMM                   = 0x50;
constexpr std::uint8_t BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB    = 0x70;
constexpr std::uint8_t BIND_OPCODE_DO_BIND                        = 0x90;
constexpr std::uint8_t BIND_TYPE_POINTER                          = 1;

constexpr std::size_t kDyldInfoCommandSize = 48;
constexpr std::size_t kDysymtabCommandSize = 80;
constexpr std::size_t kGotSlotSize         = 8;

// ── Per-cputype __stubs substrate (MIRRORS elf.cpp's emitPltStub) ──
//
// Mach-O `__stubs` plays the same role as ELF's PLT: one stub per
// extern, pointing at the extern's `__got` slot. symbolVa[extern] is
// the STUB (direct-plt), so the call site is a plain direct call/BL to
// the stub and the stub does the __got indirection itself.
//
// CPU_TYPE values (<mach-o/machine.h>): the high bit 0x01000000 is
// CPU_ARCH_ABI64; x86_64 = (7 | ABI64) = 0x01000007; arm64 = (12 |
// ABI64) = 0x0100000C. The walker dispatches on the schema's cputype
// (read as data) — adding a 3rd ISA = a new size arm + a new emitter +
// a new dispatch case, all localized to this file (the elf.cpp
// precedent). x86_64 stub = 6-byte `FF 25 disp32`; arm64 stub = 12-byte
// ADRP+LDR+BR macro.
constexpr std::uint32_t kCpuTypeX86_64 = 0x01000007u;
constexpr std::uint32_t kCpuTypeArm64  = 0x0100000Cu;

constexpr std::size_t kX86_64MachoStubSize = 6;   // FF 25 disp32
constexpr std::size_t kArm64MachoStubSize  = 12;  // ADRP+LDR+BR (3×4)

// Per-cputype __stubs entry size in bytes. Returns 0 for an unhandled
// cputype — every CALLER pairs the size query with `emitMachoStub`,
// whose `default` arm fails loud, so a 0 here never silently ships a
// zero-stride __stubs section.
[[nodiscard]] constexpr std::size_t
machoStubSizeFor(std::uint32_t cputype) noexcept {
    switch (cputype) {
        case kCpuTypeX86_64: return kX86_64MachoStubSize;
        case kCpuTypeArm64:  return kArm64MachoStubSize;
    }
    return 0u;
}

// Emit one x86_64 `__stubs` entry: 6-byte `FF 25 disp32` =
// `jmp *(rip + disp32)` jumping indirectly through the __got slot.
// disp32 is PC-relative from the END of the 6-byte instruction.
// (Byte-IDENTICAL to the original inline x86 stub body — extracted
// verbatim so the existing macho tests keep passing.)
[[nodiscard]] inline bool emitX86_64MachoStub(
        std::vector<std::uint8_t>& stubs,
        std::uint64_t              stubVa,
        std::uint64_t              gotSlotVa,
        std::size_t                externIdx,
        DiagnosticReporter&        reporter) {
    std::int64_t const disp =
        static_cast<std::int64_t>(gotSlotVa) -
        static_cast<std::int64_t>(stubVa + kX86_64MachoStubSize);
    if (disp < std::numeric_limits<std::int32_t>::min()
     || disp > std::numeric_limits<std::int32_t>::max()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: __stubs disp32 "
                         "overflow (0x{:x}) for extern #{}; image too "
                         "large for 32-bit PC-relative __got reference.",
                         static_cast<std::uint64_t>(disp), externIdx));
        return false;
    }
    std::uint32_t const d32 =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(disp));
    stubs.push_back(0xFF);
    stubs.push_back(0x25);
    stubs.push_back(static_cast<std::uint8_t>(d32 & 0xFF));
    stubs.push_back(static_cast<std::uint8_t>((d32 >> 8) & 0xFF));
    stubs.push_back(static_cast<std::uint8_t>((d32 >> 16) & 0xFF));
    stubs.push_back(static_cast<std::uint8_t>((d32 >> 24) & 0xFF));
    return true;
}

// Emit one arm64 `__stubs` entry: 12 bytes = ADRP x16, page-of(got)
// → LDR x16, [x16, #lo12(got)] → BR x16. This is the canonical macOS
// `dyld_stub` shape. The ADRP page-pair + LDR imm12 math is IDENTICAL
// to elf.cpp's `emitArm64PltStub` (kept consistent so the page/lo12
// derivation lives in one mental model) — the only differences are
// (a) x16-only here (vs ELF's x16→x17 split; both are AAPCS64 IP
// scratch, ARM IHI 0055 §6.1.1) so dyld's stub-binder convention is
// matched, and (b) BR x16 + NO trailing NOP (12 bytes, not 16).
[[nodiscard]] inline bool emitArm64MachoStub(
        std::vector<std::uint8_t>& stubs,
        std::uint64_t              stubVa,
        std::uint64_t              gotSlotVa,
        std::size_t                externIdx,
        DiagnosticReporter&        reporter) {
    auto const pageOf = [](std::uint64_t v) noexcept -> std::uint64_t {
        return v & ~std::uint64_t{0xFFF};
    };
    std::int64_t const pageDiff =
        static_cast<std::int64_t>(pageOf(gotSlotVa))
      - static_cast<std::int64_t>(pageOf(stubVa));
    std::int64_t const adrpValue = pageDiff >> 12;
    constexpr std::int64_t kAdrpMax =  (std::int64_t{1} << 20) - 1;
    constexpr std::int64_t kAdrpMin = -(std::int64_t{1} << 20);
    if (adrpValue < kAdrpMin || adrpValue > kAdrpMax) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic (arm64 __stubs): "
                         "ADRP page-pair value {} for extern #{} is out "
                         "of signed 21-bit range — __stubs and __got "
                         "too far apart (>±4 GiB).",
                         adrpValue, externIdx));
        return false;
    }
    // LDR (immediate, unsigned offset) 64-bit scales imm12 by 8, so the
    // __got slot's low-12 must be 8-byte aligned. __got slots are 8
    // bytes each and __got is page-aligned by construction.
    std::uint64_t const lo12 = gotSlotVa & std::uint64_t{0xFFF};
    if ((lo12 & 0x7u) != 0u) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic (arm64 __stubs): "
                         "__got slot VA for extern #{} is not 8-byte "
                         "aligned (low12=0x{:x}) — the AArch64 64-bit "
                         "LDR encoding requires an 8-byte-aligned "
                         "offset; __got layout is broken.",
                         externIdx, lo12));
        return false;
    }
    // ADRP x16: base 0x90000010 (Rd=x16); immlo at bits[30:29], immhi
    // at bits[23:5] carrying the 21-bit two's-complement page value.
    std::uint32_t const adrpV =
        static_cast<std::uint32_t>(adrpValue) & 0x1FFFFFu;
    std::uint32_t const immlo = (adrpV & 0x3u) << 29;
    std::uint32_t const immhi = ((adrpV >> 2) & 0x7FFFFu) << 5;
    std::uint32_t const adrp  = 0x90000010u | immlo | immhi;
    // LDR x16, [x16, #lo12]: base 0xF9400000; imm12 at bits[21:10]
    // (scaled by 8), Rn=x16 at bits[9:5], Rt=x16 at bits[4:0].
    // Pre-shifted base 0xF9400210 carries Rn=16 + Rt=16.
    std::uint32_t const imm12 = static_cast<std::uint32_t>(lo12 >> 3);
    std::uint32_t const ldr   = 0xF9400210u | (imm12 << 10);
    // BR x16 (unconditional indirect branch to x16).
    constexpr std::uint32_t kBrX16 = 0xD61F0200u;
    auto const pushInst = [&](std::uint32_t inst) {
        stubs.push_back(static_cast<std::uint8_t>(inst         & 0xFFu));
        stubs.push_back(static_cast<std::uint8_t>((inst >>  8) & 0xFFu));
        stubs.push_back(static_cast<std::uint8_t>((inst >> 16) & 0xFFu));
        stubs.push_back(static_cast<std::uint8_t>((inst >> 24) & 0xFFu));
    };
    pushInst(adrp);
    pushInst(ldr);
    pushInst(kBrX16);
    return true;
}

// Per-cputype __stubs dispatch (mirrors elf.cpp's emitPltStub). Appends
// exactly `machoStubSizeFor(cputype)` bytes on success. Fail-loud
// `default` — NO silent zero-byte stub for an unhandled cputype.
[[nodiscard]] inline bool emitMachoStub(
        std::uint32_t              cputype,
        std::vector<std::uint8_t>& stubs,
        std::uint64_t              stubVa,
        std::uint64_t              gotSlotVa,
        std::size_t                externIdx,
        DiagnosticReporter&        reporter) {
    switch (cputype) {
        case kCpuTypeX86_64:
            return emitX86_64MachoStub(stubs, stubVa, gotSlotVa,
                                       externIdx, reporter);
        case kCpuTypeArm64:
            return emitArm64MachoStub(stubs, stubVa, gotSlotVa,
                                      externIdx, reporter);
    }
    emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
         std::format("macho::encodeExecDynamic: cputype 0x{:x} has no "
                     "__stubs emitter — the Mach-O dynamic-import path "
                     "supports x86_64 (0x{:x}) and arm64 (0x{:x}). Add "
                     "a per-cputype emitter (see emitMachoStub) to ship "
                     "FFI for this architecture.",
                     cputype, kCpuTypeX86_64, kCpuTypeArm64));
    return false;
}

// LC byte-size of a path-bearing load command: fixed header + path
// bytes (NUL-terminated) + pad to kLoadCmdAlign. Used by
// LC_LOAD_DYLINKER (fixedSize=12) and LC_LOAD_DYLIB (fixedSize=24).
[[nodiscard]] inline std::size_t
commandSizeWithPath(std::size_t fixedSize, std::string const& path) {
    std::size_t const raw = fixedSize + path.size() + 1;
    return (raw + kLoadCmdAlign - 1) & ~(kLoadCmdAlign - 1);
}

// ULEB128 encoder (used by the bind-opcode stream emitter).
inline void
appendULEB128(std::vector<std::uint8_t>& out, std::uint64_t v) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7F);
        v >>= 7;
        if (v != 0) byte |= 0x80u;
        out.push_back(byte);
    } while (v != 0);
}

} // namespace

namespace {
[[nodiscard]] std::vector<std::uint8_t>
encodeExec(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& fmt,
           ObjectFormatSectionInfo const& secText,
           DiagnosticReporter&       reporter);
[[nodiscard]] std::vector<std::uint8_t>
encodeExecDynamic(AssembledModule const&    module,
                  TargetSchema const&       targetSchema,
                  ObjectFormatSchema const& fmt,
                  ObjectFormatSectionInfo const& secText,
                  DiagnosticReporter&       reporter);
} // namespace

std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter) {
    auto const& fmt = objectFormatSchema;
    if (fmt.kind() != ObjectFormatKind::MachO) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"macho::encode called with non-Mach-O format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }

    // Mach-O requires `__text`; symtab/strtab live inside LC_SYMTAB,
    // not as separate section headers.
    auto const* secText =
        requireSection(fmt, SectionKind::Text, "Mach-O writer", reporter);
    if (!secText) return {};

    // Dispatch between MH_OBJECT (cycle 1) and MH_EXECUTE (cycle 2)
    // based on the schema's declared filetype. validate() rejects
    // any value outside {1, 2} so this switch is exhaustive. The
    // MH_EXECUTE arm further splits into the static path (no
    // externs; LK3 cycle 2) and the dynamic path (externs present;
    // LK6 cycle 2c).
    if (fmt.macho().filetype == MachOObjectType::Execute) {
        // LK7 codesign-placeholder gate (silent-failure HIGH fold,
        // architect anchor): the static `encodeExec` path emits no
        // __LINKEDIT segment, so an LC_CODE_SIGNATURE pointing at
        // file-tail bytes lies outside any LC_SEGMENT_64 — Apple's
        // kernel `cs_validate_range` rejects such binaries because
        // the CD blob must be mmap-able via a segment. Force the
        // dynamic path (which DOES carry __LINKEDIT) whenever a
        // codesign reservation is requested. Empty `externImports`
        // with `codeSignatureSize > 0` is currently a no-realistic-
        // use-case combination (a signed binary that imports nothing
        // is degenerate); when first observed in practice, the
        // dynamic-path arm will widen — anchored D-LK7-1 at plan 14
        // §3.1.
        if (fmt.machoImage().codeSignatureSize != 0
         && module.externImports.empty()) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 "macho::encode: 'image.codeSignatureSize' is "
                 "non-zero but the module has no externImports — "
                 "the static encodeExec path emits no __LINKEDIT "
                 "segment, so the LC_CODE_SIGNATURE reservation "
                 "would land outside any LC_SEGMENT_64 and the "
                 "kernel `cs_validate_range` would reject the "
                 "binary at exec time. Add at least one extern "
                 "import (which routes through encodeExecDynamic + "
                 "synthesizes __LINKEDIT) OR clear "
                 "'codeSignatureSize'. Anchored at plan 14 §3.1 "
                 "D-LK7-1 (static-path __LINKEDIT synthesis).");
            return {};
        }
        if (!module.externImports.empty()) {
            if (!fmt.machoImage().bindNow) {
                emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                     "macho::encode: schema requests lazy binding "
                     "('image.bindNow' = false) but Mach-O lazy "
                     "binding (LC_DYLD_INFO_ONLY.lazy_bind_off "
                     "opcode stream + dyld_stub_binder) has not "
                     "yet landed. Anchored at plan 14 §3.1 D-LK6-13. "
                     "Set 'image.bindNow' = true (the v1 stance — "
                     "immediate bind_off opcode stream) to proceed.");
                return {};
            }
            return encodeExecDynamic(module, targetSchema, fmt,
                                     *secText, reporter);
        }
        return encodeExec(module, targetSchema, fmt, *secText, reporter);
    }
    (void)targetSchema;  // MH_OBJECT path does not apply relocations;
                         // the assembler stamped the bytes and the
                         // .o writer just serializes them.

    // ── Build .text + per-function symbols ─────────────────────
    std::vector<std::uint8_t> textBody;
    struct FuncSymRecord {
        SymbolId      symId{};
        std::uint64_t valueInText = 0;
        std::uint64_t size = 0;
    };
    std::vector<FuncSymRecord> funcSyms;
    funcSyms.reserve(module.functions.size());
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());

    for (auto const& fn : module.functions) {
        std::uint64_t const start = textBody.size();
        funcTextStart.push_back(start);
        textBody.insert(textBody.end(), fn.bytes.begin(), fn.bytes.end());
        funcSyms.push_back({fn.symbol, start,
                            static_cast<std::uint64_t>(fn.bytes.size())});
    }

    // ── Build symbol-table indices (same discipline as PE) ─────
    //
    // Order: defined externs (N_SECT|N_EXT) followed by undefined
    // externs (N_UNDF|N_EXT). Mach-O doesn't require local-then-
    // global when LC_DYSYMTAB is absent, but defined-then-undefined
    // is the convention Apple's ld64 produces.

    std::unordered_set<SymbolId> definedSet;
    definedSet.reserve(funcSyms.size());
    for (auto const& f : funcSyms) definedSet.insert(f.symId);

    std::vector<SymbolId> externSyms;
    std::unordered_set<SymbolId> externSeen;
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (definedSet.contains(rel.target)) continue;
            if (externSeen.insert(rel.target).second) {
                externSyms.push_back(rel.target);
            }
        }
    }

    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;
    symIdxBySymbol.reserve(funcSyms.size() + externSyms.size());
    std::uint32_t nextSymIdx = 0;
    for (auto const& f : funcSyms) {
        auto const [it, fresh] = symIdxBySymbol.emplace(f.symId, nextSymIdx);
        if (!fresh) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"Mach-O writer: duplicate defined symbol #"}
                     + std::to_string(f.symId.v));
            return {};
        }
        ++nextSymIdx;
    }
    for (auto const& e : externSyms) {
        if (symIdxBySymbol.emplace(e, nextSymIdx).second) ++nextSymIdx;
    }

    // ── Build per-section relocation_info table ────────────────
    //
    // Each relocation_info is 8 bytes packed: i32 r_address +
    // u32 r_info. r_info packing (bit positions 0..31, LE):
    //   bits 0..23  : r_symbolnum (24 bits)
    //   bit  24     : r_pcrel
    //   bits 25..26 : r_length
    //   bit  27     : r_extern
    //   bits 28..31 : r_type
    //
    // The format JSON's `nativeId` packs (type<<28)|(length<<25)
    // |(pcrel<<24); the walker ORs in (1<<27) for r_extern + the
    // 24-bit symbol index.
    //
    // Same discipline as PE: Mach-O's `relocation_info` has no
    // addend column (per `<mach-o/reloc.h>`); addends live in the
    // section's patch bytes. ELF Rela is the outlier with its
    // explicit `r_addend`. Fail loud on non-zero addend so an
    // ELF-shaped input cannot silently drop the addend here.

    std::vector<std::uint8_t> textRelocs;
    std::uint32_t textRelocCount = 0;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        auto const& fn = module.functions[fi];
        std::uint64_t const fnStart = funcTextStart[fi];
        for (auto const& rel : fn.relocations) {
            if (rel.addend != 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"Mach-O writer: relocation in symbol #"}
                         + std::to_string(fn.symbol.v)
                         + " carries addend=" + std::to_string(rel.addend)
                         + " but Mach-O stores addends in the section's "
                           "patch bytes, not on relocation_info");
                return {};
            }
            // The `nullptr` / missing-symbol branches mirror ELF's
            // fail-loud discipline. `link()`'s cross-reference gate
            // makes these "unreachable", but silent `continue` is
            // the silent-failure class the substrate rejects — when
            // the gate ever drifts, the walker must surface the
            // problem (silent-failure C2 convergence).
            auto const* fmtReloc = fmt.relocationByKind(rel.kind);
            if (fmtReloc == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"Mach-O writer: relocation kind "}
                         + std::to_string(rel.kind.v)
                         + " is not declared by object format '"
                         + std::string{fmt.name()}
                         + "' — linker pre-walker gate should have "
                           "caught this; substrate-invariant violation");
                return {};
            }
            auto const it = symIdxBySymbol.find(rel.target);
            if (it == symIdxBySymbol.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"Mach-O writer: relocation target "
                                 "symbol #"}
                         + std::to_string(rel.target.v)
                         + " has no symtab entry — substrate-invariant "
                           "violation");
                return {};
            }
            std::uint32_t const symIdx = it->second;
            std::uint32_t const rAddress =
                static_cast<std::uint32_t>(fnStart + rel.offset);
            // Static portion of r_info (type|length|pcrel) lives in
            // nativeId; OR in r_extern (bit 27) + r_symbolnum
            // (low 24 bits). All Mach-O relocs in cycle scope are
            // extern (point at a symbol), so r_extern = 1.
            std::uint32_t const rInfo =
                fmtReloc->nativeId | (1u << 27) | (symIdx & 0x00FFFFFFu);
            appendU32LE(textRelocs, rAddress);
            appendU32LE(textRelocs, rInfo);
            ++textRelocCount;
        }
    }

    // ── Build nlist_64[] + string table ────────────────────────

    StringTable strtab;  // NulByte init — Mach-O n_strx=0 means "no name"
    std::vector<std::uint8_t> nlistBytes;

    auto appendNlist = [&](std::uint32_t nStrx, std::uint8_t nType,
                            std::uint8_t nSect, std::uint16_t nDesc,
                            std::uint64_t nValue) {
        appendU32LE(nlistBytes, nStrx);
        appendU8(nlistBytes, nType);
        appendU8(nlistBytes, nSect);
        appendU16LE(nlistBytes, nDesc);
        appendU64LE(nlistBytes, nValue);
    };

    constexpr std::uint8_t kTextSectionNumber = 1;

    // Defined function symbols: N_SECT|N_EXT, n_sect=1, n_value=offset.
    for (auto const& f : funcSyms) {
        std::string const symName =
            std::string{"_sym_"} + std::to_string(f.symId.v);
        std::uint32_t const nameOff = strtab.add(symName);
        appendNlist(nameOff,
                    static_cast<std::uint8_t>(N_SECT | N_EXT),
                    kTextSectionNumber,
                    /*n_desc=*/0,
                    f.valueInText);
    }
    // Undefined extern symbols: N_UNDF|N_EXT, n_sect=0, n_value=0.
    for (auto const& e : externSyms) {
        std::string const symName =
            std::string{"_sym_"} + std::to_string(e.v);
        std::uint32_t const nameOff = strtab.add(symName);
        appendNlist(nameOff,
                    static_cast<std::uint8_t>(N_UNDF | N_EXT),
                    /*n_sect=*/0,
                    /*n_desc=*/0,
                    /*n_value=*/0);
    }

    // Substrate invariant: every appendNlist writes exactly 16 bytes.
    if (nlistBytes.size() % kNlist64Size != 0) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"Mach-O writer: nlist table byte size "}
                 + std::to_string(nlistBytes.size())
                 + " is not a multiple of nlist_64 size (16)");
        return {};
    }
    std::uint32_t const numberOfSymbols =
        static_cast<std::uint32_t>(nlistBytes.size() / kNlist64Size);

    // ── Layout: header + load commands + section data + relocs
    //    + symtab + strtab ─────────────────────────────────────
    //
    // Section count is DERIVED from a per-emit vector below
    // (architect D-LK2-5 precedent — pre-fix LK2 hardcoded `1` and
    // had to be folded into a derived size; ELF was rewritten the
    // same way at LK1). The cycle-1 walker emits exactly one
    // section (`__text`), but the count flows through the
    // `mach_header_64.sizeofcmds` AND `LC_SEGMENT_64.cmdsize` AND
    // `LC_SEGMENT_64.nsects` derivations from `numSections`; a
    // hardcoded literal would silently desync those three when a
    // future cycle adds `__data`/`__const`.
    std::size_t const numSections = 1;  // __text only this cycle;
                                         // future cycles enumerate
                                         // emitted sections here.
    std::size_t const headerAndCommands =
        kMachHeader64Size
        + kSegmentCommand64Size + numSections * kSection64Size
        + kSymtabCommandSize;

    std::uint64_t const textRawOffset =
        static_cast<std::uint64_t>(headerAndCommands);
    std::uint64_t const textRawSize =
        static_cast<std::uint64_t>(textBody.size());
    std::uint64_t const textRelocOffset =
        textRelocCount > 0 ? textRawOffset + textRawSize : 0;
    std::uint64_t const textRelocSize =
        static_cast<std::uint64_t>(textRelocs.size());

    std::uint64_t const symtabOffset =
        textRawOffset + textRawSize + textRelocSize;
    std::uint64_t const stringTableOffset =
        symtabOffset + nlistBytes.size();
    std::uint64_t const stringTableSize =
        static_cast<std::uint64_t>(strtab.size());

    // ── Emit bytes ────────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(stringTableOffset + stringTableSize);

    auto const& id = fmt.macho();

    // mach_header_64
    appendU32LE(bytes, MH_MAGIC_64);
    appendU32LE(bytes, id.cputype);
    appendU32LE(bytes, id.cpusubtype);
    appendU32LE(bytes, static_cast<std::uint32_t>(id.filetype));
    appendU32LE(bytes, 2);  // ncmds: LC_SEGMENT_64 + LC_SYMTAB
    appendU32LE(bytes,
        static_cast<std::uint32_t>(kSegmentCommand64Size
                                    + numSections * kSection64Size
                                    + kSymtabCommandSize));
    appendU32LE(bytes, id.flags);
    appendU32LE(bytes, 0);  // reserved (64-bit padding)

    // LC_SEGMENT_64 (anonymous catch-all segment for MH_OBJECT)
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes,
        static_cast<std::uint32_t>(kSegmentCommand64Size
                                    + numSections * kSection64Size));
    appendName16(bytes, "");  // segname empty for MH_OBJECT
    appendU64LE(bytes, 0);    // vmaddr
    appendU64LE(bytes, textRawSize);  // vmsize
    appendU64LE(bytes, textRawOffset);  // fileoff
    appendU64LE(bytes, textRawSize);  // filesize
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRwx));  // maxprot
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRwx));  // initprot
    appendU32LE(bytes, static_cast<std::uint32_t>(numSections));  // nsects
    appendU32LE(bytes, 0);  // segment flags

    // section_64 for __text
    appendName16(bytes, secText->name);
    appendName16(bytes, secText->segment);
    appendU64LE(bytes, 0);  // addr (loaded later)
    appendU64LE(bytes, textRawSize);  // size
    appendU32LE(bytes, static_cast<std::uint32_t>(textRawOffset));
    appendU32LE(bytes, static_cast<std::uint32_t>(secText->addrAlign));
    appendU32LE(bytes, static_cast<std::uint32_t>(textRelocOffset));
    appendU32LE(bytes, textRelocCount);
    appendU32LE(bytes, secText->type);  // section flags from JSON
                                         // (cycle 1 ships S_REGULAR |
                                         // S_ATTR_PURE_INSTRUCTIONS |
                                         // S_ATTR_SOME_INSTRUCTIONS;
                                         // future cycles read JSON-
                                         // declared values verbatim)
    appendU32LE(bytes, 0);  // reserved1
    appendU32LE(bytes, 0);  // reserved2
    appendU32LE(bytes, 0);  // reserved3

    // LC_SYMTAB
    appendU32LE(bytes, LC_SYMTAB);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSymtabCommandSize));
    appendU32LE(bytes, static_cast<std::uint32_t>(symtabOffset));
    appendU32LE(bytes, numberOfSymbols);
    appendU32LE(bytes, static_cast<std::uint32_t>(stringTableOffset));
    appendU32LE(bytes, static_cast<std::uint32_t>(stringTableSize));

    // section data
    bytes.insert(bytes.end(), textBody.begin(), textBody.end());
    bytes.insert(bytes.end(), textRelocs.begin(), textRelocs.end());

    // symbol table
    bytes.insert(bytes.end(), nlistBytes.begin(), nlistBytes.end());

    // string table
    auto strtabBytes = std::move(strtab).release();
    bytes.insert(bytes.end(), strtabBytes.begin(), strtabBytes.end());

    return bytes;
}

// ── Mach-O MH_EXECUTE writer — plan 14 LK3 cycle 2 ─────────────
//
// Closes plan 14 §3.1 D-LK3-2. Emits a minimum-viable MH_EXECUTE
// the macOS loader will accept:
//   * __PAGEZERO segment at vmaddr=0 (catches null-deref).
//   * __TEXT segment at vmaddr=imageBase, contains __text.
//   * LC_LOAD_DYLINKER (/usr/lib/dyld).
//   * LC_MAIN with entryoff = entry function offset.
//   * LC_LOAD_DYLIB for each declared library.
//   * LC_SYMTAB.
//
// Intra-module relocations are applied in-place via the LK6 cycle
// 1 structured-formula triple — symmetric with PE / ELF EXEC.
// LC_DYLD_INFO chained-fixups (the modern Mach-O reloc form for
// MH_EXECUTE) arrive at LK6 cycle 2 paired with FFI.

namespace {

// LC_* command constants + commandSizeWithPath() hoisted to the
// file-level anonymous namespace at the top of this file (shared
// with encodeExecDynamic; code-simplifier REQUIRED fold, LK6
// cycle 2c review).

[[nodiscard]] std::vector<std::uint8_t>
encodeExec(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& fmt,
           ObjectFormatSectionInfo const& secText,
           DiagnosticReporter&       reporter) {
    auto const& id = fmt.macho();
    auto const& im = fmt.machoImage();

    // ── (a) Build .text body + per-function start map ─────────
    std::vector<std::uint8_t> textBody;
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        std::uint64_t const start = textBody.size();
        funcTextStart.push_back(start);
        textBody.insert(textBody.end(), fn.bytes.begin(), fn.bytes.end());
    }
    if (textBody.empty()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             "macho::encodeExec: `__text` is empty — every "
             "AssembledFunction contributed zero bytes.");
        return {};
    }
    // The static (no-extern) path emits no __TEXT,__const yet — a
    // read-only data global routes through encodeExecDynamic (the
    // global_int corpus reaches it via the _exit trampoline, which
    // appends an extern). Fail loud rather than silently drop the
    // data globals (the exec read-only data-section arm is dynamic-
    // only; mirrors ELF D-LK1-ELF-EXEC-DATA-SECTIONS).
    if (!module.dataItems.empty()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExec: {} AssembledData item(s) "
                         "present but the static (no-extern) Mach-O "
                         "path emits no __TEXT,__const — read-only data "
                         "globals are supported only on the dynamic "
                         "path (encodeExecDynamic). The exec read-only "
                         "data-section arm mirrors ELF "
                         "D-LK1-ELF-EXEC-DATA-SECTIONS.",
                         module.dataItems.size()));
        return {};
    }

    // ── (b) Resolve entry function index from schema.entryPoint
    // D-LK10-ENTRY Slice C audit fold: shared resolver. Mach-O
    // synthesized-name prefix is `_sym_` (leading underscore per
    // Apple convention).
    auto const entryIdxOpt = link::format::resolveEntryFnIdx(
        module, fmt, "_sym_", "macho::encodeExec", reporter);
    if (!entryIdxOpt.has_value()) return {};
    std::size_t const entryFnIdx = *entryIdxOpt;

    // ── (c) Apply intra-module relocations in-place ───────────
    //
    // Delegated to the shared `applyExecRelocations` kernel
    // (`link/format/exec_reloc_apply.hpp`). Mach-O-specific input:
    // sectionVa = secText.virtualAddress (section_64.addr; Mach-O
    // stores absolute VAs on sections, not RVA-from-ImageBase like
    // PE). Format-side fmt.relocationByKind check stays here.
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (fmt.relocationByKind(rel.kind) == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("macho::encodeExec: kind {} not "
                                 "declared by object format '{}' — "
                                 "substrate-invariant violation.",
                                 rel.kind.v, fmt.name()));
                return {};
            }
        }
    }
    // Build absolute symbol-VA map: function VA = section_64.addr
    // + offsetInText. This static path runs only when
    // externImports.empty() (the dispatch above routes extern-
    // bearing modules to encodeExecDynamic — D-LK6-5 closed at
    // LK6 cycle 2c).
    std::uint64_t const sectionVa = secText.virtualAddress;
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        symbolVa.emplace(module.functions[i].symbol,
                         sectionVa + funcTextStart[i]);
    }
    if (!link::format::applyExecRelocations(
            textBody, module, funcTextStart, symbolVa,
            targetSchema, sectionVa, "macho::encodeExec", reporter)) {
        return {};
    }

    // ── (d) Build LC_LOAD_DYLINKER + LC_LOAD_DYLIB sizes ──────
    //
    // Each path-bearing load command is `[u32 cmd][u32 cmdsize]
    // [u32 name_offset = 0x0C or 0x18][path bytes NUL-terminated]
    // [pad to 8]`. dyld reads cmdsize, finds the path at cmd+12 /
    // cmd+24, and resolves it.
    std::size_t const dylinkerCmdSize =
        commandSizeWithPath(12, im.dylinkerPath);
    std::size_t totalDylibCmdSize = 0;
    for (auto const& d : im.loadDylibs) {
        totalDylibCmdSize += commandSizeWithPath(24, d.path);
    }

    // ── (e) Layout constants ──────────────────────────────────
    //
    // ncmds = 2 (__PAGEZERO + __TEXT) + 1 (LC_LOAD_DYLINKER)
    //       + 1 (LC_MAIN) + N (LC_LOAD_DYLIB) + 1 (LC_SYMTAB).
    constexpr std::size_t kSegCmdPageZeroSize = kSegmentCommand64Size;
    constexpr std::size_t kSegCmdTextSize     =
        kSegmentCommand64Size + kSection64Size;  // 1 section: __text
    constexpr std::size_t kLcMainSize         = 24;

    // LK7 codesign reservation is unreachable here — the dispatch
    // in `encode()` gates `codeSignatureSize > 0` to the dynamic
    // path (anchored D-LK7-1 for static-path __LINKEDIT synthesis).
    // Keeping a defensive assertion so a future refactor that
    // bypasses the gate fails loud rather than silently emitting
    // an unsignable binary.
    if (im.codeSignatureSize != 0) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             "macho::encodeExec: invariant violation — the dispatch "
             "gate in macho::encode should have rejected non-zero "
             "codeSignatureSize on the static path. Anchored at "
             "plan 14 §3.1 D-LK7-1.");
        return {};
    }
    // LC_BUILD_VERSION is emitted only on the dynamic exec path
    // (encodeExecDynamic) — the sole path the runnable arm64-darwin
    // corpus uses. A static exec carrying `image.buildVersion` is a
    // legitimate future combination (e.g. an x86_64-darwin static exec
    // wanting a platform LC), but it has no shipped consumer today; fail
    // loud here rather than silently drop the platform command.
    // D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION (trigger: first static
    // Mach-O exec format that declares image.buildVersion).
    if (im.buildVersion.has_value()) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             "macho::encodeExec: 'image.buildVersion' (LC_BUILD_VERSION) "
             "is currently emitted only on the dynamic Mach-O exec path "
             "(encodeExecDynamic). The static path does not yet emit it "
             "— route through the dynamic exec path or omit "
             "image.buildVersion. Anchored "
             "D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION.");
        return {};
    }
    std::uint32_t const ncmds = static_cast<std::uint32_t>(
        2u + 1u + 1u + im.loadDylibs.size() + 1u);
    std::size_t const sizeofcmds =
        kSegCmdPageZeroSize + kSegCmdTextSize + dylinkerCmdSize
        + kLcMainSize + totalDylibCmdSize + kSymtabCommandSize;

    // ── (f) Build nlist_64 + string table — defined symbols only.
    //       (Extern dyld symbols flow through the encodeExecDynamic
    //       arm with N_UNDF|N_EXT entries; LK6 cycle 2c closed.)
    StringTable strtab;
    std::vector<std::uint8_t> nlistBytes;
    for (auto const& fn : module.functions) {
        std::string const symName =
            "_sym_" + std::to_string(fn.symbol.v);
        std::uint32_t const nameOff = strtab.add(symName);
        std::size_t const fi = static_cast<std::size_t>(&fn - module.functions.data());
        appendU32LE(nlistBytes, nameOff);
        appendU8(nlistBytes,
                 static_cast<std::uint8_t>(N_SECT | N_EXT));
        appendU8(nlistBytes, /*n_sect=*/1);
        appendU16LE(nlistBytes, /*n_desc=*/0);
        appendU64LE(nlistBytes,
                    sectionVa + funcTextStart[fi]);  // n_value = runtime VA
    }
    std::uint32_t const numberOfSymbols =
        static_cast<std::uint32_t>(module.functions.size());

    std::size_t const headerAndCmds = kMachHeader64Size + sizeofcmds;

    // .text body lives at file offset aligned to addrAlign (typical
    // 16 for x86_64), AFTER all load commands. The vmaddr of __text
    // (declared on the schema as virtualAddress) determines the
    // congruence: vmaddr % pageSize must equal fileOffset % pageSize
    // for the dyld mmap to map it correctly.
    //
    // Apple's loader convention: __TEXT segment's fileoff = 0 (the
    // mach header itself is "inside" __TEXT — the segment covers
    // [fileoff, fileoff+filesize) including the header). vmsize =
    // page-aligned size of all sections. Sections within __TEXT
    // have offset = (their byte position in the file).
    //
    // VM segment page size is config-driven (`image.segmentPageSize`):
    // 4 KiB for x86_64-darwin, 16 KiB (0x4000) for arm64-darwin (Apple
    // Silicon rejects 4 KiB-aligned segments with EBADMACHO). validate()
    // guarantees a power of two. (D-LK10-ENTRY-MACHO-EXIT.)
    std::uint64_t const kPageSize = im.segmentPageSize;
    std::uint64_t const textFileOff =
        alignUp(headerAndCmds, kPageSize);
    std::uint64_t const textFileSize = textBody.size();
    std::uint64_t const symtabOffset = textFileOff + textFileSize;
    std::uint64_t const stringTableOffset =
        symtabOffset + nlistBytes.size();
    std::uint64_t const stringTableSize = strtab.size();

    // __TEXT segment vmsize: page-aligned size from segment start
    // (vmaddr) covering all sections. Since __text starts at
    // section.virtualAddress and is textFileSize bytes, vmsize =
    // (section.virtualAddress - __TEXT.vmaddr) + textFileSize,
    // page-aligned. __TEXT.vmaddr = im.pageZeroSize (right after
    // __PAGEZERO).
    //
    // Defensive walker check (silent-failure H4 + code-reviewer C2
    // convergence): validate() rejects schemas where sectionVa <
    // pageZeroSize, but a programmatic / in-process construction
    // path could bypass that. Surface the underflow rather than
    // silently emitting a ~2^64 vmsize.
    if (sectionVa < im.pageZeroSize) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExec: section VA 0x{:x} is "
                         "below __TEXT vmaddr 0x{:x} (pageZeroSize) "
                         "— __TEXT would overlap __PAGEZERO; loader "
                         "rejects.",
                         sectionVa, im.pageZeroSize));
        return {};
    }
    std::uint64_t const textSegVmaddr = im.pageZeroSize;
    std::uint64_t const textSecOffsetInSeg = sectionVa - textSegVmaddr;
    std::uint64_t const textSegVmsize =
        alignUp(textSecOffsetInSeg + textFileSize, kPageSize);
    // __TEXT.filesize must NOT include the symtab/strtab bytes that
    // follow (those are outside any segment per Apple convention —
    // LC_SYMTAB references them by absolute file offset). Setting
    // filesize = vmsize (page-aligned) would silently make dyld
    // map the symtab as if it were code (silent-failure C2 + code-
    // reviewer C1 convergence). Use the byte-exact section span:
    // textSecOffsetInSeg + textFileSize.
    std::uint64_t const textSegFileSize = textSecOffsetInSeg + textFileSize;

    // entryoff for LC_MAIN: per <mach-o/loader.h> entry_point_command,
    // the offset is measured from the START of the __TEXT segment
    // (relative to __TEXT.fileoff). This walker emits __TEXT.fileoff
    // = 0 (Apple convention — the mach header itself sits inside
    // __TEXT), so the value also equals the absolute file offset of
    // the entry function. The dependency on `__TEXT.fileoff == 0`
    // is load-bearing — if a future cycle prepends a fat-header
    // wrapper or a signing prefix, this formula must subtract
    // __TEXT.fileoff to stay correct (architect O2 fix-up).
    std::uint64_t const entryOff = textFileOff + funcTextStart[entryFnIdx];

    // ── (g) Emit bytes ────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(stringTableOffset + stringTableSize);

    // mach_header_64
    appendU32LE(bytes, MH_MAGIC_64);
    appendU32LE(bytes, id.cputype);
    appendU32LE(bytes, id.cpusubtype);
    appendU32LE(bytes, static_cast<std::uint32_t>(id.filetype));
    appendU32LE(bytes, ncmds);
    appendU32LE(bytes, static_cast<std::uint32_t>(sizeofcmds));
    appendU32LE(bytes, id.flags);
    appendU32LE(bytes, 0);  // reserved (64-bit padding)

    // LC_SEGMENT_64 __PAGEZERO (vmaddr=0, vmsize=pageZeroSize, no prot)
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdPageZeroSize));
    appendName16(bytes, "__PAGEZERO");
    appendU64LE(bytes, 0);              // vmaddr
    appendU64LE(bytes, im.pageZeroSize); // vmsize
    appendU64LE(bytes, 0);              // fileoff
    appendU64LE(bytes, 0);              // filesize
    appendU32LE(bytes, 0);              // maxprot = 0
    appendU32LE(bytes, 0);              // initprot = 0
    appendU32LE(bytes, 0);              // nsects = 0
    appendU32LE(bytes, 0);              // segment flags

    // LC_SEGMENT_64 __TEXT (vmaddr=pageZeroSize, fileoff=0, R|X)
    constexpr std::int32_t kVmProtRx = 5;  // R|X
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdTextSize));
    appendName16(bytes, "__TEXT");
    appendU64LE(bytes, textSegVmaddr);
    appendU64LE(bytes, textSegVmsize);
    appendU64LE(bytes, 0);                // fileoff = 0 (mach header in __TEXT)
    appendU64LE(bytes, textSegFileSize);
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRx));
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRx));
    appendU32LE(bytes, 1);                // nsects = 1 (__text)
    appendU32LE(bytes, 0);                // flags

    // section_64 __text within __TEXT
    appendName16(bytes, secText.name);     // "__text"
    appendName16(bytes, secText.segment);  // "__TEXT"
    appendU64LE(bytes, sectionVa);
    appendU64LE(bytes, textFileSize);      // size
    appendU32LE(bytes, static_cast<std::uint32_t>(textFileOff));
    appendU32LE(bytes, static_cast<std::uint32_t>(secText.addrAlign));
    appendU32LE(bytes, 0);                 // reloff (no .o-style relocs)
    appendU32LE(bytes, 0);                 // nreloc
    appendU32LE(bytes, secText.type);      // flags from JSON
    appendU32LE(bytes, 0);                 // reserved1
    appendU32LE(bytes, 0);                 // reserved2
    appendU32LE(bytes, 0);                 // reserved3

    // LC_LOAD_DYLINKER
    {
        std::size_t const cmdStart = bytes.size();
        appendU32LE(bytes, LC_LOAD_DYLINKER);
        appendU32LE(bytes, static_cast<std::uint32_t>(dylinkerCmdSize));
        appendU32LE(bytes, 12);            // name offset = 12 (cmd+12)
        for (char c : im.dylinkerPath) appendU8(bytes, static_cast<std::uint8_t>(c));
        appendU8(bytes, 0);                // NUL terminator
        while (bytes.size() - cmdStart < dylinkerCmdSize) appendU8(bytes, 0);
    }

    // LC_MAIN
    appendU32LE(bytes, LC_MAIN);
    appendU32LE(bytes, static_cast<std::uint32_t>(kLcMainSize));
    appendU64LE(bytes, entryOff);
    appendU64LE(bytes, 0);                 // stacksize = 0 (use default)

    // LC_LOAD_DYLIB[]
    for (auto const& d : im.loadDylibs) {
        std::size_t const cmdStart = bytes.size();
        std::size_t const cmdSize  = commandSizeWithPath(24, d.path);
        appendU32LE(bytes, LC_LOAD_DYLIB);
        appendU32LE(bytes, static_cast<std::uint32_t>(cmdSize));
        appendU32LE(bytes, 24);            // name offset = 24 (cmd+24)
        appendU32LE(bytes, 0);             // timestamp = 0
        appendU32LE(bytes, 0);             // current_version
        appendU32LE(bytes, 0);             // compatibility_version
        for (char c : d.path) appendU8(bytes, static_cast<std::uint8_t>(c));
        appendU8(bytes, 0);                // NUL terminator
        while (bytes.size() - cmdStart < cmdSize) appendU8(bytes, 0);
    }

    // LC_SYMTAB
    appendU32LE(bytes, LC_SYMTAB);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSymtabCommandSize));
    appendU32LE(bytes, static_cast<std::uint32_t>(symtabOffset));
    appendU32LE(bytes, numberOfSymbols);
    appendU32LE(bytes, static_cast<std::uint32_t>(stringTableOffset));
    appendU32LE(bytes, static_cast<std::uint32_t>(stringTableSize));

    // Sanity-check that we emitted exactly sizeofcmds bytes of load
    // commands after the mach header. A mismatch silently corrupts
    // dyld's load-command parser; surface it.
    if (bytes.size() != kMachHeader64Size + sizeofcmds) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"macho::encodeExec: emitted "}
                 + std::to_string(bytes.size() - kMachHeader64Size)
                 + " bytes of load commands but sizeofcmds="
                 + std::to_string(sizeofcmds)
                 + " — substrate invariant violation.");
        return {};
    }

    // Pad to textFileOff
    while (bytes.size() < textFileOff) bytes.push_back(0);

    // __text body
    bytes.insert(bytes.end(), textBody.begin(), textBody.end());

    // symbol table + string table
    bytes.insert(bytes.end(), nlistBytes.begin(), nlistBytes.end());
    auto strtabBytes = std::move(strtab).release();
    bytes.insert(bytes.end(), strtabBytes.begin(), strtabBytes.end());

    return bytes;
}

} // namespace

// ── Mach-O MH_EXECUTE dynamic-linker writer — plan 14 LK6 cycle 2c ──
//
// Closes plan 14 §3.1 D-LK6-5 (x86_64 macOS arm). Emits a loadable
// dynamic MH_EXECUTE that PT-LOAD's into dyld + binds extern imports
// eagerly at startup via the immediate `LC_DYLD_INFO_ONLY.bind_off`
// opcode stream (parallel to ELF cycle 2b.2's `DF_1_NOW` + GLOB_DAT
// stance — `lazy_bind_off` stays zero). ARM64-darwin chained-fixups
// path anchored at D-LK6-14 (separate cycle — requires its own JSON
// + LC_DYLD_CHAINED_FIXUPS structure walker).
//
// Layout:
//   [mach_header_64]                                                  (32)
//   [LC_SEGMENT_64 __PAGEZERO]                                        (72)
//   [LC_SEGMENT_64 __TEXT + section_64{__text, __stubs}]              (72 + 80*2)
//   [LC_SEGMENT_64 __DATA_CONST + section_64{__got}]                  (72 + 80)
//   [LC_SEGMENT_64 __LINKEDIT]                                        (72)
//   [LC_DYLD_INFO_ONLY]                                               (48)
//   [LC_LOAD_DYLINKER]                                                (~24)
//   [LC_MAIN]                                                         (24)
//   [LC_LOAD_DYLIB[N]]                                                (~32 each)
//   [LC_SYMTAB]                                                       (24)
//   [LC_DYSYMTAB]                                                     (80)
//   [pad to page]
//   [__text bytes (intra-mod relocs applied; extern rel32 → __stubs)]
//   [__stubs bytes (6 B each: FF 25 disp32 → __got slot)]
//   [pad to 8]
//   [__got slots (8 B each, init 0; dyld fills at load via bind opcodes)]
//   [pad to page]
//   [__LINKEDIT: bind opcode stream]
//   [bind opcode stream → indirect symtab pad]
//   [indirect symtab (u32 per stub + per __got slot)]
//   [nlist_64[]   — defined externs first, undefined externs after]
//   [string table — NUL-seeded]
//
// `applyExecRelocations` (shared kernel) consumes `symbolVa` =
// { intra-module function SymbolIds → text VA } ∪
// { extern SymbolIds → __stubs slot VA } so the kernel's `S - P - 4`
// formula patches in-text rel32 to the stub. The stub then jumps
// through __got, which dyld fills at load.

namespace {

// LC_DYLD_INFO_ONLY / LC_DYSYMTAB / S_* / BIND_OPCODE_* /
// machoStubSizeFor + emitMachoStub / kGotSlotSize / appendULEB128 are
// file-level constants/helpers at the top of this file (shared with
// encodeExec; code-simplifier REQUIRED fold, LK6 cycle 2c review).

// D-LK6-14 chained-fixups payload builder hoisted to private
// header `link/format/macho_chained_fixups.hpp` at the d312c1c
// audit fold for direct unit testing. When D-LK6-14-INTEGRATION
// lands, encodeExecDynamic will `#include` the header and call
// `dss::macho::detail::buildChainedFixupsPayload()`.

[[nodiscard]] std::vector<std::uint8_t>
encodeExecDynamic(AssembledModule const&    module,
                  TargetSchema const&       targetSchema,
                  ObjectFormatSchema const& fmt,
                  ObjectFormatSectionInfo const& secText,
                  DiagnosticReporter&       reporter) {
    auto const& id = fmt.macho();
    auto const& im = fmt.machoImage();

    // ── (a) Preconditions ────────────────────────────────────────
    //
    // Walker contract guards (parallel to ELF cycle 2b.2). The
    // outer `encode()` already gated on externImports.empty() ==
    // false + filetype == Execute + bindNow == true. Defense-in-
    // depth + load-bearing inputs not enforced upstream.
    if (module.externImports.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "macho::encodeExecDynamic: called with zero "
             "externImports — dynamic-image emission requires at "
             "least one extern; static images route through "
             "encodeExec.");
        return {};
    }
    // D-LK6-14 substrate: the useChainedFixups guard now fires at
    // outer `encode()` (covers both encodeExec + encodeExecDynamic
    // paths uniformly). Reaching this point means the schema set
    // useChainedFixups=false → legacy LC_DYLD_INFO_ONLY path.
    if (module.functions.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "macho::encodeExecDynamic: zero functions — MH_EXECUTE "
             "needs at least one entry function.");
        return {};
    }
    // VM segment page size is config-driven (`image.segmentPageSize`):
    // 4 KiB x86_64-darwin / 16 KiB (0x4000) arm64-darwin. Apple Silicon
    // rejects 4 KiB-aligned segments with EBADMACHO. validate() proves
    // it is a power of two. (D-LK10-ENTRY-MACHO-EXIT.)
    std::uint64_t const kPageSize = im.segmentPageSize;
    std::uint64_t const sectionVa = secText.virtualAddress;
    if (sectionVa < im.pageZeroSize) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: section VA 0x{:x} "
                         "is below __TEXT vmaddr 0x{:x} "
                         "(pageZeroSize) — __TEXT would overlap "
                         "__PAGEZERO; loader rejects.",
                         sectionVa, im.pageZeroSize));
        return {};
    }
    if (sectionVa < kPageSize) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: section VA 0x{:x} "
                         "is below kPageSize 0x{:x} — __stubs/__got "
                         "layout requires room above sectionVa within "
                         "the same page.", sectionVa, kPageSize));
        return {};
    }
    // Mmap-congruence guard: __TEXT.fileoff = 0 by convention, so
    // __TEXT.vmaddr (= pageZeroSize) and sectionVa - pageZeroSize
    // must both be page-aligned for the kernel to accept the
    // load. validate() enforces pageZeroSize-power-of-two, but the
    // sectionVa offset within __TEXT could still be off if an
    // unusual JSON sets virtualAddress to a non-page-aligned value.
    // Surface here rather than silently emit a corrupt segment.
    // (silent-failure-hunter MEDIUM, code-reviewer I1 fold)
    if ((sectionVa - im.pageZeroSize) % kPageSize != 0) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: section VA 0x{:x} "
                         "is not page-aligned relative to "
                         "pageZeroSize 0x{:x} (delta {} bytes "
                         "violates kernel mmap congruence "
                         "vmaddr % page == fileoff % page).",
                         sectionVa, im.pageZeroSize,
                         (sectionVa - im.pageZeroSize) % kPageSize));
        return {};
    }
    // Format-side fmt.relocationByKind pre-flight.
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (fmt.relocationByKind(rel.kind) == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("macho::encodeExecDynamic: kind {} not "
                                 "declared by object format '{}' — "
                                 "substrate-invariant violation.",
                                 rel.kind.v, fmt.name()));
                return {};
            }
        }
    }

    // Dispatch ↔ symbolVa-target coherence guard (D-FFI-EXTERN-CALL-
    // DISPATCH). This walker points every extern's symbolVa at its
    // `__stubs` STUB (see `symbolVa.emplace(..., stubVa)` below) and the
    // stub does the __got indirection — that is `direct-plt` semantics.
    // A format that declares `indirect-slot` would make the call site
    // DEREFERENCE the stub's CODE bytes as a function pointer (`FF 15`
    // through stubVa) → SIGSEGV at the first extern call. The two facts
    // (where symbolVa points + the declared call shape) MUST agree;
    // surface the contradiction at link rather than emit a binary that
    // crashes at runtime. (`direct-plt` and nullopt are both fine here:
    // direct-plt matches the stub target; a nullopt format would already
    // have fail-louded at MIR→LIR `lowerCall` for any module with
    // externs — defense-in-depth, no silent acceptance of indirect-slot.)
    if (fmt.externCallDispatch().has_value()
     && externCallUsesIndirectShape(*fmt.externCallDispatch())) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             std::format("macho::encodeExecDynamic: object format '{}' "
                         "declares externCallDispatch='indirect-slot' but "
                         "the Mach-O walker points each extern symbol's "
                         "VA at its __stubs STUB (direct-plt semantics) "
                         "— an indirect-slot call site would dereference "
                         "the stub's code bytes as a function pointer and "
                         "SIGSEGV. Set externCallDispatch='direct-plt' "
                         "(Mach-O symbolVa→stub is direct-plt, like ELF). "
                         "D-FFI-EXTERN-CALL-DISPATCH.",
                         fmt.name()));
        return {};
    }

    std::size_t const numExterns = module.externImports.size();

    // ── (b) Group externs by library (preserve declaration order)
    std::vector<std::string> libraryOrder;
    std::unordered_map<std::string, std::uint32_t> libOrdinal;
    for (auto const& ext : module.externImports) {
        if (libOrdinal.emplace(ext.libraryPath,
                static_cast<std::uint32_t>(libraryOrder.size())).second) {
            libraryOrder.push_back(ext.libraryPath);
        }
    }
    std::size_t const numLibs = libraryOrder.size();
    if (numLibs == 0) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "macho::encodeExecDynamic: " +
             std::to_string(numExterns) +
             " extern(s) declared but every `libraryPath` is empty "
             "— zero LC_LOAD_DYLIB entries would ship; dyld cannot "
             "resolve undefined symbols without a library.");
        return {};
    }
    // Mach-O dylib ordinals are 1-based (0 is reserved for
    // BIND_SPECIAL_DYLIB_SELF). The bind opcode uses the immediate
    // form (BIND_OPCODE_SET_DYLIB_ORDINAL_IMM) only for ordinals
    // in 1..15 (4-bit immediate). Beyond that, the opcode falls
    // back to the ULEB form (SET_DYLIB_ORDINAL_ULEB). Use the IMM
    // form when possible — same byte cost (1B opcode + 1B ULEB =
    // 2B for IMM-able ordinals; 1B opcode + 1B opcode = 2B for the
    // ULEB form), but IMM is the conventional shape Apple's ld64
    // emits and dyld's hot path optimizes for.

    // ── (c) Build .text body + per-function start map ────────────
    std::vector<std::uint8_t> textBody;
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        funcTextStart.push_back(textBody.size());
        textBody.insert(textBody.end(), fn.bytes.begin(), fn.bytes.end());
    }
    if (textBody.empty()) {
        emit(reporter, DiagnosticCode::K_SymbolUndefined,
             "macho::encodeExecDynamic: every AssembledFunction "
             "contributed zero bytes — `__text` is empty.");
        return {};
    }

    // ── (d) Resolve entry function index — shared resolver
    // (D-LK10-ENTRY Slice C audit fold).
    auto const entryIdxDynOpt = link::format::resolveEntryFnIdx(
        module, fmt, "_sym_", "macho::encodeExecDynamic", reporter);
    if (!entryIdxDynOpt.has_value()) return {};
    std::size_t const entryFnIdx = *entryIdxDynOpt;

    // ── (e) Symbol-VA map: intra-fn VAs + extern __stubs slot VAs.
    //       __stubs lives right after __text within __TEXT.
    //
    // Per-cputype __stubs entry stride (6 for x86_64, 12 for arm64).
    // Computed ONCE here and threaded through every layout site below
    // so the stub size is single-sourced. A 0 (unhandled cputype) is
    // a fail-loud up front — defense-in-depth ahead of `emitMachoStub`,
    // which also fails loud, but a 0 stride would corrupt the layout
    // arithmetic before emission if not caught here.
    std::size_t const stubSize = machoStubSizeFor(id.cputype);
    if (stubSize == 0u) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             std::format("macho::encodeExecDynamic: cputype 0x{:x} has "
                         "no __stubs entry size — the Mach-O dynamic-"
                         "import path supports x86_64 (0x{:x}) and arm64 "
                         "(0x{:x}). Add a per-cputype arm to "
                         "machoStubSizeFor + emitMachoStub.",
                         id.cputype, kCpuTypeX86_64, kCpuTypeArm64));
        return {};
    }
    std::uint64_t const stubsVa = sectionVa + textBody.size();
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size() + numExterns);
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        if (!symbolVa.emplace(module.functions[i].symbol,
                              sectionVa + funcTextStart[i]).second) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 "macho::encodeExecDynamic: duplicate function "
                 "symbol #" +
                 std::to_string(module.functions[i].symbol.v) +
                 " — caller must give each function a unique "
                 "SymbolId.");
            return {};
        }
    }
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint64_t const stubVa =
            stubsVa + static_cast<std::uint64_t>(i) * stubSize;
        if (!symbolVa.emplace(module.externImports[i].symbol,
                              stubVa).second) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 "macho::encodeExecDynamic: extern SymbolId #" +
                 std::to_string(module.externImports[i].symbol.v) +
                 " collides with another symbol — caller must give "
                 "each extern a unique SymbolId distinct from "
                 "function ids.");
            return {};
        }
    }

    // ── (e.5) D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror: __TEXT,__const ──────
    //
    // A loadable read-only data section folded into the R+X __TEXT
    // segment (mirrors the ELF `.rodata`-in-R+X-PT_LOAD arm, commit
    // 8040410). `__const` holds the module's read-only data globals
    // (`int answer=42;`). Like ELF, the section's VA is computed
    // EARLY here (from `stubsVa` + the stubs byte count, both known
    // now) so a NAMED data symbol can join `symbolVa` BEFORE the
    // shared `applyExecRelocations` kernel runs — a code reloc into
    // the data symbol (the `lea`/ADRP+ADD) then resolves through the
    // SAME kernel, no rodata loop added to it. The matching FILE
    // offset (`constFileOff`/`constEnd`) is computed once the layout
    // is known below, and a fail-loud guard asserts file/VA
    // congruence inside __TEXT (__TEXT.fileoff = 0 ⇒ VA−fileoff is
    // constant). Validation + byte layout + the H1 section-align come
    // from the shared `buildExecRodata` substrate (the SAME helper
    // the ELF writer uses — single-sourced, format-neutral). EVERY
    // change below is gated on `hasConst` so an empty `dataItems`
    // leaves the output byte-identical to the no-data path (the
    // mandatory control).
    // Read-only globals → __TEXT,__const (D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O
    // mirror). Mutable initialized → __DATA,__data; zero-init → __DATA,__bss
    // (S_ZEROFILL) — D-LK4-DATA-PRODUCER. Each kind laid out by the SAME shared
    // kind-parameterized helper the ELF writer uses (single-sourced, format-
    // neutral). The `__const` arm is gated on `hasConst`, the writable arms on
    // `hasData`/`hasBss`, so an empty `dataItems` is byte-identical to the
    // no-data path (the mandatory control).
    ObjectFormatSectionInfo const* secConst =
        fmt.sectionByKind(SectionKind::Rodata);
    ObjectFormatSectionInfo const* secData =
        fmt.sectionByKind(SectionKind::Data);
    ObjectFormatSectionInfo const* secBss =
        fmt.sectionByKind(SectionKind::Bss);
    auto const constLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Rodata,
        secConst != nullptr ? secConst->addrAlign : 1,
        "macho::encodeExecDynamic", reporter);
    if (!constLayoutOpt.has_value()) return {};
    auto const& constLayout = *constLayoutOpt;
    auto const dataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Data,
        secData != nullptr ? secData->addrAlign : 1,
        "macho::encodeExecDynamic", reporter);
    if (!dataLayoutOpt.has_value()) return {};
    auto const& dataLayout = *dataLayoutOpt;
    auto const bssLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Bss,
        secBss != nullptr ? secBss->addrAlign : 1,
        "macho::encodeExecDynamic", reporter);
    if (!bssLayoutOpt.has_value()) return {};
    auto const& bssLayout = *bssLayoutOpt;
    bool const hasConst = !constLayout.empty();
    bool const hasData  = !dataLayout.empty();
    bool const hasBss   = !bssLayout.empty();
    bool const hasDataSeg = hasData || hasBss;   // needs a __DATA segment
    std::uint64_t const constSize = constLayout.spanSize;
    // Each present section's schema row is MANDATORY — fail loud (the format
    // JSON must declare it). The rows feed the section_64 records below.
    if (hasConst && secConst == nullptr) return {};
    if (hasData && secData == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "macho::encodeExecDynamic: module carries Data items but the "
             "format declares no 'data' section row (D-LK4-DATA-PRODUCER).");
        return {};
    }
    if (hasBss && secBss == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "macho::encodeExecDynamic: module carries Bss items but the "
             "format declares no 'bss' section row (D-LK4-DATA-PRODUCER).");
        return {};
    }
    // __stubs occupies `numExterns * stubSize` bytes contiguously
    // after __text; __const starts at the H1-aligned VA above them.
    std::uint64_t const stubsBytes =
        static_cast<std::uint64_t>(numExterns) * stubSize;
    std::uint64_t const constSectionVa =
        hasConst
            ? alignUp(stubsVa + stubsBytes, constLayout.maxAlign)
            : 0;
    // Each NAMED __const item joins symbolVa at `constSectionVa +
    // itemOffsets[j]` (anonymous SymbolId{} items skipped — M1). A
    // duplicate symbol fails loud (K_DuplicateDataSymbol). __data/__bss item
    // VAs are registered later (their VAs derive from the __DATA segment
    // layout, computed after __DATA_CONST).
    if (hasConst
        && !link::format::addDataSymbolVas(
               module.dataItems, constLayout, constSectionVa,
               symbolVa, "macho::encodeExecDynamic", reporter)) {
        return {};
    }
    // __data / __bss VAs (D-LK4-DATA-PRODUCER) — computed EARLY so a `.text`
    // load/store of a mutable global resolves through `applyExecRelocations`
    // below (a global access lowers to ADRP+ADD / lea against the global's
    // SymbolId). They live in a NEW writable `__DATA` segment placed after
    // `__DATA_CONST` (the GOT); both `__DATA_CONST` and `__DATA` are page-
    // aligned segments (dyld maps each independently), so the VAs derive from
    // the same page-aligned chain the LATE layout recomputes via file-offset
    // deltas — the LATE layout asserts congruence (a self-check that fires
    // fail-loud if this early arithmetic ever diverges). `__bss` is S_ZEROFILL:
    // it occupies VM (vmsize) but NO file bytes, and is the LAST section in
    // `__DATA`. Sizes: __got = numExterns*8 (one page-aligned segment).
    std::uint64_t const gotBytesEarly =
        static_cast<std::uint64_t>(numExterns) * kGotSlotSize;
    std::uint64_t const dataConstEndVaEarly =
        alignUp((hasConst ? constSectionVa + constSize
                          : stubsVa + stubsBytes),
                kPageSize)            // __DATA_CONST.vmaddr (page boundary)
        + gotBytesEarly;             // + __got contents
    std::uint64_t const dataSegVaEarly =
        hasDataSeg ? alignUp(dataConstEndVaEarly, kPageSize) : 0;
    std::uint64_t const dataSecVa =
        hasData ? alignUp(dataSegVaEarly, dataLayout.maxAlign) : 0;
    std::uint64_t const bssSecVa =
        hasBss ? alignUp((hasData ? dataSecVa + dataLayout.spanSize
                                  : dataSegVaEarly),
                         bssLayout.maxAlign)
               : 0;
    if (hasData
        && !link::format::addDataSymbolVas(
               module.dataItems, dataLayout, dataSecVa,
               symbolVa, "macho::encodeExecDynamic", reporter)) {
        return {};
    }
    if (hasBss
        && !link::format::addDataSymbolVas(
               module.dataItems, bssLayout, bssSecVa,
               symbolVa, "macho::encodeExecDynamic", reporter)) {
        return {};
    }

    // ── (f) Apply intra-module relocations in-place ─────────────
    if (!link::format::applyExecRelocations(
            textBody, module, funcTextStart, symbolVa,
            targetSchema, sectionVa, "macho::encodeExecDynamic",
            reporter)) {
        return {};
    }

    // ── (g) Build __stubs body: N per-cputype stubs, each reaching
    //       __got slot N. x86_64 = 6-byte `FF 25 disp32` (PC-relative
    //       jmp through the slot); arm64 = 12-byte ADRP x16 → LDR x16,
    //       [x16,#lo12] → BR x16. __got lives in __DATA_CONST at a
    //       separate VA computed once the section layout is known
    //       below. The actual byte emission is `emitMachoStub` (the
    //       file-level stub substrate); see section (l) where gotVa is
    //       resolved.

    // ── (h) Build LC_LOAD_DYLINKER + LC_LOAD_DYLIB sizes ────────
    std::size_t const dylinkerCmdSize =
        commandSizeWithPath(12, im.dylinkerPath);
    std::size_t totalDylibCmdSize = 0;
    for (auto const& d : im.loadDylibs) {
        totalDylibCmdSize += commandSizeWithPath(24, d.path);
    }
    // Linker validates non-empty loadDylibs upstream; defense-in-
    // depth makes sure every library referenced by an externImport
    // is in loadDylibs. dyld rejects bind opcodes referring to
    // ordinals not present in LC_LOAD_DYLIB[].
    std::unordered_set<std::string> declaredLibs;
    declaredLibs.reserve(im.loadDylibs.size());
    for (auto const& d : im.loadDylibs) declaredLibs.insert(d.path);
    for (auto const& lib : libraryOrder) {
        if (!declaredLibs.contains(lib)) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"macho::encodeExecDynamic: extern "
                             "imports reference library '"} + lib +
                 "' but it is not declared in image.loadDylibs — "
                 "dyld rejects bind opcodes referring to undeclared "
                 "dylib ordinals.");
            return {};
        }
    }
    // Stable map: library → dylib ordinal (1-based per dyld).
    auto dylibOrdinal = [&](std::string const& path)
                          -> std::uint32_t {
        for (std::size_t i = 0; i < im.loadDylibs.size(); ++i) {
            if (im.loadDylibs[i].path == path) {
                return static_cast<std::uint32_t>(i + 1);
            }
        }
        return 0;  // unreachable — declaredLibs check above
    };

    // ── (i) Build dyld-binding bytes ─────────────────────────────
    //
    // Two paths: the legacy LC_DYLD_INFO_ONLY bind-opcode stream OR
    // the modern LC_DYLD_CHAINED_FIXUPS payload. Schema flag
    // `image.useChainedFixups` selects. The resulting `dyldBindBlob`
    // is what lands in __LINKEDIT at `dyldBindOff`; the load-command
    // emission below picks the matching LC.
    //
    // D-LK6-14-INTEGRATION-PAYLOAD (this commit): the chained-fixups
    // arm calls `dss::macho::detail::buildChainedFixupsPayload` and
    // emits LC_DYLD_CHAINED_FIXUPS pointing at the result. __got
    // slots remain zero-initialized — D-LK6-14-INTEGRATION-GOT-SLOTS
    // is the companion fold that populates them as
    // DYLD_CHAINED_PTR_64 bitfields + drops LC_DYSYMTAB.
    bool const useChainedFixups = im.useChainedFixups;
    std::vector<std::uint8_t> dyldBindBlob;
    // `chainedImports` is built in section (i) for the chained path
    // and consumed AFTER layout (section l.5 below) when
    // `segmentOffset = gotVa - im.pageZeroSize` is known (per Apple
    // convention __TEXT.vmaddr == im.pageZeroSize on PIE binaries).
    // The payload contains a `dyld_chained_starts_in_segment` struct
    // whose `segment_offset` u64 must reference the actual
    // __DATA_CONST VM offset — that value depends on `sizeofcmds →
    // textFileOff → gotFileOff → gotVa` computed in section (l).
    // Hoisting the imports here keeps the pre-check loud while
    // deferring payload bytes.
    std::vector<dss::macho::detail::ChainedFixupImport> chainedImports;
    if (useChainedFixups) {
        // D-LK6-14-NAME-OFFSET-OVERFLOW close: pre-check the
        // cumulative symbols-pool size before payload construction
        // (the helper's 23-bit name_offset field would silently
        // truncate offsets > 8 MiB - 1; the mask in
        // buildChainedFixupsPayload is defense-in-depth only).
        // Leading NUL sentinel + N × (name.size() + 1) NUL-terminator.
        std::uint64_t cumulativeSymbolsPoolSize = 1u;  // leading NUL
        for (auto const& ext : module.externImports) {
            cumulativeSymbolsPoolSize +=
                static_cast<std::uint64_t>(ext.mangledName.size()) + 1u;
        }
        if (cumulativeSymbolsPoolSize >
            dss::macho::detail::kDyldChainedImportNameOffsetMax) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::format(
                     "macho::encodeExecDynamic: chained-fixups "
                     "symbols pool ({} bytes) exceeds the 23-bit "
                     "name_offset field (max {} = 8 MiB - 1). The "
                     "DYLD_CHAINED_IMPORT struct cannot represent "
                     "name offsets above this bound; either reduce "
                     "the number/length of extern imports, or set "
                     "`image.useChainedFixups` = false to fall back "
                     "to LC_DYLD_INFO_ONLY (which has no analogous "
                     "limit). Anchored D-LK6-14-NAME-OFFSET-OVERFLOW.",
                     cumulativeSymbolsPoolSize,
                     static_cast<std::uint64_t>(
                         dss::macho::detail::
                             kDyldChainedImportNameOffsetMax)));
            return {};
        }
        chainedImports.reserve(numExterns);
        for (auto const& ext : module.externImports) {
            std::uint32_t const ord = dylibOrdinal(ext.libraryPath);
            // DYLD_CHAINED_IMPORT.lib_ordinal is a SIGNED 8-bit field.
            // Ordinals 1..127 are valid; > 127 is architecturally
            // unreachable for real binaries but a config-fuzz with
            // >127 LC_LOAD_DYLIB entries would silently truncate.
            if (ord > 127u) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::format(
                         "macho::encodeExecDynamic: dylib ordinal {} "
                         "exceeds chained-fixups signed-8-bit field "
                         "(max 127). Reduce LC_LOAD_DYLIB count or "
                         "fall back to LC_DYLD_INFO_ONLY.", ord));
                return {};
            }
            // D-LK6-14-MACHO-WEAK-DEF: weakImport hardcoded false.
            // Trigger to ship: first `__attribute__((weak_import))`
            // from FF2's C-header parser (or first schema field
            // requesting weak-import semantics). Bit-8 of the
            // packed DYLD_CHAINED_IMPORT row.
            chainedImports.push_back({
                ext.mangledName,
                static_cast<std::int8_t>(ord),
                false  // weakImport — D-LK6-14-MACHO-WEAK-DEF
            });
        }
        // Payload bytes deferred to section (k) below — segment_offset
        // resolution requires gotVa.
    } else {
        // Legacy LC_DYLD_INFO_ONLY bind opcode stream.
        //
        // For each extern, emit:
        //   SET_DYLIB_ORDINAL_(IMM|ULEB)  <ord>
        //   SET_SYMBOL_TRAILING_FLAGS_IMM 0 + symbol name + NUL
        //   SET_TYPE_IMM BIND_TYPE_POINTER         (once is enough but
        //                                            re-emit per extern
        //                                            for clarity)
        //   SET_SEGMENT_AND_OFFSET_ULEB  <__DATA_CONST seg idx> <offset>
        //   DO_BIND
        // Then BIND_OPCODE_DONE.
        //
        // Segment index is 0-based in BIND_OPCODE_SET_SEGMENT_AND_
        // OFFSET_ULEB's 4-bit immediate. Segments are numbered in the
        // order they appear via LC_SEGMENT_64: __PAGEZERO=0, __TEXT=1,
        // __DATA_CONST=2, __LINKEDIT=3 (this walker's emission order).
        // If the emission order ever changes, this constant must too.
        // (code-reviewer I2 fold — comment said "1-based" then
        // enumerated 0-based, contradicting itself.)
        constexpr std::uint8_t kSegIdxDataConst = 2;
        for (std::size_t i = 0; i < numExterns; ++i) {
            auto const& ext = module.externImports[i];
            std::uint32_t const ord = dylibOrdinal(ext.libraryPath);
            if (ord <= 0x0F) {
                dyldBindBlob.push_back(static_cast<std::uint8_t>(
                    BIND_OPCODE_SET_DYLIB_ORDINAL_IMM |
                    static_cast<std::uint8_t>(ord & 0x0F)));
            } else {
                dyldBindBlob.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
                appendULEB128(dyldBindBlob, ord);
            }
            dyldBindBlob.push_back(static_cast<std::uint8_t>(
                BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | 0));
            // Mach-O bind symbols are conventionally prefixed with `_`
            // for C ABI compatibility. The walker emits the
            // `mangledName` verbatim — the caller (linker / assembler)
            // is responsible for adding the underscore in mangledName
            // if the symbol's source language uses C-style mangling.
            for (char c : ext.mangledName)
                dyldBindBlob.push_back(static_cast<std::uint8_t>(c));
            dyldBindBlob.push_back(0);  // NUL terminator
            dyldBindBlob.push_back(BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);
            dyldBindBlob.push_back(
                BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | kSegIdxDataConst);
            appendULEB128(dyldBindBlob,
                static_cast<std::uint64_t>(i) * kGotSlotSize);
            dyldBindBlob.push_back(BIND_OPCODE_DO_BIND);
        }
        dyldBindBlob.push_back(BIND_OPCODE_DONE);
        // dyld parses the bind stream byte-by-byte; pad to 8 for
        // load-command alignment downstream.
        while (dyldBindBlob.size() % kLoadCmdAlign != 0)
            dyldBindBlob.push_back(0);
    }

    // ── (j) Indirect symbols table: one u32 per stub slot + one
    //       u32 per __got slot. Each entry is the index into
    //       nlist_64 of the matching undefined extern. nlist_64
    //       ordering: defined externs first (functions),
    //       undefined externs next — so undef indices are
    //       [numDefs .. numDefs + numExterns).
    std::uint32_t const numDefs =
        static_cast<std::uint32_t>(module.functions.size());
    std::vector<std::uint32_t> indirectSyms;
    if (!useChainedFixups) {
        // D-LK6-14-INTEGRATION-GOT-SLOTS: chained pointers in __got
        // encode the import ordinal directly, so the indirect symbol
        // table is redundant. Skip construction (and the matching
        // LC_DYSYMTAB + __LINKEDIT emission below) on chained path.
        indirectSyms.reserve(numExterns * 2);
        for (std::size_t i = 0; i < numExterns; ++i)
            indirectSyms.push_back(numDefs + static_cast<std::uint32_t>(i));
        for (std::size_t i = 0; i < numExterns; ++i)
            indirectSyms.push_back(numDefs + static_cast<std::uint32_t>(i));
    }

    // ── (k) Build nlist_64 + string table: defined externs first,
    //       undefined externs next.
    StringTable strtab;
    std::vector<std::uint8_t> nlistBytes;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto const& fn = module.functions[i];
        std::string const symName =
            "_sym_" + std::to_string(fn.symbol.v);
        std::uint32_t const nameOff = strtab.add(symName);
        appendU32LE(nlistBytes, nameOff);
        appendU8(nlistBytes,
                 static_cast<std::uint8_t>(N_SECT | N_EXT));
        appendU8(nlistBytes, /*n_sect=*/1);
        appendU16LE(nlistBytes, /*n_desc=*/0);
        appendU64LE(nlistBytes, sectionVa + funcTextStart[i]);
    }
    for (std::size_t i = 0; i < numExterns; ++i) {
        auto const& ext = module.externImports[i];
        std::uint32_t const nameOff = strtab.add(ext.mangledName);
        std::uint32_t const ord = dylibOrdinal(ext.libraryPath);
        appendU32LE(nlistBytes, nameOff);
        appendU8(nlistBytes,
                 static_cast<std::uint8_t>(N_UNDF | N_EXT));
        appendU8(nlistBytes, /*n_sect=*/0);  // undefined
        // n_desc bits 8..15 = LIBRARY_ORDINAL (1-based dylib idx).
        appendU16LE(nlistBytes,
                    static_cast<std::uint16_t>(ord << 8));
        appendU64LE(nlistBytes, 0);  // n_value = 0 for undefined
    }
    std::uint32_t const numberOfSymbols =
        numDefs + static_cast<std::uint32_t>(numExterns);

    // ── (l) Layout: compute file offsets for everything ─────────
    //
    // Layout order:
    //   header + load commands + pad → __text → __stubs → __got
    //   (__got page-aligned, in __DATA_CONST) → pad → __LINKEDIT
    //   (bind opcodes → indirect symtab → nlist → strtab)

    constexpr std::size_t kSegCmdPageZeroSize = kSegmentCommand64Size;
    // __TEXT carries __text + __stubs, plus __const when data globals
    // are present (D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror). One extra 80-byte
    // section_64 grows `sizeofcmds` → `headerAndCmds` →
    // `textFileOff = alignUp(headerAndCmds, kPageSize)`. The full
    // load-command region is well under one page (~840 bytes for the
    // arm64 corpus + 80 = ~920 ≪ 0x4000 segmentPageSize), so
    // `textFileOff` stays 0x4000 and `__text.virtualAddress`
    // congruence (the guard at the top of this function) still holds.
    std::size_t const kSegCmdTextSize =
        kSegmentCommand64Size + kSection64Size * (hasConst ? 3u : 2u);
    constexpr std::size_t kSegCmdDataConstSize =
        kSegmentCommand64Size + kSection64Size;      // __got
    // __DATA segment (D-LK4-DATA-PRODUCER) — one LC_SEGMENT_64 + a section_64
    // per present writable section (__data and/or __bss). Absent when the module
    // has no writable globals (byte-identical to the pre-data image).
    std::uint32_t const dataSegNsects =
        (hasData ? 1u : 0u) + (hasBss ? 1u : 0u);
    std::size_t const kSegCmdDataSize =
        hasDataSeg
            ? kSegmentCommand64Size + kSection64Size * dataSegNsects
            : 0u;
    constexpr std::size_t kSegCmdLinkeditSize = kSegmentCommand64Size;
    constexpr std::size_t kLcMainSize         = 24;

    // LK7: when `codeSignatureSize > 0` OR an ad-hoc `codeSignature`
    // block is present, append LC_CODE_SIGNATURE (16-byte
    // linkedit_data_command). The reservation lives at the tail of
    // __LINKEDIT so the fill (this cycle, for the ad-hoc block; or
    // plan 16's full Apple SuperBlob) lands without disturbing earlier
    // layout.
    //
    // D-LK7-ADHOC-CODESIGN-MACHO (increment 2/2): when `codeSignature`
    // is set the reservation size is DERIVED from the ad-hoc blob's
    // exact byte length — `adHocCodeSignatureSize` over the SAME
    // (codeLimit, pageSize, identifier) the fill uses below — NOT the
    // hand-typed `codeSignatureSize`. `codeLimit` (= the signature's
    // file offset) is not known until the __LINKEDIT layout completes,
    // so the derivation happens at the `codeSigReserveSize` assignment
    // further down (after `codeSigFileOff`). Here we only decide
    // whether to emit the load command at all.
    bool const emitCodeSig =
        im.codeSignatureSize != 0 || im.codeSignature.has_value();
    // LC_BUILD_VERSION (platform/min-OS) is emitted iff the schema
    // declares `image.buildVersion` — required for the image to load on
    // macOS 11+ / Apple Silicon (D-LK10-ENTRY-MACHO-EXIT).
    bool const emitBuildVersion = im.buildVersion.has_value();
    // ncmds = 4 segments + LC_DYLD_{INFO_ONLY|CHAINED_FIXUPS}
    //       + LC_LOAD_DYLINKER + LC_MAIN + N × LC_LOAD_DYLIB
    //       + LC_SYMTAB + (LC_DYSYMTAB when !useChainedFixups —
    //       D-LK6-14-INTEGRATION-GOT-SLOTS drops it because chained
    //       pointers in __got encode the import ordinal directly, so
    //       the indirect symbol table is redundant) + LC_CODE_SIGNATURE
    //       when emitCodeSig + LC_BUILD_VERSION when emitBuildVersion.
    // Segment count: __PAGEZERO + __TEXT + __DATA_CONST + __LINKEDIT = 4, plus
    // __DATA when the module has writable globals (D-LK4-DATA-PRODUCER).
    std::uint32_t const segCount = 4u + (hasDataSeg ? 1u : 0u);
    std::uint32_t const ncmds = static_cast<std::uint32_t>(
        segCount + 1u + 1u + 1u + im.loadDylibs.size() + 1u
        + (useChainedFixups ? 0u : 1u)
        + (emitCodeSig ? 1u : 0u)
        + (emitBuildVersion ? 1u : 0u));
    // sizeofcmds: LC_DYLD_CHAINED_FIXUPS is 16 bytes (linkedit_data_
    // command shape — same as LC_CODE_SIGNATURE); LC_DYLD_INFO_ONLY
    // is 48 bytes. Pick the right size for the dyld-binding command.
    std::size_t const dyldBindCmdSize = useChainedFixups
        ? kLinkeditDataCommandSize
        : kDyldInfoCommandSize;
    std::size_t const sizeofcmds =
        kSegCmdPageZeroSize + kSegCmdTextSize + kSegCmdDataConstSize +
        kSegCmdDataSize + kSegCmdLinkeditSize + dyldBindCmdSize +
        dylinkerCmdSize + kLcMainSize + totalDylibCmdSize +
        kSymtabCommandSize +
        (useChainedFixups ? 0u : kDysymtabCommandSize) +
        (emitCodeSig ? kCodeSigCommandSize : 0u) +
        (emitBuildVersion ? kBuildVersionCommandSize : 0u);
    std::size_t const headerAndCmds = kMachHeader64Size + sizeofcmds;

    std::uint64_t const textFileOff =
        alignUp(headerAndCmds, kPageSize);
    std::uint64_t const textFileSize = textBody.size();
    std::uint64_t const stubsFileOff = textFileOff + textFileSize;
    std::uint64_t const stubsFileSize =
        static_cast<std::uint64_t>(numExterns) * stubSize;

    // __TEXT,__const file offset — H1-aligned above __stubs, inside the
    // same __TEXT segment (D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror). Because
    // __TEXT.fileoff = 0 (Apple convention), VA−fileoff is constant
    // across __text/__stubs/__const, so `sectionVa + (constFileOff -
    // textFileOff)` must equal the EARLY `constSectionVa` computed from
    // `stubsVa`. Assert that congruence fail-loud (a future layout
    // change or non-divisor align would silently desync the data
    // symbol VAs the reloc kernel already resolved against).
    std::uint64_t const constFileOff =
        hasConst
            ? alignUp(stubsFileOff + stubsFileSize, constLayout.maxAlign)
            : (stubsFileOff + stubsFileSize);
    std::uint64_t const constEnd =
        hasConst ? (constFileOff + constSize)
                 : (stubsFileOff + stubsFileSize);
    if (hasConst
     && constSectionVa != sectionVa + (constFileOff - textFileOff)) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: __const file/VA "
                         "congruence broken — constSectionVa(0x{:x}) != "
                         "sectionVa(0x{:x}) + (constFileOff(0x{:x}) - "
                         "textFileOff(0x{:x})). The __TEXT segment "
                         "requires the on-disk __const-from-__text "
                         "offset to equal the VA offset (the exec "
                         "read-only data-section arm; mirrors ELF "
                         "D-LK1-ELF-EXEC-DATA-SECTIONS).",
                         constSectionVa, sectionVa, constFileOff,
                         textFileOff));
        return {};
    }

    // __got lives in __DATA_CONST — separate segment, separate page.
    // dyld maps each segment independently. Place __got's file
    // offset on a page boundary above __const (when present) / __stubs
    // (and above the header/cmds region in VA via __DATA_CONST.vmaddr).
    std::uint64_t const gotFileOff =
        alignUp(hasConst ? constEnd : (stubsFileOff + stubsFileSize),
                kPageSize);
    std::uint64_t const gotFileSize =
        static_cast<std::uint64_t>(numExterns) * kGotSlotSize;
    // gotVa derives from the file-offset delta because __TEXT.fileoff
    // = 0 by Apple convention — file offsets map directly to VAs
    // relative to textSegVmaddr. The formula collapses to
    // `textSegVmaddr + textSecOffsetInSeg + (gotFileOff - textFileOff)`
    // when written out fully; here we use the equivalent
    // `sectionVa + (gotFileOff - textFileOff)` since sectionVa
    // already encodes the segment + section offset sum. If plan 16
    // (codesign) ever prepends a signing superblob and forces
    // __TEXT.fileoff != 0, this formula must be rewritten in the
    // segment-anchored form (code-architect MEDIUM, LK6 cycle 2c).
    std::uint64_t const gotVa =
        sectionVa + (gotFileOff - textFileOff);

    // __DATA segment (D-LK4-DATA-PRODUCER) — writable (rw-), placed on a page
    // boundary after __DATA_CONST (the GOT). Holds __data (file-backed, mutable
    // initialized globals) then __bss (S_ZEROFILL, last — VM only, no file
    // bytes). VAs derive from file-offset deltas (same `sectionVa + (off -
    // textFileOff)` identity the GOT uses); the EARLY VAs registered in symbolVa
    // above must match — asserted fail-loud below.
    std::uint64_t const dataSegFileOff =
        hasDataSeg ? alignUp(gotFileOff + gotFileSize, kPageSize) : 0;
    std::uint64_t const dataSecFileOff =
        hasData ? alignUp(dataSegFileOff, dataLayout.maxAlign) : 0;
    std::uint64_t const dataSecFileSize = dataLayout.spanSize;
    std::uint64_t const dataSecVaLate =
        hasData ? sectionVa + (dataSecFileOff - textFileOff) : 0;
    // __bss VA follows __data in VM (zero-fill — no file bytes consumed).
    std::uint64_t const bssSecVaLate =
        hasBss ? alignUp((hasData ? dataSecVaLate + dataSecFileSize
                                  : sectionVa + (dataSegFileOff - textFileOff)),
                         bssLayout.maxAlign)
               : 0;
    std::uint64_t const bssSecSize = bssLayout.spanSize;
    // Congruence self-check: the EARLY VAs (used for reloc resolution) MUST
    // equal the LATE layout VAs (used in the section_64 records). A mismatch
    // would silently place a global's bytes at a different VA than the address
    // a `.text` reloc was resolved to → wrong runtime value. Fail loud.
    if ((hasData && dataSecVa != dataSecVaLate)
        || (hasBss && bssSecVa != bssSecVaLate)) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: __DATA VA congruence "
                         "broken — early/late mismatch (data early=0x{:x} "
                         "late=0x{:x}; bss early=0x{:x} late=0x{:x}). The "
                         "reloc-time and section-record VAs must agree "
                         "(D-LK4-DATA-PRODUCER).",
                         dataSecVa, dataSecVaLate, bssSecVa, bssSecVaLate));
        return {};
    }
    // __DATA segment VM/file extents. filesize covers __data only (__bss adds
    // none); vmsize covers __data + __bss, page-rounded.
    std::uint64_t const dataSegVmaddr =
        hasDataSeg ? sectionVa + (dataSegFileOff - textFileOff) : 0;
    std::uint64_t const dataSegFileSize = hasData ? dataSecFileSize : 0;
    std::uint64_t const dataSegVmEnd =
        hasBss ? (bssSecVaLate + bssSecSize)
               : (hasData ? dataSecVaLate + dataSecFileSize : dataSegVmaddr);
    std::uint64_t const dataSegVmsize =
        hasDataSeg ? alignUp(dataSegVmEnd - dataSegVmaddr, kPageSize) : 0;

    // Build the __stubs body: one per-cputype stub per extern, each
    // pointing at its __got slot. The per-cputype emitter (x86_64
    // `FF 25 disp32`; arm64 ADRP+LDR+BR x16) is localized to the file-
    // level stub substrate above (mirrors elf.cpp's emitPltStub). The
    // walker stays shape-blind — it threads (stubVa, gotSlotVa) and the
    // emitter writes `stubSize` bytes or fails loud.
    std::vector<std::uint8_t> stubsBody;
    stubsBody.reserve(stubsFileSize);
    for (std::size_t i = 0; i < numExterns; ++i) {
        std::uint64_t const stubVa = stubsVa +
            static_cast<std::uint64_t>(i) * stubSize;
        std::uint64_t const gotSlotVa = gotVa +
            static_cast<std::uint64_t>(i) * kGotSlotSize;
        if (!emitMachoStub(id.cputype, stubsBody, stubVa, gotSlotVa, i,
                           reporter)) {
            return {};
        }
    }
    // Substrate invariant: the emitter wrote exactly `stubSize` bytes
    // per extern. A mismatch means an emitter's byte count diverged
    // from `machoStubSizeFor` — that would desync every later layout
    // offset (LC_SEGMENT_64 filesizes, __got page). Fail loud.
    if (stubsBody.size() != stubsFileSize) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: emitted {} __stubs "
                         "bytes but layout reserved {} ({} externs × {} "
                         "stride) — emitMachoStub and machoStubSizeFor "
                         "disagree (substrate invariant violation).",
                         stubsBody.size(), stubsFileSize, numExterns,
                         stubSize));
        return {};
    }

    // ── (l.5) D-LK6-14-INTEGRATION-GOT-SLOTS: now that gotVa is
    //          known (section (l) layout complete), build the
    //          chained-fixups payload with
    //          `dyld_chained_starts_in_segment.segment_offset`
    //          resolved. (Legacy path's `dyldBindBlob` was already
    //          populated in section (i)'s else-arm.)
    if (useChainedFixups) {
        // Single-page __DATA_CONST guard. v1 supports at most one
        // page of __got: kPageSize / kGotSlotSize slots (512 on a
        // 4 KiB page, 2048 on a 16 KiB page — kPageSize is now the
        // config-driven segmentPageSize). Multi-page support requires
        // computing page_starts[i] for each page — anchored
        // D-LK6-14-MULTI-PAGE-GOT (trigger: first module with more
        // extern imports than fit one segmentPageSize of __got).
        if (gotFileSize > kPageSize) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("macho::encodeExecDynamic: chained-fixups "
                             "__got spans {} bytes > kPageSize {} "
                             "(= {} extern slots at {} bytes each on a "
                             "{}-byte page) — v1 supports single-page "
                             "chains only. Reduce extern count or "
                             "fall back to LC_DYLD_INFO_ONLY. Anchored "
                             "D-LK6-14-MULTI-PAGE-GOT.",
                             gotFileSize, kPageSize,
                             kPageSize / kGotSlotSize, kGotSlotSize,
                             kPageSize));
            return {};
        }
        dss::macho::detail::ChainedSegInfo segInfo;
        // segment_offset is the VM offset from __TEXT to __DATA_CONST.
        // __TEXT.vmaddr = im.pageZeroSize (by convention);
        // __DATA_CONST.vmaddr = gotVa (since __got is the only
        // section in __DATA_CONST, at the segment start).
        segInfo.segmentOffset = gotVa - im.pageZeroSize;
        segInfo.pageSize      = static_cast<std::uint16_t>(kPageSize);
        segInfo.pointerFormat = dss::macho::detail::kDyldChainedPtrFormat64;
        // Single page; first chained pointer at byte 0 of __got
        // (which is at byte 0 of __DATA_CONST). page_starts[0] = 0.
        segInfo.pageStarts.push_back(0u);
        dyldBindBlob = dss::macho::detail::buildChainedFixupsPayload(
            chainedImports, &segInfo);
        // Pad to 8-byte load-command alignment per Apple convention.
        while (dyldBindBlob.size() % kLoadCmdAlign != 0)
            dyldBindBlob.push_back(0);
    }

    // __got body: numExterns × 8 zero bytes on legacy path (dyld
    // fills via bind opcodes at load); DYLD_CHAINED_PTR_64 bitfields
    // on chained path so dyld walks the chain at load.
    std::vector<std::uint8_t> gotBody(gotFileSize, 0u);
    if (useChainedFixups) {
        // dyld_chained_ptr_64_bind bitfield (Apple fixup-chains.h):
        //   bits [ 0..23]  ordinal  (24-bit; DYLD_CHAINED_IMPORT row index)
        //   bits [24..31]  addend   (8-bit; 0 for our straightforward binds)
        //   bits [32..50]  reserved (19-bit; must be 0)
        //   bits [51..62]  next     (12-bit; offset in 4-byte units
        //                            to next chained pointer on same
        //                            page per DYLD_CHAINED_PTR_64
        //                            stride rule; 0=end)
        //   bit  [63]      bind     (1=bind, 0=rebase; always 1 here)
        for (std::size_t i = 0; i < numExterns; ++i) {
            std::uint64_t bits = 0;
            bits |= static_cast<std::uint64_t>(i) & 0xFFFFFFull;  // ordinal
            // addend = 0; reserved = 0.
            bool const isLast = (i + 1 == numExterns);
            // Adjacent 8-byte slots are 2 four-byte units apart per
            // kDyldChainedPtr64NextStride; 0 = end of chain.
            std::uint64_t const next = isLast
                ? 0ull
                : dss::macho::detail::kDyldChainedPtr64NextStride;
            bits |= next << 51;
            bits |= 1ull << 63;  // bind = 1
            std::size_t const slotOff = i * kGotSlotSize;
            for (int b = 0; b < 8; ++b) {
                gotBody[slotOff + b] = static_cast<std::uint8_t>(
                    (bits >> (b * 8)) & 0xFFu);
            }
        }
    }

    // __LINKEDIT contents — page-aligned after the last loaded segment: __DATA
    // (when present, D-LK4-DATA-PRODUCER) else __DATA_CONST (the GOT). __bss is
    // S_ZEROFILL so it consumes NO file bytes; __LINKEDIT's file offset follows
    // __data's file end (+ the GOT/data file extent), NOT the bss vm extent.
    std::uint64_t const linkeditFileOff =
        hasDataSeg
            ? alignUp(dataSecFileOff + dataSecFileSize, kPageSize)
            : alignUp(gotFileOff + gotFileSize, kPageSize);
    std::uint64_t const bindOff = linkeditFileOff;
    std::uint64_t const bindSize = dyldBindBlob.size();
    std::uint64_t const indirectSymtabOff = bindOff + bindSize;
    std::uint64_t const indirectSymtabSize =
        static_cast<std::uint64_t>(indirectSyms.size()) * 4u;
    std::uint64_t const symtabOff = indirectSymtabOff + indirectSymtabSize;
    std::uint64_t const symtabSize =
        static_cast<std::uint64_t>(numberOfSymbols) * kNlist64Size;
    std::uint64_t const strtabOff = symtabOff + symtabSize;
    std::uint64_t const strtabSize = strtab.size();
    // LK7: codesign placeholder lives at the tail of __LINKEDIT
    // (8-byte aligned per Apple's `cs_blobs.h` SuperBlob alignment).
    // Plan 16 fills the reserved bytes post-link. The segment's
    // filesize covers the reservation so dyld maps it into the
    // __LINKEDIT segment alongside the other linkedit payloads.
    std::uint64_t const codeSigFileOff = emitCodeSig
        ? alignUp(strtabOff + strtabSize, 8u)
        : 0u;
    // LC_CODE_SIGNATURE's `dataoff` field is `uint32_t` per Apple's
    // `linkedit_data_command` definition. If the __LINKEDIT layout
    // pushes the reservation past 4 GiB, the cast at the emit site
    // below would silently truncate and the kernel would walk into
    // invalid bytes. Surface the overflow rather than ship a corrupt
    // signature directory. (silent-failure MEDIUM fold, LK7 review.)
    if (emitCodeSig
     && codeSigFileOff > std::numeric_limits<std::uint32_t>::max()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("macho::encodeExecDynamic: codesign "
                         "reservation file offset 0x{:x} exceeds "
                         "u32 — LC_CODE_SIGNATURE.dataoff is u32 "
                         "by Apple's linkedit_data_command spec.",
                         codeSigFileOff));
        return {};
    }
    // The reservation byte count. For the ad-hoc `codeSignature` path
    // it is DERIVED from the exact blob length (Condition 2 — never a
    // hand-typed size); `codeSigFileOff` is the signature's file offset
    // == the CodeDirectory `codeLimit` (everything before the signature
    // is hashed), and was just proven to fit in u32. For the legacy
    // placeholder path it stays the schema's `codeSignatureSize`.
    std::uint32_t const codeSigReserveSize =
        im.codeSignature.has_value()
            ? dss::macho::detail::adHocCodeSignatureSize(
                  static_cast<std::uint32_t>(codeSigFileOff),
                  im.codeSignature->pageSize,
                  im.codeSignature->identifier)
            : im.codeSignatureSize;
    std::uint64_t const linkeditFileSize = emitCodeSig
        ? ((codeSigFileOff + codeSigReserveSize) - linkeditFileOff)
        : ((strtabOff + strtabSize) - linkeditFileOff);

    // Segment VAs:
    std::uint64_t const textSegVmaddr = im.pageZeroSize;
    std::uint64_t const textSecOffsetInSeg = sectionVa - textSegVmaddr;
    // The __TEXT segment covers __text + __stubs, plus __const when
    // present (D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror). `constEnd` already
    // equals `stubsFileOff + stubsFileSize` on the no-data path, so
    // this is byte-identical when hasConst is false.
    std::uint64_t const textSegCoveredEnd = hasConst
        ? constEnd
        : (stubsFileOff + stubsFileSize);
    std::uint64_t const textSegVmsize =
        alignUp(textSegCoveredEnd - textFileOff + textSecOffsetInSeg,
                kPageSize);
    // __TEXT.fileoff = 0 by Apple convention (the mach header sits
    // inside __TEXT). The segment therefore spans [0, coveredEnd) in
    // the file — its filesize must equal coveredEnd, NOT
    // (coveredEnd - textFileOff) which would truncate the mmap before
    // reaching .text (code-reviewer C1 fold, dyld correctness).
    std::uint64_t const textSegFileSize = textSegCoveredEnd;

    std::uint64_t const dataConstSegVmaddr = gotVa;
    std::uint64_t const dataConstSegVmsize =
        alignUp(gotFileSize, kPageSize);
    std::uint64_t const dataConstSegFileSize = gotFileSize;

    // __LINKEDIT VM address follows the last loaded data segment: __DATA when
    // present (its vmsize covers __data + __bss), else __DATA_CONST.
    std::uint64_t const linkeditSegVmaddr =
        hasDataSeg ? (dataSegVmaddr + dataSegVmsize)
                   : (dataConstSegVmaddr + dataConstSegVmsize);
    std::uint64_t const linkeditSegVmsize =
        alignUp(linkeditFileSize, kPageSize);

    // Indirect-symtab reserved1 indices: __stubs uses [0..numExterns),
    // __got uses [numExterns..2*numExterns). On the chained-fixups
    // path the indirect symbol table is absent; reserved1 = 0 for
    // both sections (D-LK6-14-INTEGRATION-GOT-SLOTS — chained
    // pointers in __got encode the ordinal directly).
    constexpr std::uint32_t kStubsReserved1 = 0;
    std::uint32_t const kGotReserved1 = useChainedFixups
        ? 0u
        : static_cast<std::uint32_t>(numExterns);

    // ── (m) Emit bytes ───────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(strtabOff + strtabSize);

    // mach_header_64
    appendU32LE(bytes, MH_MAGIC_64);
    appendU32LE(bytes, id.cputype);
    appendU32LE(bytes, id.cpusubtype);
    appendU32LE(bytes, static_cast<std::uint32_t>(id.filetype));
    appendU32LE(bytes, ncmds);
    appendU32LE(bytes, static_cast<std::uint32_t>(sizeofcmds));
    appendU32LE(bytes, id.flags);
    appendU32LE(bytes, 0);

    // LC_SEGMENT_64 __PAGEZERO
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdPageZeroSize));
    appendName16(bytes, "__PAGEZERO");
    appendU64LE(bytes, 0);
    appendU64LE(bytes, im.pageZeroSize);
    appendU64LE(bytes, 0);
    appendU64LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);

    // LC_SEGMENT_64 __TEXT (__text + __stubs, plus __const when data
    // globals are present — D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror).
    constexpr std::int32_t kVmProtRx = 5;
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdTextSize));
    appendName16(bytes, "__TEXT");
    appendU64LE(bytes, textSegVmaddr);
    appendU64LE(bytes, textSegVmsize);
    appendU64LE(bytes, 0);
    appendU64LE(bytes, textSegFileSize);
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRx));
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRx));
    appendU32LE(bytes, hasConst ? 3u : 2u);   // nsects
    appendU32LE(bytes, 0);

    // section_64 __text
    appendName16(bytes, secText.name);
    appendName16(bytes, secText.segment);
    appendU64LE(bytes, sectionVa);
    appendU64LE(bytes, textFileSize);
    appendU32LE(bytes, static_cast<std::uint32_t>(textFileOff));
    appendU32LE(bytes, static_cast<std::uint32_t>(secText.addrAlign));
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, secText.type);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);

    // section_64 __stubs
    appendName16(bytes, "__stubs");
    appendName16(bytes, "__TEXT");
    appendU64LE(bytes, stubsVa);
    appendU64LE(bytes, stubsFileSize);
    appendU32LE(bytes, static_cast<std::uint32_t>(stubsFileOff));
    appendU32LE(bytes, /*addrAlign=*/1);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes,
                S_SYMBOL_STUBS | S_ATTR_PURE_INSTRUCTIONS |
                    S_ATTR_SOME_INSTRUCTIONS);
    appendU32LE(bytes, kStubsReserved1);    // index of first indirect sym
    appendU32LE(bytes, static_cast<std::uint32_t>(stubSize));  // stub size
    appendU32LE(bytes, 0);

    // section_64 __const (D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror) — read-only
    // data globals, inside __TEXT after __stubs. flags = S_REGULAR (0).
    // The `align` field is log2 (the section_64 convention — matches
    // __got writing 3 for log2(8)), derived via std::countr_zero from
    // the H1-raised section alignment.
    if (hasConst) {
        appendName16(bytes, secConst->name);
        appendName16(bytes, secConst->segment);
        appendU64LE(bytes, constSectionVa);
        appendU64LE(bytes, constSize);
        appendU32LE(bytes, static_cast<std::uint32_t>(constFileOff));
        appendU32LE(bytes, static_cast<std::uint32_t>(
                               std::countr_zero(constLayout.maxAlign)));
        appendU32LE(bytes, 0);                 // reloff
        appendU32LE(bytes, 0);                 // nreloc
        appendU32LE(bytes, 0);                 // flags = S_REGULAR
        appendU32LE(bytes, 0);                 // reserved1
        appendU32LE(bytes, 0);                 // reserved2
        appendU32LE(bytes, 0);                 // reserved3
    }

    // LC_SEGMENT_64 __DATA_CONST (1 section: __got)
    constexpr std::int32_t kVmProtRw = 3;
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdDataConstSize));
    appendName16(bytes, "__DATA_CONST");
    appendU64LE(bytes, dataConstSegVmaddr);
    appendU64LE(bytes, dataConstSegVmsize);
    appendU64LE(bytes, gotFileOff);
    appendU64LE(bytes, dataConstSegFileSize);
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRw));
    appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRw));
    appendU32LE(bytes, 1);
    appendU32LE(bytes, 0);

    // section_64 __got
    appendName16(bytes, "__got");
    appendName16(bytes, "__DATA_CONST");
    appendU64LE(bytes, gotVa);
    appendU64LE(bytes, gotFileSize);
    appendU32LE(bytes, static_cast<std::uint32_t>(gotFileOff));
    appendU32LE(bytes, /*addrAlign=*/3);   // log2(8) = 3
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, S_NON_LAZY_SYMBOL_POINTERS);
    appendU32LE(bytes, kGotReserved1);    // index of first indirect sym
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);

    // LC_SEGMENT_64 __DATA (D-LK4-DATA-PRODUCER) — writable (rw-) segment with
    // __data (file-backed mutable globals) and/or __bss (S_ZEROFILL zero-fill,
    // LAST). Emitted between __DATA_CONST and __LINKEDIT. Section flags come
    // from the SCHEMA ROW (`secData->type` = S_REGULAR; `secBss->type` =
    // S_ZEROFILL) — never hardcoded, keeping the writer agnostic. filesize
    // covers __data only (__bss is zero-fill); vmsize covers both.
    if (hasDataSeg) {
        appendU32LE(bytes, LC_SEGMENT_64);
        appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdDataSize));
        appendName16(bytes,
                     secData != nullptr ? secData->segment
                     : (secBss != nullptr ? secBss->segment
                                          : std::string{"__DATA"}));
        appendU64LE(bytes, dataSegVmaddr);
        appendU64LE(bytes, dataSegVmsize);
        appendU64LE(bytes, hasData ? dataSecFileOff : linkeditFileOff);
        appendU64LE(bytes, dataSegFileSize);
        appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRw));
        appendU32LE(bytes, static_cast<std::uint32_t>(kVmProtRw));
        appendU32LE(bytes, dataSegNsects);
        appendU32LE(bytes, 0);
        // section_64 __data (file-backed). flags from schema (S_REGULAR).
        if (hasData) {
            appendName16(bytes, secData->name);
            appendName16(bytes, secData->segment);
            appendU64LE(bytes, dataSecVaLate);
            appendU64LE(bytes, dataSecFileSize);
            appendU32LE(bytes, static_cast<std::uint32_t>(dataSecFileOff));
            appendU32LE(bytes, static_cast<std::uint32_t>(
                                   std::countr_zero(dataLayout.maxAlign)));
            appendU32LE(bytes, 0);                 // reloff
            appendU32LE(bytes, 0);                 // nreloc
            appendU32LE(bytes, secData->type);     // flags (S_REGULAR) from schema
            appendU32LE(bytes, 0);                 // reserved1
            appendU32LE(bytes, 0);                 // reserved2
            appendU32LE(bytes, 0);                 // reserved3
        }
        // section_64 __bss (S_ZEROFILL — offset 0, no file bytes; LAST in
        // __DATA so its zero-fill span tails the segment's vmsize). flags from
        // schema (S_ZEROFILL = 1).
        if (hasBss) {
            appendName16(bytes, secBss->name);
            appendName16(bytes, secBss->segment);
            appendU64LE(bytes, bssSecVaLate);
            appendU64LE(bytes, bssSecSize);
            appendU32LE(bytes, 0);                 // offset = 0 (S_ZEROFILL)
            appendU32LE(bytes, static_cast<std::uint32_t>(
                                   std::countr_zero(bssLayout.maxAlign)));
            appendU32LE(bytes, 0);                 // reloff
            appendU32LE(bytes, 0);                 // nreloc
            appendU32LE(bytes, secBss->type);      // flags (S_ZEROFILL) from schema
            appendU32LE(bytes, 0);                 // reserved1
            appendU32LE(bytes, 0);                 // reserved2
            appendU32LE(bytes, 0);                 // reserved3
        }
    }

    // LC_SEGMENT_64 __LINKEDIT (no sections)
    appendU32LE(bytes, LC_SEGMENT_64);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSegCmdLinkeditSize));
    appendName16(bytes, "__LINKEDIT");
    appendU64LE(bytes, linkeditSegVmaddr);
    appendU64LE(bytes, linkeditSegVmsize);
    appendU64LE(bytes, linkeditFileOff);
    appendU64LE(bytes, linkeditFileSize);
    appendU32LE(bytes, /*maxprot=*/1);  // R
    appendU32LE(bytes, /*initprot=*/1);
    appendU32LE(bytes, 0);
    appendU32LE(bytes, 0);

    // LC_BUILD_VERSION — emitted after the four LC_SEGMENT_64 commands
    // (so it does NOT shift the segment indices the bind opcodes
    // reference) and before the dyld-binding command. Declares the
    // platform / min-OS so dyld accepts the image on macOS 11+ / Apple
    // Silicon (D-LK10-ENTRY-MACHO-EXIT).
    if (emitBuildVersion) {
        appendBuildVersionCommand(bytes, *im.buildVersion);
    }

    if (useChainedFixups) {
        // LC_DYLD_CHAINED_FIXUPS (modern dyld binding format).
        // The 16-byte linkedit_data_command points at the
        // buildChainedFixupsPayload blob in __LINKEDIT. Companion
        // D-LK6-14-INTEGRATION-GOT-SLOTS populates __got slots with
        // DYLD_CHAINED_PTR_64 bitfields + drops LC_DYSYMTAB below.
        appendU32LE(bytes, LC_DYLD_CHAINED_FIXUPS);
        appendU32LE(bytes, static_cast<std::uint32_t>(kLinkeditDataCommandSize));
        appendU32LE(bytes, static_cast<std::uint32_t>(bindOff));
        appendU32LE(bytes, static_cast<std::uint32_t>(bindSize));
    } else {
        // LC_DYLD_INFO_ONLY (legacy opcode-stream binding).
        appendU32LE(bytes, LC_DYLD_INFO_ONLY);
        appendU32LE(bytes, static_cast<std::uint32_t>(kDyldInfoCommandSize));
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);   // rebase_off/size
        appendU32LE(bytes, static_cast<std::uint32_t>(bindOff));
        appendU32LE(bytes, static_cast<std::uint32_t>(bindSize));
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);   // weak_bind
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);   // lazy_bind (eager — 0)
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);   // export
    }

    // LC_LOAD_DYLINKER
    {
        std::size_t const cmdStart = bytes.size();
        appendU32LE(bytes, LC_LOAD_DYLINKER);
        appendU32LE(bytes, static_cast<std::uint32_t>(dylinkerCmdSize));
        appendU32LE(bytes, 12);
        for (char c : im.dylinkerPath)
            appendU8(bytes, static_cast<std::uint8_t>(c));
        appendU8(bytes, 0);
        while (bytes.size() - cmdStart < dylinkerCmdSize) appendU8(bytes, 0);
    }

    // LC_MAIN
    appendU32LE(bytes, LC_MAIN);
    appendU32LE(bytes, static_cast<std::uint32_t>(kLcMainSize));
    appendU64LE(bytes, textFileOff + funcTextStart[entryFnIdx]);
    appendU64LE(bytes, 0);

    // LC_LOAD_DYLIB[]
    for (auto const& d : im.loadDylibs) {
        std::size_t const cmdStart = bytes.size();
        std::size_t const cmdSize = commandSizeWithPath(24, d.path);
        appendU32LE(bytes, LC_LOAD_DYLIB);
        appendU32LE(bytes, static_cast<std::uint32_t>(cmdSize));
        appendU32LE(bytes, 24);
        appendU32LE(bytes, 0);
        appendU32LE(bytes, 0);
        appendU32LE(bytes, 0);
        for (char c : d.path)
            appendU8(bytes, static_cast<std::uint8_t>(c));
        appendU8(bytes, 0);
        while (bytes.size() - cmdStart < cmdSize) appendU8(bytes, 0);
    }

    // LC_SYMTAB
    appendU32LE(bytes, LC_SYMTAB);
    appendU32LE(bytes, static_cast<std::uint32_t>(kSymtabCommandSize));
    appendU32LE(bytes, static_cast<std::uint32_t>(symtabOff));
    appendU32LE(bytes, numberOfSymbols);
    appendU32LE(bytes, static_cast<std::uint32_t>(strtabOff));
    appendU32LE(bytes, static_cast<std::uint32_t>(strtabSize));

    if (!useChainedFixups) {
        // LC_DYSYMTAB (indirect symbol table for __stubs/__got).
        // D-LK6-14-INTEGRATION-GOT-SLOTS CLOSED: chained fixups make
        // this LC redundant — the chained pointers in __got encode
        // the import ordinal directly via DYLD_CHAINED_PTR_64 row
        // bits[0..23]. The entire block is skipped on chained path
        // (the ncmds/sizeofcmds arithmetic above accounts for the
        // absence). Indirect-symtab byte emission below is similarly
        // skipped.
        appendU32LE(bytes, LC_DYSYMTAB);
        appendU32LE(bytes, static_cast<std::uint32_t>(kDysymtabCommandSize));
        appendU32LE(bytes, 0);                    // ilocalsym
        appendU32LE(bytes, 0);                    // nlocalsym
        appendU32LE(bytes, 0);                    // iextdefsym
        appendU32LE(bytes, numDefs);              // nextdefsym
        appendU32LE(bytes, numDefs);              // iundefsym
        appendU32LE(bytes,
                    static_cast<std::uint32_t>(numExterns));  // nundefsym
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);  // toc
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);  // modtab
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);  // extrefsym
        appendU32LE(bytes,
                    static_cast<std::uint32_t>(indirectSymtabOff));
        appendU32LE(bytes,
                    static_cast<std::uint32_t>(indirectSyms.size()));
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);  // extrel
        appendU32LE(bytes, 0); appendU32LE(bytes, 0);  // locrel
    }

    // LK7: LC_CODE_SIGNATURE placeholder. linkedit_data_command =
    // cmd(4) cmdsize(4) dataoff(4) datasize(4). Plan 16 fills the
    // reserved bytes at codeSigFileOff post-link with the Apple
    // SuperBlob (CodeDirectory + Requirements + Entitlements + CMS).
    if (emitCodeSig) {
        appendU32LE(bytes, LC_CODE_SIGNATURE);
        appendU32LE(bytes,
                    static_cast<std::uint32_t>(kCodeSigCommandSize));
        appendU32LE(bytes,
                    static_cast<std::uint32_t>(codeSigFileOff));
        // datasize = the reserved byte count: the derived ad-hoc blob
        // length on the codeSignature path, else the schema placeholder
        // size. The ad-hoc fill below asserts the built blob is exactly
        // this many bytes.
        appendU32LE(bytes, codeSigReserveSize);
    }

    // Sanity: emitted exactly sizeofcmds bytes.
    if (bytes.size() != kMachHeader64Size + sizeofcmds) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"macho::encodeExecDynamic: emitted "}
                 + std::to_string(bytes.size() - kMachHeader64Size)
                 + " bytes of load commands but sizeofcmds="
                 + std::to_string(sizeofcmds)
                 + " — substrate invariant violation.");
        return {};
    }

    // Pad to textFileOff
    while (bytes.size() < textFileOff) bytes.push_back(0);

    // __text body (with applied relocations)
    bytes.insert(bytes.end(), textBody.begin(), textBody.end());

    // __stubs body
    bytes.insert(bytes.end(), stubsBody.begin(), stubsBody.end());

    // __TEXT,__const body — read-only data globals, H1-aligned above
    // __stubs (D-LK1-ELF-EXEC-DATA-SECTIONS Mach-O __const mirror). Pad to constFileOff
    // then emit the laid-out bytes; the EXISTING pad-to-gotFileOff
    // below then advances to the __DATA_CONST page. No-op when
    // hasConst is false (the const block is empty + constFileOff
    // collapses into the stubs end).
    if (hasConst) {
        while (bytes.size() < constFileOff) bytes.push_back(0);
        bytes.insert(bytes.end(), constLayout.bytes.begin(),
                     constLayout.bytes.end());
    }

    // Pad to gotFileOff (separate page from __TEXT)
    while (bytes.size() < gotFileOff) bytes.push_back(0);

    // __got body (zeroes — dyld fills at load via bind opcodes)
    bytes.insert(bytes.end(), gotBody.begin(), gotBody.end());

    // __DATA,__data body (D-LK4-DATA-PRODUCER) — file-backed mutable globals.
    // __bss is S_ZEROFILL: it emits NO file bytes (the loader zero-fills it),
    // so only __data contributes to the file here.
    if (hasData) {
        while (bytes.size() < dataSecFileOff) bytes.push_back(0);
        bytes.insert(bytes.end(), dataLayout.bytes.begin(),
                     dataLayout.bytes.end());
    }

    // Pad to linkeditFileOff
    while (bytes.size() < linkeditFileOff) bytes.push_back(0);

    // __LINKEDIT: dyld binding bytes (legacy bind opcode stream
    // OR chained-fixups payload depending on useChainedFixups).
    bytes.insert(bytes.end(), dyldBindBlob.begin(), dyldBindBlob.end());

    // Indirect-symtab
    for (auto const idx : indirectSyms) appendU32LE(bytes, idx);

    // nlist_64[]
    bytes.insert(bytes.end(), nlistBytes.begin(), nlistBytes.end());

    // string table
    auto strtabBytes = std::move(strtab).release();
    bytes.insert(bytes.end(), strtabBytes.begin(), strtabBytes.end());

    // LK7 / D-LK7-ADHOC-CODESIGN-MACHO: pad to the 8-byte-aligned
    // codesign reservation, then fill it. At this point `bytes.size()`
    // has been padded to exactly `codeSigFileOff`, so `bytes[0,
    // codeSigFileOff)` is the complete signed region (everything before
    // the signature). The CodeDirectory's `codeLimit` is therefore
    // `codeSigFileOff` and the page hashes cover those bytes.
    if (emitCodeSig) {
        while (bytes.size() < codeSigFileOff) bytes.push_back(0);
        if (im.codeSignature.has_value()) {
            // Build the real ad-hoc CodeDirectory + SuperBlob over the
            // signed bytes and write it into the reservation. execSeg
            // limit = the __TEXT segment file size (the kernel maps it
            // as the main binary's executable region).
            std::vector<std::uint8_t> const sig =
                dss::macho::detail::buildAdHocCodeSignature(
                    std::span<std::uint8_t const>{bytes.data(),
                                                  codeSigFileOff},
                    static_cast<std::uint32_t>(codeSigFileOff),
                    im.codeSignature->pageSize,
                    im.codeSignature->identifier,
                    textSegFileSize);
            // Substrate invariant: the built blob occupies the reserved
            // region EXACTLY (no overrun, no slack). The reservation
            // size was derived from `adHocCodeSignatureSize` over the
            // same arguments, so any mismatch is an internal bug —
            // fail loud rather than emit a corrupt signature.
            if (sig.size() != codeSigReserveSize) {
                emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("macho::encodeExecDynamic: ad-hoc "
                                 "code-signature blob is {} bytes but "
                                 "the reservation is {} — "
                                 "adHocCodeSignatureSize and "
                                 "buildAdHocCodeSignature disagree "
                                 "(substrate invariant violation).",
                                 sig.size(), codeSigReserveSize));
                return {};
            }
            bytes.insert(bytes.end(), sig.begin(), sig.end());
        } else {
            // Legacy placeholder path: `codeSignatureSize` zero bytes
            // for a later (plan 16) post-link fill.
            bytes.insert(bytes.end(), codeSigReserveSize,
                         std::uint8_t{0});
        }
    }

    return bytes;
}

} // namespace

} // namespace dss::macho
