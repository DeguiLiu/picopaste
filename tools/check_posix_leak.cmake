# picopaste — POSIX-leak gate (design §10 verification strategy).
#
# Invariant: no POSIX-only surface is *reachable from a Windows translation
# unit*. A construct is legal only when an enclosing platform #if guarantees it
# is unreachable on Windows:
#
#   * the *then* branch of a positive non-Windows guard
#     (__linux__, __APPLE__, __unix__, OSP_PLATFORM_LINUX/MACOS/POSIX), or
#   * the *else* branch of a positive Windows guard (_WIN32/_WIN64/_MSC_VER/
#     OSP_PLATFORM_WINDOWS), or
#   * the negated forms of the same identity macros (e.g. #ifndef _WIN32).
#
# The #else of a compiler/RT-Thread/arch guard is NOT safe: `defined(__GNUC__)
# && !defined(__clang__)` and `_WIN64 || __x86_64__` are true on Windows GCC /
# on Linux respectively, so those are treated as unknown and checked.
#
# The previous version enumerated a fixed header list and matched
# `#include <...>` on one physical line. That is a denylist and it lost:
#   - <sys/mman.h>, <fcntl.h> and any other header not enumerated passed;
#   - a line-spliced `#include <unistd\` + `.h>` defeated the per-line regex;
#   - bare ::open/::write/::mprotect/pthread_t/... references passed (only
#     `fork(` was matched).
#
# This version enforces the invariant positively:
#   1. Includes are read from *logical* lines (backslash continuations joined),
#      so splicing cannot hide a header.
#   2. A header reachable on Windows must be on a curated allowlist of headers
#      that exist on MSVC (C++ standard library, MSVC CRT, Windows SDK). Any
#      unknown header fails until a human adds it — the allowlist makes the
#      check complete where a denylist cannot. The handful of CRT headers that
#      shadow POSIX names (fcntl.h, io.h, sys/stat.h, sys/types.h) are allowed
#      only inside an explicit Windows guard, because outside one they are a
#      platform dependency, not portable code.
#   3. POSIX function/type identifiers used on the Windows-reachable path are
#      flagged: qualified `::name`, whole-word POSIX types (pthread_t,
#      cpu_set_t, ...), and unqualified POSIX calls (read(, write(, ioctl(,
#      socket(, mmap(, ...) that are not member accesses. Comments and string
#      literals are stripped first so prose ("link()+unlink()") stays legal.
#
# Scope: product sources under src/ and include/, and the host test suite under
# tests/ — all three are compiled by the MSVC job, so a POSIX include or symbol
# in a test is as fatal as one in a product file. The only exclusion is
# src/platform/posix/, which is legitimately POSIX and is excluded from the
# Windows build by `if(NOT WIN32)` in CMakeLists.txt.
#
# Usage:
#   cmake -DPICOPASTE_SOURCE_DIR=<repo root> -P tools/check_posix_leak.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT PICOPASTE_SOURCE_DIR)
  get_filename_component(PICOPASTE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# ---------------------------------------------------------------------------
# Policy data. Space-separated, converted to lists below.
# ---------------------------------------------------------------------------

# Headers that exist on MSVC and may appear at any point of a Windows TU.
set(_portable_headers "
  algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat
  charconv chrono cinttypes climits clocale cmath codecvt compare complex
  concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint
  cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution
  filesystem format forward_list fstream functional future initializer_list
  iomanip ios iosfwd iostream istream iterator latch limits list locale map
  memory mutex new numbers numeric optional ostream queue random ranges ratio
  regex scoped_allocator semaphore set shared_mutex source_location span sstream
  stack stdexcept stop_token streambuf string string_view strstream syncstream
  system_error thread tuple type_traits typeindex typeinfo unordered_map
  unordered_set utility valarray variant vector version
  assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h
  locale.h math.h setjmp.h signal.h stdalign.h stdarg.h stdbool.h stddef.h
  stdint.h stdio.h stdlib.h string.h tgmath.h time.h uchar.h wchar.h wctype.h
  windows.h windef.h winbase.h winuser.h winnt.h winerror.h winnls.h wincon.h
  wingdi.h winsock2.h ws2tcpip.h shellapi.h shlwapi.h shlobj.h shlobj_core.h
  objbase.h objidl.h ole2.h oleidl.h combaseapi.h knownfolders.h propidl.h
  propkey.h propvarutil.h wincodec.h wtypes.h wtypesbase.h psapi.h synchapi.h
  handleapi.h fileapi.h processenv.h memoryapi.h errhandlingapi.h jobapi.h
  jobapi2.h winreg.h tchar.h strsafe.h securitybaseapi.h sddl.h userenv.h
  processthreadsapi.h namedpipeapi.h ioapiset.h winioctl.h timezoneapi.h
  profileapi.h sysinfoapi.h libloaderapi.h consoleapi.h debugapi.h commctrl.h
  dbt.h shellscalingapi.h shtypes.h shobjidl.h
")

# MSVC CRT headers that shadow POSIX names. Legal on Windows only inside an
# explicit Windows guard; unguarded they are a platform dependency.
set(_windows_crt_headers "
  fcntl.h io.h sys/stat.h sys/types.h sys/timeb.h process.h direct.h conio.h
  share.h sys/locking.h malloc.h
")

# POSIX-only headers. Reachable on Windows only if a POSIX guard is missing.
set(_posix_headers "
  unistd.h pthread.h sched.h semaphore.h termios.h dirent.h poll.h syslog.h
  spawn.h dlfcn.h getopt.h grp.h pwd.h strings.h libgen.h wordexp.h regex.h
  glob.h fnmatch.h iconv.h langinfo.h monetary.h nl_types.h search.h sysexits.h
  tar.h ulimit.h utime.h ftw.h aio.h mqueue.h netdb.h ifaddrs.h
  sys/mman.h sys/socket.h sys/epoll.h sys/wait.h sys/time.h sys/resource.h
  sys/utsname.h sys/uio.h sys/un.h sys/ipc.h sys/shm.h sys/sem.h sys/msg.h
  sys/select.h sys/ioctl.h sys/param.h sys/prctl.h sys/syscall.h sys/inotify.h
  sys/eventfd.h sys/timerfd.h sys/signalfd.h sys/file.h sys/ptrace.h
  netinet/in.h netinet/tcp.h net/if.h arpa/inet.h
")

# POSIX functions flagged when qualified as ::name (a qualified call is
# unambiguous POSIX surface regardless of call syntax).
set(_posix_funcs "
  open close read write lseek pread pwrite fstat stat lstat fstatat
  mmap munmap mprotect msync ioctl socket socketpair connect accept bind listen
  send recv sendto recvfrom select pselect poll ppoll fork vfork pipe dup dup2
  unlink rmdir mkdir chmod chown kill raise fsync fdatasync ftruncate truncate
  readlink symlink opendir readdir closedir rewinddir telldir seekdir
  dlopen dlsym dlclose sigaction sigprocmask sigemptyset sigaddset
  gettimeofday clock_gettime nanosleep usleep sleep getpid getppid getuid
  geteuid getgid sysconf pathconf isatty ttyname tcgetattr tcsetattr
  sched_yield pthread_create pthread_join pthread_mutex_lock pthread_mutex_unlock
  pthread_cond_wait pthread_cond_signal
  mkstemp mkstemps mkostemp mkdtemp
")

# POSIX functions flagged when called *unqualified* (read(, write(, ...).
# The preceding-character class excludes `obj.read(` and `->write(`.
set(_posix_bare_funcs "
  open close read write lseek pread pwrite ioctl socket mmap munmap mprotect
  msync poll select pselect fork vfork pipe dup dup2 unlink rmdir mkdir chmod
  chown kill fsync fdatasync ftruncate truncate readlink symlink opendir readdir
  closedir dlopen dlsym gettimeofday clock_gettime nanosleep usleep isatty
  tcgetattr tcsetattr sysconf getpid getppid sched_yield
  pthread_create pthread_mutex_lock pthread_mutex_unlock
  mkstemp mkstemps mkostemp mkdtemp
")

# Whole-word POSIX types.
set(_posix_types "
  pthread_t pthread_attr_t pthread_mutex_t pthread_cond_t pthread_rwlock_t
  pthread_key_t pthread_once_t pthread_barrier_t pthread_mutexattr_t
  cpu_set_t sigset_t ssize_t off_t mode_t pid_t uid_t gid_t id_t key_t nfds_t
  socklen_t in_addr_t in_port_t sa_family_t clockid_t timer_t sem_t mqd_t
  dev_t ino_t nlink_t blksize_t blkcnt_t useconds_t suseconds_t sighandler_t
  u_char u_short u_int u_long caddr_t register_t rlim_t sigjmp_buf DIR dirent
")

foreach(_v _portable_headers _windows_crt_headers _posix_headers _posix_funcs _posix_bare_funcs _posix_types)
  # Collapse all whitespace runs to single spaces first: the literals above are
  # indented across lines, and naive splitting would create empty list items
  # (which turn regex alternations into a match-anything).
  string(REGEX REPLACE "[ \t\r\n]+" " " ${_v} "${${_v}}")
  string(STRIP "${${_v}}" ${_v})
  string(REPLACE " " ";" ${_v} "${${_v}}")
endforeach()

# Regex alternations for the symbol scan.
string(REPLACE ";" "|" _funcs_alt "${_posix_funcs}")
string(REPLACE ";" "|" _bare_alt "${_posix_bare_funcs}")
string(REPLACE ";" "|" _types_alt "${_posix_types}")

# ---------------------------------------------------------------------------
# Guard classification.
# ---------------------------------------------------------------------------
function(picopaste_classify_guard line out_kind out_negated)
  string(TOLOWER "${line}" l)
  set(_k "other")
  set(_neg FALSE)

  # Explicit single-macro identity guards (whole expression), incl. negation.
  if(l MATCHES "^#[ \t]*ifndef[ \t]+(_win32|_win64|_msc_ver|osp_platform_windows)[ \t]*$")
    set(_k "windows")
    set(_neg TRUE)
  elseif(l MATCHES "^#[ \t]*ifndef[ \t]+(osp_platform_linux|osp_platform_macos|osp_platform_posix|__linux__|__apple__|__unix__)[ \t]*$")
    set(_k "posix")
    set(_neg TRUE)
  elseif(l MATCHES "^#[ \t]*if[ \t]+![ \t]*defined[ \t]*\\([ \t]*(_win32|_win64|_msc_ver|osp_platform_windows)[ \t]*\\)[ \t]*$")
    set(_k "windows")
    set(_neg TRUE)
  elseif(l MATCHES "^#[ \t]*if[ \t]+![ \t]*defined[ \t]*\\([ \t]*(osp_platform_linux|osp_platform_macos|osp_platform_posix|__linux__|__apple__|__unix__)[ \t]*\\)[ \t]*$")
    set(_k "posix")
    set(_neg TRUE)
  else()
    # Positive forms. Anything mixing platform classes, an architecture macro,
    # a compiler macro, or containing '!' is unknown (neither branch assumed
    # Windows-unreachable). This kept `defined(__GNUC__) && !defined(__clang__)`
    # and `_WIN64 || __x86_64__` conservative.
    set(_win FALSE)
    set(_pos FALSE)
    set(_excl FALSE)
    if(l MATCHES "(_win32|_win64|_msc_ver|osp_platform_windows)([^A-Za-z0-9_]|$)")
      set(_win TRUE)
    endif()
    if(l MATCHES "(osp_platform_linux|osp_platform_macos|osp_platform_posix|__linux__|__apple__|__unix__)([^A-Za-z0-9_]|$)")
      set(_pos TRUE)
    endif()
    if(l MATCHES "(__gnuc__|__clang__|__has_include|__has_feature|__x86_64__|__aarch64__|__i386__|__arm__|__amd64__|osp_platform_rtthread)")
      set(_excl TRUE)
    endif()
    string(FIND "${l}" "!" _bang)
    if(_bang EQUAL -1)
      if(_win AND NOT _pos AND NOT _excl)
        set(_k "windows")
      elseif(_pos AND NOT _win AND NOT _excl)
        set(_k "posix")
      endif()
    endif()
  endif()

  set(${out_kind} "${_k}" PARENT_SCOPE)
  set(${out_negated} "${_neg}" PARENT_SCOPE)
endfunction()

# A frame's current branch is Windows-unreachable when its condition can only
# hold off Windows.
function(picopaste_branch_safe kind negated branch out)
  set(_s FALSE)
  if(kind STREQUAL "windows")
    if(negated AND branch STREQUAL "then")
      set(_s TRUE)
    elseif(NOT negated AND branch STREQUAL "else")
      set(_s TRUE)
    endif()
  elseif(kind STREQUAL "posix")
    if(NOT negated AND branch STREQUAL "then")
      set(_s TRUE)
    elseif(negated AND branch STREQUAL "else")
      set(_s TRUE)
    endif()
  endif()
  set(${out} "${_s}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Scan.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _sources
  "${PICOPASTE_SOURCE_DIR}/src/*.c"    "${PICOPASTE_SOURCE_DIR}/src/*.cc"
  "${PICOPASTE_SOURCE_DIR}/src/*.cpp"  "${PICOPASTE_SOURCE_DIR}/src/*.h"
  "${PICOPASTE_SOURCE_DIR}/src/*.hpp"
  "${PICOPASTE_SOURCE_DIR}/include/*.h" "${PICOPASTE_SOURCE_DIR}/include/*.hpp"
  "${PICOPASTE_SOURCE_DIR}/tests/*.c"    "${PICOPASTE_SOURCE_DIR}/tests/*.cc"
  "${PICOPASTE_SOURCE_DIR}/tests/*.cpp"  "${PICOPASTE_SOURCE_DIR}/tests/*.h"
  "${PICOPASTE_SOURCE_DIR}/tests/*.hpp")

set(_violations "")
set(_checked 0)

foreach(_file ${_sources})
  string(REPLACE "\\" "/" _fwd "${_file}")
  if(_fwd MATCHES "platform/posix/")
    continue()
  endif()
  math(EXPR _checked "${_checked} + 1")

  file(READ "${_file}" _raw)
  # Protect semicolons (CMake's list separator) and normalise CRLF, then walk
  # the raw content by newline. file(STRINGS) is not used because it drops
  # blank lines, which would make reported line numbers wrong.
  string(REPLACE ";" "@SEMI@" _raw "${_raw}")
  string(REPLACE "\r" "" _raw "${_raw}")
  set(_rest "${_raw}")

  set(_depth 0)
  set(_in_block FALSE)
  set(_logical "")
  set(_lineno 0)
  set(_logical_start 0)

  while(NOT _rest STREQUAL "")
    string(FIND "${_rest}" "\n" _nl)
    if(_nl EQUAL -1)
      set(_clean "${_rest}")
      set(_rest "")
    else()
      string(SUBSTRING "${_rest}" 0 ${_nl} _clean)
      math(EXPR _after "${_nl} + 1")
      string(SUBSTRING "${_rest}" ${_after} -1 _rest)
    endif()
    math(EXPR _lineno "${_lineno} + 1")

    # --- strip comments (stateful, preserves line count) -------------------
    if(_in_block)
      string(FIND "${_clean}" "*/" _end)
      if(_end EQUAL -1)
        set(_clean "")
      else()
        math(EXPR _after "${_end} + 2")
        string(SUBSTRING "${_clean}" ${_after} -1 _clean)
        set(_in_block FALSE)
      endif()
    endif()
    if(NOT _in_block)
      while(TRUE)
        string(FIND "${_clean}" "/*" _bs)
        string(FIND "${_clean}" "//" _ls)
        if(NOT _bs EQUAL -1 AND (_ls EQUAL -1 OR _bs LESS _ls))
          # block comment starts before any line comment
          string(FIND "${_clean}" "*/" _be)
          if(_be EQUAL -1)
            string(SUBSTRING "${_clean}" 0 ${_bs} _clean)
            set(_in_block TRUE)
          else()
            math(EXPR _bafter "${_be} + 2")
            string(SUBSTRING "${_clean}" 0 ${_bs} _pre)
            string(SUBSTRING "${_clean}" ${_bafter} -1 _post)
            set(_clean "${_pre} ${_post}")
          endif()
        elseif(NOT _ls EQUAL -1)
          string(SUBSTRING "${_clean}" 0 ${_ls} _clean)
          break()
        else()
          break()
        endif()
      endwhile()
    endif()

    # --- join backslash continuations --------------------------------------
    if(_logical STREQUAL "")
      set(_logical "${_clean}")
      set(_logical_start ${_lineno})
    else()
      string(APPEND _logical "${_clean}")
    endif()
    if(_logical MATCHES "\\\\$")
      string(REGEX REPLACE "\\\\$" "" _logical "${_logical}")
      continue()
    endif()

    set(_s "${_logical}")
    string(STRIP "${_s}" _s)
    string(REPLACE ";" "@SEMI@" _s "${_s}")

    # --- guard stack --------------------------------------------------------
    if(_s MATCHES "^#[ \t]*if")
      math(EXPR _depth "${_depth} + 1")
      picopaste_classify_guard("${_s}" _kind _neg)
      set(_kind_${_depth} "${_kind}")
      set(_neg_${_depth} "${_neg}")
      set(_branch_${_depth} "then")
    elseif(_s MATCHES "^#[ \t]*elif")
      string(REGEX REPLACE "^#[ \t]*elif" "#if" _norm "${_s}")
      picopaste_classify_guard("${_norm}" _kind _neg)
      set(_kind_${_depth} "${_kind}")
      set(_neg_${_depth} "${_neg}")
      set(_branch_${_depth} "then")
    elseif(_s MATCHES "^#[ \t]*else")
      if(_depth GREATER 0)
        set(_branch_${_depth} "else")
      endif()
    elseif(_s MATCHES "^#[ \t]*endif")
      if(_depth GREATER 0)
        unset(_kind_${_depth})
        unset(_neg_${_depth})
        unset(_branch_${_depth})
        math(EXPR _depth "${_depth} - 1")
      endif()
    else()
      # Reachability on Windows: unreachable if any enclosing branch is safe.
      set(_reachable TRUE)
      set(_win_then FALSE)
      if(_depth GREATER 0)
        foreach(_i RANGE 1 ${_depth})
          picopaste_branch_safe("${_kind_${_i}}" "${_neg_${_i}}" "${_branch_${_i}}" _safe)
          if(_safe)
            set(_reachable FALSE)
          endif()
          if("${_kind_${_i}}" STREQUAL "windows" AND NOT ${_neg_${_i}} AND "${_branch_${_i}}" STREQUAL "then")
            set(_win_then TRUE)
          endif()
        endforeach()
      endif()

      if(_reachable)
        # --- includes -----------------------------------------------------
        set(_hdr "")
        set(_angle FALSE)
        set(_malformed FALSE)
        if(_s MATCHES "^#[ \t]*include")
          if(_s MATCHES "^#[ \t]*include[ \t]*<([^>]+)>")
            set(_hdr "${CMAKE_MATCH_1}")
            set(_angle TRUE)
          elseif(_s MATCHES "^#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(_hdr "${CMAKE_MATCH_1}")
          else()
            set(_malformed TRUE)
          endif()
        endif()

        if(_malformed)
          list(APPEND _violations "${_file}:${_logical_start}: unparseable #include on the Windows path: ${_s}")
        elseif(NOT _hdr STREQUAL "")
          string(TOLOWER "${_hdr}" _hdr_l)
          if(_angle)
            list(FIND _portable_headers "${_hdr_l}" _p)
            # Catch2 is fetched and built by the test target on every platform,
            # so its headers are reachable on MSVC by construction. The
            # allowlist below covers headers shipped by the toolchain/SDK, which
            # a build dependency is not; allowing it here is not a POSIX exemption.
            string(FIND "${_hdr_l}" "catch2/" _catch2)
            if(NOT _p EQUAL -1)
              # allowed anywhere
            elseif(_catch2 EQUAL 0)
              # allowed: test-framework header built as part of this target
            else()
              list(FIND _windows_crt_headers "${_hdr_l}" _w)
              if(NOT _w EQUAL -1 AND _win_then)
                # allowed: explicitly selected Windows CRT variant
              else()
                if(_win_then)
                  list(APPEND _violations "${_file}:${_logical_start}: non-portable header reachable on the Windows path: <${_hdr}>")
                else()
                  list(APPEND _violations "${_file}:${_logical_start}: non-portable header not behind a platform guard: <${_hdr}>")
                endif()
              endif()
            endif()
          else()
            get_filename_component(_base "${_hdr_l}" NAME)
            list(FIND _posix_headers "${_base}" _pb)
            list(FIND _posix_headers "${_hdr_l}" _pf)
            if(NOT _pb EQUAL -1 OR NOT _pf EQUAL -1)
              list(APPEND _violations "${_file}:${_logical_start}: POSIX-only header not behind a POSIX guard: \"${_hdr}\"")
            else()
              list(FIND _windows_crt_headers "${_base}" _wb)
              if(NOT _wb EQUAL -1 AND NOT _win_then)
                list(APPEND _violations "${_file}:${_logical_start}: Windows CRT header outside a Windows guard: \"${_hdr}\"")
              endif()
            endif()
          endif()
        endif()

        # --- POSIX identifiers --------------------------------------------
        set(_scan "${_s}")
        string(REGEX REPLACE "\"[^\"]*\"" " " _scan "${_scan}")
        string(REPLACE ";" "@SEMI@" _scan "${_scan}")
        if(_scan MATCHES "::[ \t]*(${_funcs_alt})([^A-Za-z0-9_]|$)")
          list(APPEND _violations "${_file}:${_logical_start}: POSIX function ::${CMAKE_MATCH_1} reachable on the Windows path")
        endif()
        # Unqualified calls only: require a call-like character before the name
        # so a member declaration/definition (`int read(int);`, `void close();`)
        # and member access (`s.read(`, `->write(`) do not false-positive.
        if(_scan MATCHES "(^|[{};,(=!&|+*/%<>?)-]|return[ \t]+|co_return[ \t]+|throw[ \t]+)[ \t]*(${_bare_alt})[ \t]*\\(")
          list(APPEND _violations "${_file}:${_logical_start}: POSIX call ${CMAKE_MATCH_2}( reachable on the Windows path")
        endif()
        if(_scan MATCHES "(^|[^A-Za-z0-9_])(${_types_alt})([^A-Za-z0-9_]|$)")
          list(APPEND _violations "${_file}:${_logical_start}: POSIX type ${CMAKE_MATCH_2} reachable on the Windows path")
        endif()
      endif()
    endif()

    set(_logical "")
  endwhile()
endforeach()

if(_violations)
  list(REMOVE_DUPLICATES _violations)
  list(LENGTH _violations _n)
  string(REPLACE ";" "\n    " _report "${_violations}")
  message(FATAL_ERROR
    "POSIX-leak gate: ${_n} violation(s); POSIX surface reachable from the MSVC path:\n    ${_report}")
endif()

message(STATUS "POSIX-leak gate: PASS (${_checked} files scanned: src/, include/, tests/; platform/posix/ excluded)")
