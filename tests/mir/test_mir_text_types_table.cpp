// P68 round 8, lane `ht`, part 1d — THE `.dssir` v2 `types` TABLE.
//
// The MIR text v1 spelled every struct and union INLINE, at every use, with its field TYPES only. Three defects
// rode that one spelling, each MEASURED before the change:
//   * the SIZE — the spelling is O(mentions × graph): `emitMir` of sqlite3.c's module ran to 30 GB and
//     `std::bad_alloc`;
//   * the CYCLE — a self-referential composite (`struct S { int v; struct S *next; }`, the commonest shape in C) had
//     no spelling at all: the writer refused it with a `?` mark and an Error, so no module holding a linked list could
//     be written;
//   * the LAYOUT — none of the channels `TypeInterner::completeComposite` takes travelled, so a packed, bit-field,
//     `aligned`, `#pragma pack`, explicit-offset or member-aligned composite read back with a DIFFERENT layout while
//     its round trip stayed byte-identical (the re-emission spells the stripped type exactly as the first one did).
//
// v2 is the MIR twin of the HIR v5 table: a composite is `type <H>`, DEFINED ONCE in a `types` section with every
// channel, through the ONE owner both tiers share (`core/types/type_lattice/composite_definition.hpp`). These pins
// hold each of the three, the refusals a reader of text it did not write owes, the spelling the two tiers share, and
// the `.dss.mir` body payload the first product caller of the codec will inherit.

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/hir.hpp"
#include "hir/hir_text.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_text.hpp"
#include "mir/summary/mir_body_codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// The `types { … }` section of a text, closing brace included; empty when there is none.
[[nodiscard]] std::string typesSectionOf(std::string const& text) {
    std::size_t const at = text.find("types {\n");
    if (at == std::string::npos) return {};
    std::size_t const end = text.find("\n}\n", at);
    return end == std::string::npos ? std::string{} : text.substr(at, end + 3 - at);
}

[[nodiscard]] std::size_t occurrences(std::string const& text, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// A module whose one function `%1` takes `params` and returns: the composites reach the text only through the
// signature, which is exactly a composite MENTION.
[[nodiscard]] Mir moduleTaking(TypeInterner& in, std::span<TypeId const> params, std::uint32_t functions = 1) {
    TypeId const sig = in.fnSig(params, in.primitive(TypeKind::Void), CallConv::CcSysV);
    MirBuilder b;
    for (std::uint32_t k = 0; k < functions; ++k) {
        (void)b.addFunction(sig, SymbolId{k + 1});
        MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(entry);
        b.addReturn();
    }
    return std::move(b).finish();
}

struct RoundTrip {
    std::string                     text;
    std::string                     again;
    std::size_t                     emitErrors = 0;
    std::unique_ptr<MirParseResult> parsed;
};

// emit -> parse (verify-on-load included) -> re-emit.
[[nodiscard]] RoundTrip roundTrip(Mir const& m, TypeInterner const& in) {
    RoundTrip rt;
    DiagnosticReporter r1;
    MirTextContext const ctx{&in, nullptr};
    rt.text       = emitMir(m, ctx, r1);
    rt.emitErrors = r1.errorCount();
    DiagnosticReporter r2;
    rt.parsed = parseMir(rt.text, CompilationUnitId{77}, r2);
    MirTextContext const ctx2{&rt.parsed->interner, nullptr, &rt.parsed->symbolNames};
    DiagnosticReporter r3;
    rt.again = emitMir(rt.parsed->mir, ctx2, r3);
    return rt;
}

// The composite under a `ptr<…>` parameter `k` of the first function of a parsed module.
[[nodiscard]] TypeId pointeeOfParam(MirParseResult const& p, std::size_t k) {
    TypeId const sig = p.mir.funcSignature(p.mir.funcAt(0));
    return p.interner.operands(p.interner.fnParams(sig)[k])[0];
}

} // namespace

// ═══ THE CYCLE ═══════════════════════════════════════════════════════════════════════════════════════════

// A linked-list node and a mutually recursive pair: each is ONE entry whose fields name handles — its own
// included — and the text reads back to composites that point at themselves exactly as the originals do.
TEST(MirTextTypesTable, SelfReferentialAndMutuallyRecursiveCompositesAreEntriesAndReadBack) {
    TypeInterner in{CompilationUnitId{21}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const node = in.forwardComposite(TypeKind::Struct, "Node", 1);
    std::array<TypeId, 2> const nodeF{i32, in.pointer(node)};
    in.completeComposite(node, nodeF, false, {});
    TypeId const a = in.forwardComposite(TypeKind::Struct, "A", 2);
    TypeId const b = in.forwardComposite(TypeKind::Union, "B", 3);
    std::array<TypeId, 1> const aF{in.pointer(b)};
    std::array<TypeId, 2> const bF{in.pointer(a), i32};
    in.completeComposite(a, aF, false, {});
    in.completeComposite(b, bF, false, {});

    std::array<TypeId, 2> const params{in.pointer(node), in.pointer(a)};
    Mir const m = moduleTaking(in, params);
    RoundTrip const rt = roundTrip(m, in);
    EXPECT_EQ(rt.emitErrors, 0u) << rt.text;
    EXPECT_EQ(typesSectionOf(rt.text),
              "types {\n"
              "  type 1 = struct \"Node\" {i32, ptr<type 1>}\n"
              "  type 2 = struct \"A\" {ptr<type 3>}\n"
              "  type 3 = union \"B\" {ptr<type 2>, i32}\n"
              "}\n") << rt.text;
    ASSERT_TRUE(rt.parsed->ok) << rt.text;
    EXPECT_EQ(rt.again, rt.text) << "the round trip must be byte-identical";

    TypeInterner const& back = rt.parsed->interner;
    TypeId const node2 = pointeeOfParam(*rt.parsed, 0);
    EXPECT_EQ(back.name(node2), "Node");
    EXPECT_EQ(back.operands(back.operands(node2)[1])[0], node2) << "`next` points at its own composite";
    TypeId const a2 = pointeeOfParam(*rt.parsed, 1);
    TypeId const b2 = back.operands(back.operands(a2)[0])[0];
    EXPECT_EQ(back.kind(b2), TypeKind::Union);
    EXPECT_EQ(back.operands(back.operands(b2)[0])[0], a2) << "the pair closes back on itself";
}

// ═══ THE LAYOUT ══════════════════════════════════════════════════════════════════════════════════════════

// Every channel `completeComposite` takes travels through the definition, and the HALF THE BYTES CANNOT SHOW is
// checked on the rebuilt composites: every channel, and the layout the layout engine computes from them — size,
// alignment, every field offset — equals the original's.
TEST(MirTextTypesTable, EveryLayoutChannelOfACompositeTravelsThroughItsDefinition) {
    TypeInterner in{CompilationUnitId{31}};
    TypeId const u32 = in.primitive(TypeKind::U32);
    TypeId const u8  = in.primitive(TypeKind::U8);
    TypeId const chr = in.primitive(TypeKind::Char);
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const i64 = in.primitive(TypeKind::I64);
    std::span<std::int64_t const>  const noW{};
    std::span<std::uint64_t const> const noO{};
    std::span<std::uint32_t const> const noA{};

    // (1) bit-field widths, the zero-width packing break included.
    TypeId const bits = in.forwardComposite(TypeKind::Struct, "Bits", 1);
    std::array<TypeId, 4> const bitsF{u32, u32, u32, u32};
    std::array<std::int64_t, 4> const bitsW{3, 0, 5, kNotBitfield};
    in.completeComposite(bits, bitsF, false, bitsW);
    // (2) `packed, aligned(16)` on one struct.
    TypeId const al = in.forwardComposite(TypeKind::Struct, "Al", 2);
    std::array<TypeId, 2> const alF{chr, i32};
    in.completeComposite(al, alF, /*packed=*/true, noW, noO, noA, /*explicitAlign=*/16);
    // (3) a `#pragma pack(2)` cap.
    TypeId const pk = in.forwardComposite(TypeKind::Struct, "Pk", 3);
    std::array<TypeId, 2> const pkF{chr, i64};
    in.completeComposite(pk, pkF, false, noW, noO, noA, 0, /*maxFieldAlign=*/2);
    // (4) a member alignment on a UNION member.
    TypeId const ua = in.forwardComposite(TypeKind::Union, "UA", 4);
    std::array<TypeId, 2> const uaF{chr, i32};
    std::array<std::uint32_t, 2> const uaA{0u, 8u};
    in.completeComposite(ua, uaF, false, noW, noO, uaA);
    // (5) a union carrying a bit-field, `aligned(8)` and a `pack(4)` cap at once.
    TypeId const ub = in.forwardComposite(TypeKind::Union, "UB", 5);
    std::array<TypeId, 2> const ubF{u32, u8};
    std::array<std::int64_t, 2> const ubW{7, kNotBitfield};
    in.completeComposite(ub, ubF, false, ubW, noO, noA, 8, 4);
    // (6) explicit offsets — an FFI overlay, two members in the same bytes.
    TypeId const ov = in.forwardComposite(TypeKind::Struct, "Ov", 6);
    std::array<TypeId, 2> const ovF{i32, i64};
    std::array<std::uint64_t, 2> const ovO{0u, 0u};
    in.completeComposite(ov, ovF, false, noW, ovO);
    // (7) ONE member packed.
    TypeId const pm = in.forwardComposite(TypeKind::Struct, "Pm", 7);
    std::array<TypeId, 2> const pmF{chr, i32};
    std::array<std::uint8_t, 2> const pmP{0u, 1u};
    in.completeComposite(pm, pmF, false, noW, noO, noA, 0, 0, pmP);
    // (8) an INCOMPLETE composite — `opaque`, never `{}` (which is a legal complete zero-member one).
    TypeId const op = in.forwardComposite(TypeKind::Struct, "FILE", 8);

    std::array<TypeId, 8> const original{bits, al, pk, ua, ub, ov, pm, op};
    std::vector<TypeId> params;
    for (TypeId const t : original) params.push_back(in.pointer(t));
    Mir const m = moduleTaking(in, params);
    RoundTrip const rt = roundTrip(m, in);
    EXPECT_EQ(rt.emitErrors, 0u) << rt.text;
    EXPECT_EQ(typesSectionOf(rt.text),
              "types {\n"
              "  type 1 = struct \"Bits\" {u32 bits 3, u32 bits 0, u32 bits 5, u32}\n"
              "  type 2 = struct \"Al\" packed aligned 16 {char, i32}\n"
              "  type 3 = struct \"Pk\" pack 2 {char, i64}\n"
              "  type 4 = union \"UA\" {char ~0, i32 ~8}\n"
              "  type 5 = union \"UB\" aligned 8 pack 4 {u32 bits 7, u8}\n"
              "  type 6 = struct \"Ov\" {i32 @0, i64 @0}\n"
              "  type 7 = struct \"Pm\" {char, i32 packed}\n"
              "  type 8 = struct \"FILE\" opaque\n"
              "}\n") << rt.text;
    ASSERT_TRUE(rt.parsed->ok) << rt.text;
    EXPECT_EQ(rt.again, rt.text);

    TypeInterner const& back = rt.parsed->interner;
    AggregateLayoutParams const abi{ScalarAlignmentRule::Natural, 16};
    for (std::size_t k = 0; k < original.size(); ++k) {
        TypeId const o = original[k];
        TypeId const n = pointeeOfParam(*rt.parsed, k);
        SCOPED_TRACE(std::string{in.name(o)});
        EXPECT_EQ(back.kind(n), in.kind(o));
        EXPECT_EQ(back.isIncompleteComposite(n), in.isIncompleteComposite(o));
        if (in.isIncompleteComposite(o)) continue;
        EXPECT_EQ(back.isPacked(n), in.isPacked(o));
        EXPECT_EQ(back.explicitCompositeAlign(n), in.explicitCompositeAlign(o));
        EXPECT_EQ(back.maxFieldAlign(n), in.maxFieldAlign(o));
        EXPECT_EQ(back.hasExplicitAligns(n), in.hasExplicitAligns(o));
        EXPECT_EQ(back.hasExplicitOffsets(n), in.hasExplicitOffsets(o));
        EXPECT_EQ(back.hasFieldPacked(n), in.hasFieldPacked(o));
        auto const fo = in.operands(o);
        ASSERT_EQ(back.operands(n).size(), fo.size());
        for (std::size_t i = 0; i < fo.size(); ++i) {
            EXPECT_EQ(back.fieldBitWidth(n, i), in.fieldBitWidth(o, i)) << "field " << i;
            EXPECT_EQ(back.explicitFieldAlign(n, i), in.explicitFieldAlign(o, i)) << "field " << i;
            EXPECT_EQ(back.explicitFieldOffset(n, i), in.explicitFieldOffset(o, i)) << "field " << i;
            EXPECT_EQ(back.isFieldPacked(n, i), in.isFieldPacked(o, i)) << "field " << i;
        }
        auto const lo = computeLayout(o, in, abi, DataModel::Lp64);
        auto const ln = computeLayout(n, back, abi, DataModel::Lp64);
        ASSERT_EQ(lo.has_value(), ln.has_value());
        if (!lo) continue;
        EXPECT_EQ(ln->size, lo->size);
        EXPECT_EQ(ln->align.bytes(), lo->align.bytes());
        EXPECT_EQ(ln->fieldOffsets, lo->fieldOffsets);
    }
}

// ═══ THE SIZE ════════════════════════════════════════════════════════════════════════════════════════════

// A composite graph mentioned from many places is DEFINED ONCE, and every mention is its handle — so the text grows
// with the mentions and the graph, never with their product (v1's measured 30 GB on sqlite3.c).
TEST(MirTextTypesTable, ACompositeGraphMentionedManyTimesIsDefinedOnce) {
    TypeInterner in{CompilationUnitId{41}};
    TypeId const i64 = in.primitive(TypeKind::I64);
    TypeId const node = in.forwardComposite(TypeKind::Struct, "Node", 1);
    std::array<TypeId, 3> const nodeF{i64, in.pointer(node), in.pointer(node)};
    in.completeComposite(node, nodeF, false, {});
    TypeId const tree = in.forwardComposite(TypeKind::Struct, "Tree", 2);
    std::array<TypeId, 3> const treeF{node, in.pointer(node), i64};
    in.completeComposite(tree, treeF, false, {});
    std::array<TypeId, 2> const params{in.pointer(tree), in.pointer(node)};

    auto const textWith = [&](std::uint32_t functions) {
        Mir const m = moduleTaking(in, params, functions);
        DiagnosticReporter r;
        MirTextContext const ctx{&in, nullptr};
        return emitMir(m, ctx, r);
    };
    std::string const one = textWith(1);
    std::string const eight = textWith(8);
    std::string const sixteen = textWith(16);
    EXPECT_EQ(occurrences(sixteen, "= struct \"Node\""), 1u) << sixteen;
    EXPECT_EQ(occurrences(sixteen, "= struct \"Tree\""), 1u) << sixteen;
    EXPECT_EQ(occurrences(sixteen, "struct \"Node\""), 1u) << "a mention never re-spells the definition";
    // The TABLE does not grow with the mentions at all — one module mentioning the graph once and one mentioning it
    // sixteen times carry the same `types` section, byte for byte.
    EXPECT_FALSE(typesSectionOf(one).empty()) << one;
    EXPECT_EQ(typesSectionOf(eight), typesSectionOf(one));
    EXPECT_EQ(typesSectionOf(sixteen), typesSectionOf(one));
    // …and each extra mention costs exactly its handle: the eight extra functions add their two `ptr<type H>`
    // parameters each, and nothing else that names a composite.
    EXPECT_EQ(occurrences(sixteen, "ptr<type ") - occurrences(eight, "ptr<type "), 2u * 8u);
}

// ═══ THE REFUSALS ════════════════════════════════════════════════════════════════════════════════════════

// The table bought a representation, not a licence to accept anything: each arm is text the v2 writer cannot
// produce, and for the layout channels a combination `completeComposite` would ABORT on — which a reader of text it
// did not write must never reach. Each is refused BY NAME.
TEST(MirTextTypesTable, ATypesEntryOrReferenceTheWriterCannotProduceIsRefusedByName) {
    struct Arm { char const* what; char const* types; char const* type; char const* says; };
    std::array<Arm, 15> const arms{{
        {"a reference to no entry", "  type 1 = struct \"S\" {i32}\n", "type 2", "names no entry"},
        {"a reference with no table at all", "", "ptr<type 1>", "names no entry"},
        {"the 0 handle", "  type 0 = struct \"S\" {i32}\n", "i32", "1-based"},
        {"a handle past 32 bits", "  type 4294967296 = struct \"S\" {i32}\n", "i32", "1-based"},
        {"one handle, two bodies", "  type 1 = struct \"S\" {i32}\n  type 1 = struct \"S\" {i64}\n", "type 1",
         "defined twice"},
        {"an entry that is not a composite", "  type 1 = enum \"E\"\n", "i32", "a `struct` or a `union`"},
        {"an unrepresentable composite alignment", "  type 1 = struct \"S\" aligned 3 {i32}\n", "type 1",
         "not a composite alignment"},
        {"a zero pack cap", "  type 1 = struct \"S\" pack 0 {i32}\n", "type 1", "not a composite alignment"},
        {"an unrepresentable member alignment", "  type 1 = union \"U\" {i32 ~3}\n", "type 1",
         "not an alignment this build can represent"},
        {"offsets mixed with member aligns", "  type 1 = struct \"S\" {i32 @0, i32 ~4}\n", "type 1", "cannot mix"},
        {"a partial offset channel", "  type 1 = struct \"S\" {i32 @0, i32}\n", "type 1", "all-or-none"},
        {"a packed composite with explicit offsets", "  type 1 = struct \"S\" packed {i32 @0}\n", "type 1",
         "cannot also be packed"},
        {"explicit offsets with bit-field widths", "  type 1 = struct \"S\" {u32 @0 bits 3, u32 @0}\n", "type 1",
         "cannot also carry bit-field widths"},
        {"an INLINE struct in a module (v1's spelling)", "", "struct \"S\" {i32}", "defined once in the `types`"},
        {"an INLINE union in a module (v1's spelling)", "", "union \"U\" {i32}", "defined once in the `types`"},
    }};
    auto const wrap = [](char const* types, char const* ty) {
        std::string s{"dssir 2\n"};
        if (*types != '\0') s += std::string{"types {\n"} + types + "}\n";
        s += "symbols {\n  %1 \"g\"\n}\nmodule {\n  global %1 : ";
        return s + ty + " = zero\n}\n";
    };
    for (Arm const& arm : arms) {
        std::string const text = wrap(arm.types, arm.type);
        DiagnosticReporter r;
        auto res = parseMir(text, CompilationUnitId{87}, r);
        EXPECT_FALSE(res->ok) << arm.what << " was ACCEPTED:\n" << text;
        bool named = false;
        std::string all;
        for (auto const& d : r.all()) {
            all += "\n  " + d.actual;
            if (d.severity == DiagnosticSeverity::Error && d.actual.find(arm.says) != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << arm.what << ": no Error says \"" << arm.says << "\" —" << all;
    }
    // …and the control: the same wrapper around a well-formed entry reads back clean, so no arm above is red for
    // a reason the wrapper itself supplies.
    DiagnosticReporter ok;
    EXPECT_TRUE(parseMir(wrap("  type 1 = struct \"S\" {i32 ~4, i64 ~0}\n", "type 1"), CompilationUnitId{88}, ok)->ok)
        << "the control wrapper must read clean";
}

// A v1 text spells composites inline and cannot be read by this grammar; it is refused by the version check, by
// name — never half-read. (No committed file carries a `.dss.mir` payload, so nothing stored is stranded.)
TEST(MirTextTypesTable, AVersionOneTextIsRefusedByTheVersionCheck) {
    DiagnosticReporter r;
    auto const res = parseMir("dssir 1\nmodule {\n}\n", CompilationUnitId{89}, r);
    EXPECT_FALSE(res->ok);
    bool named = false;
    for (auto const& d : r.all())
        named = named || (d.code == DiagnosticCode::I_TextVersionMismatch
                          && d.actual.find("expected version 2") != std::string::npos);
    EXPECT_TRUE(named);
}

// ═══ ONE SPELLING, TWO TIERS ═════════════════════════════════════════════════════════════════════════════

// The HIR v5 and MIR v2 tables are written through the ONE owner of what a definition carries, and they spell a
// definition IDENTICALLY — one serialization must not have two syntaxes. Every channel, in both tiers' own writers.
TEST(MirTextTypesTable, TheHirAndMirTablesSpellADefinitionIdentically) {
    TypeInterner in{CompilationUnitId{51}};
    TypeId const u32 = in.primitive(TypeKind::U32);
    TypeId const chr = in.primitive(TypeKind::Char);
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const s = in.forwardComposite(TypeKind::Struct, "S", 1);
    std::array<TypeId, 4> const sF{chr, i32, u32, in.pointer(s)};
    std::array<std::int64_t, 4> const sW{kNotBitfield, kNotBitfield, 5, kNotBitfield};
    std::array<std::uint32_t, 4> const sA{0u, 8u, 0u, 0u};
    std::array<std::uint8_t, 4> const sP{0u, 1u, 0u, 0u};
    in.completeComposite(s, sF, false, sW, {}, sA, 16, 4, sP);
    std::array<TypeId, 1> const params{in.pointer(s)};

    Mir const m = moduleTaking(in, params);
    DiagnosticReporter mr;
    MirTextContext const mctx{&in, nullptr};
    std::string const mirText = emitMir(m, mctx, mr);

    TypeId const sig = in.fnSig(params, in.primitive(TypeKind::Void), CallConv::CcSysV);
    HirBuilder b{"c"};
    HirNodeId const fn = b.makeFunction(sig, 1, {}, b.makeBlock(std::vector<HirNodeId>{}));
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{fn}));
    std::vector<std::string> names{"", "f"};
    HirTextContext hctx;
    hctx.interner    = &in;
    hctx.symbolNames = &names;
    DiagnosticReporter hr;
    std::string const hirText = emitHir(hir, hctx, hr);

    ASSERT_EQ(mr.errorCount(), 0u) << mirText;
    ASSERT_EQ(hr.errorCount(), 0u) << hirText;
    EXPECT_EQ(typesSectionOf(mirText),
              "types {\n  type 1 = struct \"S\" aligned 16 pack 4 {char ~0, i32 ~8 packed, u32 ~0 bits 5, "
              "ptr<type 1> ~0}\n}\n") << mirText;
    EXPECT_EQ(typesSectionOf(mirText), typesSectionOf(hirText)) << "MIR:\n" << mirText << "\nHIR:\n" << hirText;
}

// ═══ THE BODY PAYLOAD ════════════════════════════════════════════════════════════════════════════════════

// `encodeModuleBody` refuses to produce a `.dss.mir` payload whenever `emitMir` reports an Error — so under v1 a
// module holding a self-referential composite had NO body. The codec has no product caller yet; this is what its
// first one inherits: the module encodes, and decodes back to a module whose composite points at itself.
TEST(MirTextTypesTable, AModuleHoldingALinkedListNodeEncodesAsABodyAndDecodesBack) {
    TypeInterner in{CompilationUnitId{61}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const node = in.forwardComposite(TypeKind::Struct, "Node", 1);
    std::array<TypeId, 2> const nodeF{i32, in.pointer(node)};
    in.completeComposite(node, nodeF, false, {});
    std::array<TypeId, 1> const params{in.pointer(node)};
    Mir const m = moduleTaking(in, params);
    std::vector<std::string> const names{"", "f"};

    DiagnosticReporter r;
    auto const bytes = mirsum::encodeModuleBody(m, in, names, "digest", "x86_64:elf64-x86_64-linux", r);
    ASSERT_FALSE(bytes.empty()) << "a module holding a linked-list node must produce a body";
    EXPECT_EQ(r.errorCount(), 0u);
    auto decoded = mirsum::decodeModuleBody(bytes, CompilationUnitId{62}, "digest",
                                            "x86_64:elf64-x86_64-linux", r);
    ASSERT_TRUE(decoded.has_value()) << "errorCount=" << r.errorCount();
    TypeInterner const& back = decoded->parsed->interner;
    TypeId const node2 = pointeeOfParam(*decoded->parsed, 0);
    EXPECT_EQ(back.operands(back.operands(node2)[1])[0], node2);
}
