cmake_minimum_required(VERSION 3.20)

add_executable(velociloops main.cpp)
target_link_libraries(velociloops PUBLIC velociloops_static)
target_compile_features(velociloops PRIVATE cxx_std_17)
