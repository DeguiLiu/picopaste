# Bounded log sink with size-based rotation. Owned by the lifecycle workstream.
target_sources(picopaste_core PRIVATE
  log_ring.cpp
  log_sink.cpp
)
