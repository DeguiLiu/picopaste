# SFTP v3 protocol layer. Owned by the SFTP workstream.
#
# Everything here is platform-neutral and verified on the host against the
# system's own /usr/lib/openssh/sftp-server.
target_sources(ccclip_core PRIVATE
  client.cpp
  packet.cpp
)

# Longest remote path handled; mirrors ccclip::sftp::kMaxPathBytes.
target_compile_definitions(ccclip_core PUBLIC CCLIP_SFTP_MAX_PATH=512)
