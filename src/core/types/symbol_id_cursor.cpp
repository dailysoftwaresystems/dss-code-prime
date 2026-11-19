#include "core/types/symbol_id_cursor.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

// The one emitter of each refusal form (the contract is on the declarations, symbol_id_cursor.hpp).
namespace dss {

namespace {

// What ran out, in one clause both emitters share.
[[nodiscard]] std::string exhaustionClause(SymbolIdSpaceKind kind, std::uint64_t at) {
    switch (kind) {
        case SymbolIdSpaceKind::CountingEnd:
            return std::format("SymbolId space is exhausted (its end is {}) -- no fresh symbol can be minted, "
                               "and a wrapped id would be 0, the invalid id, or one a symbol already holds",
                               at);
        case SymbolIdSpaceKind::IndexPosition:
            return std::format("SymbolId space is exhausted: position {} reaches {}, the first SymbolId a "
                               "format writer reserves -- an id is its position, and an index space ends there",
                               at, kFirstWriterReservedSymbolIdValue);
    }
    std::fprintf(stderr, "dss::symbol_id_cursor fatal: SymbolIdSpaceKind value %u is no enumerator\n",
                 static_cast<unsigned>(kind));
    std::abort();
}

} // namespace

void refuseExhaustedSymbolIds(std::string_view who, std::string_view space, SymbolIdSpaceKind kind,
                              std::uint64_t at) {
    std::string const line = std::format("dss::{} fatal: {}'s {}\n", who, space, exhaustionClause(kind, at));
    std::fputs(line.c_str(), stderr);
    std::abort();
}

void reportExhaustedSymbolIds(SymbolIdRefusalSink sink, std::string_view who, SymbolIdSpaceKind kind,
                              std::uint64_t at) {
    ParseDiagnostic d;
    d.code     = sink.code;
    d.severity = DiagnosticSeverity::Error;
    // The ONLY statement that the output was refused: no volume gate may take it (and its code is an
    // unsuppressable member, `kWhySymbolIdSpace`, so no policy may either).
    d.delivery = DiagnosticDelivery::Guaranteed;
    d.actual   = std::format("{}: the {}", who, exhaustionClause(kind, at));
    sink.reporter->report(std::move(d));
}

ReportedSymbolIds::~ReportedSymbolIds() {
    if (handedOut_ && !settled_) {
        std::string const line = std::format(
            "dss::ReportedSymbolIds fatal: {} handed out ids and was destroyed with its output neither sealed nor "
            "withheld -- an output could carry an id of a refused space\n",
            who_);
        std::fputs(line.c_str(), stderr);
        std::abort();
    }
}

} // namespace dss
