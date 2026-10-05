#include "core/fs.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>

namespace decomp::fs {

stdfs::path from_utf8(std::string_view s) {
    return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string to_utf8(const stdfs::path& p) {
    auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

Result<std::vector<std::byte>> read_file(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return make_error(ErrorCode::io, "cannot open '{}'", to_utf8(path));
    auto size = in.tellg();
    if (size < 0) return make_error(ErrorCode::io, "cannot size '{}'", to_utf8(path));
    std::vector<std::byte> data(static_cast<usize>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size))
        return make_error(ErrorCode::io, "cannot read '{}'", to_utf8(path));
    return data;
}

Result<std::string> read_text(const stdfs::path& path) {
    TRY_ASSIGN(auto bytes, read_file(path));
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

namespace {

std::string unique_suffix() {
    static std::atomic<u64> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    thread_local std::mt19937_64 rng(std::random_device{}() ^ static_cast<u64>(now));
    return std::format("{:x}{:x}", rng() & 0xFFFFFFFF, counter.fetch_add(1));
}

Result<void> write_impl(const stdfs::path& path, const char* data, usize size, bool append) {
    std::error_code ec;
    if (path.has_parent_path()) stdfs::create_directories(path.parent_path(), ec);
    if (append) {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) return make_error(ErrorCode::io, "cannot open '{}' for append", to_utf8(path));
        out.write(data, static_cast<std::streamsize>(size));
        if (!out) return make_error(ErrorCode::io, "cannot append to '{}'", to_utf8(path));
        return {};
    }
    auto tmp = path;
    tmp += ".tmp-" + unique_suffix();
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return make_error(ErrorCode::io, "cannot create '{}'", to_utf8(tmp));
        out.write(data, static_cast<std::streamsize>(size));
        if (!out) return make_error(ErrorCode::io, "cannot write '{}'", to_utf8(tmp));
    }
    // On Windows a replace fails while another process (a reader, an indexer, a virus scanner) has the
    // target open; such failures are transient, so they are retried for a while.
    for (int attempt = 0;; ++attempt) {
        stdfs::rename(tmp, path, ec);
        if (!ec) return {};
        const bool transient = ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy ||
                               ec == std::errc::resource_unavailable_try_again;
        if (!transient || attempt >= 40) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10 * (attempt + 1), 100)));
    }
    std::error_code ignored;
    stdfs::remove(tmp, ignored);
    return make_error(ErrorCode::io, "cannot replace '{}': {}", to_utf8(path), ec.message());
}

} // namespace

Result<void> write_file(const stdfs::path& path, std::span<const std::byte> data) {
    return write_impl(path, reinterpret_cast<const char*>(data.data()), data.size(), false);
}

Result<void> write_text(const stdfs::path& path, std::string_view text) {
    return write_impl(path, text.data(), text.size(), false);
}

Result<void> append_text(const stdfs::path& path, std::string_view text) {
    return write_impl(path, text.data(), text.size(), true);
}

Result<void> create_directories(const stdfs::path& path) {
    std::error_code ec;
    stdfs::create_directories(path, ec);
    if (ec) return make_error(ErrorCode::io, "cannot create directory '{}': {}", to_utf8(path), ec.message());
    return {};
}

std::optional<stdfs::path> find_upwards(const stdfs::path& start, std::string_view name) {
    std::error_code ec;
    auto dir = stdfs::absolute(start, ec);
    if (ec) return std::nullopt;
    while (true) {
        if (stdfs::exists(dir / from_utf8(name), ec)) return dir;
        auto parent = dir.parent_path();
        if (parent == dir || parent.empty()) return std::nullopt;
        dir = parent;
    }
}

Result<TempDir> TempDir::create(std::string_view prefix, const stdfs::path& parent) {
    std::error_code ec;
    auto base = parent.empty() ? stdfs::temp_directory_path(ec) : parent;
    if (ec) return make_error(ErrorCode::io, "no temp directory: {}", ec.message());
    if (!parent.empty()) stdfs::create_directories(base, ec);
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto candidate = base / from_utf8(std::string(prefix) + "-" + unique_suffix());
        if (stdfs::create_directory(candidate, ec) && !ec) return TempDir(candidate);
    }
    return make_error(ErrorCode::io, "cannot create a temp directory under '{}'", to_utf8(base));
}

TempDir::TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }

TempDir& TempDir::operator=(TempDir&& other) noexcept {
    if (this != &other) {
        std::error_code ec;
        if (!path_.empty()) stdfs::remove_all(path_, ec);
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

TempDir::~TempDir() {
    if (!path_.empty()) {
        std::error_code ec;
        stdfs::remove_all(path_, ec);
    }
}

stdfs::path TempDir::release() {
    auto p = std::move(path_);
    path_.clear();
    return p;
}

} // namespace decomp::fs
