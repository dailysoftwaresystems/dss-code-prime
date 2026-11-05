#include "link/load_time_fixups.hpp"

#include "core/types/object_format_kind.hpp"   // TlsAccessModel, kTlsIndexReservedSymbolIdValue
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir.hpp"
#include "lir/lir_literal_pool.hpp"            // LirLiteralValue (a 64-bit addend)
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <array>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace dss::linker {

namespace {

void fail(DiagnosticReporter& reporter, std::string msg) {
    report(reporter, DiagnosticCode::K_NoMatchingObjectFormat, DiagnosticSeverity::Error,
           "load-time fix-up runner: " + std::move(msg));
}

}  // namespace

std::optional<AssembledFunction>
synthesizeLoadTimeFixupRunner(std::span<LoadTimeFixup const> fixups,
                              SymbolId                       symbol,
                              SymbolId                       threadTemplateBase,
                              std::size_t                    guardArgumentIndex,
                              std::int32_t                   guardValue,
                              TargetSchema const&            target,
                              ObjectFormatSchema const&      format,
                              DiagnosticReporter&            reporter) {
    // ── The calling convention the image's loader calls it with ───────────
    std::string const ccName = format.cCallingConvention().convention;
    auto const* cc = target.callingConventionByName(ccName);
    if (cc == nullptr) {
        fail(reporter, std::format("format '{}' names C calling convention '{}', which target "
                                   "'{}' does not declare.",
                                   format.name(), ccName, target.name()));
        return std::nullopt;
    }
    if (guardArgumentIndex >= cc->argGprs.size()) {
        fail(reporter, std::format("calling convention '{}' passes {} integer argument(s) in "
                                   "registers; the runner's guard reads argument {}.",
                                   ccName, cc->argGprs.size(), guardArgumentIndex));
        return std::nullopt;
    }
    auto const physical = [&](std::string_view name) -> std::optional<LirReg> {
        auto const ord = target.registerByName(name);
        if (!ord.has_value()) return std::nullopt;
        auto const* info = target.registerInfo(*ord);
        if (info == nullptr) return std::nullopt;
        return makePhysicalReg(*ord, static_cast<LirRegClass>(info->regClass));
    };
    auto const guardReg = physical(cc->argGprs[guardArgumentIndex]);
    if (!guardReg.has_value()) {
        fail(reporter, std::format("argument register '{}' of '{}' is not a register of target "
                                   "'{}'.",
                                   cc->argGprs[guardArgumentIndex], ccName, target.name()));
        return std::nullopt;
    }
    // Four scratch registers, every one caller-saved (the runner is a leaf that
    // saves nothing), none of them the guard's.
    std::vector<LirReg> scratch;
    for (auto const& name : cc->callerSaved) {
        auto const r = physical(name);
        if (!r.has_value() || r->regClass() != LirRegClass::GPR || *r == *guardReg) continue;
        scratch.push_back(*r);
        if (scratch.size() == 4) break;
    }
    if (scratch.size() < 4) {
        fail(reporter, std::format("calling convention '{}' of target '{}' names fewer than four "
                                   "caller-saved general registers besides argument {}.",
                                   ccName, target.name(), guardArgumentIndex));
        return std::nullopt;
    }
    LirReg const value = scratch[0];
    LirReg const addr  = scratch[1];
    LirReg const tp    = scratch[2];
    LirReg const index = scratch[3];

    // ── The instructions, every one the target's own ────────────────────
    struct Ops {
        std::optional<std::uint16_t> cmp, jcc, jmp, lea, load, store, add, mov, ret, tlsbase;
    } op;
    op.cmp     = target.opcodeByMnemonic("cmp");
    op.jcc     = target.opcodeByMnemonic("jcc");
    op.jmp     = target.opcodeByMnemonic("jmp");
    op.lea     = target.opcodeByMnemonic("lea");
    op.load    = target.opcodeByMnemonic("load");
    op.store   = target.opcodeByMnemonic("store");
    op.add     = target.opcodeByMnemonic("add");
    op.mov     = target.opcodeByMnemonic("mov");
    op.ret     = target.opcodeByMnemonic("ret");
    op.tlsbase = target.opcodeByMnemonic("tlsbase");
    if (!op.cmp || !op.jcc || !op.jmp || !op.lea || !op.load || !op.store || !op.add || !op.mov
        || !op.ret) {
        fail(reporter, std::format("target '{}' lacks one of the opcodes the runner is made of "
                                   "(cmp, jcc, jmp, lea, load, store, add, mov, ret).",
                                   target.name()));
        return std::nullopt;
    }
    bool needsThread = false;
    for (auto const& f : fixups) needsThread = needsThread || f.threadTemplate;
    std::optional<TlsAccessInfo> tls;
    if (needsThread) {
        if (!threadTemplateBase.valid()) {
            fail(reporter, "a fix-up writes a thread-local template item, and no symbol names "
                           "the template's first byte.");
            return std::nullopt;
        }
        tls = format.tlsAccess();
        if (!tls.has_value() || tls->model != TlsAccessModel::PeIndexed || !op.tlsbase) {
            // The one thread-block access this runner spells is the indexed
            // one (a thread-pointer slot array, indexed by the image's TLS
            // index); a format whose thread-local model is another has no
            // loader that copies a template before binding imports, so it
            // never asks for a template fix-up.
            fail(reporter, std::format("a fix-up writes a thread-local template, and format '{}' "
                                       "declares no 'pe-indexed' tlsAccess model (or target "
                                       "'{}' no 'tlsbase') to reach the running thread's copy.",
                                       format.name(), target.name()));
            return std::nullopt;
        }
        bool memOffsetShape = false;
        if (auto const* tb = target.opcodeInfo(*op.tlsbase)) {
            for (auto const& v : tb->encoding.variants) {
                if (v.operandKinds.size() == 1
                    && v.operandKinds[0] == OperandKindFilter::MemOffset) {
                    memOffsetShape = true;
                }
            }
        }
        if (!memOffsetShape
            || tls->baseDisplacement
                   > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            fail(reporter, std::format("target '{}' declares no memory-slot 'tlsbase' "
                                       "(mov r, seg:[disp32]) for the indexed thread-block "
                                       "access, or the format's slot displacement exceeds it.",
                                       target.name()));
            return std::nullopt;
        }
    }

    LirBuilder b{target};
    (void)b.addFunction(symbol);
    LirBlockId const entry = b.createBlock();
    LirBlockId const body  = b.createBlock();
    LirBlockId const done  = b.createBlock();

    // entry: run only when the guard argument equals the guard value — a
    // 32-bit compare, the width a DWORD argument is passed at.
    b.beginBlock(entry);
    {
        std::array<LirOperand, 2> const cmpOps{LirOperand::makeReg(*guardReg),
                                               LirOperand::makeImmInt32(guardValue)};
        (void)b.addInst(*op.cmp, InvalidLirReg, cmpOps, 0, kLirInstFlagWidth32);
        std::array<LirOperand, 2> const jccOps{LirOperand::makeBlockRef(done.v),
                                               LirOperand::makeBlockRef(body.v)};
        (void)b.addCondBr(*op.jcc, jccOps, done, body,
                          static_cast<std::uint32_t>(TargetCondCode::Ne));
    }

    b.beginBlock(body);
    for (auto const& f : fixups) {
        if (f.offsetInItem > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            fail(reporter, std::format("a fix-up lies {} bytes into its item, beyond the target's "
                                       "32-bit displacement.",
                                       f.offsetInItem));
            return std::nullopt;
        }
        auto const at = static_cast<std::int32_t>(f.offsetInItem);
        // value = *sourceSlot
        std::array<LirOperand, 1> const slotOps{LirOperand::makeSymbolRef(f.sourceSlot.v)};
        (void)b.addInst(*op.lea, value, slotOps);
        std::array<LirOperand, 3> const derefOps{LirOperand::makeReg(value),
                                                 LirOperand::makeMemBase(1),
                                                 LirOperand::makeMemOffset(0)};
        (void)b.addInst(*op.load, value, derefOps);
        // value += addend
        if (f.addend != 0) {
            if (f.addend >= std::numeric_limits<std::int32_t>::min()
                && f.addend <= std::numeric_limits<std::int32_t>::max()) {
                std::array<LirOperand, 3> const plusOps{
                    LirOperand::makeReg(value), LirOperand::makeMemBase(1),
                    LirOperand::makeMemOffset(static_cast<std::int32_t>(f.addend))};
                (void)b.addInst(*op.lea, value, plusOps);
            } else {
                LirLiteralValue wide;
                wide.core  = TypeKind::U64;
                wide.value = static_cast<std::uint64_t>(f.addend);
                std::array<LirOperand, 1> const wideOps{
                    LirOperand::makeLiteralIndex(b.literalPoolAdd(std::move(wide)))};
                (void)b.addInst(*op.mov, index, wideOps);
                std::array<LirOperand, 2> const addOps{LirOperand::makeReg(value),
                                                       LirOperand::makeReg(index)};
                (void)b.addInst(*op.add, value, addOps);
            }
        }
        // *(item + offset) = value — for a template item, the TEMPLATE, which
        // every thread started after this run copies: the template's first
        // byte plus the item's offset in a block.
        if (!f.threadTemplate) {
            std::array<LirOperand, 1> const itemOps{LirOperand::makeSymbolRef(f.item.v)};
            (void)b.addInst(*op.lea, addr, itemOps);
        } else {
            std::array<LirOperand, 1> const baseOps{
                LirOperand::makeSymbolRef(threadTemplateBase.v)};
            (void)b.addInst(*op.lea, addr, baseOps);
            std::array<LirOperand, 2> const inTemplateOps{LirOperand::makeReg(addr),
                                                          LirOperand::makeSymbolRef(f.item.v)};
            (void)b.addInst(*op.lea, addr, inTemplateOps);
        }
        std::array<LirOperand, 4> const storeOps{
            LirOperand::makeReg(value), LirOperand::makeReg(addr), LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(at)};
        (void)b.addInst(*op.store, InvalidLirReg, storeOps);
        if (!f.threadTemplate) continue;
        // ...and the RUNNING thread's copy, made before any import was bound:
        // block = tp[index], the item at its offset in the block.
        std::array<LirOperand, 1> const tpOps{
            LirOperand::makeMemOffset(static_cast<std::int32_t>(tls->baseDisplacement))};
        (void)b.addInst(*op.tlsbase, tp, tpOps, tls->segmentPrefixByte);
        std::array<LirOperand, 1> const idxAddrOps{
            LirOperand::makeSymbolRef(kTlsIndexReservedSymbolIdValue)};
        (void)b.addInst(*op.lea, index, idxAddrOps);
        std::array<LirOperand, 3> const idxOps{LirOperand::makeReg(index),
                                               LirOperand::makeMemBase(1),
                                               LirOperand::makeMemOffset(0)};
        (void)b.addInst(*op.load, index, idxOps, 0, kLirInstFlagWidth32);
        std::array<LirOperand, 4> const blockOps{LirOperand::makeReg(tp), LirOperand::makeReg(index),
                                                 LirOperand::makeMemBase(8),
                                                 LirOperand::makeMemOffset(0)};
        (void)b.addInst(*op.load, tp, blockOps);
        std::array<LirOperand, 2> const inBlockOps{LirOperand::makeReg(tp),
                                                   LirOperand::makeSymbolRef(f.item.v)};
        (void)b.addInst(*op.lea, tp, inBlockOps);
        std::array<LirOperand, 4> const copyOps{
            LirOperand::makeReg(value), LirOperand::makeReg(tp), LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(at)};
        (void)b.addInst(*op.store, InvalidLirReg, copyOps);
    }
    (void)b.addBr(*op.jmp, done);

    b.beginBlock(done);
    (void)b.addReturn(*op.ret, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    std::size_t const errorsBefore = reporter.errorCount();
    auto assembled = assemble(lir, target, lirToMir, reporter);
    if (reporter.errorCount() != errorsBefore || assembled.functions.empty()
        || assembled.functions[0].bytes.empty()) {
        if (reporter.errorCount() == errorsBefore) {
            fail(reporter, std::format("target '{}' assembled the runner to no bytes.",
                                       target.name()));
        }
        return std::nullopt;
    }
    AssembledFunction fn = std::move(assembled.functions[0]);
    fn.symbol = symbol;
    return fn;
}

}  // namespace dss::linker
