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
  picopaste.rc       # icon resources: the tray's three states + the .exe icon
)

# rc.exe looks for the .ico files named in picopaste.rc along its include path,
# so the directory that holds resources/ has to be on it. Harmless for the C++
# side: this directory holds no headers other than tray_icons.h.
target_include_directories(picopaste PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

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
# The vendored sources are not ours to fix, and they are not warning-clean under
# MSVC /W4: C4018 signed/unsigned comparison, C4267 size_t narrowing, C4456 and
# C4458 shadowing. They must therefore be compiled with warnings off.
#
# A separate static library rather than set_source_files_properties(): a
# source-file property is scoped to the directory that sets it, so setting one
# here does NOT reach picopaste, whose target is declared in the root
# CMakeLists. The previous form was silently inert -- /w appeared nowhere in the
# generated project and the vendored warnings were promoted to errors by the
# root target's /WX. A target is the correct unit of scope, and a whole-target
# /w cannot silently miss: the vendored code is the only thing in this library.
#
# The include directory is marked SYSTEM so the headers this library exports to
# picopaste (clip.h) stay warning-exempt for its consumers too.
# ---------------------------------------------------------------------------
set(PICOPASTE_CLIP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/clip/clip.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/clip/image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/clip/clip_win.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/clip/clip_win_bmp.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/clip/clip_win_wic.cpp"
)
foreach(_clip_src IN LISTS PICOPASTE_CLIP_SOURCES)
  if(NOT EXISTS "${_clip_src}")
    message(FATAL_ERROR "vendored clip source missing: ${_clip_src}")
  endif()
endforeach()

add_library(picopaste_clip STATIC ${PICOPASTE_CLIP_SOURCES})
target_include_directories(picopaste_clip SYSTEM PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/clip")
target_compile_definitions(picopaste_clip PUBLIC CLIP_ENABLE_IMAGE=1)
# Static CRT, matching the root target: a /MD library linked into an /MT
# executable produces LNK2005 duplicate-symbol errors at link time.
set_property(TARGET picopaste_clip PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(MSVC)
  # /w turns off all warnings for these translation units only.
  target_compile_options(picopaste_clip PRIVATE /w)
  # The library's default error handler throws; this project builds /EHsc-free.
  target_compile_options(picopaste_clip PRIVATE /EHs-c-)
else()
  target_compile_options(picopaste_clip PRIVATE -w)
endif()

target_link_libraries(picopaste PRIVATE picopaste_clip)

# ImmDisableIME, which keeps the input-method framework out of this process (see
# the comment in Tray::Create). The IME's own code running on our message path is
# what crashed the process repeatedly.
if(WIN32)
  target_link_libraries(picopaste PRIVATE imm32)
endif()
