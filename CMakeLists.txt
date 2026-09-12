cmake_minimum_required(VERSION 3.20)
project(picopaste LANGUAGES CXX VERSION 0.1.0)

# The executable carries its own icons — the tray's three states and, through the
# lowest-numbered ICON resource, the .exe icon itself. `LANGUAGES CXX` alone
# leaves .rc files with no build rule, so the resource compiler is enabled
# explicitly, and only on Windows: the Linux build has none.
if(WIN32)
  enable_language(RC)
endif()

# ---------------------------------------------------------------------------
# Standards & build posture
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# One CRT for every target in the build, including FetchContent'd dependencies.
# Setting MSVC_RUNTIME_LIBRARY per target is not enough: it leaves third-party
# targets (Catch2) on the default /MD while ours are /MT, and mixing the two in
# one link produces LNK2005 on STL internals -- basic_ios, basic_streambuf,
# locale::facet -- reported as "already defined in <some other test>.obj", which
# points at the symptom rather than the cause. Static CRT stays the goal: the
# shipped exe must run without a redistributable install.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
endif()

option(PICOPASTE_BUILD_TESTS "Build the host test suite" ON)
option(PICOPASTE_WERROR "Treat warnings as errors" OFF)
# newosp config backend. INI ships vendored in newosp (include/osp/inicpp.hpp),
# so it configures with no network fetch; TOML pulls toml++ and is opt-in.
set(PICOPASTE_CONFIG_FORMAT "ini" CACHE STRING "newosp config backend: ini|toml")

# ---------------------------------------------------------------------------
# newosp -- required, not optional, and PINNED TO A COMMIT ON A BRANCH.
#
# The Windows build depends on newosp's `windows` branch, NOT on `main`. On
# main, osp/platform.hpp reads windows.h's RT_VERSION (a resource-type constant)
# as the RT-Thread marker and then includes a non-existent <rtthread.h>, so any
# Windows translation unit that pulls windows.h first fails to compile. That fix
# lives on `windows` and is not yet merged.
#
# CI pins PICOPASTE_NEWOSP_REF to a commit SHA on that branch, not the branch
# name, so a force-push or merge-and-delete cannot break the build unnoticed.
# The branch/pin is recorded in two places and they must agree:
#   - .github/workflows/ci.yml  (PICOPASTE_NEWOSP_REPO / PICOPASTE_NEWOSP_REF)
#   - docs/build/windows-compile-test.md (step 1)
# Drop the pin once `windows` merges into main.
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
# Vendored third-party headers are not ours to fix, and their warnings must
# never be promoted to errors by our -Werror posture.
#
# A plain SYSTEM include directory is not sufficient here: hook_install.cpp
# includes picojson by relative path ("../../../third_party/picojson/..."), and
# every compiler classifies a header by *how it was found*. A relative include
# is found through the current-file search, so it stays a normal header and its
# -Wmaybe-uninitialized (GCC -O2+) survives -isystem. That is why the previous
# per-target approach would not have worked and why the single-site
# `#pragma GCC diagnostic` existed (and did nothing for MSVC, which ignores it).
#
# Instead a generated shim is force-included into the whole target. It pulls
# the vendored headers through <> (the system/external search path) before the
# product source reaches them; the include guard then makes the later relative
# include a no-op, and the header is system/external for the entire TU. This
# covers every current and future TU in the target on both toolchains, rather
# than only the one file that happens to carry a pragma today.
#
# The `#pragma GCC diagnostic` in src/core/app/hook_install.cpp is now redundant
# (the header is a system header before that include is reached) but is left in
# place: removing it is the source owner's call, and it is harmless.
# ---------------------------------------------------------------------------
set(PICOPASTE_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(PICOPASTE_VENDOR_SHIM "${CMAKE_CURRENT_BINARY_DIR}/picopaste_vendor_system.hpp")
file(WRITE "${PICOPASTE_VENDOR_SHIM}"
  "#pragma once\n#include <picojson/picojson.h>\n")

function(picopaste_apply_vendor_includes target)
  if(MSVC)
    # /external:I declares the tree external; /external:W0 keeps its warnings
    # out of /WX. /FI is the MSVC equivalent of -include.
    target_compile_options(${target} PRIVATE
      /external:W0
      "/external:I${PICOPASTE_VENDOR_DIR}"
      "/FI${PICOPASTE_VENDOR_SHIM}")
  else()
    # SYSTEM makes -isystem; the shim's <picojson/...> resolves there and is
    # therefore exempt from -Werror.
    target_include_directories(${target} SYSTEM PRIVATE "${PICOPASTE_VENDOR_DIR}")
    target_compile_options(${target} PRIVATE -include "${PICOPASTE_VENDOR_SHIM}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# MSVC marks the portable CRT calls fopen/_open as deprecated (C4996), and
# /WX promotes that warning to a hard error. Both our POSIX and Windows builds
# must keep using fopen/_open (the *_s variants are MSVC-only and would break
# portability), so the fix is the one the compiler names: suppress the
# deprecation warning. _CRT_SECURE_NO_WARNINGS is defined only for MSVC --
# GCC/Clang ignore it -- and only on the interface of our targets. newosp's
# config.hpp fopen uses are compiled inside our translation units, so the same
# definition covers them without touching newosp.
# ---------------------------------------------------------------------------
add_library(picopaste_msvc_crt INTERFACE)
if(MSVC)
  target_compile_definitions(picopaste_msvc_crt INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Core — platform-neutral. Each subdirectory appends its own sources, so
# parallel workstreams never touch a shared file.
# ---------------------------------------------------------------------------
add_library(picopaste_core STATIC)
target_include_directories(picopaste_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(picopaste_core PUBLIC osp PRIVATE picopaste_msvc_crt)
picopaste_apply_flags(picopaste_core)
picopaste_apply_vendor_includes(picopaste_core)

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
  target_link_libraries(picopaste PRIVATE picopaste_core picopaste_msvc_crt
    user32 shell32 ole32 windowscodecs shlwapi advapi32)
  picopaste_apply_flags(picopaste)
  picopaste_apply_vendor_includes(picopaste)
  add_subdirectory(src/platform/win32)
endif()

if(PICOPASTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
