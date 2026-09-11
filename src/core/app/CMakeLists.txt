# Configuration, lifecycle state machine and upload pipeline orchestration.
# Owned by the lifecycle workstream.
target_sources(ccclip_core PRIVATE
  config.cpp
  lifecycle.cpp
  upload_pipeline.cpp
)
