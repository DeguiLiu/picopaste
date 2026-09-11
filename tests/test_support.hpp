// picopaste — shared host-test support (portable process id + scratch file).
//
// WHY THIS FILE EXISTS
// The host test suite is compiled twice: on Linux for the sanitizer job and by
// MSVC for the Windows job. Two test needs are inherently platform-specific — a
// process id (embedded in remote path names so concurrent runs do not collide)
// and a securely-created local scratch file to upload. The coding conventions
// require platform differences to live in exactly one place rather than as
// #ifdefs sprinkled through every call site. For product code that place is
// src/platform/{posix,win32}/; this header is the tests/ analogue, so the three
// integration tests share one portable surface and every platform branch is
// confined here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
// MSVC CRT equivalents of the POSIX primitives below. Deliberately no
// <windows.h>: these tests also include Catch2, and keeping the heavy Win32
// header out of a Catch2 translation unit avoids its min/max/interface pollution.
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace picopaste::test {

// Current process id. Tests embed it in remote directory names so two runs on
// the same host (or a rerun after a crash) do not share a directory.
inline std::uint64_t ProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// A local scratch file with a name unique to this process. It is created
// exclusively (POSIX mkstemp / Windows O_EXCL), holds a caller-supplied payload
// for the whole test, and is removed on destruction. RAII guard: copy and assign
// are deleted so exactly one owner removes the file.
class TempFile final {
 public:
    TempFile() noexcept = default;
    ~TempFile() noexcept { Remove(); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    // Move is allowed so factories can return by value; the moved-from object is
    // left path-less and its destructor then removes nothing.
    TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
    TempFile& operator=(TempFile&&) = delete;

    // Creates the file and writes `len` bytes from `data` into it. Returns an
    // invalid object (valid() == false) on any failure and leaves no file behind.
    static TempFile Create(const std::uint8_t* data, std::size_t len) noexcept;

    bool valid() const noexcept { return !path_.empty(); }
    const char* path() const noexcept { return path_.c_str(); }

 private:
    void Remove() noexcept;
    std::string path_{};
};

namespace detail {

// Platform hook: create an exclusively-claimed scratch file and write `len`
// bytes. Returns true and sets `path` on success; on failure returns false and
// removes anything it created.
inline bool WriteScratchFile(const std::uint8_t* data, std::size_t len, std::string& path) noexcept
{
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = ".";
    }
    // _mktemp_s only mints a candidate name; _O_EXCL is what actually claims it,
    // so a lost race just mints the next candidate. Bounded to avoid an endless
    // loop if the temp directory is permanently unwritable.
    for (std::uint32_t attempt = 0u; attempt < 8u; ++attempt) {
        std::string candidate = (dir / "picopaste-test-XXXXXX").string();
        if (0 != ::_mktemp_s(candidate.data(), candidate.size() + 1u)) {
            continue;
        }
        const std::int32_t fd = static_cast<std::int32_t>(
            ::_open(candidate.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                    _S_IREAD | _S_IWRITE));
        if (0 > fd) {
            continue;
        }
        std::size_t off = 0u;
        bool ok = true;
        while (off < len) {
            const std::int32_t n =
                static_cast<std::int32_t>(::_write(fd, data + off, static_cast<unsigned int>(len - off)));
            if (0 >= n) {
                ok = false;
                break;
            }
            off += static_cast<std::size_t>(n);
        }
        (void)::_close(fd);
        if (ok) {
            path.assign(candidate);
            return true;
        }
        (void)::_unlink(candidate.c_str());
    }
    return false;
#else
    char tmpl[] = "/tmp/picopaste-test-XXXXXX";
    const std::int32_t fd = static_cast<std::int32_t>(::mkstemp(tmpl));
    if (0 > fd) {
        return false;
    }
    std::size_t off = 0u;
    bool ok = true;
    while (off < len) {
        const auto n = ::write(fd, data + off, len - off);
        if (0 >= n) {
            ok = false;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    if (0 != ::close(fd)) {
        ok = false;
    }
    if (ok) {
        path.assign(tmpl);
    } else {
        (void)::unlink(tmpl);
    }
    return ok;
#endif
}

inline void DeleteScratchFile(const char* path) noexcept
{
#if defined(_WIN32)
    (void)::_unlink(path);
#else
    (void)::unlink(path);
#endif
}

}  // namespace detail

inline TempFile TempFile::Create(const std::uint8_t* data, std::size_t len) noexcept
{
    TempFile file;
    if ((nullptr == data) && (0u != len)) {
        return file;
    }
    if (!detail::WriteScratchFile(data, len, file.path_)) {
        file.path_.clear();
    }
    return file;
}

inline void TempFile::Remove() noexcept
{
    if (!path_.empty()) {
        detail::DeleteScratchFile(path_.c_str());
        path_.clear();
    }
}

}  // namespace picopaste::test
