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

add_executable(ccclip_tests
  test_sftp_packet.cpp
  test_sftp_client.cpp
  test_config.cpp
  test_log.cpp
  test_lifecycle.cpp
)
target_link_libraries(ccclip_tests PRIVATE ccclip_core Catch2::Catch2WithMain)

if(MSVC)
  target_compile_options(ccclip_tests PRIVATE /W4 /permissive- /utf-8)
  set_property(TARGET ccclip_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_compile_options(ccclip_tests PRIVATE -Wall -Wextra)
endif()

include(Catch)
catch_discover_tests(ccclip_tests)
