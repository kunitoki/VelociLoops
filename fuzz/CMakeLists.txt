add_executable(velociloops_fuzzer fuzz_velociloops.cpp)

target_link_libraries(velociloops_fuzzer PRIVATE velociloops_static)

target_compile_features(velociloops_fuzzer PRIVATE cxx_std_17)

target_compile_definitions(velociloops_fuzzer
    PRIVATE VELOCILOOPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/data")

target_compile_options(velociloops_fuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

target_link_options(velociloops_fuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined)
