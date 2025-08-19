include(FetchContent)

set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.2
)
FetchContent_MakeAvailable(doctest)

add_executable(velociloops_tests test_velociloops.cpp)
target_link_libraries(velociloops_tests PRIVATE velociloops_static doctest::doctest)
target_compile_features(velociloops_tests PRIVATE cxx_std_17)
target_compile_definitions(velociloops_tests
    PRIVATE
        VELOCILOOPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

add_test(NAME velociloops_tests COMMAND velociloops_tests)

if (VELOCILOOPS_COVERAGE)
    target_compile_options(velociloops_tests PRIVATE --coverage)
    target_link_options(velociloops_tests PRIVATE --coverage)
endif()

if (VELOCILOOPS_SANITIZERS)
    target_compile_options(velociloops_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(velociloops_tests PRIVATE
        -fsanitize=address,undefined)
endif()

if (VELOCILOOPS_ENABLE_FUZZING)
    add_executable(velociloops_fuzzer fuzz_velociloops.cpp)
    target_link_libraries(velociloops_fuzzer PRIVATE velociloops_static)
    target_compile_features(velociloops_fuzzer PRIVATE cxx_std_17)
    target_compile_definitions(velociloops_fuzzer
        PRIVATE VELOCILOOPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    target_compile_options(velociloops_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(velociloops_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()
