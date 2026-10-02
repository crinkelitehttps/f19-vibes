cmake_minimum_required(VERSION 3.25)
project(f19native CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Core: x86-16 interpreter and memory. No platform dependencies.
add_library(f19core STATIC
  src/core/cpu.cpp
  src/core/machine.cpp
  src/core/dos.cpp
)
target_include_directories(f19core PUBLIC src)

# CPU conformance tests (SingleStepTests 8088 vectors, see tools/sst_fetch.py).
add_executable(sst_runner tests/sst_runner.cpp)
target_link_libraries(sst_runner PRIVATE f19core)

# Headless runner: boot the game, trace services, dump screenshots.
add_executable(f19trace src/host/f19trace.cpp)
target_link_libraries(f19trace PRIVATE f19core)
