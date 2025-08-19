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
target_link_libraries(velociloops_tests PRIVATE velociloops_library doctest::doctest)
target_compile_features(velociloops_tests PRIVATE cxx_std_17)
target_compile_definitions(velociloops_tests
    PRIVATE
        VELOCILOOPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

add_test(NAME velociloops_tests COMMAND velociloops_tests)
