// ── A DESCRIPTOR'S TYPE GRAPH COSTS HEAP, NOT HOST CALL FRAMES ──────────────
//
// The operator's standing ruling of 2026-09-02: no input-proportional recursion
// in the compiler. `ShippedTypeConsistency::walk` is the FFI tier's instance of
// it — a DFS over every type a shipped-lib descriptor declares, run by the
// semantic analyzer immediately before descriptor injection.
//
// ★★ WHY THE `visited_` MEMO WAS NOT A BOUND. The walk's own comment said it was
// "bounded only by a visited-set over a corpus that happens to be finite", and
// both halves of that were true and neither was a depth bound: `visited_` makes a
// SELF-REFERENTIAL type terminate, and it does nothing at all about a type that
// is merely DEEP. A descriptor is CONFIG the user writes and ships, so the depth
// of `ptr<ptr<…>>` there is the user's to choose. Today's shipped corpus is
// shallow; that is a property of today's corpus.
//
// ★★ THE FIXTURE IS BUILT THROUGH THE INTERNER, NOT THROUGH A DESCRIPTOR FILE,
// and the reason is that the decoder is a DIFFERENT question. `parseTypeFromText`
// (the `ptr<…>` spelling reader) lives in `src/hir/hir_text.cpp`, which another
// lane owns and which the residue row already names as carrying the same class of
// defect. Reading a 100_000-level spelling would measure THAT, and a red would
// mean the wrong thing. Interning the chain in a `for` loop measures exactly the
// walk this file is about.
//
// ★★ ITS OWN BINARY ON PURPOSE: a red here is a stack overflow — no
// `[  FAILED  ]` line, no case name — and it must not take a sibling test's
// verdict with it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "ffi/shipped_type_consistency.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;

// A pointer chain far past anything one host frame per level survives, and it
// costs nothing on the heap. The FIXTURE is iterative too — a recursive builder
// would red for its own reason.
TEST(ShippedTypeConsistencyDeepWalk, HundredThousandTypeLevelsCostHeap) {
    constexpr int kDepth = 100000;

    TypeInterner interner{CompilationUnitId{1}};
    TypeId chain = interner.primitive(TypeKind::I32);
    for (int i = 0; i < kDepth; ++i) chain = interner.pointer(chain);

    ShippedLibDescriptor desc;
    desc.header = "deep.h";
    desc.typedefs.push_back(ShippedTypedef{"deep_t", chain});

    DiagnosticReporter reporter;
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{}};

    // Before the conversion this call did not return — it exhausted the stack one
    // frame per pointer level. The contract now is that it COMPLETES, and that it
    // still says the descriptor is consistent (a walk that silently gave up would
    // also "complete", so the verdict is asserted, not just the return).
    EXPECT_TRUE(checker.add("deep.json", desc, reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}

// The complement, and the reason `visited_` must survive the conversion: the memo
// is what stops a SHARED subgraph from being re-walked once per path into it. A
// diamond over a deep chain would otherwise cost 2^k walks of the shared tail.
//
// ⚠ A TRUE CYCLE IS NOT CONSTRUCTIBLE HERE TODAY, and that is worth stating
// rather than leaving as a gap: `TypeInterner` has no declare-tag-then-complete
// entry point (the architectural fork the MIR-tier lane deliberately did not
// take, recorded in
// D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED), so
// a self-referential composite cannot be built through this API at all. The memo
// is kept regardless — an explicit stack would loop forever on one, and a HANG is
// worse than a crash because no watchdog reports it.
TEST(ShippedTypeConsistencyDeepWalk, ASharedSubgraphIsWalkedOnce) {
    constexpr int kDepth = 20000;

    TypeInterner interner{CompilationUnitId{1}};
    TypeId tail = interner.primitive(TypeKind::I32);
    for (int i = 0; i < kDepth; ++i) tail = interner.pointer(tail);

    // Two typedefs naming the SAME deep chain, plus a struct whose two fields both
    // point at it: four separate entries into one shared subgraph.
    std::vector<TypeId> const fields{tail, tail};
    TypeId const rec = interner.structType("R", fields);

    ShippedLibDescriptor desc;
    desc.header = "shared.h";
    desc.typedefs.push_back(ShippedTypedef{"a_t", tail});
    desc.typedefs.push_back(ShippedTypedef{"b_t", tail});
    desc.typedefs.push_back(ShippedTypedef{"r_t", rec});

    DiagnosticReporter reporter;
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{}};
    EXPECT_TRUE(checker.add("shared.json", desc, reporter));
    EXPECT_EQ(reporter.errorCount(), 0u);
}
