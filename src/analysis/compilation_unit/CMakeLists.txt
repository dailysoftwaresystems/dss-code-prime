add_library(analysis_compilation_unit OBJECT
    compilation_unit.cpp
)

target_include_directories(analysis_compilation_unit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:analysis_compilation_unit>
# at src/CMakeLists.txt — same pattern as core/, tokenizer/, analysis_syntactic/.
# DSS_BUILDING_DLL must live here (not on the SHARED target) so DSS_EXPORT
# decorations resolve to dllexport when these .obj files are produced on MSVC.
target_compile_definitions(analysis_compilation_unit PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# CU1 receives Tree by value via UnitBuilder::addTree — it does NOT invoke
# the parser or tokenizer internally. CU2 will add a tokenizer + analysis_syntactic
# dependency for addFile/addInMemory.
target_link_libraries(analysis_compilation_unit PRIVATE core)
