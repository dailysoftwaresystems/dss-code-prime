// A VALUE THE MIDDLE END KEEPS IN MEMORY, BOUND TO AN INLINE-ASM REGISTER
// CONSTRAINT, REACHES THE TEMPLATE AS ITS VALUE — IN ONE REGISTER OR A PAIR —
// AND NEVER AS ITS ADDRESS.
//
//   D-MIR-ASM-BY-ADDRESS-OPERAND-BOUND-TO-A-REGISTER-RECEIVES-ITS-ADDRESS
//   D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED
//
// ★★★ THE DEFECT, ✔MEASURED 2026-09-18 THROUGH THE SHIPPED CLI at `7df54cc1`:
// a struct, a union, a `_Complex` or a 128-bit integer bound to `"r"`/`"w"`/`"x"`
// reached the template as the ADDRESS of the value (`hir_to_mir` handed a
// register-form operand the by-address value it keeps for such a type) or, for
// a struct, as its first 8 bytes — rc=0, the wrong bits, debug and release, on
// both shipped targets. gcc 13.3.0 and clang 18.1.3 carry the VALUE: one
// register when it fits, a register PAIR (aarch64 `%H0` names the second) when
// it is twice a register wide — per class, size and direction, which the target
// now DECLARES (`asmValueCarriage`) because no rule over kinds or sizes alone
// reproduces what the references do.
//
// ★★ THE ARMS, EACH A STATEMENT ABOUT THE TIER THAT DECIDES IT:
//   (A) MIR: every register-form operand of a by-address type is filed BY
//       ADDRESS with its size and direction on the entry — an output too, which
//       then yields no result piece and no tied read half; a register-resident
//       operand carries nothing.
//   (B) LIR, pairs: each half LOADED at its own displacement into its own
//       register, `%2`/`%H2` reading them in order; the x86_64 pair loaded
//       whole though AT&T names only its low register.
//   (C) LIR, one register: a 2-byte struct in an XMM register STAGED through a
//       general register (x86-64 has no 16-bit SSE load), gcc's own shape.
//   (D) THE CONSTRAINED RANGE HOLDS ONLY WHAT CARRIES THE CONSTRAINT: no
//       frame-address `lea_frame_slot` carries the statement's register
//       constraint (`lir_callconv` drops side data on such a virtual op and the
//       rebuild verifier refused the module — valid C refused).
//   (E) REFUSED BY NAME, each beside an accepted neighbour: a size no register
//       arrangement carries, a direction no reference carries, a pinned
//       register asked to hold a pair, a `_BitInt`.
//   (F) THE VOCABULARY'S LOAD-TIME CONTRACT: every malformed `asmValueCarriage`
//       row and `selects` key refused at load; a target that declares no
//       carriage refuses the operand by name (the red-on-disable of the config).
//   (G) `movaps` names a 128-bit XMM operand — its encoding is keyed at 128.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`.

#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "asm_region_test_support.hpp"
#include "lowered_lir_fixture.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

[[nodiscard]] std::string summarize(LoweredLir const& r) {
    std::string s;
    for (auto const& d : r.hirReporter.all()) s += "\n  hir: " + d.actual;
    for (auto const& d : r.mirReporter.all()) s += "\n  mir: " + d.actual;
    for (auto const& d : r.lirReporter.all()) s += "\n  lir: " + d.actual;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

[[nodiscard]] bool anyText(LoweredLir const& r, std::string_view needle) {
    for (auto const* rep : {&r.hirReporter, &r.mirReporter, &r.lirReporter}) {
        for (auto const& d : rep->all()) {
            if (d.actual.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

[[nodiscard]] bool anyCode(LoweredLir const& r, DiagnosticCode code) {
    for (auto const* rep : {&r.hirReporter, &r.mirReporter, &r.lirReporter}) {
        for (auto const& d : rep->all()) {
            if (d.code == code) return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<MirInstId> allMirInsts(Mir const& mir) {
    std::vector<MirInstId> out;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                out.push_back(mir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

// The ONE asm statement of a snippet (the non-terminator form).
[[nodiscard]] MirInstId theAsm(Mir const& mir) {
    std::vector<MirInstId> found;
    for (MirInstId const id : allMirInsts(mir)) {
        if (mir.instOpcode(id) == MirOpcode::InlineAsm) found.push_back(id);
    }
    EXPECT_EQ(found.size(), 1u) << "the snippet carries exactly one asm statement";
    return found.empty() ? InvalidMirInst : found.front();
}

[[nodiscard]] std::size_t returnPieceCount(Mir const& mir) {
    std::size_t n = 0;
    for (MirInstId const id : allMirInsts(mir)) {
        if (mir.instOpcode(id) == MirOpcode::ReturnPiece) ++n;
    }
    return n;
}

struct Placed { LirInstId id; LirBlockId block; };
[[nodiscard]] std::vector<Placed> allInsts(Lir const& lir) {
    std::vector<Placed> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                out.push_back({lir.blockInstAt(b, ii), b});
            }
        }
    }
    return out;
}

[[nodiscard]] std::uint16_t opOf(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    EXPECT_TRUE(i.has_value()) << "target declares no '" << m << "'";
    return i.has_value() ? *i : std::uint16_t{0};
}

[[nodiscard]] std::optional<std::int32_t> memOffset(Lir const& lir, LirInstId i) {
    for (auto const& o : lir.instOperands(i)) {
        if (o.kind == LirOperandKind::MemOffset) return o.offset;
    }
    return std::nullopt;
}

struct MemAccess { LirInstId id; LirReg reg; std::int32_t offset; };

// Every `op` at `widthBits` whose RESULT is a register of `cls`.
[[nodiscard]] std::vector<MemAccess>
loadsOf(Lir const& lir, std::uint16_t op, std::uint32_t widthBits, LirRegClass cls) {
    std::vector<MemAccess> out;
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) != op) continue;
        if (lirInstWidthBits(lir.instFlags(p.id)) != widthBits) continue;
        LirReg const r = lir.instResult(p.id);
        if (!r.valid() || r.regClass() != cls) continue;
        auto const off = memOffset(lir, p.id);
        if (!off.has_value()) continue;
        out.push_back({p.id, r, *off});
    }
    return out;
}

// The first instruction after `after` (in module order) that READS `reg`.
[[nodiscard]] std::optional<LirInstId>
firstReaderAfter(Lir const& lir, LirReg reg, LirInstId after) {
    bool seen = false;
    for (auto const& p : allInsts(lir)) {
        if (p.id.v == after.v) { seen = true; continue; }
        if (!seen) continue;
        for (auto const& o : lir.instOperands(p.id)) {
            if (o.kind == LirOperandKind::Reg && o.reg == reg) return p.id;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t positionOf(Lir const& lir, LirInstId id) {
    auto const all = allInsts(lir);
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].id.v == id.v) return i;
    }
    return all.size();
}

[[nodiscard]] bool hasOpcode(Lir const& lir, std::uint16_t op) {
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) == op) return true;
    }
    return false;
}

// The base register of a memory access (its first `Reg` operand for a load,
// its second for a store).
[[nodiscard]] std::optional<LirReg> baseOf(Lir const& lir, LirInstId i, bool store) {
    std::size_t seen = 0;
    for (auto const& o : lir.instOperands(i)) {
        if (o.kind != LirOperandKind::Reg) continue;
        if (seen == (store ? 1u : 0u)) return o.reg;
        ++seen;
    }
    return std::nullopt;
}

// The two halves of a PAIR carriage: 64-bit loads at +0 and +8 of ONE base
// register. nullopt when no such pair exists.
[[nodiscard]] std::optional<std::pair<MemAccess, MemAccess>>
pairLoads(Lir const& lir, std::uint16_t loadOp) {
    auto const loads = loadsOf(lir, loadOp, 64, LirRegClass::GPR);
    for (auto const& a : loads) {
        if (a.offset != 0) continue;
        auto const ba = baseOf(lir, a.id, false);
        for (auto const& b : loads) {
            if (b.offset != 8) continue;
            auto const bb = baseOf(lir, b.id, false);
            if (ba && bb && *ba == *bb) return std::make_pair(a, b);
        }
    }
    return std::nullopt;
}

// ── dialect documents, read from the shipped tree and edited in memory ──────
[[nodiscard]] std::string dialectText(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{std::string{"cannot locate dialect "} + std::string{name}};
    }
    std::ifstream in{*pathR};
    if (!in) throw std::runtime_error{"cannot open dialect document"};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Load the shipped aarch64 dialect with `edit` applied to its `H` modifier row.
// Returns every load error, joined; empty ⇔ the edited document loaded.
[[nodiscard]] std::string loadDialectWithHRow(
        std::function<void(nlohmann::json&)> const& edit) {
    nlohmann::json doc = nlohmann::json::parse(dialectText("asm-arm64-gas"));
    auto& rows = doc.at("assembly").at("templateModifiers");
    bool edited = false;
    for (auto& r : rows) {
        if (r.contains("letter") && r.at("letter") == "H") {
            edit(r);
            edited = true;
        }
    }
    if (!edited) throw std::runtime_error{"the dialect declares no `H` row"};
    auto g = GrammarSchema::loadFromText(doc.dump(), "<mutated asm-arm64-gas>");
    if (g.has_value()) return {};
    std::string why;
    for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
    return why.empty() ? std::string{"<refused with no message>"} : why;
}

// Load the shipped aarch64 dialect with `edit` applied to EVERY modifier row.
[[nodiscard]] std::string loadDialectWithEveryRow(
        std::function<void(nlohmann::json&)> const& edit) {
    nlohmann::json doc = nlohmann::json::parse(dialectText("asm-arm64-gas"));
    for (auto& r : doc.at("assembly").at("templateModifiers")) edit(r);
    auto g = GrammarSchema::loadFromText(doc.dump(), "<mutated asm-arm64-gas>");
    if (g.has_value()) return {};
    std::string why;
    for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
    return why.empty() ? std::string{"<refused with no message>"} : why;
}

[[nodiscard]] std::string loadErrors(
        LoadResult<std::shared_ptr<TargetSchema>> const& r) {
    if (r.has_value()) return {};
    std::string why;
    for (auto const& e : r.error()) why += e.path + ": " + e.message + "\n";
    return why.empty() ? std::string{"<refused with no message>"} : why;
}

// The shipped document with the `asmValueCarriage` row of `cls` edited.
[[nodiscard]] LoadResult<std::shared_ptr<TargetSchema>>
withCarriageRow(std::string_view target, std::string_view cls,
                std::function<void(nlohmann::json&)> const& edit) {
    return mutateShippedTargetSchemaDoc(target, [&](nlohmann::json& doc) {
        for (auto& row : doc.at("asmValueCarriage")) {
            // Compared as a std::string, never `json == string_view`: MSVC 19.51 rejects that as
            // AMBIGUOUS (C2666 — json's own operator== against the C++20-synthesized reversed
            // string_view comparison reached through json's implicit conversion), where gcc picks one.
            if (row.at("class").get<std::string>() == cls) edit(row);
        }
    });
}

} // namespace

// ── (A) MIR: THE ENTRY SAYS "CARRIED", AN OUTPUT TOO ─────────────────────────
TEST(LirAsmCarriedOperand, AByAddressOperandIsFiledWithItsSizeAndDirection) {
    // an input, an output and a `+` operand of three by-address kinds, and a
    // register-resident control in the same statement
    auto r = lowerCToLir(
        "struct s16 { unsigned long a, b; };\n"
        "struct s8 { unsigned int a, b; };\n"
        "long f(__int128 v, long k, struct s16 *out, struct s8 *io) {\n"
        "    struct s16 o; struct s8 x = *io; long r;\n"
        "    __asm__(\"movq %4, %0\" : \"=r\"(r), \"=r\"(o), \"+r\"(x)"
        " : \"r\"(v), \"r\"(k));\n"
        "    *out = o; *io = x; return r;\n"
        "}\n",
        "x86_64");
    ASSERT_FALSE(r.mirReporter.hasErrors()) << summarize(r);
    Mir const& mir = r.mir.mir;
    MirInstId const a = theAsm(mir);
    ASSERT_TRUE(a.valid());
    MirAsmDescriptor const& d = mir.asmDescriptor(a);

    // ONE result piece: the `long` output. The struct output yields none — its
    // value is stored through its address by the LIR tier — and the `+` struct
    // gets no synthesized tied read half.
    ASSERT_EQ(d.outputs.size(), 1u)
        << "only the register-resident `long` output is a result piece";
    EXPECT_EQ(d.outputs[0].carriedBytes, 0u);
    EXPECT_EQ(returnPieceCount(mir), 0u)
        << "output 0 is the asm's own value; no further piece is minted";

    // inputs: [addressed outputs: o, x][source inputs: v, k] — no tied half
    ASSERT_EQ(d.inputs.size(), 4u)
        << "two by-address outputs filed first, then the two source inputs";
    auto const& o = d.inputs[0];
    EXPECT_EQ(o.carriedBytes, 16u);
    EXPECT_TRUE(o.carriedOut);
    EXPECT_FALSE(o.carriedIn) << "an `=` output is not read";
    EXPECT_EQ(o.carriedTypeKind, static_cast<std::uint8_t>(TypeKind::Struct));
    auto const& x = d.inputs[1];
    EXPECT_EQ(x.carriedBytes, 8u);
    EXPECT_TRUE(x.carriedOut);
    EXPECT_TRUE(x.carriedIn) << "a `+` operand is read through the same address";
    EXPECT_FALSE(x.isReadWrite)
        << "no tied read half is requested: the load through the one address is it";
    auto const& v = d.inputs[2];
    EXPECT_EQ(v.carriedBytes, 16u);
    EXPECT_TRUE(v.carriedIn);
    EXPECT_FALSE(v.carriedOut);
    EXPECT_EQ(v.carriedTypeKind, static_cast<std::uint8_t>(TypeKind::I128));
    EXPECT_EQ(d.inputs[3].carriedBytes, 0u)
        << "a register-resident `long` carries nothing — it is moved, as ever";
    for (auto const& in : d.inputs) EXPECT_FALSE(in.tiedOutput.has_value());
}

// ── (B) LIR: THE PAIR, HALF BY HALF ──────────────────────────────────────────
TEST(LirAsmCarriedOperand, AnInt128InputIsLoadedAsAPairAndNamedInOrder) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto r = lowerCToLir(
        "void f(__int128 v, unsigned long *lo, unsigned long *hi) {\n"
        "    unsigned long a, b;\n"
        "    __asm__(\"mov %0, %2\\n\\tmov %1, %H2\" : \"=&r\"(a), \"=&r\"(b) : \"r\"(v));\n"
        "    *lo = a; *hi = b;\n"
        "}\n",
        *target);
    ASSERT_FALSE(r.lirReporter.hasErrors()) << summarize(r);
    Lir const& lir = r.lir.lir;
    // The two halves: 64-bit loads at +0 and +8 of ONE base into two GPRs.
    auto const pair = pairLoads(lir, opOf(t, "load"));
    ASSERT_TRUE(pair.has_value())
        << "the pair must be LOADED half by half out of the value — before the "
           "fix the template read the value's ADDRESS" << summarize(r);
    MemAccess const lo = pair->first, hi = pair->second;
    EXPECT_FALSE(lo.reg == hi.reg) << "a pair is two registers";
    // P68 round 8 part 4: the template's lines are the statement bundle's
    // BODY, and both loaded halves are slots it READS.
    auto const bundle = onlyAsmRegion(lir);
    ASSERT_TRUE(bundle.has_value());
    EXPECT_EQ(asmSlotRoleOf(*bundle->region, lo.reg), LirAsmOperandRole::Use);
    EXPECT_EQ(asmSlotRoleOf(*bundle->region, hi.reg), LirAsmOperandRole::Use);
    Lir const& body = bundle->region->body;
    auto const lines = asmRegionBodyInsts(*bundle->region);
    auto const firstReader = [&](LirReg reg) -> std::size_t {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            for (auto const& o : body.instOperands(lines[i])) {
                if (o.kind == LirOperandKind::Reg && o.reg == reg) return i;
            }
        }
        return lines.size();
    };
    std::size_t const readLo = firstReader(lo.reg);
    std::size_t const readHi = firstReader(hi.reg);
    ASSERT_TRUE(readLo < lines.size() && readHi < lines.size())
        << "the template must read both loaded halves";
    EXPECT_LT(readLo, readHi)
        << "`%2` (the first template line) must read the half at +0 and `%H2` "
           "(the second) the half at +8";
}

TEST(LirAsmCarriedOperand, AnX86PairIsCarriedWholeThoughOnlyItsLowRegisterHasAName) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto r = lowerCToLir(
        "void f(__int128 *p) {\n"
        "    __int128 v = *p;\n"
        "    __asm__(\"addq $1, %0\" : \"+r\"(v));\n"
        "    *p = v;\n"
        "}\n",
        *target);
    ASSERT_FALSE(r.lirReporter.hasErrors()) << summarize(r);
    Lir const& lir = r.lir.lir;
    auto const pair = pairLoads(lir, opOf(t, "load"));
    ASSERT_TRUE(pair.has_value())
        << "the `+r` pair is LOADED half by half before the template"
        << summarize(r);
    // The HIGH half — a register the template never names — is stored back
    // UNCHANGED at +8 of the same base after the template: gcc's pair partner
    // survives a `+` operand, and a one-register carriage would drop it.
    MemAccess const hi = pair->second;
    auto const hiBase = baseOf(lir, hi.id, false);
    bool storedBack = false;
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) != opOf(t, "store")) continue;
        if (lirInstWidthBits(lir.instFlags(p.id)) != 64) continue;
        if (memOffset(lir, p.id) != 8) continue;
        auto const ops = lir.instOperands(p.id);
        if (ops.empty() || ops[0].kind != LirOperandKind::Reg) continue;
        if (!(ops[0].reg == hi.reg)) continue;
        if (baseOf(lir, p.id, true) != hiBase) continue;
        storedBack = positionOf(lir, p.id) > positionOf(lir, hi.id);
    }
    EXPECT_TRUE(storedBack)
        << "the pair's HIGH half must be stored back through the same address"
        << summarize(r);
}

// ── (C) STAGED: a 2-byte struct into an XMM register through a GPR ───────────
TEST(LirAsmCarriedOperand, ATwoByteStructRidesAnXmmRegisterThroughAGeneralOne) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto r = lowerCToLir(
        "struct two { unsigned char b[2]; };\n"
        "void f(struct two *p) {\n"
        "    struct two v = *p;\n"
        "    __asm__ volatile(\"nop\" : \"+x\"(v));\n"
        "    *p = v;\n"
        "}\n",
        *target);
    ASSERT_FALSE(r.lirReporter.hasErrors())
        << "gcc 13.3.0 carries a 2-byte struct on `\"x\"` (`movzwl` + `movd`): "
        << summarize(r);
    Lir const& lir = r.lir.lir;
    auto const in16 = loadsOf(lir, opOf(t, "load"), 16, LirRegClass::GPR);
    ASSERT_FALSE(in16.empty())
        << "x86-64 SSE2 has no 16-bit XMM load: the value is loaded into a "
           "general register first";
    auto const toXmm = firstReaderAfter(lir, in16.back().reg, in16.back().id);
    ASSERT_TRUE(toXmm.has_value());
    EXPECT_EQ(lir.instOpcode(*toXmm), opOf(t, "movq_gpr_to_xmm"))
        << "and moved across by the target's own cross-class move";
    EXPECT_EQ(lir.instResult(*toXmm).regClass(), LirRegClass::FPR);
    EXPECT_TRUE(hasOpcode(lir, opOf(t, "movq_xmm_to_gpr")))
        << "and moved back out the same way before its 16-bit store";
}

// ── (D) THE CONSTRAINED RANGE: no frame-address `lea` carries the constraint ─
TEST(LirAsmCarriedOperand, NoFrameAddressInsideTheStatementCarriesItsConstraint) {
    struct Case { char const* target; char const* src; };
    for (Case const& c : {
             // a plain frame ADDRESS input beside a clobber — valid C gcc and
             // clang compile, refused at `callconv` before this fix
             Case{"x86_64",
                  "int f(void) { long x = 42, r;\n"
                  "  __asm__(\"movq %1, %0\" : \"=r\"(r) : \"r\"(&x) : \"r8\");\n"
                  "  return r == (long)&x; }\n"},
             Case{"arm64",
                  "int f(void) { long x = 42, r;\n"
                  "  __asm__(\"mov %0, %1\" : \"=r\"(r) : \"r\"(&x) : \"x9\");\n"
                  "  return r == (long)&x; }\n"},
             // a CARRIED operand (its address is a frame slot) beside a clobber
             Case{"x86_64",
                  "long f(__int128 v) { long a;\n"
                  "  __asm__(\"movq %1, %0\" : \"=r\"(a) : \"r\"(v) : \"r8\");\n"
                  "  return a; }\n"},
             // a memory-form OUTPUT beside a clobber
             Case{"x86_64",
                  "long f(long y) { long r;\n"
                  "  __asm__(\"movq %1, %0\" : \"=m\"(r) : \"r\"(y) : \"r8\");\n"
                  "  return r; }\n"}}) {
        auto target = TargetSchema::loadShipped(c.target);
        ASSERT_TRUE(target.has_value());
        TargetSchema const& t = **target;
        auto r = lowerCToLir(c.src, *target);
        ASSERT_FALSE(r.lirReporter.hasErrors()) << c.target << summarize(r);
        Lir const& lir = r.lir.lir;
        std::size_t constrained = 0, leas = 0;
        for (auto const& p : allInsts(lir)) {
            if (lir.instRegConstraintHandle(p.id) != kLirNoRegConstraints) ++constrained;
            if (lir.instOpcode(p.id) != opOf(t, "lea_frame_slot")) continue;
            ++leas;
            EXPECT_EQ(lir.instRegConstraintHandle(p.id), kLirNoRegConstraints)
                << c.target << ": a `lea_frame_slot` inside the statement's "
                   "constrained range — `lir_callconv` rewrites it without its "
                   "side data and the module is refused "
                   "(L_SideStructureReferenceLost)\n" << c.src;
        }
        EXPECT_GT(constrained, 0u)
            << c.target << ": the statement carries a clobber, so its own "
               "instructions must carry the constraint — the arm is vacuous "
               "without it";
        EXPECT_GT(leas, 0u)
            << c.target << ": the snippet must take a frame address somewhere, "
               "or this arm asserts nothing";
    }
}

// ── (E) REFUSED BY NAME, EACH BESIDE AN ACCEPTED NEIGHBOUR ───────────────────
TEST(LirAsmCarriedOperand, ASizeNoRegisterArrangementCarriesIsRefusedByName) {
    auto bad = lowerCToLir(
        "struct s3 { unsigned char b[3]; };\n"
        "unsigned f(struct s3 v) { unsigned a;\n"
        "  __asm__(\"mov %w0, %w1\" : \"=r\"(a) : \"r\"(v)); return a; }\n",
        "arm64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyText(bad, "3-byte 'Struct' value")) << summarize(bad);
    EXPECT_TRUE(anyText(bad, "`asmValueCarriage`")) << summarize(bad);
    EXPECT_TRUE(anyText(bad, "operand '%1'"))
        << "the refusal names the operand by what the source wrote"
        << summarize(bad);
    // gcc 13.3.0 refuses it too: "impossible constraint in 'asm'". CONTROL: a
    // 4-byte struct on the same letter is carried.
    auto ok = lowerCToLir(
        "struct s4 { unsigned char b[4]; };\n"
        "unsigned f(struct s4 v) { unsigned a;\n"
        "  __asm__(\"mov %w0, %w1\" : \"=r\"(a) : \"r\"(v)); return a; }\n",
        "arm64");
    EXPECT_FALSE(ok.lirReporter.hasErrors()) << summarize(ok);
}

TEST(LirAsmCarriedOperand, ADirectionNoReferenceCarriesIsRefusedByName) {
    // clang 18.1.3 carries a `_Complex double` in one Q register as an OUTPUT;
    // as an input gcc hands the template another register and clang refuses.
    auto bad = lowerCToLir(
        "unsigned long f(_Complex double z) { unsigned long a;\n"
        "  __asm__(\"fmov %0, %d1\" : \"=r\"(a) : \"w\"(z)); return a; }\n",
        "arm64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyText(bad, "only as 'out'")) << summarize(bad);
    auto ok = lowerCToLir(
        "void f(_Complex double *p, unsigned long x) { _Complex double z;\n"
        "  __asm__(\"fmov %d0, %1\" : \"=w\"(z) : \"r\"(x)); *p = z; }\n",
        "arm64");
    EXPECT_FALSE(ok.lirReporter.hasErrors()) << summarize(ok);
}

TEST(LirAsmCarriedOperand, APinnedRegisterAskedToHoldAPairIsRefusedByName) {
    // gcc 13.3.0 ("inconsistent operand constraints in an 'asm'") and clang
    // 18.1.3 ("couldn't allocate input reg") both refuse `"a"(__int128)`.
    auto bad = lowerCToLir(
        "long f(__int128 v) { long a;\n"
        "  __asm__(\"movq %%rax, %0\" : \"=r\"(a) : \"a\"(v)); return a; }\n",
        "x86_64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyText(bad, "pins ONE")) << summarize(bad);
    // CONTROL: the pinned letter with a value that fits one register.
    auto ok = lowerCToLir(
        "struct s8 { unsigned int a, b; };\n"
        "long f(struct s8 v) { long a;\n"
        "  __asm__(\"movq %%rax, %0\" : \"=r\"(a) : \"a\"(v)); return a; }\n",
        "x86_64");
    EXPECT_FALSE(ok.lirReporter.hasErrors()) << summarize(ok);
}

TEST(LirAsmCarriedOperand, ABitPreciseIntegerIsRefusedByName) {
    // clang 18.1.3: "invalid type '_BitInt(128)' in asm input" for every width;
    // gcc 13.3.0 has no `_BitInt`. A wide one is by-address here, so it is the
    // front end that refuses it — never passing its address.
    auto bad = lowerCToLir(
        "unsigned long f(_BitInt(128) v) { unsigned long a;\n"
        "  __asm__(\"movq %1, %0\" : \"=r\"(a) : \"r\"(v)); return a; }\n",
        "x86_64", 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyCode(bad, DiagnosticCode::H_UnsupportedLoweringForKind))
        << summarize(bad);
    EXPECT_TRUE(anyText(bad, "`_BitInt(128)`")) << summarize(bad);
}

// ── (F) THE VOCABULARY'S LOAD-TIME CONTRACT ──────────────────────────────────
TEST(LirAsmCarriedOperand, EveryMalformedCarriageRowIsRefusedAtLoad) {
    struct Case {
        char const* what;
        std::function<void(nlohmann::json&)> edit;
        char const* expect;
    };
    std::vector<Case> const cases{
        {"a register-resident kind",
         [](nlohmann::json& row) { row["carries"][0]["kinds"] = {"I64"}; },
         "not a kind the pipeline keeps in memory"},
        {"a zero size",
         [](nlohmann::json& row) { row["carries"][0]["bytes"] = {0}; },
         "positive byte count"},
        {"an unknown direction",
         [](nlohmann::json& row) { row["carries"][0]["directions"] = {"sideways"}; },
         "expected \"in\", \"out\" or \"inout\""},
        {"a size listed twice",
         [](nlohmann::json& row) { row["carries"][0]["bytes"] = {8, 8}; },
         "twice"},
        {"an unknown key",
         [](nlohmann::json& row) { row["carries"][0]["pairs"] = true; },
         "pairs"},
        {"a class staging through itself",
         [](nlohmann::json& row) { row["stagesThrough"] = "gpr"; },
         "stages through itself"},
    };
    for (auto const& c : cases) {
        auto const r = withCarriageRow("x86_64", "gpr", c.edit);
        std::string const why = loadErrors(r);
        EXPECT_FALSE(why.empty()) << c.what << " must be refused at load";
        EXPECT_NE(why.find(c.expect), std::string::npos)
            << c.what << ": expected a message containing '" << c.expect
            << "', got:\n" << why;
    }
    // A second row for one class would be two lists answering one question.
    auto const dup = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        auto rows = doc.at("asmValueCarriage");
        doc["asmValueCarriage"].push_back(rows[0]);
    });
    EXPECT_NE(loadErrors(dup).find("one row per class"), std::string::npos)
        << loadErrors(dup);
}

TEST(LirAsmCarriedOperand, ATargetThatDeclaresNoCarriageRefusesTheOperandByName) {
    // ★ THE CONFIG'S RED-ON-DISABLE: the carriage is the TARGET's declaration,
    // and without it the same source that lowers below is refused by name —
    // never handed its address.
    auto stripped = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        doc.erase("asmValueCarriage");
    });
    ASSERT_TRUE(stripped.has_value()) << loadErrors(stripped);
    char const* const src =
        "long f(__int128 v) { long a;\n"
        "  __asm__(\"movq %1, %0\" : \"=r\"(a) : \"r\"(v)); return a; }\n";
    auto bad = lowerCToLir(src, *stripped, 0, LoweringExpectation::Refuses);
    EXPECT_TRUE(anyText(bad, "carries no 16-byte 'I128' value")) << summarize(bad);
    auto ok = lowerCToLir(src, "x86_64");
    EXPECT_FALSE(ok.lirReporter.hasErrors()) << summarize(ok);
}

TEST(LirAsmCarriedOperand, TheSecondRegisterSelectorIsAClosedKeyIndependentOfScope) {
    EXPECT_TRUE(loadDialectWithHRow([](nlohmann::json&) {}).empty())
        << "the shipped `H` row must load: "
        << loadDialectWithHRow([](nlohmann::json&) {});
    std::string const other = loadDialectWithHRow(
        [](nlohmann::json& r) { r["selects"] = "pairFirst"; });
    EXPECT_NE(other.find("pairSecond"), std::string::npos)
        << "a selector the engine does not know would load as a plain view of "
           "the FIRST register: " << other;
    // The selector is NOT a scope rule: a pair lives in the one file its
    // operand is bound in, so an ALL-unscoped document — the width-only
    // posture x86-64's dialect uses, which the loader must accept — carries
    // `H` like any other letter (a part-2 rule that demanded a class here
    // refused exactly that document; asm/test_asm_class_scoped_modifiers
    // caught it).
    std::string const unscoped = loadDialectWithEveryRow(
        [](nlohmann::json& r) { r.erase("registerClass"); });
    EXPECT_TRUE(unscoped.empty())
        << "an all-unscoped document carrying the selector must load: "
        << unscoped;
}

// ── (G) `movaps` NAMES A 128-BIT XMM OPERAND ─────────────────────────────────
TEST(LirAsmCarriedOperand, MovapsEncodesA128BitXmmOperand) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    // ✔MEASURED at `7df54cc1`: both statements were refused A_AsmTextUnsupported
    // ("'movaps' operates on 128 bits, but … declares no width-keyed encoding
    // variant") — the literal registers derive 128, and so does a carried
    // 16-byte value.
    auto r = lowerCToLir(
        "void f(__int128 *p, __int128 w) { __int128 v = *p;\n"
        "  __asm__(\"movaps %1, %0\" : \"+x\"(v) : \"x\"(w));\n"
        "  __asm__ volatile(\"movaps %%xmm1, %%xmm2\" ::: \"xmm2\");\n"
        "  *p = v; }\n",
        *target);
    ASSERT_FALSE(r.lirReporter.hasErrors()) << summarize(r);
    // P68 round 8 part 4: each statement's template is its bundle's BODY.
    std::size_t wide = 0;
    for (LirInstId const b : asmRegionBundles(r.lir.lir)) {
        LirAsmRegion const& region = *r.lir.lir.instAsmRegion(b);
        for (LirInstId const i : asmRegionBodyInsts(region)) {
            if (region.body.instOpcode(i) != opOf(t, "movaps")) continue;
            if (lirInstWidthBits(region.body.instFlags(i)) == 128) ++wide;
        }
    }
    EXPECT_GE(wide, 2u) << "both template `movaps` run at 128 bits";
}

// ── (H) AN ARRAY INPUT IS THE ADDRESS OF ITS FIRST ELEMENT ───────────────────
// P68 round 8, D-ASM-ARRAY-INPUT-OPERAND-NOT-DECAYED. C 6.3.2.1p3: an array
// expression's VALUE is a pointer to its first element, and a register-bound
// input takes its operand's value. ✔MEASURED 2026-09-21: gcc 13.3.0 and clang
// 18.1.3 hand `"r"(cells)` the address on both processors; the round-7 base
// carried the ARRAY and refused it ("binds a 16-byte 'Array' value"). The `"m"`
// input is the CONTROL — a memory form names the OBJECT and keeps it.
TEST(LirAsmCarriedOperand, AnArrayInputIsTheAddressOfItsFirstElement) {
    for (auto const& [target, tmpl] :
         {std::pair{"x86_64", "movq %1, %0"}, std::pair{"arm64", "mov %0, %1"}}) {
        SCOPED_TRACE(target);
        auto r = lowerCToLir(
            std::string{"long f(void) { long cells[2]; long r;\n"
                        "  cells[0] = 1; cells[1] = 2;\n"
                        "  __asm__(\""} + tmpl + "\" : \"=r\"(r) : \"r\"(cells));\n"
            "  return r; }\n",
            target);
        ASSERT_FALSE(r.lirReporter.hasErrors()) << summarize(r);
        Mir const& mir = r.mir.mir;
        MirInstId const a = theAsm(mir);
        ASSERT_TRUE(a.valid());
        auto const ops = mir.instOperands(a);
        ASSERT_EQ(ops.size(), 1u) << "one source input";
        EXPECT_EQ(r.model.lattice().interner().kind(mir.instType(ops[0])),
                  TypeKind::Ptr)
            << "the input's value is a POINTER — the array decayed";
        EXPECT_EQ(mir.asmDescriptor(a).inputs[0].carriedBytes, 0u)
            << "a pointer is register-resident: nothing is carried by address";
    }
    // The control: `"m"` keeps the object.
    auto m = lowerCToLir(
        "long g(void) { long cells[2]; long r; cells[0] = 1; cells[1] = 2;\n"
        "  __asm__(\"movq %1, %0\" : \"=r\"(r) : \"m\"(cells)); return r; }\n",
        "x86_64");
    ASSERT_FALSE(m.lirReporter.hasErrors()) << summarize(m);
}
