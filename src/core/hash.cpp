#include "core/hash.hpp"

#include <bit>
#include <cstring>
#include <format>

namespace decomp {

Sha1::Sha1() : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u} {}

void Sha1::process_block(const u8* block) {
    u32 w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) | (u32(block[i * 4 + 2]) << 8) | u32(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        u32 t = std::rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = std::rotl(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(ByteSpan data) {
    auto p = reinterpret_cast<const u8*>(data.data());
    usize n = data.size();
    total_len_ += n;
    while (n > 0) {
        usize take = std::min<usize>(64 - buffer_len_, n);
        std::memcpy(buffer_.data() + buffer_len_, p, take);
        buffer_len_ += take;
        p += take;
        n -= take;
        if (buffer_len_ == 64) {
            process_block(buffer_.data());
            buffer_len_ = 0;
        }
    }
}

Sha1Digest Sha1::finish() {
    u64 bit_len = total_len_ * 8;
    u8 pad = 0x80;
    update(as_bytes(&pad, 1));
    u8 zero = 0;
    while (buffer_len_ != 56) update(as_bytes(&zero, 1));
    u8 len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = static_cast<u8>(bit_len >> (56 - 8 * i));
    update(as_bytes(len_be, 8));
    Sha1Digest out;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<u8>(h_[i] >> (24 - 8 * j));
    return out;
}

Sha1Digest sha1(ByteSpan data) {
    Sha1 s;
    s.update(data);
    return s.finish();
}

std::string to_hex(const Sha1Digest& digest) {
    std::string out;
    for (u8 b : digest) out += std::format("{:02x}", b);
    return out;
}

std::string sha1_hex(ByteSpan data) { return to_hex(sha1(data)); }
std::string sha1_hex(std::string_view text) { return to_hex(sha1(as_bytes(text.data(), text.size()))); }

} // namespace decomp
