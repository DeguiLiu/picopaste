# picopaste — no-scripts gate (design requirement 2, "彻底去脚本").
#
# The product must ship no scripts and spawn none. The Go implementation this
# replaces shipped toast.vbs + toast.ps1 plus a VBScript supervisor loop and
# shelled out to reg.exe / taskkill; eliminating that is the reason this
# project exists. Supervision, restart and log rotation all live in the
# process, so nothing under src/ or include/ may reintroduce a script.
#
# Fails on either of:
#   1. a script file (.ps1/.vbs/.cmd/.bat/.sh) anywhere under src/ or include/;
#   2. a *string literal* in a product source naming a script host or a shell
#      command interpreter (wscript, cscript, powershell, pwsh, cmd /c, reg.exe,
#      taskkill, ...).
#
# The forbidden-tool check is a denylist and is therefore only as complete as
# the list below; unlike the POSIX gate there is no allowlist that expresses
# "spawn only ssh", so the list is kept broad and reviewed when a new host is
# added. Matching is done on *logical* lines (backslash continuations joined),
# so a literal split as "power\<newline>shell" no longer slips through, and
# only inside a quoted run, so an unquoted mention in a comment that explains
# why we do NOT use the tool stays legal.
#
# Scope is the product only. Dev-time CI tooling is not scanned — this check
# is itself a CMake script, not a shell script.
#
# Usage:
#   cmake -DPICOPASTE_SOURCE_DIR=<repo root> -P tools/check_no_scripts.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT PICOPASTE_SOURCE_DIR)
  get_filename_component(PICOPASTE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_roots "${PICOPASTE_SOURCE_DIR}/src" "${PICOPASTE_SOURCE_DIR}/include")
set(_violations "")

# --- 1. Script files, by extension, anywhere under the product roots --------
foreach(_root ${_roots})
  if(EXISTS "${_root}")
    foreach(_suffix ps1 vbs cmd bat sh)
      file(GLOB_RECURSE _found "${_root}/*.${_suffix}")
      foreach(_f ${_found})
        list(APPEND _violations "script file: ${_f}")
      endforeach()
    endforeach()
  endif()
endforeach()

# --- 2. String literals naming a forbidden host tool -----------------------
# Each entry is a regex matched only inside one quoted run; when it matches, the
# display name is reported.
set(_forbidden_regex
  "wscript"
  "cscript"
  "powershell"
  "pwsh"
  "mshta"
  "rundll32"
  "wmic"
  "schtasks"
  "taskkill"
  "command\\.com"
  "reg\\.exe"
  "reg[ \t]+(add|delete|query|import|export)"
  "cmd(\\.exe)?[ \t]*/[ck]"
)
set(_forbidden_name
  "wscript" "cscript" "powershell" "pwsh" "mshta" "rundll32" "wmic" "schtasks"
  "taskkill" "command.com" "reg.exe" "reg add/delete/query" "cmd /c|/k"
)
list(LENGTH _forbidden_regex _forbidden_count)
math(EXPR _forbidden_last "${_forbidden_count} - 1")

file(GLOB_RECURSE _sources
  "${PICOPASTE_SOURCE_DIR}/src/*.c"    "${PICOPASTE_SOURCE_DIR}/src/*.cc"
  "${PICOPASTE_SOURCE_DIR}/src/*.cpp"  "${PICOPASTE_SOURCE_DIR}/src/*.h"
  "${PICOPASTE_SOURCE_DIR}/src/*.hpp"
  "${PICOPASTE_SOURCE_DIR}/include/*.h" "${PICOPASTE_SOURCE_DIR}/include/*.hpp")

foreach(_f ${_sources})
  file(READ "${_f}" _content)
  string(REPLACE "\r" "" _content "${_content}")
  # Join backslash-newline continuations so a spliced literal is seen whole.
  string(REGEX REPLACE "\\\\\n" "" _content "${_content}")
  # Protect semicolons (CMake list separator) before splitting into lines.
  string(REPLACE ";" "@SEMI@" _content "${_content}")
  string(REPLACE "\n" ";" _lines "${_content}")

  foreach(_line ${_lines})
    string(TOLOWER "${_line}" _low)
    foreach(_i RANGE 0 ${_forbidden_last})
      list(GET _forbidden_regex ${_i} _token)
      list(GET _forbidden_name ${_i} _name)
      if(_low MATCHES "\"[^\"\n]*${_token}[^\"\n]*\"")
        list(APPEND _violations "string literal '${_name}': ${_f}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(_violations)
  list(REMOVE_DUPLICATES _violations)
  string(REPLACE ";" "\n    " _report "${_violations}")
  message(FATAL_ERROR
    "no-scripts gate: product ships a script or names a forbidden host tool:\n    ${_report}")
endif()

message(STATUS "no-scripts gate: PASS (no script files; no forbidden tool literals in src/, include/)")
