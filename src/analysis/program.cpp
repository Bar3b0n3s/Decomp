#include "analysis/program.hpp"

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "formats/pdb.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace decomp {

std::string_view to_string(XrefKind kind) {
    switch (kind) {
    case XrefKind::call: return "call";
    case XrefKind::jump: return "jump";
    case XrefKind::read: return "read";
    case XrefKind::address: return "address";
    }
    return "?";
}

Result<Program> Program::open(const std::filesystem::path& binary, const std::optional<std::filesystem::path>& pdb_path) {
    Program p;
    p.path_ = binary;
    TRY_ASSIGN(auto image, pe::Image::load(binary));
    p.image_ = std::make_unique<pe::Image>(std::move(image));
    p.decoder_ = std::make_unique<x86::Decoder>(p.image_->arch());

    std::vector<std::filesystem::path> candidates;
    if (pdb_path) candidates.push_back(*pdb_path);
    else {
        auto dir = binary.parent_path();
        if (const auto& cv = p.image_->codeview(); cv && !cv->pdb_path.empty()) {
            auto recorded = fs::from_utf8(replace_all(cv->pdb_path, "\\", "/"));
            candidates.push_back(dir / recorded.filename());
        }
        auto stem_pdb = binary;
        stem_pdb.replace_extension(".pdb");
        candidates.push_back(stem_pdb);
    }

    std::unique_ptr<pdb::Reader> reader;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        auto loaded = pdb::Reader::load(candidate);
        if (!loaded) {
            log::warn("ignoring PDB: {}", loaded.error().message);
            continue;
        }
        const auto& cv = p.image_->codeview();
        if (cv && cv->signature == "RSDS" && !loaded->matches(cv->guid, cv->age)) {
            log::warn("ignoring PDB '{}': GUID/age does not match the image", fs::to_utf8(candidate));
            continue;
        }
        reader = std::make_unique<pdb::Reader>(std::move(*loaded));
        p.pdb_path_ = candidate;
        break;
    }
    if (pdb_path && !reader) return make_error(ErrorCode::not_found, "PDB '{}' could not be used", fs::to_utf8(*pdb_path));
    p.symbols_ = SymbolDb::from_pe(*p.image_, reader.get());
    return p;
}

std::optional<u64> Program::resolve(std::string_view text) const {
    text = trim(text);
    if (auto value = parse_u64(text); value && (text.starts_with("0x") || text.starts_with("0X") || text.ends_with('h') ||
                                                image_->contains(*value)))
        return *value;
    if (auto s = symbols_.find(text)) return s->va;
    return std::nullopt;
}

std::optional<JumpTable> Program::read_jump_table(const x86::Instruction& jmp, u64 fn_start, u64 fn_limit) const {
    // x86 switch dispatch: jmp dword ptr [reg*4 + table]
    if (jmp.flow != x86::Flow::indirect_jump || jmp.operands.empty()) return std::nullopt;
    const auto& op = jmp.operands[0];
    if (op.kind != x86::OperandKind::mem || !op.mem.base.empty() || op.mem.index.empty()) return std::nullopt;
    unsigned entry = pointer_size(arch());
    if (op.mem.scale != entry || !jmp.memory_target) return std::nullopt;
    JumpTable table;
    table.jump_va = jmp.address;
    table.table_va = *jmp.memory_target;
    table.entry_size = entry;
    for (unsigned i = 0; i < 4096; ++i) {
        u64 slot = table.table_va + u64(i) * entry;
        auto value = entry == 4 ? image_->read<u32>(slot).transform([](u32 v) { return u64(v); }) : image_->read<u64>(slot);
        if (!value || *value < fn_start || *value >= fn_limit || !image_->is_code(*value)) break;
        if (i > 0 && symbols_.at(slot) && symbols_.at(slot)->kind != SymbolKind::label) break;  // next object
        table.targets.push_back(*value);
    }
    if (table.targets.empty()) return std::nullopt;
    return table;
}

Result<FunctionExtent> Program::function_extent(u64 start) const {
    if (!image_->is_code(start)) return make_error(ErrorCode::invalid_argument, "{:#x} is not in an executable section", start);
    FunctionExtent ext;
    ext.start = start;
    const Symbol* sym = symbols_.at(start);
    const ImageSection* section = image_->section_at(start);
    u64 section_end = section->va + section->file_size;

    if (sym && sym->size > 0) {
        ext.end = std::min<u64>(start + sym->size, section_end);
        ext.from_symbol = true;
        auto list = decoder_->decode_all(*image_->view(start, ext.end - start), start);
        for (const auto& ins : list) {
            if (auto table = read_jump_table(ins, start, ext.end)) {
                if (table->table_va >= start && table->table_va < ext.end) {
                    table->inside_code = true;
                    ext.data_ranges.emplace_back(table->table_va, table->table_va + table->targets.size() * table->entry_size);
                }
                ext.jump_tables.push_back(std::move(*table));
            }
        }
    } else {
        // Recursive descent bounded by the section and by other known function starts.
        u64 limit = section_end;
        if (auto next = symbols_.next_function_after(start)) limit = std::min(limit, next->va);
        std::set<u64> visited;
        std::vector<u64> work{start};
        u64 max_end = start;
        while (!work.empty()) {
            u64 addr = work.back();
            work.pop_back();
            while (addr >= start && addr < limit && !visited.contains(addr)) {
                auto bytes = image_->view(addr, static_cast<usize>(std::min<u64>(15, limit - addr)));
                if (!bytes) break;
                auto ins = decoder_->decode(*bytes, addr);
                if (!ins) break;
                visited.insert(addr);
                max_end = std::max(max_end, ins->end());
                bool stop = false;
                switch (ins->flow) {
                case x86::Flow::cond_jump:
                    if (ins->branch_target) work.push_back(*ins->branch_target);
                    break;
                case x86::Flow::jump:
                    if (ins->branch_target && *ins->branch_target >= start && *ins->branch_target < limit)
                        work.push_back(*ins->branch_target);
                    stop = true;
                    break;
                case x86::Flow::indirect_jump:
                    if (auto table = read_jump_table(*ins, start, limit)) {
                        for (u64 t : table->targets) work.push_back(t);
                        ext.jump_tables.push_back(std::move(*table));
                    }
                    stop = true;
                    break;
                case x86::Flow::ret:
                case x86::Flow::trap:
                case x86::Flow::halt: stop = true; break;
                default: break;
                }
                if (stop) break;
                addr = ins->end();
            }
        }
        ext.end = std::max(max_end, start + 1);
        for (auto& table : ext.jump_tables) {
            if (table.table_va >= start && table.table_va < ext.end) {
                table.inside_code = true;
                ext.data_ranges.emplace_back(table.table_va, table.table_va + table.targets.size() * table.entry_size);
            }
        }
    }
    std::ranges::sort(ext.data_ranges);
    return ext;
}

Result<std::vector<x86::Instruction>> Program::function_instructions(const FunctionExtent& ext) const {
    std::vector<x86::Instruction> out;
    u64 pos = ext.start;
    usize next_data = 0;
    while (pos < ext.end) {
        while (next_data < ext.data_ranges.size() && ext.data_ranges[next_data].second <= pos) ++next_data;
        u64 stop = ext.end;
        if (next_data < ext.data_ranges.size()) {
            if (ext.data_ranges[next_data].first <= pos) {
                pos = ext.data_ranges[next_data].second;
                continue;
            }
            stop = ext.data_ranges[next_data].first;
        }
        auto bytes = image_->view(pos, stop - pos);
        if (!bytes) return make_error(ErrorCode::invalid_argument, "function bytes at {:#x} are not file-backed", pos);
        auto chunk = decoder_->decode_all(*bytes, pos);
        out.insert(out.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
        pos = stop;
    }
    return out;
}

Result<std::vector<x86::Instruction>> Program::function_instructions(u64 start) const {
    TRY_ASSIGN(auto ext, function_extent(start));
    return function_instructions(ext);
}

void Program::build_xrefs() const {
    for (const auto* fn : symbols_.functions()) {
        if (fn->size == 0 || !image_->is_code(fn->va)) continue;
        auto ext = function_extent(fn->va);
        if (!ext) continue;
        auto list = function_instructions(*ext);
        if (!list) continue;
        for (const auto& ins : *list) {
            if (ins.branch_target && !ext->contains(*ins.branch_target)) {
                auto kind = ins.flow == x86::Flow::call ? XrefKind::call : XrefKind::jump;
                xrefs_[*ins.branch_target].push_back({ins.address, fn->va, kind});
            }
            for (const auto& f : ins.fields) {
                if (f.kind == x86::FieldKind::rel) continue;
                bool address_like = f.rip_relative || image_->is_relocated(ins.address + f.offset) ||
                                    (!image_->has_relocations() && f.size >= 4 && image_->contains(f.absolute));
                if (!address_like || !image_->contains(f.absolute)) continue;
                auto kind = f.kind == x86::FieldKind::disp ? XrefKind::read : XrefKind::address;
                if (ins.flow == x86::Flow::indirect_call) kind = XrefKind::call;
                xrefs_[f.absolute].push_back({ins.address, fn->va, kind});
            }
        }
    }
}

std::vector<Xref> Program::xrefs_to(u64 target) const {
    std::call_once(*xref_once_, [this] { build_xrefs(); });
    auto it = xrefs_.find(target);
    return it == xrefs_.end() ? std::vector<Xref>{} : it->second;
}

std::vector<u64> Program::callers_of(u64 target) const {
    std::vector<u64> out;
    for (const auto& x : xrefs_to(target))
        if ((x.kind == XrefKind::call || x.kind == XrefKind::jump) && x.function &&
            std::ranges::find(out, x.function) == out.end())
            out.push_back(x.function);
    return out;
}

std::string Program::describe_address(u64 va) const {
    if (auto s = symbols_.at(va)) return s->display.empty() ? s->name : s->display;
    if (auto s = symbols_.containing(va)) return std::format("{}+{:#x}", s->display.empty() ? s->name : s->display, va - s->va);
    return std::format("{:#x}", va);
}

} // namespace decomp
