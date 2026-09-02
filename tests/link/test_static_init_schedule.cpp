// D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN — the LINK tier's
// half of the static-initializer feature: the whole-program ORDER, the per-format
// RUNNER, and the refusal that keeps a format which can run nothing from silently
// running nothing.
//
// ★★ WHY THESE PINS EXIST SEPARATELY FROM THE CORPUS EXAMPLE. The example proves
// a program EXITS 42, which is one observation of the entire chain; it cannot say
// WHICH link happened. Two of the facts below have no other witness at all:
//
//   * the `imageLoader` arm emits NO trampoline calls. No shipped format selects
//     that arm today, so nothing in the corpus exercises it — and the day one
//     does, the failure mode is every initializer running TWICE, which no exit
//     code from a commutative accumulator would reveal.
//   * the after-entry channel is the before-entry sequence REVERSED. An example
//     with one destructor cannot tell a reversal from an identity.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "diagnostic_count.hpp"
#include "link/entry_trampoline.hpp"
#include "link/object_format_schema.hpp"
#include "link/static_init_tables.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::linker;

namespace {

[[nodiscard]] StaticInitSchedule before(std::uint32_t prio) {
    StaticInitSchedule s;
    s.setPriorityFor(StaticInitPhase::BeforeEntry, prio);
    return s;
}
[[nodiscard]] StaticInitSchedule after(std::uint32_t prio) {
    StaticInitSchedule s;
    s.setPriorityFor(StaticInitPhase::AfterEntry, prio);
    return s;
}

[[nodiscard]] std::vector<std::uint32_t>
symbolsOf(std::vector<StaticInitOrderEntry> const& v) {
    std::vector<std::uint32_t> out;
    for (auto const& e : v) out.push_back(e.symbol.v);
    return out;
}

// A module carrying nothing but a schedule — the ordering is a pure function of
// `staticInitSchedule`, so the functions need not exist for these pins.
[[nodiscard]] AssembledModule scheduleOnly(
    std::vector<LirStaticInitEntry> entries) {
    AssembledModule m;
    m.staticInitSchedule = std::move(entries);
    return m;
}

// The shipped x86_64 target, for the trampoline pins.
[[nodiscard]] std::shared_ptr<TargetSchema const> x64Target() {
    static auto const t = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        return r.has_value() ? *r : nullptr;
    }();
    return t;
}

// A minimal ELF-exec format whose static-initializer RUNNER is parameterised, so
// both arms of the axis can be driven from one fixture. Everything else is the
// synthetic syscall-exec shape the Slice C tests already use.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
makeElfExecFormat(std::string const& staticInitBlock) {
    std::string const doc = std::string(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": "",
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": { "name": "synth-elf-staticinit", "version": "0.1", "kind": "elf" },
      "entryPoint": "",
      "externCallDispatch": "direct-plt",
      "elf": {
        "class": "elf64", "data": "lsb", "osabi": "sysv", "machine": 62,
        "type": "exec", "pageAlign": 4096,
        "interpreter": "/lib64/ld-linux-x86-64.so.2", "bindNow": true
      },
      "entryCallingConvention": "sysv_amd64",
      "entryVerbs": ["none","argc-argv"],
      "processExit": {
        "mechanism": "syscall",
        "syscallNumber": 231,
        "syscallNumGpr": "rax",
        "syscallOpcodeBytes": [15, 5]
      },)") + staticInitBlock + R"(
      "sections": [
        { "kind": "text", "name": ".text", "type": 1, "flags": 6,
          "addrAlign": 16, "entrySize": 0, "virtualAddress": 4198400 }
      ]
    })";
    auto r = ObjectFormatSchema::loadFromText(doc);
    if (!r.has_value()) return nullptr;
    return *r;
}

// A module with three real functions (`ret` only) plus a schedule naming two of
// them, so the trampoline has something to call.
[[nodiscard]] AssembledModule makeThreeFnModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 3;
    for (std::uint32_t i = 1; i <= 3; ++i) {
        AssembledFunction fn;
        fn.symbol = SymbolId{i};
        fn.bytes  = {0xC3};   // ret
        mod.functions.push_back(std::move(fn));
    }
    mod.userEntrySymbol = SymbolId{1};
    mod.staticInitSchedule = {
        LirStaticInitEntry{SymbolId{2}, before(kUnprioritizedStaticInit)},
        LirStaticInitEntry{SymbolId{3}, after(kUnprioritizedStaticInit)},
    };
    return mod;
}

// How many `call` instructions the emitted trampoline contains, counted as E8
// opcodes in the synthetic `_start` at functions[0]. Crude on purpose: the point
// is the COUNT changing with the runner arm, and a byte-exact disassembly here
// would pin the encoder rather than the axis under test.
[[nodiscard]] std::size_t trampolineRelCallCount(AssembledModule const& m) {
    if (m.functions.empty()) return 0;
    std::size_t n = 0;
    for (auto const& r : m.functions.front().relocations) {
        (void)r;
        ++n;   // every trampoline call site produces one relocation
    }
    return n;
}

} // namespace

// ── the ORDER ───────────────────────────────────────────────────────────────

// Ascending priority, and the UNPRIORITIZED form LAST. ✔MEASURED 2026-08-28,
// unanimous across gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0:
// `c101 c102 cBARE`. The sentinel is a MAXIMUM for exactly this reason — a zero
// sentinel would sort the bare form FIRST, the opposite of every reference.
TEST(StaticInitSchedule, BeforeEntryRunsAscendingWithTheBareFormLast) {
    auto const m = scheduleOnly({
        LirStaticInitEntry{SymbolId{7}, before(kUnprioritizedStaticInit)},
        LirStaticInitEntry{SymbolId{8}, before(102)},
        LirStaticInitEntry{SymbolId{9}, before(101)},
    });
    EXPECT_EQ(symbolsOf(staticInitOrder(m, StaticInitPhase::BeforeEntry)),
              (std::vector<std::uint32_t>{9, 8, 7}));
}

// The after-entry channel is that SAME sequence walked BACKWARD — ✔MEASURED,
// `dBARE d102 d101` in all three references. An example with one destructor
// cannot tell a reversal from an identity, which is why this pin is here.
TEST(StaticInitSchedule, AfterEntryIsTheBeforeEntryOrderReversed) {
    auto const m = scheduleOnly({
        LirStaticInitEntry{SymbolId{7}, after(kUnprioritizedStaticInit)},
        LirStaticInitEntry{SymbolId{8}, after(102)},
        LirStaticInitEntry{SymbolId{9}, after(101)},
    });
    EXPECT_EQ(symbolsOf(staticInitOrder(m, StaticInitPhase::AfterEntry)),
              (std::vector<std::uint32_t>{7, 8, 9}))
        << "reversed relative to the before-entry order 9,8,7";
}

// A function in BOTH channels appears in BOTH orders, at its own priority in each
// — the shape a single `{phase, priority}` pair could not express.
TEST(StaticInitSchedule, OneFunctionInBothChannelsAppearsInBothOrders) {
    StaticInitSchedule bothCh;
    bothCh.setPriorityFor(StaticInitPhase::BeforeEntry, 101);
    bothCh.setPriorityFor(StaticInitPhase::AfterEntry, 102);
    auto const m = scheduleOnly({LirStaticInitEntry{SymbolId{5}, bothCh}});
    EXPECT_EQ(symbolsOf(staticInitOrder(m, StaticInitPhase::BeforeEntry)),
              (std::vector<std::uint32_t>{5}));
    EXPECT_EQ(symbolsOf(staticInitOrder(m, StaticInitPhase::AfterEntry)),
              (std::vector<std::uint32_t>{5}));
}

// A channel a declaration did not join contributes NOTHING to that channel's
// order. Reading the schedule through `priorityFor` rather than by member name is
// what makes this hold for both directions from one implementation.
TEST(StaticInitSchedule, AChannelOnlyOrdersTheDeclarationsThatJoinedIt) {
    auto const m = scheduleOnly({
        LirStaticInitEntry{SymbolId{1}, before(101)},
        LirStaticInitEntry{SymbolId{2}, after(101)},
    });
    EXPECT_EQ(symbolsOf(staticInitOrder(m, StaticInitPhase::BeforeEntry)),
              (std::vector<std::uint32_t>{1}));
    EXPECT_EQ(symbolsOf(staticInitOrder(m, StaticInitPhase::AfterEntry)),
              (std::vector<std::uint32_t>{2}));
}

// ⚠⚠ EQUAL PRIORITIES ARE ORDERED DETERMINISTICALLY, AND THIS PIN ASSERTS
// DETERMINISM RATHER THAN A PARTICULAR SEQUENCE. ✔MEASURED: two bare
// same-priority constructors run 1,2 under Linux gcc and clang and 2,1 under
// mingw-w64 gcc — the references DISAGREE, so DSS owes no specific order and a
// test that demanded one would be asserting a fact nobody honours. What DSS does
// owe is the same answer twice for the same input, which a hash-order or
// address-order tie-break would not give.
TEST(StaticInitSchedule, EqualPrioritiesAreBrokenDeterministically) {
    auto const a = scheduleOnly({
        LirStaticInitEntry{SymbolId{40}, before(101)},
        LirStaticInitEntry{SymbolId{20}, before(101)},
        LirStaticInitEntry{SymbolId{30}, before(101)},
    });
    auto const b = scheduleOnly({
        LirStaticInitEntry{SymbolId{30}, before(101)},
        LirStaticInitEntry{SymbolId{40}, before(101)},
        LirStaticInitEntry{SymbolId{20}, before(101)},
    });
    auto const oa = symbolsOf(staticInitOrder(a, StaticInitPhase::BeforeEntry));
    EXPECT_EQ(oa, symbolsOf(staticInitOrder(b, StaticInitPhase::BeforeEntry)))
        << "the same set must order identically however it was collected";
    EXPECT_EQ(oa, (std::vector<std::uint32_t>{20, 30, 40}))
        << "…by merged SymbolId, which is the one stable key at this tier";
}

// ── the RUNNER ──────────────────────────────────────────────────────────────

// `entryTrampoline`: the synthesized entry calls the scheduled functions. The
// witness is the relocation count on functions[0] — one per call site — which is
// STRICTLY GREATER with a schedule than without.
TEST(StaticInitSchedule, EntryTrampolineRunnerEmitsOneCallPerScheduledFunction) {
    auto const target = x64Target();
    ASSERT_TRUE(target != nullptr);
    auto const fmt = makeElfExecFormat(
        R"("staticInitializers": { "runner": "entryTrampoline" },)");
    ASSERT_TRUE(fmt != nullptr);

    AssembledModule bare = makeThreeFnModule();
    bare.staticInitSchedule.clear();
    DiagnosticReporter r0;
    ASSERT_TRUE(injectEntryTrampoline(bare, *target, *fmt, r0));

    AssembledModule sched = makeThreeFnModule();
    DiagnosticReporter r1;
    ASSERT_TRUE(injectEntryTrampoline(sched, *target, *fmt, r1));

    EXPECT_EQ(trampolineRelCallCount(sched),
              trampolineRelCallCount(bare) + 2u)
        << "one extra call site for the before-entry function and one for the "
           "after-entry one — the trampoline IS the runtime on this arm, so "
           "these calls are the whole feature";
}

// ⚠⚠ `imageLoader` EMITS NONE. This is the arm where the PLATFORM walks a
// section, so trampoline calls would be a DUPLICATE rather than a redundancy:
// every initializer would run twice. No shipped format selects this arm today,
// so this pin is the only thing standing between the branch and silent deletion.
TEST(StaticInitSchedule, ImageLoaderRunnerEmitsNoTrampolineCalls) {
    auto const target = x64Target();
    ASSERT_TRUE(target != nullptr);
    auto const fmt = makeElfExecFormat(
        R"("staticInitializers": { "runner": "imageLoader" },)");
    ASSERT_TRUE(fmt != nullptr);

    AssembledModule bare = makeThreeFnModule();
    bare.staticInitSchedule.clear();
    DiagnosticReporter r0;
    ASSERT_TRUE(injectEntryTrampoline(bare, *target, *fmt, r0));

    AssembledModule sched = makeThreeFnModule();
    DiagnosticReporter r1;
    ASSERT_TRUE(injectEntryTrampoline(sched, *target, *fmt, r1));

    EXPECT_EQ(trampolineRelCallCount(sched), trampolineRelCallCount(bare))
        << "the loader owns the channel on this arm; a call here would run "
           "every initializer TWICE";
}

// ── the format-document RULES ───────────────────────────────────────────────

// `entryTrampoline` claims "the entry DSS synthesizes calls them", and only an
// EXEC flavor HAS such an entry. Refused at LOAD, where the claim is written.
TEST(StaticInitSchedule, EntryTrampolineRunnerIsRefusedOnANonExecFlavor) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": ".o",
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": { "name": "synth-elf-obj-staticinit", "version": "0.1", "kind": "elf" },
      "elf": {
        "class": "elf64", "data": "lsb", "osabi": "sysv", "machine": 62,
        "type": "rel", "pageAlign": 4096, "interpreter": "", "bindNow": false
      },
      "staticInitializers": { "runner": "entryTrampoline" },
      "sections": [
        { "kind": "text", "name": ".text", "type": 1, "flags": 6,
          "addrAlign": 16, "entrySize": 0, "virtualAddress": 0 }
      ]
    })");
    ASSERT_FALSE(r.has_value())
        << "a relocatable object has no DSS-synthesized entry, so the claim that "
           "one calls its initializers is false and must not load";
}

// The CONTROL for the rule above: the SAME document on an EXEC flavor loads. A
// refusal that fired on both would be a rule against the key rather than against
// the mismatch.
TEST(StaticInitSchedule, EntryTrampolineRunnerLoadsOnAnExecFlavor) {
    auto const fmt = makeElfExecFormat(
        R"("staticInitializers": { "runner": "entryTrampoline" },)");
    EXPECT_TRUE(fmt != nullptr)
        << "the exec-flavor document that differs ONLY in flavor must load";
}

// An unknown runner spelling fails the load and the message names the closed set,
// rendered from the vocabulary table rather than restated beside it.
TEST(StaticInitSchedule, UnknownRunnerSpellingIsRefusedAndListsTheClosedSet) {
    auto const fmt = makeElfExecFormat(
        R"("staticInitializers": { "runner": "entrytrampoline" },)");
    EXPECT_TRUE(fmt == nullptr)
        << "the vocabulary is case-sensitive and closed; a near-miss must not "
           "silently load with no runner, which would emit a program whose "
           "initializers never run";
}

// A `staticInitializers` block that names no runner is refused: it declares the
// capability and answers nothing.
TEST(StaticInitSchedule, StaticInitializersBlockWithoutARunnerIsRefused) {
    auto const fmt = makeElfExecFormat(R"("staticInitializers": { },)");
    EXPECT_TRUE(fmt == nullptr);
}

// ── the SHIPPED documents ───────────────────────────────────────────────────

// Every shipped EXEC-flavor format declares a runner, and every non-exec one
// declares none. The first half is what makes the feature work at all; the second
// is what makes the linker's refusal reachable instead of dead code.
TEST(StaticInitSchedule, ShippedExecFormatsDeclareARunnerAndOthersDoNot) {
    static constexpr char const* kExec[] = {
        "elf64-x86_64-linux-exec", "elf64-aarch64-linux-exec",
        "elf64-x86_64-linux-pie",  "elf64-aarch64-linux-pie",
        "pe64-x86_64-windows-exec",
        "macho64-arm64-darwin-exec", "macho64-x86_64-darwin-exec",
    };
    for (char const* name : kExec) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        ASSERT_TRUE((*r)->staticInitRunner().has_value())
            << name << ": an exec flavor that declares no runner would make the "
                       "linker refuse every program using a constructor";
        EXPECT_EQ(*(*r)->staticInitRunner(), StaticInitRunner::EntryTrampoline)
            << name;
    }
    static constexpr char const* kNonExec[] = {
        "elf64-x86_64-linux", "elf64-x86_64-linux-staticlib",
        "elf64-x86_64-linux-dyn", "pe64-x86_64-windows-dll",
        "macho64-arm64-darwin-dylib",
    };
    for (char const* name : kNonExec) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        EXPECT_FALSE((*r)->staticInitRunner().has_value())
            << name << ": DSS synthesizes no entry for this flavor and emits no "
                       "table for a loader to walk, so declaring a runner would "
                       "be a claim it cannot deliver";
    }
}

// ── the ARGUMENT PARK ───────────────────────────────────────────────────────
//
// The before-entry calls sit between the trampoline's argc/argv materialization
// and the call to the user entry, and the argument registers are CALLER-saved —
// so without a park, `main(int argc, char **argv)` receives whatever the last
// initializer left behind.
//
// ⚠⚠ THIS PIN EXISTS BECAUSE THE END-TO-END ONE IS VACUOUS ON THIS HOST, AND
// THAT WAS MEASURED RATHER THAN REASONED. The corpus example checks argc, but on
// the WINDOWS gate host only its `pe64` arm runs — and PE fetches argv through
// CRT ACCESSOR CALLS (`processArgs.mechanism == crt-argv-accessors`), not through
// the argument registers, so the park is inert there and deleting it reddened
// NOTHING. The elf legs do exercise it end to end; this makes it observable
// everywhere.
//
// It is a SIZE pin because the park is `mov` traffic that emits no relocation, so
// the relocation count the two runner pins above use cannot see it. The bound is
// a LOWER bound rather than an exact size: the exact byte count is the encoder's
// business and pinning it here would red on an unrelated encoding change.
TEST(StaticInitSchedule, TheTrampolineParksTheArgumentRegistersAcrossTheCalls) {
    auto const target = x64Target();
    ASSERT_TRUE(target != nullptr);
    // A format whose arguments ride the ARGUMENT REGISTERS — the stack-vector
    // mechanism the elf documents declare. That is the arm the park protects.
    auto const fmt = makeElfExecFormat(
        R"("staticInitializers": { "runner": "entryTrampoline" },
           "processArgs": { "mechanism": "stack-vector",
                            "argcStackOffset": 0, "argvStackOffset": 8 },)");
    ASSERT_TRUE(fmt != nullptr);

    AssembledModule bare = makeThreeFnModule();
    bare.staticInitSchedule.clear();
    DiagnosticReporter r0;
    ASSERT_TRUE(injectEntryTrampoline(bare, *target, *fmt, r0));

    AssembledModule sched = makeThreeFnModule();
    DiagnosticReporter r1;
    ASSERT_TRUE(injectEntryTrampoline(sched, *target, *fmt, r1));

    ASSERT_FALSE(bare.functions.empty());
    ASSERT_FALSE(sched.functions.empty());
    auto const grew = static_cast<long long>(sched.functions.front().bytes.size())
                    - static_cast<long long>(bare.functions.front().bytes.size());
    // ⚠⚠ THE BOUND IS CALIBRATED AGAINST THE MUTANT, NOT GUESSED, and the first
    // guess was VACUOUS: `> 10` (the two calls alone) does NOT catch the defect,
    // ✔MEASURED — deleting the RESTORE half leaves the SAVE half, so the growth
    // only falls 25 → 19 and stays clear of 10. A pin that cannot fail is a
    // coverage claim nobody is honouring.
    //
    // 25 = two `call rel32` at 5 bytes each, plus FIVE `mov r64,r64` at 3 bytes
    // each: two argument saves, two argument restores, one status save.
    // ✔MEASURED on the shipped x86_64 target.
    //
    // ★ IT IS A `>=`, SO THE DIRECTION IT FAILS IN IS THE SAFE ONE. Growth is
    // fine — a longer encoding, or a future value that also needs parking, only
    // adds. A SHRINK is the signal, and every way of shrinking this sequence
    // means a save or a restore stopped being emitted. An encoding change that
    // genuinely made these instructions smaller would red it too, and that is
    // the right moment to re-read this arithmetic rather than to widen it.
    EXPECT_GE(grew, 25)
        << "the trampoline grew by " << grew << " bytes for two scheduled calls "
           "— fewer than the 25 that two calls plus the five register moves cost, "
           "so a save or a restore is missing and `main(argc, argv)` would "
           "receive whatever the last initializer left behind";
}
