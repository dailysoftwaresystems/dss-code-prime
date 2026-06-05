add_library(opt OBJECT
    optimizer.cpp
    optimizer_json.cpp
    passes/const_fold.cpp
    passes/copy_prop.cpp
    passes/cse.cpp
    passes/dce.cpp
    passes/inlining.cpp
    passes/licm.cpp
    passes/mem2reg.cpp
    passes/mir_rebuild_helper.cpp
    passes/simplify_cfg.cpp
)

target_include_directories(opt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Same DLL-export discipline as mir/lir/asm. Aggregated into
# dss-code-prime-lib via $<TARGET_OBJECTS:opt> at src/CMakeLists.txt.
target_compile_definitions(opt PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# OPT1 cycle 1 substrate: the optimizer reads + rewrites MIR, so it
# depends on mir + core. The ConstFold pass reuses the CE arithmetic
# helpers from `hir/const_eval_arith.hpp` (header-only, no linker
# dependency on hir's TU). The JSON loader depends on nlohmann_json
# via core's public interface. Per plan 22's "linker-agnostic"
# guarantee, no link-tier dependency. Future LIR-tier passes (OPT5+)
# will add lir.
target_link_libraries(opt PRIVATE core mir hir nlohmann_json::nlohmann_json)

# D-OPT-RUNPASS-EXHAUSTIVE-WARN closure: enforce exhaustive PassId
# dispatch at compile time, scoped to optimizer.cpp ONLY. The runPass
# switch has no default arm, so the "no-default + missing case = error"
# flavor (GCC/Clang `-Werror=switch`; MSVC `/we4062`) catches a future
# PassId enumerator added without a dispatch arm at build time — closing
# the runtime-only `X_UnknownPassId` gap the static_assert on
# kPassIdCount can't reach.
# Per-file scope is load-bearing: pass implementations legitimately use
# subset-switches WITH defaults, which the with-default flavor (/we4061
# / -Wswitch-enum) would noisily flag without catching real drift.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(optimizer.cpp PROPERTIES
        COMPILE_OPTIONS "-Werror=switch")
elseif(MSVC)
    set_source_files_properties(optimizer.cpp PROPERTIES
        COMPILE_OPTIONS "/we4062")
endif()
