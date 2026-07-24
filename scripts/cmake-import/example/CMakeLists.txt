cmake_minimum_required(VERSION 3.13)
project(demo_calc C)

# A tiny self-contained C executable: main.c calls a function defined in
# util.c, both include a header from include/, and the build carries two
# preprocessor defines. It exercises everything cmake-import aggregates
# (sources, an include dir, and -D defines) without any external libraries.
add_executable(demo_calc
    src/main.c
    src/util.c)

target_include_directories(demo_calc PRIVATE include)

target_compile_definitions(demo_calc PRIVATE SCALE=2 DEMO_BUILD)
