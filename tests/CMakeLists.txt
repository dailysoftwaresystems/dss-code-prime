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
    target_include_directories(${target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests/test_support
    )

    # Reuse the shared test PCH (heavy STL + <gtest/gtest.h>). All test
    # targets compile with identical flags (DSS_SHARED_BUILD via the lib's
    # PUBLIC define, the same includes), so the one precompiled header is
    # valid for every one of them. No-op in Debug (CMAKE_DISABLE_PRECOMPILE_
    # HEADERS), so the linux-clang-asan leg builds PCH-free.
    target_precompile_headers(${target} REUSE_FROM dss_test_pch)
    dss_pch_reproducible(${target})  # consumer must match producer's PCH flags

    add_test(NAME ${DSS_NAME} COMMAND ${target})

    # Shipped-config discovery: findShippedConfig() prefers $DSS_CONFIG_ROOT
    # (a directory containing src/dss-config/) over its cwd-walk. Point it at
    # the repo root so every test resolves shipped config regardless of build
    # location — an OUT-OF-TREE build's ctest cwd is a build subdirectory with
    # no src/dss-config/ in its ancestry, which would otherwise null the loader
    # (loadShipped → nullptr → downstream null-deref). In-tree builds already
    # walk to the config; this makes out-of-tree/CI builds work identically.
    # (CTest ENVIRONMENT is a ';'-separated list; a repo path containing a
    # literal ';' would mis-split — impossible on normal Windows/WSL paths.)
    set_tests_properties(${DSS_NAME} PROPERTIES
        ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
endfunction()

# Shared PCH producer for the test executables (see DSS_PCH_HEADERS in the
# top-level CMakeLists). The ~166 test TUs are the larger half of the cold
# compile and every one of them includes <gtest/gtest.h> + the heavy STL set;
# precompiling them ONCE here and reusing across all test targets removes that
# reparse. It links dss-code-prime-lib + GTest::gtest purely to inherit their
# usage-requirement flags (the PUBLIC DSS_SHARED_BUILD define, the gtest include
# dir) so the precompiled header matches every consumer's compile line exactly.
# `dss_test_pch` is never linked anywhere; REUSE_FROM wires the build dependency.
add_library(dss_test_pch STATIC ${CMAKE_CURRENT_SOURCE_DIR}/test_support/pch_stub.cpp)
target_link_libraries(dss_test_pch PRIVATE dss-code-prime-lib GTest::gtest)
target_include_directories(dss_test_pch
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/test_support
)
target_precompile_headers(dss_test_pch PRIVATE ${DSS_PCH_HEADERS} <gtest/gtest.h>)
dss_pch_reproducible(dss_test_pch)  # producer: timestamp-free PCH → ccache hits

add_subdirectory(core)
add_subdirectory(test_support)
add_subdirectory(tokenizer)
add_subdirectory(analysis)
add_subdirectory(analysis/syntactic)
add_subdirectory(analysis/compilation_unit)
add_subdirectory(analysis/preprocess)
add_subdirectory(analysis/semantic)
add_subdirectory(hir)
add_subdirectory(mir)
add_subdirectory(opt)
add_subdirectory(lir)
add_subdirectory(asm)
add_subdirectory(link)
add_subdirectory(ffi)
add_subdirectory(program)
add_subdirectory(lsp)
add_subdirectory(examples)
