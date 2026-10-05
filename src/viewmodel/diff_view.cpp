#include "viewmodel/diff_view.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <numeric>
#include <set>
#include <tuple>

namespace decomp::vm {

using matching::FunctionDiff;
using matching::OperandDiff;
using matching::RefKind;
using matching::Row;
using matching::RowKind;

namespace {

const matching::Side& side_of(const FunctionDiff& diff, DiffSide side) { return side == DiffSide::target ? diff.target : diff.candidate; }

std::optional<usize> index_of(const Row& row, DiffSide side) { return side == DiffSide::target ? row.target : row.candidate; }

usize field_index(const x86::Instruction& ins, const x86::Field& f) { return static_cast<usize>(&f - ins.fields.data()); }

bool soft_operands(const Row& row) {
    return !row.operands.empty() &&
           std::ranges::all_of(row.operands, [](const auto& p) { return p.second == OperandDiff::reg || p.second == OperandDiff::stack; });
}

// "L12" -> 12; "off+1f" and anything else -> nullopt.
std::optional<usize> label_index(std::string_view key) {
    if (key.size() < 2 || key[0] != 'L') return std::nullopt;
    usize v = 0;
    for (char c : key.substr(1)) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<usize>(c - '0');
    }
    return v;
}

bool is_data_kind(RefKind k) {
    return k == RefKind::string || k == RefKind::wide_string || k == RefKind::float32 || k == RefKind::float64 || k == RefKind::vector ||
           k == RefKind::table || k == RefKind::index_table;
}

// A jump table key's entries ("L3,L7,off+2a") as the labels the listing shows.
std::vector<std::string> table_labels(const matching::Side& side, std::string_view key) {
    std::vector<std::string> out;
    for (auto part : split(key, ',')) {
        if (auto i = label_index(part); i && *i < side.instructions.size())
            out.push_back(std::format("loc_{:x}", side.instructions[*i].ins.address - side.address));
        else
            out.emplace_back(part);
    }
    return out;
}

// The target's bytes at an address shown the way the candidate's reference of kind `kind` is shown.
std::string target_data_at(const Program& program, u64 va, RefKind kind) {
    const auto& image = program.image();
    switch (kind) {
    case RefKind::string:
        if (auto s = image.read_cstring(va, 4096)) return escape_c_string(truncate_utf8(*s, 48));
        break;
    case RefKind::float32:
        if (auto v = image.read<u32>(va)) return std::format("{}f", std::bit_cast<float>(*v));
        break;
    case RefKind::float64:
        if (auto v = image.read<u64>(va)) return std::format("{}", std::bit_cast<double>(*v));
        break;
    case RefKind::vector:
        if (auto bytes = image.view(va, 16)) return "const:" + hex_bytes(reinterpret_cast<const u8*>(bytes->data()), bytes->size(), "");
        break;
    default: break;
    }
    return {};
}

std::string ref_text(const matching::Ref& ref) {
    if (ref.kind == RefKind::table) return std::format("switch_table ({} cases)", split(ref.key, ',').size());
    if (ref.kind == RefKind::index_table) return std::format("switch_index ({} bytes)", ref.key.size() / 2);
    return ref.display;
}

} // namespace

char row_glyph(const Row& row) {
    switch (row.kind) {
    case RowKind::equal: return '=';
    case RowKind::encoding: return 'e';
    case RowKind::operand:
        return std::ranges::any_of(row.operands, [](const auto& p) { return p.second == OperandDiff::symbol; }) ? '@' : '~';
    case RowKind::opcode: return '!';
    case RowKind::insert: return '+';
    case RowKind::del: return '-';
    }
    return '?';
}

RowKind display_kind(const Row& row, bool fuzzy) {
    if (fuzzy && row.kind == RowKind::operand && soft_operands(row)) return RowKind::equal;
    return row.kind;
}

std::vector<DisplayRow> layout_rows(const FunctionDiff& diff, const DiffViewOptions& options) {
    const usize n = diff.rows.size();
    std::vector<RowKind> kinds(n);
    for (usize i = 0; i < n; ++i) kinds[i] = display_kind(diff.rows[i], options.fuzzy);
    std::vector<bool> show(n, !options.differing_only);
    if (options.differing_only) {
        for (usize i = 0; i < n; ++i) {
            if (kinds[i] == RowKind::equal) continue;
            const usize lo = i >= options.context ? i - options.context : 0;
            const usize hi = std::min(n - 1, i + options.context);
            for (usize j = lo; j <= hi; ++j) show[j] = true;
        }
    }
    std::vector<DisplayRow> rows;
    for (usize i = 0; i < n; ++i) {
        if (!show[i]) continue;
        DisplayRow r;
        r.row = static_cast<u32>(i);
        r.kind = kinds[i];
        r.glyph = r.kind == RowKind::equal ? '=' : row_glyph(diff.rows[i]);
        r.gap_before = i > 0 && !show[i - 1];
        rows.push_back(r);
    }
    return rows;
}

std::optional<usize> next_difference(std::span<const DisplayRow> rows, std::optional<usize> from, bool forward) {
    const usize n = rows.size();
    if (n == 0) return std::nullopt;
    for (usize step = 1; step <= n; ++step) {
        usize i = 0;
        if (forward) i = from ? (*from + step) % n : step - 1;
        else i = from ? (*from + n - (step % n)) % n : n - step;
        if (rows[i].differs()) return i;
    }
    return std::nullopt;
}

std::optional<usize> display_index(std::span<const DisplayRow> rows, usize row) {
    auto it = std::ranges::lower_bound(rows, static_cast<u32>(row), {}, &DisplayRow::row);
    if (it == rows.end() || it->row != row) return std::nullopt;
    return static_cast<usize>(it - rows.begin());
}

std::string SideCell::text() const {
    std::string out = mnemonic;
    for (usize i = 0; i < operands.size(); ++i) {
        out += i == 0 ? " " : ", ";
        out += operands[i].text;
    }
    return out;
}

SideCell side_cell(const FunctionDiff& diff, const Row& row, DiffSide side, TextMode mode) {
    SideCell cell;
    const auto idx = index_of(row, side);
    const matching::Side& s = side_of(diff, side);
    if (!idx || *idx >= s.instructions.size()) return cell;
    const matching::SideInstruction& si = s.instructions[*idx];
    cell.present = true;
    cell.instruction = *idx;
    cell.offset = si.ins.address - s.address;
    cell.bytes = hex_bytes(si.ins.bytes.data(), si.ins.length);
    cell.mnemonic = si.ins.prefix + si.ins.mnemonic;
    x86::FieldRenderer names;
    if (mode == TextMode::normalized)
        names = [&si](const x86::Instruction& in, const x86::Field& f) -> std::optional<std::string> {
            const usize fi = field_index(in, f);
            if (fi < si.refs.size() && si.refs[fi]) return si.refs[fi]->display;
            return std::nullopt;
        };
    const auto texts = x86::render_operands(si.ins, names);
    cell.operands.resize(texts.size());
    for (usize i = 0; i < texts.size(); ++i) cell.operands[i].text = texts[i];
    for (usize f = 0; f < si.ins.fields.size() && f < si.refs.size(); ++f) {
        const i8 op = si.ins.fields[f].operand;
        if (si.refs[f] && op >= 0 && static_cast<usize>(op) < cell.operands.size()) cell.operands[static_cast<usize>(op)].has_ref = true;
    }
    for (const auto& [op, kind] : row.operands) {
        if (op < cell.operands.size()) cell.operands[op].diff = kind;
        // A different number of operands is reported at the first missing one: every extra operand differs.
        if (kind == OperandDiff::mem)
            for (usize i = op; i < cell.operands.size(); ++i)
                if (!cell.operands[i].diff) cell.operands[i].diff = kind;
    }
    return cell;
}

std::vector<FieldMark> field_marks(const matching::SideInstruction& si) {
    std::vector<FieldMark> out;
    for (usize f = 0; f < si.ins.fields.size() && f < si.refs.size(); ++f) {
        if (!si.refs[f]) continue;
        const auto& field = si.ins.fields[f];
        out.push_back(FieldMark{field.offset, field.size, field.operand, si.refs[f]->kind, si.refs[f]->display});
    }
    return out;
}

std::vector<BranchArrow> branch_arrows(const FunctionDiff& diff, std::span<const DisplayRow> rows, DiffSide side) {
    const matching::Side& s = side_of(diff, side);
    const usize n = s.instructions.size();
    std::vector<std::optional<usize>> shown(n);
    for (usize d = 0; d < rows.size(); ++d)
        if (auto idx = index_of(diff.rows[rows[d].row], side); idx && *idx < n) shown[*idx] = d;

    std::vector<BranchArrow> arrows;
    std::set<std::tuple<usize, usize, bool>> seen;
    auto add = [&](usize from_row, usize source, usize dest, bool table) {
        if (dest >= n) return;
        BranchArrow a;
        a.from = from_row;
        a.table = table;
        if (shown[dest]) {
            a.to = *shown[dest];
        } else {
            // Ends at the shown row next to the hidden destination, on the side the branch comes from.
            a.to_hidden = true;
            std::optional<usize> near;
            if (dest > source) {
                for (usize k = dest; k < n && !near; ++k) near = shown[k];
                if (!near) near = rows.empty() ? 0 : rows.size() - 1;
            } else {
                for (usize k = dest + 1; k-- > 0 && !near;) near = shown[k];
                if (!near) near = 0;
            }
            a.to = *near;
        }
        if (seen.emplace(a.from, a.to, a.to_hidden).second) arrows.push_back(a);
    };
    for (usize k = 0; k < n; ++k) {
        if (!shown[k]) continue;
        const auto& si = s.instructions[k];
        for (const auto& ref : si.refs) {
            if (!ref) continue;
            if (ref->kind == RefKind::label) {
                if (auto dest = label_index(ref->key)) add(*shown[k], k, *dest, false);
            } else if (ref->kind == RefKind::table) {
                for (auto part : split(ref->key, ','))
                    if (auto dest = label_index(part)) add(*shown[k], k, *dest, true);
            }
        }
    }

    // Lanes: shortest arrows first, each in the lowest lane where it shares no row with another.
    std::vector<usize> order(arrows.size());
    std::iota(order.begin(), order.end(), usize{0});
    auto span_of = [&](const BranchArrow& a) { return a.from > a.to ? a.from - a.to : a.to - a.from; };
    std::ranges::stable_sort(order, [&](usize x, usize y) {
        const usize sx = span_of(arrows[x]), sy = span_of(arrows[y]);
        return sx != sy ? sx < sy : std::min(arrows[x].from, arrows[x].to) < std::min(arrows[y].from, arrows[y].to);
    });
    std::vector<std::vector<std::pair<usize, usize>>> lanes;
    for (usize i : order) {
        BranchArrow& a = arrows[i];
        const usize lo = std::min(a.from, a.to), hi = std::max(a.from, a.to);
        usize lane = 0;
        for (; lane < lanes.size(); ++lane) {
            const bool overlaps = std::ranges::any_of(lanes[lane], [&](const auto& iv) { return iv.first <= hi && lo <= iv.second; });
            if (!overlaps) break;
        }
        if (lane == lanes.size()) lanes.emplace_back();
        lanes[lane].emplace_back(lo, hi);
        a.lane = static_cast<int>(lane);
    }
    return arrows;
}

int lane_count(std::span<const BranchArrow> arrows) {
    int lanes = 0;
    for (const auto& a : arrows) lanes = std::max(lanes, a.lane + 1);
    return lanes;
}

std::vector<OperandRef> operand_refs(const FunctionDiff& diff, const Row& row, DiffSide side, usize operand, const Program* program) {
    std::vector<OperandRef> out;
    const auto idx = index_of(row, side);
    const matching::Side& s = side_of(diff, side);
    if (!idx || *idx >= s.instructions.size()) return out;
    const auto& si = s.instructions[*idx];
    for (usize f = 0; f < si.ins.fields.size() && f < si.refs.size(); ++f) {
        if (!si.refs[f] || si.ins.fields[f].operand != static_cast<i8>(operand)) continue;
        const matching::Ref& ref = *si.refs[f];
        OperandRef r;
        r.kind = ref.kind;
        r.display = ref_text(ref);
        const Symbol* sym = nullptr;
        if (side == DiffSide::target && ref.target_va) {
            r.address = ref.target_va;
            if (program) {
                sym = program->symbols().at(ref.target_va);
                if (!sym) sym = program->symbols().containing(ref.target_va);
            }
        }
        if (ref.kind == RefKind::symbol) {
            r.name = ref.key;
            r.readable = display_name(ref.key);
            if (!sym && program)
                if (const Symbol* found = program->symbols().find(ref.key)) {
                    sym = found;
                    r.address = found->va + static_cast<u64>(ref.offset);
                }
        }
        if (ref.kind == RefKind::label)
            if (auto dest = label_index(ref.key); dest && *dest < s.instructions.size())
                r.readable = std::format("instruction #{} at offset {:#x}", *dest, s.instructions[*dest].ins.address - s.address);
        if (sym) {
            if (r.name.empty()) r.name = sym->name;
            if (r.readable.empty()) r.readable = sym->display.empty() ? display_name(sym->name) : sym->display;
            r.symbol_kind = std::string(to_string(sym->kind));
            r.size = sym->size;
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<DataDiffEntry> data_diff(const FunctionDiff& diff, const Program* program) {
    std::vector<DataDiffEntry> out;
    for (usize i = 0; i < diff.rows.size(); ++i) {
        const Row& row = diff.rows[i];
        const matching::SideInstruction* t = row.target && *row.target < diff.target.instructions.size() ? &diff.target.instructions[*row.target] : nullptr;
        const matching::SideInstruction* c =
            row.candidate && *row.candidate < diff.candidate.instructions.size() ? &diff.candidate.instructions[*row.candidate] : nullptr;
        const usize fields = std::max(t ? t->refs.size() : 0, c ? c->refs.size() : 0);
        for (usize f = 0; f < fields; ++f) {
            const matching::Ref* tr = t && f < t->refs.size() && t->refs[f] ? &*t->refs[f] : nullptr;
            const matching::Ref* cr = c && f < c->refs.size() && c->refs[f] ? &*c->refs[f] : nullptr;
            if (!(tr && is_data_kind(tr->kind)) && !(cr && is_data_kind(cr->kind))) continue;
            DataDiffEntry e;
            e.row = i;
            e.operand = t && f < t->ins.fields.size() ? t->ins.fields[f].operand : c && f < c->ins.fields.size() ? c->ins.fields[f].operand : -1;
            e.kind = cr && is_data_kind(cr->kind) ? cr->kind : tr->kind;
            if (tr) {
                e.target = tr->display;
                // An address without a string or constant symbol: show what the target holds there.
                if (!is_data_kind(tr->kind) && tr->target_va && program && e.kind != RefKind::table)
                    if (auto held = target_data_at(*program, tr->target_va, e.kind); !held.empty())
                        e.target = std::format("{} at {:#x}", held, tr->target_va);
            }
            if (cr) e.candidate = cr->display;
            if (e.kind == RefKind::table) {
                const auto tl = tr && tr->kind == RefKind::table ? table_labels(diff.target, tr->key) : std::vector<std::string>{};
                const auto cl = cr && cr->kind == RefKind::table ? table_labels(diff.candidate, cr->key) : std::vector<std::string>{};
                for (usize k = 0; k < std::max(tl.size(), cl.size()); ++k)
                    e.entries.emplace_back(k < tl.size() ? tl[k] : std::string(), k < cl.size() ? cl[k] : std::string());
                if (tr) e.target = std::format("switch_table ({} cases)", tl.size());
                if (cr) e.candidate = std::format("switch_table ({} cases)", cl.size());
            }
            if (e.kind == RefKind::index_table) {
                // Each switch value's entry in the jump table.
                const std::string tk = tr && tr->kind == RefKind::index_table ? tr->key : std::string();
                const std::string ck = cr && cr->kind == RefKind::index_table ? cr->key : std::string();
                auto entry = [](const std::string& key, usize k) {
                    return 2 * k + 2 <= key.size() ? std::to_string(std::stoul(key.substr(2 * k, 2), nullptr, 16)) : std::string();
                };
                for (usize k = 0; k < std::max(tk.size(), ck.size()) / 2; ++k) e.entries.emplace_back(entry(tk, k), entry(ck, k));
                if (tr) e.target = ref_text(*tr);
                if (cr) e.candidate = ref_text(*cr);
            }
            if (!tr || !cr) {
                e.equal = false;
            } else if (row.kind == RowKind::operand) {
                e.equal = !std::ranges::any_of(row.operands, [&](const auto& p) {
                    return static_cast<i8>(p.first) == e.operand && (p.second == OperandDiff::symbol || p.second == OperandDiff::mem);
                });
            } else if (row.kind == RowKind::opcode) {
                e.equal = e.target == e.candidate;
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<usize> hint_rows(const FunctionDiff& diff, std::string_view hint) {
    std::set<usize> targets;
    for (usize pos = hint.find("arget #"); pos != std::string_view::npos; pos = hint.find("arget #", pos + 1)) {
        if (pos == 0 || (hint[pos - 1] != 't' && hint[pos - 1] != 'T')) continue;  // "target #4" or "Target #4"
        usize v = 0, k = pos + 7;
        bool any = false;
        for (; k < hint.size() && hint[k] >= '0' && hint[k] <= '9'; ++k) {
            v = v * 10 + static_cast<usize>(hint[k] - '0');
            any = true;
        }
        if (any) targets.insert(v);
    }
    std::vector<usize> out;
    if (!targets.empty()) {
        for (usize i = 0; i < diff.rows.size(); ++i)
            if (diff.rows[i].target && targets.contains(*diff.rows[i].target)) out.push_back(i);
        return out;
    }
    auto rows_where = [&](auto&& pred) {
        for (usize i = 0; i < diff.rows.size(); ++i)
            if (pred(diff.rows[i])) out.push_back(i);
    };
    auto has_operand = [](const Row& r, OperandDiff k) {
        return r.kind == RowKind::operand && std::ranges::any_of(r.operands, [k](const auto& p) { return p.second == k; });
    };
    if (hint.find("register allocation") != std::string_view::npos) rows_where([&](const Row& r) { return has_operand(r, OperandDiff::reg); });
    else if (hint.find("stack offsets") != std::string_view::npos) rows_where([&](const Row& r) { return has_operand(r, OperandDiff::stack); });
    else if (hint.find("encoded differently") != std::string_view::npos) rows_where([](const Row& r) { return r.kind == RowKind::encoding; });
    else if (hint.find("than the target") != std::string_view::npos)
        rows_where([](const Row& r) { return r.kind == RowKind::insert || r.kind == RowKind::del; });
    else if (hint.find("different order") != std::string_view::npos || hint.find("could not be verified") != std::string_view::npos)
        rows_where([](const Row& r) { return r.kind != RowKind::equal; });
    return out;
}

std::vector<usize> binding_rows(const FunctionDiff& diff, const matching::Binding& binding) {
    std::vector<usize> out;
    for (usize i = 0; i < diff.rows.size(); ++i) {
        const Row& row = diff.rows[i];
        if (!row.target || !row.candidate) continue;
        const auto& t = diff.target.instructions[*row.target];
        const auto& c = diff.candidate.instructions[*row.candidate];
        bool here = false;
        for (usize f = 0; f < t.refs.size() && f < c.refs.size() && !here; ++f)
            here = t.refs[f] && c.refs[f] && t.refs[f]->kind == RefKind::unknown && t.refs[f]->target_va == binding.target_va &&
                   c.refs[f]->kind == RefKind::symbol && c.refs[f]->key == binding.candidate_symbol;
        if (here) out.push_back(i);
    }
    return out;
}

SymbolKind binding_kind(const FunctionDiff& diff, const matching::Binding& binding) {
    const auto rows = binding_rows(diff, binding);
    if (!rows.empty() && std::ranges::all_of(rows, [&](usize i) {
            const auto flow = diff.target.instructions[*diff.rows[i].target].ins.flow;
            return flow == x86::Flow::call || flow == x86::Flow::jump || flow == x86::Flow::cond_jump;
        }))
        return SymbolKind::function;
    return SymbolKind::data;
}

std::string rows_text(const FunctionDiff& diff, std::span<const DisplayRow> rows, TextMode mode, bool bytes) {
    std::string out;
    for (const DisplayRow& d : rows) {
        if (d.gap_before) out += "   ...\n";
        const Row& r = diff.rows[d.row];
        auto cell = [&](DiffSide side) -> std::string {
            const SideCell c = side_cell(diff, r, side, mode);
            if (!c.present) return {};
            std::string t = std::format("{:4x}: ", c.offset);
            if (bytes) {
                std::string packed = c.bytes;
                std::erase(packed, ' ');
                t += std::format("{:<20} ", packed);
            }
            return t + c.text();
        };
        char marker = ' ';
        switch (r.kind) {
        case RowKind::equal: marker = ' '; break;
        case RowKind::encoding: marker = 'e'; break;
        case RowKind::operand: marker = '~'; break;
        case RowKind::opcode: marker = '!'; break;
        case RowKind::insert: marker = '+'; break;
        case RowKind::del: marker = '-'; break;
        }
        std::string note;
        if (r.kind == RowKind::operand) {
            std::vector<std::string> parts;
            for (const auto& [op, kind] : r.operands) parts.push_back(std::format("op{} {}", op, matching::to_string(kind)));
            note = "  (" + join(parts, ", ") + ")";
        } else if (r.kind == RowKind::encoding) {
            note = "  (encoding)";
        }
        out += std::format("{} {:<52}| {}{}\n", marker, cell(DiffSide::target), cell(DiffSide::candidate), note);
    }
    return out;
}

std::vector<PairedRow> pair_attempt_rows(const FunctionDiff& a, const FunctionDiff& b) {
    std::vector<PairedRow> out;
    usize i = 0, j = 0;
    const usize na = a.rows.size(), nb = b.rows.size();
    while (i < na || j < nb) {
        // Candidate-only rows (no target instruction) first, paired in order.
        usize ia = i, jb = j;
        while (ia < na && !a.rows[ia].target) ++ia;
        while (jb < nb && !b.rows[jb].target) ++jb;
        for (usize k = 0; k < std::max(ia - i, jb - j); ++k)
            out.push_back(PairedRow{i + k < ia ? std::optional<usize>(i + k) : std::nullopt, j + k < jb ? std::optional<usize>(j + k) : std::nullopt});
        i = ia;
        j = jb;
        if (i < na && j < nb) {
            const usize ta = *a.rows[i].target, tb = *b.rows[j].target;
            if (ta == tb) out.push_back(PairedRow{i++, j++});
            else if (ta < tb) out.push_back(PairedRow{i++, std::nullopt});
            else out.push_back(PairedRow{std::nullopt, j++});
        } else if (i < na) {
            out.push_back(PairedRow{i++, std::nullopt});
        } else if (j < nb) {
            out.push_back(PairedRow{std::nullopt, j++});
        }
    }
    return out;
}

} // namespace decomp::vm
