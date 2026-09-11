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

# picojson's value union trips GCC's -Wmaybe-uninitialized at -O2+ on GCC 13+,
# where the diagnostic is emitted late (during optimization) and is therefore
# NOT attributed to a system-header location. Neither the vendored-header shim
# nor the location-scoped pragma inside the file suppresses it there, although
# both do on GCC 11 -- which is why this only ever failed on CI. hook_install.cpp
# is the only translation unit that includes picojson, so disable that single
# warning for that single file rather than weakening -Werror for the target.
# MSVC does not emit this warning at all.
if(NOT MSVC)
  # TARGET_DIRECTORY is load-bearing, not decoration. picopaste_core is declared
  # in the root CMakeLists and only extended here, and a source-file property set
  # without an explicit target directory applies to THIS directory's scope only,
  # so it never reaches the root-scoped target and is dropped silently. Both the
  # bare relative path and the absolute path were tried and neither took effect;
  # the flag was verifiably absent from the generated build tree until this form.
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/hook_install.cpp"
    TARGET_DIRECTORY picopaste_core
    PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()
