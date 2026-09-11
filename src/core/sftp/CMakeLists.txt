# SFTP v3 protocol layer. Owned by the SFTP workstream.
#
# Everything here is platform-neutral and verified on the host against the
# system's own /usr/lib/openssh/sftp-server.
target_sources(picopaste_core PRIVATE
  client.cpp
  packet.cpp
  dir_ops.cpp
)

# Deliberately no configure-time override for the maximum remote path length:
# picopaste::sftp::kMaxPathBytes in include/picopaste/sftp/client.hpp is the one
# source of truth. A matching #define here would be free to drift from it, and
# previously did (it was defined as PICOPASTE_SFTP_MAX_PATH=512 and read by
# nobody, so a stale name silently had no effect).
