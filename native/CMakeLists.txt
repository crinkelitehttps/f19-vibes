cmake_minimum_required(VERSION 3.25)
project(f19native C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /wd4244 /wd4267 /wd4305)
  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MINGW)
  # One self-contained f19.exe: no MinGW runtime DLLs beside it.
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Dependencies: the system's (find_package), or with F19_FETCH_DEPS
# (default on Windows) downloaded and linked statically.
option(F19_FETCH_DEPS "Download and build SDL3, the OpenXR loader and zlib" ${WIN32})
if(F19_FETCH_DEPS)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)  # OpenXR loader, static
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
  set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3 URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.tar.gz
                       FIND_PACKAGE_ARGS CONFIG)
  FetchContent_Declare(OpenXR URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.60.tar.gz
                       FIND_PACKAGE_ARGS CONFIG)
  FetchContent_Declare(zlib URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz)
  FetchContent_MakeAvailable(SDL3 OpenXR zlib)
  if(NOT TARGET OpenXR::openxr_loader)
    add_library(OpenXR::openxr_loader ALIAS openxr_loader)
  endif()
  if(TARGET zlibstatic)
    set(F19_ZLIB zlibstatic)
  else()
    set(F19_ZLIB ZLIB::ZLIBSTATIC)
  endif()
else()
  find_package(SDL3 REQUIRED CONFIG)
  find_package(OpenXR REQUIRED CONFIG)
  find_package(ZLIB REQUIRED)
  set(F19_ZLIB ZLIB::ZLIB)
endif()

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
# Khronos GL headers (glcorearb.h is not shipped on Windows).
target_include_directories(f19core PUBLIC src third_party/include)

# CPU conformance tests (SingleStepTests 8088 vectors, see tools/sst_fetch.py).
add_executable(sst_runner tests/sst_runner.cpp)
target_link_libraries(sst_runner PRIVATE f19core)


# OpenGL 3.3 renderer (world at window resolution, MSAA).
add_library(f19hires STATIC src/hires/gl.cpp src/hires/gl_render.cpp)
target_link_libraries(f19hires PUBLIC f19core SDL3::SDL3)

# Headless runner: boot the game, trace services, dump screenshots.
add_executable(f19trace src/host/f19trace.cpp)
target_link_libraries(f19trace PRIVATE f19core f19hires)

# Interactive host (SDL3), with OpenXR output (--vr): GLX on Linux,
# Direct3D 11 (shared with GL) on Windows.
find_package(Threads REQUIRED)
# Default gamepad bindings, built in from native/gamepad.cfg.
file(READ gamepad.cfg F19_GAMEPAD_CFG)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS gamepad.cfg)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/gamepad_cfg.h
     CONTENT "// Generated from native/gamepad.cfg.\n#pragma once\ninline constexpr const char* kDefaultGamepadCfg = R\"cfg(${F19_GAMEPAD_CFG})cfg\";\n")
add_executable(f19 src/host/f19.cpp src/host/headtrack.cpp src/host/xr.cpp src/host/keyboard.cpp src/host/gamepad.cpp src/host/manual.cpp src/host/font.cpp)
target_include_directories(f19 PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The console font, built in.
file(READ data/default8x16.psfu.gz F19_FONT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," F19_FONT_BYTES "${F19_FONT_HEX}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS data/default8x16.psfu.gz)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_font.h
     CONTENT "// Generated from native/data/default8x16.psfu.gz.\n#pragma once\n#include <cstdint>\ninline constexpr uint8_t kBuiltinFont[] = {${F19_FONT_BYTES}};\n")
target_link_libraries(f19 PRIVATE f19core f19hires ${F19_ZLIB} Threads::Threads OpenXR::openxr_loader)
if(WIN32)
  target_link_libraries(f19 PRIVATE d3d11 dxgi opengl32 ws2_32)
else()
  find_package(OpenGL REQUIRED COMPONENTS GLX)
  find_package(X11 REQUIRED)
  target_link_libraries(f19 PRIVATE OpenGL::GLX X11::X11)
endif()
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
if(NOT MSVC)
  set_source_files_properties(src/host/xr.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
endif()
