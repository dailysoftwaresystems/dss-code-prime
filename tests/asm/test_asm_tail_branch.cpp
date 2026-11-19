// P69 fold H5, lane p69/hm -- a direct TAIL BRANCH TO A SYMBOL in the asm route (the orchestrator's ledger 440).
//
// gas assembles `jmp ext` -- a branch whose target is no label of the function -- as E9 + a 4-byte displacement
// relocated against `ext`: control leaves the function for the symbol's code. It is the shape a hand-written `.s`
// tail-calls with, and the one MSVC's own `__security_check_cookie` ends its failure path with; DSS's pe
// `compilerSupport` runtime (src/dss-config/runtime/platform/src/pe_compiler_support_x86_64.s) needs it. Before
// this fold the asm route refused it ("a branch out of the translation unit is a relocation this build does not
// reach from assembly yet", ✔MEASURED, Mac run 20261029-023452-cd6c5369).
//
// THE MECHANISM: a direct branch whose target the HOST says leaves the function (a function entry of this unit, or a
// name it does not define) is resolved as `resolveCallee` resolves a callee and built as a no-successor terminator,
// elected among the row's RETURN-class opcodes that take a symbol -- the target's tail branch (x86_64.target.json's
// `jmp_tail`, E9 + disp32 through the rel32 relocation; the att dialect's `jmp` row names it). A row naming none
// keeps refusing, by name. No target or dialect name is in the code.
//
// Every TEST but the image ones reads the lowering; the images are linked on every leg and run where the host is
// the image's own platform.

#include "asm_text_fixture.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "program/program.hpp"

#include "host_native_target.hpp"
#include "mutate_target_schema.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;

namespace {

namespace fs = std::filesystem;

std::uint16_t op(TargetSchema const& schema, std::string_view mnemonic) {
    auto const v = schema.opcodeByMnemonic(mnemonic);
    EXPECT_TRUE(v.has_value()) << "unknown mnemonic " << mnemonic;
    return v.value_or(0);
}

// The LIR function the file's label `name` opened, or nullopt.
std::optional<LirFuncId> functionNamed(AsmTextModule const& m, std::string_view name) {
    for (std::size_t i = 0; i < m.symbols.size(); ++i) {
        if (m.symbols[i].name == name) return m.lir.funcAt(static_cast<std::uint32_t>(i));
    }
    return std::nullopt;
}

// The terminator closing the function's LAST block.
LirInstId lastTerminator(Lir const& lir, LirFuncId fn) {
    auto const blk = lir.funcBlockAt(fn, lir.funcBlockCount(fn) - 1u);
    return lir.blockInstAt(blk, lir.blockInstCount(blk) - 1u);
}

// The shipped att dialect, its `jmp` row naming the opcodes `opcodes` instead.
nlohmann::json dialectWithJmp(std::vector<std::string> const& opcodes) {
    auto doc = shippedDialectDoc("asm-x86_64-att");
    for (auto& row : doc["assembly"]["instructions"]) {
        if (row.value("spelling", std::string{}) == "jmp") row["opcodes"] = opcodes;
    }
    return doc;
}

constexpr char const* kToAnImport =
    "\t.text\n\t.globl\tf\n\t.type\tf, @function\nf:\n\tjmp\text\n";

}  // namespace

// ══ THE LOWERING ══════════════════════════════════════════════════════════════════════════════════════════════════

// `jmp ext`, `ext` defined nowhere in the file: the tail branch, its one operand the import minted for `ext` -- a
// branch states CODE, as a call does -- and no successor.
TEST(AsmTailBranch, AJumpToANameTheFileDoesNotDefineIsATailBranchToAnImport) {
    auto const run = lowerAsmText(shippedDialectDoc("asm-x86_64-att"), kToAnImport);
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    auto const& m = *run->module;
    ASSERT_EQ(m.externImports.size(), 1u) << "the branch mints exactly one import";
    EXPECT_EQ(m.externImports[0].mangledName, "ext");
    EXPECT_EQ(m.externImports[0].kindOrigin, ExternKindOrigin::Stated) << "a branch states code";
    EXPECT_FALSE(m.externImports[0].isData);
    auto const fn = functionNamed(m, "f");
    ASSERT_TRUE(fn.has_value());
    auto const term = lastTerminator(m.lir, *fn);
    EXPECT_EQ(m.lir.instOpcode(term), op(*run->target, "jmp_tail"));
    auto const ops = m.lir.instOperands(term);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, LirOperandKind::SymbolRef);
    EXPECT_EQ(ops[0].symbolV, m.externImports[0].symbol.v) << "the branch relocates against the import";
    EXPECT_TRUE(m.lir.blockSuccessors(m.lir.funcBlockAt(*fn, 0)).empty()) << "a tail branch has no successor";
}

// `jmp g`, `g` another function's entry in the same file: the tail branch to g's own symbol, no import.
TEST(AsmTailBranch, AJumpToAFunctionTheFileDefinesIsATailBranchToItsSymbol) {
    auto const run = lowerAsmText(shippedDialectDoc("asm-x86_64-att"),
                                  "\t.text\n\t.globl\tf\n\t.type\tf, @function\nf:\n\tjmp\tg\n"
                                  "\t.globl\tg\n\t.type\tg, @function\ng:\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    auto const& m = *run->module;
    EXPECT_TRUE(m.externImports.empty()) << "g is defined here: nothing is imported";
    auto const f = functionNamed(m, "f");
    auto const g = functionNamed(m, "g");
    ASSERT_TRUE(f.has_value() && g.has_value());
    auto const term = lastTerminator(m.lir, *f);
    EXPECT_EQ(m.lir.instOpcode(term), op(*run->target, "jmp_tail"));
    auto const ops = m.lir.instOperands(term);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, LirOperandKind::SymbolRef);
    std::uint32_t gSymbol = 0;
    for (auto const& s : m.symbols) {
        if (s.name == "g") gSymbol = s.symbol.v;
    }
    EXPECT_EQ(ops[0].symbolV, gSymbol);
}

// A block of the branch's own function stays the block branch it always was.
TEST(AsmTailBranch, AJumpToABlockOfItsOwnFunctionIsStillABranch) {
    auto const run = lowerAsmText(shippedDialectDoc("asm-x86_64-att"),
                                  "\t.text\n\t.globl\tf\n\t.type\tf, @function\nf:\n\tjmp\t1f\n1:\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    ASSERT_TRUE(run->module.has_value()) << messages(*run);
    auto const& m   = *run->module;
    auto const  fn  = functionNamed(m, "f");
    ASSERT_TRUE(fn.has_value());
    auto const blk  = m.lir.funcBlockAt(*fn, 0);
    auto const term = m.lir.blockInstAt(blk, m.lir.blockInstCount(blk) - 1u);
    EXPECT_EQ(m.lir.instOpcode(term), op(*run->target, "jmp"));
    EXPECT_EQ(m.lir.blockSuccessors(blk).size(), 1u);
    EXPECT_TRUE(m.externImports.empty());
}

// A block of ANOTHER function is no way out: refused, as before.
TEST(AsmTailBranch, AJumpToABlockOfAnotherFunctionIsStillRefused) {
    auto const run = lowerAsmText(shippedDialectDoc("asm-x86_64-att"),
                                  "\t.text\n\t.globl\tg\n\t.type\tg, @function\ng:\n\tret\ninner:\n\tret\n"
                                  "\t.globl\tf\n\t.type\tf, @function\nf:\n\tjmp\tinner\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("belongs to a different function"), std::string::npos) << messages(*run);
}

// A spelling whose row names no tail branch keeps refusing a branch out of the function, by name.
TEST(AsmTailBranch, ARowThatNamesNoTailBranchKeepsTheRefusal) {
    auto const run = lowerAsmText(dialectWithJmp({"jmp", "jmp_indirect"}), kToAnImport);
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    auto const said = messages(*run);
    EXPECT_NE(said.find("'jmp' branches to 'ext', which this file defines no label for"), std::string::npos) << said;
    EXPECT_NE(said.find("declares no opcode this spelling names that branches to a symbol"), std::string::npos)
        << said;
}

// A TARGET that declares no tail branch keeps the refusal too, though the dialect's row names one: the candidate
// the target does not declare is dropped, and the branch has no way out.
TEST(AsmTailBranch, ATargetThatDeclaresNoTailBranchKeepsTheRefusal) {
    auto schema = test_support::mutateShippedTargetSchemaJson("x86_64", {"jmp_tail"});
    ASSERT_TRUE(schema.has_value()) << "the x86_64 target without its tail branch must still load";
    auto const run = lowerAsmTextWithTarget(shippedDialectDoc("asm-x86_64-att"), kToAnImport, *schema);
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("'jmp' branches to 'ext', which this file defines no label for"),
              std::string::npos)
        << messages(*run);
}

// The branch relocation names the symbol alone, so a constant past it is refused rather than dropped.
TEST(AsmTailBranch, AConstantPastTheSymbolIsRefused) {
    auto const run = lowerAsmText(shippedDialectDoc("asm-x86_64-att"),
                                  "\t.text\n\t.globl\tf\n\t.type\tf, @function\nf:\n\tjmp\text+4\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("an address a constant past a symbol"), std::string::npos) << messages(*run);
}

// A tail branch is the out-of-function arm of a BRANCH spelling: a row naming it beside no branch is refused when
// the dialect is resolved, before any line is read.
TEST(AsmTailBranch, ATailBranchBesideNoBranchIsRefusedAtResolution) {
    auto doc = shippedDialectDoc("asm-x86_64-att");
    doc["assembly"]["instructions"].push_back({{"spelling", "tj"}, {"opcodes", {"jmp_tail"}}, {"width", 64}});
    auto const run = lowerAsmText(doc, "\t.text\n\t.globl\tf\n\t.type\tf, @function\nf:\n\tret\n");
    ASSERT_TRUE(parsedCleanly(*run)) << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value());
    EXPECT_NE(messages(*run).find("beside no unconditional branch"), std::string::npos) << messages(*run);
}

// ══ THE IMAGE ═════════════════════════════════════════════════════════════════════════════════════════════════════

namespace {

// `main` tail-branches to `helper`, a function of the same file, which tail-branches to `forty_two`, a C function of
// another unit: the program ends with forty_two's value, through two frames that never return to themselves.
constexpr char const* kTwoTailBranches =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\nmain:\n\tjmp\thelper\n"
    "\t.globl\thelper\n\t.type\thelper, @function\nhelper:\n\tjmp\tforty_two\n";

struct Format { char const* object; char const* exec; char const* image; };
constexpr Format kFormats[] = {
    {"x86_64:pe64-x86_64-windows", "x86_64:pe64-x86_64-windows-exec", "main.exe"},
    {"x86_64:elf64-x86_64-linux", "x86_64:elf64-x86_64-linux-exec", "main"},
};

std::string said(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

// The C unit's object for `f`, and the image of the assembly beside it; the image's path, or empty after a failure.
fs::path linkedImage(fs::path const& dir, Format const& f) {
    fs::create_directories(dir / "obj");
    fs::create_directories(dir / "exe");
    std::ofstream(dir / "forty_two.c", std::ios::binary) << "int forty_two(void) { return 42; }\n";
    std::ofstream(dir / "main.s", std::ios::binary) << kTwoTailBranches;
    {
        Program            p;
        DiagnosticReporter rep;
        p.setOutputDir(dir / "obj");
        if (p.compileFiles(std::vector<std::string>{(dir / "forty_two.c").string()}, "c",
                           std::vector<std::string>{f.object}, rep) != 0) {
            ADD_FAILURE() << "the C unit did not compile for " << f.object << ":" << said(rep);
            return {};
        }
    }
    fs::path object;
    for (auto const& e : fs::directory_iterator(dir / "obj")) {
        if (e.is_regular_file()) object = e.path();
    }
    if (object.empty()) {
        ADD_FAILURE() << "the C unit wrote no object for " << f.object;
        return {};
    }
    Program            p;
    DiagnosticReporter rep;
    p.setOutputDir(dir / "exe");
    if (p.compileFiles(std::vector<std::string>{(dir / "main.s").string(), object.string()}, "asm-x86_64-att",
                       std::vector<std::string>{f.exec}, rep) != 0) {
        ADD_FAILURE() << "DSS refused the image for " << f.exec << ":" << said(rep);
        return {};
    }
    return dir / "exe" / f.image;
}

}  // namespace

// The program links for pe and for ELF on every leg, and runs to 42 where the host is the image's platform.
TEST(AsmTailBranchImage, TwoTailBranchesLinkOnEveryLegAndRunWhereTheHostCan) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "asm-tail-branch"};
    for (auto const& f : kFormats) {
        SCOPED_TRACE(f.exec);
        auto const image = linkedImage(scratch.path() / (std::string{f.exec}.substr(7)), f);
        ASSERT_FALSE(image.empty());
        if (test_support::hostNativeTarget().execTarget != std::string_view{f.exec}) continue;
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the program did not end with forty_two's value";
    }
}
