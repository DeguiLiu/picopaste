# Configuration, lifecycle state machine, and remote settings.json management.
# Owned by the lifecycle workstream; hook_install by the remote-config workstream.
#
# upload_pipeline.cpp is added during integration, once the SFTP client and the
# platform clipboard layer both exist — it is the seam between them.
target_sources(picopaste_core PRIVATE
  config.cpp
  lifecycle.cpp
  hook_install.cpp
)
