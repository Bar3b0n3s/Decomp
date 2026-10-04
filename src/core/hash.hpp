#pragma once

#include "core/bytes.hpp"

#include <array>
#include <string>
#include <string_view>

namespace decomp {

using Sha1Digest = std::array<u8, 20>;

class Sha1 {
public:
    Sha1();
    void update(ByteSpan data);
    void update(std::string_view text) { update(as_bytes(text.data(), text.size())); }
    Sha1Digest finish();

private:
    void process_block(const u8* block);
    std::array<u32, 5> h_;
    std::array<u8, 64> buffer_{};
    usize buffer_len_ = 0;
    u64 total_len_ = 0;
};

Sha1Digest sha1(ByteSpan data);
std::string sha1_hex(ByteSpan data);
std::string sha1_hex(std::string_view text);
std::string to_hex(const Sha1Digest& digest);

} // namespace decomp
