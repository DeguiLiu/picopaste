# SFTP v3 protocol layer. Owned by the SFTP workstream.
#
# Everything here is platform-neutral and verified on the host against the
# system's own /usr/lib/openssh/sftp-server.
target_sources(picopaste_core PRIVATE
  client.cpp
  packet.cpp
  dir_ops.cpp
)

# Longest remote path handled; mirrors picopaste::sftp::kMaxPathBytes.
target_compile_definitions(picopaste_core PUBLIC PICOPASTE_SFTP_MAX_PATH=512)
