cmake_minimum_required(VERSION 3.16)
project(VirtualFileSystem LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vfs_core
    src/vfs.cpp
    src/algorithms.cpp
    src/worker_pool.cpp
)
target_include_directories(vfs_core PUBLIC include)
target_link_libraries(vfs_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(vfs_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(vfs_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
add_executable(vfs src/main.cpp)
target_link_libraries(vfs PRIVATE vfs_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(vfs_tests tests/test_vfs.cpp)
    target_link_libraries(vfs_tests PRIVATE vfs_core)
    add_test(NAME vfs_tests COMMAND vfs_tests)
endif()
