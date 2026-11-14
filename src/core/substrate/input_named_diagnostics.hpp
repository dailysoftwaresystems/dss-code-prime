#pragma once

// ══ THE ONE HOME OF "A REFUSAL OF ONE INPUT NAMES THE INPUT" (P69 fold 3e) ═════════════════════════════════════════
//
// THE INVARIANT: every diagnostic a read of ONE named input reports says whose bytes it is about. A binary reader --
// `ar`'s, each object format's `readRelocatableObject`, each FF1 surface reader -- reports what is wrong with the bytes
// it was handed, and most of them were never told whose they are: two inputs with one defect then read as ONE refusal,
// and the reporter's recent-duplicate window keeps only the first. ✔MEASURED on the Mac leg (lane `xa`): four unreadable
// members of two archives gave one refusal and "3 dropped as recent duplicates"; two unreadable dynamic libraries named
// to an assembly unit's build gave one "PE reader: export-directory RVA ... does not lie in any section's raw data" and
// "1 dropped as recent duplicates", naming neither library.
//
// The read reports into a reporter of the run's own configuration, and each diagnostic it reported is passed on with
// the input's name in front -- the path, as the input's own open refusal spells it, or `archive(member)` as the
// reference linkers name a member -- so each input's refusal is its own and says whose it is. A message that already
// quoted the name keeps its own words after the prefix.
//
// ★ IN core's SUBSTRATE, beside `runEveryUnit`: its sites are the program tier's reads of named object and archive
// inputs (src/program/compile_pipeline.cpp) and the FF1 chokepoint every binder reads a named library's surface
// through (`ffi::readImportsForTargetFormat`, src/ffi/ingest.cpp); every layer reaches core.
//
// Pinned by tests/program/test_every_unit.cpp: each unreadable input of each site refused ONCE BY NAME.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <format>
#include <string_view>
#include <utility>

namespace dss::substrate {

// Runs `read(inputReporter)` -- the read of the ONE input `input` names -- and passes each diagnostic it reported on
// to `reporter` with `input` in front. Answers what the read answered.
template <class Read>
[[nodiscard]] auto readNamingTheInput(std::string_view input, DiagnosticReporter& reporter, Read&& read) {
    DiagnosticReporter inputReporter{reporter.config()};
    auto result = std::forward<Read>(read)(inputReporter);
    for (ParseDiagnostic d : inputReporter.all()) {
        d.actual = std::format("{}: {}", input, d.actual);
        reporter.report(std::move(d));
    }
    return result;
}

}  // namespace dss::substrate
