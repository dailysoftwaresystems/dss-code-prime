add_library(ffi_abi OBJECT
    abi_catalog.cpp
)

target_include_directories(ffi_abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_compile_definitions(ffi_abi PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# Declared deps mirror the include surface: abi_catalog.{hpp,cpp}
# reads TargetSchema (core), ObjectFormatSchema (link), and reports
# via DiagnosticReporter (core).
target_link_libraries(ffi_abi PRIVATE
    core
    link
)
