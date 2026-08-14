# research harness only: it links the gifsicle and flexigif sources out of a
# chisel checkout so the numbers describe exactly what chisel ships
set(CHISEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../chisel" CACHE PATH "chisel source root")
if(NOT EXISTS "${CHISEL_ROOT}/third_party/gifsicle/CMakeLists.txt")
    message(FATAL_ERROR "CHISEL_ROOT does not point at a chisel checkout: ${CHISEL_ROOT}")
endif()

set(BUILD_GIFSICLE_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_GIFDIFF OFF CACHE BOOL "" FORCE)
set(BUILD_GIFVIEW OFF CACHE BOOL "" FORCE)
add_subdirectory("${CHISEL_ROOT}/third_party/gifsicle" gifsicle EXCLUDE_FROM_ALL)
add_subdirectory("${CHISEL_ROOT}/third_party/flexigif" flexigif EXCLUDE_FROM_ALL)

add_executable(stage_bench stage_bench.cpp)
target_include_directories(stage_bench PRIVATE
        "${CHISEL_ROOT}/third_party/gifsicle/include"
        "${CHISEL_ROOT}/third_party/flexigif/core")
target_link_libraries(stage_bench PRIVATE libgifsicle flexigif_core)
target_compile_options(stage_bench PRIVATE -g -fno-omit-frame-pointer)

add_executable(inst_bench instrumented/inst_bench.cpp instrumented/LzwEncoder_inst.cpp)
target_include_directories(inst_bench PRIVATE
        "${CHISEL_ROOT}/third_party/flexigif/core"
        instrumented)
target_link_libraries(inst_bench PRIVATE flexigif_core)
target_compile_options(inst_bench PRIVATE -g -fno-omit-frame-pointer)

# cross-check: our decoder against gifsicle's on the same file
add_executable(decode_check decode_check.cpp)
target_include_directories(decode_check PRIVATE "${CHISEL_ROOT}/third_party/gifsicle/include")
target_link_libraries(decode_check PRIVATE gifout_core libgifsicle)

add_executable(encode_check encode_check.cpp)
target_include_directories(encode_check PRIVATE "${CHISEL_ROOT}/third_party/gifsicle/include")
target_link_libraries(encode_check PRIVATE gifout_core libgifsicle)
