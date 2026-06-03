add_library(opt OBJECT
    optimizer.cpp
)

target_include_directories(opt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Same DLL-export discipline as mir/lir/asm. Aggregated into
# dss-code-prime-lib via $<TARGET_OBJECTS:opt> at src/CMakeLists.txt.
target_compile_definitions(opt PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# OPT1 cycle 1 substrate: the optimizer reads + rewrites MIR, so it
# depends on mir + core. Future LIR-tier passes (OPT5+) will add lir.
# Per plan 22's "linker-agnostic" guarantee, no link-tier dependency.
target_link_libraries(opt PRIVATE core mir)
