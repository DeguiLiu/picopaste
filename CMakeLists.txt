cmake_minimum_required(VERSION 3.20)
project(picopaste LANGUAGES CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
# Standards & build posture
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PICOPASTE_BUILD_TESTS "Build the host test suite" ON)
option(PICOPASTE_WERROR "Treat warnings as errors" OFF)
# newosp config backend. INI ships vendored in newosp (include/osp/inicpp.hpp),
# so it configures with no network fetch; TOML pulls toml++ and is opt-in.
set(PICOPASTE_CONFIG_FORMAT "ini" CACHE STRING "newosp config backend: ini|toml")

# ---------------------------------------------------------------------------
# newosp — required, not optional.
#
# Points at the `windows` worktree while the MSVC platform layer is developed
# there; switch to the main checkout once that branch merges.
# ---------------------------------------------------------------------------
set(PICOPASTE_NEWOSP_DIR "$ENV{HOME}/newosp-windows" CACHE PATH "Path to the newosp checkout")

if(NOT EXISTS "${PICOPASTE_NEWOSP_DIR}/include/osp/platform.hpp")
  message(FATAL_ERROR
    "newosp not found at PICOPASTE_NEWOSP_DIR='${PICOPASTE_NEWOSP_DIR}'.\n"
    "Point it at a newosp checkout: -DPICOPASTE_NEWOSP_DIR=/path/to/newosp")
endif()

set(OSP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(OSP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OSP_WITH_NETWORK   OFF CACHE BOOL "" FORCE)
set(OSP_CONFIG_INI     OFF CACHE BOOL "" FORCE)
set(OSP_CONFIG_JSON    OFF CACHE BOOL "" FORCE)
set(OSP_CONFIG_YAML    OFF CACHE BOOL "" FORCE)
set(OSP_CONFIG_TOML    OFF CACHE BOOL "" FORCE)

if(PICOPASTE_CONFIG_FORMAT STREQUAL "toml")
  set(OSP_CONFIG_TOML ON CACHE BOOL "" FORCE)
else()
  set(OSP_CONFIG_INI ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${PICOPASTE_NEWOSP_DIR}" "${CMAKE_BINARY_DIR}/newosp" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Compiler posture, applied per-target via picopaste_apply_flags().
# ---------------------------------------------------------------------------
function(picopaste_apply_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    # Static CRT: the shipped exe must run without a redistributable install.
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    if(PICOPASTE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(PICOPASTE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core — platform-neutral. Each subdirectory appends its own sources, so
# parallel workstreams never touch a shared file.
# ---------------------------------------------------------------------------
add_library(picopaste_core STATIC)
target_include_directories(picopaste_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(picopaste_core PUBLIC osp)
picopaste_apply_flags(picopaste_core)

add_subdirectory(src/core/sftp)
add_subdirectory(src/core/log)
add_subdirectory(src/core/app)

# Host-side byte streams (pipes to a real `ssh -s <host> sftp`), used by the
# test suite on Linux as well as by the Windows client's own local testing.
add_subdirectory(src/platform/posix)

# ---------------------------------------------------------------------------
# Windows client. Built only on Windows: the host build compiles the core and
# its tests so the protocol layer stays verifiable without a Windows machine.
# ---------------------------------------------------------------------------
if(WIN32)
  add_executable(picopaste WIN32)
  target_link_libraries(picopaste PRIVATE picopaste_core
    user32 shell32 ole32 windowscodecs shlwapi advapi32)
  picopaste_apply_flags(picopaste)
  add_subdirectory(src/platform/win32)
endif()

if(PICOPASTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
