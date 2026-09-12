# picopaste -- parallel Windows (MSVC) build driver.
#
# This file is a CMake script, not a batch/shell script. The product ships no
# scripts at all: tools/check_no_scripts.cmake rejects .bat/.cmd/.ps1/.vbs/.sh
# files under src/, include/, tests/ or tools/, and rejects string literals
# naming a script host in the sources and in the CMake build/test logic.
# Keeping dev tooling in the same non-shell form as the gate scripts avoids an
# exemption a reviewer has to reason about, and it runs identically from cmd,
# PowerShell and git bash.
#
# Behaviour: load the MSVC environment, configure only when the build directory
# is missing or out of date, build Release in parallel on all cores, optionally
# run ctest, print PASS/FAIL and exit non-zero on the first failing step.
#
# Usage:
#   cmake -DPICOPASTE_SOURCE_DIR=D:/workspace/picopaste -P tools/build_windows_parallel.cmake
#   cmake -DBUILD_DIR=D:/scratch/pp -DRUN_TESTS=OFF -P tools/build_windows_parallel.cmake
#
# Inputs (all optional):
#   BUILD_DIR              build/scratch tree; default <repo>/build-parallel
#   PICOPASTE_SOURCE_DIR   repo root; default the directory above this script
#   PICOPASTE_VCVARS       vcvars64.bat; default discovered below
#   PICOPASTE_NEWOSP_DIR   newosp checkout; default $USERPROFILE/newosp-windows
#   PICOPASTE_BUILD_TESTS  ON by default
#   PICOPASTE_WERROR       ON by default
#   PICOPASTE_CONFIG_FORMAT ini by default
#   RUN_TESTS              ON by default

cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
  message(FATAL_ERROR "build_windows_parallel.cmake drives the MSVC build and only runs on Windows")
endif()

if(NOT PICOPASTE_SOURCE_DIR)
  get_filename_component(PICOPASTE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
get_filename_component(_source "${PICOPASTE_SOURCE_DIR}" ABSOLUTE)

if(NOT BUILD_DIR)
  set(BUILD_DIR "${_source}/build-parallel")
endif()
get_filename_component(BUILD_DIR "${BUILD_DIR}" ABSOLUTE)

if(NOT DEFINED PICOPASTE_BUILD_TESTS)
  set(PICOPASTE_BUILD_TESTS ON)
endif()
if(NOT DEFINED PICOPASTE_WERROR)
  set(PICOPASTE_WERROR ON)
endif()
if(NOT DEFINED PICOPASTE_CONFIG_FORMAT)
  set(PICOPASTE_CONFIG_FORMAT "ini")
endif()
if(NOT DEFINED RUN_TESTS)
  set(RUN_TESTS ON)
endif()

if(NOT PICOPASTE_NEWOSP_DIR)
  if(DEFINED ENV{USERPROFILE})
    set(PICOPASTE_NEWOSP_DIR "$ENV{USERPROFILE}/newosp-windows")
  else()
    set(PICOPASTE_NEWOSP_DIR "${_source}/../newosp-windows")
  endif()
endif()
file(TO_CMAKE_PATH "${PICOPASTE_NEWOSP_DIR}" PICOPASTE_NEWOSP_DIR)

# --- Locate the MSVC environment -------------------------------------------
# An explicit -DPICOPASTE_VCVARS wins, then the environment, then the known
# Build Tools location on this machine, then vswhere for a stock install.
set(_vcvars "")
if(DEFINED PICOPASTE_VCVARS AND PICOPASTE_VCVARS)
  set(_vcvars "${PICOPASTE_VCVARS}")
elseif(DEFINED ENV{PICOPASTE_VCVARS})
  set(_vcvars "$ENV{PICOPASTE_VCVARS}")
elseif(EXISTS "D:/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
  set(_vcvars "D:/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
elseif(EXISTS "$ENV{ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe")
  execute_process(
    COMMAND "$ENV{ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
            -latest -products * -find VC/Auxiliary/Build/vcvars64.bat
    OUTPUT_VARIABLE _vswhere_out
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _vswhere_rc)
  if(_vswhere_rc EQUAL 0 AND _vswhere_out)
    string(REPLACE "\n" ";" _vswhere_lines "${_vswhere_out}")
    foreach(_candidate ${_vswhere_lines})
      string(STRIP "${_candidate}" _candidate)
      if(EXISTS "${_candidate}")
        set(_vcvars "${_candidate}")
        break()
      endif()
    endforeach()
  endif()
endif()

if(NOT _vcvars OR NOT EXISTS "${_vcvars}")
  message(FATAL_ERROR
    "vcvars64.bat not found. Pass -DPICOPASTE_VCVARS=<path> or set PICOPASTE_VCVARS.")
endif()
file(TO_CMAKE_PATH "${_vcvars}" _vcvars)
message(STATUS "MSVC environment: ${_vcvars}")

if(NOT EXISTS "${_source}/CMakeLists.txt")
  message(FATAL_ERROR "no CMakeLists.txt under PICOPASTE_SOURCE_DIR='${_source}'")
endif()
if(NOT EXISTS "${PICOPASTE_NEWOSP_DIR}/include/osp/platform.hpp")
  message(FATAL_ERROR
    "newosp not found at '${PICOPASTE_NEWOSP_DIR}'. Pass "
    "-DPICOPASTE_NEWOSP_DIR=<newosp checkout on the windows branch>.")
endif()

set(_cmake "${CMAKE_COMMAND}")
if(NOT _cmake)
  set(_cmake "cmake")
endif()

# Run a command with the MSVC environment loaded. vcvars is sourced per step:
# it is cheap, it keeps cl.exe/link.exe and the SDK on PATH regardless of how
# CMake locates MSBuild, and it is what any future non-VS generator needs.
if(DEFINED ENV{COMSPEC})
  set(_cmd "$ENV{COMSPEC}")
else()
  set(_cmd "cmd")
endif()

function(run_with_msvc step)
  execute_process(
    COMMAND "${_cmd}" /c call "${_vcvars}" && ${ARGN}
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(STATUS "FAIL: ${step} (exit ${_rc})")
    message(FATAL_ERROR "picopaste Windows build failed at step: ${step}")
  endif()
  message(STATUS "ok: ${step}")
endfunction()

# Read one cache entry without trusting its type; empty if the key is absent.
function(read_cache_value cache key out)
  set(${out} "" PARENT_SCOPE)
  file(STRINGS "${cache}" _lines REGEX "^${key}:")
  if(_lines)
    list(GET _lines 0 _first)
    string(REGEX REPLACE "^[^=]*=" "" _value "${_first}")
    string(STRIP "${_value}" _value)
    set(${out} "${_value}" PARENT_SCOPE)
  endif()
endfunction()

# --- Decide whether to configure -------------------------------------------
# The VS generator re-runs CMake itself when a CMakeLists.txt changes, so this
# check is not the only safety net; it exists so a stale cache with different
# options or a different generator is caught before the build, where the error
# would otherwise be an incomprehensible link/toolset failure.
set(_cache "${BUILD_DIR}/CMakeCache.txt")
set(_need_configure TRUE)
set(_why "no configuration in ${BUILD_DIR}")

if(EXISTS "${_cache}")
  set(_need_configure FALSE)
  read_cache_value("${_cache}" CMAKE_HOME_DIRECTORY _cached_source)
  read_cache_value("${_cache}" CMAKE_GENERATOR _cached_generator)
  read_cache_value("${_cache}" CMAKE_GENERATOR_PLATFORM _cached_platform)
  read_cache_value("${_cache}" PICOPASTE_BUILD_TESTS _cached_tests)
  read_cache_value("${_cache}" PICOPASTE_WERROR _cached_werror)
  read_cache_value("${_cache}" PICOPASTE_NEWOSP_DIR _cached_newosp)
  read_cache_value("${_cache}" PICOPASTE_CONFIG_FORMAT _cached_format)
  file(TO_CMAKE_PATH "${_cached_source}" _cached_source)
  file(TO_CMAKE_PATH "${_cached_newosp}" _cached_newosp)
  string(TOUPPER "${_cached_tests}" _cached_tests)
  string(TOUPPER "${_cached_werror}" _cached_werror)
  string(TOUPPER "${PICOPASTE_BUILD_TESTS}" _want_tests)
  string(TOUPPER "${PICOPASTE_WERROR}" _want_werror)

  if(NOT _cached_source STREQUAL "${_source}")
    set(_need_configure TRUE)
    set(_why "source directory changed: ${_cached_source} -> ${_source}")
  elseif(NOT _cached_generator STREQUAL "Visual Studio 17 2022")
    set(_need_configure TRUE)
    set(_why "generator changed: ${_cached_generator} -> Visual Studio 17 2022")
  elseif(NOT _cached_platform STREQUAL "x64")
    set(_need_configure TRUE)
    set(_why "platform changed: '${_cached_platform}' -> x64")
  elseif(NOT _cached_tests STREQUAL "${_want_tests}")
    set(_need_configure TRUE)
    set(_why "PICOPASTE_BUILD_TESTS changed: ${_cached_tests} -> ${_want_tests}")
  elseif(NOT _cached_werror STREQUAL "${_want_werror}")
    set(_need_configure TRUE)
    set(_why "PICOPASTE_WERROR changed: ${_cached_werror} -> ${_want_werror}")
  elseif(NOT _cached_newosp STREQUAL "${PICOPASTE_NEWOSP_DIR}")
    set(_need_configure TRUE)
    set(_why "PICOPASTE_NEWOSP_DIR changed: ${_cached_newosp} -> ${PICOPASTE_NEWOSP_DIR}")
  elseif(NOT _cached_format STREQUAL "${PICOPASTE_CONFIG_FORMAT}")
    set(_need_configure TRUE)
    set(_why "PICOPASTE_CONFIG_FORMAT changed: ${_cached_format} -> ${PICOPASTE_CONFIG_FORMAT}")
  else()
    # Only the project's own configure inputs are tracked. A recursive glob
    # would also walk nested build trees (build/, _deps/) and reconfigure on
    # every run because their generated files are newer than this cache.
    set(_inputs "${_source}/CMakeLists.txt" "${_source}/tests/CMakeLists.txt")
    file(GLOB_RECURSE _src_lists LIST_DIRECTORIES FALSE "${_source}/src/CMakeLists.txt")
    list(APPEND _inputs ${_src_lists})
    foreach(_input ${_inputs})
      if(EXISTS "${_input}" AND "${_input}" IS_NEWER_THAN "${_cache}")
        set(_need_configure TRUE)
        set(_why "configure input changed: ${_input}")
        break()
      endif()
    endforeach()
  endif()
endif()

if(_need_configure)
  message(STATUS "configure: needed (${_why})")
  run_with_msvc("configure"
    "${_cmake}" -S "${_source}" -B "${BUILD_DIR}"
    -G "Visual Studio 17 2022" -A x64
    "-DPICOPASTE_BUILD_TESTS=${PICOPASTE_BUILD_TESTS}"
    "-DPICOPASTE_WERROR=${PICOPASTE_WERROR}"
    "-DPICOPASTE_NEWOSP_DIR=${PICOPASTE_NEWOSP_DIR}"
    "-DPICOPASTE_CONFIG_FORMAT=${PICOPASTE_CONFIG_FORMAT}")
else()
  message(STATUS "configure: up to date, skipping")
endif()

message(STATUS "build: Release, parallel (all cores)")
run_with_msvc("build" "${_cmake}" --build "${BUILD_DIR}" --config Release --parallel)

if(RUN_TESTS)
  set(_ctest "${CMAKE_CTEST_COMMAND}")
  if(NOT _ctest OR NOT EXISTS "${_ctest}")
    get_filename_component(_cmake_dir "${_cmake}" DIRECTORY)
    if(EXISTS "${_cmake_dir}/ctest.exe")
      set(_ctest "${_cmake_dir}/ctest.exe")
    else()
      set(_ctest "ctest")
    endif()
  endif()
  message(STATUS "ctest: Release")
  run_with_msvc("ctest" "${_ctest}" --test-dir "${BUILD_DIR}" -C Release --output-on-failure)
endif()

if(RUN_TESTS)
  message(STATUS "PASS: Release build and ctest succeeded in ${BUILD_DIR}")
else()
  message(STATUS "PASS: Release build succeeded in ${BUILD_DIR} (tests skipped)")
endif()
