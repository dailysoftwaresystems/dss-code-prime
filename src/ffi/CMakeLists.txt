add_library(ffi OBJECT
    binary_reader.cpp
)

target_include_directories(ffi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:ffi>.
target_compile_definitions(ffi PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(ffi PRIVATE core)
