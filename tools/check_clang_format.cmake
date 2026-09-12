# picopaste — clang-format gate (conventions §7, design §10 verification).
#
# Invariant: every hand-written product source under src/ and include/ is
# formatted with the repo-root .clang-format (Google base, 2-space indent,
# Attach braces, 120 columns, left-aligned pointers). The format file is the
# contract; without a mechanical gate the tree drifts and the next repair pass
# touches lines that had no business changing.
#
# Scope mirrors tools/check_posix_leak.cmake: the product sources under src/
# and include/, same extensions. tests/ and tools/ are deliberately out of
# scope — the product is what ships. third_party/ is vendored and never
# reformatted; it lives outside the two roots already, and the explicit
# exclusion below keeps that promise written down if a vendored drop ever
# moves inside a product root.
#
# clang-format is not part of the C++ toolchain on the CI runners, so a missing
# binary is not a failure: the gate SKIPs with a clear message and passes. Set
# PICOPASTE_CLANG_FORMAT (cache variable, e.g. -DPICOPASTE_CLANG_FORMAT=<path>)
# or the PICOPASTE_CLANG_FORMAT environment variable to point at a specific
# binary; otherwise PATH is searched.
#
# Usage:
#   cmake -DPICOPASTE_SOURCE_DIR=<repo root> -P tools/check_clang_format.cmake
#   cmake -DPICOPASTE_SOURCE_DIR=<repo root> \
#         -DPICOPASTE_CLANG_FORMAT=clang-format-18 -P tools/check_clang_format.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT PICOPASTE_SOURCE_DIR)
  get_filename_component(PICOPASTE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# ---------------------------------------------------------------------------
# Locate clang-format. An explicit cache/CLI value wins, then the environment,
# then PATH. Absent everywhere is a SKIP, not a failure: failing here would make
# the gate unlandable on runners that do not ship clang-format.
# ---------------------------------------------------------------------------
if(NOT PICOPASTE_CLANG_FORMAT AND DEFINED ENV{PICOPASTE_CLANG_FORMAT})
  set(PICOPASTE_CLANG_FORMAT "$ENV{PICOPASTE_CLANG_FORMAT}")
endif()
if(NOT PICOPASTE_CLANG_FORMAT)
  find_program(_picopaste_clang_format
    NAMES clang-format clang-format-20 clang-format-19 clang-format-18
          clang-format-17 clang-format-16 clang-format-15 clang-format-14)
  set(PICOPASTE_CLANG_FORMAT "${_picopaste_clang_format}")
endif()
if(NOT PICOPASTE_CLANG_FORMAT)
  message(STATUS
    "clang-format gate: SKIP (no clang-format found on PATH; "
    "set PICOPASTE_CLANG_FORMAT to enable)")
  return()
endif()

# ---------------------------------------------------------------------------
# Enumerate the hand-written product sources, exactly as check_posix_leak.cmake
# does. A file need not be formatted when clang-format cannot be found, but once
# it is found every product source must be.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _sources
  "${PICOPASTE_SOURCE_DIR}/src/*.c"    "${PICOPASTE_SOURCE_DIR}/src/*.cc"
  "${PICOPASTE_SOURCE_DIR}/src/*.cpp"  "${PICOPASTE_SOURCE_DIR}/src/*.h"
  "${PICOPASTE_SOURCE_DIR}/src/*.hpp"
  "${PICOPASTE_SOURCE_DIR}/include/*.h" "${PICOPASTE_SOURCE_DIR}/include/*.hpp")

set(_violations "")
set(_checked 0)

foreach(_file ${_sources})
  string(REPLACE "\\" "/" _fwd "${_file}")
  if(_fwd MATCHES "third_party/")
    continue()
  endif()
  math(EXPR _checked "${_checked} + 1")

  # --dry-run + --Werror: no file is modified, and a formatting difference is a
  # non-zero exit. The warning on stderr already names file:line:column, which
  # is the shape the other gates report.
  execute_process(
    COMMAND "${PICOPASTE_CLANG_FORMAT}" --dry-run --Werror "--style=file" "${_file}"
    WORKING_DIRECTORY "${PICOPASTE_SOURCE_DIR}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    # Report one file:line:column per file, the shape the other gates use.
    # clang-format prints "<file>:<line>:<col>: error: code should be
    # clang-formatted ..." on stderr; keep only that location. Semicolons are
    # stripped because CMake treats them as list separators and the diagnostic
    # may echo a line of source.
    set(_where "${_file}")
    string(FIND "${_err}" "\n" _nl)
    if(_nl GREATER 0)
      string(SUBSTRING "${_err}" 0 ${_nl} _head)
      string(REGEX REPLACE " (error|warning):.*$" "" _head "${_head}")
      string(STRIP "${_head}" _head)
      if(_head)
        set(_where "${_head}")
      endif()
    endif()
    string(REPLACE ";" "" _where "${_where}")
    list(APPEND _violations "${_where}")
  endif()
endforeach()

if(_violations)
  list(REMOVE_DUPLICATES _violations)
  list(LENGTH _violations _n)
  string(REPLACE ";" "\n    " _report "${_violations}")
  message(FATAL_ERROR
    "clang-format gate: ${_n} file(s) need formatting "
    "(run: clang-format -i --style=file <file>):\n    ${_report}")
endif()

message(STATUS
  "clang-format gate: PASS (${_checked} product files match .clang-format)")
