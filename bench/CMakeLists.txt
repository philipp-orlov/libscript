add_executable(bench_longrun bench_longrun.cpp)
target_link_libraries(bench_longrun PRIVATE script)
target_include_directories(bench_longrun PRIVATE ${SCRIPT_LIBCORE_DIR}/libbench/include)
script_set_warnings(bench_longrun)

add_executable(bench_soak_mixed soak_mixed.cpp)
target_link_libraries(bench_soak_mixed PRIVATE script core::bench)
target_include_directories(bench_soak_mixed PRIVATE ${SCRIPT_LIBCORE_DIR}/libbench/include)
script_set_warnings(bench_soak_mixed)
