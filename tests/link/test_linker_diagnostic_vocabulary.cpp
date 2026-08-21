// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET — the linker's one
// site of the class.
//
// The class: a diagnostic that renders the set of ACCEPTED spellings as a string
// LITERAL while acceptance itself is decided by a table or an enum. The two owners
// drift, and the drift is invisible in both directions — a config author reads the
// message, so a stale one tells them BY NAME that a spelling the loader accepts does
// not exist, or that one it refuses is fine. A test that asserts the sentence
// CONTAINS a name stays green the whole way down, because the sentence is what the
// test reads.
//
// `linker::link`'s exec-flavored-format refusal (K_FormatLacksProcessExit) ends in a
// REMEDY that names the `processExit` mechanisms. Acceptance is owned by
// `kExitMechanismTable` (target_schema.hpp) minus the `None` sentinel, which
// `exitMechanismFromName("none")` resolves and the loader arm refuses explicitly.
//
// ✔MEASURED before the conversion: the literal read "'syscall' or 'by-name-import'"
// and the table's non-sentinel rows are exactly {"syscall", "by-name-import"} — this
// site had NOT drifted. The conversion buys the future.
//
// ★ THE PIN ASSERTS THE PROPERTY, NOT TODAY'S SENTENCE. It walks
// `kExitMechanismTable` itself and requires the message to name every DECLARABLE row
// and no refused one. Hardcoding the expected sentence here would be the same
// retyping one level up: it would need editing the day the vocabulary changes, which
// is precisely the day it should fail on its own.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"   // ExitMechanism, kExitMechanismTable
#include "link/linker.hpp"
#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace dss;

namespace {

// The SMALLEST format that reaches the refusal: exec-flavored (the ELF backend
// answers that from `elf.objectType`, never from a declared boolean — a hand-built
// struct must not be able to fake the flavour with one field) and declaring no
// `processExit`. Deliberately NOT loadable — `ObjectFormatData::validate()` rejects
// exactly this shape at config-load time — so it reaches the linker through the
// validation-bypassing in-memory constructor, which is the path the refusal exists
// to guard. Nothing past the gate runs, so no walker-facing field is set: the gate
// returns before any section, relocation or symbol is consulted.
[[nodiscard]] dss::detail::ObjectFormatData makeExecFormatWithoutProcessExit() {
    dss::detail::ObjectFormatData data;
    data.name    = "synth-elf-exec-no-exit-vocabulary-pin";
    data.version = "0.1";
    data.backend = dss::link::objectFormatBackendByConfigName("elf");
    data.elf.objectType = ElfObjectType::Exec;
    return data;
}

[[nodiscard]] AssembledModule makeOneFunctionModule() {
    AssembledModule m;
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax,42 ; ret
    m.functions.push_back(std::move(fn));
    return m;
}

}  // namespace

TEST(LinkerDiagnosticVocabulary, ProcessExitRemedyNamesEverySpellingTheTableAccepts) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ObjectFormatSchema const format{makeExecFormatWithoutProcessExit()};
    ASSERT_TRUE(format.isExecFlavor())
        << "the fixture must reach the exec-flavored gate, or this pin reads a "
           "message that was never produced";

    AssembledModule const mod = makeOneFunctionModule();
    DiagnosticReporter rep;
    LinkedImage const img = linker::link(mod, **target, format, rep);
    EXPECT_FALSE(img.ok());

    std::string message;
    std::size_t hits = 0;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::K_FormatLacksProcessExit) continue;
        ++hits;
        message = d.actual;
    }
    ASSERT_EQ(hits, 1u) << "expected exactly one K_FormatLacksProcessExit";

    // ── THE PROPERTY, walked off the owning table ───────────────────────────
    // Every row a config author MAY write must be named; the sentinel the loader
    // refuses must NOT be, or the message invites a value that cannot be accepted.
    // Quoting is part of the claim: an unquoted substring match would let `syscall`
    // be satisfied by the word appearing in ordinary prose.
    std::size_t declarable = 0;
    for (auto const& row : kExitMechanismTable.rows) {
        std::string const quoted = "'" + std::string{row.second} + "'";
        if (row.first == ExitMechanism::None) {
            EXPECT_EQ(message.find(quoted), std::string::npos)
                << "the message offers the refused sentinel spelling " << quoted
                << " as a remedy: " << message;
            continue;
        }
        ++declarable;
        EXPECT_NE(message.find(quoted), std::string::npos)
            << "the message does not name the accepted mechanism " << quoted
            << " — the sentence and `kExitMechanismTable` have drifted apart: "
            << message;
    }
    // Anti-vacuity: with fewer than two declarable rows the loop above could pass
    // over a message naming nothing at all.
    EXPECT_GE(declarable, 2u)
        << "kExitMechanismTable declares fewer than two writable mechanisms — the "
           "loop above is no longer asserting anything";

    // The remedy must still be actionable on its own: the KEY and the FORMAT are
    // named alongside the vocabulary.
    EXPECT_NE(message.find("processExit"), std::string::npos) << message;
    EXPECT_NE(message.find("synth-elf-exec-no-exit-vocabulary-pin"),
              std::string::npos)
        << message;
}
