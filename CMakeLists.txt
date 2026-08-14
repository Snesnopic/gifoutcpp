cmake_minimum_required(VERSION 3.16)
project(gifoutcpp VERSION 0.2.0 LANGUAGES C CXX)

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

# std::thread needs an explicit -pthread on some platforms; freebsd fails to link
# without it, while glibc and macos happen to resolve it anyway
find_package(Threads REQUIRED)

# core: everything a library consumer needs, no i/o policy and no cli
add_library(gifout_core
    src/gifoutcpp.cpp
    src/gif_lzw.cpp
    src/gif_lzw_search.cpp
    src/gif_encoder.cpp
    src/gif_optimizer.cpp
    src/gif_reader.cpp
    src/gif_writer.cpp
)
target_include_directories(gifout_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
set_target_properties(gifout_core PROPERTIES VERSION ${PROJECT_VERSION} EXPORT_NAME core)
add_library(gifoutcpp::core ALIAS gifout_core)
target_link_libraries(gifout_core PUBLIC Threads::Threads)

# cli: a thin consumer of the core, mirroring how chisel would use it
add_executable(gifoutcpp src/main.cpp)
target_link_libraries(gifoutcpp PRIVATE gifout_core)
target_compile_definitions(gifoutcpp PRIVATE GIFOUTCPP_VERSION="${PROJECT_VERSION}")

# a consumer gets find_package(gifoutcpp) and links gifoutcpp::core
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS gifout_core EXPORT gifoutcppTargets)
install(TARGETS gifoutcpp DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/gifoutcpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT gifoutcppTargets NAMESPACE gifoutcpp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gifoutcpp)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/gifoutcppConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gifoutcppConfig.cmake"
     "include(CMakeFindDependencyMacro)\nfind_dependency(Threads)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/gifoutcppTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gifoutcppConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/gifoutcppConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gifoutcpp)

option(GIFOUT_BUILD_TESTS "Build the test binary" ON)
if(GIFOUT_BUILD_TESTS)
    enable_testing()
    add_executable(gifout_tests tests/gifout_tests.cpp)
    target_link_libraries(gifout_tests PRIVATE gifout_core)
    add_test(NAME gifout_tests COMMAND gifout_tests)
endif()

if(GIFOUT_BUILD_BENCH)
    add_subdirectory(bench)
endif()
