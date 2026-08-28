// D-LK-STATIC-LINK-INTRASECTION-BAKED-CALL — a reloc-LESS `E8 rel32` baked by
// the assembler must still land on its callee after DSS re-lays the atoms out.
//
// ★★★ WHY THIS FILE ASSERTS ARITHMETIC ON BYTES AND NEVER "IT LINKS". It
// linked before the fix. ✔MEASURED on a plain `gcc -O2 -c` object: DSS
// produced an executable with rc 0 and ZERO diagnostics, and the binary
// SEGFAULTED because `compute`'s baked call had been silently redirected into
// the middle of `main`. A test that checked the link succeeded would have been
// green for the entire life of the defect. So every assertion below recovers
// the DISPLACEMENT from the emitted bytes and computes where it lands.
//
// THE SHAPE (see `gcc_intrasection_baked_call.inc` for the full account):
// one `.text`, three `STT_FUNC` atoms at 16-byte-aligned offsets, no
// `.rela.text` at all, and 12 + 7 bytes of inter-function padding that belongs
// to no symbol.
//
//     helper   @ 0x00 size  4     filler @ 0x10 size 9     compute @ 0x20 size 13
//
// `compute` calls `helper` as `E8 D7 FF FF FF` — rel32 = -41 — resolved by the
// ASSEMBLER, with no relocation anywhere that could repair it.
//
// ★★ THE INVARIANT: the reader must reconstruct atoms whose CONCATENATION
// reproduces the input section's byte layout, so that the distance the
// assembler baked in is still the distance between the two atoms. Dropping the
// padding shrinks it by 19 bytes and the call lands 19 bytes before `helper`.
//
// RED-ON-DISABLE: neutralise the `(6.9) INTER-ATOM PADDING` pass at the end of
// `readRelocatableObject` in `src/link/format/elf_object_reader.cpp` → the two
// distance assertions below fail by NAME, reporting the shrunken offset.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "gcc_intrasection_baked_call.inc"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace dss;

namespace {

struct Shipped {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Shipped loadShipped() {
    Shipped s;
    if (auto t = TargetSchema::loadShipped("x86_64")) s.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux")) s.format = *f;
    return s;
}

[[nodiscard]] AssembledFunction const* funcNamed(AssembledModule const& m,
                                                 std::string const& name) {
    for (auto const& f : m.functions) {
        for (auto const& s : m.symbols) {
            if (s.symbol == f.symbol && s.name == name) return &f;
        }
    }
    return nullptr;
}

// The offset an atom will occupy once the writer concatenates `m.functions`
// back-to-back — which is exactly what every format writer does.
[[nodiscard]] bool concatOffsetOf(AssembledModule const& m,
                                  AssembledFunction const* want,
                                  std::uint64_t& out) {
    std::uint64_t at = 0;
    for (auto const& f : m.functions) {
        if (&f == want) { out = at; return true; }
        at += f.bytes.size();
    }
    return false;
}

// Recover the rel32 of the FIRST `E8` in an atom, and the offset of the byte
// following that instruction (the point a rel32 is measured from).
[[nodiscard]] bool firstCallRel32(AssembledFunction const& f,
                                  std::int32_t& rel, std::uint64_t& nextOff) {
    for (std::size_t i = 0; i + 5 <= f.bytes.size(); ++i) {
        if (f.bytes[i] != 0xE8) continue;
        std::int32_t v = 0;
        std::memcpy(&v, f.bytes.data() + i + 1, 4);
        rel = v;
        nextOff = static_cast<std::uint64_t>(i) + 5;
        return true;
    }
    return false;
}

} // namespace

TEST(IntrasectionBakedCall, TheBakedDisplacementStillLandsOnItsCallee) {
    auto sh = loadShipped();
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto got = elf::readRelocatableObject(dss::test::gccIntrasectionBakedCallObject(),
                                          *sh.target, *sh.format, rep);
    ASSERT_TRUE(got.has_value()) << "errors=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const* helper  = funcNamed(*got, "helper");
    auto const* compute = funcNamed(*got, "compute");
    ASSERT_NE(helper, nullptr);
    ASSERT_NE(compute, nullptr);

    std::uint64_t helperAt = 0, computeAt = 0;
    ASSERT_TRUE(concatOffsetOf(*got, helper, helperAt));
    ASSERT_TRUE(concatOffsetOf(*got, compute, computeAt));

    std::int32_t  rel = 0;
    std::uint64_t nextOff = 0;
    ASSERT_TRUE(firstCallRel32(*compute, rel, nextOff))
        << "the fixture's `compute` must still carry its baked E8";
    EXPECT_EQ(rel, -41) << "the baked displacement is an INPUT constant — if it "
                           "changed, the fixture changed, not the linker";

    // Where the CPU will actually go, computed from the emitted bytes alone.
    std::int64_t const landing =
        static_cast<std::int64_t>(computeAt + nextOff) + rel;
    EXPECT_EQ(landing, static_cast<std::int64_t>(helperAt))
        << "the baked call must land exactly on `helper`. computeAt="
        << computeAt << " nextOff=" << nextOff << " rel=" << rel
        << " -> " << landing << ", helperAt=" << helperAt
        << ". A mismatch here is a jump into the middle of another function.";
}

TEST(IntrasectionBakedCall, InterAtomDistancesReproduceTheInputSection) {
    // The invariant behind the arithmetic above, stated directly: the input
    // section placed its three functions at 0x00 / 0x10 / 0x20, and the
    // concatenated atoms must reproduce exactly those relative offsets.
    auto sh = loadShipped();
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto got = elf::readRelocatableObject(dss::test::gccIntrasectionBakedCallObject(),
                                          *sh.target, *sh.format, rep);
    ASSERT_TRUE(got.has_value());

    auto const* helper  = funcNamed(*got, "helper");
    auto const* filler  = funcNamed(*got, "filler");
    auto const* compute = funcNamed(*got, "compute");
    ASSERT_NE(helper, nullptr);
    ASSERT_NE(filler, nullptr)
        << "the unreferenced middle function must still reconstruct — dropping "
           "it is another way to break the distance";
    ASSERT_NE(compute, nullptr);

    std::uint64_t h = 0, f = 0, c = 0;
    ASSERT_TRUE(concatOffsetOf(*got, helper, h));
    ASSERT_TRUE(concatOffsetOf(*got, filler, f));
    ASSERT_TRUE(concatOffsetOf(*got, compute, c));

    EXPECT_EQ(f - h, 0x10u) << "helper -> filler was 0x10 in the input section";
    EXPECT_EQ(c - h, 0x20u) << "helper -> compute was 0x20 in the input section";
}

TEST(IntrasectionBakedCall, EveryFunctionKeepsItsExactSizeAndFdeRange) {
    // ★★ THE REASON THE PADDING IS ITS OWN ATOM. An earlier draft absorbed the
    // padding into the PRECEDING function's bytes. That preserved the distance
    // too — and made `helper` report 16 bytes for a 4-byte function, which is
    // what a profiler, a crash reporter and `nm --print-size` would then read.
    // The ELF writer also refused it outright (`K_UnwindRuleUnrepresentable`),
    // because the FDE's `address_range` no longer matched the byte count.
    // These assertions are what keep the exact-metadata property from being
    // quietly traded away again for a smaller diff.
    auto sh = loadShipped();
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto got = elf::readRelocatableObject(dss::test::gccIntrasectionBakedCallObject(),
                                          *sh.target, *sh.format, rep);
    ASSERT_TRUE(got.has_value());

    auto const* helper  = funcNamed(*got, "helper");
    auto const* filler  = funcNamed(*got, "filler");
    auto const* compute = funcNamed(*got, "compute");
    ASSERT_NE(helper, nullptr);
    ASSERT_NE(filler, nullptr);
    ASSERT_NE(compute, nullptr);

    // The sizes the object's own symbol table declares — no padding folded in.
    EXPECT_EQ(helper->bytes.size(), 4u);
    EXPECT_EQ(filler->bytes.size(), 9u);
    EXPECT_EQ(compute->bytes.size(), 13u);

    // And the writer's invariant holds for every atom that carries CFI.
    for (auto const& fn : got->functions) {
        if (!fn.cfi.has_value()) continue;
        EXPECT_EQ(fn.cfi->codeLength, static_cast<std::uint32_t>(fn.bytes.size()))
            << "an FDE that disagrees with its atom makes the writer refuse the "
               "whole link";
    }
}

TEST(IntrasectionBakedCall, ThePaddingBecomesAnonymousAtomsNobodyCanName) {
    // The padding is carried by SYNTHETIC atoms: two of them here (12 bytes
    // after `helper`, 7 after `filler`), each with a fresh SymbolId above every
    // real symbol and NO `ModuleSymbol` — module-private, never cross-CU
    // folded, exactly the discipline the (6.5) data gap atoms use. Without
    // them the three functions would pack to 4/9/13 and the baked call would
    // move.
    auto sh = loadShipped();
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto got = elf::readRelocatableObject(dss::test::gccIntrasectionBakedCallObject(),
                                          *sh.target, *sh.format, rep);
    ASSERT_TRUE(got.has_value());

    EXPECT_EQ(got->functions.size(), 5u)
        << "three real functions plus two padding atoms";

    std::size_t named = 0, anonymous = 0;
    std::size_t anonBytes = 0;
    for (auto const& f : got->functions) {
        bool hasName = false;
        for (auto const& sym : got->symbols) {
            if (sym.symbol == f.symbol) { hasName = true; break; }
        }
        if (hasName) { ++named; continue; }
        ++anonymous;
        anonBytes += f.bytes.size();
    }
    EXPECT_EQ(named, 3u);
    EXPECT_EQ(anonymous, 2u);
    EXPECT_EQ(anonBytes, 19u) << "12 bytes after helper + 7 after filler";
}
