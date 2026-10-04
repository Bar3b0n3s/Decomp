#include "formats/pdb.hpp"

#include "core/fs.hpp"

#include <PDB.h>
#include <PDB_DBIStream.h>
#include <PDB_InfoStream.h>
#include <PDB_RawFile.h>

#include <cstring>

namespace decomp::pdb {
namespace {

std::string array_to_string(const PDB::ArrayView<char>& view) {
    std::string out(view.Decay(), view.GetLength());
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
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
    u32 module_index = 0;
    for (const auto& module : modules) {
        reader.modules_.push_back({array_to_string(module.GetName()), array_to_string(module.GetObjectName())});
        if (module.HasSymbolStream()) {
            const auto symbols = module.CreateSymbolStream(raw);
            symbols.ForEachSymbol([&](const PDB::CodeView::DBI::Record* record) {
                auto kind = record->header.kind;
                if (kind == Kind::S_GPROC32 || kind == Kind::S_LPROC32 || kind == Kind::S_GPROC32_ID ||
                    kind == Kind::S_LPROC32_ID) {
                    // All four records share the same layout.
                    const auto& p = record->data.S_GPROC32;
                    u32 rva = sections.ConvertSectionOffsetToRVA(p.section, p.offset);
                    if (rva == 0) return;
                    reader.procedures_.push_back({p.name, rva, p.codeSize,
                                                  kind == Kind::S_GPROC32 || kind == Kind::S_GPROC32_ID, module_index});
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
