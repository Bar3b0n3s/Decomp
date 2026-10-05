#include "gui/views/symbol_editor.hpp"

#include "core/strings.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <format>

namespace decomp::gui {

namespace {

constexpr std::array<SymbolKind, 7> kKinds = {SymbolKind::function, SymbolKind::data,  SymbolKind::string, SymbolKind::float_const,
                                              SymbolKind::import,   SymbolKind::label, SymbolKind::unknown};

int kind_index(SymbolKind kind) {
    for (usize i = 0; i < kKinds.size(); ++i)
        if (kKinds[i] == kind) return static_cast<int>(i);
    return 0;
}

} // namespace

void SymbolEditor::open(const ProjectAccess& access, u64 va, std::string where) {
    va_ = va;
    where_ = std::move(where);
    error_.clear();
    reason_.clear();
    const Symbol* s = access.program ? access.program->symbols().at(va) : nullptr;
    exists_ = s != nullptr;
    name_ = s ? s->name : std::format("data_{:x}", va);
    original_name_ = s ? s->name : std::string();
    kind_ = kind_index(s ? s->kind : SymbolKind::data);
    size_ = s ? static_cast<int>(s->size) : 0;
    open_request_ = true;
}

void SymbolEditor::draw(ViewContext& ctx, const ProjectAccess& access) {
    if (open_request_) {
        ImGui::OpenPopup("Symbol###symbol_editor");
        open_request_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Symbol###symbol_editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%s the symbol at %s", exists_ ? "Edit" : "Create", hex(va_, 8).c_str());
    const float field = ImGui::GetFontSize() * 26;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Name (decorated, as in symbols.txt)", &name_);
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("Kind", std::string(to_string(kKinds[static_cast<usize>(kind_)])).c_str())) {
        for (usize i = 0; i < kKinds.size(); ++i)
            if (ImGui::Selectable(std::string(to_string(kKinds[i])).c_str(), static_cast<int>(i) == kind_)) kind_ = static_cast<int>(i);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(field);
    ImGui::InputInt("Size in bytes (0 = unknown)", &size_);
    size_ = std::max(size_, 0);
    ImGui::SetNextItemWidth(field);
    ImGui::InputTextWithHint("Reason", "optional, recorded in the provenance log", &reason_);
    ImGui::TextDisabled("Recorded as a user change in symbols.txt and .decomp/symbols.log.jsonl.");
    if (ctx.services.commands->live()) ImGui::TextDisabled("Sessions that start from now on see the change; running ones keep theirs.");
    if (!error_.empty()) colored_text(ctx.colors().error, error_);

    auto apply = [&](const project::SymbolEdit& edit, std::string_view what) {
        const std::string reason = trim(reason_).empty() ? std::format("{} in the {}", what, where_) : std::string(trim(reason_));
        auto r = access.project->set_symbol(edit, project::ChangeOrigin{SymbolSource::user, "", reason});
        if (!r) {
            error_ = r.error().message;
            return;
        }
        if (access.workspace) access.workspace->reload_symbols();
        ctx.notify(Severity::info, std::format("Symbol at {} {}.", hex(va_, 8), what));
        ImGui::CloseCurrentPopup();
    };
    const std::string name(trim(name_));
    ImGui::BeginDisabled(name.empty() || !access.project);
    if (ImGui::Button(exists_ ? "Save" : "Create")) {
        project::SymbolEdit edit;
        edit.va = va_;
        edit.name = name;
        edit.kind = kKinds[static_cast<usize>(kind_)];
        edit.size = static_cast<u32>(size_);
        apply(edit, !exists_ ? "created" : name != original_name_ ? "renamed" : "edited");
    }
    ImGui::EndDisabled();
    if (exists_) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            project::SymbolEdit edit;
            edit.va = va_;
            edit.remove = true;
            apply(edit, "removed");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Remove it from symbols.txt (what the binary itself says stays).");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace decomp::gui
