cmake_minimum_required(VERSION 3.16)
project(optigif VERSION 1.0.0 LANGUAGES C CXX)

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

option(OPTIGIF_BUILD_BENCH "Build the research harness that links gifsicle and flexigif" OFF)

# std::thread needs an explicit -pthread on some platforms; freebsd fails to link
# without it, while glibc and macos happen to resolve it anyway
find_package(Threads REQUIRED)

# core: everything a library consumer needs, no i/o policy and no cli
add_library(optigif_core
    src/optigif.cpp
    src/gif_lzw.cpp
    src/gif_lzw_search.cpp
    src/gif_lzw_beam.cpp
    src/gif_encoder.cpp
    src/gif_optimizer.cpp
    src/gif_reader.cpp
    src/gif_writer.cpp
)
target_include_directories(optigif_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
set_target_properties(optigif_core PROPERTIES VERSION ${PROJECT_VERSION} EXPORT_NAME core)
add_library(optigif::core ALIAS optigif_core)
target_link_libraries(optigif_core PUBLIC Threads::Threads)
# the version is written down once, in the project() call above, and reaches the code
# as a define on the targets that need it
target_compile_definitions(optigif_core PRIVATE OPTIGIF_VERSION="${PROJECT_VERSION}")

# cli: a thin consumer of the core, mirroring how chisel would use it
add_executable(optigif src/main.cpp)
target_link_libraries(optigif PRIVATE optigif_core)
target_compile_definitions(optigif PRIVATE OPTIGIF_VERSION="${PROJECT_VERSION}")

# a consumer gets find_package(optigif) and links optigif::core
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS optigif_core EXPORT optigifTargets)
install(TARGETS optigif DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/optigif DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT optigifTargets NAMESPACE optigif::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/optigif)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/optigifConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/optigifConfig.cmake"
     "include(CMakeFindDependencyMacro)\nfind_dependency(Threads)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/optigifTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/optigifConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/optigifConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/optigif)

option(OPTIGIF_BUILD_TESTS "Build the test binary" ON)
if(OPTIGIF_BUILD_TESTS)
    enable_testing()
    add_executable(optigif_tests tests/optigif_tests.cpp)
    target_link_libraries(optigif_tests PRIVATE optigif_core)
    add_test(NAME optigif_tests COMMAND optigif_tests)
endif()

# libfuzzer targets; needs a clang that ships the runtime
option(OPTIGIF_BUILD_FUZZERS "Build the libFuzzer targets" OFF)
if(OPTIGIF_BUILD_FUZZERS)
    foreach(target fuzz_reader fuzz_roundtrip)
        add_executable(${target} fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE optigif_core)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()

if(OPTIGIF_BUILD_BENCH)
    add_subdirectory(bench)
endif()
