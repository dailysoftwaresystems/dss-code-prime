// OPT11 — THE `.dss.mir` BODY PAYLOAD (plan 22 §0.2, D-OPT11-LAZY-IMPORT-EDGE).
//
// `.dss.summary` carries the DECISION material and `test_mir_summary.cpp` pins
// it. This file pins the other section: the BODIES a per-TU optimizer pages in.
//
// ★★★ THE PIN THAT MAKES THE SECTION REAL RATHER THAN DECORATIVE is
// `ADecodedBodyImportsIdenticallyToTheInMemoryOne`: the shipped in-process
// driver fetches from a live `Mir`, so nothing would notice if the encoded form
// were subtly lossy. Importing THROUGH the codec and comparing the resulting
// module against the direct import is what proves the encoded body is a faithful
// substitute — which is the whole premise of a fat object and of Tier-2 caching.
//
// The refusal arms are not decoration either. A body is an input to codegen
// DECISIONS, so a misread one is a miscompile: every malformed shape must be a
// REFUSAL, and a STALE body — the right shape, the wrong build — must be a
// refusal too, because it is the one failure that produces a plausible artifact.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/summary/lazy_import_optimize.hpp"
#include "mir/summary/mir_body_codec.hpp"
#include "mir/summary/mir_summary.hpp"
#include "mir/summary/summary_index.hpp"

#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::mirsum;

namespace {

constexpr char const* kTarget = "x86_64:elf64-x86_64-linux-exec";

struct TestCu {
    std::unique_ptr<TypeLattice> lattice;
    Mir                          mir;
    std::vector<std::string>     names;
    std::vector<ExternImport>    externs;
};

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// `int <self>(void) { return <callee>(); }` or `{ return <konst>; }`.
TestCu makeLink(std::uint32_t cuIdx, std::string self, std::string callee,
                std::int64_t konst) {
    TestCu cu;
    cu.lattice = std::make_unique<TypeLattice>(CompilationUnitId{cuIdx + 1}, "c");
    TypeId const i32 = cu.lattice->interner().primitive(TypeKind::I32);
    TypeId const sig = cu.lattice->interner().fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    if (callee.empty()) {
        mb.addReturn(mb.addConst(i32Lit(konst), i32));
    } else {
        MirInstId const addr  = mb.addGlobalAddr(SymbolId{10}, sig);
        MirInstId const ops[] = {addr};
        mb.addReturn(mb.addInst(MirOpcode::Call, ops, i32));
    }
    cu.mir = std::move(mb).finish();
    cu.names.assign(101, std::string{});
    cu.names[100] = std::move(self);
    if (!callee.empty()) {
        cu.names[10] = callee;
        cu.externs.push_back(ExternImport{SymbolId{10}, callee, ""});
    }
    return cu;
}

std::vector<ModuleSummary> summarize(std::vector<TestCu> const& cus) {
    std::vector<ModuleSummary> out;
    for (std::uint32_t i = 0; i < cus.size(); ++i) {
        SummaryCuInput in;
        in.mir    = &cus[i].mir;
        in.nameOf = [&names = cus[i].names](SymbolId s) {
            return s.v < names.size() ? names[s.v] : std::string{};
        };
        in.externImports  = cus[i].externs;
        in.moduleDigest   = std::format("digest-{}", i);
        in.targetIdentity = kTarget;
        out.push_back(buildModuleSummary(in));
    }
    return out;
}

std::string structuralDump(Mir const& mir,
                           std::vector<std::string> const& names) {
    auto nameOf = [&](SymbolId s) {
        return s.v < names.size() && !names[s.v].empty()
                   ? names[s.v] : std::format("<anon:{}>", s.v);
    };
    std::vector<std::string> funcs;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::string text = std::format("fn {} blocks={}\n",
                                       nameOf(mir.funcSymbol(f)),
                                       mir.funcBlockCount(f));
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const inst = mir.blockInstAt(b, ii);
                MirOpcode const op   = mir.instOpcode(inst);
                text += std::format("    {}", static_cast<int>(op));
                if (op == MirOpcode::GlobalAddr)
                    text += " sym=" + nameOf(mir.globalAddrSymbol(inst));
                if (op == MirOpcode::Const) {
                    auto const& lit =
                        mir.literalValue(mir.constLiteralIndex(inst));
                    if (auto const* iv = std::get_if<std::int64_t>(&lit.value))
                        text += std::format(" lit={}", *iv);
                }
                text += "\n";
            }
        }
        funcs.push_back(std::move(text));
    }
    std::sort(funcs.begin(), funcs.end());
    std::string all;
    for (std::string const& f : funcs) all += f;
    return all;
}

std::vector<std::uint8_t> encodeOne(TestCu const& cu, DiagnosticReporter& rep) {
    return encodeModuleBody(cu.mir, cu.lattice->interner(), cu.names,
                            "digest-1", kTarget, rep);
}

} // namespace

TEST(MirBodyCodec, RoundTripPreservesTheModule) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto const bytes = encodeOne(cu, rep);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);

    auto decoded = decodeModuleBody(bytes, CompilationUnitId{9}, "digest-1",
                                    kTarget, rep);
    ASSERT_TRUE(decoded.has_value()) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(decoded->moduleDigest, "digest-1");
    EXPECT_EQ(decoded->targetIdentity, kTarget);
    EXPECT_EQ(structuralDump(decoded->parsed->mir, decoded->parsed->symbolNames),
              structuralDump(cu.mir, cu.names));
}

TEST(MirBodyCodec, RefusesABadMagic) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto bytes = encodeOne(cu, rep);
    ASSERT_FALSE(bytes.empty());
    bytes[0] = 'X';
    DiagnosticReporter bad;
    EXPECT_FALSE(decodeModuleBody(bytes, CompilationUnitId{9}, "", "", bad)
                     .has_value());
    EXPECT_GT(bad.errorCount(), 0u);
}

TEST(MirBodyCodec, RefusesAnUnknownVersion) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto bytes = encodeOne(cu, rep);
    ASSERT_GT(bytes.size(), 12u);
    bytes[8] = static_cast<std::uint8_t>(kBodyFormatVersion + 1);
    DiagnosticReporter bad;
    EXPECT_FALSE(decodeModuleBody(bytes, CompilationUnitId{9}, "", "", bad)
                     .has_value());
    EXPECT_GT(bad.errorCount(), 0u);
}

TEST(MirBodyCodec, RefusesATruncatedBuffer) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto bytes = encodeOne(cu, rep);
    ASSERT_GT(bytes.size(), 40u);
    bytes.resize(bytes.size() - 10);
    DiagnosticReporter bad;
    EXPECT_FALSE(decodeModuleBody(bytes, CompilationUnitId{9}, "", "", bad)
                     .has_value());
    EXPECT_GT(bad.errorCount(), 0u);
}

// Trailing bytes mean the buffer is not what its header says it is. Accepting
// them is how a reader silently decodes the first of two concatenated payloads.
TEST(MirBodyCodec, RefusesBytesPastTheDeclaredPayload) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto bytes = encodeOne(cu, rep);
    ASSERT_FALSE(bytes.empty());
    bytes.push_back(0);
    DiagnosticReporter bad;
    EXPECT_FALSE(decodeModuleBody(bytes, CompilationUnitId{9}, "", "", bad)
                     .has_value());
    EXPECT_GT(bad.errorCount(), 0u);
}

// ★★★ THE STALE-BODY GUARD. This is the only failure mode that produces a
// PLAUSIBLE artifact — the right shape from the wrong build — so it is the one
// that must never be a warning.
TEST(MirBodyCodec, RefusesAStaleDigest) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto const bytes = encodeOne(cu, rep);
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter bad;
    EXPECT_FALSE(decodeModuleBody(bytes, CompilationUnitId{9},
                                  "digest-from-the-previous-build", kTarget, bad)
                     .has_value());
    EXPECT_GT(bad.errorCount(), 0u);
}

TEST(MirBodyCodec, RefusesABodyBuiltForAnotherTarget) {
    TestCu const cu = makeLink(1, "f1", "f2", 0);
    DiagnosticReporter rep;
    auto const bytes = encodeOne(cu, rep);
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter bad;
    EXPECT_FALSE(decodeModuleBody(bytes, CompilationUnitId{9}, "digest-1",
                                  "arm64:macho64-arm64-darwin-exec", bad)
                     .has_value());
    EXPECT_GT(bad.errorCount(), 0u);
}

// ★★★ THE PIN THAT MAKES `.dss.mir` REAL: a body fetched THROUGH the codec
// imports into exactly the module a body fetched from memory does.
TEST(MirBodyCodec, ADecodedBodyImportsIdenticallyToTheInMemoryOne) {
    auto const importThrough = [](bool viaCodec) {
        std::vector<TestCu> cus;
        cus.push_back(makeLink(0, "main", "f1", 0));
        cus.push_back(makeLink(1, "f1", "", 7));
        auto const summaries = summarize(cus);

        DiagnosticReporter rep;
        std::optional<DecodedModuleBody> decoded;
        std::vector<std::string> decodedNames;
        if (viaCodec) {
            auto const bytes =
                encodeModuleBody(cus[1].mir, cus[1].lattice->interner(),
                                 cus[1].names, "digest-1", kTarget, rep);
            EXPECT_FALSE(bytes.empty());
            decoded = decodeModuleBody(bytes, CompilationUnitId{2}, "digest-1",
                                       kTarget, rep);
            EXPECT_TRUE(decoded.has_value());
            decodedNames = decoded->parsed->symbolNames;
        }

        std::vector<LazyImportCu> views;
        views.push_back(LazyImportCu{&cus[0].mir, &cus[0].lattice->interner(),
                                     &cus[0].names, cus[0].externs});
        if (viaCodec) {
            views.push_back(LazyImportCu{&decoded->parsed->mir,
                                         &decoded->parsed->interner,
                                         &decodedNames, cus[1].externs});
        } else {
            views.push_back(LazyImportCu{&cus[1].mir,
                                         &cus[1].lattice->interner(),
                                         &cus[1].names, cus[1].externs});
        }

        SummaryIndexPolicy policy;
        policy.maxImportDepth = 4;
        auto index = buildSummaryIndex(summaries, policy, rep);
        EXPECT_TRUE(index.has_value());
        EXPECT_TRUE(summariesDescribeModules(views, summaries, rep));

        auto const outcome = lazyImportOptimize(
            0, views, summaries, *index, policy, "c",
            [](Mir&, TypeInterner const&, std::span<ExternImport const>) {
                return true;
            },
            rep);
        EXPECT_TRUE(outcome.ok);
        EXPECT_EQ(outcome.importedBodies, 1u);
        EXPECT_EQ(rep.errorCount(), 0u);

        std::uint32_t maxV = 0;
        for (auto const& [v, n] : outcome.symbolNames) maxV = std::max(maxV, v);
        std::vector<std::string> names(static_cast<std::size_t>(maxV) + 1);
        for (auto const& [v, n] : outcome.symbolNames) names[v] = n;
        return structuralDump(*outcome.mir, names);
    };

    EXPECT_EQ(importThrough(/*viaCodec=*/false), importThrough(true))
        << "a body paged in from a `.dss.mir` payload must produce the SAME "
           "module as one taken from the live Mir — otherwise the section is a "
           "different compiler";
}
