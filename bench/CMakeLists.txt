cmake_minimum_required(VERSION 3.22)

project(steroids-bench)

add_executable(alloc-bench alloc_bench.cpp)

target_link_libraries(alloc-bench PRIVATE steroids::steroids)

target_compile_features(alloc-bench PRIVATE cxx_std_20)

target_compile_options(alloc-bench PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3;-march=native>)

add_executable(ool-bench ool_bench.cpp)

target_link_libraries(ool-bench PRIVATE steroids::steroids)

target_compile_features(ool-bench PRIVATE cxx_std_20)

target_compile_options(ool-bench PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3;-march=native>)
