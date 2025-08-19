cmake_minimum_required(VERSION 3.20)

project(VelociLoops)

add_library(velociloops_library STATIC src/velociloops.cpp)
target_include_directories(velociloops_library PUBLIC include)
target_compile_features(velociloops_library PRIVATE cxx_std_17)

if (PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if (BUILD_TESTING)
        add_subdirectory(tests)
    endif()

    add_subdirectory(demo)
endif()
