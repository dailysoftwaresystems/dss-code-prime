add_library(ffi_mangling OBJECT
    c_mangle.cpp
)

target_include_directories(ffi_mangling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_compile_definitions(ffi_mangling PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# c_mangle.{hpp,cpp} depends on ObjectFormatKind (link tier — the
# enum lives in link/object_format_schema.hpp). No core dep beyond
# the headers `link` already pulls in.
target_link_libraries(ffi_mangling PRIVATE
    core
    link
)
