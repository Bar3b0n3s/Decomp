#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace decomp {

using ByteSpan = std::span<const std::byte>;

inline ByteSpan as_bytes(const void* data, usize size) { return {static_cast<const std::byte*>(data), size}; }

// Little-endian read of a trivially copyable integer/float at `offset`; nullopt when out of range.
template <class T>
std::optional<T> read_le(ByteSpan data, usize offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > data.size() || data.size() - offset < sizeof(T)) return std::nullopt;
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    if constexpr (std::endian::native == std::endian::big && std::is_integral_v<T> && sizeof(T) > 1)
        value = std::byteswap(value);
    return value;
}

// Little-endian write of an integer at `offset`; false when it does not fit.
template <class T>
bool write_le(std::span<std::byte> data, usize offset, T value) {
    static_assert(std::is_integral_v<T>);
    if (offset > data.size() || data.size() - offset < sizeof(T)) return false;
    if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) value = std::byteswap(value);
    std::memcpy(data.data() + offset, &value, sizeof(T));
    return true;
}

// Appends an integer in little-endian order.
template <class T>
void append_le(std::vector<std::byte>& out, T value) {
    out.resize(out.size() + sizeof(T));
    write_le<T>(out, out.size() - sizeof(T), value);
}

inline void append_bytes(std::vector<std::byte>& out, ByteSpan data) { out.insert(out.end(), data.begin(), data.end()); }
inline void append_string(std::vector<std::byte>& out, std::string_view text) {
    append_bytes(out, as_bytes(text.data(), text.size()));
}

// Sequential bounds-checked reader used by the binary format parsers.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data, usize offset = 0) : data_(data), pos_(offset) {}

    template <class T>
    Result<T> read() {
        auto v = read_le<T>(data_, pos_);
        if (!v) return make_error(ErrorCode::parse, "unexpected end of data at offset {:#x} (need {} bytes)", pos_, sizeof(T));
        pos_ += sizeof(T);
        return *v;
    }

    Result<ByteSpan> read_bytes(usize count) {
        if (pos_ > data_.size() || data_.size() - pos_ < count)
            return make_error(ErrorCode::parse, "unexpected end of data at offset {:#x} (need {} bytes)", pos_, count);
        auto out = data_.subspan(pos_, count);
        pos_ += count;
        return out;
    }

    // Reads a NUL-terminated string (the terminator is consumed, not returned).
    Result<std::string> read_cstring(usize max_len = 1 << 20) {
        std::string out;
        while (true) {
            if (pos_ >= data_.size()) return make_error(ErrorCode::parse, "unterminated string at offset {:#x}", pos_);
            char c = static_cast<char>(data_[pos_++]);
            if (c == '\0') return out;
            if (out.size() >= max_len) return make_error(ErrorCode::parse, "string too long at offset {:#x}", pos_);
            out.push_back(c);
        }
    }

    Result<void> skip(usize count) {
        if (pos_ > data_.size() || data_.size() - pos_ < count)
            return make_error(ErrorCode::parse, "cannot skip {} bytes at offset {:#x}", count, pos_);
        pos_ += count;
        return {};
    }

    void seek(usize offset) { pos_ = offset; }
    usize tell() const { return pos_; }
    usize remaining() const { return pos_ < data_.size() ? data_.size() - pos_ : 0; }
    ByteSpan data() const { return data_; }

private:
    ByteSpan data_;
    usize pos_;
};

// Reads a NUL-terminated string at `offset` without a reader (nullopt if unterminated within `max_len`).
inline std::optional<std::string> read_cstring_at(ByteSpan data, usize offset, usize max_len = 4096) {
    std::string out;
    for (usize i = offset; i < data.size() && out.size() <= max_len; ++i) {
        char c = static_cast<char>(data[i]);
        if (c == '\0') return out;
        out.push_back(c);
    }
    return std::nullopt;
}

} // namespace decomp
