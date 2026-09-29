#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ── THE INLINE-ASM BUNDLE (P68 round 8 part 4) ─────────────────────────────
//
// ★★★ ONE STATEMENT IS ONE INSTRUCTION FOR AS LONG AS REGISTERS ARE BEING
// ALLOCATED, AND ITS TEMPLATE BECOMES REAL INSTRUCTIONS ONLY AFTER THAT.
//
// An inline-asm template used to be lowered straight into the enclosing
// function, one LIR instruction per template line. Each line was then its own
// allocation point, and the linear-scan allocator and the spill-everywhere
// rewriter placed spill and reload code BETWEEN the template's lines.
// ✔MEASURED 2026-09-21 (x86_64, release, six register inputs, a seven-line
// template): 29 instructions and 15 memory accesses between the template's two
// `nop` delimiters. GCC's manual gives the template to the ASSEMBLER verbatim
// — nothing the compiler generates ever lands inside it — and a template
// whose meaning depends on that (an `ldaxr` / `stlxr` pair, which a store
// between the two makes fail every time) LOOPED FOREVER.
//
// The statement is now an `asm_region` BUNDLE:
//   * its OPERANDS are the template's operand registers — each a virtual
//     register before allocation, a physical one after — and a ROLE per
//     operand says whether the statement reads it, writes it, or both, and
//     whether it writes it before its inputs are consumed ("=&r");
//   * its BODY — the template lowered by the shared assembly engine into a
//     scratch one-function module, with the template's labels as blocks — is
//     a module side structure (`LirAsmRegionPool`, carried like the
//     register-constraint pool);
//   * `expandAsmRegions` (below) replaces the bundle with its body once the
//     allocation is final, substituting each operand's physical register, and
//     turning the body's blocks into real blocks of the function.
//
// ★★ THE ROLES ARE READ THROUGH ONE HELPER. What an instruction DEFINES used to
// be `instResult`, full stop — every pass that needs "the registers this
// instruction writes" read that one field. A bundle writes several, and a pass
// that kept reading only `instResult` would see a bundle that writes nothing:
// the allocator would hand an output's register to a value live across the
// statement, silently. `lirForEachInstDef` / `lirForEachInstUse` are the ONE
// statement of the question; every pass that asks it asks them.

namespace dss {

// The bundle's opcodes, as every target declares them (virtual ops with no
// encoding, like `arg`): the ordinary statement, and the `asm goto` statement,
// which ends its block (terminator kind `asm-goto`; its successors are its
// labels, then its fall-through). One spelling each, read by the producer, the
// verifier and the expansion.
inline constexpr std::string_view kLirAsmRegionMnemonic     = "asm_region";
inline constexpr std::string_view kLirAsmRegionGotoMnemonic = "asm_region_goto";

// How the statement accesses one bundle operand.
enum class LirAsmOperandRole : std::uint8_t {
    // Read, at the bundle's EARLY slot. (Also the ADDRESS register of a
    // memory operand — `"m"` / `"=m"`: the template reads the address even
    // when it writes the memory.)
    Use = 0,
    // Written, at the bundle's LATE slot — so it may share a register with an
    // input, which is GCC's documented default ("GCC may allocate the output
    // operand in the same register as an unrelated input operand").
    Def = 1,
    // Read at EARLY and written at LATE: a `"+r"` operand (and an input tied to
    // an output by a matching constraint), which is ONE register.
    UseDef = 2,
    // Written at the EARLY slot: an earlyclobber output (`"=&r"`), which may
    // share no register with any input.
    EarlyDef = 3,
};

[[nodiscard]] constexpr bool lirAsmRoleReads(LirAsmOperandRole r) noexcept {
    return r == LirAsmOperandRole::Use || r == LirAsmOperandRole::UseDef;
}
[[nodiscard]] constexpr bool lirAsmRoleWrites(LirAsmOperandRole r) noexcept {
    return r == LirAsmOperandRole::Def || r == LirAsmOperandRole::UseDef
        || r == LirAsmOperandRole::EarlyDef;
}
[[nodiscard]] constexpr bool lirAsmRoleWritesEarly(LirAsmOperandRole r) noexcept {
    return r == LirAsmOperandRole::EarlyDef;
}
[[nodiscard]] DSS_EXPORT char const* lirAsmRoleName(LirAsmOperandRole r) noexcept;

// One statement's body and operand bookkeeping. Immutable once pooled.
struct DSS_EXPORT LirAsmRegion {
    // One per bundle operand, in operand order.
    std::vector<LirAsmOperandRole> roles;
    // The VIRTUAL register the body names for bundle operand k — the register
    // the assembly engine was bound to. Distinct, one per operand. The body
    // may also name PHYSICAL registers (a pinned operand, a register the
    // template spells literally); those are copied through untouched.
    std::vector<LirReg> bodyRegs;
    // The body: exactly one function. Its blocks, in function order, are the
    // entry (block 0), one block per template label and per line after a
    // conditional branch, `exit`, and the `asm goto` stubs.
    Lir body;
    // "After the statement": a branch here leaves the template. Its content is
    // never emitted (it only exists because every LIR block is terminated).
    LirBlockId exit{};
    // `asm goto`: the body block standing for the bundle's successor k (the
    // goto labels, in order). The bundle's LAST successor — the fall-through —
    // is `exit`. Empty for a statement that is not `asm goto`.
    std::vector<LirBlockId> gotoTargets;
    // Per body block (function order): 1 iff the block's terminator is an
    // unconditional branch the TEMPLATE DID NOT WRITE — the lowering's
    // statement that the text falls into the next line (a label, or the end of
    // the statement). The expansion may realize such an edge as layout
    // adjacency; a branch the template wrote is always emitted.
    std::vector<std::uint8_t> syntheticFallthrough;
};

// ★ THE ONE STATEMENT OF A WELL-FORMED REGION, asked by the builder (which
// aborts — a producer contract), the verifier and the expansion (which
// report). `nullopt` = well-formed; otherwise what is wrong, in a sentence.
[[nodiscard]] DSS_EXPORT std::optional<std::string>
lirAsmRegionShapeDefect(LirAsmRegion const& region);

// Per slot, the widest operation width (`lirInstWidthBits`) any BODY
// instruction states while naming that slot's register — 0 for a slot the
// template never names. The access-width census's answer for a bundle operand:
// a bundle states no width of its own, its template does.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
lirAsmRegionSlotAccessWidthBits(LirAsmRegion const& region);

// ── THE ONE READER OF WHAT AN INSTRUCTION WRITES AND READS ─────────────────
//
// `f(LirReg reg, bool early)` for every register the instruction DEFINES: its
// `result`, and on a bundle every operand whose role writes it. `early` = the
// write happens at the instruction's EARLY slot (`kLirInstFlagEarlyClobberResult`
// on the result; `EarlyDef` on a bundle operand). Physical and virtual alike —
// a caller that tracks only virtual registers filters on `isPhysical`.
template <class F>
void lirForEachInstDef(Lir const& lir, LirInstId inst, F&& f) {
    LirReg const r = lir.instResult(inst);
    if (r.valid()) f(r, lirInstResultIsEarlyClobber(lir.instFlags(inst)));
    LirAsmRegion const* region = lir.instAsmRegion(inst);
    if (region == nullptr) return;
    auto const ops = lir.instOperands(inst);
    std::size_t const n =
        ops.size() < region->roles.size() ? ops.size() : region->roles.size();
    for (std::size_t k = 0; k < n; ++k) {
        if (ops[k].kind != LirOperandKind::Reg || !ops[k].reg.valid()) continue;
        LirAsmOperandRole const role = region->roles[k];
        if (!lirAsmRoleWrites(role)) continue;
        f(ops[k].reg, lirAsmRoleWritesEarly(role));
    }
}

// `f(LirReg reg)` for every register OPERAND the instruction READS: every Reg
// operand, except a bundle operand whose role only writes it.
template <class F>
void lirForEachInstUse(Lir const& lir, LirInstId inst, F&& f) {
    LirAsmRegion const* region = lir.instAsmRegion(inst);
    auto const ops = lir.instOperands(inst);
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].kind != LirOperandKind::Reg || !ops[k].reg.valid()) continue;
        if (region != nullptr && k < region->roles.size()
            && !lirAsmRoleReads(region->roles[k])) {
            continue;
        }
        f(ops[k].reg);
    }
}

// ── the expansion pass ─────────────────────────────────────────────────────
//
// Runs AFTER the peephole and BEFORE `materializeCallingConvention`: after,
// because allocation, two-address legalization and the peephole must see each
// statement as the one opaque instruction it is; before, because callconv's
// per-function CFI is keyed by `LirInstId` and a rebuild after it would
// renumber every CFI subject. The output carries NO bundle and an EMPTY region
// pool — the one pass that consumes a side structure rather than carrying it.
struct DSS_EXPORT LirAsmRegionExpansionResult {
    Lir         lir{};
    bool        ok = false;
    std::size_t regionsExpanded = 0;
};

[[nodiscard]] DSS_EXPORT LirAsmRegionExpansionResult
expandAsmRegions(Lir const& src, TargetSchema const& schema,
                 DiagnosticReporter& reporter);

// (The pipeline's paired check for this pass is `verifyLirAsmRegionExpansion`,
// `lir_verifier.hpp`: every other side structure carried as by any rebuild,
// and no bundle and no region left.)

} // namespace dss
