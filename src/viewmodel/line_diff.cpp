#include "viewmodel/line_diff.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace decomp::vm {

namespace {

std::vector<std::string> split_text(std::string_view text, bool strip_cr) {
    std::vector<std::string> lines;
    usize start = 0;
    while (start < text.size()) {
        const usize nl = text.find('\n', start);
        std::string_view line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (strip_cr && nl != std::string_view::npos && line.ends_with('\r')) line.remove_suffix(1);
        lines.emplace_back(line);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return lines;
}

// Myers' algorithm over line ids, following the bisection of Neil Fraser's diff-match-patch: find the
// middle snake of the D-path with both a forward and a reverse search, split there, and recurse.
class Myers {
public:
    Myers(const std::vector<u32>& a, const std::vector<u32>& b, std::vector<LineEdit>& out) : a_(a), b_(b), out_(out) {}

    void diff(usize a0, usize a1, usize b0, usize b1) {
        while (a0 < a1 && b0 < b1 && a_[a0] == b_[b0]) emit(DiffOp::equal, a0++, b0++);
        usize suffix = 0;
        while (a1 - suffix > a0 && b1 - suffix > b0 && a_[a1 - 1 - suffix] == b_[b1 - 1 - suffix]) ++suffix;
        middle(a0, a1 - suffix, b0, b1 - suffix);
        for (usize i = 0; i < suffix; ++i) emit(DiffOp::equal, a1 - suffix + i, b1 - suffix + i);
    }

private:
    void emit(DiffOp op, usize a, usize b) { out_.push_back(LineEdit{op, static_cast<u32>(a), static_cast<u32>(b)}); }

    void replace(usize a0, usize a1, usize b0, usize b1) {
        for (usize i = a0; i < a1; ++i) emit(DiffOp::del, i, b0);
        for (usize j = b0; j < b1; ++j) emit(DiffOp::insert, a1, j);
    }

    void middle(usize a0, usize a1, usize b0, usize b1) {
        if (a0 == a1 || b0 == b1) return replace(a0, a1, b0, b1);
        const long long n = static_cast<long long>(a1 - a0), m = static_cast<long long>(b1 - b0);
        const long long max_d = (n + m + 1) / 2;
        const long long offset = max_d, length = 2 * max_d;
        std::vector<long long> v1(static_cast<usize>(length), -1), v2(static_cast<usize>(length), -1);
        v1[static_cast<usize>(offset + 1)] = 0;
        v2[static_cast<usize>(offset + 1)] = 0;
        const long long delta = n - m;
        const bool front = delta % 2 != 0;  // the forward path meets the reverse one
        long long k1start = 0, k1end = 0, k2start = 0, k2end = 0;
        auto A = [&](long long i) { return a_[a0 + static_cast<usize>(i)]; };
        auto B = [&](long long j) { return b_[b0 + static_cast<usize>(j)]; };
        for (long long d = 0; d < max_d; ++d) {
            for (long long k1 = -d + k1start; k1 <= d - k1end; k1 += 2) {
                const long long k1_offset = offset + k1;
                long long x1 = k1 == -d || (k1 != d && v1[static_cast<usize>(k1_offset - 1)] < v1[static_cast<usize>(k1_offset + 1)])
                                   ? v1[static_cast<usize>(k1_offset + 1)]
                                   : v1[static_cast<usize>(k1_offset - 1)] + 1;
                long long y1 = x1 - k1;
                while (x1 < n && y1 < m && A(x1) == B(y1)) {
                    ++x1;
                    ++y1;
                }
                v1[static_cast<usize>(k1_offset)] = x1;
                if (x1 > n) {
                    k1end += 2;  // ran off the right of the edit graph
                } else if (y1 > m) {
                    k1start += 2;  // ran off the bottom
                } else if (front) {
                    const long long k2_offset = offset + delta - k1;
                    if (k2_offset >= 0 && k2_offset < length && v2[static_cast<usize>(k2_offset)] != -1 &&
                        x1 >= n - v2[static_cast<usize>(k2_offset)])
                        return split(a0, a1, b0, b1, x1, y1);
                }
            }
            for (long long k2 = -d + k2start; k2 <= d - k2end; k2 += 2) {
                const long long k2_offset = offset + k2;
                long long x2 = k2 == -d || (k2 != d && v2[static_cast<usize>(k2_offset - 1)] < v2[static_cast<usize>(k2_offset + 1)])
                                   ? v2[static_cast<usize>(k2_offset + 1)]
                                   : v2[static_cast<usize>(k2_offset - 1)] + 1;
                long long y2 = x2 - k2;
                while (x2 < n && y2 < m && A(n - x2 - 1) == B(m - y2 - 1)) {
                    ++x2;
                    ++y2;
                }
                v2[static_cast<usize>(k2_offset)] = x2;
                if (x2 > n) {
                    k2end += 2;
                } else if (y2 > m) {
                    k2start += 2;
                } else if (!front) {
                    const long long k1_offset = offset + delta - k2;
                    if (k1_offset >= 0 && k1_offset < length && v1[static_cast<usize>(k1_offset)] != -1) {
                        const long long x1 = v1[static_cast<usize>(k1_offset)];
                        const long long y1 = offset + x1 - k1_offset;
                        if (x1 >= n - x2) return split(a0, a1, b0, b1, x1, y1);
                    }
                }
            }
        }
        replace(a0, a1, b0, b1);  // no common line at all
    }

    void split(usize a0, usize a1, usize b0, usize b1, long long x, long long y) {
        diff(a0, a0 + static_cast<usize>(x), b0, b0 + static_cast<usize>(y));
        diff(a0 + static_cast<usize>(x), a1, b0 + static_cast<usize>(y), b1);
    }

    const std::vector<u32>& a_;
    const std::vector<u32>& b_;
    std::vector<LineEdit>& out_;
};

// Within each change, deletions first; then the positions of insertions and deletions on the other side.
void normalize(std::vector<LineEdit>& edits) {
    for (usize i = 0; i < edits.size();) {
        if (edits[i].op == DiffOp::equal) {
            ++i;
            continue;
        }
        usize j = i;
        while (j < edits.size() && edits[j].op != DiffOp::equal) ++j;
        std::stable_partition(edits.begin() + static_cast<std::ptrdiff_t>(i), edits.begin() + static_cast<std::ptrdiff_t>(j),
                              [](const LineEdit& e) { return e.op == DiffOp::del; });
        i = j;
    }
    u32 old_pos = 0, new_pos = 0;
    for (auto& e : edits) {
        e.old_index = old_pos;
        e.new_index = new_pos;
        if (e.op != DiffOp::insert) ++old_pos;
        if (e.op != DiffOp::del) ++new_pos;
    }
}

} // namespace

LineDiff diff_lines(std::string_view old_text, std::string_view new_text, const LineDiffOptions& options) {
    LineDiff d;
    d.old_lines = split_text(old_text, options.strip_cr);
    d.new_lines = split_text(new_text, options.strip_cr);
    std::unordered_map<std::string_view, u32> ids;
    auto to_ids = [&](const std::vector<std::string>& lines) {
        std::vector<u32> out;
        out.reserve(lines.size());
        for (const auto& l : lines) out.push_back(ids.try_emplace(l, static_cast<u32>(ids.size())).first->second);
        return out;
    };
    const std::vector<u32> a = to_ids(d.old_lines);
    const usize old_ids = ids.size();
    const std::vector<u32> b = to_ids(d.new_lines);
    d.edits.reserve(a.size() + b.size());
    Myers myers(a, b, d.edits);
    if (std::ranges::none_of(b, [&](u32 id) { return id < old_ids; })) {
        // Nothing in common: the search would only confirm it.
        for (usize i = 0; i < a.size(); ++i) d.edits.push_back(LineEdit{DiffOp::del, static_cast<u32>(i), 0});
        for (usize j = 0; j < b.size(); ++j) d.edits.push_back(LineEdit{DiffOp::insert, static_cast<u32>(a.size()), static_cast<u32>(j)});
    } else {
        myers.diff(0, a.size(), 0, b.size());
    }
    normalize(d.edits);
    for (const auto& e : d.edits) {
        if (e.op == DiffOp::insert) ++d.inserted;
        if (e.op == DiffOp::del) ++d.deleted;
    }
    return d;
}

std::vector<Hunk> unified_hunks(const LineDiff& diff, usize context) {
    std::vector<Hunk> hunks;
    const auto& e = diff.edits;
    const usize n = e.size();
    usize i = 0;
    while (i < n) {
        usize change = i;
        while (change < n && e[change].op == DiffOp::equal) ++change;
        if (change == n) break;
        const usize start = change - std::min(context, change - i);
        usize end = change;  // one past the last change in the hunk
        usize j = change;
        while (j < n) {
            if (e[j].op != DiffOp::equal) {
                end = ++j;
                continue;
            }
            usize k = j;
            while (k < n && e[k].op == DiffOp::equal) ++k;
            if (k < n && k - j <= 2 * context) j = k;  // the next change shares the context: same hunk
            else break;
        }
        const usize stop = std::min(n, end + context);
        Hunk h;
        h.first_edit = start;
        h.edit_count = stop - start;
        for (usize x = start; x < stop; ++x) {
            if (e[x].op != DiffOp::insert) ++h.old_count;
            if (e[x].op != DiffOp::del) ++h.new_count;
        }
        h.old_start = e[start].old_index + (h.old_count ? 1 : 0);
        h.new_start = e[start].new_index + (h.new_count ? 1 : 0);
        hunks.push_back(h);
        i = stop;
    }
    return hunks;
}

namespace {

std::string range(usize start, usize count) { return count == 1 ? std::format("{}", start) : std::format("{},{}", start, count); }

} // namespace

std::string to_unified(const LineDiff& diff, std::string_view old_label, std::string_view new_label, usize context) {
    if (diff.identical()) return {};
    std::string out = std::format("--- {}\n+++ {}\n", old_label, new_label);
    for (const auto& h : unified_hunks(diff, context)) {
        out += std::format("@@ -{} +{} @@\n", range(h.old_start, h.old_count), range(h.new_start, h.new_count));
        for (usize x = h.first_edit; x < h.first_edit + h.edit_count; ++x) {
            const LineEdit& e = diff.edits[x];
            switch (e.op) {
            case DiffOp::equal: out += " " + diff.old_lines[e.old_index] + "\n"; break;
            case DiffOp::del: out += "-" + diff.old_lines[e.old_index] + "\n"; break;
            case DiffOp::insert: out += "+" + diff.new_lines[e.new_index] + "\n"; break;
            }
        }
    }
    return out;
}

std::vector<SideBySideRow> side_by_side(const LineDiff& diff) {
    std::vector<SideBySideRow> rows;
    const auto& e = diff.edits;
    for (usize i = 0; i < e.size();) {
        if (e[i].op == DiffOp::equal) {
            rows.push_back({SideBySideRow::Kind::equal, static_cast<i32>(e[i].old_index), static_cast<i32>(e[i].new_index)});
            ++i;
            continue;
        }
        std::vector<i32> removed, added;
        for (; i < e.size() && e[i].op != DiffOp::equal; ++i)
            (e[i].op == DiffOp::del ? removed : added).push_back(static_cast<i32>(e[i].op == DiffOp::del ? e[i].old_index : e[i].new_index));
        const usize pairs = std::min(removed.size(), added.size());
        for (usize k = 0; k < pairs; ++k) rows.push_back({SideBySideRow::Kind::changed, removed[k], added[k]});
        for (usize k = pairs; k < removed.size(); ++k) rows.push_back({SideBySideRow::Kind::removed, removed[k], -1});
        for (usize k = pairs; k < added.size(); ++k) rows.push_back({SideBySideRow::Kind::added, -1, added[k]});
    }
    return rows;
}

} // namespace decomp::vm
