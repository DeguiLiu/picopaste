// picopaste — shared host-test support (portable process id + scratch file).
//
// WHY THIS FILE EXISTS
// The host test suite is compiled twice: on Linux for the sanitizer job and by
// MSVC for the Windows job. Three test needs are inherently platform-specific —
// a process id (embedded in remote path names so concurrent runs do not
// collide), a securely-created local scratch file to upload, and the
// child-process stream that carries the SFTP session. The coding conventions
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
// MSVC CRT equivalents of the POSIX primitives below.
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

// The child-process stream the integration tests drive: tests/' one platform
// selection point, mirroring src/platform/{posix,win32}/. Both sides hand back
// the same picopaste::sftp::ByteStream, so the tests below never name a
// platform. The Windows header reaches <windows.h>; it defines
// WIN32_LEAN_AND_MEAN/NOMINMAX first, so the min/max/interface pollution the
// previous comment warned about is contained to the three TUs that need it.
#if defined(_WIN32)
#include "../src/platform/win32/stream_win32.hpp"
#include "../src/platform/win32/win32_util.hpp"
#else
#include "../src/platform/posix/stream_posix.hpp"
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

namespace detail {

#if defined(_WIN32)
// win32::ChildStream::stream() sets ByteStream::ctx to the ChildStream itself,
// so the object must outlive the stream. The tests carry only the ByteStream,
// so the factory owns a small fixed pool — the Windows analogue of the fixed
// pipe pool inside posix::SpawnStream. Test cases run sequentially and close
// their stream before the next spawn, so slots are reused at once.
constexpr std::size_t kMaxTestStreams = 8u;

struct ChildStreamPool {
    win32::ChildStream streams[kMaxTestStreams];
    bool in_use[kMaxTestStreams] = {};
};

// Function-local static in an inline function: one shared instance across every
// test translation unit that includes this header.
inline ChildStreamPool& StreamPool() noexcept
{
    static ChildStreamPool pool;
    return pool;
}

// Replaces ChildStream::stream().close so closing also frees the pool slot. The
// pointer difference is well defined because ctx always points into this pool.
inline void PooledClose(void* ctx) noexcept
{
    ChildStreamPool& pool = StreamPool();
    win32::ChildStream* self = static_cast<win32::ChildStream*>(ctx);
    const std::ptrdiff_t index = self - pool.streams;
    if ((index >= 0) && (index < static_cast<std::ptrdiff_t>(kMaxTestStreams))) {
        self->Close();
        pool.in_use[static_cast<std::size_t>(index)] = false;
    }
}

// The integration argv is fixed and contains no whitespace or quotes. A general
// Windows command-line quoter is out of scope here, so a token that would need
// quoting fails the factory closed (callers then SKIP) rather than risk
// silently mis-splitting the command.
inline bool IsSimpleArg(const char* arg) noexcept
{
    if (nullptr == arg) {
        return false;
    }
    for (const char* p = arg; '\0' != *p; ++p) {
        if ((' ' == *p) || ('\t' == *p) || ('"' == *p)) {
            return false;
        }
    }
    return true;
}

// Joins argv (nullptr-terminated) into a CreateProcessW command line. Returns
// false on a null argv, an argument that needs quoting, or buffer overflow.
inline bool BuildCommandLine(const char* const* argv, wchar_t* dst, std::size_t cap) noexcept
{
    if (nullptr == argv) {
        return false;
    }
    std::size_t used = 0u;
    for (std::size_t i = 0u; nullptr != argv[i]; ++i) {
        if (!IsSimpleArg(argv[i])) {
            return false;
        }
        wchar_t token[win32::kMaxCommandLineChars] = {};
        if (!win32::Utf8ToWide(argv[i], token, win32::kMaxCommandLineChars)) {
            return false;
        }
        if (0u != i) {
            if ((used + 1u) >= cap) {
                return false;
            }
            dst[used] = L' ';
            ++used;
        }
        for (std::size_t k = 0u; L'\0' != token[k]; ++k) {
            if ((used + 1u) >= cap) {
                return false;
            }
            dst[used] = token[k];
            ++used;
        }
        dst[used] = L'\0';
    }
    return (0u != used);
}
#endif

}  // namespace detail

// Spawn a child process whose stdin/stdout become a ByteStream, in the same
// shape the POSIX path already exposes: `ssh -s localhost sftp` terminated at
// argv[0] == nullptr. Returns kChannelSpawnFailed on any failure so callers
// keep their existing SKIP-not-fail behaviour.
inline Result<sftp::ByteStream> SpawnStream(const char* const* argv) noexcept
{
#if defined(_WIN32)
    wchar_t command_line[win32::kMaxCommandLineChars] = {};
    if (!detail::BuildCommandLine(argv, command_line, win32::kMaxCommandLineChars)) {
        return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
    }

    detail::ChildStreamPool& pool = detail::StreamPool();
    for (std::size_t i = 0u; i < detail::kMaxTestStreams; ++i) {
        if (pool.in_use[i]) {
            continue;
        }
        pool.in_use[i] = true;
        const win32::ChildStreamOptions options{command_line, nullptr, nullptr};
        if (!pool.streams[i].Spawn(options)) {
            pool.in_use[i] = false;
            return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
        }
        sftp::ByteStream stream = pool.streams[i].stream();
        // ChildStream's own close thunk reaps the child but cannot free the slot
        // it came from; this wrapper does both.
        stream.close = &detail::PooledClose;
        return Result<sftp::ByteStream>::success(stream);
    }
    return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
#else
    return posix::SpawnStream(argv);
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
