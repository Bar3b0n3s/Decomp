#pragma once

#include "core/bytes.hpp"
#include "core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

enum class Arch : u8 { x86, x64 };

constexpr std::string_view to_string(Arch arch) { return arch == Arch::x86 ? "x86" : "x64"; }
constexpr unsigned pointer_size(Arch arch) { return arch == Arch::x86 ? 4u : 8u; }

struct ImageSection {
    std::string name;
    u64 va = 0;          // absolute virtual address
    u64 virtual_size = 0;
    u64 file_size = 0;   // bytes backed by file data (the rest reads as zero)
    bool executable = false;
    bool writable = false;
    bool readable = true;

    bool contains(u64 address) const { return address >= va && address < va + std::max(virtual_size, file_size); }
};

// Format-agnostic view of a linked executable image (PE today, ELF later).
class BinaryImage {
public:
    virtual ~BinaryImage() = default;

    virtual Arch arch() const = 0;
    virtual u64 image_base() const = 0;
    virtual u64 image_size() const = 0;
    virtual u64 entry_point() const = 0;  // absolute VA
    virtual const std::vector<ImageSection>& image_sections() const = 0;

    // Contiguous file-backed bytes at `va` (nullopt when not fully backed by the file).
    virtual std::optional<ByteSpan> view(u64 va, usize size) const = 0;

    // Whether the image carries relocation info (PE: .reloc present and not stripped).
    virtual bool has_relocations() const = 0;
    // Whether a base relocation applies to the field starting at `va`.
    virtual bool is_relocated(u64 va) const = 0;

    const ImageSection* section_at(u64 va) const {
        for (const auto& s : image_sections())
            if (s.contains(va)) return &s;
        return nullptr;
    }
    bool contains(u64 va) const { return section_at(va) != nullptr; }
    bool is_code(u64 va) const {
        auto s = section_at(va);
        return s && s->executable;
    }
    bool is_readonly_data(u64 va) const {
        auto s = section_at(va);
        return s && !s->executable && !s->writable;
    }

    template <class T>
    std::optional<T> read(u64 va) const {
        auto v = view(va, sizeof(T));
        if (!v) return std::nullopt;
        return read_le<T>(*v, 0);
    }
    std::optional<std::string> read_cstring(u64 va, usize max_len = 4096) const {
        auto s = section_at(va);
        if (!s) return std::nullopt;
        u64 avail = s->va + s->file_size > va ? s->va + s->file_size - va : 0;
        auto v = view(va, static_cast<usize>(std::min<u64>(avail, max_len + 1)));
        if (!v) return std::nullopt;
        return read_cstring_at(*v, 0, max_len);
    }
};

} // namespace decomp
