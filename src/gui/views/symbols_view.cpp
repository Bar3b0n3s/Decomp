// Symbols and provenance (docs/ui.md#symbols-and-provenance): every symbol in a virtualized, filterable
// table; who set each one and when (.decomp/symbols.log.jsonl and the shown run's symbol_changed
// events); the audit trail of agent edits by session; renaming and editing symbols; reverting agent
// edits one at a time or per session (as user changes).

#include "gui/views/symbols_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/symbol_editor.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/symbols_table.hpp"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace decomp::gui {

namespace {

using Log = std::shared_ptr<const std::vector<vm::SymbolLogRecord>>;
using Rows = std::shared_ptr<const std::vector<vm::SymbolRow>>;

struct LogKey {
    std::string root;
    u64 version = 0;
    bool operator==(const LogKey&) const = default;
};

struct RowsKey {
    ProjectInputs project;
    u64 log = 0;
    bool operator==(const RowsKey&) const = default;
};

struct OrderKey {
    u64 rows = 0, filter = 0, sort = 0;
    bool operator==(const OrderKey&) const = default;
};

struct OrderData {
    Rows rows;
    std::vector<u32> order;
};

struct RevertOutcome {
    usize done = 0, total = 0;
    std::string session = {}, error = {};
};

constexpr std::array<SymbolKind, 7> kKinds = {SymbolKind::function, SymbolKind::data,  SymbolKind::string, SymbolKind::float_const,
                                              SymbolKind::import,   SymbolKind::label, SymbolKind::unknown};
constexpr std::array<SymbolSource, 7> kSources = {SymbolSource::analysis, SymbolSource::import_table, SymbolSource::export_table, SymbolSource::pdb_public,
                                                  SymbolSource::pdb,      SymbolSource::agent,        SymbolSource::user};

struct ColumnSpec {
    vm::SymbolColumn column;
    const char* label;
    float width;  // in font sizes
};
constexpr std::array<ColumnSpec, 8> kColumns = {{
    {vm::SymbolColumn::address, "Address", 6.5f},
    {vm::SymbolColumn::kind, "Kind", 4.5f},
    {vm::SymbolColumn::display, "Name", 18.0f},
    {vm::SymbolColumn::name, "Decorated name", 16.0f},
    {vm::SymbolColumn::size, "Size", 4.0f},
    {vm::SymbolColumn::source, "Source", 5.5f},
    {vm::SymbolColumn::status, "Status", 7.0f},
    {vm::SymbolColumn::edits, "Edits", 3.5f},
}};

template <class T, usize N>
Json enum_list(const std::vector<T>& values, const std::array<T, N>& all) {
    Json out = Json::array();
    for (T v : values)
        if (std::ranges::find(all, v) != all.end()) out.push_back(std::string(to_string(v)));
    return out;
}

class SymbolsView final : public View {
public:
    std::string_view id() const override { return "symbols"; }
    std::string_view title() const override { return "Symbols and provenance"; }

    void navigate(ViewContext& ctx, const NavTarget& target) override {
        load_state(ctx);
        if (target.anchor == "agent_edits") {
            tab_request_ = 1;
            return;
        }
        std::optional<u64> va = target.va;
        if (!va && !target.anchor.empty()) va = parse_u64(target.anchor);
        if (!va) return;
        selected_ = va;
        scroll_to_ = va;
        tab_request_ = 0;
    }

    void draw(ViewContext& ctx) override {
        load_state(ctx);
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "Symbols and provenance lists every symbol with its kind, names, size, source and status, who set it and "
                             "when, and the agent's edits, which can be reverted.")) {
            return;
        }
        poll_revert(ctx, access);
        update_jobs(ctx, access);
        draw_filters(ctx);
        if (ImGui::BeginTabBar("##symbols_tabs")) {
            if (ImGui::BeginTabItem("Symbols", nullptr, tab_request_ == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                draw_symbols(ctx, access);
                ImGui::EndTabItem();
            }
            const usize agent_edits = log_.value() ? static_cast<usize>(std::ranges::count_if(**log_.value(), &vm::SymbolLogRecord::by_agent)) : 0;
            if (ImGui::BeginTabItem(std::format("Agent edits ({})###agent_edits", agent_edits).c_str(), nullptr,
                                    tab_request_ == 1 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                draw_agent_edits(ctx, access);
                ImGui::EndTabItem();
            }
            tab_request_ = -1;
            ImGui::EndTabBar();
        }
        draw_confirm(ctx, access);
        editor_.draw(ctx, access);
    }

private:
    // ---- state ----------------------------------------------------------------------------------------

    void load_state(ViewContext& ctx) {
        const std::string key = fs::to_utf8(ctx.project.root);
        if (state_loaded_ && key == state_project_) return;
        state_loaded_ = true;
        state_project_ = key;
        const Json& s = ctx.view_state(id());
        filter_ = {};
        filter_.text = json_string_or(s, "text", "");
        filter_.edited_only = json_bool_or(s, "edited_only", false);
        if (auto it = s.find("kinds"); it != s.end() && it->is_array())
            for (const auto& k : *it)
                if (auto kind = k.is_string() ? symbol_kind_from_string(k.get<std::string>()) : std::nullopt) filter_.kinds.push_back(*kind);
        if (auto it = s.find("sources"); it != s.end() && it->is_array())
            for (const auto& k : *it)
                if (auto source = k.is_string() ? symbol_source_from_string(k.get<std::string>()) : std::nullopt) filter_.sources.push_back(*source);
        sort_.clear();
        if (auto it = s.find("sort"); it != s.end() && it->is_array())
            for (const auto& k : *it)
                if (auto column = vm::symbol_column_from_string(json_string_or(k, "column", "")))
                    sort_.push_back({*column, json_bool_or(k, "descending", false)});
        apply_sort_ = true;
        selected_.reset();
        ++filter_version_;
        ++sort_version_;
    }

    void save_state(ViewContext& ctx) {
        Json& s = ctx.view_state(id());
        s["text"] = filter_.text;
        s["edited_only"] = filter_.edited_only;
        s["kinds"] = enum_list(filter_.kinds, kKinds);
        s["sources"] = enum_list(filter_.sources, kSources);
        Json sort = Json::array();
        for (const auto& k : sort_) sort.push_back(Json{{"column", std::string(vm::to_string(k.column))}, {"descending", k.descending}});
        s["sort"] = std::move(sort);
        ctx.mark_settings_dirty();
    }

    // ---- jobs -----------------------------------------------------------------------------------------

    void update_jobs(ViewContext& ctx, const ProjectAccess& access) {
        if (log_.poll()) ++log_generation_;
        if (rows_.poll()) ++rows_generation_;
        order_.poll();
        const project::Project& project = *access.project;  // copied into the jobs
        const auto program = access.program;
        if (const std::string root = fs::to_utf8(project.root()); root != project_root_) {
            project_root_ = root;
            log_.reset();
            rows_.reset();
            order_.reset();
        }
        log_.update(ctx.jobs, LogKey{fs::to_utf8(project.root()), project.version()},
                    [&] { return [project] { return std::make_shared<const std::vector<vm::SymbolLogRecord>>(vm::parse_symbol_log(project.symbol_log())); }; });
        if (!log_.value()) return;
        const Log log = *log_.value();
        rows_.update(ctx.jobs, RowsKey{project_inputs(access), log_generation_}, [&] {
            return [=] { return std::make_shared<const std::vector<vm::SymbolRow>>(vm::build_symbol_rows(program->symbols(), *project.function_infos(), log.get())); };
        });
        if (!rows_.value()) return;
        order_.update(ctx.jobs, OrderKey{rows_generation_, filter_version_, sort_version_}, [&] {
            return [rows = *rows_.value(), filter = filter_, sort = sort_](const CancelToken& token) {
                OrderData d;
                d.rows = rows;
                if (auto order = vm::filter_and_sort_symbols(*rows, filter, sort, [&token] { return token.cancelled(); })) d.order = std::move(*order);
                return d;
            };
        });
    }

    // ---- filters --------------------------------------------------------------------------------------

    template <class T, usize N>
    bool enum_filter(const char* label, std::vector<T>& chosen, const std::array<T, N>& all) {
        bool changed = false;
        const std::string text = chosen.empty() ? std::format("{}: all", label) : std::format("{}: {}", label, chosen.size());
        const std::string button = std::format("{}###{}", text, label);
        same_line_or_wrap(button_width(button));
        if (ImGui::Button(button.c_str())) ImGui::OpenPopup(label);
        if (ImGui::BeginPopup(label)) {
            for (T v : all) {
                bool on = std::ranges::find(chosen, v) != chosen.end();
                if (ImGui::Checkbox(std::string(to_string(v)).c_str(), &on)) {
                    if (on) chosen.push_back(v);
                    else std::erase(chosen, v);
                    changed = true;
                }
            }
            if (ImGui::Button("All")) {
                chosen.clear();
                changed = true;
            }
            ImGui::EndPopup();
        }
        return changed;
    }

    void draw_filters(ViewContext& ctx) {
        bool changed = false;
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 14));
        changed |= ImGui::InputTextWithHint("##text", "Name or address (0x...)", &filter_.text);
        changed |= enum_filter("Kind", filter_.kinds, kKinds);
        changed |= enum_filter("Source", filter_.sources, kSources);
        same_line_or_wrap(checkbox_width("Edited only"));
        changed |= ImGui::Checkbox("Edited only", &filter_.edited_only);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Only symbols with records in .decomp/symbols.log.jsonl");
        if (changed) {
            ++filter_version_;
            save_state(ctx);
        }
        if (order_.value() && rows_.value()) {
            const std::string count = std::format("{} of {} symbols", order_.value()->order.size(), (*rows_.value())->size());
            same_line_or_wrap(ImGui::CalcTextSize(count.c_str()).x);
            ImGui::TextDisabled("%s", count.c_str());
        }
        busy_marker(ctx, log_.busy() || rows_.busy() || order_.busy() || revert_.valid());
    }

    // ---- the table ------------------------------------------------------------------------------------

    void draw_symbols(ViewContext& ctx, const ProjectAccess& access) {
        if (!order_.value() || !order_.value()->rows) {
            ImGui::TextDisabled("Listing the symbols...");
            return;
        }
        const OrderData& d = *order_.value();
        const auto& rows = *d.rows;
        const float details = selected_ ? std::max(ImGui::GetContentRegionAvail().y * 0.38f, ImGui::GetFontSize() * 10) : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollX |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                      ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_SizingFixedFit;
        const float height = std::max(ImGui::GetContentRegionAvail().y - details, ImGui::GetFontSize() * 6);
        if (ImGui::BeginTable("##symbol_table", static_cast<int>(kColumns.size()), flags, ImVec2(0, height))) {
            const float font = ImGui::GetFontSize();
            ImGui::TableSetupScrollFreeze(1, 1);
            for (const auto& c : kColumns) {
                ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed;
                if (c.column == vm::SymbolColumn::address) cf |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_DefaultSort;
                ImGui::TableSetupColumn(c.label, cf, c.width * font, static_cast<ImGuiID>(c.column));
            }
            if (apply_sort_ && !ImGui::GetCurrentTable()->IsInitializing) {
                for (usize k = 0; k < sort_.size(); ++k)
                    for (usize i = 0; i < kColumns.size(); ++i)
                        if (kColumns[i].column == sort_[k].column)
                            ImGui::TableSetColumnSortDirection(static_cast<int>(i), sort_[k].descending ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending,
                                                               k > 0);
                apply_sort_ = false;
            }
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* specs = apply_sort_ ? nullptr : ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
                std::vector<vm::SymbolSortKey> keys;
                for (int i = 0; i < specs->SpecsCount; ++i)
                    keys.push_back({static_cast<vm::SymbolColumn>(specs->Specs[i].ColumnUserID), specs->Specs[i].SortDirection == ImGuiSortDirection_Descending});
                specs->SpecsDirty = false;
                const bool same = keys.size() == sort_.size() && std::equal(keys.begin(), keys.end(), sort_.begin(), [](const auto& a, const auto& b) {
                                      return a.column == b.column && a.descending == b.descending;
                                  });
                if (!same) {
                    sort_ = std::move(keys);
                    ++sort_version_;
                    save_state(ctx);
                }
            }
            std::optional<int> scroll_index;
            if (scroll_to_ && order_.current(OrderKey{rows_generation_, filter_version_, sort_version_})) {
                for (usize n = 0; n < d.order.size() && !scroll_index; ++n)
                    if (rows[d.order[n]].va == *scroll_to_) scroll_index = static_cast<int>(n);
                const bool filtered = !filter_.text.empty() || !filter_.kinds.empty() || !filter_.sources.empty() || filter_.edited_only;
                if (!scroll_index && filtered && std::ranges::any_of(rows, [&](const vm::SymbolRow& r) { return r.va == *scroll_to_; })) {
                    filter_ = {};  // the symbol is filtered out: show everything (it scrolls there once listed)
                    ++filter_version_;
                    save_state(ctx);
                } else {
                    scroll_to_.reset();
                }
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(d.order.size()));
            if (scroll_index) clipper.IncludeItemByIndex(*scroll_index);
            while (clipper.Step()) {
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const vm::SymbolRow& r = rows[d.order[static_cast<usize>(n)]];
                    ImGui::PushID(n);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushFont(ctx.fonts.mono, 0.0f);
                    if (ImGui::Selectable(std::format("{:08x}", r.va).c_str(), selected_ == r.va, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                        selected_ = r.va;
                    ImGui::PopFont();
                    if (ImGui::BeginPopupContextItem("##symbol_menu")) {
                        selected_ = r.va;
                        if (ImGui::MenuItem("Rename or edit...")) editor_.open(access, r.va, "Symbols view");
                        if (r.kind == SymbolKind::function) {
                            ImGui::Separator();
                            function_open_items(ctx, r.va);
                        } else if (ImGui::MenuItem("Show in Binary explorer")) {
                            ctx.open("binary_explorer", {.anchor = hex(r.va)});
                        }
                        ImGui::EndPopup();
                    }
                    if (scroll_index && *scroll_index == n) ImGui::SetScrollHereY(0.4f);
                    if (ImGui::TableSetColumnIndex(1)) ImGui::TextUnformatted(std::string(to_string(r.kind)).c_str());
                    if (ImGui::TableSetColumnIndex(2)) ImGui::TextUnformatted(r.display.c_str());
                    if (ImGui::TableSetColumnIndex(3)) ImGui::TextUnformatted(r.name.c_str());
                    if (ImGui::TableSetColumnIndex(4)) ImGui::Text("%u", r.size);
                    if (ImGui::TableSetColumnIndex(5)) ImGui::TextUnformatted(std::string(to_string(r.source)).c_str());
                    if (ImGui::TableSetColumnIndex(6)) {
                        if (r.status) status_cell(ctx, *r.status);
                    }
                    if (ImGui::TableSetColumnIndex(7)) {
                        if (r.edits) ImGui::Text("%u%s", r.edits, r.agent_edited ? " (agent)" : "");
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (selected_) draw_details(ctx, access, *selected_);
    }

    void draw_details(ViewContext& ctx, const ProjectAccess& access, u64 va) {
        const Symbol* s = access.program->symbols().at(va);
        ImGui::SeparatorText(s ? std::format("{} at {}", s->display.empty() ? s->name : s->display, hex(va, 8)).c_str()
                               : std::format("No symbol at {}", hex(va, 8)).c_str());
        if (s) {
            ImGui::TextDisabled("%s %s, %u bytes, set by %s%s", std::string(to_string(s->kind)).c_str(), s->name.c_str(), s->size,
                                std::string(to_string(s->source)).c_str(), s->aliases.empty() ? "" : std::format(", also known as {}", join(s->aliases, ", ")).c_str());
        }
        if (ImGui::SmallButton(s ? "Rename or edit..." : "Create...")) editor_.open(access, va, "Symbols view");
        ImGui::SameLine();
        if (s && s->kind == SymbolKind::function) {
            if (ImGui::SmallButton("Inspector")) ctx.open("inspector", {.va = va});
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Binary explorer")) ctx.open("binary_explorer", {.anchor = hex(va)});
        if (!log_.value()) return;
        const auto& log = **log_.value();
        const auto entries = vm::symbol_provenance(va, log, ctx.snapshot.get());
        if (entries.empty()) {
            ImGui::TextDisabled("No recorded edit: the symbol comes from %s (symbols.txt edits by hand are not logged).",
                                s ? std::string(to_string(s->source)).c_str() : "nowhere");
            return;
        }
        provenance_table(ctx, access, log, entries);
    }

    void provenance_table(ViewContext& ctx, const ProjectAccess& access, const std::vector<vm::SymbolLogRecord>& log,
                          const std::vector<vm::ProvenanceEntry>& entries) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("##provenance", 5, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 4)))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("When");
        ImGui::TableSetupColumn("By");
        ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Reason");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (usize i = entries.size(); i-- > 0;) {
            const vm::ProvenanceEntry& e = entries[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(local_date_time(e.time).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(std::string(to_string(e.source)).c_str());
            if (!e.session.empty()) {
                ImGui::SameLine();
                const u64 va = e.record ? log[*e.record].va : 0;
                if (ImGui::TextLink(std::format("session {}", e.session).c_str()))
                    ctx.open("agent_session", {.va = va ? std::optional<u64>(va) : std::nullopt, .session = e.session});
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.what.c_str());
            if (!e.record) {
                ImGui::SameLine();
                ImGui::TextDisabled("(run event)");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.reason.c_str());
            ImGui::TableNextColumn();
            if (e.record && log[*e.record].by_agent()) {
                if (ImGui::SmallButton("Revert")) revert(ctx, access, {*e.record}, "");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Restore the symbol as it was before this edit (a user change).");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- agent edits ----------------------------------------------------------------------------------

    void draw_agent_edits(ViewContext& ctx, const ProjectAccess& access) {
        if (!log_.value()) {
            ImGui::TextDisabled("Reading the symbol log...");
            return;
        }
        const auto& log = **log_.value();
        const auto groups = vm::agent_edit_groups(log);
        if (groups.empty()) {
            ImGui::TextDisabled("The agent has not changed any symbol. Its edits (set_symbol, bindings recorded on a match) appear here by session.");
            return;
        }
        for (usize g = 0; g < groups.size(); ++g) {
            const vm::AgentEditGroup& group = groups[g];
            ImGui::PushID(static_cast<int>(g));
            const std::string session = group.session.empty() ? std::string("(no session)") : group.session;
            const bool open = ImGui::CollapsingHeader(std::format("Session {}: {} edit(s), {} - {}###group", session, group.records.size(),
                                                                  local_date_time(group.first), local_date_time(group.last))
                                                          .c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            if (open) {
                const u64 first_va = log[group.records.front()].va;
                if (!group.session.empty() && ImGui::SmallButton("Open the session")) ctx.open("agent_session", {.va = first_va, .session = group.session});
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert all of this session")) {
                    confirm_records_ = group.records;
                    confirm_session_ = session;
                    open_confirm_ = true;
                }
                const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
                if (ImGui::BeginTable("##edits", 5, flags)) {
                    ImGui::TableSetupColumn("When");
                    ImGui::TableSetupColumn("Symbol");
                    ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Reason");
                    ImGui::TableSetupColumn("");
                    ImGui::TableHeadersRow();
                    for (usize k = group.records.size(); k-- > 0;) {
                        const usize index = group.records[k];
                        const vm::SymbolLogRecord& r = log[index];
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(local_date_time(r.time).c_str());
                        ImGui::TableNextColumn();
                        ImGui::PushFont(ctx.fonts.mono, 0.0f);
                        if (ImGui::TextLink(hex(r.va, 8).c_str())) {
                            selected_ = r.va;
                            scroll_to_ = r.va;
                            tab_request_ = 0;
                        }
                        ImGui::PopFont();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(vm::describe_edit(r.before, r.after).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.reason.c_str());
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("Revert")) revert(ctx, access, {index}, "");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::PopID();
        }
    }

    // Restores the `before` of each record (newest first) as user changes; refused when a later change
    // touched the symbol (the plan names it). The edits run in the background, one revert at a time.
    void revert(ViewContext& ctx, const ProjectAccess& access, std::vector<usize> records, const std::string& session) {
        if (!log_.value() || revert_.valid()) return;
        const auto& log = **log_.value();
        std::map<u64, vm::SymbolState> current;
        for (const Symbol& s : access.project->symbols()) current.emplace(s.va, vm::symbol_state(s));
        auto plan = vm::plan_revert(log, records, [&](u64 va) -> std::optional<vm::SymbolState> {
            auto it = current.find(va);
            return it == current.end() ? std::nullopt : std::optional(it->second);
        });
        if (!plan) {
            ctx.notify(Severity::error, std::format("Cannot revert: {}", plan.error().message));
            return;
        }
        std::vector<std::pair<project::SymbolEdit, std::string>> steps;
        for (const auto& step : *plan) {
            const vm::SymbolLogRecord& r = log[step.record];
            steps.emplace_back(step.edit, std::format("revert of the agent's edit of {}{}", r.time_text, r.session.empty() ? "" : " in session " + r.session));
        }
        revert_ = ctx.jobs.submit([project = *access.project, steps = std::move(steps), session]() mutable {
            RevertOutcome out{.total = steps.size(), .session = session};
            for (const auto& [edit, reason] : steps) {
                if (auto r = project.set_symbol(edit, project::ChangeOrigin{SymbolSource::user, "", reason}); !r) {
                    out.error = r.error().message;
                    break;
                }
                ++out.done;
            }
            return out;
        });
    }

    void poll_revert(ViewContext& ctx, const ProjectAccess& access) {
        if (!revert_.valid()) return;
        std::optional<RevertOutcome> out;
        try {
            out = revert_.take();
        } catch (const std::exception& e) {
            out = RevertOutcome{.error = e.what()};
        }
        if (!out) return;
        revert_.reset();
        if (out->done > 0 && access.workspace) access.workspace->reload_symbols();
        if (!out->error.empty())
            ctx.notify(Severity::error, std::format("Reverted {} of {} edit(s), then failed: {}", out->done, out->total, out->error));
        else if (out->session.empty())
            ctx.notify(Severity::info, std::format("Reverted {} edit(s).", out->done));
        else
            ctx.notify(Severity::info, std::format("Reverted {} edit(s) of session {}.", out->done, out->session));
    }

    void draw_confirm(ViewContext& ctx, const ProjectAccess& access) {
        if (open_confirm_) {
            ImGui::OpenPopup("Revert###revert_session");
            open_confirm_ = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Revert###revert_session", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::Text("Revert the %zu symbol edit(s) of session %s?", confirm_records_.size(), confirm_session_.c_str());
        ImGui::TextDisabled("Each symbol gets back what it was before the session; the reverts are recorded as user changes.");
        if (ImGui::Button("Revert")) {
            revert(ctx, access, confirm_records_, confirm_session_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    vm::SymbolFilter filter_;
    std::vector<vm::SymbolSortKey> sort_;
    bool apply_sort_ = true;
    u64 filter_version_ = 0, sort_version_ = 0;
    bool state_loaded_ = false;
    std::string state_project_, project_root_;
    int tab_request_ = -1;
    std::optional<u64> selected_, scroll_to_;

    KeyedJob<LogKey, Log> log_;
    u64 log_generation_ = 0;
    KeyedJob<RowsKey, Rows> rows_;
    u64 rows_generation_ = 0;
    KeyedJob<OrderKey, OrderData> order_;

    bool open_confirm_ = false;
    std::vector<usize> confirm_records_;
    std::string confirm_session_;
    JobHandle<RevertOutcome> revert_;
    SymbolEditor editor_;
};

} // namespace

std::unique_ptr<View> make_symbols_view() { return std::make_unique<SymbolsView>(); }

} // namespace decomp::gui
