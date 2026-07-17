#include "link/format/pe.hpp"

#include "asm/format/x86_variable.hpp"   // kStackProbeLoopBytes (unwind allocLen)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"   // isExternallyVisible (dll .edata exports)
#include "link/format/byte_emit.hpp"
#include "link/format/exec_data_section.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/format/interior_block_symbol_va.hpp"
#include "link/format/object_symbol_names.hpp"
#include "link/format/string_table.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// PE/COFF writer — plan 14 LK2 cycle 1 (.obj) + cycle 2 (PE32+ .exe)
// + c148 (.obj data sections + extern naming — the PE analog of the
// ELF c141/c142/c145 and Mach-O c146/c147 cross-toolchain arcs).
//
// .obj byte layout (PE/COFF spec §3-5):
//   [0x00]   IMAGE_FILE_HEADER (20 B)
//   [0x14]   IMAGE_SECTION_HEADER × N (40 B each; .text, then any
//            present .rdata / .data / .rdata-relro / .bss)
//   [...]    section raw data (.text first, then the file-backed data
//            sections packed back-to-back; .bss stores no bytes)
//   [...]    per-section IMAGE_RELOCATION[] (10 B packed each; .text,
//            then data, then relro)
//   [ptr]    IMAGE_SYMBOL[] (18 B packed each)
//   [...]    String table (u32 size + NUL-terminated strings)
//
// PE32+ image (.exe) byte layout (PE/COFF §3.2-3.4):
//   [0x00]   IMAGE_DOS_HEADER (64 B) — `MZ` + e_lfanew → 0x80
//   [0x40]   MS-DOS stub (~64 B "This program cannot be run in DOS")
//   [0x80]   PE signature "PE\0\0" (4 B)
//   [0x84]   IMAGE_FILE_HEADER (20 B)
//   [0x98]   IMAGE_OPTIONAL_HEADER64 (240 B = 112 fixed + 16×8 data dirs)
//   [...]    IMAGE_SECTION_HEADER × N (40 B each)
//   [pad]    pad to fileAlignment (512)
//   [...]    section raw data (.text first, etc.) padded to fileAlignment
//
// The walker is target-blind in shape — every PE-specific number
// (machine, characteristics, section Characteristics, reloc
// nativeIds, section names, optional-header fields, ImageBase, etc.)
// is read from the format schema. The only hardcoded structural
// knowledge is the PE/COFF binary record layout.

namespace dss::pe {

namespace {

using dss::report;
using link::format::detail::appendU8;
using link::format::detail::appendU16LE;
using link::format::detail::appendU32LE;
using link::format::detail::appendU64LE;
using link::format::detail::appendI16LE;
using link::format::detail::emit;
using link::format::detail::requireSection;

// ── PE/COFF constants (spec §5) ─────────────────────────────────

// IMAGE_SYM_CLASS_*
constexpr std::uint8_t IMAGE_SYM_CLASS_EXTERNAL = 2;
// STATIC(3): a symbol resolved ONLY within its own object — link.exe
// never enters it into the cross-object global table. Used for the
// synthetic per-block symbols (jump-table / computed-goto targets):
// EXTERNAL would let a sibling object's unrelated `sym_<id>` bind to
// an interior block address (silent wrong-control-flow) — the COFF
// analog of ELF STB_LOCAL / Mach-O no-N_EXT (c147 fold, baked in).
constexpr std::uint8_t IMAGE_SYM_CLASS_STATIC = 3;
// IMAGE_SECTION_NUMBER specials
constexpr std::int16_t IMAGE_SYM_UNDEFINED = 0;
// IMAGE_SYM_TYPE_*
constexpr std::uint16_t IMAGE_SYM_DTYPE_FUNCTION = 0x20;

constexpr std::uint16_t kFileHeaderSize    = 20;
constexpr std::uint16_t kSectionHeaderSize = 40;
constexpr std::size_t   kSymbolRecordSize  = 18;
constexpr std::size_t   kRelocRecordSize   = 10;

// Byte-emit helpers + emit() + requireSection() now hoisted to
// `src/link/format/byte_emit.hpp` (simplifier fold-in #1+#3).

// ── Per-machine import-THUNK substrate (MIRRORS macho.cpp's __stubs
//    and elf.cpp's PLT) — D-FFI-PE-IMPORT-THUNK ───────────────────
//
// A PE import thunk plays the same role as an ELF PLT stub / Mach-O
// `__stubs` entry: one code thunk per extern, jumping indirectly
// through the extern's IAT slot (the loader-patched FirstThunk). With
// `externCallDispatch == direct-plt`, `symbolVa[extern]` names the
// THUNK (code), so an ADDRESS-TAKEN import is a CALLABLE address and a
// plain `call rel32 → thunk` reaches the callee. This retires the
// crash where `symbolVa[extern]` named the IAT *data* slot: taking an
// import's address and calling it indirectly (`call *reg`) jumped into
// `.idata` and executed the pointer bytes as code (sqlite os_win.c
// `aSyscall[]` on Windows — the pe64 leg's last run-green blocker).
//
// IMAGE_FILE_MACHINE (object_format_schema.hpp): AMD64 = 0x8664,
// ARM64 = 0xAA64. The walker dispatches on the schema's `machine`
// (read as DATA) — a 2nd ISA = a new size arm + a new emitter + a new
// dispatch case, all localized here (the elf.cpp / macho.cpp
// precedent). Only x86_64 ships a PE exec target today; arm64-PE has
// no shipped format, so its emitter is deferred behind the fail-loud
// `default` (NOT a silent tight slice — a real diagnostic).
constexpr std::uint16_t kMachineAmd64PE = 0x8664;

constexpr std::size_t kX86_64PeThunkSize = 6;  // FF 25 disp32

// Per-machine import-thunk entry size in bytes. Returns 0 for an
// unhandled machine — every CALLER pairs the size query with
// `emitPeThunk`, whose `default` fails loud, so a 0 here never
// silently ships a zero-stride thunk block.
[[nodiscard]] constexpr std::size_t
peThunkSizeFor(std::uint16_t machine) noexcept {
    switch (machine) {
        case kMachineAmd64PE: return kX86_64PeThunkSize;
    }
    return 0u;
}

// Emit one x86_64 import thunk into `text` at [thunkOff .. thunkOff+6):
// 6-byte `FF 25 disp32` = `jmp *(rip + disp32)` jumping indirectly
// through the extern's IAT slot. disp32 is PC-relative from the END of
// the 6-byte instruction. Byte-shape-IDENTICAL to macho.cpp's
// `emitX86_64MachoStub` (which jumps through `__got`) — the only
// difference is the slot section (PE `.idata` IAT vs Mach-O `__got`),
// both loader-patched pointer tables. The caller has already RESERVED
// these 6 bytes (Phase (a2) `text.resize`), so this writes in place.
[[nodiscard]] inline bool emitX86_64PeThunk(
        std::vector<std::uint8_t>& text,
        std::size_t                thunkOff,
        std::uint64_t              thunkVa,
        std::uint64_t              iatSlotVa,
        std::size_t                externIdx,
        DiagnosticReporter&        reporter) {
    std::int64_t const disp =
        static_cast<std::int64_t>(iatSlotVa) -
        static_cast<std::int64_t>(thunkVa + kX86_64PeThunkSize);
    if (disp < std::numeric_limits<std::int32_t>::min()
     || disp > std::numeric_limits<std::int32_t>::max()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("pe::encodeExec: import-thunk disp32 overflow "
                         "(0x{:x}) for extern #{}; image too large for a "
                         "32-bit PC-relative IAT reference.",
                         static_cast<std::uint64_t>(disp), externIdx));
        return false;
    }
    auto const d32 =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(disp));
    text[thunkOff + 0] = 0xFFu;
    text[thunkOff + 1] = 0x25u;
    text[thunkOff + 2] = static_cast<std::uint8_t>(d32 & 0xFFu);
    text[thunkOff + 3] = static_cast<std::uint8_t>((d32 >> 8) & 0xFFu);
    text[thunkOff + 4] = static_cast<std::uint8_t>((d32 >> 16) & 0xFFu);
    text[thunkOff + 5] = static_cast<std::uint8_t>((d32 >> 24) & 0xFFu);
    return true;
}

// Per-machine import-thunk dispatch (mirrors macho.cpp's `emitMachoStub`
// / elf.cpp's `emitPltStub`). Writes exactly `peThunkSizeFor(machine)`
// bytes into `text` at `thunkOff` on success. Fail-loud `default` — NO
// silent zero-byte thunk for an unhandled machine.
[[nodiscard]] inline bool emitPeThunk(
        std::uint16_t              machine,
        std::vector<std::uint8_t>& text,
        std::size_t                thunkOff,
        std::uint64_t              thunkVa,
        std::uint64_t              iatSlotVa,
        std::size_t                externIdx,
        DiagnosticReporter&        reporter) {
    switch (machine) {
        case kMachineAmd64PE:
            return emitX86_64PeThunk(text, thunkOff, thunkVa, iatSlotVa,
                                     externIdx, reporter);
    }
    emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
         std::format("pe::encodeExec: machine 0x{:x} has no import-thunk "
                     "emitter — the PE dynamic-import path supports x86_64 "
                     "(0x{:x}). Add a per-machine emitter (see emitPeThunk) "
                     "to ship address-taken FFI for this architecture.",
                     machine, kMachineAmd64PE));
    return false;
}

// ── Windows x64 unwind info (.xdata UNWIND_INFO) — D-WIN64-PDATA-XDATA-UNWIND
//
// The x64 SEH/unwind ABI (learn.microsoft.com/cpp/build/exception-handling-x64)
// describes each function's PROLOGUE so RtlVirtualUnwind can reconstruct the
// caller's context from a fault anywhere in the body. The DSS x86_64 prologue is
// `sub rsp, frame` (or, for `frame > stackProbePageBytes`, the inline page-probe
// loop) then one `mov [rsp + saveOffset], reg` per used callee-save — no push,
// no frame pointer. So the unwind codes are UWOP_ALLOC_{SMALL,LARGE} for the RSP
// adjustment + UWOP_SAVE_NONVOL per saved GPR. A saved FPR (MS-x64 xmm6..15) is
// spilled low-64 via MOVSD, for which there is no matching UWOP (SAVE_XMM128
// describes a full-16-byte MOVAPS slot); since an xmm save does not move RSP it is
// OMITTED from the codes — the RSP/return-address walk stays exact. Its handler-
// case RESTORE is deferred to D-WIN64-XMM-UNWIND-RESTORE (c116).
//
// CRITICAL (audit-F1): each UNWIND_CODE's CodeOffset = the byte offset of the END
// of the instruction that performs that op (NOT the whole-prologue length); the
// `sub`/probe is FIRST (small offset), the saves AFTER (larger offsets); the array
// must be emitted SORTED DESCENDING by CodeOffset. `SizeOfProlog` (a separate
// header byte) is the whole-prologue length. All offsets are single bytes — a
// prologue > 255 B is undescribable (fails loud → D-WIN64-CHKSTK-LARGE-PROLOGUE).
//
// Byte lengths are recomputed deterministically from FrameUnwindInfo (the
// x86-variable encoder never emits a shorter-than-worst-case form for a fixed
// operand shape): `sub rsp,imm32` = 7 B; the inline stack-probe loop = a FIXED
// `kStackProbeLoopBytes` (37 B — the loop iterates at RUNTIME, it is not
// unrolled per page; sourced from x86_variable.hpp so it can't drift from
// emitStackProbeLoop); each GPR save `mov [rsp+disp32],r` = 8 B, each FPR save
// `movsd [rsp+disp32],xmm` = 8 B (xmm0–7) / 9 B (xmm8–15). A first-byte-opcode
// check fail-louds if the real prologue diverges.
constexpr std::uint8_t kUwopAllocLarge  = 1;  // 2 nodes (opinfo 0, size/8 as u16) — frames ≤ 512 KiB
constexpr std::uint8_t kUwopAllocSmall  = 2;  // 1 node, opinfo = size/8 - 1 — frames 8..128 B
constexpr std::uint8_t kUwopSaveNonvol  = 4;  // 2 nodes (opinfo=reg, offset/8 as u16)
constexpr std::size_t  kRuntimeFunctionSize = 12;  // BeginAddress + EndAddress + UnwindInfoAddress (3 u32)

// Build one function's UNWIND_INFO blob (aligned to a multiple of 4 bytes so the
// next one and any RUNTIME_FUNCTION stays DWORD-aligned). Returns nullopt (with a
// loud diagnostic) on a >255-byte prologue, a > 512 KiB frame, a non-8-aligned
// frame/save, or a prologue-shape mismatch — never a silent wrong table.
// c116 (D-WIN64-SEH-FUNCLETS): one deferred back-patch of
// the UNWIND_INFO exception-handler RVA field (the __C_specific_handler personality
// THUNK RVA — a .text address resolved only in the LATER .idata pass). `xdataOffset`
// is the byte offset WITHIN the whole `.xdata` blob of the 4-byte field; `symbol`
// is the personality extern whose thunk RVA is written there.
struct SehHandlerPatch { std::uint32_t xdataOffset; SymbolId symbol; };

[[nodiscard]] inline std::optional<std::vector<std::uint8_t>>
buildFunctionUnwindInfo(FrameUnwindInfo const&          ui,
                        std::span<std::uint8_t const>   funcBytes,
                        std::size_t                     funcIndex,
                        // c116: this function's image-RVA base (= textRva0 +
                        // funcTextStart[funcIndex]) — the scope table's Begin/End/
                        // JumpTarget are this + the SehScopeEntry byte offsets.
                        std::uint32_t                   funcBeginRva,
                        // c116: SymbolId → image-RVA (the filter-funclet function's
                        // RVA = the scope-table HandlerAddress). Returns 0 when the
                        // symbol is not a defined function (fail-loud caller).
                        std::function<std::uint32_t(SymbolId)> const& symbolToRva,
                        // c116: the base byte offset THIS blob will occupy within
                        // the whole `.xdata` (= xdataBytes.size() at the call), so
                        // recorded handler-field patch offsets are .xdata-global.
                        std::uint32_t                   xdataBaseOffset,
                        std::vector<SehHandlerPatch>&   handlerPatches,
                        DiagnosticReporter&             reporter) {
    auto fail = [&](std::string msg) -> std::optional<std::vector<std::uint8_t>> {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             "pe::encodeExec: unwind info for function #"
                 + std::to_string(funcIndex) + ": " + msg);
        return std::nullopt;
    };
    std::uint32_t const frame = ui.totalFrameSize;
    if (frame % 8u != 0u) {
        return fail("frame size " + std::to_string(frame)
                    + " is not 8-byte aligned — x64 UWOP_ALLOC requires /8");
    }

    // (1) The RSP-adjust instruction's byte length + its CodeOffset (= its end).
    std::uint32_t allocLen = 0;
    if (ui.usesStackProbe) {
        // The inline page-probe loop is a FIXED-length sequence — it ITERATES
        // at runtime, it is NOT unrolled per page — so its prologue byte count
        // is the emitter's own authoritative constant, independent of frame/
        // page. (The c114 audit caught a drifted private `9 + 28*pages + 3`
        // here that mis-sized SizeOfProlog + every CodeOffset for any frame
        // over one guard page → a silently-wrong unwind table.)
        allocLen = dss::x86_variable::kStackProbeLoopBytes;
    } else if (frame > 0u) {
        allocLen = 7u;                       // sub rsp, imm32 = 48 81 EC id
    }
    // Prologue-shape guard: verify the first real byte matches (sub → 0x48, probe
    // → 0x41) so a future prologue-encoding change can't silently desync the
    // recomputed offsets from the emitted bytes.
    if (allocLen > 0u) {
        if (funcBytes.empty()) return fail("empty function body but frame > 0");
        std::uint8_t const b0 = funcBytes[0];
        std::uint8_t const want = ui.usesStackProbe ? 0x41u : 0x48u;
        if (b0 != want) {
            return fail("prologue first byte 0x"
                        + std::format("{:02x}", b0) + " != expected 0x"
                        + std::format("{:02x}", want)
                        + " — the recomputed unwind offsets would desync");
        }
    }

    // (2) Saved-reg stores. Walk them in prologue (emission) order to advance the
    //     byte cursor by EACH store's exact width, so a later GPR save's CodeOffset
    //     is right even after an interleaved FPR save. GPR save `mov [rsp+d32],r`
    //     = 8 B (REX.W always present). FPR save is DSS's `movsd [rsp+d32],xmm`
    //     (F2 0F 11 /r, the low-64 store — Win64 spill) = 8 B for xmm0-7, 9 B for
    //     xmm8-15 (REX.R). We EMIT a UWOP_SAVE_NONVOL per GPR but OMIT the FPR
    //     saves from the unwind codes: DSS saves only the low 64 bits via MOVSD,
    //     for which there is no x64 UWOP (SAVE_XMM128 restores a full 16 B from a
    //     movaps slot), and an XMM save does NOT affect RSP/return-address
    //     reconstruction — so stack unwinding stays exact. The handler-case xmm
    //     RESTORE is deferred to D-WIN64-XMM-UNWIND-RESTORE (c116): either spill
    //     xmm6-15 with movaps + emit SAVE_XMM128, or confirm no handler reads them.
    struct Code { std::uint8_t codeOffset; std::uint8_t opAndInfo; std::uint16_t node; bool hasNode; };
    std::vector<Code> saveCodes;
    // c116b (D-WIN64-XMM-UNWIND-RESTORE): a function that GUARDS a `__try` has an
    // exception handler that RESUMES in this frame post-unwind and runs parent code.
    // If that code reads a non-volatile xmm (xmm6-15) that was live before the fault,
    // the OS must restore it during the unwind — which needs a UWOP_SAVE_XMM128 in
    // this UNWIND_INFO (backed by a 16-byte movaps spill). DSS spills only the low 64
    // bits (movsd) and OMITS the FPR unwind codes (fine for NON-SEH functions: an xmm
    // save never affects RSP/return-address reconstruction). For a SEH function it is
    // NOT fine — so fail LOUD rather than emit an unwind table that silently fails to
    // restore a non-volatile xmm on the handler path. sqlite's WAL SEH functions are
    // pure integer/pointer code (empirically: 0 non-volatile-xmm spills across all
    // their prologues), so this guard never fires for sqlite; the day a SEH function
    // DOES spill an xmm, D-WIN64-XMM-UNWIND-RESTORE's spill-with-SAVE_XMM128 path must
    // land first. This converts the H5 "no consumer" proof into an enforced invariant.
    bool const guardsSeh = !ui.sehScopes.empty();
    std::uint32_t cursor = allocLen;
    for (auto const& sr : ui.savedRegs) {
        std::uint32_t const width =
            sr.isFpr ? (sr.regEncoding < 8u ? 8u : 9u) : 8u;
        cursor += width;
        if (cursor > 255u) {
            return fail("prologue exceeds 255 bytes (x64 UNWIND_INFO offsets are "
                        "single bytes) — switch the large-frame probe to __chkstk "
                        "(D-WIN64-CHKSTK-LARGE-PROLOGUE)");
        }
        if (sr.isFpr) {
            if (guardsSeh) {
                return fail("a __try-guarding function saves non-volatile xmm"
                            + std::to_string(sr.regEncoding)
                            + " but DSS omits UWOP_SAVE_XMM128 (spills low-64 via "
                              "movsd) — the __except handler could read an unrestored "
                              "xmm on resume. D-WIN64-XMM-UNWIND-RESTORE (spill xmm6-"
                              "15 with movaps + emit SAVE_XMM128) must land before a "
                              "SEH function may use a non-volatile xmm.");
            }
            continue;   // non-SEH: low-64 MOVSD save — omitted (RSP-irrelevant)
        }
        if (sr.saveOffset % 8u != 0u) {
            return fail("saved-reg offset " + std::to_string(sr.saveOffset)
                        + " not 8-aligned — UWOP_SAVE_NONVOL node is offset/8");
        }
        saveCodes.push_back(Code{
            static_cast<std::uint8_t>(cursor),
            static_cast<std::uint8_t>(kUwopSaveNonvol | (sr.regEncoding << 4)),
            static_cast<std::uint16_t>(sr.saveOffset / 8u), true});
    }
    std::uint32_t const sizeOfProlog = cursor;
    if (sizeOfProlog > 255u) {
        return fail("prologue " + std::to_string(sizeOfProlog) + " > 255 bytes");
    }

    // (3) The ALLOC code (at CodeOffset allocLen — the SMALLEST prologue offset).
    std::optional<Code> allocCode;
    if (frame > 0u) {
        std::uint32_t const slots = frame / 8u;
        if (frame <= 128u) {
            allocCode = Code{static_cast<std::uint8_t>(allocLen),
                             static_cast<std::uint8_t>(
                                 kUwopAllocSmall | ((slots - 1u) << 4)),
                             0u, false};
        } else if (slots <= 0xFFFFu) {  // ≤ 512 KiB — opinfo 0, one u16 node
            allocCode = Code{static_cast<std::uint8_t>(allocLen),
                             static_cast<std::uint8_t>(kUwopAllocLarge | (0u << 4)),
                             static_cast<std::uint16_t>(slots), true};
        } else {
            return fail("frame " + std::to_string(frame) + " > 512 KiB needs "
                        "UWOP_ALLOC_LARGE op-info=1 (u32 node) — no shipped "
                        "corpus reaches it (D-WIN64-HUGE-FRAME-ALLOC)");
        }
    }

    // (4) Emit: header, then codes DESCENDING by CodeOffset (saves — already in
    //     ascending store order, so REVERSED — then the ALLOC last).
    std::uint32_t nodeCount = 0;
    for (auto const& c : saveCodes) nodeCount += c.hasNode ? 2u : 1u;
    if (allocCode.has_value()) nodeCount += allocCode->hasNode ? 2u : 1u;
    if (nodeCount > 255u) return fail("unwind-code node count > 255");

    // c116 (D-WIN64-SEH-FUNCLETS): a function that guards a `__try` sets
    // UNW_FLAG_EHANDLER (bit 0 of the Flags nibble ⇒ (1<<3) in byte0's high nibble)
    // and carries a trailing handler-RVA + SCOPE_TABLE after the DWORD-aligned
    // codes. `__C_specific_handler` (the x64 C personality) walks the scope table.
    bool const hasSeh = !ui.sehScopes.empty();
    std::uint8_t const byte0 = hasSeh ? 0x09u   // Version=1 | Flags(EHANDLER=1)<<3
                                      : 0x01u;   // Version=1, Flags=0

    std::vector<std::uint8_t> out;
    out.push_back(byte0);
    out.push_back(static_cast<std::uint8_t>(sizeOfProlog));
    out.push_back(static_cast<std::uint8_t>(nodeCount));
    out.push_back(0x00u);                                  // FrameRegister=0, Offset=0
    auto pushCode = [&](Code const& c) {
        out.push_back(c.codeOffset);
        out.push_back(c.opAndInfo);
        if (c.hasNode) {
            out.push_back(static_cast<std::uint8_t>(c.node & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((c.node >> 8) & 0xFFu));
        }
    };
    for (auto it = saveCodes.rbegin(); it != saveCodes.rend(); ++it) pushCode(*it);
    if (allocCode.has_value()) pushCode(*allocCode);
    while (out.size() % 4u != 0u) out.push_back(0x00u);    // DWORD-align the codes

    if (hasSeh) {
        auto pushU32 = [&](std::uint32_t v) {
            out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
        };
        // ExceptionHandler RVA (= __C_specific_handler THUNK). Unknown until the
        // .idata pass resolves the thunk VA → emit a placeholder + record the
        // .xdata-global byte offset for back-patch. Every scope in this function
        // shares one personality; use the first scope's.
        std::uint32_t const handlerFieldOff =
            xdataBaseOffset + static_cast<std::uint32_t>(out.size());
        handlerPatches.push_back(
            SehHandlerPatch{handlerFieldOff, ui.sehScopes.front().personalitySymbol});
        pushU32(0u);   // placeholder — back-patched to the thunk RVA post-.idata

        // SCOPE_TABLE: u32 Count; then Count × { Begin, End, Handler, JumpTarget }.
        pushU32(static_cast<std::uint32_t>(ui.sehScopes.size()));
        for (auto const& s : ui.sehScopes) {
            std::uint32_t const filterRva = symbolToRva(s.filterFuncletSymbol);
            if (filterRva == 0u) {
                return fail("SEH scope's filter funclet symbol #"
                            + std::to_string(s.filterFuncletSymbol.v)
                            + " has no function RVA (unresolved funclet) — "
                              "D-WIN64-SEH-FUNCLETS");
            }
            pushU32(funcBeginRva + s.beginByteOffset);       // BeginAddress
            pushU32(funcBeginRva + s.endByteOffset);         // EndAddress
            pushU32(filterRva);                              // HandlerAddress (filter funclet)
            pushU32(funcBeginRva + s.jumpTargetByteOffset);  // JumpTarget (__except body)
        }
        // The blob already ends u32-aligned (every appended field is a u32).
    }
    return out;
}

// ── PE/COFF name encoding ───────────────────────────────────────
//
// PE/COFF section names + symbol names have a dual encoding: names
// ≤ 8 chars are inlined into a fixed 8-byte field NUL-padded.
// Longer names use `[u32 zero][u32 offset]` (symbol form) where
// `offset` is the byte offset into the string table. Section names
// use `/N` ASCII-decimal offset.

struct NameField {
    std::array<std::uint8_t, 8> bytes{};
};

// Symbol name encoding: NUL-pad to 8 chars OR set offset form.
[[nodiscard]] NameField encodeSymbolName(std::string_view name,
                                          std::uint32_t strtabOffset) {
    NameField out;
    if (name.size() <= 8) {
        for (std::size_t i = 0; i < name.size(); ++i) {
            out.bytes[i] = static_cast<std::uint8_t>(name[i]);
        }
        // remaining bytes already zero
    } else {
        // First 4 bytes = 0; last 4 bytes = strtab offset LE
        for (int i = 0; i < 4; ++i) {
            out.bytes[4 + i] =
                static_cast<std::uint8_t>(strtabOffset >> (i * 8));
        }
    }
    return out;
}

// Section name encoding: `.text` fits in 8 chars; long names use
// `/N` ASCII-decimal offset.
[[nodiscard]] NameField encodeSectionName(std::string_view name,
                                           std::uint32_t strtabOffset) {
    NameField out;
    if (name.size() <= 8) {
        for (std::size_t i = 0; i < name.size(); ++i) {
            out.bytes[i] = static_cast<std::uint8_t>(name[i]);
        }
    } else {
        std::string const slashForm = "/" + std::to_string(strtabOffset);
        for (std::size_t i = 0; i < slashForm.size() && i < 8; ++i) {
            out.bytes[i] = static_cast<std::uint8_t>(slashForm[i]);
        }
    }
    return out;
}

// String-table builder hoisted to `src/link/format/string_table.hpp`
// (D-LK4-9 closure). PE uses the U32SizePrefix init: bytes 0..3 hold
// an inclusive u32 size prefix stamped at release() time. Smallest
// legal offset returned by `add()` is 4 (just past the size).
using link::format::detail::StringTable;

// ── Section header record (in-memory) ───────────────────────────

struct PeSectionHeader {
    NameField     name{};
    std::uint32_t virtualSize           = 0;
    std::uint32_t virtualAddress        = 0;
    std::uint32_t sizeOfRawData         = 0;
    std::uint32_t pointerToRawData      = 0;
    std::uint32_t pointerToRelocations  = 0;
    std::uint32_t pointerToLinenumbers  = 0;
    std::uint16_t numberOfRelocations   = 0;
    std::uint16_t numberOfLinenumbers   = 0;
    std::uint32_t characteristics       = 0;
};

void writeSectionHeader(std::vector<std::uint8_t>& out,
                         PeSectionHeader const& h) {
    for (auto b : h.name.bytes) appendU8(out, b);
    appendU32LE(out, h.virtualSize);
    appendU32LE(out, h.virtualAddress);
    appendU32LE(out, h.sizeOfRawData);
    appendU32LE(out, h.pointerToRawData);
    appendU32LE(out, h.pointerToRelocations);
    appendU32LE(out, h.pointerToLinenumbers);
    appendU16LE(out, h.numberOfRelocations);
    appendU16LE(out, h.numberOfLinenumbers);
    appendU32LE(out, h.characteristics);
}

} // namespace

// Forward declaration: EXEC arm lives below the Obj body so the
// .obj path (LK2 cycle 1) keeps its top-of-file position.
namespace {
[[nodiscard]] std::vector<std::uint8_t>
encodeExec(AssembledModule const&    module,
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
    if (fmt.kind() != ObjectFormatKind::Pe) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"pe::encode called with non-PE format '"}
                 + std::string{fmt.name()}
                 + "' (kind="
                 + std::string{objectFormatKindName(fmt.kind())}
                 + ")");
        return {};
    }

    // PE/COFF has NO IMAGE_SECTION_HEADER for `symtab` / `strtab` —
    // the symbol table lives at `IMAGE_FILE_HEADER.PointerToSymbolTable`
    // and the string table immediately follows. The walker therefore
    // only REQUIRES `text`; `symtab` / `strtab` are looked up
    // optionally (never an error if a PE JSON omits them — architect
    // convergence on the prior cycle's spurious-failure trap).
    auto const* secText = requireSection(fmt, SectionKind::Text,
                                          "PE writer", reporter);
    if (!secText) return {};

    // Dispatch between .obj (Obj) and PE32+ image (Exec / Dll). The
    // two paths share only `secText` lookup + the schema kind check
    // — every other byte differs (.obj has no MS-DOS stub / PE sig /
    // optional header; image-side has no IMAGE_RELOCATION[] /
    // IMAGE_SYMBOL[] tables). validate() guarantees the optional
    // header is populated for Exec/Dll, so the image arm doesn't
    // re-check those fields. c152 (D-LK2-4): Dll routes through the
    // SAME `encodeExec` substrate the .exe uses (the elf.cpp
    // ET_EXEC/ET_DYN precedent) with the dll divergences keyed on
    // the schema's declared objectType inside the walker —
    // AddressOfEntryPoint = 0 (no DllMain; the entry trampoline is
    // already absent because the dll schema declares no
    // `processExit`), a `.edata` export directory wired into
    // data-directory[0], and the extern-address / text-abs-reloc
    // fail-loud belts.
    if (fmt.pe().objectType != PeObjectType::Obj) {
        return encodeExec(module, targetSchema, fmt, *secText, reporter);
    }
    // NOTE: the Obj path does not APPLY relocations (the assembler
    // stamped the bytes; the .obj writer serializes them + the
    // IMAGE_RELOCATION records link.exe resolves). `targetSchema` is
    // consulted only to VALIDATE data-item relocation kinds (linear,
    // absolute, non-zero width) before emitting their records —
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (PE/COFF Obj arm, c148).

    // ── D-LK-OBJECT-DATA-SECTION-RELOCATABLE (PE/COFF Obj arm, c148) ──
    //
    // The .obj writer emits DATA sections from `AssembledModule.dataItems`
    // — the PE analog of the ELF c142 ET_REL arm + the Mach-O c147 mirror.
    // A global lands in `.rdata` (rodata) / `.data` (data) / a SECOND
    // `.rdata` (relro — COFF has no relro segment; MSVC itself places
    // reloc-bearing const data in `.rdata` and the linked image
    // base-relocates it; link.exe merges same-name contributions) /
    // `.bss` (bss) with a SECTION-RELATIVE IMAGE_SYMBOL (SectionNumber =
    // 1-based ordinal, Value = offset within the section — the COFF
    // convention, NOT Mach-O's flat-address n_value) the final linker
    // binds. Section names / Characteristics come from the format JSON
    // rows (sectionByKind) — nothing hardcoded here.
    //
    // Thread-local items are rejected loud (mirror of the ELF / Mach-O
    // object-arm belts): the .obj walker emits no `.tls` /
    // IMAGE_TLS_DIRECTORY64 machinery (that is the PE EXEC arm,
    // D-CSUBSET-THREAD-LOCAL C3). Unreachable via the shipped pipeline
    // (the linker's acceptsDataSection gate fires first — the schema
    // declares no tdata/tbss), this is the anti-silent-drop belt for a
    // direct walker call or a format JSON opting in prematurely.
    for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
        auto const s = module.dataItems[i].section;
        if (s != DataSectionKind::Tdata && s != DataSectionKind::Tbss)
            continue;
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             std::format("pe::encode (Obj): AssembledData item #{} is "
                         "thread-local ({}) but the .obj walker emits no "
                         ".tls machinery - thread-locals are supported "
                         "only on the PE EXEC arm "
                         "(D-CSUBSET-THREAD-LOCAL).",
                         i, dataSectionKindName(s)));
        return {};
    }

    // Lay out each data-section kind via the shared kind-parameterized
    // substrate (`exec_data_section.hpp`) — the SAME helper the ELF /
    // Mach-O / PE-exec arms use. The alignment floor is 1 for every PE
    // row: validate() REJECTS a non-zero `addrAlign` on PE rows (PE
    // carries alignment in the Characteristics IMAGE_SCN_ALIGN_*BYTES
    // bits, which the writer derives from the H1-raised layout alignment
    // below — the per-contribution align class cl.exe itself stamps).
    // `data` + `relro` ALLOW item relocations (the c145/c147 analog: the
    // writer emits their IMAGE_RELOCATION tables below); a reloc-bearing
    // `rodata` item stays fail-loud (allow=false — the
    // RodataDataItemWithRelocationFailsLoud discipline), and `bss` items
    // carry no bytes to relocate.
    auto const rodataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Rodata,
        /*alignFloor=*/1, "pe::encode (Obj)", reporter);
    if (!rodataLayoutOpt.has_value()) return {};
    auto const& rodataLayout = *rodataLayoutOpt;
    auto dataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Data,
        /*alignFloor=*/1, "pe::encode (Obj)", reporter,
        /*allowItemRelocations=*/true);
    if (!dataLayoutOpt.has_value()) return {};
    auto& dataLayout = *dataLayoutOpt;
    auto const bssLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Bss,
        /*alignFloor=*/1, "pe::encode (Obj)", reporter);
    if (!bssLayoutOpt.has_value()) return {};
    auto const& bssLayout = *bssLayoutOpt;
    // Unlike the PE-exec arm (which FOLDS relro into `.rdata` bytes), the
    // .obj keeps relro a DISTINCT section (the ELF `.data.rel.ro` /
    // Mach-O `__DATA,__const` mirror) so it carries its OWN
    // IMAGE_RELOCATION table; it shares the `.rdata` NAME (the MSVC
    // placement) and link.exe merges the same-name contributions.
    auto relroLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::RelRoConst,
        /*alignFloor=*/1, "pe::encode (Obj)", reporter,
        /*allowItemRelocations=*/true);
    if (!relroLayoutOpt.has_value()) return {};
    auto& relroLayout = *relroLayoutOpt;

    bool const hasRodata = !rodataLayout.empty();
    bool const hasData   = !dataLayout.empty();
    bool const hasRelRo  = !relroLayout.empty();
    bool const hasBss    = !bssLayout.empty();
    // Each PRESENT section's schema row is MANDATORY — fail loud (the
    // format JSON must declare it; requireSection emits).
    ObjectFormatSectionInfo const* secRodata =
        hasRodata ? requireSection(fmt, SectionKind::Rodata,
                                   "PE writer (Obj)", reporter)
                  : nullptr;
    if (hasRodata && secRodata == nullptr) return {};
    ObjectFormatSectionInfo const* secData =
        hasData ? requireSection(fmt, SectionKind::Data,
                                 "PE writer (Obj)", reporter)
                : nullptr;
    if (hasData && secData == nullptr) return {};
    ObjectFormatSectionInfo const* secRelRo =
        hasRelRo ? requireSection(fmt, SectionKind::RelRoConst,
                                  "PE writer (Obj)", reporter)
                 : nullptr;
    if (hasRelRo && secRelRo == nullptr) return {};
    ObjectFormatSectionInfo const* secBss =
        hasBss ? requireSection(fmt, SectionKind::Bss,
                                "PE writer (Obj)", reporter)
               : nullptr;
    if (hasBss && secBss == nullptr) return {};

    // COFF section-contribution alignment: IMAGE_SCN_ALIGN_*BYTES bits in
    // Characteristics — value (log2(align)+1) << 20, representable for
    // 1..8192 bytes only. Derived from the H1-raised layout alignment
    // (max of every member item's alignment) so the final linker places
    // the contribution at least as aligned as its strictest member —
    // exactly the per-contribution class cl.exe stamps. Fail loud on a
    // non-power-of-two (no log2) or > 8192 (no encoding exists) — either
    // would otherwise ship silently-wrong align bits. The JSON row's
    // `type` must arrive WITHOUT align bits (the rows deliberately carry
    // none); OR-ing onto preexisting bits would corrupt the class field
    // (e.g. ALIGN_16 | ALIGN_4 reads as ALIGN_64).
    auto const alignBitsFor =
        [&](std::uint64_t maxAlign, ObjectFormatSectionInfo const& row,
            std::string_view sectionLabel,
            std::uint32_t& bitsOut) -> bool {
        constexpr std::uint32_t kAlignClassMask = 0x00F00000u;
        if (!std::has_single_bit(maxAlign) || maxAlign > 8192u) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encode (Obj): {} section alignment {} "
                             "has no IMAGE_SCN_ALIGN_*BYTES encoding - "
                             "COFF expresses object-section alignment as "
                             "a power of two between 1 and 8192 bytes.",
                             sectionLabel, maxAlign));
            return false;
        }
        if ((row.type & kAlignClassMask) != 0) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encode (Obj): {} schema row '{}' "
                             "Characteristics 0x{:x} already carries "
                             "IMAGE_SCN_ALIGN_*BYTES bits - the .obj "
                             "walker derives the align class from the "
                             "layout (H1) and OR-ing would corrupt it; "
                             "declare the row without align bits.",
                             sectionLabel, row.name, row.type));
            return false;
        }
        bitsOut = static_cast<std::uint32_t>(
                      std::countr_zero(maxAlign) + 1)
                  << 20;
        return true;
    };
    std::uint32_t rodataAlignBits = 0;
    std::uint32_t dataAlignBits   = 0;
    std::uint32_t relroAlignBits  = 0;
    std::uint32_t bssAlignBits    = 0;
    if (hasRodata && !alignBitsFor(rodataLayout.maxAlign, *secRodata,
                                   "rodata", rodataAlignBits)) {
        return {};
    }
    if (hasData && !alignBitsFor(dataLayout.maxAlign, *secData, "data",
                                 dataAlignBits)) {
        return {};
    }
    if (hasRelRo && !alignBitsFor(relroLayout.maxAlign, *secRelRo,
                                  "relro", relroAlignBits)) {
        return {};
    }
    if (hasBss && !alignBitsFor(bssLayout.maxAlign, *secBss, "bss",
                                bssAlignBits)) {
        return {};
    }
    // u32 overflow belt (SizeOfRawData / pointers are u32 wire fields —
    // the PE-exec checkU32Span mirror): surface a > 4 GiB span loud
    // rather than silently truncating at the narrowing casts below.
    auto const checkU32Span = [&](char const* kindName,
                                  std::uint64_t span) -> bool {
        if (span > std::numeric_limits<std::uint32_t>::max()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encode (Obj): {} section size {} bytes "
                             "exceeds the PE/COFF u32 wire limit "
                             "(2^32-1); the format cannot represent "
                             "this object.",
                             kindName, span));
            return false;
        }
        return true;
    };
    if (!checkU32Span("rodata", rodataLayout.spanSize)
        || !checkU32Span("data", dataLayout.spanSize)
        || !checkU32Span("relro", relroLayout.spanSize)
        || !checkU32Span("bss", bssLayout.spanSize)) {
        return {};
    }

    // ── Build .text + per-function symbols ─────────────────────
    std::vector<std::uint8_t> text;
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
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
        funcSyms.push_back({fn.symbol, start,
                            static_cast<std::uint64_t>(fn.bytes.size())});
    }

    // 1-based COFF section ordinals (IMAGE_SYMBOL SectionNumber /
    // section-header order): .text = 1, then each present data section
    // in emission order (rodata → data → relro → bss — bss last, the
    // no-file-bytes tail; matches the Mach-O zerofill-last mirror).
    std::int16_t sectOrdinalCursor = 1;   // .text
    std::int16_t const IDX_RODATA =
        hasRodata ? ++sectOrdinalCursor : 0;
    std::int16_t const IDX_DATA  = hasData ? ++sectOrdinalCursor : 0;
    std::int16_t const IDX_RELRO = hasRelRo ? ++sectOrdinalCursor : 0;
    std::int16_t const IDX_BSS   = hasBss ? ++sectOrdinalCursor : 0;
    std::size_t const numSections =
        static_cast<std::size_t>(sectOrdinalCursor);

    // ── Build symbol-table indices ─────────────────────────────
    //
    // Order: defined FUNCTION symbols → synthetic per-BLOCK locals
    // (computed-goto / jump-table targets at interior .text offsets) →
    // defined DATA symbols (SectionNumber = the data section's ordinal,
    // Value = the item's SECTION-RELATIVE offset) → undefined externs
    // (SectionNumber = 0). The IMAGE_RELOCATION SymbolTableIndex values
    // below must match this exact order.
    //
    // AUX-ENTRY ACCOUNTING: COFF SymbolTableIndex counts auxiliary
    // records as index slots. This writer emits ZERO aux records (every
    // appendSym call below passes numAuxSymbols = 0 — no section-
    // definition, file, or function-definition aux), so a symbol's index
    // equals its record ordinal with no adjustment. If aux records are
    // ever added, every index minted here must skip them.
    //
    // Symbol indices are minted strictly with `emplace.second` —
    // duplicates do not advance the index counter (silent-failure
    // H3 fix: prior version `nextSymIdx++` ran unconditionally,
    // desynchronizing the index map from the appended symtab).
    std::unordered_map<SymbolId, std::uint32_t> symIdxBySymbol;
    symIdxBySymbol.reserve(module.functions.size()
                           + module.dataItems.size());
    std::uint32_t nextSymIdx = 0;
    for (auto const& f : funcSyms) {
        auto const [it, fresh] =
            symIdxBySymbol.emplace(f.symId, nextSymIdx);
        if (!fresh) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"PE writer: duplicate defined symbol #"}
                     + std::to_string(f.symId.v)
                     + " (assembler emitted the same SymbolId twice "
                       "for distinct AssembledFunctions)");
            return {};
        }
        ++nextSymIdx;
    }

    // D-CSUBSET-COMPUTED-GOTO / jump tables: synthetic per-block symbols
    // (the `&&label` / dense-switch jump-table relocation targets) are
    // intra-module DEFINED LOCAL symbols pointing at an interior `.text`
    // offset. Register + emit them as IMAGE_SYM_CLASS_STATIC (LOCAL — a
    // sibling object's unrelated `sym_<id>` must never bind to or be
    // bound by an interior block address; the ELF STB_LOCAL / Mach-O
    // no-N_EXT mirror), SectionNumber = 1 (.text), Value =
    // funcTextStart + blockByteOffset (SECTION-relative — for .text that
    // equals the flat offset). Registering them BEFORE the undefined-
    // extern scan makes a jump-table slot's abs64 reloc resolve to this
    // defined local rather than the extern fallback fabricating a bogus
    // undefined `sym_<id>` nothing can ever define (the c147 Mach-O
    // CRITICAL fold, baked into the PE arm from the start).
    struct BlockSymRecord {
        SymbolId      symId{};
        std::uint64_t textOffset = 0;
    };
    std::vector<BlockSymRecord> blockSyms;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        for (auto const& bs : module.functions[fi].blockSymbols) {
            auto const [it, fresh] =
                symIdxBySymbol.emplace(bs.symbol, nextSymIdx);
            if (!fresh) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"PE writer: synthetic block symbol #"}
                         + std::to_string(bs.symbol.v)
                         + " collides with an already-registered symbol"
                           " - block SymbolIds must be unique.");
                return {};
            }
            ++nextSymIdx;
            blockSyms.push_back(
                {bs.symbol, funcTextStart[fi] + bs.blockByteOffset});
        }
    }

    // Defined DATA symbols — one IMAGE_SYMBOL per NAMED item (anonymous
    // SymbolId{} items are section-offset-referenced, never reloc
    // targets — M1 skip). A SymbolId colliding with an already-
    // registered (function / block / data) symbol is a producer-contract
    // breach — fail loud (the ELF ET_REL / Mach-O K_DuplicateDataSymbol
    // mirror), never a silent skip that would bind a reloc to the WRONG
    // symbol.
    struct DataSymRecord {
        SymbolId      symId{};
        std::int16_t  sectionNumber = 0;      // 1-based ordinal
        std::uint64_t sectionRelOffset = 0;   // COFF Value convention
    };
    std::vector<DataSymRecord> dataSyms;
    auto registerDataSyms =
        [&](link::format::ExecDataSectionLayout const& layout,
            std::int16_t sectionNumber) -> bool {
        for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
            AssembledData const& di =
                module.dataItems[layout.itemIndices[j]];
            if (di.symbol == SymbolId{}) continue;   // anonymous item (M1)
            if (!symIdxBySymbol.emplace(di.symbol, nextSymIdx).second) {
                emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                     std::format(
                         "pe::encode (Obj): data SymbolId={{ {} }} "
                         "collides with an already-emitted symbol - a "
                         "data global's SymbolId must be unique.",
                         di.symbol.v));
                return false;
            }
            ++nextSymIdx;
            dataSyms.push_back({di.symbol, sectionNumber,
                                layout.itemOffsets[j]});
        }
        return true;
    };
    if (hasRodata && !registerDataSyms(rodataLayout, IDX_RODATA)) {
        return {};
    }
    if (hasData && !registerDataSyms(dataLayout, IDX_DATA)) return {};
    if (hasRelRo && !registerDataSyms(relroLayout, IDX_RELRO)) return {};
    if (hasBss && !registerDataSyms(bssLayout, IDX_BSS)) return {};

    // Undefined externs: any reloc target that is neither a defined
    // function / block / data symbol. Scans DATA-ITEM relocations too
    // (the ELF c145 / Mach-O c147 mirror) — a relro const table of libc
    // function pointers whose targets appear in NO function reloc still
    // needs its IMAGE_SYM_UNDEFINED entries so the data IMAGE_RELOCATION
    // records below resolve their SymbolTableIndex.
    std::vector<SymbolId> externSyms;
    std::unordered_set<SymbolId> externSeen;
    auto noteExternTarget = [&](SymbolId target) {
        if (symIdxBySymbol.contains(target)) return;   // defined
        if (externSeen.insert(target).second) externSyms.push_back(target);
    };
    for (auto const& fn : module.functions)
        for (auto const& rel : fn.relocations) noteExternTarget(rel.target);
    for (auto const& di : module.dataItems)
        for (auto const& rel : di.relocations) noteExternTarget(rel.target);
    for (auto const& e : externSyms) {
        if (symIdxBySymbol.emplace(e, nextSymIdx).second) ++nextSymIdx;
    }

    // ── Build .text relocation table ───────────────────────────
    //
    // PE/COFF stores relocations per-section. Each IMAGE_RELOCATION is
    // 10 bytes packed; each reloc-bearing section header names its own
    // table via PointerToRelocations/NumberOfRelocations.

    // PE/COFF convention: the relocation addend lives IN THE PATCH
    // BYTES (the section's raw data), NOT as a separate field on
    // the IMAGE_RELOCATION record. ELF Rela carries `r_addend`
    // explicitly; PE has no such column. If an `AssembledModule`
    // FUNCTION reloc arrives with `rel.addend != 0`, the PE walker
    // would silently drop the addend — that's exactly the silent-
    // failure class the substrate discipline rejects. Fail loud so the
    // caller fixes the assembler (or pre-stamps the addend into the
    // patch bytes). DATA-item relocations differ: their slot bytes ARE
    // the addend carrier, so the data-reloc pass below WRITES the
    // addend in-slot instead (see its block comment).
    std::vector<std::uint8_t> textRelocs;
    std::uint32_t textRelocCount = 0;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        auto const& fn = module.functions[fi];
        std::uint64_t const fnStart = funcTextStart[fi];
        for (auto const& rel : fn.relocations) {
            if (rel.addend != 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"PE writer: relocation in symbol #"}
                         + std::to_string(fn.symbol.v)
                         + " carries addend=" + std::to_string(rel.addend)
                         + " but PE/COFF stores addends in the section's "
                           "patch bytes, not on IMAGE_RELOCATION. The "
                           "assembler must pre-stamp the addend into "
                           ".text (or emit addend=0 for the call/jmp "
                           "rel32 case where link.exe applies the RIP "
                           "bias intrinsically).");
                return {};
            }
            auto const* fmtReloc = fmt.relocationByKind(rel.kind);
            if (fmtReloc == nullptr) continue;  // unreachable: linker
                                                // engine pre-validated
            auto const it = symIdxBySymbol.find(rel.target);
            if (it == symIdxBySymbol.end()) continue;  // ditto
            std::uint32_t const va =
                static_cast<std::uint32_t>(fnStart + rel.offset);
            appendU32LE(textRelocs, va);
            appendU32LE(textRelocs, it->second);
            appendU16LE(textRelocs,
                static_cast<std::uint16_t>(fmtReloc->nativeId));
            ++textRelocCount;
        }
    }

    // ── Build per-DATA-SECTION IMAGE_RELOCATION tables ─────────
    //
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (PE/COFF Obj arm, c148) — the
    // `.rela.data` / `.rela.data.rel.ro` analog: a data section whose
    // items carry their OWN relocations (a const/mutable pointer table —
    // sqlite's VFS method tables + `aSyscall[]`) gets its own
    // IMAGE_RELOCATION table (PointerToRelocations/NumberOfRelocations).
    // Each record: u32 VirtualAddress = the slot's offset WITHIN its
    // section (a .obj section's VA base is 0), u32 SymbolTableIndex, u16
    // Type = the reloc kind's format-declared nativeId (an abs64 slot
    // arrives as the schema's IMAGE_REL_AMD64_ADDR64 row — read from the
    // JSON, never hardcoded). A data reloc is NEVER a call, so the plain
    // nativeId is always used.
    //
    // ADDEND: IMAGE_RELOCATION has NO addend column. The .text table
    // above fails loud on a non-zero addend (the assembler pre-stamps
    // call/jmp patch bytes). A DATA slot is different: link.exe reads
    // the slot CONTENT as the implicit addend for ADDR64 (final = S +
    // slot) — the in-place-addend convention cl.exe itself emits for
    // `&arr[1]`-style initializers, and exactly what the PE-exec DIR64
    // path realizes when `applyDataItemRelocations` resolves S+A into
    // the slot. So the writer WRITES rel.addend into the slot bytes here
    // (the producer zero-fills every reloc slot, so the write is
    // authoritative, not additive). Failing loud instead would
    // gratuitously reject initializers the ELF/Mach-O mirrors support.
    auto buildDataRelocTable =
        [&](link::format::ExecDataSectionLayout& layout,
            std::string_view sectionLabel,
            std::vector<std::uint8_t>& relocsOut,
            std::uint32_t& relocCountOut) -> bool {
        for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
            AssembledData const& di =
                module.dataItems[layout.itemIndices[j]];
            std::uint64_t const itemOff = layout.itemOffsets[j];
            for (auto const& rel : di.relocations) {
                auto const* fmtReloc = fmt.relocationByKind(rel.kind);
                if (fmtReloc == nullptr) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format("pe::encode (Obj): {} data "
                                     "relocation kind {} is not declared "
                                     "by object format '{}'.",
                                     sectionLabel, rel.kind.v, fmt.name()));
                    return false;
                }
                auto const* tri = targetSchema.relocationInfo(rel.kind);
                if (tri == nullptr) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format("pe::encode (Obj): {} data "
                                     "relocation kind {} has no "
                                     "TargetRelocationInfo on target "
                                     "schema '{}'.",
                                     sectionLabel, rel.kind.v,
                                     targetSchema.name()));
                    return false;
                }
                // A data pointer slot needs a plain LINEAR absolute fixup
                // with a concrete width. `pcRelative` alone is NOT a
                // sufficient discriminator (it is a Linear-only field —
                // the loader forces every non-Linear formula row to
                // pcRelative=false), so reject any non-Linear kind
                // outright (the c147 Mach-O gate, copied verbatim).
                if (tri->formulaKind != RelocFormulaKind::Linear
                    || tri->pcRelative || tri->widthBytes == 0) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format("pe::encode (Obj): {} data item "
                                     "SymbolId={{ {} }} relocation '{}' "
                                     "is pc-relative, zero-width, or a "
                                     "non-linear instruction fixup - a "
                                     "data pointer needs a plain absolute "
                                     "fixup with a concrete write width.",
                                     sectionLabel, di.symbol.v, tri->name));
                    return false;
                }
                auto const it = symIdxBySymbol.find(rel.target);
                if (it == symIdxBySymbol.end()) {
                    emit(reporter, DiagnosticCode::K_SymbolUndefined,
                         std::format("pe::encode (Obj): {} data "
                                     "relocation target symbol #{} has no "
                                     "symtab entry - substrate-invariant "
                                     "violation.",
                                     sectionLabel, rel.target.v));
                    return false;
                }
                std::uint64_t const patchOff = itemOff + rel.offset;
                if (rel.offset + tri->widthBytes > di.bytes.size()
                    || patchOff + tri->widthBytes > layout.bytes.size()) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format("pe::encode (Obj): {} data item "
                                     "SymbolId={{ {} }} relocation offset "
                                     "{} + width {} overruns the item's "
                                     "{} bytes.",
                                     sectionLabel, di.symbol.v, rel.offset,
                                     static_cast<int>(tri->widthBytes),
                                     di.bytes.size()));
                    return false;
                }
                // In-place addend (see the block comment above): the slot
                // bytes carry A; link.exe computes S + slot. addend 0
                // rewrites the producer's zero slot — a no-op by
                // construction.
                auto const a = static_cast<std::uint64_t>(rel.addend);
                for (std::uint8_t b = 0; b < tri->widthBytes; ++b) {
                    layout.bytes[static_cast<std::size_t>(patchOff) + b] =
                        static_cast<std::uint8_t>((a >> (8u * b)) & 0xFFu);
                }
                appendU32LE(relocsOut,
                            static_cast<std::uint32_t>(patchOff));
                appendU32LE(relocsOut, it->second);
                appendU16LE(relocsOut,
                            static_cast<std::uint16_t>(fmtReloc->nativeId));
                ++relocCountOut;
            }
        }
        return true;
    };
    std::vector<std::uint8_t> dataRelocs;
    std::uint32_t dataRelocCount = 0;
    std::vector<std::uint8_t> relroRelocs;
    std::uint32_t relroRelocCount = 0;
    if (hasData
        && !buildDataRelocTable(dataLayout, "data", dataRelocs,
                                dataRelocCount)) {
        return {};
    }
    if (hasRelRo
        && !buildDataRelocTable(relroLayout, "relro", relroRelocs,
                                relroRelocCount)) {
        return {};
    }

    // Silent-failure C1 guard: PE/COFF spec §4 says when a section's
    // relocation count > 65534, the writer must set
    // IMAGE_SCN_LNK_NRELOC_OVFL (0x01000000) in section Characteristics
    // AND put the real count in the first IMAGE_RELOCATION's
    // VirtualAddress field. That path is not implemented; emit a hard
    // diagnostic rather than silently truncating to u16. Anchored at
    // plan 14 §3.1 as a deferred item (overflow path arrives with the
    // first module that needs it). Applied PER SECTION — the u16
    // NumberOfRelocations is a per-header field.
    auto const checkRelocCountFits = [&](char const* sectionLabel,
                                         std::uint32_t count) -> bool {
        if (count <= 0xFFFEu) return true;
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"PE writer: "} + sectionLabel
                 + " relocation count " + std::to_string(count)
                 + " exceeds u16 capacity; PE/COFF NRELOC_OVFL overflow "
                   "path is anchored as plan 14 §3.1 D-LK2-3 (LK6 trigger). "
                   "Module too large for cycle-1 PE writer.");
        return false;
    };
    if (!checkRelocCountFits(".text", textRelocCount)
        || !checkRelocCountFits(".data", dataRelocCount)
        || !checkRelocCountFits("relro", relroRelocCount)) {
        return {};
    }

    // ── Build string table + symbol table records ─────────────
    //
    // String table starts pre-reserved with 4 bytes for its size
    // prefix; `add()` returns the offset (always ≥ 4). For each
    // symbol name ≤ 8 chars we inline; longer names get appended
    // to the string table.

    StringTable strtab{StringTable::Init::U32SizePrefix};
    std::vector<std::uint8_t> symtab;

    auto appendSym = [&](NameField const& nameField, std::uint32_t value,
                          std::int16_t sectionNumber, std::uint16_t type,
                          std::uint8_t storageClass,
                          std::uint8_t numAuxSymbols) {
        for (auto b : nameField.bytes) appendU8(symtab, b);
        appendU32LE(symtab, value);
        appendI16LE(symtab, sectionNumber);
        appendU16LE(symtab, type);
        appendU8(symtab, storageClass);
        appendU8(symtab, numAuxSymbols);
    };

    auto emitSymWithName = [&](std::string_view name, std::uint32_t value,
                                std::int16_t sectionNumber,
                                std::uint16_t type,
                                std::uint8_t storageClass) {
        std::uint32_t offset = 0;
        if (name.size() > 8) offset = strtab.add(name);
        appendSym(name.size() <= 8
                      ? encodeSymbolName(name, 0)
                      : encodeSymbolName(name, offset),
                  value, sectionNumber, type, storageClass, /*aux=*/0);
    };

    constexpr std::int16_t kTextSectionNumber = 1;

    // D-LK-OBJECT-EXTERN-SYMBOL-NAMES: real source-level C names for the
    // externally-visible defined functions; `sym_<id>` fallback for
    // static/local/synthesized symbols.
    link::format::ObjectSymbolNames const objNames{module};

    // Defined function symbols (GLOBAL EXTERNAL, type=FUNCTION,
    // SectionNumber=1 for `.text`).
    for (auto const& f : funcSyms) {
        std::string const symName = objNames.definedName(f.symId, "sym_");
        emitSymWithName(symName,
                        static_cast<std::uint32_t>(f.valueInText),
                        kTextSectionNumber,
                        IMAGE_SYM_DTYPE_FUNCTION,
                        IMAGE_SYM_CLASS_EXTERNAL);
    }
    // Synthetic per-block symbols (computed-goto labels / dense-switch
    // jump-table targets): IMAGE_SYM_CLASS_STATIC — deliberately LOCAL,
    // so a sibling object's unrelated `sym_<id>` can never bind to (or
    // be bound by) an interior block address — SectionNumber = 1
    // (.text), Value = the section-relative text offset. Emission order
    // matches the symIdxBySymbol registration above (functions → blocks
    // → data → externs) so relocation SymbolTableIndex values line up.
    // The ELF STB_LOCAL / Mach-O no-N_EXT block-symbol legs are the
    // mirrors (c147 fold, baked in).
    for (auto const& b : blockSyms) {
        std::string const symName =
            std::string{"sym_"} + std::to_string(b.symId.v);
        emitSymWithName(symName,
                        static_cast<std::uint32_t>(b.textOffset),
                        kTextSectionNumber,
                        /*type=*/0,
                        IMAGE_SYM_CLASS_STATIC);
    }
    // Defined DATA symbols (D-LK-OBJECT-DATA-SECTION-RELOCATABLE, PE
    // arm): EXTERNAL, SectionNumber = the item's 1-based section
    // ordinal, Value = the item's SECTION-RELATIVE offset (the COFF
    // convention — dumpbin shows e.g. `table` as `00000000 SECT4
    // External`; NOT Mach-O's flat-address n_value), type = 0 (COFF
    // data symbols are `notype`; DTYPE_FUNCTION is functions-only).
    // Externally-visible items carry their real (identity-mangled)
    // name; static/synthesized ones keep `sym_<id>` (the same carve-out
    // the function loop above uses).
    for (auto const& d : dataSyms) {
        std::string const symName = objNames.definedName(d.symId, "sym_");
        emitSymWithName(symName,
                        static_cast<std::uint32_t>(d.sectionRelOffset),
                        d.sectionNumber,
                        /*type=*/0,
                        IMAGE_SYM_CLASS_EXTERNAL);
    }
    // Undefined extern symbols (SectionNumber=0=UNDEF, type=0,
    // value=0). D-LK-OBJECT-EXTERN-CALL-RELOCATABLE (PE arm, c148): the
    // undefined extern carries its REAL import name (`externName` — the
    // pipeline-mangled `mangledName`; PE x64 C mangling is IDENTITY, so
    // `puts` is emitted verbatim with no leading underscore) so the
    // FINAL linker (link.exe / lld-link) resolves it against a sibling
    // object or an import library — the PE analog of the ELF c141 /
    // Mach-O c146 fix. `sym_<id>` remains the fallback for a reloc
    // target that is neither defined nor a known import.
    // Map each extern SymbolId to its isData class (from the module's extern
    // imports). A FUNCTION extern (isData==false) carries
    // IMAGE_SYM_DTYPE_FUNCTION -- mirroring the defined-function path above --
    // so the c170 COFF object reader reconstructs the function/data class
    // FAITHFULLY (c170 silent-failure-review fold: without the hint a
    // reconstructed function extern defaults to DATA, which would route a
    // `call rel32 -> extern` through the data IAT slot). An extern with no
    // ExternImport row (a reloc target the source never declared) stays
    // type=0; cl.exe / link.exe treat a type-0 undefined symbol as either.
    std::unordered_map<SymbolId, bool> externIsData;
    for (auto const& imp : module.externImports)
        externIsData.emplace(imp.symbol, imp.isData);
    for (auto const& e : externSyms) {
        std::string const symName = objNames.externName(e, "sym_");
        auto const it = externIsData.find(e);
        bool const isFunction = (it != externIsData.end()) && !it->second;
        std::uint16_t const symType =
            isFunction ? static_cast<std::uint16_t>(IMAGE_SYM_DTYPE_FUNCTION) : 0u;
        emitSymWithName(symName, /*value=*/0, IMAGE_SYM_UNDEFINED,
                        symType, IMAGE_SYM_CLASS_EXTERNAL);
    }

    // ── Layout the file: header → section headers → section data
    //    → per-section relocs → symbol table → string table ─────
    //
    // Section count is DERIVED from `numSections` (the ordinal cursor
    // above — architect D-LK2-5 convergence; a hardcoded literal would
    // silently corrupt the file whenever a data section appears). Raw
    // section data is PACKED back-to-back with no inter-section file
    // padding — cl.exe's own convention (its raw pointers land at odd
    // offsets); a COFF contribution's placement alignment comes from the
    // Characteristics ALIGN bits at final link, never its file offset.
    // A data-free module derives numSections = 1 and every offset below
    // collapses to the historical single-section layout — BYTE-IDENTICAL
    // output (an explicit test pin).
    std::vector<PeSectionHeader> sectionHeaders;
    std::size_t const sectionDataOffsetBase =
        kFileHeaderSize + numSections * kSectionHeaderSize;

    std::uint32_t const textRawPointer =
        static_cast<std::uint32_t>(sectionDataOffsetBase);
    std::uint32_t const textRawSize    =
        static_cast<std::uint32_t>(text.size());
    // File-backed data sections follow .text (rodata → data → relro);
    // .bss stores NO file bytes (PointerToRawData = 0 — its span rides
    // SizeOfRawData only, the cl.exe-witnessed COFF .obj convention).
    std::uint32_t rawCursor = textRawPointer + textRawSize;
    std::uint32_t const rodataRawPointer = hasRodata ? rawCursor : 0u;
    if (hasRodata) {
        rawCursor += static_cast<std::uint32_t>(rodataLayout.spanSize);
    }
    std::uint32_t const dataRawPointer = hasData ? rawCursor : 0u;
    if (hasData) {
        rawCursor += static_cast<std::uint32_t>(dataLayout.spanSize);
    }
    std::uint32_t const relroRawPointer = hasRelRo ? rawCursor : 0u;
    if (hasRelRo) {
        rawCursor += static_cast<std::uint32_t>(relroLayout.spanSize);
    }
    // Relocation tables follow ALL file-backed section bytes (.text's
    // first, then each reloc-bearing data section's — the same order the
    // section headers are emitted). A no-reloc table keeps pointer = 0.
    std::uint32_t relocCursor = rawCursor;
    std::uint32_t const textRelocPointer =
        textRelocCount > 0 ? relocCursor : 0u;
    relocCursor += static_cast<std::uint32_t>(textRelocs.size());
    std::uint32_t const dataRelocPointer =
        dataRelocCount > 0 ? relocCursor : 0u;
    relocCursor += static_cast<std::uint32_t>(dataRelocs.size());
    std::uint32_t const relroRelocPointer =
        relroRelocCount > 0 ? relocCursor : 0u;
    relocCursor += static_cast<std::uint32_t>(relroRelocs.size());

    std::uint32_t const symtabPointer = relocCursor;
    std::uint32_t const symtabSizeBytes =
        static_cast<std::uint32_t>(symtab.size());
    // Substrate invariant: every `appendSym` writes exactly 18
    // bytes. A future bug that dropped one byte would silently
    // produce fewer symbols than expected (the integer division
    // truncates the trailing partial record). Surface the
    // violation rather than letting the symtab silently shrink
    // (silent-failure-hunter H5).
    if (symtab.size() % kSymbolRecordSize != 0) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{"PE writer: symbol-table byte size "}
                 + std::to_string(symtab.size())
                 + " is not a multiple of IMAGE_SYMBOL size (18) — "
                   "substrate invariant violation in appendSym path");
        return {};
    }
    std::uint32_t const numberOfSymbols =
        static_cast<std::uint32_t>(symtab.size() / kSymbolRecordSize);

    // ── Emit ──
    std::vector<std::uint8_t> bytes;
    bytes.reserve(symtabPointer + symtabSizeBytes + strtab.size());

    // The .obj arm never adds section names to the string table, so a
    // schema row name > 8 chars would silently encode as a dangling
    // `/0` string-table reference. Every shipped PE row name fits; fail
    // loud on a future JSON edit rather than emit a corrupt header.
    auto const checkSectionNameFits =
        [&](ObjectFormatSectionInfo const& row) -> bool {
        if (row.name.size() <= 8) return true;
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("pe::encode (Obj): section name '{}' exceeds 8 "
                         "chars; the .obj writer does not emit the "
                         "COFF long-name (/N string-table) form.",
                         row.name));
        return false;
    };
    if (!checkSectionNameFits(*secText)
        || (hasRodata && !checkSectionNameFits(*secRodata))
        || (hasData && !checkSectionNameFits(*secData))
        || (hasRelRo && !checkSectionNameFits(*secRelRo))
        || (hasBss && !checkSectionNameFits(*secBss))) {
        return {};
    }

    // Section header for .text; data-section headers follow in ordinal
    // order (rodata → data → relro → bss).
    {
        PeSectionHeader hText{};
        hText.name                  = encodeSectionName(secText->name, 0);
        hText.virtualSize           = 0;        // 0 for .obj
        hText.virtualAddress        = 0;        // 0 for .obj
        hText.sizeOfRawData         = textRawSize;
        hText.pointerToRawData      = textRawPointer;
        hText.pointerToRelocations  = textRelocPointer;
        hText.pointerToLinenumbers  = 0;
        hText.numberOfRelocations   = static_cast<std::uint16_t>(textRelocCount);
        hText.numberOfLinenumbers   = 0;
        hText.characteristics       = secText->type;  // PE uses substrate
                                                       // `type` field for
                                                       // Characteristics
                                                       // (.text row carries
                                                       // its align bits
                                                       // statically)
        sectionHeaders.push_back(hText);
    }
    // Data-section headers. VirtualSize/VirtualAddress = 0 (.obj);
    // SizeOfRawData = the section span (for .bss the ZERO-FILL span with
    // PointerToRawData = 0 — the cl.exe-witnessed convention: dumpbin
    // shows `.bss` SizeOfRawData = 0x190, no raw pointer);
    // Characteristics = the schema row's class bits OR the walker-derived
    // IMAGE_SCN_ALIGN_*BYTES (validated align-bit-free above).
    auto const pushDataSectionHeader =
        [&](ObjectFormatSectionInfo const& row,
            link::format::ExecDataSectionLayout const& layout,
            bool zeroFill, std::uint32_t rawPointer,
            std::uint32_t relocPointer, std::uint32_t relocCount,
            std::uint32_t alignBits) {
        PeSectionHeader h{};
        h.name                 = encodeSectionName(row.name, 0);
        h.virtualSize          = 0;
        h.virtualAddress       = 0;
        h.sizeOfRawData        =
            static_cast<std::uint32_t>(layout.spanSize);
        h.pointerToRawData     = zeroFill ? 0u : rawPointer;
        h.pointerToRelocations = relocPointer;
        h.pointerToLinenumbers = 0;
        h.numberOfRelocations  = static_cast<std::uint16_t>(relocCount);
        h.numberOfLinenumbers  = 0;
        h.characteristics      = row.type | alignBits;
        sectionHeaders.push_back(h);
    };
    if (hasRodata) {
        pushDataSectionHeader(*secRodata, rodataLayout, /*zeroFill=*/false,
                              rodataRawPointer, /*relocPointer=*/0,
                              /*relocCount=*/0, rodataAlignBits);
    }
    if (hasData) {
        pushDataSectionHeader(*secData, dataLayout, /*zeroFill=*/false,
                              dataRawPointer, dataRelocPointer,
                              dataRelocCount, dataAlignBits);
    }
    if (hasRelRo) {
        pushDataSectionHeader(*secRelRo, relroLayout, /*zeroFill=*/false,
                              relroRawPointer, relroRelocPointer,
                              relroRelocCount, relroAlignBits);
    }
    if (hasBss) {
        pushDataSectionHeader(*secBss, bssLayout, /*zeroFill=*/true,
                              /*rawPointer=*/0, /*relocPointer=*/0,
                              /*relocCount=*/0, bssAlignBits);
    }
    // Belt: the header vector must match the ordinal cursor the symbol
    // SectionNumbers were minted from — a drift here would bind every
    // data symbol to the WRONG section at final link.
    if (sectionHeaders.size() != numSections) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("pe::encode (Obj): emitted {} section headers "
                         "but the ordinal cursor derived {} - "
                         "substrate-invariant violation.",
                         sectionHeaders.size(), numSections));
        return {};
    }

    // IMAGE_FILE_HEADER
    auto const& id = fmt.pe();
    appendU16LE(bytes, id.machine);
    appendU16LE(bytes, static_cast<std::uint16_t>(sectionHeaders.size()));
    appendU32LE(bytes, 0);  // TimeDateStamp = 0 (deterministic)
    appendU32LE(bytes, symtabPointer);
    appendU32LE(bytes, numberOfSymbols);
    appendU16LE(bytes, 0);  // SizeOfOptionalHeader = 0 for .obj
    appendU16LE(bytes, id.characteristics);

    // IMAGE_SECTION_HEADER table
    for (auto const& h : sectionHeaders) {
        writeSectionHeader(bytes, h);
    }

    // Section raw data (.text, then each file-backed data section —
    // .bss stores nothing), then the relocation tables (.text, data,
    // relro — the layouts' bytes already carry the in-slot addends the
    // data-reloc pass patched).
    bytes.insert(bytes.end(), text.begin(), text.end());
    if (hasRodata) {
        bytes.insert(bytes.end(), rodataLayout.bytes.begin(),
                     rodataLayout.bytes.end());
    }
    if (hasData) {
        bytes.insert(bytes.end(), dataLayout.bytes.begin(),
                     dataLayout.bytes.end());
    }
    if (hasRelRo) {
        bytes.insert(bytes.end(), relroLayout.bytes.begin(),
                     relroLayout.bytes.end());
    }
    bytes.insert(bytes.end(), textRelocs.begin(), textRelocs.end());
    bytes.insert(bytes.end(), dataRelocs.begin(), dataRelocs.end());
    bytes.insert(bytes.end(), relroRelocs.begin(), relroRelocs.end());

    // The emitted byte cursor must equal the symtab pointer stamped into
    // the file header — a file-backed layout whose bytes diverge from
    // its spanSize would silently shift every trailing structure.
    if (bytes.size() != symtabPointer) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("pe::encode (Obj): assembled section data ends "
                         "at {} but the layout computed symtab pointer "
                         "{} - substrate-invariant violation (a "
                         "file-backed layout's bytes must equal its "
                         "spanSize).",
                         bytes.size(), symtabPointer));
        return {};
    }

    // Symbol table
    bytes.insert(bytes.end(), symtab.begin(), symtab.end());

    // String table (with size prefix)
    auto strtabBytes = std::move(strtab).release();
    bytes.insert(bytes.end(), strtabBytes.begin(), strtabBytes.end());

    return bytes;
}

// ── PE32+ image walker (.exe Exec / .dll Dll) — LK2 cycle 2 + c152 ──
//
// Closes plan 14 §3.1 D-LK2-1 (Exec) + D-LK2-4 part 1 (Dll, c152 —
// the PE mirror of the ELF c150 `.so`). Emits a minimal-valid PE32+
// image the Windows loader will accept: MS-DOS stub + PE signature +
// IMAGE_FILE_HEADER (EXECUTABLE_IMAGE, plus IMAGE_FILE_DLL on the
// dll arm — both flags come from the SCHEMA's `pe.characteristics`,
// validate()-pinned) + IMAGE_OPTIONAL_HEADER64 + section headers +
// section data (fileAlignment-padded). Intra-module relocations are
// applied in-place via the LK6 cycle 1 structured-formula triple
// (`pcRelative + addendBias + widthBytes`).
//
// Dll divergences (all keyed on the schema's declared objectType —
// the elf.cpp isDyn precedent, never a format-name branch):
//   * AddressOfEntryPoint = 0 — a DSS .dll ships with NO DllMain
//     (legal PE: the loader skips process/thread notifications for
//     an entry-less module, the `link /NOENTRY` shape; the entry
//     trampoline is already absent because the dll schema declares
//     no `processExit` — linker.cpp's schema-keyed condition). The
//     DllMain arm is the pinned follow-up D-LK2-DLL-DLLMAIN-ENTRY.
//   * `.edata` export directory (data-directory[0]): Export Address
//     Table + LEXICOGRAPHICALLY SORTED Name Pointer Table + Ordinal
//     Table + DllName, built from `module.symbols` (externally-
//     visible DEFINED functions + data globals under their real
//     names — the same set the ELF dyn `.dynsym` exports, c150).
//     GetProcAddress BINARY-SEARCHES the name table; the sort is a
//     tested invariant (an unsorted table silently breaks lookup).
//   * `.reloc` completeness: a DLL relocates — every absolute 64-bit
//     slot in the image already gets an IMAGE_REL_BASED_DIR64 row
//     (the c145 data-item machinery; the TLS directory VAs on exec).
//     The remaining absolute-slot class — an absolute fixup in
//     `.text` — is REJECTED loud on any DYNAMIC_BASE image
//     (D-LK-PE-IMAGE-TEXT-ABS-RELOC below): its site is not
//     collected into `.reloc`, so shipping it would leave a
//     preferred-base address the loader never adjusts.
//   * symbolVa keeps the schema's preferred ImageBase (0x180000000,
//     the MSVC dll convention) — NO base-0 trick (unlike ELF dyn):
//     PE DIR64 slots carry absolute preferred-base VAs the loader
//     adjusts by the load delta.
//
// The validate() pass enforces the optional-header field
// population, so this function never has to re-check those
// invariants — read directly from `fmt.peOptionalHeader()`.

namespace {

[[nodiscard]] std::vector<std::uint8_t>
encodeExec(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& fmt,
           ObjectFormatSectionInfo const& secText,
           DiagnosticReporter&       reporter) {
    auto const& id = fmt.pe();
    auto const& oh = fmt.peOptionalHeader();
    // c152 (D-LK2-4): the dll-arm discriminator — schema-declared
    // objectType, mirroring elf.cpp's `isDyn`.
    bool const isDll = id.objectType == PeObjectType::Dll;

    // ── (a) Build .text body + per-function start map ─────────
    std::vector<std::uint8_t> text;
    std::vector<std::uint64_t> funcTextStart;
    funcTextStart.reserve(module.functions.size());
    for (auto const& fn : module.functions) {
        std::uint64_t const start = text.size();
        funcTextStart.push_back(start);
        text.insert(text.end(), fn.bytes.begin(), fn.bytes.end());
    }

    if (text.empty()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             "pe::encodeExec: `.text` is empty — every "
             "AssembledFunction contributed zero bytes. An exec "
             "with no instructions would crash at entry.");
        return {};
    }

    // ── (a2) Reserve the import-thunk block at the tail of .text ──
    // D-FFI-PE-IMPORT-THUNK: one code thunk per FUNCTION extern (`jmp
    // *[IAT slot]`) — the PE analog of an ELF PLT stub / Mach-O __stubs
    // entry. RESERVED here, BEFORE .text is sized, so textVirtualSize /
    // textRawSize / the data-chain RVAs (incl. .idata) all account for
    // it; the bytes are written in step (c2) once each IAT slot's VA is
    // known. `symbolVa[funcExtern]` then names the thunk (direct-plt),
    // making an address-taken import a CALLABLE code address. Zero
    // externs → `resize(+0)` no-op → byte-identical output (the
    // extern-free byte-for-byte writer pins hold).
    //
    // c149 (D-LK-EXTERN-DATA-IMPORT, the PE half): a DATA extern
    // (isData — an msvcrt `_fmode`-class data export) is never CALLED,
    // so it gets NO thunk — only its IAT slot, which the loader fills
    // with the imported OBJECT's address (`__imp_<name>` semantics; the
    // Mach-O __got non-lazy-pointer model, same `got-indirect` binding).
    // The thunk block is therefore COMPACTED to the function externs
    // (`funcExternIdxs[j]` = the extern index of the j-th thunk —
    // mirrors macho.cpp's D-LK-MACHO-DATA-EXTERN-DEAD-STUB compaction),
    // while the `.idata` ILT/IAT/HINT-NAME layout below stays LOCKSTEP
    // with EVERY extern (a data import's name resolves identically).
    // symbolVa[dataExtern] binds to the IAT SLOT VA — never a thunk VA
    // (reading jump-stub bytes as the object's value is the exact
    // silent-miscompile class the linker's data-import gate guards).
    std::vector<std::size_t> funcExternIdxs;
    funcExternIdxs.reserve(module.externImports.size());
    std::size_t numDataExterns = 0;
    for (std::size_t i = 0; i < module.externImports.size(); ++i) {
        if (module.externImports[i].isData) ++numDataExterns;
        else funcExternIdxs.push_back(i);
    }
    if (numDataExterns > 0) {
        // The linker's pre-walker gate admits data imports only when
        // the schema DECLARES a binding model; this walker implements
        // exactly `got-indirect` (the IAT-slot pointer). A future
        // second member reaching here without a walker arm must fail
        // loud, not silently get an IAT-slot binding it did not
        // declare (mirrors elf.cpp's copy-relocation assertion and
        // macho.cpp's got-indirect assertion).
        auto const binding = fmt.dataImportBinding();
        if (!binding.has_value()
            || *binding != DataImportBinding::GotIndirect) {
            emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
                 std::string{"pe::encodeExec: module carries "}
                     + std::to_string(numDataExterns)
                     + " extern DATA import(s) but format '"
                     + std::string{fmt.name()}
                     + "' does not declare 'dataImportBinding': "
                       "\"got-indirect\" -- the only data-import "
                       "mechanism this walker implements "
                       "(D-LK-EXTERN-DATA-IMPORT).");
            return {};
        }
    }
    std::size_t const peThunkSize    = peThunkSizeFor(id.machine);
    std::size_t const numImportThunks = funcExternIdxs.size();
    if (numImportThunks > 0 && peThunkSize == 0) {
        emit(reporter, DiagnosticCode::K_FormatLacksImportSupport,
             std::format("pe::encodeExec: machine 0x{:x} declares {} extern "
                         "function import(s) but has no import-thunk emitter "
                         "-- an address-taken import needs a callable thunk. "
                         "Add a per-machine emitter (see emitPeThunk).",
                         id.machine, numImportThunks));
        return {};
    }
    std::size_t const thunkBlockOffset = text.size();
    text.resize(text.size() + numImportThunks * peThunkSize, 0u);

    // ── (b) Resolve entry-function index from schema.entryPoint
    //
    // Mirrors the ELF ET_EXEC arm (D-LK1-1 follow-up): empty
    // entryPoint defaults to functions[0]; non-empty looks up by
    // synthesized `sym_<id>` name today (real-name resolution
    // closes with the HIR→AssembledFunction symbol-name thread).
    // D-LK10-ENTRY Slice C: image-entry resolution shared across
    // all 3 walkers via `resolveEntryFnIdx`. Honors
    // `imageEntryOverride` first (trampoline at functions[index]),
    // falls back to `format.entryPoint()` string resolution, then
    // defaults to functions[0]. Synthesized-name prefix is "sym_"
    // for PE/ELF.
    //
    // c152 (D-LK2-4): the Dll arm resolves NO entry —
    // AddressOfEntryPoint is emitted as 0 (no DllMain; the loader
    // skips the notification call for an entry-less module). A
    // caller-provided `imageEntryOverride` on a dll module is a
    // producer-contract breach (the linker never injects a
    // trampoline for a schema without `processExit`; an override
    // here means someone built entry machinery for an entry-less
    // artifact) — fail loud rather than silently drop it.
    std::size_t entryFnIdx = 0;
    if (isDll) {
        if (module.imageEntryOverride.has_value()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encodeExec (dll): module carries "
                             "imageEntryOverride = {} but a PE dll has "
                             "no image entry (AddressOfEntryPoint = 0, "
                             "no DllMain -- D-LK2-DLL-DLLMAIN-ENTRY is "
                             "the pinned follow-up). The entry override "
                             "would be silently discarded (D-LK2-4).",
                             *module.imageEntryOverride));
            return {};
        }
    } else {
        auto const entryIdxOpt = link::format::resolveEntryFnIdx(
            module, fmt, "sym_", "pe::encodeExec", reporter);
        if (!entryIdxOpt.has_value()) return {};
        entryFnIdx = *entryIdxOpt;
    }

    // ── (c) Synthesize .idata section for extern imports (LK6
    //         cycle 2a). PE32+ import-table layout per PE/COFF §6.4:
    //         ImageImportDescriptor[N+1] (terminator) → ILT[] → IAT[]
    //         → HINT/NAME table → DLL name strings. Each descriptor
    //         is 20 B; each ILT/IAT slot is u64; HINT/NAME is u16
    //         hint=0 + NUL-terminated symbol name (padded to even);
    //         DLL names are NUL-terminated. The loader uses the IAT
    //         slot at runtime: it overwrites IAT[i] with the
    //         resolved function pointer (for PE32+, IAT and ILT are
    //         identical at file-image time; loader patches IAT
    //         in-place).
    //
    // Build a map `libraryPath → ordered list of extern symbols`
    // (preserves declaration order within each library; deterministic
    // build outputs). One ImageImportDescriptor per library.
    std::vector<std::string> libraryOrder;  // libs in declaration order
    std::unordered_map<std::string, std::vector<std::size_t>>
        externsByLib;  // libraryPath → indices into module.externImports
    for (std::size_t i = 0; i < module.externImports.size(); ++i) {
        auto const& ext = module.externImports[i];
        auto const it = externsByLib.find(ext.libraryPath);
        if (it == externsByLib.end()) {
            libraryOrder.push_back(ext.libraryPath);
            externsByLib.emplace(ext.libraryPath, std::vector<std::size_t>{i});
        } else {
            it->second.push_back(i);
        }
    }

    bool const hasImports = !module.externImports.empty();
    std::uint32_t const sectionAlignE = oh.sectionAlignment;
    std::uint32_t const textVirtualSizeE =
        static_cast<std::uint32_t>(
            (text.size() + sectionAlignE - 1) & ~(sectionAlignE - 1ull));

    // ── DataSectionLayout: per-section layout vocabulary ──────────
    //
    // Consolidates the (rva, virtualSize, rawSize, rawPointer,
    // headerIndex) tuple that every PE walker section must carry
    // into a single value. `headerIndex` is captured at the
    // sectionHeaders push site below, so subsequent patch-ins
    // (sizeOfRawData / pointerToRawData) index by NAMED field
    // rather than the magic `sectionHeaders[1u + (rdata ?
    // 1u : 0u)]` arithmetic that an earlier shape used.
    //
    // **Anchor D-LK2-RODATA-SECTION-LAYOUT-RECORD**: this type is
    // walker-local today (PE is the sole consumer); when D-LK1-
    // RODATA (ELF) or D-LK3-RODATA (Mach-O) closes, hoist to
    // `src/link/format/data_section_layout.hpp` as shared
    // substrate. Trigger: 2nd walker arm.
    struct DataSectionLayout {
        std::uint32_t rva         = 0;
        std::uint32_t virtualSize = 0;  // section-aligned
        std::uint32_t rawSize     = 0;  // file-aligned
        std::uint32_t rawPointer  = 0;
        std::size_t   headerIndex = 0;
    };

    // ── Build .rdata bytes from AssembledData.dataItems ───────────
    //
    // D-LK2-RODATA closure: PE walker emits `.rdata` between `.text`
    // and `.idata` when the module carries any `AssembledData` item
    // with `section == DataSectionKind::Rodata`. Per-item bytes are
    // placed at the item's `Alignment` (padding zero-filled); the
    // section's raw size is file-aligned, its virtual size section-
    // aligned (PE/COFF §3.4).
    //
    // Schema discipline: requireSection(Rodata) fail-louds if the
    // format JSON omits the row — silent walker emission is
    // forbidden. SymbolId→VA resolution for relocations targeting
    // rodata items is deferred to D-LK4-RODATA-WALKER-RELOC-BASE-
    // OFFSET (no producer emits such relocations yet — see plan
    // §3.1 anchor row).
    //
    // D-LK2-RODATA (`.rdata`) + D-LK4-DATA-PRODUCER (writable `.data` +
    // zero-fill `.bss`): each `AssembledData` kind is laid out by the SAME
    // shared, kind-parameterized `buildExecDataSection` helper the ELF/Mach-O
    // writers use (single-sourced byte layout). `.data` carries MEM_WRITE in
    // its Characteristics (read from the schema row — a store must not fault);
    // `.bss` is uninitialized (SizeOfRawData=0, PointerToRawData=0 — NO file
    // bytes, the loader zero-fills VirtualSize bytes). Section VA/file RVAs
    // chain text → rdata → data → bss → idata → reloc.
    using link::format::detail::alignUp;
    // ── D-CSUBSET-THREAD-LOCAL (TLS C3): lay out the thread-local TEMPLATE.
    // `tdata` (initialized `thread_local int g = 7;`) becomes the ONE `.tls`
    // section's file bytes; `tbss` (zero-init `thread_local int g;`)
    // contributes NO file bytes — its span folds into the
    // IMAGE_TLS_DIRECTORY64 `SizeOfZeroFill` tail (there is no separate
    // `.tbss` section on PE). `allowItemRelocations=true` on the tdata call:
    // a `thread_local char *msg = "hi";` template item carries an abs64
    // data->data reloc to the rodata string, patched below (and given a
    // DIR64 base-reloc site) so each thread's copy holds the rebased pointer.
    auto const tdataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Tdata,
        /*alignFloor=*/1, "pe::encodeExec", reporter,
        /*allowItemRelocations=*/true);
    if (!tdataLayoutOpt.has_value()) return {};
    auto const& tdataLayout = *tdataLayoutOpt;
    auto const tbssLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Tbss,
        /*alignFloor=*/1, "pe::encodeExec", reporter,
        /*allowItemRelocations=*/false);   // zero-fill has no on-disk site
    if (!tbssLayoutOpt.has_value()) return {};
    auto const& tbssLayout = *tbssLayoutOpt;
    bool const hasTdata = !tdataLayout.empty();
    bool const hasTbss  = !tbssLayout.empty();
    bool const hasTls   = hasTdata || hasTbss;
    // The `.tls` section row (name + Characteristics) — looked up by the
    // ThreadData kind. A module carrying TLS items but a format that
    // declares no `.tls` row is a fail-loud misconfiguration (the
    // supportedDataSections gate advertised tdata/tbss but the row was
    // omitted). The anti-static-alias backstop: never lay a thread-local
    // item out as ordinary data (that is the storage-duration miscompile).
    ObjectFormatSectionInfo const* secTls =
        fmt.sectionByKind(SectionKind::ThreadData);
    if (hasTls && secTls == nullptr) {
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             "pe::encodeExec: module carries thread-local items but the "
             "format declares no ThreadData ('.tls') section row "
             "(D-CSUBSET-THREAD-LOCAL).");
        return {};
    }
    // The format must declare a tlsAccess block naming the `_tls_index`
    // singleton (the writer binds the reserved id to the slot's VA + wires
    // IMAGE_TLS_DIRECTORY64.AddressOfIndex to it). A module with TLS but no
    // such block cannot be given a working access sequence.
    if (hasTls && (!fmt.tlsAccess().has_value()
                   || fmt.tlsAccess()->tlsIndexSlotName.empty())) {
        emit(reporter, DiagnosticCode::K_FormatLacksThreadLocalSupport,
             "pe::encodeExec: module carries thread-local items but the "
             "format declares no tlsAccess.tlsIndexSlotName — the writer "
             "cannot bind the module TLS-index slot "
             "(D-CSUBSET-THREAD-LOCAL).");
        return {};
    }
    // Per-thread block geometry (mirrors the ELF walker's PT_TLS math): the
    // initialized template (`tdataSpan` bytes), then the tbss zero-fill
    // starting at its aligned base. `SizeOfZeroFill` = the block memsz beyond
    // the template. A tbss item's secrel (registered below) is
    // `tbssBlockBase + itemOffset`, landing in that zero-fill tail.
    // ── D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN (audit FOLD-1): fail loud on an
    // OVER-ALIGNED thread-local. The Windows x64 loader allocates each
    // thread's static-TLS block at only MEMORY_ALLOCATION_ALIGNMENT (16 bytes
    // = 2*sizeof(void*)), and IMAGE_TLS_DIRECTORY64 carries NO block-base-
    // alignment field to request more — so a var whose alignment exceeds 16
    // would be SILENTLY under-aligned in every thread's copy (a SIMD / atomic
    // thread_local relying on it is UB). ELF's PT_TLS p_align honors any
    // alignment (the C1/C2 `_Alignas(32) thread_local` witnesses run green),
    // so this gate is PE-format-LOCAL — the format writer's own ABI knowledge,
    // never a shared-substrate branch. Alignments <= 16 (every normal scalar /
    // pointer / small aggregate, incl. `_Alignas(16)`) pass; the gate bites
    // ONLY explicit over-alignment.
    constexpr std::uint64_t kPeX64TlsBlockBaseAlign = 16;  // MEMORY_ALLOCATION_ALIGNMENT
    std::uint64_t tlsMaxAlign = 1;
    if (hasTdata) tlsMaxAlign = std::max(tlsMaxAlign, tdataLayout.maxAlign);
    if (hasTbss)  tlsMaxAlign = std::max(tlsMaxAlign, tbssLayout.maxAlign);
    if (hasTls && tlsMaxAlign > kPeX64TlsBlockBaseAlign) {
        emit(reporter, DiagnosticCode::K_ThreadLocalOveralignedForFormat,
             std::format(
                 "pe::encodeExec: a thread-local object requires {}-byte "
                 "alignment, but the Windows x64 loader guarantees only "
                 "{}-byte (MEMORY_ALLOCATION_ALIGNMENT) static-TLS block-base "
                 "alignment and IMAGE_TLS_DIRECTORY64 has no field to request "
                 "more — the per-thread copy would be silently under-aligned "
                 "(D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN).",
                 tlsMaxAlign, kPeX64TlsBlockBaseAlign));
        return {};
    }
    std::uint64_t const tdataSpan = tdataLayout.spanSize;   // 0 if none
    std::uint64_t const tbssBlockBase =
        alignUp(tdataSpan,
                std::max<std::uint64_t>(1, hasTbss ? tbssLayout.maxAlign : 1));
    std::uint64_t const tlsBlockMemsz =
        hasTbss ? tbssBlockBase + tbssLayout.spanSize : tdataSpan;
    std::uint64_t const tlsZeroFill = tlsBlockMemsz - tdataSpan;
    // The u32-wire-limit guard for these spans rides the SAME `checkU32Span`
    // call the rdata/data/bss spans use (below, once the lambda is defined).
    // `allowItemRelocations=true`: PE patches data-item (data→data) relocations
    // (F5 symbol-address pointers, folded relro tables) into the laid-out
    // bytes below + emits their `.reloc` DIR64 base relocations.
    auto rdataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Rodata,
        /*alignFloor=*/1, "pe::encodeExec", reporter,
        /*allowItemRelocations=*/true);
    if (!rdataLayoutOpt.has_value()) return {};
    auto& rdataDataLayout = *rdataLayoutOpt;
    auto const dataLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Data,
        /*alignFloor=*/1, "pe::encodeExec", reporter,
        /*allowItemRelocations=*/true);
    if (!dataLayoutOpt.has_value()) return {};
    auto const& dataDataLayout = *dataLayoutOpt;
    auto const bssLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::Bss,
        /*alignFloor=*/1, "pe::encodeExec", reporter,
        /*allowItemRelocations=*/true);
    if (!bssLayoutOpt.has_value()) return {};
    auto const& bssDataLayout = *bssLayoutOpt;
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): a CONST global carrying LOAD-TIME
    // relocations (a const function-pointer table — sqlite os_win.c aSyscall[])
    // lands in `relro`. In the PE32+ image we FOLD it into read-only `.rdata`
    // (the design contract: PE base-relocates `.rdata` — the loader applies the
    // IMAGE_REL_BASED_DIR64 base-relocation into each slot BEFORE the page is
    // sealed read-only, exactly as it already does for F5 symbol-address global
    // pointers). Merging routes relro through the SAME `.rdata` machinery (the
    // rdata offset map, the reloc-loop DIR64 base-reloc arm, the section header)
    // — no parallel section, byte-identical for a relro-free module.
    auto relroLayoutOpt = link::format::buildExecDataSection(
        module.dataItems, DataSectionKind::RelRoConst,
        /*alignFloor=*/1, "pe::encodeExec", reporter,
        /*allowItemRelocations=*/true);
    if (!relroLayoutOpt.has_value()) return {};
    link::format::mergeFileBackedDataSection(rdataDataLayout, *relroLayoutOpt);
    bool const hasRdata = !rdataDataLayout.empty();
    bool const hasData  = !dataDataLayout.empty();
    bool const hasBss   = !bssDataLayout.empty();
    // u32 overflow guard (PE/COFF SizeOfImage / virtualSize / sizeOfRawData are
    // u32 wire fields). A producer that lands > 4 GiB in any section would
    // silently truncate at the narrowing casts below; surface it loud.
    auto const checkU32Span = [&](char const* kindName,
                                   std::uint64_t span) -> bool {
        if (span > std::numeric_limits<std::uint32_t>::max()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encodeExec: .{} size {} bytes exceeds PE/COFF "
                             "u32 wire limit (2^32-1); the format cannot "
                             "represent this image.",
                             kindName, span));
            return false;
        }
        return true;
    };
    if (!checkU32Span("rdata", rdataDataLayout.spanSize)
        || !checkU32Span("data", dataDataLayout.spanSize)
        || !checkU32Span("bss", bssDataLayout.spanSize)
        || !checkU32Span("tls-template", tdataSpan)      // TLS C3
        || !checkU32Span("tls-zerofill", tlsZeroFill)) { // TLS C3
        return {};
    }
    // `.rdata` AND `.data` bytes are MUTABLE here because PE patches data-item
    // relocations into them post-layout (below): folded relro const tables + F5
    // string-rodata pointers land in `.rdata`; F5 MUTABLE symbol-address global
    // pointers (`char* g="..."`, `int* p=&x`) land in `.data` and carry an abs64
    // data→data reloc to their target's VA (the patched bytes are emitted at the
    // `.data` insert below).
    std::vector<std::uint8_t> rdataBytes = rdataDataLayout.bytes;
    std::vector<std::uint8_t> dataBytes  = dataDataLayout.bytes;
    // Original-dataItems-index → `.rdata` section-relative offset, for the
    // data-item-relocation patch loop below (the layout records these for the
    // items it placed — including the relro items merged in above).
    std::unordered_map<std::size_t, std::uint64_t> rdataOffsetByIndex;
    for (std::size_t j = 0; j < rdataDataLayout.itemIndices.size(); ++j) {
        rdataOffsetByIndex.emplace(rdataDataLayout.itemIndices[j],
                                   rdataDataLayout.itemOffsets[j]);
    }
    // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): the same index→section-offset map for
    // `.data` items, so a MUTABLE symbol-address global pointer's abs64 reloc can
    // be patched into `.data` (the data-item reloc loop below resolves either map).
    std::unordered_map<std::size_t, std::uint64_t> dataOffsetByIndex;
    for (std::size_t j = 0; j < dataDataLayout.itemIndices.size(); ++j) {
        dataOffsetByIndex.emplace(dataDataLayout.itemIndices[j],
                                  dataDataLayout.itemOffsets[j]);
    }
    // TLS C3 (D-CSUBSET-THREAD-LOCAL): the SAME index→section-offset map for
    // `.tls` TEMPLATE (tdata) items, so a reloc-bearing template item
    // (`thread_local char *msg = "hi";`) is patched into `tlsBytes` by the
    // data-item reloc loop + given its DIR64 base-reloc site (the template
    // opens the `.tls` section at offset 0, so the tls offset IS the tdata
    // layout offset). Tbss items are zero-fill (no file bytes, no reloc site).
    std::unordered_map<std::size_t, std::uint64_t> tlsOffsetByIndex;
    for (std::size_t j = 0; j < tdataLayout.itemIndices.size(); ++j) {
        tlsOffsetByIndex.emplace(tdataLayout.itemIndices[j],
                                 tdataLayout.itemOffsets[j]);
    }
    ObjectFormatSectionInfo const* secRodata = nullptr;
    ObjectFormatSectionInfo const* secData   = nullptr;
    ObjectFormatSectionInfo const* secBss    = nullptr;
    if (hasRdata) {
        secRodata = link::format::detail::requireSection(
            fmt, SectionKind::Rodata, "pe::encodeExec", reporter);
        if (secRodata == nullptr) return {};
    }
    if (hasData) {
        secData = link::format::detail::requireSection(
            fmt, SectionKind::Data, "pe::encodeExec", reporter);
        if (secData == nullptr) return {};
    }
    if (hasBss) {
        secBss = link::format::detail::requireSection(
            fmt, SectionKind::Bss, "pe::encodeExec", reporter);
        if (secBss == nullptr) return {};
    }
    // Section RVAs chain contiguously (each section-aligned). `.rdata` after
    // `.text`; `.data` after `.rdata`; `.bss` after `.data`. virtualSize is
    // section-aligned; for `.bss` it is the zero-fill memory extent.
    std::optional<DataSectionLayout> rdata;
    std::optional<DataSectionLayout> data;
    std::optional<DataSectionLayout> bss;
    std::optional<DataSectionLayout> tls;   // TLS C3 (.tls template + dir + index)
    std::uint32_t dataChainRva =
        static_cast<std::uint32_t>(secText.virtualAddress) + textVirtualSizeE;
    if (hasRdata) {
        DataSectionLayout layout;
        layout.rva = dataChainRva;
        layout.virtualSize = static_cast<std::uint32_t>(
            alignUp(rdataDataLayout.spanSize, sectionAlignE));
        rdata = layout;
        dataChainRva += layout.virtualSize;
    }
    if (hasData) {
        DataSectionLayout layout;
        layout.rva = dataChainRva;
        layout.virtualSize = static_cast<std::uint32_t>(
            alignUp(dataDataLayout.spanSize, sectionAlignE));
        data = layout;
        dataChainRva += layout.virtualSize;
    }
    if (hasBss) {
        DataSectionLayout layout;
        layout.rva = dataChainRva;
        layout.virtualSize = static_cast<std::uint32_t>(
            alignUp(bssDataLayout.spanSize, sectionAlignE));
        bss = layout;
        dataChainRva += layout.virtualSize;
    }
    // ── D-CSUBSET-THREAD-LOCAL (TLS C3): the `.tls` section — laid out AFTER
    // `.bss` in the VA chain. Its file bytes are
    //   [ FULL per-thread block | pad→8 | IMAGE_TLS_DIRECTORY64 (40) |
    //     _tls_index (4) ].
    // ★ The template span [0, tlsBlockMemsz) covers the ENTIRE per-thread
    // block — the initialized `tdata` bytes THEN the zero-init `tbss` region
    // as literal ZERO bytes in the raw data — and SizeOfZeroFill is 0. This is
    // deliberate (and what MSVC does): the Windows loader does NOT reliably
    // zero the directory's SizeOfZeroFill region for the MAIN thread's static
    // TLS (set up very early in process init), so a `static thread_local int
    // c;` left to SizeOfZeroFill reads garbage — EMPIRICALLY CONFIRMED (embed
    // → c reads 0; SizeOfZeroFill → c reads garbage). Embedding the whole
    // block as raw template bytes makes the loader's template COPY zero it.
    // (A future large zero-init TLS could re-introduce SizeOfZeroFill to avoid
    // file bloat once a loader-honoring path is verified — deferral.) The 40-
    // byte directory + 4-byte index sit PAST End so they are never per-thread-
    // copied (a writable `.tls` is a valid home for the loader-read directory
    // AND the loader-written index, and keeping all TLS structures here avoids
    // entangling the delicate .rdata/.data span arithmetic). The 3 directory
    // VA fields + any template pointer bytes get IMAGE_REL_BASED_DIR64 base-
    // reloc sites (collected before the .reloc builder); AddressOfCallBacks=0
    // gets NONE (a reloc on 0 → the loader adds the slide → a non-null garbage
    // callback pointer).
    std::vector<std::uint8_t> tlsBytes;
    std::uint64_t const tlsDirOffset = alignUp(tlsBlockMemsz, 8);  // dir past block
    std::uint64_t const tlsIndexOffset = tlsDirOffset + 40;     // dir is 40 B
    std::uint64_t const tlsFileSize = tlsIndexOffset + 4;       // + 4-byte index
    std::uint32_t tlsRva = 0;
    if (hasTls) {
        DataSectionLayout layout;
        layout.rva = dataChainRva;
        layout.virtualSize = static_cast<std::uint32_t>(
            alignUp(tlsFileSize, sectionAlignE));
        tls = layout;
        tlsRva = layout.rva;
        dataChainRva += layout.virtualSize;
        // Build the section's file bytes: the tdata template bytes (mutable —
        // a reloc-bearing `thread_local char*` item is patched in the data-
        // item loop below), then ZERO-FILLED up to tlsFileSize — which zeroes
        // the tbss region of the block [tdataSpan, tlsBlockMemsz), the pad to
        // the 8-aligned directory offset, the 40-byte directory (VA fields
        // patched once symbolVa is built), and the 4-byte index (stays 0 for
        // the loader to fill).
        tlsBytes = tdataLayout.bytes;
        while (tlsBytes.size() < tlsFileSize) tlsBytes.push_back(0);
    }
    std::size_t const rdataSize = rdataDataLayout.spanSize;
    std::size_t const dataSize  = dataDataLayout.spanSize;

    // ── (b3) Windows x64 unwind tables — D-WIN64-PDATA-XDATA-UNWIND ──
    // `.xdata` (UNWIND_INFO blobs, one per function that carries frame info)
    // + `.pdata` (a RUNTIME_FUNCTION per entry: {BeginAddress, EndAddress,
    // UnwindInfoAddress}). Chained after `.bss`, BEFORE `.idata`/`.reloc`
    // (so their VA cursor + the .reloc prevVaEnd hand-off account for them
    // with no extra edit). Emitted ONLY for the x64 machine (arm64-PE would
    // need its own unwind format — not shipped); a function WITHOUT
    // `unwind` (a leaf / the post-pipeline entry trampoline) gets NO entry
    // (x64 leaf treatment). RUNTIME_FUNCTIONs are naturally ascending-by-
    // BeginAddress (funcTextStart is ascending) — the loader binary-searches.
    std::vector<std::uint8_t> xdataBytes;
    std::vector<std::uint8_t> pdataBytes;
    std::optional<DataSectionLayout> xdata;
    std::optional<DataSectionLayout> pdata;
    // c116 (D-WIN64-SEH-FUNCLETS): deferred back-patches of each SEH function's
    // UNWIND_INFO handler-RVA field (the __C_specific_handler thunk RVA, resolved
    // in the .idata pass below). Filled by buildFunctionUnwindInfo, applied after
    // `externThunkVaBySym` is populated.
    std::vector<SehHandlerPatch> sehHandlerPatches;
    if (id.machine == kMachineAmd64PE) {
        std::uint32_t const textRva0 =
            static_cast<std::uint32_t>(secText.virtualAddress);
        // c116: SymbolId → image-RVA for DEFINED functions (the filter funclet's
        // scope-table HandlerAddress). funcTextStart is parallel to
        // module.functions; a symbol not found ⇒ 0 (buildFunctionUnwindInfo
        // fail-louds). Built once (cheap) only on the x64 PE path.
        std::unordered_map<SymbolId, std::uint32_t> funcRvaBySym;
        funcRvaBySym.reserve(module.functions.size());
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            funcRvaBySym[module.functions[i].symbol] =
                textRva0 + static_cast<std::uint32_t>(funcTextStart[i]);
        }
        std::function<std::uint32_t(SymbolId)> const symbolToRva =
            [&](SymbolId s) -> std::uint32_t {
                auto it = funcRvaBySym.find(s);
                return it == funcRvaBySym.end() ? 0u : it->second;
            };

        struct PdataEntry { std::size_t funcIdx; std::uint32_t xdataOffset; };
        std::vector<PdataEntry> pdataEntries;
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            auto const& fn = module.functions[fi];
            if (!fn.unwind.has_value()) continue;
            std::uint32_t const funcBeginRva =
                textRva0 + static_cast<std::uint32_t>(funcTextStart[fi]);
            auto uwOpt = buildFunctionUnwindInfo(
                *fn.unwind, fn.bytes, fi, funcBeginRva, symbolToRva,
                static_cast<std::uint32_t>(xdataBytes.size()), sehHandlerPatches,
                reporter);
            if (!uwOpt.has_value()) return {};
            pdataEntries.push_back(
                {fi, static_cast<std::uint32_t>(xdataBytes.size())});
            xdataBytes.insert(xdataBytes.end(), uwOpt->begin(), uwOpt->end());
        }
        if (!xdataBytes.empty()) {
            DataSectionLayout xl;
            xl.rva = dataChainRva;
            xl.virtualSize =
                static_cast<std::uint32_t>(alignUp(xdataBytes.size(), sectionAlignE));
            xdata = xl;
            dataChainRva += xl.virtualSize;
            std::uint32_t const textRva0 =
                static_cast<std::uint32_t>(secText.virtualAddress);
            for (auto const& e : pdataEntries) {
                auto const& fn = module.functions[e.funcIdx];
                std::uint32_t const begin = textRva0
                    + static_cast<std::uint32_t>(funcTextStart[e.funcIdx]);
                std::uint32_t const end =
                    begin + static_cast<std::uint32_t>(fn.bytes.size());
                appendU32LE(pdataBytes, begin);
                appendU32LE(pdataBytes, end);
                appendU32LE(pdataBytes, xl.rva + e.xdataOffset);
            }
            DataSectionLayout pl;
            pl.rva = dataChainRva;
            pl.virtualSize =
                static_cast<std::uint32_t>(alignUp(pdataBytes.size(), sectionAlignE));
            pdata = pl;
            dataChainRva += pl.virtualSize;
        }
    }

    // ── (b4) `.edata` export directory — c152, D-LK2-4 (dll arm only) ──
    //
    // The PE mirror of the ELF dyn `.dynsym` export set (c150, elf.cpp
    // (b.7)): every externally-visible DEFINED symbol — function or
    // data global — from `module.symbols` (the same real-name rows the
    // ET_REL `.symtab` uses, c139) becomes a named export. Local /
    // Hidden / Internal symbols stay out (a `static` function is not
    // part of the library's ABI); Weak exports export plainly (the PE
    // export table has no binding column). Classification is by
    // definition table: a row naming a function exports its `.text`
    // RVA; a row naming a data item exports its section RVA
    // (`.rdata`/`.data`/`.bss` — a data export's EAT RVA points into
    // the mapped section; the consumer knows it is an object).
    //
    // Byte layout (PE/COFF §6.3), one self-contained `.edata` section:
    //   [0]    IMAGE_EXPORT_DIRECTORY (40 B)
    //   [40]   Export Address Table          — u32 RVA × N
    //   [...]  Name Pointer Table            — u32 RVA × N, SORTED
    //   [...]  Ordinal Table                 — u16 × N
    //   [...]  DllName string + export-name strings
    //
    // ★ SORT INVARIANT: GetProcAddress BINARY-SEARCHES the Name
    // Pointer Table (memcmp order — unsigned bytes); an unsorted
    // table silently breaks lookup for a subset of names. The records
    // are sorted here and the EAT is laid out IN SORTED ORDER with
    // IDENTITY ordinals (ordinal[i] = i, Base 1) — the link.exe
    // convention (dumpbin ground truth: ordinals 1..N follow the
    // sorted names). The emitted-table sort is a tested invariant
    // (deliberately out-of-order input names in the pin).
    //
    // The directory's Name field (the DLL's self-name) is emitted as
    // an EMPTY string: per-artifact identity does not belong in a
    // shipped format schema (the c150 decision that leaves DT_SONAME
    // absent); GetProcAddress never reads it, and DSS emits no
    // forwarder RVAs — the only loader consumer of that name.
    // (Forwarder detection is RVA-range-based: an EAT entry pointing
    // INSIDE the export directory's [rva, rva+size) span is a
    // forwarder string. Every DSS export RVA points at `.text` /
    // `.rdata` / `.data` / `.bss`, all OUTSIDE `.edata`, so no export
    // can be misread as a forwarder.)
    struct PeExportRec {
        std::string   name;   // real (already-mangled) source name
        std::uint32_t rva = 0;
    };
    std::vector<PeExportRec> dllExports;
    if (isDll) {
        std::unordered_map<std::uint32_t, std::size_t> funcIdxBySym;
        funcIdxBySym.reserve(module.functions.size());
        for (std::size_t i = 0; i < module.functions.size(); ++i) {
            funcIdxBySym.emplace(module.functions[i].symbol.v, i);
        }
        // Data-item SymbolId → image RVA, across every placed data
        // section (rdata incl. the c145 relro fold, data, bss). TLS
        // items never appear (the dll schema advertises no
        // tdata/tbss, so the pre-walker gate rejected them; a
        // thread-local's template offset is not an RVA anyway).
        std::unordered_map<std::uint32_t, std::uint32_t> dataRvaBySym;
        auto addDataRvas = [&](link::format::ExecDataSectionLayout const&
                                   layout,
                               std::uint32_t sectionRva) {
            for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
                auto const& item = module.dataItems[layout.itemIndices[j]];
                if (item.symbol == SymbolId{}) continue;  // anonymous
                dataRvaBySym.emplace(
                    item.symbol.v,
                    sectionRva
                        + static_cast<std::uint32_t>(layout.itemOffsets[j]));
            }
        };
        if (hasRdata) addDataRvas(rdataDataLayout, rdata->rva);
        if (hasData)  addDataRvas(dataDataLayout, data->rva);
        if (hasBss)   addDataRvas(bssDataLayout, bss->rva);
        std::uint32_t const textRva0 =
            static_cast<std::uint32_t>(secText.virtualAddress);
        std::unordered_set<std::string_view> seenExportNames;
        for (auto const& ms : module.symbols) {
            if (ms.name.empty()) continue;
            if (!isExternallyVisible(ms.binding, ms.visibility)) continue;
            if (!seenExportNames.insert(ms.name).second) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::format(
                         "pe::encodeExec (dll): duplicate export name "
                         "'{}' -- two externally-visible ModuleSymbol "
                         "rows share one name; the Name Pointer Table "
                         "is a binary-searched UNIQUE-key table, so a "
                         "duplicate silently shadows one definition "
                         "(D-LK2-4).",
                         ms.name));
                return {};
            }
            if (auto const fit = funcIdxBySym.find(ms.symbol.v);
                fit != funcIdxBySym.end()) {
                dllExports.push_back(PeExportRec{
                    ms.name,
                    textRva0 + static_cast<std::uint32_t>(
                                   funcTextStart[fit->second])});
            } else if (auto const dit = dataRvaBySym.find(ms.symbol.v);
                       dit != dataRvaBySym.end()) {
                dllExports.push_back(PeExportRec{ms.name, dit->second});
            } else {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::format(
                         "pe::encodeExec (dll): ModuleSymbol '{}' "
                         "(SymbolId #{}) names neither a defined "
                         "function nor a placed data item -- the "
                         "export table cannot place it (producer "
                         "contract: one ModuleSymbol row per DEFINED "
                         "function/global). D-LK2-4.",
                         ms.name, ms.symbol.v));
                return {};
            }
        }
        // The binary-search invariant: memcmp order (std::string's
        // char_traits compare is unsigned-byte lexicographic — the
        // exact GetProcAddress comparison).
        std::sort(dllExports.begin(), dllExports.end(),
                  [](PeExportRec const& a, PeExportRec const& b) {
                      return a.name < b.name;
                  });
    }
    bool const hasExports = !dllExports.empty();
    constexpr std::size_t kExportDirectorySize = 40;
    std::uint32_t const edataRva = hasExports ? dataChainRva : 0u;
    std::vector<std::uint8_t> edataBytes;
    std::optional<DataSectionLayout> edata;
    if (hasExports) {
        std::size_t const n = dllExports.size();
        std::size_t const eatOff  = kExportDirectorySize;
        std::size_t const nameOff = eatOff + 4u * n;
        std::size_t const ordOff  = nameOff + 4u * n;
        std::size_t const strOff  = ordOff + 2u * n;
        // Strings: DllName (empty — see the block comment) first,
        // then the export names in sorted order.
        std::vector<std::uint32_t> nameStrRvas(n, 0u);
        std::size_t strCursor = strOff;
        std::uint32_t const dllNameRva =
            edataRva + static_cast<std::uint32_t>(strCursor);
        strCursor += 1;  // the empty DllName's NUL
        for (std::size_t i = 0; i < n; ++i) {
            nameStrRvas[i] =
                edataRva + static_cast<std::uint32_t>(strCursor);
            strCursor += dllExports[i].name.size() + 1;
        }
        std::size_t const edataSize = strCursor;
        if (edataSize > std::numeric_limits<std::uint32_t>::max()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encodeExec (dll): synthesized .edata "
                             "size {} exceeds u32 -- PE32+ RVAs are "
                             "32-bit.",
                             edataSize));
            return {};
        }
        // The ordinal table is u16 by PE spec (GetProcAddress takes a WORD
        // ordinal; link.exe hard-errors at the 64K-export limit). Past
        // 0xFFFF the cast below would WRAP: the name at sorted position
        // 65536 would silently resolve to EAT[0] -- the wrong function,
        // no diagnostic (c152 review fold; belt, no live producer).
        if (n > 0xFFFFu) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("pe::encodeExec (dll): {} exports exceed the "
                             "PE 64K-export limit (the ordinal table is "
                             "u16; GetProcAddress takes a WORD ordinal).",
                             n));
            return {};
        }
        edataBytes.assign(edataSize, 0u);
        auto putU16 = [&](std::size_t off, std::uint16_t v) {
            edataBytes[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
            edataBytes[off + 1] =
                static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        };
        auto putU32 = [&](std::size_t off, std::uint32_t v) {
            for (int b = 0; b < 4; ++b)
                edataBytes[off + b] =
                    static_cast<std::uint8_t>((v >> (b * 8)) & 0xFFu);
        };
        // IMAGE_EXPORT_DIRECTORY. Characteristics / TimeDateStamp /
        // Major/MinorVersion stay 0 (deterministic — matches the file
        // header's TimeDateStamp=0 stance).
        putU32(12, dllNameRva);                              // Name
        putU32(16, 1u);                                      // Base (ordinal)
        putU32(20, static_cast<std::uint32_t>(n));           // NumberOfFunctions
        putU32(24, static_cast<std::uint32_t>(n));           // NumberOfNames
        putU32(28, edataRva + static_cast<std::uint32_t>(eatOff));
        putU32(32, edataRva + static_cast<std::uint32_t>(nameOff));
        putU32(36, edataRva + static_cast<std::uint32_t>(ordOff));
        for (std::size_t i = 0; i < n; ++i) {
            putU32(eatOff + 4u * i, dllExports[i].rva);      // EAT
            putU32(nameOff + 4u * i, nameStrRvas[i]);        // sorted names
            putU16(ordOff + 2u * i,
                   static_cast<std::uint16_t>(i));           // identity ordinal
            std::size_t const so = nameStrRvas[i] - edataRva;
            for (std::size_t c = 0; c < dllExports[i].name.size(); ++c) {
                edataBytes[so + c] =
                    static_cast<std::uint8_t>(dllExports[i].name[c]);
            }
        }
        DataSectionLayout layout;
        layout.rva = edataRva;
        layout.virtualSize = static_cast<std::uint32_t>(
            alignUp(edataSize, sectionAlignE));
        edata = layout;
        dataChainRva += layout.virtualSize;
    }

    // `.idata` follows the LAST section in the VA chain (`dataChainRva` was
    // advanced past .rdata/.data/.bss/.xdata/.pdata/[.edata] above), else
    // after `.text`.
    std::uint32_t const idataRva = hasImports ? dataChainRva : 0u;

    // Lay out .idata bytes (synthesized, NOT raw-data padded yet):
    //   [0]               ImageImportDescriptor[N+1]
    //   [...]             ILT/IAT (PE32+ uses identical layout at
    //                     file-image time — loader patches IAT)
    //   [hintNameStart]   HINT/NAME table
    //   [dllNamesStart]   DLL name strings
    //
    // Compute offsets first so we know each IAT slot's RVA before
    // building bytes (the symbol-VA map needs it for reloc apply).
    constexpr std::size_t kImportDescriptorSize = 20;
    constexpr std::size_t kThunkSize            = 8;   // PE32+
    std::size_t const numLibs        = libraryOrder.size();
    std::size_t const descriptorBlockSize =
        (numLibs + 1) * kImportDescriptorSize;
    // Pad to kThunkSize so u64 ILT/IAT slots stay naturally aligned.
    // (numLibs+1)*20 is 8-aligned only when (numLibs+1) is even —
    // breaks at numLibs≥2 (code-reviewer #1 convergence). The pad
    // bytes stay zero in the idata buffer; the data-directory size
    // for Import Table remains `descriptorBlockSize` (excludes
    // pad).
    std::size_t const thunkBlockStart =
        (descriptorBlockSize + kThunkSize - 1) & ~(kThunkSize - 1);
    // Per-library ILT and IAT counts include a u64 zero terminator.
    // The IAT-layout loop ALSO populates `externIatVaBySym` inline
    // (simplifier #1 fold: was a separate 3rd "iterate libs × externs"
    // pass with its own slotIdx counter; merged here so the slot
    // RVA math has a single source of truth).
    std::vector<std::size_t> iltOffsets(numLibs);
    std::vector<std::size_t> iatOffsets(numLibs);
    std::unordered_map<SymbolId, std::uint64_t> externIatVaBySym;
    externIatVaBySym.reserve(module.externImports.size());
    std::size_t thunkCursor = thunkBlockStart;
    for (std::size_t li = 0; li < numLibs; ++li) {
        iltOffsets[li] = thunkCursor;
        std::size_t const slots = externsByLib[libraryOrder[li]].size() + 1;
        thunkCursor += slots * kThunkSize;
    }
    for (std::size_t li = 0; li < numLibs; ++li) {
        iatOffsets[li] = thunkCursor;
        auto const& externs = externsByLib[libraryOrder[li]];
        for (std::size_t k = 0; k < externs.size(); ++k) {
            std::size_t const iatSlotOff =
                thunkCursor + k * kThunkSize;
            externIatVaBySym.emplace(
                module.externImports[externs[k]].symbol,
                oh.imageBase + idataRva + iatSlotOff);
        }
        thunkCursor += (externs.size() + 1) * kThunkSize;  // +1 terminator
    }

    // ── (c2) Fill the import-thunk block reserved in step (a2) ──
    // Each extern's IAT slot VA is now known (externIatVaBySym); write
    // one `FF 25 disp32` thunk per FUNCTION extern jumping through it,
    // and record the thunk VA (a .text code address) in
    // `externThunkVaBySym`. THAT map — not `externIatVaBySym` — feeds
    // `symbolVa` below, so every function-extern reference (a direct
    // call OR an address-taken value) resolves to the callable thunk
    // (direct-plt), never the raw IAT data slot. c149: the loop walks
    // the COMPACTED `funcExternIdxs` (thunk index j → extern index i) —
    // a DATA extern gets no thunk; its symbolVa binds to the IAT slot
    // VA below.
    std::unordered_map<SymbolId, std::uint64_t> externThunkVaBySym;
    externThunkVaBySym.reserve(numImportThunks);
    if (numImportThunks > 0) {
        // Defense: step (a2) must have grown .text to hold the whole
        // block; a future reorder that drops the reservation would
        // otherwise write out of bounds here.
        if (thunkBlockOffset + numImportThunks * peThunkSize
                > text.size()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 "pe::encodeExec: import-thunk block was not reserved — "
                 ".text was not grown to hold the thunks (step (a2) "
                 "invariant violated).");
            return {};
        }
        for (std::size_t j = 0; j < numImportThunks; ++j) {
            std::size_t const i = funcExternIdxs[j];
            std::size_t const thunkOff = thunkBlockOffset + j * peThunkSize;
            std::uint64_t const thunkVa =
                oh.imageBase + secText.virtualAddress + thunkOff;
            auto const iatIt =
                externIatVaBySym.find(module.externImports[i].symbol);
            if (iatIt == externIatVaBySym.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"pe::encodeExec: extern #"}
                         + std::to_string(i)
                         + " has no IAT slot VA — externIatVaBySym is "
                           "incomplete (import layout bug).");
                return {};
            }
            if (!emitPeThunk(id.machine, text, thunkOff, thunkVa,
                             iatIt->second, i, reporter)) {
                return {};
            }
            externThunkVaBySym.emplace(module.externImports[i].symbol,
                                       thunkVa);
        }
    }

    // c116 (D-WIN64-SEH-FUNCLETS): back-patch each SEH function's UNWIND_INFO
    // handler-RVA field now that the __C_specific_handler thunk VA is resolved. The
    // handler field is an IMAGE-RVA (the OS calls the personality imageBase-relative
    // — the c112 address-taken-import precedent), so RVA = thunkVA - imageBase. The
    // thunk (FF 25 jmp *[IAT]) is the callable stub, NOT the raw IAT data slot.
    for (auto const& patch : sehHandlerPatches) {
        auto it = externThunkVaBySym.find(patch.symbol);
        if (it == externThunkVaBySym.end()) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: SEH personality symbol #"}
                     + std::to_string(patch.symbol.v)
                     + " (__C_specific_handler) has no import thunk — the SEH pass "
                       "must synthesize its ExternImport (D-WIN64-SEH-FUNCLETS).");
            return {};
        }
        std::uint32_t const handlerRva =
            static_cast<std::uint32_t>(it->second - oh.imageBase);
        if (patch.xdataOffset + 4u > xdataBytes.size()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 "pe::encodeExec: SEH handler-field patch offset out of range "
                 "(D-WIN64-SEH-FUNCLETS).");
            return {};
        }
        for (int b = 0; b < 4; ++b) {
            xdataBytes[patch.xdataOffset + b] =
                static_cast<std::uint8_t>((handlerRva >> (b * 8)) & 0xFFu);
        }
    }

    // HINT/NAME table starts here. Per extern: 2 bytes hint + name +
    // NUL + optional padding to even.
    std::size_t const hintNameStart = thunkCursor;
    std::vector<std::uint32_t> hintNameRvaBySym;  // per externImport index
    hintNameRvaBySym.resize(module.externImports.size(), 0u);
    std::size_t hintCursor = hintNameStart;
    for (std::size_t li = 0; li < numLibs; ++li) {
        for (auto extIdx : externsByLib[libraryOrder[li]]) {
            hintNameRvaBySym[extIdx] =
                idataRva + static_cast<std::uint32_t>(hintCursor);
            std::size_t const nameBytes =
                module.externImports[extIdx].mangledName.size() + 1; // +NUL
            hintCursor += 2 + nameBytes;                  // 2 = hint
            if ((hintCursor - hintNameStart) & 1u) ++hintCursor;
        }
    }
    std::size_t const dllNameStart = hintCursor;
    std::vector<std::uint32_t> dllNameRvaByLib(numLibs, 0u);
    for (std::size_t li = 0; li < numLibs; ++li) {
        dllNameRvaByLib[li] =
            idataRva + static_cast<std::uint32_t>(hintCursor);
        hintCursor += libraryOrder[li].size() + 1; // +NUL
    }
    std::size_t const idataSize = hintCursor;
    // u32 narrowing guard (silent-failure H2 post-audit fold):
    // every downstream `.idata` cursor (descriptor RVA, ILT/IAT
    // thunk offsets, HINT/NAME entry RVAs) is `static_cast<u32>(...)`
    // from a `size_t` path. PE32+ RVAs are 32-bit per spec; an
    // out-of-range idataSize would silently truncate at emit.
    if (idataSize > std::numeric_limits<std::uint32_t>::max()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             std::string{"pe::encodeExec: synthesized .idata size "}
                 + std::to_string(idataSize)
                 + " exceeds u32 — PE32+ RVAs are 32-bit. The "
                   "externImports list is pathologically large or "
                   "carries names that overflow the section RVA "
                   "space.");
        return {};
    }

    // ── (d) Apply intra-module + extern relocations in-place ──
    //
    // Delegated to the shared `applyExecRelocations` kernel
    // (`link/format/exec_reloc_apply.hpp`). PE-specific input:
    // patchSectionVa = ImageBase + secText.virtualAddress (RVA).
    // The symbolVa map merges intra-module function VAs (.text +
    // funcOffset) with extern import VAs (.idata + iatSlotOffset),
    // so REL32 calls to either kind work uniformly. Format-side
    // fmt.relocationByKind(rel.kind) check stays here because its
    // diagnostic wording cites the PE format name.
    // D-LK-PE-IMAGE-TEXT-ABS-RELOC (c152, the .reloc-completeness
    // belt — registered with the D-LK2-4 dll cycle, and live on the
    // exec too since its condition is schema-read): on a
    // DYNAMIC_BASE image the loader may (dll: routinely does) rebase
    // the whole image, adjusting every absolute slot listed in
    // `.reloc`. The walker collects DIR64 sites from DATA-ITEM
    // fixups + the TLS directory VAs — but NOT from `.text` (the
    // `applyExecRelocations` kernel patches in place without
    // reporting sites). An ABSOLUTE (non-pc-relative, non-tls)
    // function relocation would therefore bake a preferred-base VA
    // into code bytes that no `.reloc` row ever adjusts — a silent
    // wrong-address under rebase. DSS x86_64 codegen emits none
    // (globals are rip-relative lea, calls rel32, jump tables are
    // data items, import thunks rip-relative FF 25), so this is a
    // fail-loud belt, not a live path; the closure direction is to
    // collect text sites into `.reloc` when a producer appears.
    // tls-flagged kinds are exempt: a SECREL32 patch is a
    // section-relative template OFFSET (rebase-invariant), not a VA.
    constexpr std::uint16_t kDllCharDynamicBase = 0x0040;
    // c152 review fold: DYNAMIC_BASE opts into ASLR, but a DLL is ALSO
    // rebased on classic preferred-base COLLISION regardless of the flag
    // (whenever relocations are not stripped). A dll schema without
    // DYNAMIC_BASE would otherwise go dark on this belt and bake a
    // preferred-base VA a collision-rebase never adjusts -- the exact
    // silent wrong-address the belt exists to stop. Shipped schemas set
    // the flag; this covers hand-rolled ones.
    bool const imageIsRelocatableAtLoad =
        isDll || (oh.dllCharacteristics & kDllCharDynamicBase) != 0u;
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            if (fmt.relocationByKind(rel.kind) == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("pe::encodeExec: kind {} not declared "
                                 "by object format '{}' — substrate-"
                                 "invariant violation.",
                                 rel.kind.v, fmt.name()));
                return {};
            }
            auto const* tri = targetSchema.relocationInfo(rel.kind);
            if (imageIsRelocatableAtLoad && tri != nullptr
             && !tri->pcRelative && !tri->tls) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format(
                         "pe::encodeExec: function SymbolId={{ {} }} "
                         "carries an ABSOLUTE relocation ('{}', kind "
                         "{}) into .text, but this image sets "
                         "DYNAMIC_BASE and the walker collects "
                         "IMAGE_REL_BASED_DIR64 sites only from data "
                         "items -- the patched code bytes would keep "
                         "the preferred-base address after the loader "
                         "rebases the image (silent wrong address; on "
                         "a .dll the loader rebases routinely). Emit "
                         "the address as a data item, or extend the "
                         ".reloc collector to .text sites "
                         "(D-LK-PE-IMAGE-TEXT-ABS-RELOC).",
                         fn.symbol.v, tri->name, rel.kind.v));
                return {};
            }
        }
    }
    std::uint64_t const sectionVa = oh.imageBase + secText.virtualAddress;
    std::unordered_map<SymbolId, std::uint64_t> symbolVa;
    symbolVa.reserve(module.functions.size()
                     + module.externImports.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        if (!symbolVa.emplace(module.functions[i].symbol,
                              sectionVa + funcTextStart[i]).second) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: duplicate function "
                             "symbol #"}
                 + std::to_string(module.functions[i].symbol.v)
                 + " — caller must give each function a unique "
                   "SymbolId.");
            return {};
        }
    }
    for (auto const& [sym, va] : externThunkVaBySym) {
        // D-FFI-PE-IMPORT-THUNK: a FUNCTION extern's symbolVa names its
        // import THUNK (a .text code address) — the PE analog of an ELF
        // PLT stub / Mach-O __stubs entry — NOT the `.idata` IAT data
        // slot. This makes an address-taken import a CALLABLE code
        // address and a direct extern call a plain `call rel32 → thunk`
        // (the thunk does the IAT indirection); PE is no longer the
        // asymmetric outlier. An extern SymbolId colliding with a
        // function's SymbolId is a caller bug; silently overriding the
        // in-text VA would patch in-text rel32 to the wrong target.
        if (!symbolVa.emplace(sym, va).second) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: extern SymbolId #"}
                 + std::to_string(sym.v)
                 + " collides with another symbol — caller must "
                   "give each extern a unique SymbolId distinct "
                   "from function ids.");
            return {};
        }
    }
    // c149 (D-LK-EXTERN-DATA-IMPORT, the PE half): a DATA extern's
    // symbolVa is its `.idata` IAT SLOT VA — the address of the pointer
    // the loader fills with the imported object's address (`__imp_`
    // semantics; the Mach-O __got non-lazy-pointer model, macho.cpp's
    // c117 mirror). A GlobalAddr of the symbol (mir_to_lir got-indirect)
    // lea's this image-local slot VA then derefs it — NEVER a thunk VA
    // (a data object is not callable; reading `FF 25` jump-stub bytes as
    // the object's value is the silent-miscompile class the gate
    // guards). Bound HERE — after the IAT layout fixed each slot's VA
    // but BEFORE `applyExecRelocations` — so a code reloc into the data
    // symbol resolves to the slot VA through the SAME relocation kernel
    // a function thunk / named data item uses.
    for (auto const& ext : module.externImports) {
        if (!ext.isData) continue;
        auto const iatIt = externIatVaBySym.find(ext.symbol);
        if (iatIt == externIatVaBySym.end()) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: extern DATA SymbolId #"}
                     + std::to_string(ext.symbol.v)
                     + " has no IAT slot VA -- externIatVaBySym is "
                       "incomplete (import layout bug).");
            return {};
        }
        if (!symbolVa.emplace(ext.symbol, iatIt->second).second) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: extern DATA SymbolId #"}
                     + std::to_string(ext.symbol.v)
                     + " collides with another symbol -- caller must "
                       "give each extern a unique SymbolId distinct "
                       "from function ids.");
            return {};
        }
    }
    // D-LK4-RODATA-WALKER-RELOC-BASE-OFFSET + D-LK4-DATA-PRODUCER: each NAMED
    // data item (rdata / data / bss) joins the symbolVa map at its section's
    // absolute VA (`imageBase + section RVA` + the item's section-relative
    // offset) via the SAME shared `addDataSymbolVas` helper the ELF/Mach-O
    // writers use. A REL32/ABS64 reloc in `.text` that targets a data SymbolId
    // (a global load/store) resolves through the shared `applyExecRelocations`
    // kernel. A `.bss` global is reloc-addressable by VA just like file-backed.
    if (hasRdata
        && !link::format::addDataSymbolVas(
               module.dataItems, rdataDataLayout, oh.imageBase + rdata->rva,
               symbolVa, "pe::encodeExec", reporter)) {
        return {};
    }
    if (hasData
        && !link::format::addDataSymbolVas(
               module.dataItems, dataDataLayout, oh.imageBase + data->rva,
               symbolVa, "pe::encodeExec", reporter)) {
        return {};
    }
    if (hasBss
        && !link::format::addDataSymbolVas(
               module.dataItems, bssDataLayout, oh.imageBase + bss->rva,
               symbolVa, "pe::encodeExec", reporter)) {
        return {};
    }
    // ── D-CSUBSET-THREAD-LOCAL (TLS C3): register each thread-local item's
    // POSITIVE section-relative TEMPLATE OFFSET (the `.tls` model — NOT the
    // ELF negative Variant-II tpoff; mechanism 6) into `symbolVa`, and mint
    // the `_tls_index` singleton the access sequence reads. The tdata call
    // uses blockBase 0 (the template opens the block); the tbss call uses
    // `tbssBlockBase` so both index ONE contiguous per-thread block.
    std::unordered_set<SymbolId> tlsSymbols;
    if (hasTdata
        && !link::format::addTlsTemplateOffsets(
               module.dataItems, tdataLayout, /*blockBaseOffset=*/0,
               symbolVa, tlsSymbols, "pe::encodeExec", reporter)) {
        return {};
    }
    if (hasTbss
        && !link::format::addTlsTemplateOffsets(
               module.dataItems, tbssLayout, tbssBlockBase,
               symbolVa, tlsSymbols, "pe::encodeExec", reporter)) {
        return {};
    }
    // The writer-minted `_tls_index` singleton: a RESERVED well-known
    // SymbolId (a high sentinel dense ids never reach) the pe-indexed
    // lowering's riprel read targets. Bind it to the 4-byte slot's VA (the
    // slot lives at the tail of `.tls`). `.emplace` FAILS if a real symbol
    // already holds the reserved id — the collision-safety assertion the
    // sentinel design requires (a dense id equal to it would be a
    // catastrophic overlap; the sentinel is chosen so this can never fire).
    if (hasTls) {
        SymbolId const reservedIdxId{kTlsIndexReservedSymbolIdValue};
        std::uint64_t const idxSlotVa =
            oh.imageBase + tlsRva + tlsIndexOffset;
        if (!symbolVa.emplace(reservedIdxId, idxSlotVa).second) {
            emit(reporter, DiagnosticCode::K_SymbolUndefined,
                 std::string{"pe::encodeExec: the reserved _tls_index "
                             "SymbolId #"}
                 + std::to_string(kTlsIndexReservedSymbolIdValue)
                 + " collides with a real module symbol — the high "
                   "sentinel invariant (dense ids never reach it) was "
                   "breached (D-CSUBSET-THREAD-LOCAL).");
            return {};
        }
    }
    // ── ★ D-CSUBSET-THREAD-LOCAL backstop (audit CRIT-2): `symbolVa` is now
    // secrel-poisoned for `tlsSymbols` members (their entries are template
    // OFFSETS, not VAs). Police EVERY reloc BEFORE any patch — the exact
    // replica of the ELF walker's CRIT-1 backstop (the same silent-garbage
    // class exists identically on PE). (a) NO data-item reloc may target a
    // TLS symbol (a thread-local object has no link-time address — C11
    // 6.6p9; the semantic tier already rejects `&tls` as 0xE048). (b) A
    // function reloc's `tls` flag must AGREE with its target's storage class
    // BOTH ways (a non-tls kind against a TLS symbol embeds the offset as an
    // address; a tls kind against a non-TLS symbol embeds a VA as an offset).
    // The `.tls` template + directory span VAs written DIRECTLY (below) are
    // NOT tls-symbol relocs, so this backstop never false-rejects them.
    if (!tlsSymbols.empty()) {
        for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
            for (auto const& rel : module.dataItems[i].relocations) {
                if (tlsSymbols.contains(rel.target)) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format(
                             "pe::encodeExec: data item #{} (SymbolId={{ {} "
                             "}}) carries a relocation targeting THREAD-LOCAL "
                             "symbol #{} — the address of a thread-local "
                             "object is not a link-time constant (C11 6.6p9; "
                             "the semantic tier rejects this as 0xE048 "
                             "S_ThreadLocalAddressNotConstant). Patching it "
                             "would embed the section-relative template "
                             "offset as a garbage pointer "
                             "(D-CSUBSET-THREAD-LOCAL, CRIT-2).",
                             i, module.dataItems[i].symbol.v, rel.target.v));
                    return {};
                }
            }
        }
    }
    for (auto const& fn : module.functions) {
        for (auto const& rel : fn.relocations) {
            auto const* tri = targetSchema.relocationInfo(rel.kind);
            bool const relocIsTls  = tri != nullptr && tri->tls;
            bool const targetIsTls = tlsSymbols.contains(rel.target);
            if (relocIsTls != targetIsTls) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format(
                         "pe::encodeExec: function SymbolId={{ {} }} "
                         "relocation (kind {}{}) {} symbol #{} — a "
                         "tls-flagged relocation carries a section-relative "
                         "template OFFSET and a non-tls one an ADDRESS; "
                         "mixing them embeds the wrong value class silently "
                         "(D-CSUBSET-THREAD-LOCAL, CRIT-2).",
                         fn.symbol.v, rel.kind.v,
                         tri != nullptr ? std::format(" '{}'", tri->name)
                                        : std::string{},
                         targetIsTls
                             ? "is not tls-flagged but targets THREAD-LOCAL"
                             : "is tls-flagged but targets NON-thread-local",
                         rel.target.v));
                return {};
            }
        }
    }
    // D-CSUBSET-COMPUTED-GOTO: synthetic per-block symbols get their
    // interior-block VAs before relocation resolution — sectionVa is the
    // SAME base (`imageBase + secText.virtualAddress`) used for the
    // function symbols above, so block VA = funcVA + blockOffset. The
    // shared helper is identical across ELF/PE/Mach-O.
    if (!link::format::addInteriorBlockSymbolVas(
            module, funcTextStart, sectionVa, symbolVa,
            "pe::encodeExec", reporter)) {
        return {};
    }
    if (!link::format::applyExecRelocations(
            text, module, funcTextStart, symbolVa, targetSchema,
            sectionVa, "pe::encodeExec", reporter)) {
        return {};
    }

    // ── Apply DATA-ITEM relocations into the .rdata bytes ─────────
    //
    // `applyExecRelocations` above patches FUNCTION relocations into `.text`. A data item
    // may ALSO carry relocations — an F5 symbol-address pointer, a folded relro const
    // table slot, or the LK11b cross-CU thunk slot (an 8-byte RelRoConst pointer the
    // linker mints for an INDIRECT-SLOT-dispatch format, whose single absolute-64-bit
    // reloc targets a sibling-CU definition — the indirect cross-CU call `call qword ptr
    // [slot]` dereferences a slot holding the def's runtime address; c154: a direct-plt
    // format binds the reference straight to the def and mints no slot). The target's VA
    // already lives in `symbolVa` (built
    // above for functions + externs/IAT + rodata items). Patch each data-item relocation
    // directly into `rdataBytes` at the item's section-relative offset + the reloc's
    // intra-item offset, writing the absolute VA `widthBytes` LE. This runs against the
    // SAME `rdataBytes` buffer that is emitted into the image below — patch BEFORE emit.
    //
    // Only ABSOLUTE relocations are meaningful for a data pointer; a PC-relative kind here
    // would be a producer bug (data items have no instruction-pointer base). Fail loud on
    // (a) an unresolved target, (b) a pc-relative kind, (c) an unknown kind, (d) a write
    // that overruns the item — never emit a half-patched slot.
    //
    // Each absolute fixup ALSO needs a PE base relocation (.reloc) so the loader rebases it
    // when ASLR slides the image. Collect every absolute-64 fixup SITE RVA here for the
    // `.reloc` block builder below. (`rdata->rva` is the .rdata section base RVA; the site
    // is at that base + the item's section offset + the reloc's intra-item offset.)
    std::vector<std::uint32_t> baseRelocSiteRvas;
    for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
        auto const& di = module.dataItems[i];
        if (di.relocations.empty()) continue;
        // The reloc-bearing item lives in `.rdata` (folded relro const tables /
        // cross-CU thunk slots / F5 string-rodata pointers) or `.data` (F5 MUTABLE
        // symbol-address global pointers `char* g="..."` / `int* p=&x`). Resolve its
        // section-relative offset + the buffer to patch + the section base RVA. A
        // `.bss` / unplaced reloc-bearing item has no on-disk patch site → fail loud.
        std::vector<std::uint8_t>* patchBuf = nullptr;
        std::size_t                itemBaseOff = 0;
        std::uint32_t              itemSecRva  = 0;
        if (auto it = rdataOffsetByIndex.find(i); it != rdataOffsetByIndex.end()) {
            patchBuf    = &rdataBytes;
            itemBaseOff = static_cast<std::size_t>(it->second);
            itemSecRva  = rdata->rva;
        } else if (auto it = dataOffsetByIndex.find(i);
                   it != dataOffsetByIndex.end()) {
            patchBuf    = &dataBytes;
            itemBaseOff = static_cast<std::size_t>(it->second);
            itemSecRva  = data->rva;
        } else if (auto it = tlsOffsetByIndex.find(i);
                   it != tlsOffsetByIndex.end()) {
            // TLS C3 (D-CSUBSET-THREAD-LOCAL, seam-ii): a reloc-bearing `.tls`
            // TEMPLATE item (`thread_local char *msg = "hi";`) — the C1
            // shared-asm CRIT-2 fix keeps it in Tdata, so it rides THIS loop:
            // its pointer bytes are patched to the target's VA (+ given a
            // DIR64 base-reloc site below) so each per-thread copy holds the
            // rebased pointer. The reloc targets a NON-tls symbol (the rodata
            // string), so the backstop above did not reject it.
            patchBuf    = &tlsBytes;
            itemBaseOff = static_cast<std::size_t>(it->second);
            itemSecRva  = tlsRva;
        } else {
            emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                 std::string{"pe::encodeExec: data-item SymbolId #"}
                 + std::to_string(di.symbol.v)
                 + " carries relocations but is not a .rdata / .data / .tls item "
                   "(a .bss / zero-init item has no on-disk patch site).");
            return {};
        }
        for (auto const& rel : di.relocations) {
            // ── D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR (c152, the PE half of
            // the c150 CRITICAL): a data slot whose relocation targets
            // an extern DATA import would bake the image-local IAT
            // SLOT address — `symbolVa[dataExtern]` is the
            // loader-filled `.idata` pointer cell, NOT the imported
            // object — into the pointer bytes: one indirection off at
            // runtime (`FILE **pp = &stdout;` would hold &IAT_slot;
            // `*pp` reads the slot's content — the object's ADDRESS —
            // where C semantics require the object's VALUE). On a
            // relocatable-at-load artifact the slot also gets a DIR64
            // row, faithfully rebasing the wrong value. MSVC itself
            // REJECTS `&dllimport_obj` as a C static initializer
            // (C2099 "initializer is not a constant"), so the honest
            // arm is FAIL LOUD naming the extern. (The symbol-based
            // fix — the ELF dyn arm's R_X86_64_64-against-dynsym
            // shape — has no PE image analog short of a CRT-style
            // runtime pseudo-reloc scheme, deliberately NOT
            // attempted.)
            //
            // A FUNCTION extern is the OTHER half of the registered
            // anchor and stays LEGAL by design: its symbolVa is the
            // FF 25 import THUNK — a CALLABLE code address — so the
            // baked pointer is call-correct (the c112 shipped,
            // run-proven `addr_import` witness: `static putfn t[] =
            // {puts};` — sqlite's aSyscall[] shape) and its DIR64 row
            // rebases it correctly. Cross-image pointer IDENTITY for
            // an imported function is not guaranteed on Windows
            // (MSVC's own `&puts` binds each module's local thunk —
            // the platform norm), exactly as the anchor row records.
            if (auto const extIt = externIatVaBySym.find(rel.target);
                extIt != externIatVaBySym.end()) {
                ExternImport const* ext = nullptr;
                for (auto const& e : module.externImports) {
                    if (e.symbol == rel.target) { ext = &e; break; }
                }
                if (ext != nullptr && ext->isData) {
                    emit(reporter,
                         DiagnosticCode::K_RelocationKindMismatch,
                         std::format(
                             "pe::encodeExec: data-item SymbolId #{} "
                             "carries a relocation targeting extern "
                             "DATA import '{}' (symbol #{}) -- taking "
                             "an imported OBJECT's address in a static "
                             "initializer would bake the image-local "
                             "IAT-slot address into the data slot (one "
                             "indirection off at runtime; MSVC rejects "
                             "the same shape as a non-constant C "
                             "initializer, C2099). Initialize the "
                             "pointer at runtime instead "
                             "(D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR).",
                             di.symbol.v, ext->mangledName,
                             rel.target.v));
                    return {};
                }
            }
            auto const sIt = symbolVa.find(rel.target);
            if (sIt == symbolVa.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::string{"pe::encodeExec: data-item SymbolId #"}
                     + std::to_string(di.symbol.v)
                     + " has a relocation targeting symbol #"
                     + std::to_string(rel.target.v)
                     + " that is not defined by any function / extern / data item — a "
                       "cross-CU thunk slot's definition must be present in the merged image.");
                return {};
            }
            auto const* tri = targetSchema.relocationInfo(rel.kind);
            if (tri == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"pe::encodeExec: data-item SymbolId #"}
                     + std::to_string(di.symbol.v) + " relocation kind "
                     + std::to_string(rel.kind.v)
                     + " has no TargetRelocationInfo on the target schema.");
                return {};
            }
            if (tri->pcRelative || tri->widthBytes == 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"pe::encodeExec: data-item SymbolId #"}
                     + std::to_string(di.symbol.v) + " relocation '" + tri->name
                     + "' is pc-relative or has widthBytes=0 — a data pointer requires an "
                       "absolute fixup with a concrete write width.");
                return {};
            }
            std::size_t const itemBase = itemBaseOff;
            std::size_t const patchOff = itemBase + rel.offset;
            if (patchOff + tri->widthBytes > itemBase + di.bytes.size()) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"pe::encodeExec: data-item SymbolId #"}
                     + std::to_string(di.symbol.v) + " relocation offset "
                     + std::to_string(rel.offset) + " + widthBytes "
                     + std::to_string(static_cast<int>(tri->widthBytes))
                     + " overruns the item's " + std::to_string(di.bytes.size())
                     + " bytes.");
                return {};
            }
            std::uint64_t const value =
                static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(sIt->second) + rel.addend);
            for (std::uint8_t b = 0; b < tri->widthBytes; ++b) {
                (*patchBuf)[patchOff + b] =
                    static_cast<std::uint8_t>((value >> (8u * b)) & 0xFFu);
            }
            // Record the base-relocation site. Only an 8-byte absolute fixup maps to
            // IMAGE_REL_BASED_DIR64; a different-width absolute data reloc has no DIR64
            // representation (HIGHLOW/etc. are anchored for when a 32-bit-abs data
            // producer lands) — fail loud rather than ship an un-rebased fixup that
            // ASLR would leave pointing at the preferred-base address.
            if (tri->widthBytes != 8) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::string{"pe::encodeExec: data-item SymbolId #"}
                     + std::to_string(di.symbol.v) + " absolute relocation '" + tri->name
                     + "' has widthBytes=" + std::to_string(static_cast<int>(tri->widthBytes))
                     + " — only an 8-byte absolute fixup (IMAGE_REL_BASED_DIR64) is "
                       "supported in the .reloc base-relocation table today.");
                return {};
            }
            baseRelocSiteRvas.push_back(
                itemSecRva + static_cast<std::uint32_t>(patchOff));
        }
    }

    // ── D-CSUBSET-THREAD-LOCAL (TLS C3): patch the IMAGE_TLS_DIRECTORY64 (40
    // bytes) at the tail of `.tls` + collect its base-reloc sites. The
    // directory lives at [tlsDirOffset, +40) inside `tlsBytes`; the
    // `_tls_index` slot at [tlsIndexOffset, +4) stays zero (the loader fills
    // it). MUST run BEFORE the `.reloc` builder below consumes
    // `baseRelocSiteRvas` (audit M-2).
    //
    // ★ Seam-(i): the 3 VA fields (Start/End/AddressOfIndex) are SECTION-SPAN
    // / slot VAs written DIRECTLY here (imageBase + tlsRva + offset), pushed
    // straight to `baseRelocSiteRvas` — they are NOT tls-symbol relocations,
    // so the CRIT-2 backstop never false-rejects them. AddressOfCallBacks
    // stays 0 and gets NO base-reloc entry (EXACTLY 3 DIR64 entries): a reloc
    // on 0 → the loader adds the slide → a non-null garbage callback pointer.
    if (hasTls) {
        auto putTlsU64 = [&](std::size_t off, std::uint64_t v) {
            for (int b = 0; b < 8; ++b)
                tlsBytes[off + b] =
                    static_cast<std::uint8_t>((v >> (b * 8)) & 0xFFu);
        };
        auto putTlsU32 = [&](std::size_t off, std::uint32_t v) {
            for (int b = 0; b < 4; ++b)
                tlsBytes[off + b] =
                    static_cast<std::uint8_t>((v >> (b * 8)) & 0xFFu);
        };
        std::size_t const dirOff = static_cast<std::size_t>(tlsDirOffset);
        std::uint64_t const startVa = oh.imageBase + tlsRva + 0u;
        // End brackets the FULL per-thread block (tdata + embedded tbss zeros)
        // — the loader copies [Start, End) into every thread; SizeOfZeroFill=0
        // (the tbss zeros are IN the template, see the layout note above).
        std::uint64_t const endVa   =
            oh.imageBase + tlsRva + static_cast<std::uint32_t>(tlsBlockMemsz);
        std::uint64_t const idxVa   =
            oh.imageBase + tlsRva + static_cast<std::uint32_t>(tlsIndexOffset);
        putTlsU64(dirOff +  0, startVa);                   // StartAddressOfRawData
        putTlsU64(dirOff +  8, endVa);                     // EndAddressOfRawData
        putTlsU64(dirOff + 16, idxVa);                     // AddressOfIndex
        putTlsU64(dirOff + 24, 0);                         // AddressOfCallBacks = 0
        putTlsU32(dirOff + 32, 0);                         // SizeOfZeroFill = 0
        putTlsU32(dirOff + 36, 0);                         // Characteristics = 0
        // The 3 VA-field SITE RVAs (Start/End/AddressOfIndex) — NOT callbacks.
        std::uint32_t const dirRvaBase =
            tlsRva + static_cast<std::uint32_t>(tlsDirOffset);
        baseRelocSiteRvas.push_back(dirRvaBase +  0u);
        baseRelocSiteRvas.push_back(dirRvaBase +  8u);
        baseRelocSiteRvas.push_back(dirRvaBase + 16u);
    }

    // ── Build the .reloc (base relocation) section bytes ──────────
    //
    // The image keeps ASLR (dllCharacteristics carries DYNAMIC_BASE), so every absolute
    // 64-bit fixup the linker wrote into the image (the cross-CU thunk slots) needs an
    // IMAGE_BASE_RELOCATION entry telling the loader to add the load-bias to that 8-byte
    // word. Group the fixup site RVAs by 4 KiB page; each page becomes a block:
    //   { u32 PageRVA (4 KiB-aligned), u32 BlockSize } then u16 entries
    //   (IMAGE_REL_BASED_DIR64(=10) << 12) | (siteRVA & 0xFFF).
    // Pad each block's entry list to a 4-byte multiple (one u16 zero pad when the entry
    // count is odd — a 0-entry pad is IMAGE_REL_BASED_ABSOLUTE, a no-op the loader skips);
    // BlockSize = 8 (header) + 2 * paddedEntryCount.
    std::vector<std::uint8_t> relocBytes;
    if (!baseRelocSiteRvas.empty()) {
        std::sort(baseRelocSiteRvas.begin(), baseRelocSiteRvas.end());
        // A duplicate DIR64 site would be applied TWICE at load: unlike ELF
        // RELA (set semantics, idempotent), a base relocation ADDS the load
        // delta to the slot -- two rows at one RVA double-add it, and the
        // pointer is wrong ONLY when the loader actually rebases (works at
        // the preferred base, corrupt under ASLR -- the hardest-to-debug
        // variant). One 8-byte slot has exactly one fixup, so adjacent
        // equality after the sort can never false-positive (c152 review
        // fold; belt, no live producer emits duplicates).
        for (std::size_t k = 1; k < baseRelocSiteRvas.size(); ++k) {
            if (baseRelocSiteRvas[k] == baseRelocSiteRvas[k - 1]) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("pe::encodeExec: duplicate base-relocation "
                                 "site RVA {:#x} -- a DIR64 fixup is "
                                 "delta-ADD, so a duplicate would be "
                                 "applied twice under rebase (silently "
                                 "corrupt pointer).",
                                 baseRelocSiteRvas[k]));
                return {};
            }
        }
        constexpr std::uint32_t kPageSize          = 0x1000u;
        constexpr std::uint16_t kImageRelBasedDir64 = 10u;
        std::size_t cursor = 0;
        while (cursor < baseRelocSiteRvas.size()) {
            std::uint32_t const pageBase = baseRelocSiteRvas[cursor] & ~(kPageSize - 1u);
            std::size_t blockEnd = cursor;
            while (blockEnd < baseRelocSiteRvas.size()
                   && (baseRelocSiteRvas[blockEnd] & ~(kPageSize - 1u)) == pageBase) {
                ++blockEnd;
            }
            std::size_t const entryCount = blockEnd - cursor;
            // Pad entry list to a 4-byte boundary (each entry is 2 bytes): a trailing
            // ABSOLUTE(0) no-op entry when entryCount is odd.
            std::size_t const paddedEntries = entryCount + (entryCount & 1u);
            std::uint32_t const blockSize =
                static_cast<std::uint32_t>(8u + 2u * paddedEntries);
            appendU32LE(relocBytes, pageBase);
            appendU32LE(relocBytes, blockSize);
            for (std::size_t k = cursor; k < blockEnd; ++k) {
                std::uint16_t const entry = static_cast<std::uint16_t>(
                    (kImageRelBasedDir64 << 12)
                    | (baseRelocSiteRvas[k] & 0x0FFFu));
                appendU16LE(relocBytes, entry);
            }
            if (entryCount & 1u) appendU16LE(relocBytes, 0u);  // ABSOLUTE no-op pad
            cursor = blockEnd;
        }
    }
    bool const hasBaseRelocs = !relocBytes.empty();

    // ── (d) Layout constants ──────────────────────────────────
    //
    // Header layout for PE32+ (spec §3.2-3.4):
    //   [0x00]  IMAGE_DOS_HEADER     (64 B)
    //   [0x40]  MS-DOS stub program  (64 B — fixed)
    //   [0x80]  PE signature "PE\0\0" (4 B)
    //   [0x84]  IMAGE_FILE_HEADER     (20 B)
    //   [0x98]  IMAGE_OPTIONAL_HEADER (240 B = 112 fixed + 16×8 data dirs)
    //   [0x188] IMAGE_SECTION_HEADER  (40 B × N)
    //
    // After all headers, pad to fileAlignment (typical 0x200) and
    // emit section raw data. Section virtualAddress is the RVA;
    // sizeOfRawData is the file-aligned byte length of `.text`.
    constexpr std::size_t kDosHeaderSize          = 64;
    constexpr std::size_t kDosStubSize            = 64;
    constexpr std::size_t kPeSigSize              = 4;
    constexpr std::size_t kFileHeaderSize         = 20;
    constexpr std::size_t kOptionalHeader64Fixed  = 112;
    constexpr std::size_t kNumberOfRvaAndSizes    = 16;
    constexpr std::size_t kDataDirectoryEntrySize = 8;
    constexpr std::size_t kOptionalHeader64Size   =
        kOptionalHeader64Fixed
        + kNumberOfRvaAndSizes * kDataDirectoryEntrySize;
    constexpr std::size_t kSectionHeaderSize = 40;

    // Build the section-header vector NOW so NumberOfSections,
    // headerBytesUnpadded, sizeOfImage, etc. ALL derive from
    // `sectionHeaders.size()` — same B-LK1-2 / D-LK2-5 discipline
    // the .obj arm + ELF walker adopted (architect O3 + code-
    // reviewer #5 convergence). A future cycle adding .rdata /
    // .data simply pushes onto this vector; counts update.
    std::uint32_t const fileAlign    = oh.fileAlignment;
    std::uint32_t const sectionAlign = oh.sectionAlignment;

    std::vector<PeSectionHeader> sectionHeaders;
    sectionHeaders.reserve(1u + (rdata.has_value() ? 1u : 0u)
                              + (hasImports ? 1u : 0u));
    std::size_t const headerBytesUnpaddedInitial =
        kDosHeaderSize + kDosStubSize + kPeSigSize
        + kFileHeaderSize + kOptionalHeader64Size;

    // .text section header (always present).
    {
        PeSectionHeader hText{};
        hText.name                  = encodeSectionName(secText.name, 0);
        hText.virtualSize           = static_cast<std::uint32_t>(text.size());
        hText.virtualAddress        = static_cast<std::uint32_t>(secText.virtualAddress);
        // sizeOfRawData / pointerToRawData filled below.
        hText.pointerToRelocations  = 0;
        hText.pointerToLinenumbers  = 0;
        hText.numberOfRelocations   = 0;
        hText.numberOfLinenumbers   = 0;
        hText.characteristics       = secText.type;
        sectionHeaders.push_back(hText);
    }
    std::size_t const textHdrIdx = 0;
    // .rdata section header (when AssembledData has Rodata items).
    // Characteristics from the format schema's Rodata row
    // (0x40000040 = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_
    // READ). sizeOfRawData / pointerToRawData filled below.
    if (rdata.has_value()) {
        PeSectionHeader hRData{};
        hRData.name                  = encodeSectionName(
                                          secRodata->name, 0);
        hRData.virtualSize           = static_cast<std::uint32_t>(rdataSize);
        hRData.virtualAddress        = rdata->rva;
        hRData.pointerToRelocations  = 0;
        hRData.pointerToLinenumbers  = 0;
        hRData.numberOfRelocations   = 0;
        hRData.numberOfLinenumbers   = 0;
        hRData.characteristics       = secRodata->type;
        rdata->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hRData);
    }
    // .data section header (D-LK4-DATA-PRODUCER) — mutable initialized globals.
    // Characteristics from the SCHEMA ROW (`secData->type` = IMAGE_SCN_CNT_
    // INITIALIZED_DATA | MEM_READ | MEM_WRITE = 0xC0000040 — the MEM_WRITE bit
    // is what makes a runtime store legal; a `.rdata` store would fault).
    // sizeOfRawData / pointerToRawData filled below (file-backed).
    if (data.has_value()) {
        PeSectionHeader hData{};
        hData.name                  = encodeSectionName(secData->name, 0);
        hData.virtualSize           = static_cast<std::uint32_t>(dataSize);
        hData.virtualAddress        = data->rva;
        hData.pointerToRelocations  = 0;
        hData.pointerToLinenumbers  = 0;
        hData.numberOfRelocations   = 0;
        hData.numberOfLinenumbers   = 0;
        hData.characteristics       = secData->type;
        data->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hData);
    }
    // .bss section header (D-LK4-DATA-PRODUCER) — zero-init globals. Uninitial-
    // ized data: SizeOfRawData = 0 and PointerToRawData = 0 (NO file bytes — the
    // loader zero-fills VirtualSize bytes). Characteristics from the SCHEMA ROW
    // (`secBss->type` = IMAGE_SCN_CNT_UNINITIALIZED_DATA | MEM_READ | MEM_WRITE
    // = 0xC0000080).
    if (bss.has_value()) {
        PeSectionHeader hBss{};
        hBss.name                  = encodeSectionName(secBss->name, 0);
        hBss.virtualSize           = static_cast<std::uint32_t>(
                                         bssDataLayout.spanSize);
        hBss.virtualAddress        = bss->rva;
        hBss.sizeOfRawData         = 0;   // NOBITS — no file footprint
        hBss.pointerToRawData      = 0;
        hBss.pointerToRelocations  = 0;
        hBss.pointerToLinenumbers  = 0;
        hBss.numberOfRelocations   = 0;
        hBss.numberOfLinenumbers   = 0;
        hBss.characteristics       = secBss->type;
        bss->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hBss);
    }
    // .tls section header (D-CSUBSET-THREAD-LOCAL, TLS C3) — the thread-local
    // template + IMAGE_TLS_DIRECTORY64 + `_tls_index`. File-backed, writable
    // (Characteristics from the schema row = 0xC0000040 — the loader applies
    // base relocations into the template + directory VA fields, requiring
    // MEM_WRITE). virtualSize = the file bytes (template + dir + index); the
    // per-thread zero-fill is NOT in this section (it rides the directory's
    // SizeOfZeroFill, allocated per thread). Placed after .bss, before .xdata.
    if (tls.has_value()) {
        PeSectionHeader hTls{};
        hTls.name                  = encodeSectionName(secTls->name, 0);
        hTls.virtualSize           = static_cast<std::uint32_t>(tlsFileSize);
        hTls.virtualAddress        = tls->rva;
        // sizeOfRawData / pointerToRawData filled below (file-backed).
        hTls.pointerToRelocations  = 0;
        hTls.pointerToLinenumbers  = 0;
        hTls.numberOfRelocations   = 0;
        hTls.numberOfLinenumbers   = 0;
        hTls.characteristics       = secTls->type;
        tls->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hTls);
    }
    // .xdata + .pdata section headers (D-WIN64-PDATA-XDATA-UNWIND) — read-only
    // initialized data (0x40000040, like .rdata), placed after .bss and before
    // .idata. sizeOfRawData / pointerToRawData filled below.
    constexpr std::uint32_t kUnwindCharacteristics = 0x40000040u;
    if (xdata.has_value()) {
        PeSectionHeader hXData{};
        hXData.name            = encodeSectionName(".xdata", 0);
        hXData.virtualSize     = static_cast<std::uint32_t>(xdataBytes.size());
        hXData.virtualAddress  = xdata->rva;
        hXData.characteristics = kUnwindCharacteristics;
        xdata->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hXData);
    }
    if (pdata.has_value()) {
        PeSectionHeader hPData{};
        hPData.name            = encodeSectionName(".pdata", 0);
        hPData.virtualSize     = static_cast<std::uint32_t>(pdataBytes.size());
        hPData.virtualAddress  = pdata->rva;
        hPData.characteristics = kUnwindCharacteristics;
        pdata->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hPData);
    }
    // .edata section header (c152, D-LK2-4 — dll exports). Read-only
    // initialized data (0x40000040, like .rdata): the loader only
    // READS the export directory; GetProcAddress walks it in place.
    // Placed after .pdata and before .idata (its VA slot in the
    // chain above).
    constexpr std::uint32_t kEDataCharacteristics = 0x40000040u;
    if (edata.has_value()) {
        PeSectionHeader hEData{};
        hEData.name            = encodeSectionName(".edata", 0);
        hEData.virtualSize     = static_cast<std::uint32_t>(edataBytes.size());
        hEData.virtualAddress  = edata->rva;
        hEData.characteristics = kEDataCharacteristics;
        edata->headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hEData);
    }
    // .idata section header (when externImports non-empty).
    // Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA (0x40) |
    //                   IMAGE_SCN_MEM_READ (0x40000000) |
    //                   IMAGE_SCN_MEM_WRITE (0x80000000)
    //                 = 0xC0000040. PE32+ images keep the import
    // table writable so the loader can patch IAT slots in-place.
    constexpr std::uint32_t kIDataCharacteristics = 0xC0000040u;
    std::optional<DataSectionLayout> idata;
    if (hasImports) {
        PeSectionHeader hIData{};
        hIData.name                  = encodeSectionName(".idata", 0);
        hIData.virtualSize           = static_cast<std::uint32_t>(idataSize);
        hIData.virtualAddress        = idataRva;
        // sizeOfRawData / pointerToRawData filled below.
        hIData.pointerToRelocations  = 0;
        hIData.pointerToLinenumbers  = 0;
        hIData.numberOfRelocations   = 0;
        hIData.numberOfLinenumbers   = 0;
        hIData.characteristics       = kIDataCharacteristics;
        DataSectionLayout iLayout;
        iLayout.rva         = idataRva;
        iLayout.virtualSize = static_cast<std::uint32_t>(
            alignUp(idataSize, sectionAlign));
        iLayout.headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hIData);
        idata = iLayout;
    }
    // .reloc section header (when the image carries absolute fixups — cross-CU thunk
    // slots). Placed in VA space after .idata (or .rdata, or .text). Characteristics =
    // IMAGE_SCN_CNT_INITIALIZED_DATA (0x40) | IMAGE_SCN_MEM_DISCARDABLE (0x02000000) |
    // IMAGE_SCN_MEM_READ (0x40000000) = 0x42000040 — the loader consumes the base-reloc
    // table during load and may discard the section afterward.
    constexpr std::uint32_t kRelocCharacteristics = 0x42000040u;
    std::optional<DataSectionLayout> reloc;
    if (hasBaseRelocs) {
        // `.reloc` follows everything: .idata end when present, else the end of
        // the data-section chain (`dataChainRva` was advanced past .rdata/.data/
        // .bss; it equals text-end when no data sections exist).
        std::uint32_t const prevVaEnd =
            idata.has_value() ? idata->rva + idata->virtualSize
                              : dataChainRva;
        std::uint32_t const relocRva =
            static_cast<std::uint32_t>(alignUp(prevVaEnd, sectionAlign));
        PeSectionHeader hReloc{};
        hReloc.name                  = encodeSectionName(".reloc", 0);
        hReloc.virtualSize           = static_cast<std::uint32_t>(relocBytes.size());
        hReloc.virtualAddress        = relocRva;
        // sizeOfRawData / pointerToRawData filled below.
        hReloc.pointerToRelocations  = 0;
        hReloc.pointerToLinenumbers  = 0;
        hReloc.numberOfRelocations   = 0;
        hReloc.numberOfLinenumbers   = 0;
        hReloc.characteristics       = kRelocCharacteristics;
        DataSectionLayout rLayout;
        rLayout.rva         = relocRva;
        rLayout.virtualSize = static_cast<std::uint32_t>(
            alignUp(relocBytes.size(), sectionAlign));
        rLayout.headerIndex = sectionHeaders.size();
        sectionHeaders.push_back(hReloc);
        reloc = rLayout;
    }

    std::uint32_t const numSections =
        static_cast<std::uint32_t>(sectionHeaders.size());
    std::size_t const headerBytesUnpadded =
        headerBytesUnpaddedInitial
        + numSections * kSectionHeaderSize;
    std::uint32_t const sizeOfHeaders =
        static_cast<std::uint32_t>(alignUp(headerBytesUnpadded, fileAlign));
    std::uint32_t const textRawSize =
        static_cast<std::uint32_t>(alignUp(text.size(), fileAlign));
    std::uint32_t const textRawPointer = sizeOfHeaders;
    sectionHeaders[textHdrIdx].sizeOfRawData    = textRawSize;
    sectionHeaders[textHdrIdx].pointerToRawData = textRawPointer;
    // .rdata raw size + file pointer (D-LK2-RODATA): file-aligned,
    // placed immediately after .text. Indices captured at push
    // time (DataSectionLayout::headerIndex) — no magic-arithmetic
    // index derivation.
    // File-pointer chain: text → rdata → data → (bss: NO raw) → idata → reloc.
    // `nextRawPointer` tracks the running file offset of the next file-backed
    // section. `.bss` is NOBITS — it does NOT advance the file pointer.
    std::uint32_t nextRawPointer = textRawPointer + textRawSize;
    if (rdata.has_value()) {
        rdata->rawSize = static_cast<std::uint32_t>(
            alignUp(rdataSize, fileAlign));
        rdata->rawPointer = nextRawPointer;
        sectionHeaders[rdata->headerIndex].sizeOfRawData = rdata->rawSize;
        sectionHeaders[rdata->headerIndex].pointerToRawData = rdata->rawPointer;
        nextRawPointer += rdata->rawSize;
    }
    if (data.has_value()) {
        data->rawSize = static_cast<std::uint32_t>(
            alignUp(dataSize, fileAlign));
        data->rawPointer = nextRawPointer;
        sectionHeaders[data->headerIndex].sizeOfRawData = data->rawSize;
        sectionHeaders[data->headerIndex].pointerToRawData = data->rawPointer;
        nextRawPointer += data->rawSize;
    }
    // .bss is NOBITS: sizeOfRawData / pointerToRawData stay 0 (set at header
    // construction); it consumes no file bytes, so `nextRawPointer` is unchanged.
    // .tls (D-CSUBSET-THREAD-LOCAL, TLS C3): file-backed (template + directory
    // + index), placed after .data/.bss in file order (matching its section-
    // header order after .bss).
    if (tls.has_value()) {
        tls->rawSize = static_cast<std::uint32_t>(
            alignUp(tlsFileSize, fileAlign));
        tls->rawPointer = nextRawPointer;
        sectionHeaders[tls->headerIndex].sizeOfRawData = tls->rawSize;
        sectionHeaders[tls->headerIndex].pointerToRawData = tls->rawPointer;
        nextRawPointer += tls->rawSize;
    }
    if (xdata.has_value()) {
        xdata->rawSize = static_cast<std::uint32_t>(
            alignUp(xdataBytes.size(), fileAlign));
        xdata->rawPointer = nextRawPointer;
        sectionHeaders[xdata->headerIndex].sizeOfRawData = xdata->rawSize;
        sectionHeaders[xdata->headerIndex].pointerToRawData = xdata->rawPointer;
        nextRawPointer += xdata->rawSize;
    }
    if (pdata.has_value()) {
        pdata->rawSize = static_cast<std::uint32_t>(
            alignUp(pdataBytes.size(), fileAlign));
        pdata->rawPointer = nextRawPointer;
        sectionHeaders[pdata->headerIndex].sizeOfRawData = pdata->rawSize;
        sectionHeaders[pdata->headerIndex].pointerToRawData = pdata->rawPointer;
        nextRawPointer += pdata->rawSize;
    }
    // .edata (c152, D-LK2-4): file-backed, after .pdata and before
    // .idata (matching its section-header and VA-chain order).
    if (edata.has_value()) {
        edata->rawSize = static_cast<std::uint32_t>(
            alignUp(edataBytes.size(), fileAlign));
        edata->rawPointer = nextRawPointer;
        sectionHeaders[edata->headerIndex].sizeOfRawData = edata->rawSize;
        sectionHeaders[edata->headerIndex].pointerToRawData = edata->rawPointer;
        nextRawPointer += edata->rawSize;
    }
    if (idata.has_value()) {
        idata->rawSize = static_cast<std::uint32_t>(
            alignUp(idataSize, fileAlign));
        idata->rawPointer = nextRawPointer;
        sectionHeaders[idata->headerIndex].sizeOfRawData = idata->rawSize;
        sectionHeaders[idata->headerIndex].pointerToRawData = idata->rawPointer;
        nextRawPointer += idata->rawSize;
    }
    // .reloc raw size + file pointer: file-aligned, immediately after the last
    // prior file-backed section.
    if (reloc.has_value()) {
        reloc->rawSize = static_cast<std::uint32_t>(
            alignUp(relocBytes.size(), fileAlign));
        reloc->rawPointer = nextRawPointer;
        sectionHeaders[reloc->headerIndex].sizeOfRawData = reloc->rawSize;
        sectionHeaders[reloc->headerIndex].pointerToRawData = reloc->rawPointer;
        nextRawPointer += reloc->rawSize;
    }
    // SizeOfImage = alignUp(highest_section_va + virtualSize,
    // sectionAlignment) per PE/COFF §3.4. The highest VA-extent
    // is `.reloc` when present, else `.idata`, else `.rdata`, else
    // `.text`. The linear walk over present DataSectionLayouts
    // generalizes naturally when a 4th section type lands.
    std::uint32_t const textVirtualSize = textVirtualSizeE;
    std::uint32_t lastSectionVaEnd =
        static_cast<std::uint32_t>(secText.virtualAddress)
        + textVirtualSize;
    if (rdata.has_value()) {
        lastSectionVaEnd = rdata->rva + rdata->virtualSize;
    }
    if (data.has_value()) {
        lastSectionVaEnd = data->rva + data->virtualSize;
    }
    if (bss.has_value()) {
        // .bss contributes to the image's MEMORY extent (VirtualSize) even
        // though it has zero file footprint — the loader reserves it.
        lastSectionVaEnd = bss->rva + bss->virtualSize;
    }
    if (tls.has_value()) {   // TLS C3 — .tls RVA is after .bss in the chain
        lastSectionVaEnd = tls->rva + tls->virtualSize;
    }
    if (xdata.has_value()) {
        lastSectionVaEnd = xdata->rva + xdata->virtualSize;
    }
    if (pdata.has_value()) {
        lastSectionVaEnd = pdata->rva + pdata->virtualSize;
    }
    if (edata.has_value()) {   // c152 — .edata RVA is after .pdata
        lastSectionVaEnd = edata->rva + edata->virtualSize;
    }
    if (idata.has_value()) {
        lastSectionVaEnd = idata->rva + idata->virtualSize;
    }
    if (reloc.has_value()) {
        lastSectionVaEnd = reloc->rva + reloc->virtualSize;
    }
    std::uint32_t const sizeOfImage = static_cast<std::uint32_t>(
        alignUp(lastSectionVaEnd, sectionAlign));
    // c152 (D-LK2-4): a dll has NO entry — AddressOfEntryPoint = 0
    // tells the loader to skip the DllMain notification call
    // entirely (legal PE; the `link /NOENTRY` shape). The exec keeps
    // its resolved entry (the injected trampoline via
    // imageEntryOverride, or the schema/functions[0] fallback).
    std::uint32_t const addressOfEntryPoint = isDll
        ? 0u
        : static_cast<std::uint32_t>(secText.virtualAddress
                                      + funcTextStart[entryFnIdx]);
    std::uint32_t const baseOfCode =
        static_cast<std::uint32_t>(secText.virtualAddress);

    // ── (e) Emit bytes ────────────────────────────────────────
    std::vector<std::uint8_t> bytes;
    bytes.reserve(sizeOfHeaders + textRawSize);

    // IMAGE_DOS_HEADER: "MZ" + zeros + e_lfanew at offset 0x3C
    // pointing to the PE signature at 0x80.
    bytes.resize(kDosHeaderSize, 0);
    bytes[0] = 'M'; bytes[1] = 'Z';
    constexpr std::size_t kELfaNewOffset = 0x3C;
    std::uint32_t const peSigFileOff =
        static_cast<std::uint32_t>(kDosHeaderSize + kDosStubSize);
    bytes[kELfaNewOffset + 0] = static_cast<std::uint8_t>(peSigFileOff);
    bytes[kELfaNewOffset + 1] = static_cast<std::uint8_t>(peSigFileOff >> 8);
    bytes[kELfaNewOffset + 2] = static_cast<std::uint8_t>(peSigFileOff >> 16);
    bytes[kELfaNewOffset + 3] = static_cast<std::uint8_t>(peSigFileOff >> 24);

    // MS-DOS stub: 64 zero bytes (the kernel doesn't read it; modern
    // toolchains ship the legacy "This program cannot be run in DOS
    // mode" message, but a zero-filled stub is loader-legal — the
    // Windows loader never executes this region).
    bytes.resize(bytes.size() + kDosStubSize, 0);

    // PE signature "PE\0\0"
    appendU8(bytes, 'P'); appendU8(bytes, 'E');
    appendU8(bytes, 0);   appendU8(bytes, 0);

    // IMAGE_FILE_HEADER
    appendU16LE(bytes, id.machine);
    appendU16LE(bytes, static_cast<std::uint16_t>(numSections));
    appendU32LE(bytes, 0);  // TimeDateStamp = 0 (deterministic)
    appendU32LE(bytes, 0);  // PointerToSymbolTable = 0 (image has no symtab)
    appendU32LE(bytes, 0);  // NumberOfSymbols = 0
    appendU16LE(bytes, static_cast<std::uint16_t>(kOptionalHeader64Size));
    appendU16LE(bytes, id.characteristics);

    // IMAGE_OPTIONAL_HEADER64 (PE/COFF §3.4)
    appendU16LE(bytes, oh.magic);
    appendU8(bytes, 0);  // MajorLinkerVersion (not load-bearing)
    appendU8(bytes, 0);  // MinorLinkerVersion
    appendU32LE(bytes, textRawSize);           // SizeOfCode
    // PE/COFF §3.4: SizeOfInitializedData is the sum of file-aligned
    // sizes of all sections carrying IMAGE_SCN_CNT_INITIALIZED_DATA.
    // `.rdata` (D-LK2-RODATA), `.idata`, AND `.reloc` all carry that
    // flag (.reloc's 0x42000040 sets CNT_INITIALIZED_DATA; its
    // MEM_DISCARDABLE bit does not exempt it from the §3.4 sum).
    // `.data` (D-LK4-DATA-PRODUCER) carries CNT_INITIALIZED_DATA → joins the
    // sum; `.bss` carries CNT_UNINITIALIZED_DATA → goes to SizeOfUninitializedData.
    std::uint32_t const sizeOfInitializedData =
        (rdata.has_value() ? rdata->rawSize : 0u)
        + (data.has_value() ? data->rawSize : 0u)
        + (tls.has_value() ? tls->rawSize : 0u)   // TLS C3 — CNT_INITIALIZED_DATA
        + (edata.has_value() ? edata->rawSize : 0u)  // c152 — dll exports
        + (idata.has_value() ? idata->rawSize : 0u)
        + (reloc.has_value() ? reloc->rawSize : 0u);
    std::uint32_t const sizeOfUninitializedData =
        bss.has_value()
            ? static_cast<std::uint32_t>(alignUp(bssDataLayout.spanSize,
                                                 fileAlign))
            : 0u;
    appendU32LE(bytes, sizeOfInitializedData);
    appendU32LE(bytes, sizeOfUninitializedData);   // SizeOfUninitializedData
    appendU32LE(bytes, addressOfEntryPoint);   // AddressOfEntryPoint
    appendU32LE(bytes, baseOfCode);            // BaseOfCode
    // (PE32+ omits BaseOfData)
    appendU64LE(bytes, oh.imageBase);
    appendU32LE(bytes, sectionAlign);
    appendU32LE(bytes, fileAlign);
    appendU16LE(bytes, oh.majorOperatingSystemVersion);
    appendU16LE(bytes, oh.minorOperatingSystemVersion);
    appendU16LE(bytes, 0);                     // MajorImageVersion
    appendU16LE(bytes, 0);                     // MinorImageVersion
    appendU16LE(bytes, oh.majorSubsystemVersion);
    appendU16LE(bytes, oh.minorSubsystemVersion);
    appendU32LE(bytes, 0);                     // Win32VersionValue (reserved)
    appendU32LE(bytes, sizeOfImage);
    appendU32LE(bytes, sizeOfHeaders);
    appendU32LE(bytes, 0);                     // CheckSum (loader allows 0)
    appendU16LE(bytes, oh.subsystem);
    appendU16LE(bytes, oh.dllCharacteristics);
    appendU64LE(bytes, oh.sizeOfStackReserve);
    appendU64LE(bytes, oh.sizeOfStackCommit);
    appendU64LE(bytes, oh.sizeOfHeapReserve);
    appendU64LE(bytes, oh.sizeOfHeapCommit);
    appendU32LE(bytes, 0);                     // LoaderFlags (reserved)
    appendU32LE(bytes,
        static_cast<std::uint32_t>(kNumberOfRvaAndSizes));
    // 16 data directories. PE/COFF §3.4 indices we populate:
    //   [1] Import Table — RVA to first ImageImportDescriptor;
    //                       Size covers all descriptors + terminator.
    //  [12] Import Address Table — RVA to IAT block; Size covers
    //                       all IAT slots across libraries.
    // Other directories (Export, Resource, Exception, etc.) remain
    // zero — minimum-loadable image needs only Import for FFI.
    std::uint32_t const importDirRva  = hasImports ? idataRva : 0u;
    std::uint32_t const importDirSize = hasImports
        ? static_cast<std::uint32_t>(descriptorBlockSize)
        : 0u;
    std::uint32_t const iatDirRva = hasImports
        ? idataRva + static_cast<std::uint32_t>(iatOffsets[0])
        : 0u;
    // Sum IAT block sizes explicitly rather than subtracting cursors.
    // Subtraction-based size silently misreports if a future cycle
    // inserts data between IATs and the HINT/NAME block (silent-
    // failure H1 fold). Each library's IAT spans (externCount+1) *
    // kThunkSize bytes (including the u64-zero terminator slot).
    std::size_t iatTotal = 0;
    for (auto const& libName : libraryOrder) {
        iatTotal +=
            (externsByLib[libName].size() + 1) * kThunkSize;
    }
    std::uint32_t const iatDirSize = hasImports
        ? static_cast<std::uint32_t>(iatTotal)
        : 0u;
    // LK7: Authenticode attribute-cert placeholder reservation.
    // PE COFF §5.7 directory index 4 (IMAGE_DIRECTORY_ENTRY_SECURITY)
    // carries a FILE-OFFSET reference (NOT an RVA) and pairs it with
    // the cert-table byte size. Plan 16 (codesign + publish) fills
    // the reserved bytes post-link with the WIN_CERTIFICATE table.
    // Computed AFTER all section file offsets land below; the
    // reservation sits at the end of the file (8-byte aligned per
    // §5.9.1) so it doesn't disturb the section table layout.
    bool const emitCertReservation = oh.attributeCertReserveSize != 0;
    // sectionsEndFileOff = byte offset immediately after the last
    // section's raw data. Layout-dependent: `.text` always present;
    // optional `.rdata` after; optional `.idata` after. All raw
    // sizes are fileAlignment-padded already; the cert table sits
    // at the 8-byte-aligned offset past the last section.
    std::uint64_t sectionsEndFileOff =
        static_cast<std::uint64_t>(textRawPointer) + textRawSize;
    if (rdata.has_value()) {
        sectionsEndFileOff =
            static_cast<std::uint64_t>(rdata->rawPointer)
            + rdata->rawSize;
    }
    // `.data` is file-backed (D-LK4-DATA-PRODUCER); `.bss` is NOBITS (no file
    // bytes) so it does NOT extend the on-disk section end.
    if (data.has_value()) {
        sectionsEndFileOff =
            static_cast<std::uint64_t>(data->rawPointer)
            + data->rawSize;
    }
    // .tls (TLS C3) is file-backed, after .data in file order. (When TLS is
    // present so is .reloc — the directory VA base-relocs — which lands last
    // and overwrites this; kept for completeness / a future no-reloc TLS case.)
    if (tls.has_value()) {
        sectionsEndFileOff =
            static_cast<std::uint64_t>(tls->rawPointer)
            + tls->rawSize;
    }
    if (edata.has_value()) {   // c152 — .edata is file-backed, pre-.idata
        sectionsEndFileOff =
            static_cast<std::uint64_t>(edata->rawPointer)
            + edata->rawSize;
    }
    if (idata.has_value()) {
        sectionsEndFileOff =
            static_cast<std::uint64_t>(idata->rawPointer)
            + idata->rawSize;
    }
    if (reloc.has_value()) {
        sectionsEndFileOff =
            static_cast<std::uint64_t>(reloc->rawPointer)
            + reloc->rawSize;
    }
    std::uint64_t const certTableFileOff64 = emitCertReservation
        ? ((sectionsEndFileOff + 7u) & ~std::uint64_t{7})
        : 0u;
    // `IMAGE_DATA_DIRECTORY[4].VirtualAddress` is u32 per the PE
    // COFF §3.4.6 attribute-cert table contract. If the section
    // layout pushes the cert table past 4 GiB, a silent u32 cast
    // would point the Windows loader at section bytes, corrupting
    // the IAT. Fail loud rather than ship a malformed image.
    // (silent-failure MEDIUM fold, LK7 review.)
    if (emitCertReservation
     && certTableFileOff64
            > std::numeric_limits<std::uint32_t>::max()) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::format("pe::encodeExec: attribute-cert table file "
                         "offset 0x{:x} exceeds u32 — "
                         "IMAGE_DATA_DIRECTORY[4].VirtualAddress is "
                         "u32 by PE COFF §3.4.6.",
                         certTableFileOff64));
        return {};
    }
    std::uint32_t const certTableFileOff =
        static_cast<std::uint32_t>(certTableFileOff64);
    // IMAGE_DIRECTORY_ENTRY_BASERELOC (index 5): RVA + byte size of the .reloc
    // section's base-relocation table. The loader walks it to rebase every absolute
    // fixup when ASLR slides the image. Size is the table's VIRTUAL size (unpadded
    // block bytes), not the file-aligned raw size.
    std::uint32_t const baseRelocDirRva  =
        reloc.has_value() ? reloc->rva : 0u;
    std::uint32_t const baseRelocDirSize =
        reloc.has_value() ? static_cast<std::uint32_t>(relocBytes.size()) : 0u;
    // IMAGE_DIRECTORY_ENTRY_EXCEPTION (index 3, D-WIN64-PDATA-XDATA-UNWIND): RVA +
    // byte size of the .pdata RUNTIME_FUNCTION array — the OS exception dispatcher
    // + RtlLookupFunctionEntry binary-search it. Size is the array's VIRTUAL size.
    std::uint32_t const pdataDirRva  = pdata.has_value() ? pdata->rva : 0u;
    std::uint32_t const pdataDirSize =
        pdata.has_value() ? static_cast<std::uint32_t>(pdataBytes.size()) : 0u;
    // IMAGE_DIRECTORY_ENTRY_TLS (index 9, D-CSUBSET-THREAD-LOCAL TLS C3): RVA +
    // byte size (40 = the IMAGE_TLS_DIRECTORY64) of the TLS directory the
    // loader reads to set up each thread's block. The directory lives at the
    // tail of `.tls` (tlsRva + tlsDirOffset). A no-TLS image leaves this entry
    // zero (byte-identical to a pre-C3 image).
    std::uint32_t const tlsDirRva =
        tls.has_value()
            ? tlsRva + static_cast<std::uint32_t>(tlsDirOffset)
            : 0u;
    std::uint32_t const tlsDirSize = tls.has_value() ? 40u : 0u;
    // IMAGE_DIRECTORY_ENTRY_EXPORT (index 0, c152 D-LK2-4): RVA + byte
    // size of the export directory block — GetProcAddress reads it.
    // Size covers the WHOLE export block (directory + EAT + name
    // pointers + ordinals + strings): the loader uses the [rva,
    // rva+size) span for forwarder detection, so it must cover
    // exactly the synthesized bytes (see the (b4) block comment).
    std::uint32_t const exportDirRva =
        edata.has_value() ? edata->rva : 0u;
    std::uint32_t const exportDirSize =
        edata.has_value() ? static_cast<std::uint32_t>(edataBytes.size())
                          : 0u;
    for (std::size_t i = 0; i < kNumberOfRvaAndSizes; ++i) {
        if (i == 0u) {
            appendU32LE(bytes, exportDirRva);
            appendU32LE(bytes, exportDirSize);
        } else if (i == 1u) {
            appendU32LE(bytes, importDirRva);
            appendU32LE(bytes, importDirSize);
        } else if (i == 3u) {
            appendU32LE(bytes, pdataDirRva);
            appendU32LE(bytes, pdataDirSize);
        } else if (i == 4u) {
            // IMAGE_DIRECTORY_ENTRY_SECURITY: file offset, not RVA.
            appendU32LE(bytes, certTableFileOff);
            appendU32LE(bytes, oh.attributeCertReserveSize);
        } else if (i == 5u) {
            // IMAGE_DIRECTORY_ENTRY_BASERELOC.
            appendU32LE(bytes, baseRelocDirRva);
            appendU32LE(bytes, baseRelocDirSize);
        } else if (i == 9u) {
            // IMAGE_DIRECTORY_ENTRY_TLS.
            appendU32LE(bytes, tlsDirRva);
            appendU32LE(bytes, tlsDirSize);
        } else if (i == 12u) {
            appendU32LE(bytes, iatDirRva);
            appendU32LE(bytes, iatDirSize);
        } else {
            appendU32LE(bytes, 0);
            appendU32LE(bytes, 0);
        }
    }

    // IMAGE_SECTION_HEADER table (derives from sectionHeaders vector)
    for (auto const& h : sectionHeaders) {
        writeSectionHeader(bytes, h);
    }

    // Pad headers area to fileAlignment.
    while (bytes.size() < sizeOfHeaders) bytes.push_back(0);

    // .text body, padded to fileAlignment.
    bytes.insert(bytes.end(), text.begin(), text.end());
    while (bytes.size() < static_cast<std::size_t>(textRawPointer)
                            + textRawSize) {
        bytes.push_back(0);
    }

    // .rdata body (D-LK2-RODATA closure). The bytes are the
    // concatenated `AssembledData` items with per-item Alignment
    // padding precomputed in `rdataBytes`. Pad to fileAlignment.
    if (rdata.has_value()) {
        bytes.insert(bytes.end(),
                     rdataBytes.begin(), rdataBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(rdata->rawPointer)
                    + rdata->rawSize) {
            bytes.push_back(0);
        }
    }

    // .data body (D-LK4-DATA-PRODUCER) — mutable initialized globals, file-
    // backed, padded to fileAlignment. `.bss` is NOBITS: it emits NO file bytes
    // (the loader zero-fills it at load), so there is no `.bss` body pass.
    if (data.has_value()) {
        bytes.insert(bytes.end(), dataBytes.begin(), dataBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(data->rawPointer)
                    + data->rawSize) {
            bytes.push_back(0);
        }
    }

    // .tls body (D-CSUBSET-THREAD-LOCAL, TLS C3) — the thread-local template +
    // patched IMAGE_TLS_DIRECTORY64 + zero `_tls_index` slot, file-aligned.
    // Emitted after .data/.bss and before .xdata (matching its file-pointer
    // order); the preceding section padded bytes.size() up to tls->rawPointer.
    if (tls.has_value()) {
        bytes.insert(bytes.end(), tlsBytes.begin(), tlsBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(tls->rawPointer) + tls->rawSize) {
            bytes.push_back(0);
        }
    }

    // .xdata + .pdata bodies (D-WIN64-PDATA-XDATA-UNWIND), file-aligned. Emitted
    // after .data and before .idata (their nextRawPointer order); the preceding
    // section padded bytes.size() up to xdata->rawPointer.
    if (xdata.has_value()) {
        bytes.insert(bytes.end(), xdataBytes.begin(), xdataBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(xdata->rawPointer) + xdata->rawSize) {
            bytes.push_back(0);
        }
    }
    if (pdata.has_value()) {
        bytes.insert(bytes.end(), pdataBytes.begin(), pdataBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(pdata->rawPointer) + pdata->rawSize) {
            bytes.push_back(0);
        }
    }

    // .edata body (c152, D-LK2-4 — dll export directory), padded to
    // fileAlignment. The bytes were fully synthesized in step (b4);
    // emitted after .pdata and before .idata (its raw-pointer order).
    if (edata.has_value()) {
        bytes.insert(bytes.end(), edataBytes.begin(), edataBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(edata->rawPointer) + edata->rawSize) {
            bytes.push_back(0);
        }
    }

    // .idata body (when externImports non-empty), padded to
    // fileAlignment. Layout precomputed above; emit ImageImport
    // Descriptors[N+1] + ILT/IAT thunks + HINT/NAME table + DLL
    // name strings.
    if (idata.has_value()) {
        std::vector<std::uint8_t> idataBytes(idataSize, 0);
        auto putU32 = [&](std::size_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                idataBytes[off + i] =
                    static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
        };
        auto putU64 = [&](std::size_t off, std::uint64_t v) {
            for (int i = 0; i < 8; ++i)
                idataBytes[off + i] =
                    static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
        };
        // ImageImportDescriptor[i] for each library; entry [numLibs]
        // is the all-zero terminator.
        for (std::size_t li = 0; li < numLibs; ++li) {
            std::size_t const dOff = li * kImportDescriptorSize;
            std::uint32_t const iltRva =
                idataRva + static_cast<std::uint32_t>(iltOffsets[li]);
            std::uint32_t const iatRva =
                idataRva + static_cast<std::uint32_t>(iatOffsets[li]);
            putU32(dOff +  0, iltRva);                  // OriginalFirstThunk
            putU32(dOff +  4, 0);                       // TimeDateStamp
            putU32(dOff +  8, 0);                       // ForwarderChain
            putU32(dOff + 12, dllNameRvaByLib[li]);     // Name (DLL path RVA)
            putU32(dOff + 16, iatRva);                  // FirstThunk (IAT RVA)
        }
        // ILT + IAT thunks (PE32+: u64 each; bit 63 = ordinal flag,
        // we use by-name imports only — set RVA to HINT/NAME entry).
        for (std::size_t li = 0; li < numLibs; ++li) {
            std::size_t slotIdx = 0;
            auto const& externs = externsByLib[libraryOrder[li]];
            for (auto extIdx : externs) {
                std::uint64_t const thunk =
                    static_cast<std::uint64_t>(hintNameRvaBySym[extIdx]);
                putU64(iltOffsets[li] + slotIdx * kThunkSize, thunk);
                putU64(iatOffsets[li] + slotIdx * kThunkSize, thunk);
                ++slotIdx;
            }
            // Zero terminator already in place (idataBytes was 0-initialised).
        }
        // HINT/NAME table entries: u16 hint=0 + NUL-terminated name.
        for (std::size_t li = 0; li < numLibs; ++li) {
            for (auto extIdx : externsByLib[libraryOrder[li]]) {
                std::uint32_t const rva = hintNameRvaBySym[extIdx];
                std::size_t const off = rva - idataRva;
                // hint = 0 (no symbol-table hint); idataBytes already
                // zero-initialised so we just write the name.
                auto const& name = module.externImports[extIdx].mangledName;
                for (std::size_t i = 0; i < name.size(); ++i) {
                    idataBytes[off + 2 + i] =
                        static_cast<std::uint8_t>(name[i]);
                }
                // NUL + optional pad already zero.
            }
        }
        // DLL name strings.
        for (std::size_t li = 0; li < numLibs; ++li) {
            std::uint32_t const rva = dllNameRvaByLib[li];
            std::size_t const off = rva - idataRva;
            auto const& dll = libraryOrder[li];
            for (std::size_t i = 0; i < dll.size(); ++i) {
                idataBytes[off + i] = static_cast<std::uint8_t>(dll[i]);
            }
        }
        bytes.insert(bytes.end(), idataBytes.begin(), idataBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(idata->rawPointer)
                    + idata->rawSize) {
            bytes.push_back(0);
        }
    }

    // .reloc body (base relocation table), padded to fileAlignment. The block bytes
    // were precomputed above (grouped by 4 KiB page); just emit + pad.
    if (reloc.has_value()) {
        bytes.insert(bytes.end(), relocBytes.begin(), relocBytes.end());
        while (bytes.size()
                < static_cast<std::size_t>(reloc->rawPointer)
                    + reloc->rawSize) {
            bytes.push_back(0);
        }
    }

    // LK7: pad to 8-byte-aligned cert table file offset, then
    // append `attributeCertReserveSize` zero bytes. Plan 16
    // patches the bytes post-link with the WIN_CERTIFICATE
    // entries (PE COFF §5.9.1). The reservation sits OUTSIDE
    // the loaded image — the Windows loader maps sections by
    // RVA, while the cert table is referenced exclusively via
    // its file-offset directory entry.
    if (emitCertReservation) {
        while (bytes.size() < certTableFileOff) bytes.push_back(0);
        bytes.insert(bytes.end(),
                     oh.attributeCertReserveSize,
                     std::uint8_t{0});
    }

    return bytes;
}

} // namespace

} // namespace dss::pe
