# Configuration, lifecycle state machine, remote settings.json management, and
# the platform-neutral upload-and-paste pipeline. The pipeline depends on the
# SFTP client (src/core/sftp) and on two injected function tables for the win32
# clipboard and keystroke layers; it names no platform header itself.
target_sources(picopaste_core PRIVATE
  config.cpp
  lifecycle.cpp
  hook_install.cpp
  upload_pipeline.cpp
)
