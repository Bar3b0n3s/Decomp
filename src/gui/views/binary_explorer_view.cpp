// Binary explorer (docs/ui.md#binary-explorer): sections, imports and exports, strings with their
// cross-references, a hex view with symbol overlays (functions, data, strings, floats, jump tables,
// relocations, import slots), the Rich header and the PDB. Addresses follow cross-references; symbols
// can be created or renamed; functions open in the Inspector or the Diff viewer.

#include "gui/views/binary_explorer_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/symbol_editor.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/binary.hpp"
#include "viewmodel/dashboard.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <format>

namespace decomp::gui {

namespace {

enum Tab : int { kSections, kImports, kExports, kStrings, kHex, kRich, kPdb, kTabCount };

struct ProgramKey {
    std::weak_ptr<const Program> program;
    bool operator==(const ProgramKey& o) const { return !program.owner_before(o.program) && !o.program.owner_before(program); }
};

struct DerivedKey {
    ProgramKey program;
    u64 strings = 0;  // generation of the string scan it used
    bool operator==(const DerivedKey&) const = default;
};

struct AddressKey {
    ProgramKey program;
    u64 va = 0;
    bool operator==(const AddressKey&) const = default;
};

using Strings = std::shared_ptr<const std::vector<ImageString>>;

ImVec4 overlay_color(const ViewContext& ctx, vm::OverlayKind kind) {
    const ThemeColors& c = ctx.colors();
    switch (kind) {
    case vm::OverlayKind::function: return c.info;
    case vm::OverlayKind::data: return c.accent;
    case vm::OverlayKind::import: return c.warn;
    case vm::OverlayKind::string: return c.ok;
    case vm::OverlayKind::float_const: return status_color(ctx, project::FunctionStatus::gave_up);
    case vm::OverlayKind::jump_table: return status_color(ctx, project::FunctionStatus::nonmatching);
    case vm::OverlayKind::relocation: return c.error;
    }
    return c.text;
}

constexpr vm::OverlayKind kKinds[] = {vm::OverlayKind::function,    vm::OverlayKind::data,       vm::OverlayKind::import,    vm::OverlayKind::string,
                                      vm::OverlayKind::float_const, vm::OverlayKind::jump_table, vm::OverlayKind::relocation};

class BinaryExplorerView final : public View {
public:
    std::string_view id() const override { return "binary_explorer"; }
    std::string_view title() const override { return "Binary explorer"; }

    void navigate(ViewContext& /*ctx*/, const NavTarget& target) override {
        std::string anchor = target.anchor;
        if (anchor.empty() && target.va) anchor = hex(*target.va);
        if (anchor.empty()) return;
        pending_anchor_ = anchor;
    }

    void draw(ViewContext& ctx) override {
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "The Binary explorer shows sections, imports and exports, strings with cross-references, a hex view with "
                             "symbol overlays, the Rich header and the PDB.")) {
            return;
        }
        update_jobs(ctx, access);
        if (pending_anchor_) {
            std::string_view anchor = *pending_anchor_;
            const bool string_anchor = anchor.starts_with("string:");
            if (string_anchor) anchor.remove_prefix(7);
            if (auto va = vm::parse_address(*access.program, anchor)) {
                if (string_anchor) {
                    selected_string_va_ = *va;
                    tab_request_ = kStrings;
                } else {
                    go_to(*access.program, *va);
                }
            }
            pending_anchor_.reset();
        }
        draw_toolbar(ctx, access);
        if (ImGui::BeginTabBar("##binary_tabs")) {
            const char* names[kTabCount] = {"Sections", "Imports", "Exports", "Strings", "Hex", "Rich header", "PDB"};
            for (int t = 0; t < kTabCount; ++t) {
                const ImGuiTabItemFlags flags = tab_request_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (!ImGui::BeginTabItem(names[t], nullptr, flags)) continue;
                switch (t) {
                case kSections: draw_sections(ctx, access); break;
                case kImports: draw_imports(ctx, access); break;
                case kExports: draw_exports(ctx, access); break;
                case kStrings: draw_strings(ctx, access); break;
                case kHex: draw_hex(ctx, access); break;
                case kRich: draw_rich(access); break;
                case kPdb: draw_pdb(ctx, access); break;
                default: break;
                }
                ImGui::EndTabItem();
            }
            tab_request_ = -1;
            ImGui::EndTabBar();
        }
        editor_.draw(ctx, access);
    }

private:
    // ---- jobs -----------------------------------------------------------------------------------------

    void update_jobs(ViewContext& ctx, const ProjectAccess& access) {
        if (strings_.poll()) ++strings_generation_;
        string_refs_.poll();
        overlays_.poll();
        refs_.poll();
        const auto program = access.program;
        if (const std::string root = fs::to_utf8(access.project->root()); root != project_root_) {
            project_root_ = root;
            strings_.reset();
            string_refs_.reset();
            overlays_.reset();
            refs_.reset();
            refs_va_.reset();
            selected_va_.reset();
            selected_import_.reset();
            selected_string_va_.reset();
            nav_back_.clear();
            identity_.reset();
            section_ = 0;
        }
        const ProgramKey key{program};
        strings_.update(ctx.jobs, key, [program] {
            return [program] { return std::make_shared<const std::vector<ImageString>>(scan_strings(program->image(), {}, &program->symbols())); };
        });
        if (strings_.value() && strings_.current(key)) {
            const Strings strings = *strings_.value();
            string_refs_.update(ctx.jobs, DerivedKey{key, strings_generation_}, [program, strings] {
                return [program, strings] { return std::make_shared<const std::vector<StringRefs>>(string_refs(*program, *strings)); };
            });
            overlays_.update(ctx.jobs, DerivedKey{key, strings_generation_}, [program, strings] {
                return [program, strings](const CancelToken& token) {
                    return std::make_shared<const vm::HexOverlays>(vm::build_hex_overlays(*program, *strings, [&token] { return token.cancelled(); }));
                };
            });
        }
        if (refs_va_) {
            const u64 va = *refs_va_;
            refs_.update(ctx.jobs, AddressKey{key, va}, [program, va] { return [program, va] { return program->xrefs_to(va); }; });
        }
    }

    // Selects an address in the hex view and scrolls to it.
    void go_to(const Program& program, u64 va) {
        const auto& sections = program.image().image_sections();
        for (usize i = 0; i < sections.size(); ++i)
            if (sections[i].contains(va)) {
                section_ = static_cast<int>(i);
                selected_va_ = va;
                refs_va_ = va;
                scroll_row_ = static_cast<usize>((va - sections[i].va) / 16);
                tab_request_ = kHex;
                return;
            }
    }

    void draw_toolbar(ViewContext& ctx, const ProjectAccess& access) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        const bool enter = ImGui::InputTextWithHint("##goto", "Address or symbol", &goto_text_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Go") || enter) {
            if (auto va = vm::parse_address(*access.program, goto_text_)) go_to(*access.program, *va);
            else ctx.notify(Severity::warning, std::format("Not an address in the image: {}", goto_text_));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!nav_back_.size());
        if (ImGui::Button("Back")) {
            const u64 va = nav_back_.back();
            nav_back_.pop_back();
            go_to(*access.program, va);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Back to the address before the last followed reference.");
        busy_marker(ctx, strings_.busy() || overlays_.busy() || string_refs_.busy(), "scanning...");
    }

    // A followed cross-reference remembers where it came from.
    void follow(const Program& program, u64 to) {
        if (selected_va_) nav_back_.push_back(*selected_va_);
        if (nav_back_.size() > 64) nav_back_.erase(nav_back_.begin());
        go_to(program, to);
    }

    // ---- references of a selected address --------------------------------------------------------------

    void draw_references(ViewContext& ctx, const ProjectAccess& access, u64 va) {
        if (refs_va_ != va) refs_va_ = va;
        if (!refs_.value() || !refs_.current(AddressKey{ProgramKey{access.program}, va})) {
            ImGui::TextDisabled("Finding references (the first time scans every function)...");
            return;
        }
        const std::vector<Xref>& refs = *refs_.value();
        if (refs.empty()) {
            ImGui::TextDisabled("Nothing references %s.", hex(va, 8).c_str());
            return;
        }
        ImGui::TextDisabled("%zu reference(s):", refs.size());
        const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<usize>(refs.size(), 8) + 1);
        if (!ImGui::BeginTable("##refs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, height))) return;
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(refs.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Xref& x = refs[static_cast<usize>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                if (ImGui::TextLink(std::format("{:08x}", x.from).c_str())) follow(*access.program, x.from);
                ImGui::PopFont();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(to_string(x.kind)).c_str());
                ImGui::TableNextColumn();
                if (x.function) function_link(ctx, access.program->describe_address(x.function), x.function);
                else ImGui::TextDisabled("-");
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    // ---- tabs -----------------------------------------------------------------------------------------

    void draw_sections(ViewContext& ctx, const ProjectAccess& access) {
        const auto rows = vm::section_rows(access.program->image());
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##sections", 6, flags)) return;
        for (const char* h : {"Name", "Address", "Virtual size", "Raw size", "File offset", "Characteristics"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            if (ImGui::TextLink(hex(r.va, 8).c_str())) go_to(*access.program, r.va);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::Text("%#x", r.virtual_size);
            ImGui::TableNextColumn();
            ImGui::Text("%#x", r.raw_size);
            ImGui::TableNextColumn();
            ImGui::Text("%#x", r.raw_offset);
            ImGui::TableNextColumn();
            ImGui::Text("%08x  %s", r.characteristics, r.flags.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void draw_imports(ViewContext& ctx, const ProjectAccess& access) {
        const auto& imports = access.program->image().imports();
        if (imports.empty()) {
            ImGui::TextDisabled("The image imports nothing.");
            return;
        }
        const float details = selected_import_ ? ImGui::GetTextLineHeightWithSpacing() * 11 : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##imports", 3, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y - details, ImGui::GetFontSize() * 6)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("DLL");
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Slot");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(imports.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const pe::Import& imp = imports[static_cast<usize>(i)];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(imp.dll.c_str(), selected_import_ == imp.iat_va, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                        selected_import_ = imp.iat_va;
                        refs_va_ = imp.iat_va;
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(imp.name.empty() ? std::format("ordinal {}", imp.ordinal.value_or(0)).c_str() : imp.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushFont(ctx.fonts.mono, 0.0f);
                    if (ImGui::TextLink(hex(imp.iat_va, 8).c_str())) go_to(*access.program, imp.iat_va);
                    ImGui::PopFont();
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        if (selected_import_) {
            ImGui::SeparatorText(std::format("Calls through the slot at {}", hex(*selected_import_, 8)).c_str());
            draw_references(ctx, access, *selected_import_);
        }
    }

    void draw_exports(ViewContext& ctx, const ProjectAccess& access) {
        const auto& image = access.program->image();
        const auto& exports = image.exports();
        if (exports.empty()) {
            ImGui::TextDisabled("The image exports nothing.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("##exports", 4, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ordinal");
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Forwarder");
        ImGui::TableHeadersRow();
        for (usize i = 0; i < exports.size(); ++i) {
            const pe::Export& e = exports[i];
            const u64 va = image.image_base() + e.rva;
            const Symbol* s = access.program->symbols().at(va);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", e.ordinal);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name.empty() ? "(by ordinal)" : e.name.c_str());
            ImGui::TableNextColumn();
            if (e.forwarder) {
                ImGui::TextDisabled("-");
            } else {
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                if (s && s->kind == SymbolKind::function) function_link(ctx, hex(va, 8), va);
                else if (ImGui::TextLink(hex(va, 8).c_str())) go_to(*access.program, va);
                ImGui::PopFont();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.forwarder ? e.forwarder->c_str() : "");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void draw_strings(ViewContext& ctx, const ProjectAccess& access) {
        if (!strings_.value() || !strings_.current(ProgramKey{access.program})) {
            ImGui::TextDisabled("Scanning the data sections for strings...");
            return;
        }
        const auto& strings = **strings_.value();
        const std::vector<StringRefs>* refs = string_refs_.value() && string_refs_.current(DerivedKey{ProgramKey{access.program}, strings_generation_})
                                                  ? &**string_refs_.value()
                                                  : nullptr;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        if (ImGui::InputTextWithHint("##string_filter", "Filter text", &string_filter_)) string_filter_key_ = ~u64{0};
        // The filtered list is rebuilt only when the filter or the scan changes.
        if (string_filter_key_ != strings_generation_) {
            string_filter_key_ = strings_generation_;
            filtered_strings_.clear();
            const std::string needle = to_lower(trim(string_filter_));
            for (usize i = 0; i < strings.size(); ++i)
                if (needle.empty() || to_lower(strings[i].text).find(needle) != std::string::npos) filtered_strings_.push_back(static_cast<u32>(i));
            if (selected_string_va_) {
                for (usize n = 0; n < filtered_strings_.size(); ++n)
                    if (strings[filtered_strings_[n]].va == *selected_string_va_) string_scroll_ = n;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %zu strings%s", filtered_strings_.size(), strings.size(), refs ? "" : " (finding references...)");
        const float details = selected_string_va_ ? ImGui::GetTextLineHeightWithSpacing() * 11 : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##strings", 5, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y - details, ImGui::GetFontSize() * 6)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Section");
            ImGui::TableSetupColumn("Refs");
            ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(filtered_strings_.size()));
            if (string_scroll_) clipper.IncludeItemByIndex(static_cast<int>(*string_scroll_));
            while (clipper.Step())
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const u32 i = filtered_strings_[static_cast<usize>(n)];
                    const ImageString& s = strings[i];
                    ImGui::PushID(n);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushFont(ctx.fonts.mono, 0.0f);
                    if (ImGui::Selectable(std::format("{:08x}", s.va).c_str(), selected_string_va_ == s.va,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                        selected_string_va_ = s.va;
                    ImGui::PopFont();
                    if (ImGui::BeginPopupContextItem("##string_menu")) {
                        if (ImGui::MenuItem("Show in the hex view")) go_to(*access.program, s.va);
                        if (ImGui::MenuItem("Create or rename the symbol here...")) editor_.open(access, s.va, "Binary explorer");
                        if (ImGui::MenuItem("Copy text")) ImGui::SetClipboardText(s.text.c_str());
                        ImGui::EndPopup();
                    }
                    if (string_scroll_ && *string_scroll_ == static_cast<usize>(n)) {
                        ImGui::SetScrollHereY(0.3f);
                        string_scroll_.reset();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.encoding == StringEncoding::ascii ? "ascii" : "utf-16");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.section.c_str());
                    ImGui::TableNextColumn();
                    if (refs && i < refs->size()) ImGui::Text("%zu", (*refs)[i].xrefs.size());
                    else ImGui::TextDisabled("...");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(escape_c_string(s.text).c_str());
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        if (selected_string_va_) {
            ImGui::SeparatorText(std::format("References to the string at {}", hex(*selected_string_va_, 8)).c_str());
            draw_references(ctx, access, *selected_string_va_);
        }
    }

    void draw_hex(ViewContext& ctx, const ProjectAccess& access) {
        const pe::Image& image = access.program->image();
        const auto& sections = image.image_sections();
        if (sections.empty()) {
            ImGui::TextDisabled("The image has no sections.");
            return;
        }
        section_ = std::clamp(section_, 0, static_cast<int>(sections.size()) - 1);
        const ImageSection& sec = sections[static_cast<usize>(section_)];
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        if (ImGui::BeginCombo("##section", sec.name.c_str())) {
            for (usize i = 0; i < sections.size(); ++i)
                if (ImGui::Selectable(std::format("{}##{}", sections[i].name, i).c_str(), static_cast<int>(i) == section_)) {
                    section_ = static_cast<int>(i);
                    scroll_row_ = 0;
                }
            ImGui::EndCombo();
        }
        // The legend names every overlay color.
        for (vm::OverlayKind k : kKinds) {
            ImGui::SameLine();
            status_label(vm::to_string(k), overlay_color(ctx, k));
        }
        const vm::HexOverlays* overlays = overlays_.value() ? overlays_.value()->get() : nullptr;
        if (!overlays) {
            ImGui::SameLine();
            ImGui::TextDisabled("(finding overlays...)");
        }

        const float details = ImGui::GetTextLineHeightWithSpacing() * 13;
        if (ImGui::BeginChild("##hex", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y - details, ImGui::GetFontSize() * 8)), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            draw_hex_rows(ctx, image, sec, overlays);
        }
        ImGui::EndChild();
        draw_hex_details(ctx, access, overlays);
    }

    void draw_hex_rows(ViewContext& ctx, const pe::Image& image, const ImageSection& sec, const vm::HexOverlays* overlays) {
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        const ThemeColors& c = ctx.colors();
        const float cw = ImGui::CalcTextSize("0").x;
        const float line_h = ImGui::GetTextLineHeight();
        const u64 extent = std::max(sec.virtual_size, sec.file_size);
        const auto rows = static_cast<int>((extent + 15) / 16);
        const float x_hex = cw * 10, x_ascii = x_hex + cw * 49, x_note = x_ascii + cw * 17;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 muted = ImGui::GetColorU32(c.muted);
        ImU32 kind_colors[vm::kOverlayKinds];
        for (usize k = 0; k < vm::kOverlayKinds; ++k) kind_colors[k] = ImGui::GetColorU32(overlay_color(ctx, static_cast<vm::OverlayKind>(k)));
        ImGuiListClipper clipper;
        clipper.Begin(rows);
        if (scroll_row_ && *scroll_row_ < static_cast<usize>(rows)) clipper.IncludeItemByIndex(static_cast<int>(*scroll_row_));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const u64 row_va = sec.va + static_cast<u64>(r) * 16;
                const usize count = static_cast<usize>(std::min<u64>(16, sec.va + extent - row_va));
                ImGui::PushID(r);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##row", ImVec2(x_note, line_h));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    const float mx = ImGui::GetIO().MousePos.x - pos.x;
                    int b = -1;
                    if (mx >= x_hex && mx < x_hex + cw * 48) b = static_cast<int>((mx - x_hex) / (cw * 3));
                    else if (mx >= x_ascii && mx < x_ascii + cw * 16) b = static_cast<int>((mx - x_ascii) / cw);
                    if (b >= 0 && static_cast<usize>(b) < count) {
                        selected_va_ = row_va + static_cast<u64>(b);
                        refs_va_ = selected_va_;
                    }
                }
                if (ImGui::BeginPopupContextItem("##hex_menu")) {
                    if (selected_va_) {
                        ImGui::TextDisabled("%s", hex(*selected_va_, 8).c_str());
                        if (ImGui::MenuItem("Create or rename a symbol here...")) open_editor_at_ = *selected_va_;
                        if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(hex(*selected_va_, 8).c_str());
                    }
                    ImGui::EndPopup();
                }
                const float y = pos.y;
                dl->AddText(ImVec2(pos.x, y), muted, std::format("{:08x}", row_va).c_str());
                std::vector<const vm::OverlaySpan*> spans;
                if (overlays) spans = overlays->overlapping(row_va, row_va + count);
                const auto bytes = image.view(row_va, count);
                const usize file_end = sec.file_size > row_va - sec.va ? static_cast<usize>(std::min<u64>(count, sec.file_size - (row_va - sec.va))) : 0;
                std::string ascii;
                for (usize b = 0; b < count; ++b) {
                    const u64 va = row_va + b;
                    std::optional<u8> value;
                    if (b < file_end) {
                        if (bytes) value = static_cast<u8>((*bytes)[b]);
                        else if (auto one = image.read<u8>(va)) value = *one;
                    }
                    // The topmost overlay covering the byte gives its color.
                    const vm::OverlaySpan* top = nullptr;
                    bool relocated = false;
                    for (const auto* s : spans)
                        if (va >= s->begin && va < s->end) {
                            if (s->kind == vm::OverlayKind::relocation) relocated = true;
                            else if (!top || s->kind > top->kind) top = s;
                        }
                    const ImU32 color = top ? kind_colors[static_cast<usize>(top->kind)] : muted;
                    const float x = pos.x + x_hex + cw * 3 * static_cast<float>(b);
                    if (selected_va_ && *selected_va_ == va)
                        dl->AddRectFilled(ImVec2(x - 1, y), ImVec2(x + cw * 2 + 1, y + line_h), ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
                    char text[3] = {'.', '.', 0};
                    if (value) {
                        static constexpr char digits[] = "0123456789abcdef";
                        text[0] = digits[*value >> 4];
                        text[1] = digits[*value & 15];
                    }
                    dl->AddText(ImVec2(x, y), value ? color : muted, text);
                    if (relocated) dl->AddLine(ImVec2(x, y + line_h - 2), ImVec2(x + cw * 2, y + line_h - 2), kind_colors[static_cast<usize>(vm::OverlayKind::relocation)]);
                    ascii += value && *value >= 0x20 && *value < 0x7F ? static_cast<char>(*value) : '.';
                }
                dl->AddText(ImVec2(pos.x + x_ascii, y), muted, ascii.c_str());
                // What starts in this row, by name.
                std::string note;
                for (const auto* s : spans)
                    if (s->begin >= row_va && s->begin < row_va + count && s->kind != vm::OverlayKind::relocation) {
                        if (!note.empty()) note += "; ";
                        note += std::format("{} {}", vm::to_string(s->kind), s->label.size() > 60 ? s->label.substr(0, 57) + "..." : s->label);
                    }
                if (!note.empty()) {
                    ImGui::SameLine(0, 0);
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + x_note, y));
                    ImGui::TextUnformatted(note.c_str());
                }
                if (scroll_row_ && *scroll_row_ == static_cast<usize>(r)) {
                    ImGui::SetScrollHereY(0.3f);
                    scroll_row_.reset();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopFont();
    }

    void draw_hex_details(ViewContext& ctx, const ProjectAccess& access, const vm::HexOverlays* overlays) {
        if (open_editor_at_) {
            editor_.open(access, *open_editor_at_, "Binary explorer");
            open_editor_at_.reset();
        }
        if (!selected_va_) {
            ImGui::TextDisabled("Click a byte to see what it belongs to and who references it; right-click to name it.");
            return;
        }
        const u64 va = *selected_va_;
        const Program& program = *access.program;
        ImGui::Text("%s: %s", hex(va, 8).c_str(), program.describe_address(va).c_str());
        if (overlays) {
            auto spans = overlays->overlapping(va, va + 1);
            for (const auto* s : spans) {
                ImGui::SameLine();
                status_label(std::format("{} {}", vm::to_string(s->kind), s->label.size() > 40 ? s->label.substr(0, 37) + "..." : s->label),
                             overlay_color(ctx, s->kind));
            }
        }
        const Symbol* fn = program.symbols().containing(va);
        if (fn && fn->kind != SymbolKind::function) fn = nullptr;
        ImGui::BeginDisabled(!fn);
        if (ImGui::SmallButton("Inspector")) ctx.open("inspector", {.va = fn->va});
        ImGui::SameLine();
        if (ImGui::SmallButton("Diff viewer")) ctx.open("diff_viewer", {.va = fn->va});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Symbol...")) editor_.open(access, program.symbols().at(va) ? va : (fn ? fn->va : va), "Binary explorer");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Create or rename the symbol at this address.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Symbols view")) ctx.open("symbols", {.anchor = hex(va)});
        draw_references(ctx, access, va);
    }

    void draw_rich(const ProjectAccess& access) {
        const auto builds = vm::rich_builds(access.program->image().rich_entries());
        if (builds.empty()) {
            ImGui::TextDisabled("No Rich header (the image was not linked by Microsoft's linker).");
            return;
        }
        if (!ImGui::BeginTable("##rich", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) return;
        for (const char* h : {"Product", "Build", "Count", "Description"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const auto& b : builds) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%#06x", static_cast<unsigned>(b.product_id));
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(b.build));
            ImGui::TableNextColumn();
            ImGui::Text("%u", b.count);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.description.c_str());
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Compiler names for more product ids come with Phase 2.");
    }

    void draw_pdb(ViewContext& ctx, const ProjectAccess& access) {
        if (!identity_ || !(identity_->first == ProgramKey{access.program})) {
            const auto status = access.workspace ? access.workspace->target_status() : std::nullopt;
            identity_.emplace(ProgramKey{access.program}, vm::target_identity(*access.program, status ? &*status : nullptr));
        }
        const vm::TargetIdentity& t = identity_->second;
        const ThemeColors& c = ctx.colors();
        if (!ImGui::BeginTable("##pdb", 2, ImGuiTableFlags_SizingFixedFit)) return;
        auto row = [](const char* key) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
        };
        row("Match state");
        status_label(vm::pdb_status_text(t.pdb), t.pdb == PdbStatus::matched ? c.ok : t.pdb == PdbStatus::absent ? c.muted : c.warn);
        row("Loaded from");
        ImGui::TextUnformatted(t.pdb_path.empty() ? "-" : t.pdb_path.c_str());
        row("Image records");
        ImGui::TextUnformatted(t.has_codeview ? t.codeview_path.c_str() : "no CodeView record");
        row("GUID");
        ImGui::TextUnformatted(t.guid.empty() ? "-" : t.guid.c_str());
        row("Age");
        ImGui::Text("%u", t.age);
        if (!t.pdb_detail.empty()) {
            row("Detail");
            ImGui::TextWrapped("%s", t.pdb_detail.c_str());
        }
        ImGui::EndTable();
    }

    KeyedJob<ProgramKey, Strings> strings_;
    u64 strings_generation_ = 0;
    KeyedJob<DerivedKey, std::shared_ptr<const std::vector<StringRefs>>> string_refs_;
    KeyedJob<DerivedKey, std::shared_ptr<const vm::HexOverlays>> overlays_;
    KeyedJob<AddressKey, std::vector<Xref>> refs_;
    std::optional<u64> refs_va_;
    std::optional<std::string> pending_anchor_;
    int tab_request_ = -1;
    std::string goto_text_;
    std::vector<u64> nav_back_;
    int section_ = 0;
    std::optional<u64> selected_va_, scroll_row_, selected_import_, selected_string_va_, open_editor_at_;
    std::string string_filter_;
    u64 string_filter_key_ = ~u64{0};
    std::vector<u32> filtered_strings_;
    std::optional<usize> string_scroll_;
    SymbolEditor editor_;
    std::optional<std::pair<ProgramKey, vm::TargetIdentity>> identity_;
    std::string project_root_;
};

} // namespace

std::unique_ptr<View> make_binary_explorer_view() { return std::make_unique<BinaryExplorerView>(); }

} // namespace decomp::gui
