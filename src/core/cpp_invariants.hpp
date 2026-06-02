#pragma once

#include <cstdint>

// Centralised C++ language-level invariants the codebase relies on.
// Every TU that performs an operation depending on a specific C++
// standard guarantee should `#include "core/cpp_invariants.hpp"` —
// the build then HARD-FAILS on a non-conforming compiler/standard
// pair rather than silently miscompiling.
//
// Hoisted at A+B post-fold #2 (2026-06-01, architect Q3): the
// arithmetic-right-shift assumption was previously asserted in
// `src/ffi/binary_reader.cpp` only — a TU that doesn't include
// binary_reader.cpp (every other TU in the codebase) lost the gate.
// Notably `src/link/format/elf.cpp::emitArm64PltStub` and
// `src/link/format/exec_reloc_apply.hpp::Aarch64AdrPrelPgHi21`
// BOTH depend on the same guarantee and are NOT covered by the
// per-TU assert.

namespace dss {

// Arithmetic right shift on signed integers — guaranteed by C++20
// [expr.shift]/3, implementation-defined pre-C++20. The ARM64 PLT
// page-pair (`pageDiff >> 12` in elf.cpp's PLT emitter) and the
// Aarch64AdrPrelPgHi21 reloc kernel (`SA >> 12` / `(P) >> 12` in
// exec_reloc_apply.hpp) BOTH rely on this.
//
// Behavioural check (not a `__cplusplus`-version gate, since MSVC
// reports the conventional 199711L without `/Zc:__cplusplus` even
// under `/std:c++23`). The actual semantic invariant — that the
// compiler ACTUALLY emits arithmetic shift — is what we need.
// GCC / Clang / MSVC all implement it correctly even pre-C++20 by
// implementation choice; this assert fails only on a hypothetical
// conforming-but-logical-shift compiler.
static_assert((std::int64_t{-1} >> 1) == std::int64_t{-1},
              "dss requires arithmetic right shift on signed integers "
              "(C++20 [expr.shift]/3). The compiler appears to implement "
              "logical shift — file a compiler bug; do not work around.");

} // namespace dss
