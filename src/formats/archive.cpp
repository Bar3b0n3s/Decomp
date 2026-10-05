#include "formats/archive.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"

#include <charconv>
#include <map>

namespace decomp::archive {

namespace {

constexpr usize kHeaderSize = 60;

std::string_view field(ByteSpan data, usize offset, usize size) {
    return trim(std::string_view(reinterpret_cast<const char*>(data.data()) + offset, size));
}

u32 read_be32(ByteSpan d, usize at) {
    return (static_cast<u32>(d[at]) << 24) | (static_cast<u32>(d[at + 1]) << 16) | (static_cast<u32>(d[at + 2]) << 8) |
           static_cast<u32>(d[at + 3]);
}

std::string c_string(ByteSpan d, usize& at) {
    std::string out;
    while (at < d.size() && d[at] != std::byte{0}) out += static_cast<char>(d[at++]);
    ++at;
    return out;
}

std::optional<ImportObject> parse_import(ByteSpan d) {
    if (d.size() < 20 || read_le<u16>(d, 0) != 0 || read_le<u16>(d, 2) != 0xFFFF) return std::nullopt;
    ImportObject o;
    o.machine = read_le<u16>(d, 6).value_or(0);
    o.ordinal_or_hint = read_le<u16>(d, 16).value_or(0);
    const u16 info = read_le<u16>(d, 18).value_or(0);
    o.type = static_cast<u8>(info & 3);
    o.name_type = static_cast<u8>((info >> 2) & 7);
    usize at = 20;
    o.symbol = c_string(d, at);
    o.dll = c_string(d, at);
    return o;
}

} // namespace

Result<Archive> Archive::load(const std::filesystem::path& path) {
    TRY_ASSIGN(auto bytes, fs::read_file(path));
    auto parsed = parse(ByteSpan(bytes));
    if (!parsed) return make_error(parsed.error().code, "{}: {}", fs::to_utf8(path), parsed.error().message);
    return parsed;
}

Result<Archive> Archive::parse(ByteSpan d) {
    if (d.size() < 8 || std::string_view(reinterpret_cast<const char*>(d.data()), 8) != "!<arch>\n")
        return make_error(ErrorCode::parse, "not a COFF archive (no !<arch> signature)");
    Archive a;
    std::string_view longnames;
    std::vector<std::pair<std::string, u32>> index;  // symbol -> member header offset
    bool have_ms_index = false;
    usize linker_members = 0;
    std::map<u64, usize> by_offset;
    for (usize at = 8; at + kHeaderSize <= d.size();) {
        const std::string_view name = field(d, at, 16);
        const std::string_view size_text = field(d, at + 48, 10);
        if (field(d, at + 58, 2) != "`") return make_error(ErrorCode::parse, "bad member header at offset {:#x}", at);
        u64 size = 0;
        if (auto [p, ec] = std::from_chars(size_text.data(), size_text.data() + size_text.size(), size); ec != std::errc{})
            return make_error(ErrorCode::parse, "bad member size at offset {:#x}", at);
        const usize begin = at + kHeaderSize;
        if (begin + size > d.size()) return make_error(ErrorCode::parse, "member at offset {:#x} runs past the end of the archive", at);
        const ByteSpan data = d.subspan(begin, static_cast<usize>(size));
        if (name == "/") {
            // The first linker member: big-endian offsets; the second (Microsoft's): little-endian, with
            // member indexes. Either lists every public symbol and its member.
            if (linker_members++ == 0 && data.size() >= 4) {
                const u32 count = read_be32(data, 0);
                usize names = 4 + 4 * static_cast<usize>(count);
                if (names <= data.size()) {
                    std::vector<std::pair<std::string, u32>> first;
                    for (u32 i = 0; i < count; ++i) first.emplace_back(c_string(data, names), read_be32(data, 4 + 4 * i));
                    if (!have_ms_index) index = std::move(first);
                }
            } else if (data.size() >= 8) {
                const u32 members = read_le<u32>(data, 0).value_or(0);
                const usize after_offsets = 4 + 4 * static_cast<usize>(members);
                const u32 symbols = read_le<u32>(data, after_offsets).value_or(0);
                usize names = after_offsets + 4 + 2 * static_cast<usize>(symbols);
                if (names <= data.size()) {
                    index.clear();
                    for (u32 i = 0; i < symbols; ++i) {
                        const u16 member = read_le<u16>(data, after_offsets + 4 + 2 * i).value_or(0);
                        const u32 offset = member >= 1 && member <= members ? read_le<u32>(data, 4 + 4 * (member - 1)).value_or(0) : 0;
                        index.emplace_back(c_string(data, names), offset);
                    }
                    have_ms_index = true;
                }
            }
        } else if (name == "//") {
            longnames = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
        } else {
            Member m;
            m.offset = at;
            if (name.starts_with('/') && name.size() > 1) {
                usize off = 0;
                std::from_chars(name.data() + 1, name.data() + name.size(), off);
                if (off < longnames.size()) {
                    std::string_view rest = longnames.substr(off);
                    rest = rest.substr(0, rest.find_first_of(std::string_view("\0\n", 2)));
                    if (rest.ends_with('/')) rest.remove_suffix(1);
                    m.name = std::string(rest);
                }
            } else {
                m.name = std::string(name.ends_with('/') ? name.substr(0, name.size() - 1) : name);
            }
            m.data.assign(data.begin(), data.end());
            m.import = parse_import(data);
            by_offset.emplace(at, a.members_.size());
            a.members_.push_back(std::move(m));
        }
        at = begin + static_cast<usize>(size) + (size & 1);
    }
    for (auto& [symbol, offset] : index)
        if (auto it = by_offset.find(offset); it != by_offset.end()) a.index_.emplace_back(std::move(symbol), it->second);
    return a;
}

} // namespace decomp::archive
