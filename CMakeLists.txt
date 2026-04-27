cmake_minimum_required(VERSION 3.20)

project(VelociLoops)

find_program(LCOV_EXECUTABLE lcov)
find_program(GENHTML_EXECUTABLE genhtml)

include(CMakeDependentOption)

add_library(velociloops_library STATIC src/velociloops.cpp)
target_include_directories(velociloops_library PUBLIC include)
target_compile_features(velociloops_library PRIVATE cxx_std_17)

if (PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if (BUILD_TESTING)
        cmake_dependent_option(VELOCILOOPS_COVERAGE "Enable coverage" OFF
            "BUILD_TESTING;LCOV_EXECUTABLE;GENHTML_EXECUTABLE" OFF)

        if (VELOCILOOPS_COVERAGE)
            target_compile_options(velociloops_library PRIVATE --coverage)
            target_link_options(velociloops_library INTERFACE --coverage)
        endif()

        add_subdirectory(tests)
    endif()

    add_subdirectory(demo)
endif()
