#pragma once

// ★★★ `__has_include` ON THE **GENERATED** HEADER, AND THE DISTINCTION IS THE
// WHOLE POINT — an earlier draft of this mechanism got it backwards in BOTH
// directions on the same day, so both errors are recorded here.
//
//   ⛔ `__has_include("program/dss_build_stamp.hpp")` — testing for THIS file,
//      from its consumer — is USELESS. This file is CHECKED IN, so the test is
//      always true and can never detect anything.
//   ⛔ A BARE `#include` of the generated header — the "fix" for the above — is
//      equally wrong, just less obviously: a missing edge then fails HERE, at
//      the include, as *"dss_build_stamp_generated.hpp: No such file"*. That
//      names a FILE the reader has never heard of instead of naming the FACT,
//      and it pre-empts the `#error` forty lines below that was written
//      precisely to name the fact. The guard is dead code behind it.
//
// ⇒ The generated header is on the include path IF AND ONLY IF
// `dss_use_build_stamp(<target>)` wired it, so `__has_include` on THAT file is
// exactly the condition worth testing. Absent ⇒ fall through to the `#ifndef`
// below and fail with a message that names the missing CMake call.
// ⓘ It also still fires if the generated header exists but is malformed and
// defines nothing — the guard is on the MACRO, never on the file's presence.
#if __has_include("program/dss_build_stamp_generated.hpp")
#    include "program/dss_build_stamp_generated.hpp"
#endif

#include <string_view>

// The COMPILER'S OWN IDENTITY, as a single build-time-generated token.
//
// This header is the checked-in face of a GENERATED one. The value is computed
// by `cmake/DssBuildStamp.cmake` on every build and lands in
// `<build>/generated/program/dss_build_stamp_generated.hpp`; that script's
// docblock is where the reasoning lives and this one does not restate it.
// Shapes, for orientation only:
//
//     0.0.2                                   clean tree, git present
//     0.0.2+g4095c13b2f1a                      …and the commit it came from
//     0.0.2+g4095c13b2f1a.dirty9f2c1b0d7e4a3d56 …plus a digest of the DIRT
//     0.0.2+nogit20260817T091455Z.7            no git, or no work tree
//
// ── WHO ASKS, AND WHY IT MATTERS THAT NOBODY ELSE DOES ──────────────────────
// The runtime object cache (`runtime_object_cache.hpp`) keys a compiled
// artifact on its inputs, and THE COMPILER IS AN INPUT — the same source,
// target and config compiled by different codegen must not select the same
// entry. That cache is the intended sole consumer.
//
// ⚠⚠ DO NOT INCLUDE THIS FROM A WIDELY-INCLUDED HEADER. The generated header
// changes on every dirty edit, so EVERY translation unit that reaches this one
// recompiles on every dirty edit. Reaching it is opt-in at the build level —
// a target sees it only after `dss_use_build_stamp(<target>)` in CMake — and
// that gate exists precisely so this cannot spread by an ordinary `#include`
// added in passing. Adding a second includer is a review-stop, not a
// preference.
//
// ⚠ THE STAMP IDENTIFIES THE COMPILER, NOT THE PRODUCT VERSION. It is NOT a
// substitute for `DSS_PROJECT_VERSION`: it carries build-local components
// (commit, dirt digest) that must never reach a user-visible version string or
// a predefined macro. It exists to be hashed into a cache key.
//
// Undefined ⇒ the build is BROKEN, loudly, here and now. A default would ship a
// compiler whose cache key cannot distinguish it from any other build — the
// exact under-invalidation the cache is designed to make impossible, and it
// would fail silently, by serving a wrong artifact rather than by not building.
#ifndef DSS_BUILD_STAMP
#    error "DSS_BUILD_STAMP is not defined — the target must opt in with dss_use_build_stamp(<target>) so cmake/DssBuildStamp.cmake generates and exposes the header (see the top-level CMakeLists.txt)."
#endif

namespace dss::runtime {

// The stamp as a value, for callers that would otherwise re-wrap the macro.
// `constexpr` because it is a string literal: no initialization order, no
// runtime cost, and it participates in a key computation as plain bytes.
inline constexpr std::string_view kBuildStamp = DSS_BUILD_STAMP;

} // namespace dss::runtime
