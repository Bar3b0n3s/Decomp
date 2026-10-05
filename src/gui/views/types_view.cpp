// Types (docs/ui.md#types): the program's types. The project's headers under include/ are the source of
// truth: their types as the compiler lays them out, beside the target's PDB's, with what differs; the
// selected type's layout and where the program's functions use its fields; declaring the PDB's types,
// or class skeletons from the RTTI, in a header.

#include "gui/views/types_view.hpp"

#include "analysis/declarations.hpp"
#include "analysis/typeflow.hpp"
#include "core/strings.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "project/setup.hpp"
#include "project/types.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <format>

namespace decomp::gui {

namespace {

struct TypesKey {
    ProjectInputs project;
    u64 refresh = 0;  // the Refresh button, and what this view wrote
    bool operator==(const TypesKey&) const = default;
};

struct TypeRow {
    std::string name;
    TypeLayout layout;                     // the headers' when they declare it, else the PDB's
    std::string header;                    // include/... when a header declares it
    bool in_pdb = false;
    std::vector<std::string> differences;  // from the PDB's layout
};

struct TypesData {
    std::vector<TypeRow> rows;  // the headers' types, then the PDB's others
    std::string headers_error;  // why the headers' types are unknown
    std::shared_ptr<const project::HeaderTypes> headers;
    usize declared = 0, differ = 0, pdb_only = 0;
    bool rtti = false;  // the RTTI names classes: skeletons can be made
};

struct UsesResult {
    std::string type;
    std::vector<FieldUse> uses;
};

struct WriteResult {
    bool ok = false;
    std::string summary;
    std::vector<std::string> lines;
};

enum class Show : int { all, headers, pdb, differ };

class TypesView final : public View {
public:
    std::string_view id() const override { return "types"; }
    std::string_view title() const override { return "Types"; }

    void navigate(ViewContext&, const NavTarget& target) override {
        if (!target.anchor.empty()) selected_ = target.anchor;
    }

    void draw(ViewContext& ctx) override {
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "Types shows the types the project's headers declare as the compiler lays them out, beside the "
                             "target's PDB's, where the program uses their fields, and declares the PDB's types or class "
                             "skeletons from the RTTI in a header.")) {
            return;
        }
        update(ctx, access);
        if (!data_.value()) {
            ImGui::TextDisabled("Compiling the project's headers and reading the types...");
            return;
        }
        const TypesData& d = *data_.value();
        draw_summary(ctx, access, d);
        draw_filter();
        draw_table(d);
        draw_details(ctx, access, d);
    }

private:
    void update(ViewContext& ctx, const ProjectAccess& access) {
        data_.poll();
        if (auto r = uses_job_.take()) uses_ = std::move(*r);
        if (auto r = write_job_.take()) {
            write_ = std::move(*r);
            ctx.notify(write_->ok ? Severity::info : Severity::warning, write_->summary);
            ++refresh_;
        }
        const auto program = access.program;
        const project::Project& project = *access.project;  // copied into the job
        data_.update(
            ctx.jobs, TypesKey{project_inputs(access), refresh_},
            [&] {
                return [program, project](const CancelToken& token) {
                    TypesData out;
                    const auto header_files = project::project_headers(project);
                    auto setup = project::make_match_setup(&project, "");
                    if (!header_files || header_files->empty()) {
                        // No headers to compile.
                    } else if (!setup) {
                        out.headers_error = setup.error().message;
                    } else {
                        setup->cancelled = [token] { return token.cancelled(); };
                        auto headers = project::compile_header_types(project, *setup, program->arch());
                        if (headers) out.headers = std::make_shared<const project::HeaderTypes>(std::move(*headers));
                        else if (headers.error().code != ErrorCode::unsupported) out.headers_error = headers.error().message;
                    }
                    const TypeCatalog& pdb = program->pdb_types().catalog;
                    if (out.headers)
                        for (const auto& declared : out.headers->declared) {
                            const TypeLayout* layout = out.headers->catalog.find(declared.name);
                            if (!layout) continue;  // a typedef of a built-in type, or declared only
                            TypeRow row{declared.name, *layout, declared.header, false, {}};
                            if (const TypeLayout* expected = pdb.find(declared.name)) {
                                row.in_pdb = true;
                                row.differences = compare_layouts(*layout, *expected);
                            }
                            out.differ += row.differences.empty() ? 0 : 1;
                            out.rows.push_back(std::move(row));
                        }
                    out.declared = out.rows.size();
                    for (const TypeLayout& t : pdb.types()) {
                        if (anonymous_type_name(t.name) || (out.headers && out.headers->header_of(t.name))) continue;
                        out.rows.push_back({t.name, t, "", true, {}});
                        ++out.pdb_only;
                    }
                    out.rtti = !program->rtti().classes.empty();
                    return out;
                };
            },
            refresh_interval(ctx, std::chrono::milliseconds(2000)));
    }

    void draw_summary(ViewContext& ctx, const ProjectAccess& access, const TypesData& d) {
        if (!d.headers_error.empty()) colored_text(ctx.colors().error, "The project's headers do not compile:\n" + d.headers_error);
        ImGui::Text("%zu types in the project's headers (%zu differ from the PDB), %zu more in the PDB", d.declared, d.differ, d.pdb_only);
        same_line_or_wrap(button_width("Refresh"));
        if (ImGui::SmallButton("Refresh")) ++refresh_;
        const bool live = ctx.services.commands && ctx.services.commands->live();
        if (d.pdb_only > 0) {
            same_line_or_wrap(button_width("Import all from the PDB"));
            ImGui::BeginDisabled(write_job_.pending() || live);
            if (ImGui::SmallButton("Import all from the PDB")) start_write(ctx, access, {}, false);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Declare every type the PDB defines (but templates, anonymous ones and the compiler's and SDKs') in "
                                  "include/types.h, checked against the PDB first (decomp types import --all). Not during a run.");
        }
        if (d.rtti) {
            same_line_or_wrap(button_width("Skeletons from RTTI"));
            ImGui::BeginDisabled(write_job_.pending() || live);
            if (ImGui::SmallButton("Skeletons from RTTI")) start_write(ctx, access, {}, true);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Declare skeletons of the classes the RTTI names in include/types.h: bases, vtables and the methods "
                                  "the symbols name, checked against the RTTI first (decomp types skeletons). Not during a run.");
        }
        busy_marker(ctx, data_.busy() || write_job_.pending(), write_job_.pending() ? "writing the header..." : "updating...");
        if (write_) {
            colored_text(write_->ok ? ctx.colors().ok : ctx.colors().warn, write_->summary);
            for (const auto& line : write_->lines) ImGui::BulletText("%s", line.c_str());
        }
    }

    void draw_filter() {
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, ImGui::GetFontSize() * 20));
        ImGui::InputTextWithHint("##filter", "Filter by name", &filter_);
        static constexpr const char* kShow[] = {"All", "In headers", "Only in the PDB", "Differ from the PDB"};
        for (int i = 0; i < 4; ++i) {
            same_line_or_wrap(ImGui::CalcTextSize(kShow[i]).x + ImGui::GetFrameHeight() * 2);
            if (ImGui::RadioButton(kShow[i], show_ == static_cast<Show>(i))) show_ = static_cast<Show>(i);
        }
    }

    bool shown(const TypeRow& row) const {
        if (!filter_.empty() && to_lower(row.name).find(to_lower(filter_)) == std::string::npos) return false;
        switch (show_) {
        case Show::all: return true;
        case Show::headers: return !row.header.empty();
        case Show::pdb: return row.header.empty();
        case Show::differ: return !row.differences.empty();
        }
        return true;
    }

    void draw_table(const TypesData& d) {
        std::vector<usize> rows;
        for (usize i = 0; i < d.rows.size(); ++i)
            if (shown(d.rows[i])) rows.push_back(i);
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        const float height = std::max(ImGui::GetContentRegionAvail().y * 0.4f, ImGui::GetFrameHeight() * 5);
        if (!ImGui::BeginTable("##types", 5, flags, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"Type", "Kind", "Size", "Declared in", "PDB"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const TypeRow& row = d.rows[rows[static_cast<usize>(r)]];
                ImGui::PushID(row.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(row.name.c_str(), selected_ == row.name, ImGuiSelectableFlags_SpanAllColumns)) selected_ = row.name;
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(to_string(row.layout.kind)).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bytes_text(row.layout.size).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.header.empty() ? "(only in the PDB)" : row.header.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.header.empty() ? "" : !row.in_pdb ? "not in the PDB" : row.differences.empty() ? "same" : "differs");
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    void draw_details(ViewContext& ctx, const ProjectAccess& access, const TypesData& d) {
        if (!selected_) {
            ImGui::TextDisabled("Select a type to see its layout and where the program uses it.");
            return;
        }
        const auto it = std::ranges::find(d.rows, *selected_, &TypeRow::name);
        if (it == d.rows.end()) {
            ImGui::TextDisabled("%s", std::format("No type {} in the project's headers or the PDB.", *selected_).c_str());
            return;
        }
        const TypeRow& row = *it;
        ImGui::SeparatorText(row.name.c_str());
        ImGui::TextDisabled("%s", row.header.empty() ? "From the target's PDB: no project header declares it yet."
                                                     : std::format("Declared in {}, laid out by the project's compiler.", row.header).c_str());
        const bool live = ctx.services.commands && ctx.services.commands->live();
        if (row.header.empty()) {
            same_line_or_wrap(button_width("Import from the PDB"));
            ImGui::BeginDisabled(write_job_.pending() || live);
            if (ImGui::SmallButton("Import from the PDB")) start_write(ctx, access, {row.name}, false);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Declare %s (and what it needs) in include/types.h, checked against the PDB first. Not during a run.",
                                  row.name.c_str());
        }
        same_line_or_wrap(button_width("Find uses"));
        ImGui::BeginDisabled(uses_job_.pending());
        if (ImGui::SmallButton("Find uses")) start_uses(ctx, access, d, row.name);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Where the program's functions reach the fields of %s through the pointers they are given.", row.name.c_str());

        for (const auto& difference : row.differences) colored_text(ctx.colors().warn, "Differs from the PDB: " + difference);
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextUnformatted(to_text(row.layout).c_str());
        ImGui::PopFont();

        if (uses_job_.pending()) ImGui::TextDisabled("Looking for uses...");
        if (!uses_ || uses_->type != row.name) return;
        ImGui::SeparatorText(std::format("Uses ({})", uses_->uses.size()).c_str());
        if (uses_->uses.empty()) ImGui::TextDisabled("No function reaches its fields through a pointer it is given.");
        const float height = std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 4);
        if (!ImGui::BeginTable("##uses", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"Field", "Function", "At"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(uses_->uses.size()));
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const FieldUse& use = uses_->uses[static_cast<usize>(r)];
                ImGui::PushID(r);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((use.address_of ? "&" + use.path : use.path).c_str());
                ImGui::TableNextColumn();
                const Symbol* s = access.program->symbols().at(use.function);
                function_link(ctx, s && !s->display.empty() ? s->display : std::format("sub_{:x}", use.function), use.function);
                ImGui::TableNextColumn();
                address_link(ctx, std::format("{:#x}", use.address), use.address);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    void start_uses(ViewContext& ctx, const ProjectAccess& access, const TypesData& d, const std::string& name) {
        uses_.reset();
        uses_job_ = ctx.jobs.submit([program = access.program, headers = d.headers, name](const CancelToken& token) {
            const TypeView types(headers ? &headers->catalog : nullptr, &program->pdb_types().catalog);
            return UsesResult{name, find_field_uses(*program, types, name, [&token] { return token.cancelled(); })};
        });
    }

    // Declares `names` from the PDB (every type, when empty), or class skeletons from the RTTI.
    void start_write(ViewContext& ctx, const ProjectAccess& access, std::vector<std::string> names, bool skeletons) {
        write_.reset();
        write_job_ = ctx.jobs.submit([program = access.program, project = *access.project, names = std::move(names), skeletons](const CancelToken& token) {
            WriteResult out;
            auto setup = project::make_match_setup(&project, "");
            if (!setup) {
                out.summary = "Cannot compile the headers: " + setup.error().message;
                return out;
            }
            setup->cancelled = [token] { return token.cancelled(); };
            std::vector<std::string> wanted = names;
            if (!skeletons && wanted.empty()) {
                const ProgramTypes& types = program->pdb_types();
                for (const TypeLayout& t : types.catalog.types()) {
                    if (anonymous_type_name(t.name) || t.name.find('<') != std::string::npos || t.name == "type_info" || t.name.starts_with("__") ||
                        t.name.starts_with("std::"))
                        continue;
                    if (const auto source = types.sources.find(t.name); source != types.sources.end() && system_header(source->second)) continue;
                    wanted.push_back(t.name);
                }
            }
            auto imported = skeletons ? project::prepare_skeleton_import(project, *program, *setup, wanted, "")
                                      : project::prepare_type_import(project, *program, *setup, wanted, "");
            if (!imported) {
                const auto lines = split_lines(imported.error().message);
                out.summary = (skeletons ? "No skeletons declared: " : "Nothing imported: ") + (lines.empty() ? imported.error().message : lines.front());
                for (usize i = 1; i < lines.size() && i < 20; ++i) out.lines.push_back(std::string(trim(lines[i])));
                return out;
            }
            auto run_lock = project.try_lock_active_run();
            if (!run_lock || !*run_lock) {
                out.summary = "A run is active in this project; declare types when it is done.";
                return out;
            }
            const auto written = project::commit_type_change(project, imported->change,
                                                             project::ChangeOrigin{SymbolSource::user, "", skeletons ? "class skeletons from the RTTI"
                                                                                                                      : "types imported from the PDB"},
                                                             project::ChangeSubject{});
            if (!written) {
                out.summary = "Cannot write " + imported->change.header + ": " + written.error().message;
                return out;
            }
            out.ok = true;
            out.summary = std::format("{}: {} type(s) declared ({}){}", imported->change.header, imported->defined.size(),
                                      join(imported->defined, ", ").substr(0, 300),
                                      imported->declared.empty() ? std::string() : std::format(", {} declared forward", imported->declared.size()));
            for (const auto& s : imported->skipped) out.lines.push_back("skipped " + s);
            return out;
        });
    }

    KeyedJob<TypesKey, TypesData> data_;
    u64 refresh_ = 0;
    std::string filter_;
    Show show_ = Show::all;
    std::optional<std::string> selected_;
    JobHandle<UsesResult> uses_job_;
    std::optional<UsesResult> uses_;
    JobHandle<WriteResult> write_job_;
    std::optional<WriteResult> write_;
};

} // namespace

std::unique_ptr<View> make_types_view() { return std::make_unique<TypesView>(); }

} // namespace decomp::gui
