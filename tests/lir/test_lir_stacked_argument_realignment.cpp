// A CALLER PLACES EVERY STACK-PASSED ARGUMENT WHERE THE CALLEE'S ABI EXPECTS IT, AT ITS DECLARED ALIGNMENT,
// WHATEVER THE STACK POINTER'S OWN ALIGNMENT IS AT THE CALL (P69 round 1, lane `cs`, fold f26c, main's ledger 495).
//
// THE DEFECT. Under SysV AMD64 a MEMORY-class aggregate argument travels as its bytes on the stack, begun on its own
// alignment (psABI 3.2.3), and gcc's and clang's callees use those bytes in place; the stack pointer is promised only
// the convention's `stackAlignment` (16) at a call. DSS's outgoing cursor placed a 32-, 64- or 256-aligned aggregate
// at its alignment RELATIVE to SP and nothing aligned SP, so the bytes sat misaligned in absolute terms: MEASURED by
// the native witness in run f26c-s22 (`tests/mir/test_sysv_memory_class_reference_link.cpp`, the DSS caller with a
// gcc or clang callee: every value right, the callee's `&s` misplaced).
//
// THE FIX, AND ITS ONE HOME. A function a call of which passes such an argument realigns its stack pointer ONCE, in
// its entry block after its parameters are received: `w = sp_copy SP; d = w & (N-1); sub_sp_reg SP, d`. The
// `sub_sp_reg` is the VLA frame model's own signal, so the function runs on THAT frame -- the fixed frame off the
// frame pointer, the frame pointer saved and kept out of allocation, the CFA rule moved to it, `SP <- FP` before the
// epilogue -- with no second frame model. `lir_callconv` sizes the function's outgoing area to a multiple of N
// (`LirFuncLocalAlignment::stackedArgAlignBytes`), so the dynamic frame's biased capture `w = SP + outgoing` is
// congruent to SP mod N; a VLA in such a function descends by a multiple of N; `va_start` in it addresses its areas
// off the frame pointer (`LirCallconv.VlaVariadicFunctionAddressesItsVaAreasOffTheFramePointer`).
//
// THE UNIVERSE (ledger 495), and what this file pins of it: a stacked argument aligned past the stack exists only
// where a convention passes a MEMORY-class aggregate by its bytes -- `aggregateMemoryArgument: stack` (sysv_amd64,
// sysv_amd64_darwin). Under `callerCopy` (ms_x64, aapcs64, apple_arm64) the argument is a pointer to a copy the
// caller places as a local, which the over-aligned-local frame already aligns, so those conventions are pinned
// UNTOUCHED (no row, no realignment). A SCALAR stacked past 16 does not exist in C on these conventions: the widest
// scalar alignment (`long double`, `__int128`) is 16, the stack's own. No DSS target is 32-bit x86, so cl's 32-bit
// cell is not a DSS cell. The arms: (A) the row and the sequence over hand-built MIR -- the pin main asked for in
// ledger 497, a function with NO over-aligned local passing a 64-aligned aggregate on the stack, and its control at
// the stack's own alignment; (B) through the C front end, wide-call lowering, allocation and callconv, for N = 32,
// 64 and 256: the outgoing area a multiple of N, the frame pointer saved, the CFA moved to it and back, the capture
// biased by the outgoing area; (C) a VLA in such a function descends by a multiple of N; (D) every `callerCopy`
// convention untouched; (E) what DSS's own LINKED image says of the frame -- its unwind section, decoded by the
// reader DSS uses for a foreign object's, on the ELF and Mach-O executables of both `stack` conventions, debug and
// release (main's ledger 510; read first with llvm-dwarfdump 19 --eh-frame). The runtime proof is the corpus example
// `examples/c/a_stacked_argument_is_placed_on_its_own_alignment` (five targets, debug and release) and the native
// witness above, against gcc and clang.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run through ctest and never as a bare
// `.exe`.

#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "link/format/dwarf_cfi.hpp"
#include "link/format/dwarf_cfi_decode.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "lowered_lir_fixture.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::string inventory(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
        out += "\n";
    }
    return out.empty() ? std::string{"(nothing reported)"} : out;
}

// Every instruction of function `fi`, in block order.
[[nodiscard]] std::vector<LirInstId> instsOf(Lir const& lir, std::uint32_t fi) {
    std::vector<LirInstId> out;
    LirFuncId const fn = lir.funcAt(fi);
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
        LirBlockId const b = lir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < lir.blockInstCount(b); ++i) out.push_back(lir.blockInstAt(b, i));
    }
    return out;
}

[[nodiscard]] bool isPhysical(LirOperand const& o, std::uint16_t ord) {
    return o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0 && o.reg.id == ord;
}

// The defining instruction of every VIRTUAL register of function `fi` (MIR->LIR's output is SSA over vregs).
[[nodiscard]] std::map<std::pair<int, std::uint32_t>, LirInstId> vregDefs(Lir const& lir, std::uint32_t fi) {
    std::map<std::pair<int, std::uint32_t>, LirInstId> defs;
    for (LirInstId const inst : instsOf(lir, fi)) {
        LirReg const r = lir.instResult(inst);
        if (r.valid() && r.isPhysical == 0)
            defs.emplace(std::pair{static_cast<int>(r.regClass()), static_cast<std::uint32_t>(r.id)}, inst);
    }
    return defs;
}

[[nodiscard]] std::optional<LirInstId> defOf(std::map<std::pair<int, std::uint32_t>, LirInstId> const& defs,
                                             LirOperand const& o) {
    if (o.kind != LirOperandKind::Reg || o.reg.isPhysical != 0) return std::nullopt;
    auto const it = defs.find({static_cast<int>(o.reg.regClass()), static_cast<std::uint32_t>(o.reg.id)});
    if (it == defs.end()) return std::nullopt;
    return it->second;
}

// The first integer immediate an instruction carries (a materialized constant), if any.
[[nodiscard]] std::optional<std::int64_t> immOf(Lir const& lir, LirInstId inst) {
    for (LirOperand const& o : lir.instOperands(inst))
        if (o.kind == LirOperandKind::ImmInt) return o.immInt32;
    return std::nullopt;
}

// What MIR->LIR emitted for function `fi`'s stack-pointer descents: the REALIGNMENT (`sub_sp_reg SP, d` with
// `d = (sp_copy SP) & mask`), with its mask and its position, and every OTHER descent's rounding (the `add` bias of
// the align-up its size comes from, `size + (A-1)`), so a test reads A off the emitted immediate.
struct Descents {
    std::vector<std::int64_t> realignMasks;      // one per realignment sequence
    std::optional<std::size_t> realignIndex;     // the first realignment's `sub_sp_reg`, in instsOf order
    std::vector<std::int64_t> otherRoundingBias; // the `add` immediate of every other descent's align-up
    std::optional<std::size_t> firstCallIndex;
};

[[nodiscard]] Descents descentsOf(Lir const& lir, TargetSchema const& t, std::uint32_t fi, std::uint16_t spOrd) {
    Descents out;
    auto const subSp  = t.opcodeByMnemonic("sub_sp_reg");
    auto const spCopy = t.opcodeByMnemonic("sp_copy");
    auto const andOp  = t.opcodeByMnemonic("and");
    auto const addOp  = t.opcodeByMnemonic("add");
    auto const callOp = t.opcodeByMnemonic("call");
    if (!subSp || !spCopy || !andOp || !addOp) return out;
    auto const defs = vregDefs(lir, fi);
    auto const insts = instsOf(lir, fi);
    for (std::size_t k = 0; k < insts.size(); ++k) {
        LirInstId const inst = insts[k];
        std::uint16_t const op = lir.instOpcode(inst);
        if (callOp && op == *callOp && !out.firstCallIndex) out.firstCallIndex = k;
        if (op != *subSp) continue;
        auto const ops = lir.instOperands(inst);
        if (ops.size() < 2 || !isPhysical(ops[0], spOrd)) continue;
        // The size: an `and` (an align-up's, or the realignment's), possibly behind one copy of its own.
        std::optional<LirInstId> sizeDef = defOf(defs, ops[1]);
        if (sizeDef && lir.instOpcode(*sizeDef) != *andOp) {
            auto const copyOps = lir.instOperands(*sizeDef);
            sizeDef = copyOps.empty() ? std::nullopt : defOf(defs, copyOps[0]);
        }
        if (!sizeDef || lir.instOpcode(*sizeDef) != *andOp) continue;
        auto const andOps = lir.instOperands(*sizeDef);
        if (andOps.size() < 2) continue;
        std::optional<LirInstId> const lhs = defOf(defs, andOps[0]);
        std::optional<LirInstId> const rhs = defOf(defs, andOps[1]);
        if (!lhs) continue;
        if (lir.instOpcode(*lhs) == *spCopy && !lir.instOperands(*lhs).empty()
            && isPhysical(lir.instOperands(*lhs)[0], spOrd)) {
            out.realignMasks.push_back(rhs ? immOf(lir, *rhs).value_or(-1) : -1);
            if (!out.realignIndex) out.realignIndex = k;
        } else if (lir.instOpcode(*lhs) == *addOp) {
            out.otherRoundingBias.push_back(immOf(lir, *lhs).value_or(-1));
        }
    }
    return out;
}

// `void f(void *p) { g(<p's bytes, a by-value carrier of `bytes` bytes stating `align`>); }` -- no local at all, so
// nothing over-aligned: the argument's own alignment is the only reason the frame could change.
[[nodiscard]] Mir buildStackedCall(TypeInterner& in, std::uint32_t bytes, std::uint32_t align) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const i64   = in.primitive(TypeKind::I64);
    TypeId const ptr   = in.pointer(i64);
    TypeId const sig   = in.fnSig(std::vector<TypeId>{ptr}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const p = mb.addArg(0, ptr);
    std::array<MirInstId, 1> const carrierOps{p};
    MirInstId const carrier = mb.addInst(MirOpcode::ByValueStackArg, carrierOps, ptr,
                                         bytes | (std::uint32_t{kByValueStackArgExhaustNone}
                                                  << kByValueStackArgExhaustShift),
                                         MirInstFlags::None, align);
    std::vector<MirInstId> callOps;
    callOps.push_back(mb.addGlobalAddr(SymbolId{2}, in.pointer(sig)));
    callOps.push_back(carrier);
    (void)mb.addInst(MirOpcode::Call, callOps, voidT);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// The compile pipeline's projection of MIR->LIR's rows onto callconv's (`compile_pipeline.cpp`).
[[nodiscard]] std::vector<LirFuncLocalAlignment> projected(std::vector<FuncLocalAlignment> const& rows) {
    std::vector<LirFuncLocalAlignment> out;
    for (FuncLocalAlignment const& a : rows)
        out.push_back(LirFuncLocalAlignment{a.funcSymbol, a.maxLocalAlignBytes, a.perAllocaAlignBytes,
                                            a.stackedArgAlignBytes});
    return out;
}

// One C source through the pipeline's order up to callconv: MIR->LIR, wide-call lowering, liveness, allocation,
// rewrite, callconv with the projected rows.
struct Chain {
    test_support::LoweredLir lowered;
    LirWideCallResult        wide;
    LirLiveness              liveness;
    LirAllocation            alloc;
    LirRewriteResult         rewritten;
    DiagnosticReporter       rep;
    LirCallconvResult        cc;
    bool                     ran = false;
    explicit Chain(test_support::LoweredLir l) : lowered(std::move(l)) {}
};

[[nodiscard]] Chain chainOf(std::string src, std::string const& target, std::uint16_t ccIndex) {
    Chain c{test_support::lowerCToLir(std::move(src), target, ccIndex)};
    if (!c.lowered.lir.ok) return c;
    c.wide = lowerWideCallArgs(c.lowered.lir.lir, *c.lowered.target, ccIndex, c.rep);
    if (!c.wide.ok) return c;
    c.liveness = analyzeLiveness(c.wide.lir);
    c.alloc = allocateRegisters(c.wide.lir, *c.lowered.target, c.liveness, ccIndex, c.rep);
    if (!c.alloc.ok()) return c;
    c.rewritten = rewriteWithAllocation(c.wide.lir, *c.lowered.target, c.alloc, c.rep);
    if (!c.rewritten.ok) return c;
    auto const rows = projected(c.lowered.lir.funcLocalAlignments);
    c.cc = materializeCallingConvention(c.rewritten.lir, *c.lowered.target, c.alloc, c.rep, {}, rows);
    c.ran = true;
    return c;
}

[[nodiscard]] std::string alignedStruct(std::uint32_t n) {
    std::string const s = std::to_string(n);
    return "struct AL { _Alignas(" + s + ") long long v; long long w[" + std::to_string(n / 8 - 1) + "]; };\n"
           "long long take(struct AL s);\n"
           "long long take_tail(long long a, long long b, long long c, long long d, long long e, long long f, "
           "struct AL s, long long k);\n";
}

constexpr std::uint32_t kAlignments[] = {32, 64, 256};

namespace fs = std::filesystem;

// ── (E)'s image reading ─────────────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] std::vector<std::uint8_t> readBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A little-endian field of `n` bytes at `off`; a read past the image's end is a failure, never a zero.
[[nodiscard]] std::uint64_t le(std::vector<std::uint8_t> const& b, std::uint64_t off, unsigned n) {
    if (off + n > b.size()) {
        ADD_FAILURE() << "a read of " << n << " byte(s) past the image's end, at byte " << off;
        return 0;
    }
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(b[static_cast<std::size_t>(off + i)]) << (8u * i);
    return v;
}

[[nodiscard]] std::string cString(std::vector<std::uint8_t> const& b, std::uint64_t off, std::size_t max) {
    std::string out;
    for (std::size_t i = 0; i < max && off + i < b.size() && b[static_cast<std::size_t>(off + i)] != 0; ++i)
        out.push_back(static_cast<char>(b[static_cast<std::size_t>(off + i)]));
    return out;
}

// One section of a linked image: where its bytes are in the file, how many, and where they load.
struct ImageSection {
    std::uint64_t fileOff = 0;
    std::uint64_t size    = 0;
    std::uint64_t va      = 0;
};

// An ELF64 image's section by name, through its section header table (e_shoff, e_shnum, e_shstrndx; each 64-byte
// header's sh_name, sh_addr, sh_offset, sh_size).
[[nodiscard]] std::optional<ImageSection> elfSection(std::vector<std::uint8_t> const& b, std::string_view name) {
    std::uint64_t const shoff = le(b, 0x28, 8);
    std::uint64_t const shnum = le(b, 0x3C, 2);
    std::uint64_t const names = le(b, shoff + le(b, 0x3E, 2) * 64u + 0x18, 8);
    for (std::uint64_t i = 0; i < shnum; ++i) {
        std::uint64_t const sh = shoff + i * 64u;
        if (cString(b, names + le(b, sh, 4), 64) == name)
            return ImageSection{le(b, sh + 0x18, 8), le(b, sh + 0x20, 8), le(b, sh + 0x10, 8)};
    }
    return std::nullopt;
}

// A Mach-O 64 image's section by (segment, section), through its LC_SEGMENT_64 commands (segname at 8, nsects at
// 64, the section_64 records from 72; each record's sectname at 0, addr at 32, size at 40, offset at 48).
[[nodiscard]] std::optional<ImageSection> machoSection(std::vector<std::uint8_t> const& b, std::string_view segment,
                                                       std::string_view section) {
    constexpr std::uint64_t kLcSegment64 = 0x19;
    std::uint64_t const ncmds = le(b, 16, 4);
    std::uint64_t off = 32;
    for (std::uint64_t i = 0; i < ncmds; ++i) {
        std::uint64_t const cmd = le(b, off, 4);
        std::uint64_t const cmdsize = le(b, off + 4, 4);
        if (cmd == kLcSegment64 && cString(b, off + 8, 16) == segment) {
            std::uint64_t const nsects = le(b, off + 64, 4);
            for (std::uint64_t k = 0; k < nsects; ++k) {
                std::uint64_t const rec = off + 72 + k * 80;
                if (cString(b, rec, 16) == section)
                    return ImageSection{le(b, rec + 48, 4), le(b, rec + 40, 8), le(b, rec + 32, 8)};
            }
        }
        if (cmdsize == 0) break;
        off += cmdsize;
    }
    return std::nullopt;
}

// The executables DSS links itself for a convention that passes a MEMORY-class aggregate by its bytes (each
// convention read off the target below, never assumed), and where each container keeps its code and its unwind
// table.
struct ImageArm {
    char const* spec;
    char const* convention;
    bool        macho;
};
constexpr ImageArm kImageArms[] = {
    {"x86_64:elf64-x86_64-linux-exec", "sysv_amd64", false},
    {"x86_64:macho64-x86_64-darwin-exec", "sysv_amd64_darwin", true},
};

// A caller passing a 64-aligned aggregate on the stack, through a VOLATILE function pointer so that no pipeline
// inlines the call away: at release the caller may itself be inlined into `main`, which then realigns instead.
constexpr char kImageSource[] =
    "struct AL { _Alignas(64) long long v; long long w[7]; };\n"
    "static long long take_tail(long long a, long long b, long long c, long long d, long long e, long long f,\n"
    "                           struct AL s, long long k) { return a + f + s.v + s.w[6] + k; }\n"
    "static long long (*volatile callee)(long long, long long, long long, long long, long long, long long,\n"
    "                                    struct AL, long long) = take_tail;\n"
    "static long long caller(struct AL *p) { return callee(1, 2, 3, 4, 5, 6, *p, 7); }\n"
    "int main(void) { struct AL x = {1, {0, 0, 0, 0, 0, 0, 2}}; return (int)caller(&x); }\n";

// The two moves the frame's rules follow, as llvm-objdump 19 read them at those addresses of both images: the
// capture `movq %rsp, %rbp` and the restore `movq %rbp, %rsp`. x86-64 spells a register-to-register MOV two ways
// under REX.W -- 89 /r (ModRM 11 source destination) and 8B /r (ModRM 11 destination source) -- and either is the
// move; DSS's images carry the 8B spelling (MEASURED, run f26c-s25: the bytes this test printed on every arm).
using MoveForms = std::array<std::array<std::uint8_t, 3>, 2>;
constexpr MoveForms kCaptureFpFromSp{{{0x48, 0x89, 0xe5}, {0x48, 0x8b, 0xec}}};
constexpr MoveForms kRestoreSpFromFp{{{0x48, 0x89, 0xec}, {0x48, 0x8b, 0xe5}}};

} // namespace

// ── (A) THE ROW AND THE SEQUENCE, OVER HAND-BUILT MIR (ledger 497's pin) ────────────────────────────────────────────
//
// A function with NO over-aligned local -- no local at all -- whose call passes a 64-aligned aggregate on the stack:
// MIR->LIR emits a row for it, its local fields at their defaults and `stackedArgAlignBytes` 64 (the sparse gate
// fires on EITHER field, ledger 497), and the realignment sequence with the mask 63 before the call. Its CONTROL at
// the stack's own alignment (16) gets no row and no descent: the realignment is only ever paid for an argument the
// stack does not already align.
TEST(StackedArgumentRealignment, AFunctionWithNoOverAlignedLocalPassingA64AlignedAggregateGetsItsRowAndItsRealignment) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const* cc = (*target)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->stackPointer.has_value());
    ASSERT_EQ(cc->stackAlignment, 16u) << "the control below is stated at the stack's own alignment";
    for (std::uint32_t const align : {64u, 16u}) {
        SCOPED_TRACE(align);
        TypeInterner in{CompilationUnitId{1}};
        Mir const m = buildStackedCall(in, 64, align);
        DiagnosticReporter rep;
        MirToLirResult const r = lowerToLir(m, **target, in, rep, std::vector<ExternImport>{},
                                            ExternCallDispatch::DirectPlt);
        ASSERT_TRUE(r.ok) << inventory(rep);
        Descents const d = descentsOf(r.lir, **target, 0, cc->stackPointer->ordinal);
        if (align > cc->stackAlignment) {
            ASSERT_EQ(r.funcLocalAlignments.size(), 1u)
                << "a function passing a stacked argument aligned past the stack has a row, with no local at all";
            FuncLocalAlignment const& row = r.funcLocalAlignments[0];
            EXPECT_EQ(row.funcSymbol, SymbolId{1});
            EXPECT_EQ(row.stackedArgAlignBytes, 64u);
            EXPECT_EQ(row.maxLocalAlignBytes, 0u) << "no local is over-aligned: the local field stays at its default";
            EXPECT_TRUE(row.perAllocaAlignBytes.empty());
            ASSERT_EQ(d.realignMasks.size(), 1u) << "exactly one realignment, in the entry block";
            EXPECT_EQ(d.realignMasks[0], 63) << "the descent is SP's distance past the 64-byte boundary";
            ASSERT_TRUE(d.firstCallIndex.has_value());
            ASSERT_TRUE(d.realignIndex.has_value());
            EXPECT_LT(*d.realignIndex, *d.firstCallIndex) << "the stack pointer is realigned before the call";
        } else {
            EXPECT_TRUE(r.funcLocalAlignments.empty()) << "an argument the stack already aligns costs no row";
            EXPECT_TRUE(d.realignMasks.empty()) << "an argument the stack already aligns costs no realignment";
        }
    }
}

// ── (B) THROUGH THE PIPELINE: THE FRAME THE REALIGNMENT RUNS ON ─────────────────────────────────────────────────────
//
// For N = 32, 64 and 256 under every convention that passes a MEMORY-class aggregate by its bytes, a call that
// stacks a `long long` AFTER the aggregate (the six integer registers taken first), so the outgoing area -- N + 8 --
// is NOT already a multiple of N: the realignment is emitted with the mask N-1 before the call; callconv sizes the
// outgoing area to a multiple of N (the capture `w` is `SP + outgoing`, so `w & (N-1)` is SP's own distance past
// the boundary only then -- with N + 8 the descent would leave SP 8 bytes off N); the frame pointer is saved;
// the CFA rule moves to it in the prologue and back to SP before the epilogue (DWARF's `def_cfa_register` -- the
// frame stays describable after SP moves, the CFI read ledger 495 asks for); and the capture carries the bias.
TEST(StackedArgumentRealignment, ACallerOfAStackedArgumentAlignedPastTheStackRunsOnTheDynamicFrame) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    bool sawStackConvention = false;
    for (std::uint16_t ci = 0; ci < (*target)->callingConventionCount(); ++ci) {
        auto const* cc = (*target)->callingConvention(ci);
        ASSERT_NE(cc, nullptr);
        if (cc->aggregate.memoryArgument != AggregateMemoryArgument::Stack) continue;
        sawStackConvention = true;
        ASSERT_TRUE(cc->stackPointer.has_value());
        ASSERT_TRUE(cc->framePointer.has_value());
        std::uint16_t const sp = cc->stackPointer->ordinal;
        std::uint16_t const fp = cc->framePointer->ordinal;
        for (std::uint32_t const n : kAlignments) {
            SCOPED_TRACE(cc->name + ", N = " + std::to_string(n));
            Chain c = chainOf(alignedStruct(n) + "long long f(struct AL *p) { return take_tail(1, 2, 3, 4, 5, 6, "
                                                 "*p, 7); }\n",
                              "x86_64", ci);
            ASSERT_TRUE(c.lowered.lir.ok);
            ASSERT_TRUE(c.ran) << inventory(c.rep);
            ASSERT_TRUE(c.cc.ok()) << inventory(c.rep);
            // MIR->LIR: the row and the sequence.
            bool rowSeen = false;
            for (FuncLocalAlignment const& row : c.lowered.lir.funcLocalAlignments)
                if (row.stackedArgAlignBytes == n) rowSeen = true;
            EXPECT_TRUE(rowSeen) << "f's row states the argument's alignment";
            Descents const d = descentsOf(c.lowered.lir.lir, **target, 0, sp);
            ASSERT_EQ(d.realignMasks.size(), 1u);
            EXPECT_EQ(d.realignMasks[0], static_cast<std::int64_t>(n) - 1);
            // callconv: the frame.
            ASSERT_EQ(c.cc.perFunc.size(), 1u);
            FrameLayout const& layout = c.cc.perFunc[0];
            EXPECT_GE(layout.outgoingArgAreaSize, n + 8u)
                << "the aggregate's bytes and the eightbyte after them travel in the outgoing area";
            EXPECT_EQ(layout.outgoingArgAreaSize % n, 0u)
                << "the outgoing area is a whole multiple of N, so the biased capture is congruent to SP mod N";
            bool fpSaved = false;
            for (LirReg const& r : layout.savedRegs)
                if (r.isPhysical != 0 && r.id == fp) fpSaved = true;
            EXPECT_TRUE(fpSaved) << "the frame pointer the fixed frame is addressed off is the caller's, saved";
            ASSERT_EQ(c.cc.perFuncCfi.size(), 1u);
            bool cfaToFp = false, cfaToSp = false;
            for (LirCfiOp const& op : c.cc.perFuncCfi[0].ops) {
                if (op.kind != CfiOpKind::DefCfaRegister) continue;
                if (op.reg == CfiRegRef::physical(fp)) cfaToFp = true;
                if (op.reg == CfiRegRef::physical(sp)) cfaToSp = true;
            }
            EXPECT_TRUE(cfaToFp) << "the CFA rule moves to the frame pointer before SP moves at run time";
            EXPECT_TRUE(cfaToSp) << "and back to SP when the epilogue restores it from the frame pointer";
            // The capture's bias: every `sp_copy dst, SP` into a register other than FP is followed by
            // `add dst, outgoingArgAreaSize`.
            auto const spCopy = (*target)->opcodeByMnemonic("sp_copy");
            auto const addOp  = (*target)->opcodeByMnemonic("add");
            ASSERT_TRUE(spCopy.has_value() && addOp.has_value());
            auto const insts = instsOf(c.cc.lir, 0);
            std::size_t captures = 0;
            for (std::size_t k = 0; k + 1 < insts.size(); ++k) {
                if (c.cc.lir.instOpcode(insts[k]) != *spCopy) continue;
                auto const ops = c.cc.lir.instOperands(insts[k]);
                LirReg const dst = c.cc.lir.instResult(insts[k]);
                if (ops.empty() || !isPhysical(ops[0], sp) || dst.id == fp || dst.id == sp) continue;
                ++captures;
                auto const next = c.cc.lir.instOperands(insts[k + 1]);
                EXPECT_EQ(c.cc.lir.instOpcode(insts[k + 1]), *addOp);
                ASSERT_GE(next.size(), 2u);
                ASSERT_EQ(next[1].kind, LirOperandKind::ImmInt);
                EXPECT_EQ(next[1].immInt32, static_cast<std::int32_t>(layout.outgoingArgAreaSize))
                    << "the realignment's capture reads SP + the outgoing area, the dynamic frame's watermark";
            }
            EXPECT_EQ(captures, 1u) << "one capture: the realignment's";
        }
    }
    EXPECT_TRUE(sawStackConvention) << "x86_64 ships a convention that passes a MEMORY-class aggregate by its bytes";
}

// ── (C) A VLA IN SUCH A FUNCTION KEEPS THE BOUNDARY ─────────────────────────────────────────────────────────────────
//
// The realignment puts SP on N once; a VLA's descent that rounded to the stack's 16 would take it off N again before
// the call. So the descent rounds to N: the align-up's bias is N-1 (it was 15). The control: the same VLA in a
// function passing nothing over-aligned rounds to the stack's own alignment.
TEST(StackedArgumentRealignment, AVariableLengthArrayInARealignedFunctionDescendsByAMultipleOfTheAlignment) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto const* cc = (*target)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    ASSERT_EQ(cc->aggregate.memoryArgument, AggregateMemoryArgument::Stack);
    ASSERT_TRUE(cc->stackPointer.has_value());
    for (std::uint32_t const n : kAlignments) {
        SCOPED_TRACE(n);
        auto const lowered = test_support::lowerCToLir(
            alignedStruct(n) + "long long f(struct AL *p, int n) { char buf[n]; buf[0] = 0; "
                               "return take(*p) + buf[0]; }\n",
            "x86_64", 0);
        ASSERT_TRUE(lowered.lir.ok);
        Descents const d = descentsOf(lowered.lir.lir, **target, 0, cc->stackPointer->ordinal);
        ASSERT_EQ(d.realignMasks.size(), 1u);
        ASSERT_FALSE(d.otherRoundingBias.empty()) << "the VLA's descent is found";
        for (std::int64_t const bias : d.otherRoundingBias)
            EXPECT_EQ(bias, static_cast<std::int64_t>(n) - 1)
                << "a VLA in a function realigned to " << n << " descends by a multiple of " << n;
    }
    auto const control = test_support::lowerCToLir(
        "long long take(long long v);\n"
        "long long f(long long *p, int n) { char buf[n]; buf[0] = 0; return take(*p) + buf[0]; }\n",
        "x86_64", 0);
    ASSERT_TRUE(control.lir.ok);
    Descents const d = descentsOf(control.lir.lir, **target, 0, cc->stackPointer->ordinal);
    EXPECT_TRUE(d.realignMasks.empty());
    ASSERT_FALSE(d.otherRoundingBias.empty());
    for (std::int64_t const bias : d.otherRoundingBias)
        EXPECT_EQ(bias, static_cast<std::int64_t>(cc->stackAlignment) - 1)
            << "a VLA in a function that passes nothing over-aligned rounds to the stack's own alignment";
}

// ── (D) A CALLER-COPY CONVENTION IS UNTOUCHED ───────────────────────────────────────────────────────────────────────
//
// Under `callerCopy` the aggregate travels as a pointer to a copy the caller makes as a LOCAL (the over-aligned-local
// frame aligns it), so nothing over-aligned is stacked: no `stackedArgAlignBytes`, no realignment -- on every such
// shipped convention of both targets (ms_x64, aapcs64, apple_arm64), read off the config rather than named.
TEST(StackedArgumentRealignment, ACallerCopyConventionNeverRealignsForAnAggregateArgument) {
    std::size_t callerCopyConventions = 0;
    for (char const* const t : {"x86_64", "arm64"}) {
        auto target = TargetSchema::loadShipped(t);
        ASSERT_TRUE(target.has_value());
        for (std::uint16_t ci = 0; ci < (*target)->callingConventionCount(); ++ci) {
            auto const* cc = (*target)->callingConvention(ci);
            ASSERT_NE(cc, nullptr);
            if (cc->aggregate.memoryArgument != AggregateMemoryArgument::CallerCopy) continue;
            ++callerCopyConventions;
            ASSERT_TRUE(cc->stackPointer.has_value());
            for (std::uint32_t const n : kAlignments) {
                SCOPED_TRACE(std::string{t} + " " + cc->name + ", N = " + std::to_string(n));
                auto const lowered = test_support::lowerCToLir(
                    alignedStruct(n) + "long long f(struct AL *p) { return take(*p); }\n", t, ci);
                ASSERT_TRUE(lowered.lir.ok);
                for (FuncLocalAlignment const& row : lowered.lir.funcLocalAlignments)
                    EXPECT_EQ(row.stackedArgAlignBytes, 0u) << "a caller copy is a local, never a stacked argument";
                Descents const d = descentsOf(lowered.lir.lir, **target, 0, cc->stackPointer->ordinal);
                EXPECT_TRUE(d.realignMasks.empty()) << "a caller-copy convention never realigns for an argument";
            }
        }
    }
    EXPECT_GE(callerCopyConventions, 3u) << "ms_x64, aapcs64 and apple_arm64 are caller-copy conventions";
}

// ── (E) WHAT DSS'S OWN LINKED IMAGE SAYS OF THE FRAME (main's ledger 510) ───────────────────────────────────────────
//
// The frame as an unwinder reads it: the executable DSS compiles AND links itself, for each format whose convention
// passes a MEMORY-class aggregate by its bytes, its unwind section decoded by the reader DSS uses for a foreign
// object's (`decodeEhFrame`). MEASURED first with llvm-dwarfdump 19 --eh-frame and llvm-objdump 19 on the example
// a_stacked_argument_is_placed_on_its_own_alignment, both formats, debug and release (P69 fold f26c): every realigned
// caller's FDE states CFA = SP + F once its frame is allocated and the frame pointer's save, moves the CFA to the
// frame pointer right after the capture `movq %rsp, %rbp` -- CFA = FP + F, FP being SP there -- holds it across the
// realignment, every call and every run-time descent, moves it back to SP right after the restore `movq %rbp, %rsp`,
// and ends on the entry rule; no other function's CFA leaves SP. This pins that reading. (A DSS RELOCATABLE object on
// Mach-O carries no unwind table yet, so the native witness prints its walk there; the image is DSS's own link.)
TEST(StackedArgumentRealignment, ARealignedCallersLinkedImageDescribesItsFrameOffTheFramePointer) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    test_support::ScratchDir scratch{test_support::Location::Temp, "lir-stacked-argument-realignment-image"};
    fs::path const src = scratch.path() / "realigned.c";
    {
        std::ofstream out(src, std::ios::binary);
        out << kImageSource;
    }
    for (ImageArm const& arm : kImageArms) {
        TargetCallingConvention const* cc = nullptr;
        for (std::uint16_t ci = 0; ci < (*target)->callingConventionCount(); ++ci)
            if ((*target)->callingConvention(ci)->name == arm.convention) cc = (*target)->callingConvention(ci);
        ASSERT_NE(cc, nullptr) << arm.convention;
        ASSERT_EQ(cc->aggregate.memoryArgument, AggregateMemoryArgument::Stack)
            << arm.convention << " passes a MEMORY-class aggregate by its bytes: its caller realigns";
        ASSERT_TRUE(cc->stackPointer.has_value() && cc->framePointer.has_value());
        CfiRegRef const sp = CfiRegRef::physical(cc->stackPointer->ordinal);
        CfiRegRef const fp = CfiRegRef::physical(cc->framePointer->ordinal);
        for (auto const& [label, config] :
             {std::pair{"debug", CompileConfig::Debug}, std::pair{"release", CompileConfig::Release}}) {
            SCOPED_TRACE(std::string{arm.spec} + ", " + label);
            fs::path const out = scratch.path() / (std::string{arm.convention} + "-" + label);
            fs::create_directories(out);
            DiagnosticReporter rep;
            Program prog;
            prog.setOutputDir(out);
            prog.setCompileConfig(config);
            ASSERT_EQ(prog.compileFiles(std::vector<std::string>{src.string()}, "c",
                                        std::vector<std::string>{arm.spec}, rep),
                      0)
                << inventory(rep);
            std::vector<std::uint8_t> image;
            for (auto const& e : fs::directory_iterator(out))
                if (e.is_regular_file() && e.path().stem() == "realigned") image = readBytes(e.path());
            ASSERT_FALSE(image.empty()) << "DSS links the image itself";
            auto const unwind =
                arm.macho ? machoSection(image, "__TEXT", "__eh_frame") : elfSection(image, ".eh_frame");
            auto const code = arm.macho ? machoSection(image, "__TEXT", "__text") : elfSection(image, ".text");
            ASSERT_TRUE(unwind.has_value()) << "the image carries its unwind table";
            ASSERT_TRUE(code.has_value());
            ASSERT_LE(unwind->fileOff + unwind->size, image.size());
            DiagnosticReporter decodeRep;
            auto const decoded = link::format::decodeEhFrame(
                std::span<std::uint8_t const>(image).subspan(static_cast<std::size_t>(unwind->fileOff),
                                                             static_cast<std::size_t>(unwind->size)),
                link::format::dwarfRegisterMappingOf(**target), "test::linked-image", decodeRep);
            ASSERT_TRUE(decoded.has_value()) << inventory(decodeRep);
            // The one function whose CFA leaves SP: the realigned caller (at release, `main` if it took it in).
            std::vector<link::format::DecodedFde const*> dynamic;
            for (link::format::DecodedFde const& f : decoded->fdes)
                for (CfiOp const& op : f.cfi.ops)
                    if (op.kind == CfiOpKind::DefCfaRegister && op.reg == fp) {
                        dynamic.push_back(&f);
                        break;
                    }
            ASSERT_EQ(dynamic.size(), 1u)
                << "exactly one function -- the realigned caller -- runs on the dynamic frame";
            link::format::DecodedFde const& fde = *dynamic[0];
            CfiFunction const& fn = fde.cfi;
            ASSERT_EQ(CfiRegRef::physical(fn.initial.cfaRegister), sp) << "a function is entered on SP's rule";
            // The rules, in order.
            CfiRegRef cfaReg = CfiRegRef::physical(fn.initial.cfaRegister);
            std::int64_t cfaOff = fn.initial.cfaOffset;
            std::optional<std::uint32_t> fpSaved, toFp, toSp;
            std::int64_t offAtFp = 0;
            for (CfiOp const& op : fn.ops) {
                if (op.kind == CfiOpKind::RegAtCfaOffset && op.reg == fp && !fpSaved) fpSaved = op.pcOffset;
                if (!cfiOpTouchesCfa(op.kind)) continue;
                bool const backToSp = op.kind == CfiOpKind::DefCfaRegister && op.reg == sp;
                if (toFp && !toSp)
                    EXPECT_TRUE(backToSp) << "while SP moves at run time the CFA stays FP + F; a "
                                          << cfiOpKindName(op.kind) << " rule at +" << op.pcOffset;
                CfiRegRef const before = cfaReg;
                if (op.kind == CfiOpKind::DefCfa) {
                    cfaReg = op.reg;
                    cfaOff = op.offset;
                } else if (op.kind == CfiOpKind::DefCfaRegister) {
                    cfaReg = op.reg;
                } else if (op.kind == CfiOpKind::DefCfaOffset) {
                    cfaOff = op.offset;
                } else {
                    cfaOff += op.offset;
                }
                if (op.kind == CfiOpKind::DefCfaRegister && op.reg == fp && !toFp) {
                    toFp = op.pcOffset;
                    offAtFp = cfaOff;
                    EXPECT_EQ(before, sp) << "the CFA moves to FP from SP's rule";
                } else if (backToSp && toFp && !toSp) {
                    toSp = op.pcOffset;
                    EXPECT_EQ(cfaOff, offAtFp) << "and comes back at the same offset: FP was SP after the allocation";
                }
            }
            ASSERT_TRUE(toFp.has_value()) << "the CFA moves to the frame pointer";
            ASSERT_TRUE(toSp.has_value()) << "and back to SP";
            EXPECT_LT(*toFp, *toSp);
            EXPECT_GT(offAtFp, fn.initial.cfaOffset) << "the CFA moves to FP once the frame is allocated: FP + F";
            ASSERT_TRUE(fpSaved.has_value()) << "the caller's frame pointer's save is described";
            EXPECT_LE(*fpSaved, *toFp) << "before the capture overwrites it";
            EXPECT_EQ(cfaReg, CfiRegRef::physical(fn.initial.cfaRegister)) << "the function ends on its entry rule";
            EXPECT_EQ(cfaOff, fn.initial.cfaOffset);
            // The code the rules describe: the FDE's pc-relative start resolved in the image, then the bytes each
            // rule follows.
            ASSERT_TRUE(link::format::dwEhPeIsPcRelative(fde.pointerEncoding));
            std::int64_t const fnVa = static_cast<std::int64_t>(unwind->va + fde.initialLocationFieldOffset)
                                    + fde.storedInitialLocation;
            ASSERT_GE(fnVa, static_cast<std::int64_t>(code->va));
            std::uint64_t const at = code->fileOff + (static_cast<std::uint64_t>(fnVa) - code->va);
            ASSERT_LE(at + fn.codeLength, image.size());
            ASSERT_GE(*toFp, 3u);
            auto const endingAt = [&](std::uint32_t pc) {
                std::array<std::uint8_t, 3> b{};
                for (std::size_t i = 0; i < b.size(); ++i) b[i] = image[static_cast<std::size_t>(at + pc - 3u + i)];
                return b;
            };
            auto const isOneOf = [](std::array<std::uint8_t, 3> const& b, MoveForms const& forms) {
                return std::find(forms.begin(), forms.end(), b) != forms.end();
            };
            EXPECT_TRUE(isOneOf(endingAt(*toFp), kCaptureFpFromSp))
                << "the rule to FP takes effect right after the capture `movq %rsp, %rbp`; the bytes there: "
                << ::testing::PrintToString(endingAt(*toFp));
            EXPECT_TRUE(isOneOf(endingAt(*toSp), kRestoreSpFromFp))
                << "the rule back takes effect right after the restore `movq %rbp, %rsp`; the bytes there: "
                << ::testing::PrintToString(endingAt(*toSp));
        }
    }
}
