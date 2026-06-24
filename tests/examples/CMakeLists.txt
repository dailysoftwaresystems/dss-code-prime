# `examples/` ctest harness — registers ONE ctest entry per example
# discovered under <repo>/examples/<lang>/<name>/expected.json. Each
# entry spawns `dss_examples_runner` with the example directory as
# argv[1]; the runner drives the in-process compile + spawn + strict
# assertion sequence (see examples_runner.cpp).
#
# Discovery is at cmake-configure time via file(GLOB_RECURSE). New
# examples need a cmake reconfigure (`cmake .`) before ctest sees
# them — the trade-off is zero runtime discovery cost AND ctest -j
# parallelism across examples.
#
# Working directory invariant: the runner uses `ScratchDir(InsideRepo)
# + useAsCwd()`. ScratchDir walks UP from cwd looking for
# `src/dss-config/`, so the test process MUST start in (or under)
# the repo tree. CMake's WORKING_DIRECTORY is set to ${CMAKE_SOURCE_DIR}
# below to guarantee this on every host.

add_executable(dss_examples_runner examples_runner.cpp)
target_link_libraries(dss_examples_runner
    PRIVATE
        dss-code-prime-lib
        GTest::gtest
        GTest::gtest_main
        nlohmann_json::nlohmann_json
)
target_include_directories(dss_examples_runner
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/test_support
)

# Reuse the shared test PCH (defined in tests/CMakeLists.txt). The extra
# nlohmann_json include dir on this target is additive and does not affect the
# precompiled STL/gtest set. No-op in Debug (PCH disabled).
target_precompile_headers(dss_examples_runner REUSE_FROM dss_test_pch)
dss_pch_reproducible(dss_examples_runner)  # consumer must match producer's PCH flags

file(GLOB_RECURSE EXAMPLE_MANIFESTS
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/examples/*/*/expected.json")

foreach(MANIFEST ${EXAMPLE_MANIFESTS})
    get_filename_component(EXAMPLE_DIR ${MANIFEST} DIRECTORY)
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_DIR} NAME)
    get_filename_component(LANG_DIR ${EXAMPLE_DIR} DIRECTORY)
    get_filename_component(LANG_NAME ${LANG_DIR} NAME)
    set(TEST_NAME "examples/${LANG_NAME}/${EXAMPLE_NAME}")
    add_test(NAME ${TEST_NAME}
             COMMAND dss_examples_runner ${EXAMPLE_DIR}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
