#pragma once
// ══ THE DESCRIPTOR WORD `va_list`, BOUND FOR A VALIDATION READ ══════════════════
// (P69 L2b2, lane `lm`; the contract is c82's, D-FFI-DESCRIPTOR-VA-LIST-TYPE)
//
// A shipped descriptor spells the list type by the descriptor word `va_list`
// (`ffi::kVaListDescriptorTypeName`): stdio.json's v* rows, <stdarg.h>'s typedefs, and
// each header that makes `va_list` visible on a pair (`shippedTypedef` of stdarg.h). The
// word resolves ONLY through the binding its READER passes — the semantic tier binds the
// target's list type, the pipeline's `.s` read the pair's ABI's — and a read that reaches
// it with none fails loud, naming the binding it lacks. A typedef's variants are decoded
// eagerly, so a read of such a header reaches the word on every pair.
//
// This is the binding a VALIDATION read passes: the one builder's SysV list
// (`vaListTypes`, aggregate_layout.hpp), never a hand copy. No validation test reads the
// id; a test that compares a va_list position binds its pair's own strategy instead.
#include "core/types/aggregate_layout.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/shipped_lib_descriptor.hpp"

#include <array>

namespace dss::test_support {

[[nodiscard]] inline std::array<NamedTypeBinding, 1> validationVaListBinding(TypeInterner& interner) {
    return {NamedTypeBinding{ffi::kVaListDescriptorTypeName,
                             vaListTypes(VaListStrategy::SysVRegisterSave, interner).list}};
}

}  // namespace dss::test_support
