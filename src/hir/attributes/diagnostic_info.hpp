#pragma once

#include "core/types/parse_diagnostic.hpp"   // DiagnosticCode
#include "core/types/strong_ids.hpp"           // HirNodeId / InvalidHirNode

#include <cstdint>
#include <string>

// Per-node recovery-info side-table value (HR5). Attached via
// `HirAttribute<DiagnosticInfo>` (aliased `HirDiagnosticMap` in hir_attrs.hpp) to
// nodes carrying the `HasError` HirFlag (hir_node.hpp) — i.e. nodes the lowering
// produced on a broken path. It records WHICH diagnostic flagged the node and
// WHAT recovery the lowering applied, mirroring the CST's `detail::Node`
// `diagnostic` index discipline (tree_node.hpp): the full `ParseDiagnostic`
// record lives in the compilation unit's `DiagnosticReporter`, keyed by code +
// span — this side-table names it so the verifier, the `.dsshir` text format
// (HR7), and error rendering can surface it without re-walking the reporter.
//
// Population is the CST→HIR lowering (HR8) on broken paths; HR5 establishes the
// home + shape. No `Hir` dependency beyond the shared id type — consumers bind it
// as `HirAttribute<DiagnosticInfo>`.

namespace dss {

// What the lowering did with a construct it could not lower cleanly.
enum class HirRecovery : std::uint8_t {
    None = 0,     // no recovery — a plain `Error` sentinel node
    Substituted,  // a typed placeholder stands in for the failed construct
    Dropped,      // the malformed construct was elided from the tree
    Synthesized,  // a node was synthesized to keep the surrounding tree well-formed
};

struct DiagnosticInfo {
    // The diagnostic code that put this node on a broken path. References the
    // record in the CU's reporter (NOT a copy of its message — that lives there,
    // keyed by code + span). `None` = error flagged without a specific code.
    DiagnosticCode code = DiagnosticCode::None;

    // What recovery the lowering applied at this node.
    HirRecovery recovery = HirRecovery::None;

    // When this node is a stand-in (`Substituted`/`Synthesized`), the "true"
    // source node it replaces — lets rendering point at the real origin.
    // `InvalidHirNode` = no distinct origin.
    HirNodeId origin = InvalidHirNode;

    // Optional human-readable context beyond the reporter record (e.g. "operand
    // type unresolvable; substituted Error"). Empty when the code says enough.
    std::string detail;
};

} // namespace dss
