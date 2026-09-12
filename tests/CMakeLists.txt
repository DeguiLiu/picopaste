# Host test suite.
#
# The core (SFTP codec, config, state machine, log rotation) is platform-neutral
# and verified here on Linux — including a real end-to-end test against the
# system's own /usr/lib/openssh/sftp-server, which speaks the exact protocol the
# Windows client will speak. Windows-only layers are covered by the MSVC job in
# .github/workflows/ci.yml and by the client's own `selftest` command.
find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
  )
  FetchContent_MakeAvailable(Catch2)
endif()

add_executable(picopaste_tests
  test_sftp_packet.cpp
  test_sftp_client.cpp
  test_config.cpp
  test_log.cpp
  test_lifecycle.cpp
  test_memory_budget.cpp
  test_hook_install.cpp
  test_dir_ops.cpp
  test_upload_pipeline.cpp
  test_supervision.cpp
)
target_link_libraries(picopaste_tests PRIVATE picopaste_core Catch2::Catch2WithMain
  picopaste_msvc_crt)

if(WIN32)
  # The tests exercise the real ssh SFTP channel. picopaste::test::SpawnStream
  # forwards to win32::ChildStream on Windows, so its implementation must join
  # this target (the picopaste executable compiles its own copy; these are two
  # separate binaries). stream_win32.cpp uses only kernel32 (linked by default)
  # plus the header-only win32_util helper, so no extra libraries are needed.
  target_sources(picopaste_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/platform/win32/stream_win32.cpp")
endif()

# Same posture as the product targets, via the shared helper: /WX on MSVC and
# -Werror on GCC whenever PICOPASTE_WERROR=ON. Test sources are ours, so a
# warning in one is a defect in this repo and must fail the build; hardcoding
# the flags here without the error switch let test warnings pass CI silently
# and made docs/build/windows-compile-test.md's "/WX applies" claim false for
# this target.
picopaste_apply_flags(picopaste_tests)

include(Catch)
# When the local sftp subsystem is unavailable every case in an integration
# test SKIPs, and a run with zero executed tests exits 4. Catch2's
# --allow-running-no-tests would turn that into a PASS, which is the exact
# dishonesty this suite exists to prevent. SKIP_RETURN_CODE instead tells ctest
# to report the case as Skipped -- an honest "did not run".
catch_discover_tests(picopaste_tests PROPERTIES SKIP_RETURN_CODE 4)

# ---------------------------------------------------------------------------
# Mechanical gates for design promises that were previously only intentions.
# All are CMake scripts (not shell), so they run identically under the Linux
# and Windows CI jobs via ctest. See tools/check_*.cmake for what they enforce.
# The format gate SKIPs only when no clang-format is requested and none is on
# PATH. CI pins a formatter and passes -DPICOPASTE_CLANG_FORMAT=<name>, which
# makes a missing binary a hard failure instead of a silent pass.
# ---------------------------------------------------------------------------
add_test(NAME gate_no_scripts
  COMMAND ${CMAKE_COMMAND} "-DPICOPASTE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
          -P "${CMAKE_SOURCE_DIR}/tools/check_no_scripts.cmake")
add_test(NAME gate_posix_leak
  COMMAND ${CMAKE_COMMAND} "-DPICOPASTE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
          -P "${CMAKE_SOURCE_DIR}/tools/check_posix_leak.cmake")
set(PICOPASTE_CLANG_FORMAT "" CACHE FILEPATH
  "clang-format binary for gate_clang_format (empty searches PATH)")
add_test(NAME gate_clang_format
  COMMAND ${CMAKE_COMMAND} "-DPICOPASTE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
          "-DPICOPASTE_CLANG_FORMAT=${PICOPASTE_CLANG_FORMAT}"
          -P "${CMAKE_SOURCE_DIR}/tools/check_clang_format.cmake")
