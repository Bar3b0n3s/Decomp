#pragma once

// The dialog that creates, renames, edits or removes a symbol (Binary explorer, Symbols and
// provenance). Edits go through Project::set_symbol() as user changes, so they reach symbols.txt and its
// provenance log, and the workspace then builds a new program generation.

#include "gui/view.hpp"
#include "gui/views/view_support.hpp"

#include <optional>
#include <string>

namespace decomp::gui {

class SymbolEditor {
public:
    // Opens the dialog for the symbol at `va` (a new one when there is none). `where` names the view in
    // the change's reason ("renamed in the Binary explorer").
    void open(const ProjectAccess& access, u64 va, std::string where);
    // Draws the dialog while it is open; call every frame in the view's window.
    void draw(ViewContext& ctx, const ProjectAccess& access);

private:
    bool open_request_ = false;
    u64 va_ = 0;
    bool exists_ = false;
    std::string name_, original_name_, where_, reason_, error_;
    int kind_ = 0;  // index into kKinds (symbol_editor.cpp)
    int size_ = 0;
};

} // namespace decomp::gui
