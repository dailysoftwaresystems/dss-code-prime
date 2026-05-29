#pragma once

// Synthetic single-block MIR-function builder used by LIR tests that
// need to exercise specific MIR opcodes the c-subset frontend doesn't
// emit naturally (e.g. bitwise/float arithmetic, reverse Bitcast,
// liveness-shape probes). Promoted to a shared header in ML6 cycle 1
// (cycle-3e deferral D-3e.7) so multiple test binaries — `test_mir_to_lir`
// and `test_lir_liveness` — share the same harness.
//
// Lives in `tests/lir/` so it's available only to the LIR test binaries
// and isn't pulled into production object libs.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <span>
#include <utility>
#include <vector>

namespace dss::test_support {

struct SyntheticFn {
    ::dss::Mir          mir;
    ::dss::TypeInterner interner;
};

// Build a one-block MIR function with the given parameter types,
// return type, and body emitter. `body` is invoked with the open
// MirBuilder + the interner + the param TypeIds + the return TypeId
// so the caller can synthesize whatever MIR opcodes it wants.
template <class BodyFn>
SyntheticFn buildSyntheticFn(
    std::span<::dss::TypeKind const> paramKinds,
    ::dss::TypeKind                  returnKind,
    BodyFn&&                         body) {
    SyntheticFn out{::dss::Mir{},
                    ::dss::TypeInterner{::dss::CompilationUnitId{1}}};
    std::vector<::dss::TypeId> params;
    params.reserve(paramKinds.size());
    for (auto k : paramKinds) params.push_back(out.interner.primitive(k));
    auto const retT = out.interner.primitive(returnKind);
    auto const sig  = out.interner.fnSig(params, retT, ::dss::CallConv::CcSysV);
    ::dss::MirBuilder mb;
    mb.addFunction(sig, ::dss::SymbolId{1});
    ::dss::MirBlockId const bb = mb.createBlock(::dss::StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    body(mb, out.interner, params, retT);
    out.mir = std::move(mb).finish();
    return out;
}

} // namespace dss::test_support
