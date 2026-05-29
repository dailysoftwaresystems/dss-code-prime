# Tests root. Each module's tests live under tests/<module>/.

# Helper: register a GoogleTest executable, link it against the shared lib,
# and register it with ctest. Usage:
#   dss_add_test(NAME core/test_strong_ids SOURCES test_strong_ids.cpp)
function(dss_add_test)
    set(options)
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(DSS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(REPLACE "/" "_" target "dss_${DSS_NAME}")
    add_executable(${target} ${DSS_SOURCES})
    target_link_libraries(${target} PRIVATE dss-code-prime-lib GTest::gtest GTest::gtest_main)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_test(NAME ${DSS_NAME} COMMAND ${target})
endfunction()

add_subdirectory(core)
add_subdirectory(tokenizer)
add_subdirectory(analysis/syntactic)
add_subdirectory(analysis/compilation_unit)
add_subdirectory(analysis/semantic)
add_subdirectory(hir)
add_subdirectory(mir)
add_subdirectory(lir)
add_subdirectory(asm)
add_subdirectory(lsp)
