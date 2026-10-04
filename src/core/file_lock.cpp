#include "core/file_lock.hpp"

#include "core/fs.hpp"

#include <algorithm>
#include <format>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace decomp {

namespace {

enum class Attempt { locked, busy, failed };

} // namespace

#ifdef _WIN32

static Attempt try_lock(const std::filesystem::path& path, FileLock::Mode mode, void*& handle, std::string& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = std::format("cannot open lock file '{}' (error {})", fs::to_utf8(path), GetLastError());
        return Attempt::failed;
    }
    OVERLAPPED ov{};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (mode == FileLock::Mode::exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
        handle = h;
        return Attempt::locked;
    }
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING) return Attempt::busy;
    error = std::format("cannot lock '{}' (error {})", fs::to_utf8(path), err);
    return Attempt::failed;
}

void FileLock::release() {
    if (handle_) {
        OVERLAPPED ov{};
        UnlockFileEx(static_cast<HANDLE>(handle_), 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool FileLock::held() const { return handle_ != nullptr; }

FileLock::FileLock(FileLock&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

#else

static Attempt try_lock(const std::filesystem::path& path, FileLock::Mode mode, int& fd_out, std::string& error) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = std::format("cannot open lock file '{}': {}", fs::to_utf8(path), std::strerror(errno));
        return Attempt::failed;
    }
    const int op = (mode == FileLock::Mode::exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
    while (::flock(fd, op) != 0) {
        if (errno == EINTR) continue;
        const int err = errno;
        ::close(fd);
        if (err == EWOULDBLOCK) return Attempt::busy;
        error = std::format("cannot lock '{}': {}", fs::to_utf8(path), std::strerror(err));
        return Attempt::failed;
    }
    fd_out = fd;
    return Attempt::locked;
}

void FileLock::release() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
}

bool FileLock::held() const { return fd_ >= 0; }

FileLock::FileLock(FileLock&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

#endif

FileLock::~FileLock() { release(); }

Result<std::optional<FileLock>> FileLock::try_acquire(const std::filesystem::path& path, Mode mode) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    FileLock lock;
    std::string error;
#ifdef _WIN32
    const Attempt a = try_lock(path, mode, lock.handle_, error);
#else
    const Attempt a = try_lock(path, mode, lock.fd_, error);
#endif
    if (a == Attempt::failed) return make_error(ErrorCode::io, "{}", error);
    if (a == Attempt::busy) return std::optional<FileLock>{};
    return std::optional<FileLock>(std::move(lock));
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, Mode mode, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto delay = std::chrono::milliseconds(2);
    while (true) {
        TRY_ASSIGN(auto lock, try_acquire(path, mode));
        if (lock) return std::move(*lock);
        if (std::chrono::steady_clock::now() >= deadline)
            return make_error(ErrorCode::timeout, "timed out waiting for the lock on '{}'", fs::to_utf8(path));
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(50));
    }
}

} // namespace decomp
