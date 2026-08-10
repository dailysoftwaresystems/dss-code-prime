add_library(lir OBJECT
    lir.cpp
    lir_2addr_legalize.cpp
    lir_callconv.cpp
    lir_literal_pool.cpp
    lir_liveness.cpp
    lir_pass_util.cpp
    lir_regalloc.cpp
    lir_rewrite.cpp
    lir_wide_call_args.cpp
    lir_text.cpp
    lir_verifier.cpp
)

target_include_directories(lir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:lir>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(lir PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(lir PRIVATE core)

# D-LIR-TEXT-EXHAUSTIVE-WARN (2026-08-03, TF-C112): enforce EXHAUSTIVE
# closed-enum dispatch in lir_text.cpp. The file's two CLOSED-ENUM switches —
# the `TypeKind` round-trip-tag table `typeKindName` and `parseInst`'s
# `TargetTerminatorKind` terminator dispatch — are both written with NO
# `default:` arm (one explicit case per enumerator, plus a post-switch
# out-of-range backstop where the function returns a value), so the
# "no-default + missing case = error" flavor (GCC/Clang `-Werror=switch`;
# MSVC `/we4062`) turns a future enumerator added without a matching arm
# into a BUILD ERROR, on local MSVC too.
#
# Born from exactly that drift: `TypeKind::VolatileQual` /
# `TypeKind::NullptrT` and `TargetTerminatorKind::IndirectBr` had all been
# appended to their enums with no arm here, surfacing only as `-Wswitch` on
# the clang/ASan leg and INVISIBLE to every local GCC/MSVC build for their
# whole lifetime — the TypeKind pair as an un-parseable `core ?` tag (two
# live kinds sharing the one un-invertible sentinel spelling), the terminator
# one as a DROPPED terminator that made `LirBuilder` `abort()`.
#
# The TU's two OTHER switches are untouched and stay that way: the lexer's
# `switch (c)` is over a char, and `parseOperand`'s `TokKind` dispatch is a
# deliberate subset-with-`default` that diagnoses everything else. Both
# `-Wswitch` and C4062 are suppressed by the presence of a `default:`, so
# neither is dragged into the gate.
#
# The fourth instance of this gate pattern: mirrors
# D-DIAG-CODENAME-EXHAUSTIVE-WARN (src/core), D-AS-ENCODINGSLOT-EXHAUSTIVE-WARN
# (src/asm) and D-OPT-RUNPASS-EXHAUSTIVE-WARN (src/opt). Per-file scope is
# load-bearing: do NOT spread this to the sibling LIR TUs, whose switches
# legitimately use subset-with-`default` forms.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(lir_text.cpp PROPERTIES
        COMPILE_OPTIONS "-Werror=switch")
elseif(MSVC)
    set_source_files_properties(lir_text.cpp PROPERTIES
        COMPILE_OPTIONS "/we4062")
endif()

# ML5 cycle 3: MIR→LIR isel pass lives in `lowering/`; separate OBJECT lib
# so the LIR substrate itself stays MIR-free (the LIR module is target-
# blind output; the lowerer is the dependency-direction bridge).
add_subdirectory(lowering)
