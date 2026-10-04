#pragma once

#include "core/result.hpp"

#include <chrono>
#include <filesystem>
#include <optional>

namespace decomp {

// An advisory lock on a file, shared or exclusive, held until the object is released or destroyed
// (or the process exits). Every acquisition opens its own descriptor (POSIX flock) or handle (Windows
// LockFileEx), so locks conflict between processes and between FileLock objects of one process. The
// lock file is created when missing and never deleted.
class FileLock {
public:
    enum class Mode { shared, exclusive };

    // Waits up to `timeout` (polling) for the lock.
    static Result<FileLock> acquire(const std::filesystem::path& path, Mode mode,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));
    // Returns nullopt at once when someone else holds a conflicting lock.
    static Result<std::optional<FileLock>> try_acquire(const std::filesystem::path& path, Mode mode);

    FileLock() = default;
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

    void release();
    bool held() const;

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace decomp
