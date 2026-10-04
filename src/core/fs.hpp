#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::fs {

namespace stdfs = std::filesystem;

// UTF-8 <-> path conversions (std::filesystem uses UTF-16 on Windows).
stdfs::path from_utf8(std::string_view s);
std::string to_utf8(const stdfs::path& p);

Result<std::vector<std::byte>> read_file(const stdfs::path& path);
Result<std::string> read_text(const stdfs::path& path);

// Writes via a temporary sibling file + rename so readers never observe partial content.
Result<void> write_file(const stdfs::path& path, std::span<const std::byte> data);
Result<void> write_text(const stdfs::path& path, std::string_view text);
Result<void> append_text(const stdfs::path& path, std::string_view text);

Result<void> create_directories(const stdfs::path& path);

// Searches `start` and its parents for `name`; returns the directory containing it.
std::optional<stdfs::path> find_upwards(const stdfs::path& start, std::string_view name);

// A uniquely named directory removed (recursively) on destruction unless released.
class TempDir {
public:
    static Result<TempDir> create(std::string_view prefix = "decomp");

    TempDir(TempDir&& other) noexcept;
    TempDir& operator=(TempDir&& other) noexcept;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir();

    const stdfs::path& path() const { return path_; }
    stdfs::path release();

private:
    explicit TempDir(stdfs::path path) : path_(std::move(path)) {}
    stdfs::path path_;
};

} // namespace decomp::fs
