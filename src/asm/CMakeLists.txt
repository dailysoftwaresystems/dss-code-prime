add_library(asm OBJECT
    asm.cpp
    # Plan 29 P5: the per-instruction text->LIR engine, shared by the
    # standalone `.s` walker (asm_text_to_lir.cpp) and by an embedded
    # `__asm__` template. One engine, two callers -- see the header banner.
    asm_template_to_lir.cpp
    asm_text_to_lir.cpp
    disasm.cpp
    format/fixed32.cpp
    format/fixed32_disasm.cpp
    format/x86_variable.cpp
    format/x86_variable_disasm.cpp
)

target_include_directories(asm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:asm>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(asm PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(asm PRIVATE core lir mir)

# D-AS-ENCODINGSLOT-EXHAUSTIVE-WARN — the EncodingSlotKind exhaustiveness gate
# this file used to carry is now PROJECT-WIDE (root CMakeLists.txt), so the four
# format/ walkers (fixed32.cpp, fixed32_disasm.cpp, x86_variable.cpp,
# x86_variable_disasm.cpp) are covered by construction rather than by a list.
#
# ★ WHY THE LIST HAD TO GO, and it is not tidiness. The hand-written list here
# named FOUR files and missed `asm_text_to_lir.cpp` — same target, same
# no-`default` control-flow dispatch shape, same defect class — which then
# shipped an unhandled `Switch` enumerator that only a macOS build ever
# reported (D-BUILD-CLANG-ONLY-WARNINGS-INVISIBLE-TO-THE-MSVC-AND-GCC-LEGS).
# A gate you have to remember to extend is a gate that gets extended late.
#
# ⚠ Do NOT restore a per-file block here. The old comment justified the
# per-file scope as protecting asm.cpp / disasm.cpp, whose switches use
# deliberate subset-with-`default` forms — but that protection was never
# needed: `-Werror=switch` / C4062 are the NO-`default` flavor and are
# suppressed outright by the presence of a `default:` arm. The scoping cost a
# missed file and bought nothing.
