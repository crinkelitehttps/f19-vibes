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
  src/drivers/mgraphic.cpp
  src/hires/world_capture.cpp
)
target_include_directories(f19core PUBLIC src)

# CPU conformance tests (SingleStepTests 8088 vectors, see tools/sst_fetch.py).
add_executable(sst_runner tests/sst_runner.cpp)
target_link_libraries(sst_runner PRIVATE f19core)


find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)

# OpenGL 3.3 renderer (world at window resolution, MSAA).
add_library(f19hires STATIC src/hires/gl.cpp src/hires/gl_render.cpp)
target_link_libraries(f19hires PUBLIC f19core SDL3::SDL3)

# Headless runner: boot the game, trace services, dump screenshots.
add_executable(f19trace src/host/f19trace.cpp)
target_link_libraries(f19trace PRIVATE f19core f19hires)

# Interactive host (SDL3).
add_executable(f19 src/host/f19.cpp src/host/headtrack.cpp)
find_package(Threads REQUIRED)
target_link_libraries(f19 PRIVATE f19core f19hires ZLIB::ZLIB Threads::Threads)
