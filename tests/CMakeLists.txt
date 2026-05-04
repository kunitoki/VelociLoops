include(FetchContent)

set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.2
)
FetchContent_MakeAvailable(doctest)

function(velociloops_declare_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN} test_main.cpp)
    target_link_libraries(${TARGET_NAME} PRIVATE velociloops_static doctest::doctest)
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    target_compile_definitions(${TARGET_NAME}
        PRIVATE
            VELOCILOOPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    )
    if (VELOCILOOPS_ENABLE_COVERAGE)
        target_compile_options(${TARGET_NAME} PRIVATE --coverage)
        target_link_options(${TARGET_NAME} PRIVATE --coverage)
    endif()
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
endfunction()

velociloops_declare_test(velociloops_tests test_velociloops.cpp)
velociloops_declare_test(velociloops_superflux test_superflux.cpp)
