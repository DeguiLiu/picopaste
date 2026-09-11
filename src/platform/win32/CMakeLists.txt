# Windows-only layers. Owned by the win32 workstream.
#
# Linked into the picopaste executable target, which is created by the root
# CMakeLists before this directory is added.
target_sources(picopaste PRIVATE
  main_win32.cpp     # wWinMain: console-attached --selftest runner; interactive mode not wired
  clipboard.cpp      # Win32 clipboard capture; WIC PNG encode; single temp-file path
  stream_win32.cpp   # ByteStream over a CreateProcessW child with pipe redirection
  inject.cpp         # SendInput Ctrl+Shift+V with a focus guard
  single_instance.cpp# CreateMutexW single instance + Job Object containment/limits
  tray.cpp           # Shell_NotifyIconW tray, TaskbarCreated re-add, balloon
  hotkey.cpp         # RegisterHotKey, message loop integration
  selftest.cpp       # per-capability self-check table, also exposed as a subcommand
)

# ---------------------------------------------------------------------------
# Vendored dacap/clip (MIT). Source copy under third_party/clip with upstream
# LICENSE.txt. Only the Windows translation units are built; the X11 and macOS
# backends are deliberately not vendored. CLIP_ENABLE_IMAGE pulls in the WIC
# DIB/PNG helpers used by the clipboard write side.
#
# One local patch is applied to clip.cpp: the library's only throw (its default
# error handler) becomes a no-op, because this project builds without
# exceptions. See the comment at that function.
# ---------------------------------------------------------------------------
target_sources(picopaste PRIVATE
  "${CMAKE_SOURCE_DIR}/third_party/clip/clip.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/clip/image.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/clip/clip_win.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/clip/clip_win_bmp.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/clip/clip_win_wic.cpp"
)
target_include_directories(picopaste PRIVATE "${CMAKE_SOURCE_DIR}/third_party/clip")
target_compile_definitions(picopaste PRIVATE CLIP_ENABLE_IMAGE=1)
