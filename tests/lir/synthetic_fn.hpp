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

// ── THE PROBE'S KIND -> TypeId RESOLUTION, ONE OWNER ──────────────────────
//
// D-TEST-LIR-AND-LINK-SUITES-MINT-AN-OPERAND-LESS-PTR. These lowering probes
// name a type by its KIND because what they assert is register CLASS and
// WIDTH, not structure -- and every kind they name is a LEAF kind that
// `primitive(k)` builds directly, EXCEPT `Ptr`.
//
// `Ptr` is structural: it carries a pointee operand. `primitive(TypeKind::Ptr)`
// minted a pointer with NO pointee -- a well-formed `TypeRecord` that interns,
// flows through `fnSig` / MIR->LIR / the object writers, and never complains,
// which is exactly why it survived here unnoticed. The interner now REFUSES it
// (D-LATTICE-PRIMITIVE-BUILDER-ACCEPTS-A-NON-PRIMITIVE-KIND), so the probes say
// what they always meant: an opaque `void*`, built with the builder its kind
// requires.
//
// ★ ONE owner, so a probe cannot reach `primitive` with a structural kind
// through this path again. ⓘ The alternative shape is a `TypeId`-taking
// `buildSyntheticFn`, which is more honest still -- a probe would name its type
// rather than a kind -- but the interner is created INSIDE this helper, so a
// caller has nothing to build a TypeId with until the signature grows a
// type-factory callback and all 27 call sites move. Recorded, not done.
[[nodiscard]] inline ::dss::TypeId
probeTypeOfKind(::dss::TypeInterner& in, ::dss::TypeKind k) {
    if (k == ::dss::TypeKind::Ptr)
        return in.pointer(in.primitive(::dss::TypeKind::Void));
    return in.primitive(k);
}

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
    for (auto k : paramKinds) params.push_back(probeTypeOfKind(out.interner, k));
    auto const retT = probeTypeOfKind(out.interner, returnKind);
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
