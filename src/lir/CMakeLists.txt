add_library(lir OBJECT
    lir.cpp
    lir_2addr_legalize.cpp
    lir_callconv.cpp
    lir_literal_pool.cpp
    lir_liveness.cpp
    lir_pass_util.cpp
    lir_peephole.cpp
    lir_reg_constraints.cpp
    lir_regalloc.cpp
    lir_rewrite.cpp
    lir_wide_call_args.cpp
    lir_text.cpp
    lir_verifier.cpp
)

target_include_directories(lir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dsscp-lib via $<TARGET_OBJECTS:lir>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(lir PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(lir PRIVATE core)

# D-LIR-TEXT-EXHAUSTIVE-WARN (2026-08-03, TF-C112) — the exhaustive closed-enum
# gate on lir_text.cpp is now PROJECT-WIDE (root CMakeLists.txt). The TU's two
# no-`default` switches — the `TypeKind` round-trip-tag table `typeKindName` and
# `parseInst`'s `TargetTerminatorKind` terminator dispatch — remain covered;
# they are just no longer covered BY NAME.
#
# The history is kept because it is the strongest evidence for the gate: three
# enumerators (`TypeKind::VolatileQual`, `TypeKind::NullptrT`,
# `TargetTerminatorKind::IndirectBr`) had been appended to their enums with no
# arm here and were INVISIBLE to every local GCC/MSVC build for their whole
# lifetime — the TypeKind pair emitting an un-parseable `core ?` tag (two live
# kinds sharing one un-invertible sentinel), the terminator one DROPPING the
# terminator until `LirBuilder` `abort()`ed.
#
# The TU's two other switches stay unaffected, and that is automatic rather
# than arranged: the lexer's `switch (c)` is over a char, and `parseOperand`'s
# `TokKind` dispatch is a deliberate subset-with-`default`. Both `-Wswitch` and
# C4062 are suppressed by the presence of a `default:` arm.
#
# ⚠ This was the FOURTH per-file copy of one gate. The fifth copy's list is
# what missed `src/asm/asm_text_to_lir.cpp` — see
# D-BUILD-CLANG-ONLY-WARNINGS-INVISIBLE-TO-THE-MSVC-AND-GCC-LEGS. Do not add a
# sixth; extend the chokepoint instead.

# ML5 cycle 3: MIR→LIR isel pass lives in `lowering/`; separate OBJECT lib
# so the LIR substrate itself stays MIR-free (the LIR module is target-
# blind output; the lowerer is the dependency-direction bridge).
add_subdirectory(lowering)
