cmake_minimum_required(VERSION 3.16)
project(emu C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -D_GNU_SOURCE)

file(GLOB EMU_SOURCES src/*.c)
add_library(emu STATIC ${EMU_SOURCES})
target_include_directories(emu PUBLIC include)
target_link_libraries(emu pthread)

# Tests
file(GLOB TEST_SOURCES tests/*.c)
add_executable(test_runner ${TEST_SOURCES})
target_link_libraries(test_runner emu)
enable_testing()
add_test(NAME emu_tests COMMAND test_runner)
