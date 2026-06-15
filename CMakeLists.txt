cmake_minimum_required(VERSION 3.14)
project(hierarchical_time_wheel CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /W4 /permissive-)
else()
    add_compile_options(-O2 -Wall -Wextra -pthread)
    add_link_options(-pthread)
endif()

add_library(time_wheel STATIC time_wheel.cpp time_wheel.h)
target_include_directories(time_wheel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(example example.cpp)
target_link_libraries(example PRIVATE time_wheel)

add_executable(stress_test stress_test.cpp)
target_link_libraries(stress_test PRIVATE time_wheel)
