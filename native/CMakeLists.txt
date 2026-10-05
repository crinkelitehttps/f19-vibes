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
  src/core/pcspeaker.cpp
  src/drivers/mgraphic.cpp
  src/hires/world_capture.cpp
  src/hires/scene.cpp
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

# Interactive host (SDL3), with OpenXR output (--vr) on X11/GLX.
find_package(Threads REQUIRED)
find_package(OpenXR REQUIRED CONFIG)
find_package(OpenGL REQUIRED COMPONENTS GLX)
find_package(X11 REQUIRED)
# Default gamepad bindings, built in from native/gamepad.cfg.
file(READ gamepad.cfg F19_GAMEPAD_CFG)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS gamepad.cfg)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/gamepad_cfg.h
     CONTENT "// Generated from native/gamepad.cfg.\n#pragma once\ninline constexpr const char* kDefaultGamepadCfg = R\"cfg(${F19_GAMEPAD_CFG})cfg\";\n")
add_executable(f19 src/host/f19.cpp src/host/headtrack.cpp src/host/xr.cpp src/host/keyboard.cpp src/host/gamepad.cpp src/host/manual.cpp src/host/font.cpp)
target_include_directories(f19 PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(f19 PRIVATE f19core f19hires ZLIB::ZLIB Threads::Threads OpenXR::openxr_loader OpenGL::GLX X11::X11)
# OpenXR structs are initialised as {XR_TYPE_...}, the rest zeroed.
# The game manual on a clipboard (optional: PDF rendering with poppler).
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(POPPLER_CPP IMPORTED_TARGET poppler-cpp)
endif()
if(POPPLER_CPP_FOUND)
  target_link_libraries(f19 PRIVATE PkgConfig::POPPLER_CPP)
  target_compile_definitions(f19 PRIVATE F19_HAVE_POPPLER)
else()
  message(STATUS "poppler-cpp not found: f19 is built without the in-game manual")
endif()
set_source_files_properties(src/host/xr.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
