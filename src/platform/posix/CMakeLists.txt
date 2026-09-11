# Host-side byte stream: a POSIX pipe child running `ssh -s <host> sftp`.
#
# This is what makes the SFTP client verifiable without Windows — the test suite
# runs it against the machine's own /usr/lib/openssh/sftp-server. Not built on
# Windows, where platform/win32 supplies the equivalent.
if(NOT WIN32)
  target_sources(picopaste_core PRIVATE stream_posix.cpp)
endif()
