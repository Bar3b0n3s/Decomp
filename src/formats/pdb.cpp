#include "formats/pdb.hpp"

#include "core/fs.hpp"

#include <PDB.h>
#include <PDB_DBIStream.h>
#include <PDB_InfoStream.h>
#include <PDB_NamesStream.h>
#include <PDB_RawFile.h>
#include <PDB_TPITypes.h>

#include <cstring>

namespace decomp::pdb {
namespace {

std::string array_to_string(const PDB::ArrayView<char>& view) {
    std::string out(view.Decay(), view.GetLength());
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// A type stream's records (the TPI stream is 2, the IPI stream 4): the stream after its header. nullopt
// when the stream is missing or of a version whose records do not read (before Visual C++ 7.0).
std::optional<codeview::TypeStream> read_type_stream(const PDB::RawFile& raw, u32 index) {
    if (index >= raw.GetStreamCount()) return std::nullopt;
    const auto stream = raw.CreateMSFStream<PDB::DirectMSFStream>(index);
    if (stream.GetSize() < sizeof(PDB::TPI::StreamHeader)) return std::nullopt;
    const auto header = stream.ReadAtOffset<PDB::TPI::StreamHeader>(0);
    using Version = PDB::TPI::StreamHeader::Version;
    if (header.version != Version::V70 && header.version != Version::V80) return std::nullopt;
    if (header.headerSize < sizeof(header) || header.headerSize > stream.GetSize()) return std::nullopt;
    const usize size = std::min<usize>(header.typeRecordBytes, stream.GetSize() - header.headerSize);
    std::vector<std::byte> bytes(size);
    if (size > 0) stream.ReadAtOffset(bytes.data(), size, header.headerSize);
    auto types = codeview::TypeStream::parse(std::move(bytes), header.typeIndexBegin);
    if (!types) return std::nullopt;
    return std::move(*types);
}

} // namespace

Result<Reader> Reader::load(const std::filesystem::path& path) {
    TRY_ASSIGN(auto bytes, fs::read_file(path));
    auto context = fs::to_utf8(path);
    if (PDB::ValidateFile(bytes.data(), bytes.size()) != PDB::ErrorCode::Success)
        return make_error(ErrorCode::parse, "{}: not a PDB 7.0 file (older PDB 2.0 files are not supported yet)", context);

    const PDB::RawFile raw = PDB::CreateRawFile(bytes.data());
    if (PDB::HasValidDBIStream(raw) != PDB::ErrorCode::Success)
        return make_error(ErrorCode::parse, "{}: PDB has no valid DBI stream", context);

    Reader reader;
    const PDB::InfoStream info_stream(raw);
    if (const auto* header = info_stream.GetHeader()) {
        std::memcpy(reader.info_.guid.data(), &header->guid, 16);
        reader.info_.age = header->age;
        reader.info_.signature = header->signature;
    }
    if (auto types = read_type_stream(raw, 2)) reader.types_ = std::move(*types);
    // S_GPROC32_ID and S_LPROC32_ID name an item (LF_FUNC_ID, LF_MFUNC_ID) that names the function type.
    const auto items = info_stream.HasIPIStream() ? read_type_stream(raw, 4) : std::nullopt;
    // Where each type was defined: LF_UDT_SRC_LINE names the file with an LF_STRING_ID, LF_UDT_MOD_SRC_LINE
    // with an offset in the /names stream.
    if (items) {
        std::optional<PDB::NamesStream> names;
        if (info_stream.HasNamesStream()) names.emplace(info_stream.CreateNamesStream(raw));
        for (codeview::TypeIndex i = items->first(); i < items->end(); ++i) {
            const auto record = items->record(i);
            if (!record || record->data.size() < 12 ||
                (record->leaf != codeview::leaf::udt_src_line && record->leaf != codeview::leaf::udt_mod_src_line))
                continue;
            u32 udt = 0, file = 0;
            std::memcpy(&udt, record->data.data(), 4);
            std::memcpy(&file, record->data.data() + 4, 4);
            if (record->leaf == codeview::leaf::udt_src_line) {
                if (auto text = items->string_id(file)) reader.type_sources_.emplace(udt, std::move(*text));
            } else if (names && names->GetHeader() && file < names->GetHeader()->size) {
                reader.type_sources_.emplace(udt, std::string(names->GetFilename(file)));
            }
        }
    }

    const PDB::DBIStream dbi = PDB::CreateDBIStream(raw);
    if (dbi.HasValidImageSectionStream(raw) != PDB::ErrorCode::Success ||
        dbi.HasValidSymbolRecordStream(raw) != PDB::ErrorCode::Success ||
        dbi.HasValidPublicSymbolStream(raw) != PDB::ErrorCode::Success ||
        dbi.HasValidGlobalSymbolStream(raw) != PDB::ErrorCode::Success)
        return make_error(ErrorCode::parse, "{}: PDB is missing required DBI streams", context);

    const PDB::ImageSectionStream sections = dbi.CreateImageSectionStream(raw);
    const PDB::ModuleInfoStream module_stream = dbi.CreateModuleInfoStream(raw);
    const PDB::CoalescedMSFStream symbol_records = dbi.CreateSymbolRecordStream(raw);
    using Kind = PDB::CodeView::DBI::SymbolRecordKind;

    const auto modules = module_stream.GetModules();
    // The file info substream: a module count, two 16-bit arrays, then offsets and names (empty in
    // some PDBs).
    const PDB::SourceFileStream source_files =
        dbi.GetHeader().sourceInfoSize >= 4 ? dbi.CreateSourceFileStream(raw) : PDB::SourceFileStream();
    // A module's files follow those of the modules before it. The substream's per-module start indices
    // cannot be trusted (LLVM writes each module's own number there), so the starts are summed from the
    // per-module counts; module 0's files start the array under either convention.
    const u32* file_offsets = source_files.GetModuleCount() > 0 ? source_files.GetModuleFilenameOffsets(0).Decay() : nullptr;
    usize next_file = 0;
    u32 module_index = 0;
    for (const auto& module : modules) {
        Module m{array_to_string(module.GetName()), array_to_string(module.GetObjectName()), {}, -1, 0, {}, false, false};
        if (module_index < source_files.GetModuleCount()) {
            const usize count = source_files.GetModuleFilenameOffsets(module_index).GetLength();
            for (usize f = 0; f < count; ++f) m.source_files.emplace_back(source_files.GetFilename(file_offsets[next_file + f]));
            next_file += count;
        }
        reader.modules_.push_back(std::move(m));
        if (module.HasSymbolStream()) {
            const auto symbols = module.CreateSymbolStream(raw);
            symbols.ForEachSymbol([&](const PDB::CodeView::DBI::Record* record) {
                auto kind = record->header.kind;
                if (kind == Kind::S_COFFGROUP) {
                    const auto& g = record->data.S_COFFGROUP;
                    const u32 rva = sections.ConvertSectionOffsetToRVA(g.section, g.offset);
                    if (rva) reader.coff_groups_.push_back({g.name, rva, g.size, g.characteristics});
                } else if (kind == Kind::S_COMPILE3) {
                    reader.modules_.back().language =
                        static_cast<int>(PDB_AS_UNDERLYING(record->data.S_COMPILE3.flags) & 0xFFu);
                    reader.modules_.back().backend_build = record->data.S_COMPILE3.versionBackendBuild;
                    reader.modules_.back().compiler = record->data.S_COMPILE3.version;
                    const u32 flags = PDB_AS_UNDERLYING(record->data.S_COMPILE3.flags);
                    reader.modules_.back().security_checks = (flags & (1u << 13)) != 0;
                    reader.modules_.back().sdl = (flags & (1u << 17)) != 0;
                } else if (kind == Kind::S_GPROC32 || kind == Kind::S_LPROC32 || kind == Kind::S_GPROC32_ID ||
                    kind == Kind::S_LPROC32_ID) {
                    // All four records share the same layout.
                    const auto& p = record->data.S_GPROC32;
                    u32 rva = sections.ConvertSectionOffsetToRVA(p.section, p.offset);
                    if (rva == 0) return;
                    u32 type_index = p.typeIndex;
                    if (kind == Kind::S_GPROC32_ID || kind == Kind::S_LPROC32_ID)
                        type_index = items ? items->function_type_of_id(p.typeIndex).value_or(0) : 0;
                    reader.procedures_.push_back({p.name, rva, p.codeSize, kind == Kind::S_GPROC32 || kind == Kind::S_GPROC32_ID,
                                                  module_index, type_index});
                } else if (kind == Kind::S_LDATA32 || kind == Kind::S_GDATA32) {
                    const auto& v = record->data.S_LDATA32;
                    u32 rva = sections.ConvertSectionOffsetToRVA(v.section, v.offset);
                    if (rva == 0) return;
                    reader.data_.push_back({v.name, rva, v.typeIndex, kind == Kind::S_GDATA32, module_index});
                }
            });
        }
        ++module_index;
    }

    // Global data lives in the global symbol stream (module streams only carry statics).
    const PDB::GlobalSymbolStream globals = dbi.CreateGlobalSymbolStream(raw);
    for (const auto& hash : globals.GetRecords()) {
        const auto* record = globals.GetRecord(symbol_records, hash);
        if (record->header.kind != Kind::S_GDATA32 && record->header.kind != Kind::S_LDATA32) continue;
        const auto& v = record->data.S_GDATA32;
        u32 rva = sections.ConvertSectionOffsetToRVA(v.section, v.offset);
        if (rva == 0) continue;
        bool duplicate = false;
        for (const auto& existing : reader.data_)
            if (existing.rva == rva && existing.name == v.name) duplicate = true;
        if (!duplicate) reader.data_.push_back({v.name, rva, v.typeIndex, record->header.kind == Kind::S_GDATA32, 0});
    }

    const PDB::PublicSymbolStream publics = dbi.CreatePublicSymbolStream(raw);
    for (const auto& hash : publics.GetRecords()) {
        const auto* record = publics.GetRecord(symbol_records, hash);
        if (record->header.kind != Kind::S_PUB32) continue;
        const auto& p = record->data.S_PUB32;
        u32 rva = sections.ConvertSectionOffsetToRVA(p.section, p.offset);
        if (rva == 0) continue;
        bool is_function = (PDB_AS_UNDERLYING(p.flags) & PDB_AS_UNDERLYING(PDB::CodeView::DBI::PublicSymbolFlags::Function)) != 0;
        reader.publics_.push_back({p.name, rva, is_function});
    }

    if (dbi.HasValidSectionContributionStream(raw) == PDB::ErrorCode::Success) {
        const auto contribution_stream = dbi.CreateSectionContributionStream(raw);
        for (const auto& c : contribution_stream.GetContributions()) {
            u32 rva = sections.ConvertSectionOffsetToRVA(c.section, c.offset);
            if (rva == 0) continue;
            reader.contributions_.push_back({rva, c.size, c.moduleIndex, c.characteristics});
        }
    }
    return reader;
}

} // namespace decomp::pdb
