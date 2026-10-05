#include "gui/views/palette_search.hpp"

#include "core/strings.hpp"
#include "gui/actions.hpp"
#include "gui/view.hpp"
#include "gui/views/view_support.hpp"
#include "viewmodel/search.hpp"

#include <format>
#include <memory>

namespace decomp::gui {

namespace {

constexpr usize kResults = 40;

struct Index {
    std::vector<vm::SearchEntry> symbols;
    std::vector<ImageString> strings;
};

struct IndexKey {
    std::weak_ptr<const Program> program;
    bool operator==(const IndexKey& o) const { return !program.owner_before(o.program) && !o.program.owner_before(program); }
};

struct QueryKey {
    u64 index = 0;
    std::string text;
    bool strings = false;
    bool operator==(const QueryKey&) const = default;
};

struct Results {
    std::shared_ptr<const Index> index;
    std::vector<vm::SearchHit> hits;
    bool strings = false;
};

// Shared by both providers: the index of the current program generation and the latest search.
struct SearchState {
    KeyedJob<IndexKey, std::shared_ptr<const Index>> index;
    u64 index_generation = 0;
    KeyedJob<QueryKey, Results> search;

    // The results to show for the query: the newest finished search of this kind (it may lag a keystroke).
    const Results* update(ViewContext& ctx, const std::string& text, bool strings) {
        const ProjectAccess access = project_access(ctx);
        if (!access.ready()) return nullptr;
        if (index.poll()) ++index_generation;
        search.poll();
        const auto program = access.program;
        index.update(ctx.jobs, IndexKey{program}, [program] {
            return [program] {
                auto made = std::make_shared<Index>();
                made->symbols = vm::search_entries(program->symbols());
                made->strings = scan_strings(program->image(), {}, &program->symbols());
                return std::shared_ptr<const Index>(std::move(made));
            };
        });
        if (!index.value()) return nullptr;
        const std::shared_ptr<const Index> current = *index.value();
        search.update(ctx.jobs, QueryKey{index_generation, text, strings}, [current, text, strings] {
            return [current, text, strings](const CancelToken& token) {
                Results r;
                r.index = current;
                r.strings = strings;
                const auto cancelled = [&token] { return token.cancelled(); };
                r.hits = strings ? vm::search_strings(current->strings, text, kResults, cancelled)
                                 : vm::search_symbols(current->symbols, text, kResults, fuzzy_score, cancelled);
                return r;
            };
        });
        const auto& value = search.value();
        return value && value->strings == strings ? &*value : nullptr;
    }
};

std::string shorten(std::string text, usize max) {
    if (text.size() > max) text = text.substr(0, max - 3) + "...";
    return text;
}

} // namespace

void add_palette_providers(CommandPalette& palette) {
    auto state = std::make_shared<SearchState>();
    palette.add_provider("symbols", [state](const PaletteQuery& q, ViewContext& ctx, std::vector<PaletteItem>& out) {
        if (q.kind != PaletteQuery::Kind::all || q.text.empty() || q.address) return;
        const Results* r = state->update(ctx, q.text, false);
        if (!r) return;
        for (const auto& hit : r->hits) {
            const vm::SearchEntry& e = r->index->symbols[hit.index];
            const bool function = e.kind == SymbolKind::function;
            PaletteItem item;
            item.label = shorten(e.display, 90);
            item.detail = std::format("{} {}", to_string(e.kind), hex(e.va, 8));
            item.score = hit.score;
            const u64 va = e.va;
            item.run = function ? std::function<void()>([&ctx, va] { ctx.open("inspector", {.va = va}); })
                                : std::function<void()>([&ctx, va] { ctx.open("binary_explorer", {.anchor = hex(va)}); });
            out.push_back(std::move(item));
        }
    });
    palette.add_provider("strings", [state](const PaletteQuery& q, ViewContext& ctx, std::vector<PaletteItem>& out) {
        if (q.kind != PaletteQuery::Kind::strings || q.text.empty()) return;
        const Results* r = state->update(ctx, q.text, true);
        if (!r) return;
        for (const auto& hit : r->hits) {
            const ImageString& s = r->index->strings[hit.index];
            PaletteItem item;
            item.label = shorten(escape_c_string(s.text), 90);  // quoted
            item.detail = std::format("string {}", hex(s.va, 8));
            item.score = hit.score;
            const u64 va = s.va;
            item.run = [&ctx, va] { ctx.open("binary_explorer", {.anchor = "string:" + hex(va)}); };
            out.push_back(std::move(item));
        }
    });
}

} // namespace decomp::gui
