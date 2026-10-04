#include "viewmodel/function_table.hpp"
#include "viewmodel/treemap.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace decomp;
using namespace decomp::vm;

namespace {

double aspect(const Rect& r) { return std::max(r.w / r.h, r.h / r.w); }

bool overlap(const Rect& a, const Rect& b) {
    const double eps = 1e-9;
    return a.x + eps < b.x + b.w && b.x + eps < a.x + a.w && a.y + eps < b.y + b.h && b.y + eps < a.y + a.h;
}

// The rectangles tile `bounds`: inside it, disjoint, areas proportional to the weights.
void check_tiling(const std::vector<double>& weights, const std::vector<Rect>& rects, const Rect& bounds) {
    REQUIRE(rects.size() == weights.size());
    double total = 0;
    for (double w : weights) total += std::max(0.0, w);
    double area = 0;
    for (usize i = 0; i < rects.size(); ++i) {
        const Rect& r = rects[i];
        if (weights[i] <= 0) {
            CHECK(r.area() == 0);
            continue;
        }
        CHECK(r.x >= bounds.x - 1e-9);
        CHECK(r.y >= bounds.y - 1e-9);
        CHECK(r.x + r.w <= bounds.x + bounds.w + 1e-6);
        CHECK(r.y + r.h <= bounds.y + bounds.h + 1e-6);
        CHECK(r.area() == doctest::Approx(weights[i] / total * bounds.area()).epsilon(1e-6));
        area += r.area();
    }
    CHECK(area == doctest::Approx(bounds.area()));
    for (usize i = 0; i < rects.size(); ++i)
        for (usize j = i + 1; j < rects.size(); ++j)
            if (weights[i] > 0 && weights[j] > 0) CHECK_FALSE(overlap(rects[i], rects[j]));
}

} // namespace

TEST_CASE("squarify: the paper's example, tiling, determinism and degenerate input") {
    // Bruls et al., figure 2: 6, 6, 4, 3, 2, 2, 1 in a 6 x 4 rectangle.
    const std::vector<double> paper = {6, 6, 4, 3, 2, 2, 1};
    const Rect bounds{0, 0, 6, 4};
    const auto rects = squarify(paper, bounds);
    check_tiling(paper, rects, bounds);
    // The first row holds the two 6s as a column on the left (the shorter side is the height).
    CHECK(rects[0].x == 0);
    CHECK(rects[0].w == doctest::Approx(3));
    CHECK(rects[0].h == doctest::Approx(2));
    CHECK(rects[1].x == 0);
    CHECK(rects[1].y == doctest::Approx(2));
    CHECK(rects[2].x == doctest::Approx(3));  // the 4 starts the next row
    for (const auto& r : rects) CHECK(aspect(r) < 3.0);
    CHECK(squarify(paper, bounds).size() == rects.size());
    for (usize i = 0; i < rects.size(); ++i) CHECK(squarify(paper, bounds)[i].x == rects[i].x);

    const std::vector<double> mixed = {0, 5, -1, 5, 1e-9, 20};
    check_tiling(mixed, squarify(mixed, Rect{10, 20, 100, 30}), Rect{10, 20, 100, 30});
    CHECK(squarify({}, bounds).empty());
    for (const auto& r : squarify(std::vector<double>{1, 2}, Rect{0, 0, 0, 10})) CHECK(r.area() == 0);
    // Equal weights stay in index order (ties broken by index).
    const auto equal = squarify(std::vector<double>{1, 1, 1, 1}, Rect{0, 0, 2, 2});
    CHECK(equal[0].x == 0);
    CHECK(equal[0].y == 0);
}

TEST_CASE("treemap: the fixture's code by section, colors from the rows, hit testing") {
    test::FixtureProject fx;
    fx.set("add", project::FunctionStatus::matched, 100);
    fx.set("sum_array", project::FunctionStatus::nonmatching, 70);
    auto rows = build_function_rows(fx.program.symbols(), *fx.project.function_infos());
    const Rect bounds{0, 0, 800, 400};
    const Treemap t = build_text_treemap(rows, fx.program.image(), bounds, TreemapOptions{2, 14});
    REQUIRE(t.groups.size() == 1);
    CHECK(t.groups[0].name == ".text");
    usize sized = 0;
    u64 bytes = 0;
    for (const auto& r : rows)
        if (r.size) {
            ++sized;
            bytes += r.size;
        }
    CHECK(t.cells.size() == sized);  // ExitProcess (size 0) has no cell
    CHECK(t.groups[0].bytes == bytes);
    CHECK(t.groups[0].inner.y == doctest::Approx(16));  // padding + header
    for (usize i = 1; i < t.cells.size(); ++i) CHECK(t.cells[i - 1].bytes >= t.cells[i].bytes);

    const auto add = t.find(fx.va("add"));
    REQUIRE(add);
    const TreemapCell& cell = t.cells[*add];
    CHECK(cell.status == project::FunctionStatus::matched);
    CHECK(rows[cell.row].va == fx.va("add"));
    CHECK(t.cells[*t.find(fx.va("sum_array"))].best_match == 70);
    CHECK(t.hit(cell.rect.x + cell.rect.w / 2, cell.rect.y + cell.rect.h / 2) == add);
    CHECK_FALSE(t.hit(-1, 5));
    CHECK_FALSE(t.hit(1, 1));  // the group's padding
    CHECK_FALSE(t.find(0x12345));
    // Every point of every cell hits that cell.
    for (usize i = 0; i < t.cells.size(); ++i) {
        const Rect& r = t.cells[i].rect;
        CHECK(t.hit(r.x + r.w * 0.25, r.y + r.h * 0.75) == i);
    }
}

TEST_CASE("treemap: 100,000 functions") {
    std::vector<FunctionRow> rows(100'000);
    u64 va = 0x401000;
    u32 state = 7;
    for (auto& r : rows) {
        state = state * 1664525u + 1013904223u;
        r.va = va;
        r.size = 4 + (state >> 20) % 2000;
        va += r.size;
    }
    struct Text : BinaryImage {
        std::vector<ImageSection> sections{ImageSection{".text", 0x401000, 0x10000000, 0x10000000, true, false, true}};
        Arch arch() const override { return Arch::x86; }
        u64 image_base() const override { return 0x400000; }
        u64 image_size() const override { return 0x10001000; }
        u64 entry_point() const override { return 0; }
        const std::vector<ImageSection>& image_sections() const override { return sections; }
        std::optional<ByteSpan> view(u64, usize) const override { return std::nullopt; }
        bool has_relocations() const override { return false; }
        bool is_relocated(u64) const override { return false; }
    } image;
    const auto start = std::chrono::steady_clock::now();
    const Treemap t = build_text_treemap(rows, image, Rect{0, 0, 1600, 900});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    usize hits = 0;
    const auto hit_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10'000; ++i) hits += t.hit(std::fmod(i * 7.31, 1600.0), std::fmod(i * 3.17, 900.0)).has_value();
    const double hit_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - hit_start).count() / 10'000;
    MESSAGE("build_text_treemap: 100,000 functions in " << ms << " ms; hit() " << hit_us << " us per query");
    CHECK(t.cells.size() == rows.size());
    CHECK(hits > 9'000);
#ifdef NDEBUG
    CHECK(ms < 1000);
#endif
}
