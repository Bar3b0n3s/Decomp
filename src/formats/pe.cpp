#include "formats/pe.hpp"

#include "core/fs.hpp"

#include <algorithm>
#include <map>
#include <format>

namespace decomp::pe {
namespace {

constexpr u32 kMaxSections = 96;
constexpr usize kMaxImports = 1 << 20;

enum DataDirectory : u32 {
    dir_export = 0,
    dir_import = 1,
    dir_exception = 3,
    dir_basereloc = 5,
    dir_debug = 6,
};

} // namespace

std::string CodeViewInfo::guid_string() const {
    auto d1 = read_le<u32>(as_bytes(guid.data(), 16), 0).value_or(0);
    auto d2 = read_le<u16>(as_bytes(guid.data(), 16), 4).value_or(0);
    auto d3 = read_le<u16>(as_bytes(guid.data(), 16), 6).value_or(0);
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", d1, d2, d3, guid[8],
                       guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

Result<Image> Image::load(const std::filesystem::path& path) {
    TRY_ASSIGN(auto data, fs::read_file(path));
    auto image = parse(std::move(data));
    if (!image) return std::unexpected(std::move(image.error()).with_context(fs::to_utf8(path)));
    return image;
}

Result<Image> Image::parse(std::vector<std::byte> data) {
    Image image;
    image.data_ = std::move(data);
    TRY(image.parse_headers());
    return image;
}

Result<void> Image::parse_headers() {
    ByteSpan d = data_;
    if (read_le<u16>(d, 0) != 0x5A4D) return make_error(ErrorCode::parse, "not a PE image (missing MZ signature)");
    auto pe_offset = read_le<u32>(d, 0x3C);
    if (!pe_offset || read_le<u32>(d, *pe_offset) != 0x00004550u)
        return make_error(ErrorCode::parse, "not a PE image (missing PE signature)");
    rich_ = parse_rich_header(data_, *pe_offset);

    ByteReader r(d, *pe_offset + 4);
    TRY_ASSIGN(machine_, r.read<u16>());
    TRY_ASSIGN(u16 section_count, r.read<u16>());
    TRY_ASSIGN(timestamp_, r.read<u32>());
    TRY(r.skip(8));  // symbol table pointer + count (unused in images)
    TRY_ASSIGN(u16 optional_size, r.read<u16>());
    TRY_ASSIGN(characteristics_, r.read<u16>());
    if (machine_ != machine::i386 && machine_ != machine::amd64)
        return make_error(ErrorCode::unsupported, "unsupported machine type {:#06x} (only x86 and x64 are supported)", machine_);
    if (section_count > kMaxSections) return make_error(ErrorCode::parse, "implausible section count {}", section_count);

    usize opt = r.tell();
    auto magic = read_le<u16>(d, opt);
    if (magic == 0x10B) pe32_plus_ = false;
    else if (magic == 0x20B) pe32_plus_ = true;
    else return make_error(ErrorCode::parse, "unknown optional header magic {:#x}", magic.value_or(0));
    linker_major_ = read_le<u8>(d, opt + 2).value_or(0);
    linker_minor_ = read_le<u8>(d, opt + 3).value_or(0);
    entry_rva_ = read_le<u32>(d, opt + 16).value_or(0);
    image_base_ = pe32_plus_ ? read_le<u64>(d, opt + 24).value_or(0) : read_le<u32>(d, opt + 28).value_or(0);
    size_of_image_ = read_le<u32>(d, opt + 56).value_or(0);
    size_of_headers_ = read_le<u32>(d, opt + 60).value_or(0);
    subsystem_ = read_le<u16>(d, opt + 68).value_or(0);
    dll_characteristics_ = read_le<u16>(d, opt + 70).value_or(0);
    usize dir_count_offset = opt + (pe32_plus_ ? 108 : 92);
    u32 dir_count = std::min<u32>(read_le<u32>(d, dir_count_offset).value_or(0), 16);
    std::array<std::pair<u32, u32>, 16> dirs{};
    for (u32 i = 0; i < dir_count; ++i) {
        usize off = dir_count_offset + 4 + i * 8;
        dirs[i] = {read_le<u32>(d, off).value_or(0), read_le<u32>(d, off + 4).value_or(0)};
    }

    ByteReader sr(d, opt + optional_size);
    for (u16 i = 0; i < section_count; ++i) {
        SectionHeader s;
        TRY_ASSIGN(auto name_bytes, sr.read_bytes(8));
        for (auto b : name_bytes) {
            if (b == std::byte{0}) break;
            s.name.push_back(static_cast<char>(b));
        }
        TRY_ASSIGN(s.virtual_size, sr.read<u32>());
        TRY_ASSIGN(s.virtual_address, sr.read<u32>());
        TRY_ASSIGN(s.raw_size, sr.read<u32>());
        TRY_ASSIGN(s.raw_offset, sr.read<u32>());
        TRY(sr.skip(12));
        TRY_ASSIGN(s.characteristics, sr.read<u32>());
        sections_.push_back(s);

        ImageSection is;
        is.name = s.name;
        is.va = image_base_ + s.virtual_address;
        is.virtual_size = s.virtual_size ? s.virtual_size : s.raw_size;
        u64 backed = s.raw_offset < data_.size() ? std::min<u64>(s.raw_size, data_.size() - s.raw_offset) : 0;
        is.file_size = std::min<u64>(backed, is.virtual_size);
        is.executable = (s.characteristics & scn::mem_execute) != 0;
        is.writable = (s.characteristics & scn::mem_write) != 0;
        is.readable = (s.characteristics & scn::mem_read) != 0;
        image_sections_.push_back(is);
    }

    if (dirs[dir_export].first) TRY(parse_exports(dirs[dir_export].first, dirs[dir_export].second));
    if (dirs[dir_import].first) TRY(parse_imports(dirs[dir_import].first));
    if (dirs[dir_basereloc].first) TRY(parse_base_relocations(dirs[dir_basereloc].first, dirs[dir_basereloc].second));
    if (dirs[dir_debug].first) TRY(parse_debug_directory(dirs[dir_debug].first, dirs[dir_debug].second));
    if (dirs[dir_exception].first && machine_ == machine::amd64)
        TRY(parse_pdata(dirs[dir_exception].first, dirs[dir_exception].second));
    return {};
}

const SectionHeader* Image::section_for_rva(u32 rva) const {
    for (const auto& s : sections_) {
        u32 size = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.virtual_address && rva - s.virtual_address < size) return &s;
    }
    return nullptr;
}

std::optional<u32> Image::rva_to_offset(u32 rva) const {
    if (rva < size_of_headers_ && rva < data_.size()) return rva;
    auto s = section_for_rva(rva);
    if (!s) return std::nullopt;
    u32 delta = rva - s->virtual_address;
    if (delta >= s->raw_size) return std::nullopt;
    u64 off = u64(s->raw_offset) + delta;
    if (off >= data_.size()) return std::nullopt;
    return static_cast<u32>(off);
}

std::optional<ByteSpan> Image::view(u64 va, usize size) const {
    if (va < image_base_) return std::nullopt;
    u64 rva64 = va - image_base_;
    if (rva64 > 0xFFFFFFFFu) return std::nullopt;
    u32 rva = static_cast<u32>(rva64);
    auto off = rva_to_offset(rva);
    if (!off) return std::nullopt;
    // The whole range must be file-backed and inside one section (or the headers).
    if (auto s = section_for_rva(rva)) {
        u64 end_in_section = u64(rva - s->virtual_address) + size;
        if (end_in_section > s->raw_size) return std::nullopt;
    }
    if (u64(*off) + size > data_.size()) return std::nullopt;
    return ByteSpan(data_).subspan(*off, size);
}

std::optional<std::vector<std::byte>> Image::read_rva(u32 rva, usize size) const {
    auto s = section_for_rva(rva);
    if (!s) return std::nullopt;
    u32 delta = rva - s->virtual_address;
    u64 section_size = std::max(s->virtual_size, s->raw_size);
    if (u64(delta) + size > section_size) return std::nullopt;
    std::vector<std::byte> out(size, std::byte{0});
    for (usize i = 0; i < size; ++i) {
        u64 pos = u64(delta) + i;
        if (pos < s->raw_size && u64(s->raw_offset) + pos < data_.size()) out[i] = data_[s->raw_offset + pos];
    }
    return out;
}

bool Image::is_relocated(u64 va) const {
    if (va < image_base_) return false;
    u64 rva = va - image_base_;
    auto it = std::lower_bound(base_relocations_.begin(), base_relocations_.end(), rva,
                               [](const BaseRelocation& r, u64 v) { return r.rva < v; });
    return it != base_relocations_.end() && it->rva == rva && it->type != 0;
}

std::optional<std::string> Image::read_cstring_rva(u32 rva) const {
    auto off = rva_to_offset(rva);
    if (!off) return std::nullopt;
    return read_cstring_at(data_, *off, 1024);
}

Result<void> Image::parse_exports(u32 rva, u32 size) {
    auto off = rva_to_offset(rva);
    if (!off) return make_error(ErrorCode::parse, "export directory outside the image");
    ByteSpan d = data_;
    u32 base = read_le<u32>(d, *off + 16).value_or(0);
    u32 function_count = read_le<u32>(d, *off + 20).value_or(0);
    u32 name_count = read_le<u32>(d, *off + 24).value_or(0);
    u32 functions_rva = read_le<u32>(d, *off + 28).value_or(0);
    u32 names_rva = read_le<u32>(d, *off + 32).value_or(0);
    u32 ordinals_rva = read_le<u32>(d, *off + 36).value_or(0);
    if (function_count > 65536 || name_count > 65536) return make_error(ErrorCode::parse, "implausible export counts");

    std::vector<std::string> names(function_count);
    for (u32 i = 0; i < name_count; ++i) {
        auto name_ptr_off = rva_to_offset(names_rva + i * 4);
        auto ord_off = rva_to_offset(ordinals_rva + i * 2);
        if (!name_ptr_off || !ord_off) break;
        u32 name_rva = read_le<u32>(d, *name_ptr_off).value_or(0);
        u16 index = read_le<u16>(d, *ord_off).value_or(0);
        if (index < function_count) names[index] = read_cstring_rva(name_rva).value_or("");
    }
    for (u32 i = 0; i < function_count; ++i) {
        auto fn_off = rva_to_offset(functions_rva + i * 4);
        if (!fn_off) break;
        u32 fn_rva = read_le<u32>(d, *fn_off).value_or(0);
        if (fn_rva == 0) continue;
        Export e;
        e.name = names[i];
        e.ordinal = base + i;
        e.rva = fn_rva;
        if (fn_rva >= rva && fn_rva < rva + size) e.forwarder = read_cstring_rva(fn_rva);
        exports_.push_back(std::move(e));
    }
    return {};
}

Result<void> Image::parse_imports(u32 rva) {
    ByteSpan d = data_;
    const unsigned ptr = pe32_plus_ ? 8 : 4;
    for (u32 desc = rva;; desc += 20) {
        auto off = rva_to_offset(desc);
        if (!off) return make_error(ErrorCode::parse, "import descriptor outside the image");
        u32 ilt = read_le<u32>(d, *off).value_or(0);
        u32 name_rva = read_le<u32>(d, *off + 12).value_or(0);
        u32 iat = read_le<u32>(d, *off + 16).value_or(0);
        if (ilt == 0 && name_rva == 0 && iat == 0) break;
        std::string dll = read_cstring_rva(name_rva).value_or("?");
        u32 lookup = ilt ? ilt : iat;
        for (u32 i = 0;; ++i) {
            if (imports_.size() > kMaxImports) return make_error(ErrorCode::parse, "too many imports");
            auto entry_off = rva_to_offset(lookup + i * ptr);
            if (!entry_off) break;
            u64 entry = pe32_plus_ ? read_le<u64>(d, *entry_off).value_or(0) : read_le<u32>(d, *entry_off).value_or(0);
            if (entry == 0) break;
            Import imp;
            imp.dll = dll;
            imp.iat_va = image_base_ + iat + u64(i) * ptr;
            bool by_ordinal = pe32_plus_ ? (entry >> 63) != 0 : (entry >> 31) != 0;
            if (by_ordinal) imp.ordinal = static_cast<u16>(entry & 0xFFFF);
            else imp.name = read_cstring_rva(static_cast<u32>(entry & 0x7FFFFFFF) + 2).value_or("");
            imports_.push_back(std::move(imp));
        }
    }
    return {};
}

Result<void> Image::parse_base_relocations(u32 rva, u32 size) {
    ByteSpan d = data_;
    u32 pos = 0;
    while (pos + 8 <= size) {
        auto off = rva_to_offset(rva + pos);
        if (!off) break;
        u32 page = read_le<u32>(d, *off).value_or(0);
        u32 block_size = read_le<u32>(d, *off + 4).value_or(0);
        if (block_size < 8) break;
        for (u32 i = 8; i + 2 <= block_size; i += 2) {
            u16 entry = read_le<u16>(d, *off + i).value_or(0);
            u8 type = static_cast<u8>(entry >> 12);
            if (type == 0) continue;  // padding
            base_relocations_.push_back({page + (entry & 0xFFFu), type});
        }
        pos += block_size;
    }
    std::ranges::sort(base_relocations_, {}, &BaseRelocation::rva);
    return {};
}

Result<void> Image::parse_debug_directory(u32 rva, u32 size) {
    ByteSpan d = data_;
    for (u32 pos = 0; pos + 28 <= size; pos += 28) {
        auto off = rva_to_offset(rva + pos);
        if (!off) break;
        u32 type = read_le<u32>(d, *off + 12).value_or(0);
        u32 data_size = read_le<u32>(d, *off + 16).value_or(0);
        u32 data_off = read_le<u32>(d, *off + 24).value_or(0);
        if (type != 2 || data_size < 16 || u64(data_off) + data_size > data_.size()) continue;  // 2 = CODEVIEW
        CodeViewInfo cv;
        u32 sig = read_le<u32>(d, data_off).value_or(0);
        if (sig == 0x53445352) {  // "RSDS"
            cv.signature = "RSDS";
            for (int i = 0; i < 16; ++i) cv.guid[i] = static_cast<u8>(data_[data_off + 4 + i]);
            cv.age = read_le<u32>(d, data_off + 20).value_or(0);
            cv.pdb_path = read_cstring_at(d, data_off + 24, data_size).value_or("");
        } else if (sig == 0x3031424E) {  // "NB10"
            cv.signature = "NB10";
            cv.nb10_signature = read_le<u32>(d, data_off + 8).value_or(0);
            cv.age = read_le<u32>(d, data_off + 12).value_or(0);
            cv.pdb_path = read_cstring_at(d, data_off + 16, data_size).value_or("");
        } else {
            continue;
        }
        codeview_ = std::move(cv);
        break;
    }
    return {};
}

Result<void> Image::parse_pdata(u32 rva, u32 size) {
    ByteSpan d = data_;
    for (u32 pos = 0; pos + 12 <= size; pos += 12) {
        auto off = rva_to_offset(rva + pos);
        if (!off) break;
        RuntimeFunction f{read_le<u32>(d, *off).value_or(0), read_le<u32>(d, *off + 4).value_or(0),
                          read_le<u32>(d, *off + 8).value_or(0)};
        if (f.begin_rva == 0 && f.end_rva == 0) break;
        runtime_functions_.push_back(f);
    }
    // Chained unwind data: the entry's UNWIND_INFO has UNW_FLAG_CHAININFO and ends with the
    // RUNTIME_FUNCTION it continues (after the unwind codes, padded to an even count). Some linkers point
    // UnwindData straight at that RUNTIME_FUNCTION instead, with the low bit set. Chains are followed to
    // the entry they start from.
    auto parent_of = [&](const RuntimeFunction& f) -> std::optional<u32> {
        if (f.unwind_rva & 1) {
            auto off = rva_to_offset(f.unwind_rva & ~1u);
            if (!off) return std::nullopt;
            return read_le<u32>(d, *off).value_or(0);
        }
        auto off = rva_to_offset(f.unwind_rva);
        if (!off) return std::nullopt;
        const u8 version_flags = read_le<u8>(d, *off).value_or(0);
        const u8 codes = read_le<u8>(d, *off + 2).value_or(0);
        if ((version_flags >> 3) != 0x4) return std::nullopt;  // flags == UNW_FLAG_CHAININFO
        const u32 chained = *off + 4 + 2 * ((codes + 1u) & ~1u);
        return read_le<u32>(d, chained).value_or(0);
    };
    std::map<u32, u32> parent;  // begin -> begin of the entry it continues
    for (const auto& f : runtime_functions_)
        if (auto p = parent_of(f); p && *p && *p != f.begin_rva) parent[f.begin_rva] = *p;
    for (auto& f : runtime_functions_) {
        u32 root = f.begin_rva;
        for (int hop = 0; hop < 32; ++hop) {
            auto it = parent.find(root);
            if (it == parent.end()) break;
            root = it->second;
        }
        if (root != f.begin_rva) f.chained_to = root;
    }
    return {};
}

const std::vector<RichEntry>& Image::rich_entries() const {
    static const std::vector<RichEntry> none;
    return rich_ ? rich_->entries : none;
}

std::optional<BuildInfo> Image::build_info() const {
    if (!rich_) return std::nullopt;
    return identify_build(*rich_, linker_major_, linker_minor_);
}

} // namespace decomp::pe
