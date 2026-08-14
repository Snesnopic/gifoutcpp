cmake_minimum_required(VERSION 3.16)
project(gifoutcpp VERSION 0.1.0 LANGUAGES C CXX)

if(MSVC)
    # force static msvc runtime linkage (/mt for release, /mtd for debug)
    if(POLICY CMP0091)
        cmake_policy(SET CMP0091 NEW)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# without this a plain `cmake -S . -B build` compiles with no -O flag at all
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release")
endif()

option(GIFOUT_BUILD_BENCH "Build the research harness that links gifsicle and flexigif" OFF)

# core: everything a library consumer needs, no i/o policy and no cli
add_library(gifout_core
    src/gif_lzw.cpp
    src/gif_lzw_search.cpp
    src/gif_encoder.cpp
    src/gif_optimizer.cpp
    src/gif_reader.cpp
    src/gif_writer.cpp
)
target_include_directories(gifout_core PUBLIC include)

# cli: a thin consumer of the core, mirroring how chisel would use it
add_executable(gifoutcpp src/main.cpp)
target_link_libraries(gifoutcpp PRIVATE gifout_core)
target_compile_definitions(gifoutcpp PRIVATE GIFOUTCPP_VERSION="${PROJECT_VERSION}")

install(TARGETS gifoutcpp DESTINATION bin)

if(GIFOUT_BUILD_BENCH)
    add_subdirectory(bench)
endif()
