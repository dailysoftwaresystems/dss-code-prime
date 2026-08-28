#pragma once

// MIR-tier pointer-PROVENANCE + ESCAPE substrate.
//
// This is the precision input `mir_alias.hpp`'s `mirMayAlias` docblock listed
// as OUT OF SCOPE ("Cross-function escape analysis (address-taken
// propagation)"), and the one `D-OPT-MEMSSA-WALK-PAST-PRECISION` names as its
// OWN trigger: "`mirMayAlias` gains real precision (points-to / full TBAA /
// escape analysis) such that walk-past would fire often". It answers two
// questions about MIR pointer values, over MIR vocabulary alone — no
// source/target/format identity anywhere:
//
//   Q-ORIGIN  `originOf(p)`     — which storage can `p` name?
//                                 `Slot(a)`  : the local slot `a` and nothing
//                                              else (a is an `Alloca`);
//                                 `External` : provably NOT any local slot of
//                                              the enclosing activation;
//                                 `Unknown`  : top — may name anything.
//   Q-ESCAPE  `slotEscapes(a)`  — can the address of local slot `a` be
//                                 observed from outside this activation?
//
// and composes them into the ONE precision predicate consumers want:
//
//   `provablyDisjoint(p, q)` — the two pointers can NEVER name overlapping
//   storage. `mirMayAlias` Rule 2b is exactly this predicate; nothing else in
//   the substrate reads the analysis.
//
// ── WHY THE TWO HALVES ARE BOTH NEEDED ───────────────────────────────
// The pre-existing Rule 2 ("both operands are distinct `Alloca` defs ⇒ No")
// is the DEGENERATE case of Q-ORIGIN: it fires only when both pointers are
// the raw slot ids, which real C almost never presents. `int a[10]; a[i]`
// lowers to `Load(Gep(Alloca, i))`, so the pointer CSE and LICM actually
// probe with is a `Gep`, not the `Alloca` — Rule 2 cannot see through one
// level of address arithmetic and falls to Rule 7 (Maybe). Q-ORIGIN walks the
// provenance chain, so the Gep is `Slot(a)` and Rule 2 generalizes.
//
// Q-ESCAPE is what buys the case that actually dominates real code: a local
// ARRAY or STRUCT (the shapes Mem2Reg cannot promote, so their loads and
// stores survive to CSE/LICM) versus an OPAQUE pointer — a parameter, a
// global, a loaded pointer, a callee's return value. Today that pair is
// Rule 7 `Maybe` and every Load through the local is treated as clobbered by
// every Store through the parameter. If the local's address never LEAVES the
// function, no opaque pointer can name it, and the pair is `No`.
//
// ── THE SOUNDNESS ARGUMENT FOR `External`, STATED ONCE ───────────────
// A value is classified `External` only when its own opcode makes it
// impossible for it to hold the address of a NON-ESCAPED local slot `a` of
// this activation:
//   `Alloca`-free by construction:
//     * `GlobalAddr` / `BlockAddress` — a link-time symbol or code address; a
//       stack slot has neither.
//     * `Const` — a compile-time literal; it cannot be a runtime stack address.
//   Requires `a`'s address to have LEFT the activation, which is exactly what
//   `slotEscapes(a)` reports:
//     * `Arg` — the caller supplied it, and a caller of a FRESH activation
//       cannot hold that activation's slot address unless it was passed one
//       (recursion included: passing it is an escape).
//     * `Load` / `AtomicLoad` — read back out of memory, so `&a` must first
//       have been WRITTEN to memory (an escape).
//     * `Call` — the callee returned it, so the callee must first have RECEIVED
//       `&a` (an escape).
//   Every value whose result type is not `Ptr`/`Ref` is also `External`: it
//   does not name storage. An integer that HOLDS a slot address can only be
//   produced by `PtrToInt`, and `PtrToInt` is an ESCAPING use position (see
//   `mirPointerUseKind`), so the slot is already escaped before the integer
//   exists. ★ This is why `IntToPtr` is deliberately NOT on the `External`
//   list — it is `Unknown`. LLVM/GCC would classify an `inttoptr` result as
//   unable to reach a non-captured alloca; this substrate declines to rest a
//   `No` verdict on that UB assumption and pays the (rare) precision instead.
//
// ── THE FRAME-OBSERVING ESCAPE HATCH ─────────────────────────────────
// Every argument above reasons about SSA dataflow. A handful of opcodes can
// name frame storage WITHOUT any SSA edge from the `Alloca` — an assembly
// template that reads the frame pointer, the variadic frame leaves, a
// stack-watermark save, an SEH funclet recovering a parent frame slot. For a
// function containing ANY of them (`mirOpcodeObservesFrame`), EVERY slot is
// marked escaped, which returns that function to exactly today's precision.
//
// ── AGNOSTIC + FAIL-LOUD ─────────────────────────────────────────────
// Pure MIR-graph + opcode-table math: no language, architecture or object
// format is consulted, and the two classification tables are `constexpr`
// functions over `MirOpcode` alone. Both tables are WHITELISTS whose default
// arm is the conservative answer (`Unknown` for origin, `Escapes` for a use),
// so an opcode added to the enum without touching this file loses precision —
// it can never gain an unsound `No`. Queries fail loud on a stale module (the
// `MirMemoryClobbers::checkModule_` discipline) and the fixpoint fails loud on
// a step-cap overflow rather than returning a half-converged lattice.

#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::opt::analysis {

// What storage a pointer SSA value can name. `Unresolved` is the fixpoint's
// bottom and is INTERNAL — `originOf` never returns it (an unreached value is
// reported as `Unknown`, the conservative top).
enum class MirPointerOriginKind : std::uint8_t {
    Unresolved,
    Slot,
    External,
    Unknown,
};

struct MirPointerOrigin {
    MirPointerOriginKind kind = MirPointerOriginKind::Unknown;
    MirInstId            slot{};   // valid iff kind == Slot

    [[nodiscard]] constexpr bool
    operator==(MirPointerOrigin const&) const noexcept = default;
};

// How an instruction's own opcode determines its result's origin, BEFORE any
// operand is consulted. `BaseDerived` / `Merged` are the two propagating
// shapes: the result names whatever its provenance input names.
enum class MirPointerRootClass : std::uint8_t {
    Slot,         // `Alloca` — the value IS a local slot's address
    External,     // provably not a local slot of this activation (see docblock)
    BaseDerived,  // origin = origin(operands[0])
    Merged,       // origin = join over `phiIncomings`
    Unknown,      // top
};

// ★ THE ORIGIN TABLE. Default arm = `Unknown`, the conservative answer.
[[nodiscard]] constexpr MirPointerRootClass
mirPointerRootClass(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Alloca:
            return MirPointerRootClass::Slot;

        // The `External` whitelist — each entry justified in the docblock's
        // soundness argument. Adding an entry here is a SOUNDNESS edit.
        case MirOpcode::Arg:
        case MirOpcode::GlobalAddr:
        case MirOpcode::BlockAddress:
        case MirOpcode::Const:
        case MirOpcode::Load:
        case MirOpcode::AtomicLoad:
        case MirOpcode::Call:
            return MirPointerRootClass::External;

        // Address arithmetic and pointer re-typing preserve provenance. Both
        // carry their base at operands[0] (`Gep` = [base, offset...]).
        case MirOpcode::Gep:
        case MirOpcode::Bitcast:
            return MirPointerRootClass::BaseDerived;

        case MirOpcode::Phi:
            return MirPointerRootClass::Merged;

        default:
            return MirPointerRootClass::Unknown;
    }
}

// What one USE of a pointer value does to that value's storage.
enum class MirPointerUseKind : std::uint8_t {
    Propagates,  // the user's RESULT names the same storage
    Contained,   // the storage is read/written THROUGH the pointer; the
                 // ADDRESS itself does not leave
    Escapes,     // the address may become observable outside this activation
};

// ★ THE USE TABLE. Default arm = `Escapes`, the conservative answer — a
// `Return` of a slot address, a `Call` argument, an `InsertValue` into an
// aggregate, a `PtrToInt`, and every opcode this file has never heard of all
// land there. Only the positions listed below are proven not to leak.
//
// `Phi` is `Propagates` at every position, but a Phi's inputs live in the phi
// pool rather than the operand pool, so callers must enumerate them with
// `Mir::phiIncomings` and never with `Mir::instOperands`.
[[nodiscard]] constexpr MirPointerUseKind
mirPointerUseKind(MirOpcode userOp, std::size_t operandIndex) noexcept {
    switch (userOp) {
        case MirOpcode::Gep:      // [base, offset...]
        case MirOpcode::Bitcast:  // [value]
            return operandIndex == 0 ? MirPointerUseKind::Propagates
                                     : MirPointerUseKind::Escapes;
        case MirOpcode::Phi:
            return MirPointerUseKind::Propagates;

        // Dereference positions. The `Volatile` flag is irrelevant here: a
        // volatile access through `&a` still does not publish `&a`.
        case MirOpcode::Load:        // [ptr]
        case MirOpcode::AtomicLoad:  // [ptr]
        case MirOpcode::AtomicCas:   // [ptr, comparand, newval]
            return operandIndex == 0 ? MirPointerUseKind::Contained
                                     : MirPointerUseKind::Escapes;
        case MirOpcode::Store:        // [value, ptr]
        case MirOpcode::AtomicStore:  // [value, ptr]
            return operandIndex == 1 ? MirPointerUseKind::Contained
                                     : MirPointerUseKind::Escapes;

        // A pointer COMPARISON consumes the address and yields a Bool. The
        // address itself is not published by the comparison.
        case MirOpcode::ICmpEq:  case MirOpcode::ICmpNe:
        case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
        case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
        case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
        case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge:
            return MirPointerUseKind::Contained;

        default:
            return MirPointerUseKind::Escapes;
    }
}

// Opcodes that can name FRAME storage with no SSA edge from the `Alloca`
// whose storage they reach. A function containing any of these has EVERY
// slot marked escaped — i.e. it keeps exactly today's precision.
//
// ★ `InlineAsm`/`InlineAsmGoto` are the load-bearing entries and the reason
// this predicate exists at all: the alias substrate already treats a template
// as an opaque memory clobber, but a template can also simply READ THE FRAME
// POINTER and compute any slot address in the frame — a capability no operand
// scan can observe. The variadic leaves, `StackSave`/`StackRestore` and
// `RecoverParentFrameSlot` are frame addresses by definition, and the SEH
// region ops bracket code whose funclets address the parent frame.
[[nodiscard]] constexpr bool mirOpcodeObservesFrame(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::InlineAsm:
        case MirOpcode::InlineAsmGoto:
        case MirOpcode::VaRegSaveAreaAddr:
        case MirOpcode::VaOverflowArgAreaAddr:
        case MirOpcode::VaHomeArgAreaAddr:
        case MirOpcode::RecoverParentFrameSlot:
        case MirOpcode::StackSave:
        case MirOpcode::StackRestore:
        case MirOpcode::SehTryBegin:
        case MirOpcode::SehTryEnd:
        case MirOpcode::SehFilterReturn:
        case MirOpcode::SehExceptionCode:
        case MirOpcode::SehExceptionInfo:
            return true;
        default:
            return false;
    }
}

// ── The CSE-substitution stability lemma, as a checkable predicate ────
//
// `mirAliasProbeSubstitutionPreservesClobberVerdict` (mir_alias.hpp) licenses a
// value-numbering consumer to hand the alias gate its CANONICAL pointer id
// instead of the raw one. At Rules 1..7 that licence is a TYPE-level argument
// (equal opcode + equal TypeId + not `Alloca`). Rule 2b is not a type test —
// it reads `originOf` — so the licence needs one more property:
//
//   LEMMA. For every pair of instructions a value-numbering pass may merge,
//   the two origins are EQUAL.
//
//   PROOF. A merge requires equal keys, and a key carries the opcode, the
//   result TypeId, the payload, and the operand list RESOLVED THROUGH THE
//   REDIRECT MAP. `originOf` is a function of (opcode, origin of the
//   provenance input) — by this very predicate — so induction over the
//   (acyclic) redirect map gives equal origins, PROVIDED every mergeable
//   opcode has that shape. The two opcodes whose origin depends on an id
//   RATHER than on an operand's origin are `Alloca` (origin = its own
//   identity) and `Phi` (origin = a join over the phi pool, which no operand
//   list carries); both are excluded from merge candidacy. ∎
//
// This predicate IS the lemma's premise, stated so a test can sweep the whole
// `MirOpcode` enum against the consumer's own candidacy gate instead of
// re-arguing the induction at every edit.
[[nodiscard]] constexpr bool
mirPointerOriginIsOperandDetermined(MirOpcode op) noexcept {
    switch (mirPointerRootClass(op)) {
        case MirPointerRootClass::Slot:    return false;  // its own identity
        case MirPointerRootClass::Merged:  return false;  // the phi pool
        case MirPointerRootClass::External:
        case MirPointerRootClass::BaseDerived:
        case MirPointerRootClass::Unknown:
            return true;
    }
    return false;
}

// The analysis. ONE instance per pass invocation, built while the module is
// frozen — the same scope/lifetime contract as `MirMemoryClobbers`, whose
// constructor builds one and threads it into every alias probe.
class MirPointerEscape {
public:
    explicit MirPointerEscape(Mir const& mir)
        : mir_(mir)
        , moduleIdV_(mir.id().v)
        , instCount_(static_cast<std::uint32_t>(mir.instCount()))
        , blockCount_(static_cast<std::uint32_t>(mir.blockCount())) {
        // The two tables must agree about which opcodes propagate provenance:
        // an opcode whose RESULT is derived from operand 0 must also treat
        // operand 0 as a propagating USE, or the forward escape scan would
        // stop at the very edge the backward origin walk crossed — a slot
        // whose Gep-derived address is passed to a call would read as
        // non-escaped. Pin the correspondence at compile time.
        static_assert(mirPointerUseKind(MirOpcode::Gep, 0)
                          == MirPointerUseKind::Propagates
                      && mirPointerRootClass(MirOpcode::Gep)
                          == MirPointerRootClass::BaseDerived,
                      "Gep must propagate provenance in BOTH tables");
        static_assert(mirPointerUseKind(MirOpcode::Bitcast, 0)
                          == MirPointerUseKind::Propagates
                      && mirPointerRootClass(MirOpcode::Bitcast)
                          == MirPointerRootClass::BaseDerived,
                      "Bitcast must propagate provenance in BOTH tables");
        static_assert(mirPointerUseKind(MirOpcode::Phi, 0)
                          == MirPointerUseKind::Propagates
                      && mirPointerRootClass(MirOpcode::Phi)
                          == MirPointerRootClass::Merged,
                      "Phi must propagate provenance in BOTH tables");

        kind_.assign(instCount_, static_cast<std::uint8_t>(
                                     MirPointerOriginKind::Unresolved));
        funcAllSlotsEscape_.assign(mir.funcCount(), 0u);
        funcSlots_.assign(mir.funcCount(), {});

        seedAndCollect_();
        markSlotDerived_();
        solveOrigins_();
        scanEscapes_();
    }

    MirPointerEscape(MirPointerEscape const&)            = delete;
    MirPointerEscape& operator=(MirPointerEscape const&) = delete;

    // The origin of `p`. Never returns `Unresolved`: a value the fixpoint
    // never reached (an instruction outside every block, or an id past the
    // arena) is reported as `Unknown`, the conservative top.
    [[nodiscard]] MirPointerOrigin originOf(MirInstId p) const {
        checkModule_();
        if (!p.valid() || p.v >= instCount_) return {};
        auto const k = static_cast<MirPointerOriginKind>(kind_[p.v]);
        if (k == MirPointerOriginKind::Slot) {
            auto const it = slotOf_.find(p.v);
            if (it == slotOf_.end()) {
                std::fprintf(stderr,
                    "dss::opt::analysis::MirPointerEscape fatal: value v=%u is "
                    "classified Slot with no recorded slot id — the origin "
                    "lattice and its slot side-table disagree.\n", p.v);
                std::abort();
            }
            return {MirPointerOriginKind::Slot,
                    MirInstId{it->second, moduleIdV_}};
        }
        if (k == MirPointerOriginKind::External) {
            return {MirPointerOriginKind::External, MirInstId{}};
        }
        return {MirPointerOriginKind::Unknown, MirInstId{}};
    }

    // Can the address of local slot `slot` be observed from outside the
    // enclosing activation? A non-`Alloca` id is reported as escaped — the
    // conservative answer for a caller that misidentified a slot.
    [[nodiscard]] bool slotEscapes(MirInstId slot) const {
        checkModule_();
        if (!slot.valid() || slot.v >= instCount_) return true;
        if (mir_.instOpcode(slot) != MirOpcode::Alloca) return true;
        if (escapedSlots_.count(slot.v)) return true;
        std::uint32_t const f = funcSlotOf_(slot);
        return f < funcAllSlotsEscape_.size() && funcAllSlotsEscape_[f] != 0u;
    }

    // ★ THE precision predicate — `mirMayAlias` Rule 2b, and the only thing
    // any consumer needs from this analysis.
    //
    // True iff `a` and `b` can NEVER name overlapping storage. Two arms:
    //
    //   (i)  DISTINCT SLOTS. Two different `Alloca`s are two different
    //        objects, in this activation or in any two activations, whether or
    //        not either escapes. This is the pre-existing Rule 2 generalized
    //        through address arithmetic — Rule 2 compares the raw slot ids,
    //        this compares PROVENANCE, so `Gep(a,i)` vs `Gep(b,j)` is caught
    //        where Rule 2 saw only two `Gep`s. ⚠ It inherits Rule 2's standing
    //        assumption that address arithmetic stays INSIDE its object: a Gep
    //        that runs off the end of `a` into `b` is C undefined behaviour,
    //        and the reference compilers this project is measured against
    //        (gcc / clang / MSVC) all draw the same `No` here.
    //
    //   (ii) NON-ESCAPED SLOT vs EXTERNAL. If `a`'s address never leaves the
    //        activation, nothing outside can name it — see the docblock's
    //        soundness argument for each `External` origin.
    //
    // Every other origin combination is `false` (may alias), including any
    // pair involving `Unknown`.
    [[nodiscard]] bool provablyDisjoint(MirInstId a, MirInstId b) const {
        checkModule_();
        if (!a.valid() || !b.valid()) return false;
        if (a.v == b.v) return false;                       // the same pointer
        MirPointerOrigin const oa = originOf(a);
        MirPointerOrigin const ob = originOf(b);
        using K = MirPointerOriginKind;
        if (oa.kind == K::Slot && ob.kind == K::Slot) {      // arm (i)
            return oa.slot.v != ob.slot.v;
        }
        if (oa.kind == K::Slot && ob.kind == K::External) {  // arm (ii)
            return !slotEscapes(oa.slot);
        }
        if (ob.kind == K::Slot && oa.kind == K::External) {  // arm (ii), mirrored
            return !slotEscapes(ob.slot);
        }
        return false;
    }

    // ── Instruments (measurement, not behaviour) ─────────────────────
    // A precision claim about this analysis must be MEASURED on the module in
    // hand, never asserted from the shape of the source. These are what the
    // tests and any future profile read.
    [[nodiscard]] std::size_t slotCount() const noexcept {
        std::size_t n = 0;
        for (auto const& slots : funcSlots_) n += slots.size();
        return n;
    }
    [[nodiscard]] std::size_t escapingSlotCount() const {
        std::size_t n = 0;
        for (auto const& slots : funcSlots_) {
            for (MirInstId const s : slots) if (slotEscapes(s)) ++n;
        }
        return n;
    }
    [[nodiscard]] std::size_t frameObservingFunctionCount() const noexcept {
        std::size_t n = 0;
        for (std::uint8_t const f : funcAllSlotsEscape_) if (f) ++n;
        return n;
    }

private:
    // The optimizer mints a fresh MirModuleId per rebuild and reassigns the
    // very variable this object's reference binds, so a use-after-finish()
    // flips the id and fails loud here instead of indexing a foreign module.
    void checkModule_() const {
        if (mir_.id().v != moduleIdV_
            || static_cast<std::uint32_t>(mir_.instCount()) != instCount_
            || static_cast<std::uint32_t>(mir_.blockCount()) != blockCount_) {
            std::fprintf(stderr,
                "dss::opt::analysis::MirPointerEscape fatal: module changed "
                "under the analysis (built id=%u insts=%u blocks=%u, now id=%u "
                "insts=%zu blocks=%zu) — use-after-finish() / stale-pass "
                "object.\n",
                moduleIdV_, instCount_, blockCount_, mir_.id().v,
                mir_.instCount(), mir_.blockCount());
            std::abort();
        }
    }

    [[nodiscard]] std::uint32_t funcSlotOf_(MirInstId inst) const {
        MirBlockId const b = mir_.instBlock(inst);
        if (!b.valid()) return static_cast<std::uint32_t>(-1);
        return mir_.blockFunc(b).v;
    }

    void setKind_(std::uint32_t v, MirPointerOriginKind k) {
        kind_[v] = static_cast<std::uint8_t>(k);
    }
    [[nodiscard]] MirPointerOriginKind kindAt_(std::uint32_t v) const {
        return static_cast<MirPointerOriginKind>(kind_[v]);
    }

    // Phase 1 — ONE linear walk of the module: classify every instruction's
    // ROOT origin, remember each function's slots, record the frame-observing
    // functions, and build the (sparse) propagation use-lists the fixpoint
    // needs.
    void seedAndCollect_() {
        std::size_t const nf = mir_.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f  = mir_.funcAt(fi);
            std::uint32_t const nb = mir_.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = mir_.funcBlockAt(f, bi);
                std::uint32_t const ninst = mir_.blockInstCount(b);
                for (std::uint32_t i = 0; i < ninst; ++i) {
                    MirInstId const id = mir_.blockInstAt(b, i);
                    MirOpcode const op = mir_.instOpcode(id);

                    if (mirOpcodeObservesFrame(op) && f.v < funcAllSlotsEscape_.size()) {
                        funcAllSlotsEscape_[f.v] = 1u;
                    }
                    if (op == MirOpcode::Alloca && f.v < funcSlots_.size()) {
                        funcSlots_[f.v].push_back(id);
                    }
                    if (id.v >= instCount_) continue;

                    switch (mirPointerRootClass(op)) {
                        case MirPointerRootClass::Slot:
                            setKind_(id.v, MirPointerOriginKind::Slot);
                            slotOf_[id.v] = id.v;
                            break;
                        case MirPointerRootClass::External:
                            setKind_(id.v, MirPointerOriginKind::External);
                            break;
                        case MirPointerRootClass::Unknown:
                            setKind_(id.v, MirPointerOriginKind::Unknown);
                            break;
                        case MirPointerRootClass::BaseDerived: {
                            auto const ops = mir_.instOperands(id);
                            if (ops.empty()) {   // arity is a verifier property
                                setKind_(id.v, MirPointerOriginKind::Unknown);
                                break;
                            }
                            propUsers_[ops[0].v].push_back(id);
                            ++propEdgeCount_;
                            derived_.push_back(id);
                            break;
                        }
                        case MirPointerRootClass::Merged: {
                            for (auto const& inc : mir_.phiIncomings(id)) {
                                if (!inc.value.valid()) continue;
                                propUsers_[inc.value.v].push_back(id);
                                ++propEdgeCount_;
                            }
                            derived_.push_back(id);
                            break;
                        }
                    }
                }
            }
        }
    }

    // Phase 1b — the SLOT-DERIVED set: every value forward-reachable from an
    // `Alloca` over the propagation edges.
    //
    // ★ THIS IS WHAT KEEPS THE `Unknown` ARM OF THE ESCAPE SCAN FROM EATING
    // THE WHOLE ANALYSIS. `Unknown` is the lattice's top, so it covers two
    // completely different populations: values that genuinely MIX slot
    // provenance (a `Phi` over two slots, or over a slot and a parameter) and
    // values that simply are not addresses at all (every `Add`, every
    // comparison, every integer). Treating an escaping `Unknown` as "some
    // unnameable slot left the activation" is the only sound reading of the
    // FIRST population — but applied to the SECOND it would mark every slot in
    // every function that ever passes an integer to a call, i.e. all of them.
    //
    // Membership here separates the two, and it is SOUND in the strong
    // direction: a value outside this set cannot hold the address of a
    // non-escaped slot at all. Any route from `&a` to a value that is not
    // derived from it by address arithmetic must pass THROUGH memory, a call,
    // an integer conversion, or an aggregate — and every one of those is an
    // `Escapes` position in `mirPointerUseKind`, so `a` is already escaped
    // before such a value can exist.
    void markSlotDerived_() {
        std::vector<MirInstId> work;
        for (auto const& slots : funcSlots_) {
            for (MirInstId const s : slots) {
                if (s.v < instCount_ && slotDerived_.insert(s.v).second) {
                    work.push_back(s);
                }
            }
        }
        while (!work.empty()) {
            MirInstId const v = work.back();
            work.pop_back();
            auto const it = propUsers_.find(v.v);
            if (it == propUsers_.end()) continue;
            for (MirInstId const u : it->second) {
                if (u.v < instCount_ && slotDerived_.insert(u.v).second) {
                    work.push_back(u);
                }
            }
        }
    }

    // Phase 2 — the origin fixpoint over the propagation graph. The lattice is
    // Unresolved < {Slot(a), External} < Unknown (height 3) and every transfer
    // is a monotone join, so the worklist converges; the step cap turns a
    // non-terminating graph into a loud abort instead of a hang.
    void solveOrigins_() {
        std::vector<MirInstId> work = derived_;
        // Each node rises at most 3 times through the lattice and each rise
        // re-queues its propagation users, so pops are bounded by
        // |derived| + 3·|edges|; the cap sits comfortably above that and turns
        // a non-monotone transfer into a loud abort instead of a hang.
        std::uint64_t const stepCap =
            4ull * (static_cast<std::uint64_t>(instCount_) + propEdgeCount_) + 64ull;
        std::uint64_t steps = 0;
        while (!work.empty()) {
            if (++steps > stepCap) {
                std::fprintf(stderr,
                    "dss::opt::analysis::MirPointerEscape fatal: step-cap "
                    "exceeded solving the origin lattice (%llu steps over %u "
                    "instructions) — the propagation graph is not converging, "
                    "which means a transfer function stopped being monotone.\n",
                    static_cast<unsigned long long>(steps), instCount_);
                std::abort();
            }
            MirInstId const id = work.back();
            work.pop_back();
            if (id.v >= instCount_) continue;

            MirPointerOrigin joined{MirPointerOriginKind::Unresolved, {}};
            if (mirPointerRootClass(mir_.instOpcode(id))
                == MirPointerRootClass::Merged) {
                for (auto const& inc : mir_.phiIncomings(id)) {
                    joined = join_(joined, originAt_(inc.value));
                }
            } else {
                auto const ops = mir_.instOperands(id);
                joined = ops.empty() ? MirPointerOrigin{MirPointerOriginKind::Unknown, {}}
                                     : originAt_(ops[0]);
            }
            // A still-Unresolved input leaves this node Unresolved; the node
            // is re-queued when that input settles.
            if (joined.kind == MirPointerOriginKind::Unresolved) continue;
            if (originAt_(id) == joined) continue;

            setKind_(id.v, joined.kind);
            if (joined.kind == MirPointerOriginKind::Slot) {
                slotOf_[id.v] = joined.slot.v;
            } else {
                slotOf_.erase(id.v);
            }
            auto const it = propUsers_.find(id.v);
            if (it == propUsers_.end()) continue;
            for (MirInstId const u : it->second) work.push_back(u);
        }
    }

    // The raw lattice read, WITHOUT `checkModule_` (the fixpoint runs inside
    // the constructor, where the guard's invariants are still being built).
    [[nodiscard]] MirPointerOrigin originAt_(MirInstId p) const {
        if (!p.valid() || p.v >= instCount_) {
            return {MirPointerOriginKind::Unknown, {}};
        }
        MirPointerOriginKind const k = kindAt_(p.v);
        if (k != MirPointerOriginKind::Slot) return {k, MirInstId{}};
        auto const it = slotOf_.find(p.v);
        if (it == slotOf_.end()) return {MirPointerOriginKind::Unknown, {}};
        return {MirPointerOriginKind::Slot, MirInstId{it->second, moduleIdV_}};
    }

    [[nodiscard]] static MirPointerOrigin
    join_(MirPointerOrigin a, MirPointerOrigin b) noexcept {
        using K = MirPointerOriginKind;
        if (a.kind == K::Unresolved) return b;
        if (b.kind == K::Unresolved) return a;
        if (a == b) return a;
        return {K::Unknown, MirInstId{}};
    }

    // Phase 3 — the forward escape scan. ONE linear pass: every instruction,
    // every operand position, through the ONE use table.
    void scanEscapes_() {
        std::size_t const nf = mir_.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f  = mir_.funcAt(fi);
            std::uint32_t const nb = mir_.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = mir_.funcBlockAt(f, bi);
                std::uint32_t const ninst = mir_.blockInstCount(b);
                for (std::uint32_t i = 0; i < ninst; ++i) {
                    MirInstId const u  = mir_.blockInstAt(b, i);
                    MirOpcode const op = mir_.instOpcode(u);
                    // A Phi's inputs are all `Propagates` and live in the phi
                    // pool — `instOperands` does not address them.
                    if (mirPointerRootClass(op) == MirPointerRootClass::Merged) {
                        continue;
                    }
                    auto const ops = mir_.instOperands(u);
                    for (std::size_t oi = 0; oi < ops.size(); ++oi) {
                        if (mirPointerUseKind(op, oi) != MirPointerUseKind::Escapes) {
                            continue;
                        }
                        MirPointerOrigin const o = originAt_(ops[oi]);
                        if (o.kind == MirPointerOriginKind::Slot) {
                            escapedSlots_.insert(o.slot.v);
                        } else if (o.kind != MirPointerOriginKind::External
                                   && slotDerived_.count(ops[oi].v)
                                   && f.v < funcAllSlotsEscape_.size()) {
                            // An address that IS alloca-derived but that the
                            // lattice cannot pin to ONE slot has left the
                            // activation. It may carry any slot of this
                            // function and we cannot say which, so every slot
                            // here escapes — the only sound reading. See
                            // `markSlotDerived_` for why the membership test is
                            // what keeps this arm from firing on every integer.
                            funcAllSlotsEscape_[f.v] = 1u;
                        }
                    }
                }
            }
        }
    }

    Mir const&          mir_;
    std::uint32_t const moduleIdV_;
    std::uint32_t const instCount_;
    std::uint32_t const blockCount_;

    // One byte per instruction — the dense half. Everything else is sparse:
    // only alloca-derived values carry a slot id, and only the (few) values
    // feeding a Gep/Bitcast base or a Phi incoming carry a use-list.
    std::vector<std::uint8_t>                                     kind_;
    std::unordered_map<std::uint32_t, std::uint32_t>              slotOf_;
    std::unordered_map<std::uint32_t, std::vector<MirInstId>>     propUsers_;
    std::uint64_t                                                 propEdgeCount_ = 0;
    std::vector<MirInstId>                                        derived_;
    std::unordered_set<std::uint32_t>                             slotDerived_;
    std::unordered_set<std::uint32_t>                             escapedSlots_;
    std::vector<std::vector<MirInstId>>                           funcSlots_;
    std::vector<std::uint8_t>                                     funcAllSlotsEscape_;
};

} // namespace dss::opt::analysis
