add_library(lir_lowering OBJECT
    mir_to_lir.cpp
)

target_include_directories(lir_lowering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:lir_lowering>. Same
# DSS_BUILDING_DLL convention as the sibling LIR / MIR / HIR OBJECT libs.
target_compile_definitions(lir_lowering PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# MIR→LIR isel reads MIR + writes LIR via the target schema. The lir_lowering
# library is the first consumer of cycle-2b's TargetSchema register-class +
# calling-convention vocabulary; linking it just needs core+mir+lir.
target_link_libraries(lir_lowering PRIVATE core mir lir)
