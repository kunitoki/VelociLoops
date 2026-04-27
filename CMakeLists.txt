cmake_minimum_required(VERSION 3.20)

project(VelociLoops)

find_program(LCOV_EXECUTABLE lcov)
find_program(GENHTML_EXECUTABLE genhtml)

include(CMakeDependentOption)

option(VELOCILOOPS_ENABLE_STATIC "Build static library" ON)
option(VELOCILOOPS_ENABLE_SHARED "Build shared library" ON)
option(VELOCILOOPS_ENABLE_TESTING "Build tests" ON)
option(VELOCILOOPS_ENABLE_DEMO "Build demo" ON)

if (VELOCILOOPS_ENABLE_STATIC)
    add_library(velociloops_static STATIC src/velociloops.cpp)
    target_include_directories(velociloops_static PUBLIC include)
    target_compile_features(velociloops_static PRIVATE cxx_std_17)
endif()

if (VELOCILOOPS_ENABLE_SHARED)
    add_library(velociloops_shared SHARED src/velociloops.cpp)
    target_include_directories(velociloops_shared PUBLIC include)
    target_compile_features(velociloops_shared PRIVATE cxx_std_17)
endif()

if (PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if (VELOCILOOPS_ENABLE_TESTING)
        cmake_dependent_option(VELOCILOOPS_COVERAGE "Enable coverage" OFF
            "VELOCILOOPS_ENABLE_TESTING;LCOV_EXECUTABLE;GENHTML_EXECUTABLE" OFF)

        if (VELOCILOOPS_COVERAGE)
            target_compile_options(velociloops_static PRIVATE --coverage)
            target_link_options(velociloops_static INTERFACE --coverage)
        endif()

        cmake_dependent_option(VELOCILOOPS_SANITIZERS
            "Enable AddressSanitizer + UBSan (requires Clang or GCC)" OFF
            "VELOCILOOPS_ENABLE_TESTING" OFF)

        if (VELOCILOOPS_SANITIZERS)
            target_compile_options(velociloops_static PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer -g)
            target_link_options(velociloops_static INTERFACE
                -fsanitize=address,undefined)
        endif()

        cmake_dependent_option(VELOCILOOPS_ENABLE_FUZZING
            "Build libFuzzer target (requires Clang with -fsanitize=fuzzer)" OFF
            "VELOCILOOPS_ENABLE_TESTING" OFF)

        add_subdirectory(tests)
    endif()

    if (VELOCILOOPS_ENABLE_DEMO)
        add_subdirectory(demo)
    endif()
endif()
