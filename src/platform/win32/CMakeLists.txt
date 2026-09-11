# Windows-only layers. Owned by the win32 workstream.
#
# Linked into the cc-clip-cpp executable target, which is created by the root
# CMakeLists before this directory is added.
target_sources(cc-clip-cpp PRIVATE
  clipboard.cpp      # Win32 clipboard capture; WIC PNG encode; single temp-file path
  stream_win32.cpp   # ByteStream over a CreateProcessW child with pipe redirection
  inject.cpp         # SendInput Ctrl+Shift+V with a focus guard
  single_instance.cpp# CreateMutexW single instance + Job Object containment/limits
  tray.cpp           # Shell_NotifyIconW tray, TaskbarCreated re-add, balloon
  hotkey.cpp         # RegisterHotKey, message loop integration
  selftest.cpp       # per-capability self-check table, also exposed as a subcommand
)
