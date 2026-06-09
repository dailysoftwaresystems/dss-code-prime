add_library(asm OBJECT
    asm.cpp
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

# D-AS-ENCODINGSLOT-EXHAUSTIVE-WARN: enforce EXHAUSTIVE EncodingSlotKind
# dispatch at compile time in the four format walkers. Each switch over
# EncodingSlotKind is written with NO `default:` arm (one explicit case per
# enumerator + a post-switch out-of-range backstop), so the "no-default +
# missing case = error" flavor (GCC/Clang `-Werror=switch`; MSVC `/we4062`)
# turns a future EncodingSlotKind enumerator added without a matching arm
# into a BUILD ERROR — on local MSVC too. That closes the gap this gate was
# born from: the drift previously surfaced only as a `-Wswitch` warning on
# the macOS / linux-clang CI legs and was invisible on local MSVC. Mirrors
# the optimizer.cpp runPass gate (D-OPT-RUNPASS-EXHAUSTIVE-WARN,
# src/opt/CMakeLists.txt).
#
# Per-file scope is load-bearing: it must NOT spread to asm.cpp / disasm.cpp,
# whose switches legitimately use subset-with-`default` forms. The no-default
# flavor (4062 / -Werror=switch) leaves those alone; the with-default flavor
# (/we4061 / -Wswitch-enum) would noisily flag them without catching real
# drift.
set(_dss_asm_exhaustive_switch_files
    format/fixed32.cpp
    format/fixed32_disasm.cpp
    format/x86_variable.cpp
    format/x86_variable_disasm.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(${_dss_asm_exhaustive_switch_files} PROPERTIES
        COMPILE_OPTIONS "-Werror=switch")
elseif(MSVC)
    set_source_files_properties(${_dss_asm_exhaustive_switch_files} PROPERTIES
        COMPILE_OPTIONS "/we4062")
endif()
