#pragma once

// The search and command palette (Ctrl+P, docs/ui.md#search-and-command-palette). One input searches
// actions (fuzzy), addresses ("0x401000" jumps there) and whatever providers add: functions and symbols
// (V1), strings (prefix ", V5). The prefix ">" restricts the search to actions.

#include "core/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

struct ViewContext;

struct PaletteQuery {
    enum class Kind { all, actions, strings };
    Kind kind = Kind::all;
    std::string text;            // the query without its prefix, trimmed
    std::optional<u64> address;  // "0x..." parsed as an address (Kind::all only)
};

PaletteQuery parse_palette_query(std::string_view input);

struct PaletteItem {
    std::string label;
    std::string detail;  // right-aligned: category, shortcut, address
    int score = 0;       // higher first; items from fuzzy_score() compare across providers
    bool enabled = true;
    std::function<void()> run;
};

// Adds items for a query (called each frame while the palette is open, on the UI thread: keep it cheap,
// e.g. search an index built by a job).
using PaletteProvider = std::function<void(const PaletteQuery& query, ViewContext& ctx, std::vector<PaletteItem>& out)>;

class CommandPalette {
public:
    static constexpr usize kMaxItems = 50;

    void add_provider(std::string name, PaletteProvider provider);
    void open(std::string_view initial = {});
    void close() { close_request_ = true; }
    bool is_open() const { return open_; }
    const std::string& input() const { return input_; }

    // The palette's items for an input, best first (at most kMaxItems).
    std::vector<PaletteItem> items(std::string_view input, ViewContext& ctx) const;

    // Draws the palette popup if open; runs the chosen item after closing it.
    void draw(ViewContext& ctx);

private:
    std::vector<std::pair<std::string, PaletteProvider>> providers_;
    std::string input_;
    int selected_ = 0;
    bool open_ = false;
    bool open_request_ = false;
    bool close_request_ = false;
    bool focus_input_ = false;
    bool scroll_to_selected_ = false;
};

} // namespace decomp::gui
