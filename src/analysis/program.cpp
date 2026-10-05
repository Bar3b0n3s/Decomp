#include "analysis/program.hpp"

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "analysis/demangle.hpp"
#include "analysis/discovery.hpp"
#include "formats/map.hpp"
#include "formats/pdb.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <format>
#include <set>

namespace decomp {

std::string_view to_string(XrefKind kind) {
    switch (kind) {
    case XrefKind::call: return "call";
    case XrefKind::jump: return "jump";
    case XrefKind::read: return "read";
    case XrefKind::address: return "address";
    case XrefKind::write: return "write";
    case XrefKind::pointer: return "pointer";
    }
    return "?";
}


std::set<std::pair<u64, usize>> image_relative_fields(const BinaryImage& image, std::span<const x86::Instruction> list) {
    std::set<std::pair<u64, usize>> out;
    if (image.arch() != Arch::x64) return out;
    const u64 base = image.image_base();
    std::set<std::string> holders;  // registers currently holding the image base
    for (const auto& ins : list) {
        for (const auto& op : ins.operands) {
            if (op.kind != x86::OperandKind::mem || !op.mem.has_disp || op.mem.field < 0) continue;
            const auto& field = ins.fields[static_cast<usize>(op.mem.field)];
            if (field.rip_relative || field.size != 4 || field.raw <= 0) continue;
            const bool via_base = !op.mem.base.empty() && holders.contains(x86::gpr_family(op.mem.base));
            const bool via_index = !op.mem.index.empty() && op.mem.scale == 1 && holders.contains(x86::gpr_family(op.mem.index));
            if ((via_base || via_index) && image.section_at(base + static_cast<u64>(field.raw)))
                out.emplace(ins.address, static_cast<usize>(op.mem.field));
        }
        // Update what the registers hold after this instruction.
        if (ins.mnemonic == "lea" && ins.operands.size() == 2 && ins.operands[0].kind == x86::OperandKind::reg &&
            ins.operands[1].kind == x86::OperandKind::mem && ins.operands[1].mem.field >= 0) {
            const auto& f = ins.fields[static_cast<usize>(ins.operands[1].mem.field)];
            const std::string reg = x86::gpr_family(ins.operands[0].reg);
            if (f.rip_relative && f.absolute == base && ins.operands[1].mem.index.empty()) holders.insert(reg);
            else holders.erase(reg);
            continue;
        }
        if (ins.flow == x86::Flow::call || ins.flow == x86::Flow::indirect_call) {
            for (const char* r : {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11"}) holders.erase(r);
            continue;
        }
        static const std::set<std::string_view> no_write = {"cmp", "test", "bt", "push"};
        if (!ins.operands.empty() && ins.operands[0].kind == x86::OperandKind::reg && !no_write.contains(ins.mnemonic))
            holders.erase(x86::gpr_family(ins.operands[0].reg));
        if (ins.mnemonic == "xchg" && ins.operands.size() == 2 && ins.operands[1].kind == x86::OperandKind::reg)
            holders.erase(x86::gpr_family(ins.operands[1].reg));
        static const std::set<std::string_view> rax_rdx = {"mul", "div", "idiv", "cdq", "cqo", "cwd"};
        if (rax_rdx.contains(ins.mnemonic) || (ins.mnemonic == "imul" && ins.operands.size() == 1)) {
            holders.erase("rax");
            holders.erase("rdx");
        }
    }
    return out;
}

std::string_view to_string(PdbStatus status) {
    switch (status) {
    case PdbStatus::matched: return "matched";
    case PdbStatus::mismatch: return "mismatch";
    case PdbStatus::unsupported: return "unsupported";
    case PdbStatus::absent: return "absent";
    }
    return "absent";
}

Program Program::with_symbols(SymbolDb symbols) const {
    Program p;
    p.path_ = path_;
    p.pdb_path_ = pdb_path_;
    p.image_ = image_;
    p.decoder_ = decoder_;
    p.symbols_ = std::move(symbols);
    p.pdb_status_ = pdb_status_;
    p.pdb_detail_ = pdb_detail_;
    return p;
}

Result<Program> Program::open(const std::filesystem::path& binary, const OpenOptions& options) {
    const std::optional<std::filesystem::path>& pdb_path = options.pdb;
    Program p;
    p.path_ = binary;
    TRY_ASSIGN(auto image, pe::Image::load(binary));
    p.image_ = std::make_unique<pe::Image>(std::move(image));
    p.decoder_ = std::make_unique<x86::Decoder>(p.image_->arch());

    std::vector<std::filesystem::path> candidates;
    if (!options.use_pdb) {
        // Opened as if there were no PDB.
    } else if (pdb_path) {
        candidates.push_back(*pdb_path);
    } else {
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
            p.pdb_status_ = PdbStatus::unsupported;
            p.pdb_detail_ = loaded.error().message;
            continue;
        }
        const auto& cv = p.image_->codeview();
        if (cv && cv->signature == "RSDS" && !loaded->matches(cv->guid, cv->age)) {
            log::warn("ignoring PDB '{}': GUID/age does not match the image", fs::to_utf8(candidate));
            p.pdb_status_ = PdbStatus::mismatch;
            p.pdb_detail_ = std::format("'{}' belongs to a different build (GUID/age)", fs::to_utf8(candidate));
            continue;
        }
        reader = std::make_unique<pdb::Reader>(std::move(*loaded));
        p.pdb_path_ = candidate;
        p.pdb_status_ = PdbStatus::matched;
        p.pdb_detail_.clear();
        break;
    }
    if (pdb_path && !reader) return make_error(ErrorCode::not_found, "PDB '{}' could not be used", fs::to_utf8(*pdb_path));
    p.symbols_ = SymbolDb::from_pe(*p.image_, reader.get());
    p.fold_linker_thunks();
    if (options.map) {
        TRY_ASSIGN(auto named, p.add_map(*options.map));
        log::debug("map: {} symbols from '{}'", named, fs::to_utf8(*options.map));
    }
    if (options.discover && p.pdb_status_ != PdbStatus::matched) p.add_discovered_functions();
    return p;
}

Result<usize> Program::add_map(const std::filesystem::path& map_path) {
    TRY_ASSIGN(auto m, map::load(map_path));
    if (auto ok = map::check_image(m, *image_); !ok) return make_error(ok.error().code, "{}: {}", fs::to_utf8(map_path), ok.error().message);
    if (m.timestamp && *m.timestamp && image_->timestamp() && *m.timestamp != image_->timestamp())
        log::warn("{}: written for a link at another time (timestamp {:08x}, the image's is {:08x})", fs::to_utf8(map_path), *m.timestamp,
                  image_->timestamp());
    return symbols_.add_map(m, *image_);
}

void Program::add_discovered_functions() {
    const DiscoveryResult found = discover_functions(*image_, *decoder_, symbols_);
    for (const auto& f : found.functions) {
        Symbol s;
        s.va = f.start;
        s.size = static_cast<u32>(f.end - f.start);
        s.kind = SymbolKind::function;
        s.source = SymbolSource::analysis;
        if (!symbols_.at(f.start)) {
            s.name = std::format("sub_{:x}", f.start);
            // An import thunk is named after the import it jumps to.
            if (const Symbol* slot = f.import_slot ? symbols_.at(f.import_slot) : nullptr; slot && slot->name.starts_with("__imp_"))
                s.name = slot->name.substr(6);
        }
        symbols_.add(std::move(s));
    }
    log::debug("discovery: {} functions in {} passes ({} instructions)", found.functions.size(), found.passes, found.instructions);
}

std::optional<x86::Instruction> Program::decode_at(u64 va) const {
    const ImageSection* sec = image_->section_at(va);
    if (!sec || !sec->executable) return std::nullopt;
    const u64 avail = sec->va + std::max(sec->virtual_size, sec->file_size) - va;
    auto bytes = image_->view(va, static_cast<usize>(std::min<u64>(15, avail)));
    if (!bytes) return std::nullopt;
    return decoder_->decode(*bytes, va);
}

// With incremental linking the entry point and the exports lead to ILT entries (`jmp rel32` into the
// real function). Their names move to the destination, so they name the code the compiler produced;
// the thunk stays unnamed and calls through it resolve via thunk_destination().
void Program::fold_linker_thunks() {
    auto is_jmp_rel32 = [&](u64 va) {
        auto b = image_->read<u8>(va);
        return b && *b == 0xE9 && image_->is_code(va);
    };
    std::vector<std::pair<u64, Symbol>> moves;
    for (const auto& [va, s] : symbols_) {
        if (s.kind != SymbolKind::function) continue;
        if (s.source != SymbolSource::export_table && s.source != SymbolSource::analysis) continue;
        auto ins = decode_at(va);
        if (!ins || ins->flow != x86::Flow::jump || !ins->branch_target || ins->length != 5) continue;
        const u64 dest = *ins->branch_target;
        if (dest == va || !image_->is_code(dest)) continue;
        const Symbol* d = symbols_.at(dest);
        if (d && d->kind != SymbolKind::function) continue;
        // Only an ILT entry (a table of 5-byte jumps) or a jump to the same function under its PDB name;
        // an ordinary exported function whose body is a tail call stays where it is.
        const bool in_table = is_jmp_rel32(va - 5) || is_jmp_rel32(va + 5);
        const bool same_name = d && (names_equivalent(d->name, s.name) || (!d->pdb_name.empty() && names_equivalent(d->pdb_name, s.name)));
        if (!in_table && !same_name) continue;
        Symbol moved = s;
        moved.va = dest;
        moved.size = 0;
        moves.emplace_back(va, std::move(moved));
    }
    for (auto& [from, symbol] : moves) {
        log::debug("{} at {:#x} is a linker thunk; naming its destination {:#x}", symbol.name, from, symbol.va);
        symbols_.remove(from);
        symbols_.add(std::move(symbol));
    }
}

std::optional<u64> Program::resolve(std::string_view text) const {
    text = trim(text);
    if (auto value = parse_u64(text); value && (text.starts_with("0x") || text.starts_with("0X") || text.ends_with('h') ||
                                                image_->contains(*value)))
        return *value;
    if (auto s = symbols_.find(text)) return s->va;
    return std::nullopt;
}

std::optional<JumpTable> Program::jump_table(std::span<const x86::Instruction> before, const x86::Instruction& jmp, u64 fn_start,
                                            u64 fn_limit) const {
    const JumpTableContext ctx{.image = image_.get(), .symbols = &symbols_, .fn_start = fn_start, .fn_limit = fn_limit};
    return read_jump_table(ctx, before, jmp);
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
        for (usize i = 0; i < list.size(); ++i) {
            const auto& ins = list[i];
            if (ins.flow != x86::Flow::indirect_jump) continue;
            if (auto table = jump_table(std::span<const x86::Instruction>(list).first(i), ins, start, ext.end)) {
                if (table->table_va >= start && table->table_va < ext.end) {
                    table->inside_code = true;
                    for (const auto& range : table->data_ranges()) ext.data_ranges.push_back(range);
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
        std::vector<x86::Instruction> recent;
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
                recent.push_back(*ins);
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
                    if (auto table = jump_table(recent, *ins, start, limit)) {
                        for (u64 t : table->targets)
                            if (image_->read<u8>(t).value_or(0xCC) != 0xCC) work.push_back(t);  // int3: a case that cannot happen
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
                for (const auto& range : table.data_ranges()) ext.data_ranges.push_back(range);
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

std::vector<Xref> Program::xrefs_in(const FunctionExtent& ext, std::span<const x86::Instruction> list) const {
    std::vector<Xref> out;
    const auto rva_fields = image_relative_fields(*image_, list);
    for (const auto& ins : list) {
        if (ins.branch_target && !ext.contains(*ins.branch_target)) {
            auto kind = ins.flow == x86::Flow::call ? XrefKind::call : XrefKind::jump;
            out.push_back({ins.address, ext.start, kind, *ins.branch_target});
        }
        for (usize i = 0; i < ins.fields.size(); ++i) {
            const auto& f = ins.fields[i];
            if (f.kind == x86::FieldKind::rel) continue;
            u64 target = f.absolute;
            bool address_like = f.rip_relative || image_->is_relocated(ins.address + f.offset) ||
                                (!image_->has_relocations() && f.size >= 4 && image_->contains(f.absolute));
            if (!address_like && rva_fields.contains({ins.address, i})) {
                target = image_->image_base() + static_cast<u64>(f.raw);
                address_like = true;
            }
            if (!address_like || !image_->contains(target)) continue;
            auto kind = XrefKind::address;
            if (f.kind == x86::FieldKind::disp) {
                const bool stored = f.operand >= 0 && static_cast<usize>(f.operand) < ins.operands.size() &&
                                    ins.operands[static_cast<usize>(f.operand)].write;
                kind = stored ? XrefKind::write : XrefKind::read;
            }
            if (ins.flow == x86::Flow::indirect_call) kind = XrefKind::call;
            out.push_back({ins.address, ext.start, kind, target});
        }
    }
    return out;
}

std::vector<Xref> Program::xrefs_from(u64 function_va) const {
    auto ext = function_extent(function_va);
    if (!ext) return {};
    auto list = function_instructions(*ext);
    if (!list) return {};
    return xrefs_in(*ext, *list);
}

void Program::build_xrefs() const {
    std::unordered_map<u64, std::optional<u64>> thunks;  // target -> what is behind it, when a thunk
    auto add = [&](const Xref& x) {
        xrefs_[x.to].push_back(x);
        if (x.kind == XrefKind::read || x.kind == XrefKind::write || !image_->is_code(x.to)) return;
        auto it = thunks.find(x.to);
        if (it == thunks.end()) it = thunks.emplace(x.to, linker_thunk_target(x.to)).first;
        if (!it->second || *it->second == x.to) return;
        Xref through = x;
        through.via = x.to;
        through.to = *it->second;
        xrefs_[through.to].push_back(through);
    };
    for (const auto* fn : symbols_.functions()) {
        if (fn->size == 0 || !image_->is_code(fn->va)) continue;
        auto ext = function_extent(fn->va);
        if (!ext) continue;
        auto list = function_instructions(*ext);
        if (!list) continue;
        for (const auto& x : xrefs_in(*ext, *list)) add(x);
    }
    for (const auto& [where, value] : data_pointers()) add(Xref{.from = where, .function = 0, .kind = XrefKind::pointer, .to = value, .via = 0});
}

std::vector<std::pair<u64, u64>> Program::data_pointers() const {
    std::vector<std::pair<u64, u64>> out;
    const unsigned size = pointer_size(image_->arch());
    auto read_pointer = [&](u64 va) -> std::optional<u64> {
        if (size == 8) return image_->read<u64>(va);
        return image_->read<u32>(va).transform([](u32 v) { return u64{v}; });
    };
    if (image_->has_relocations()) {
        for (const auto& r : image_->base_relocations()) {
            const u64 where = image_->image_base() + r.rva;
            if (image_->is_code(where)) continue;  // in instructions and switch tables: the functions say
            if (auto value = read_pointer(where); value && image_->contains(*value)) out.emplace_back(where, *value);
        }
        return out;
    }
    for (const auto& s : image_->image_sections()) {
        if (s.executable || s.file_size == 0) continue;
        for (u64 va = s.va; va + size <= s.va + s.file_size; va += size)
            if (auto value = read_pointer(va); value && *value >= image_->image_base() && image_->contains(*value)) out.emplace_back(va, *value);
    }
    return out;
}

std::optional<u64> Program::linker_thunk_target(u64 va) const {
    auto ins = decode_at(va);
    if (!ins) return std::nullopt;
    if (ins->flow == x86::Flow::indirect_jump && ins->memory_target) {
        if (const Symbol* slot = symbols_.at(*ins->memory_target); slot && slot->kind == SymbolKind::import) return *ins->memory_target;
        return std::nullopt;
    }
    if (const Symbol* s = symbols_.at(va); s && s->kind == SymbolKind::function) return std::nullopt;
    return thunk_destination(va);
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

std::optional<u64> Program::thunk_destination(u64 va) const {
    u64 at = va;
    for (int hop = 0; hop < 4; ++hop) {
        auto ins = decode_at(at);
        if (!ins) return std::nullopt;
        if (ins->flow == x86::Flow::jump && ins->branch_target) {
            const u64 dest = *ins->branch_target;
            if (const Symbol* s = symbols_.at(dest)) {
                if (s->kind == SymbolKind::function) return dest;
                return std::nullopt;
            }
            at = dest;  // chained thunk
            continue;
        }
        if (ins->flow == x86::Flow::indirect_jump && ins->memory_target)
            if (const Symbol* s = symbols_.at(*ins->memory_target); s && s->kind == SymbolKind::import) return *ins->memory_target;
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace decomp
