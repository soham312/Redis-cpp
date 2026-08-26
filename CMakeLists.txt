cmake_minimum_required(VERSION 3.16)
project(goredis_cpp CXX)

# C++17 for std::shared_mutex (readers-writer lock), std::optional, and
# structured bindings. Nothing here needs anything newer.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emitted so editors/clangd can find the real compile flags (include paths,
# -std, etc.) instead of guessing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(src)

# Google Test is wired up (via CMake FetchContent) starting in the testing
# stage, once there's actual code to test — see tests/CMakeLists.txt once
# that stage adds it. Nothing pulls in an external dependency before
# there's a reason to.
