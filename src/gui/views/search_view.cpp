// Search (docs/ui.md#search): the mechanical searches of docs/matching.md#searching, started here or by
// `decomp search`. A form starts a flag search, a permutation or a compiler identification on a function's
// sources (or a file, or every verified source) in the background, with its progress as it goes; the
// project's searches are listed newest first (.decomp/search/), and the selected one shows its result by
// kind (a flag search's groups, a permutation's edits and the source's diff, an identification's ranking),
// every candidate it tried with the best so far, and its settings. A result can be kept in the project:
// flags or a toolchain in decomp.json, a permuted source as the function's verified source (or its best
// attempt).

#include "gui/views/search_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/text_diff.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "matching/toolchain.hpp"
#include "project/setup.hpp"
#include "search/apply.hpp"
#include "search/identify.hpp"
#include "search/runner.hpp"
#include "viewmodel/searches.hpp"

#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <set>

namespace decomp::gui {

namespace {

constexpr int kFlags = 0, kPermute = 1, kIdentify = 2;
constexpr const char* kKindNames[] = {"Flags", "Permute", "Identify"};

// Where a search's sources come from, per kind.
enum class Source : u8 { verified, whole, every_verified, attempt, file };

struct SourceChoice {
    Source source;
    const char* label;
};
constexpr SourceChoice kFlagSources[] = {{Source::verified, "the function's verified source"},
                                         {Source::whole, "every function of that file"},
                                         {Source::every_verified, "every verified source"},
                                         {Source::file, "a file"}};
constexpr SourceChoice kPermuteSources[] = {{Source::attempt, "the function's best attempt"}, {Source::file, "a file"}};
constexpr SourceChoice kIdentifySources[] = {{Source::verified, "the function's verified source"},
                                             {Source::every_verified, "every verified source"},
                                             {Source::attempt, "the function's best attempt"},
                                             {Source::file, "a file"}};

std::span<const SourceChoice> sources_of(int kind) {
    if (kind == kPermute) return kPermuteSources;
    if (kind == kIdentify) return kIdentifySources;
    return kFlagSources;
}

struct SearchKey {
    ProjectInputs project;
    u64 refresh = 0;
    std::string selected;
    u64 tick = 0;  // while a search runs: its log grows
    bool operator==(const SearchKey&) const = default;
};

struct SelectedRun {
    search::RunRecord record;
    std::vector<search::LogEntry> log;
    std::string file, start_source, best_source;  // a permutation's
};

struct SearchData {
    std::vector<search::RunRecord> runs;
    std::optional<SelectedRun> selected;
};

// A search running in the background, as the view shows it while it runs.
struct LiveSearch {
    std::mutex mutex;
    std::string target;
    usize candidates = 0;
    std::optional<search::LogEntry> best;

    void started(const std::string& t) {
        std::lock_guard lock(mutex);
        target = t;
    }
    void add(const search::LogEntry& e) {
        std::lock_guard lock(mutex);
        ++candidates;
        if (e.best) best = e;
    }
    std::string text() {
        std::lock_guard lock(mutex);
        if (target.empty()) return "starting...";
        return std::format("{}: {} candidate{}{}", target, candidates, candidates == 1 ? "" : "s",
                           best ? ", best " + best->score.text() : std::string());
    }
};

struct SearchOutcomeText {
    std::string run_id, text, error;
    bool complete = false;
};

struct ApplyOutcome {
    std::string text;
    bool ok = false;
};

constexpr int kResultTab = 0, kLogTab = 1, kSettingsTab = 2;

class SearchView final : public View {
public:
    std::string_view id() const override { return "search"; }
    std::string_view title() const override { return "Search"; }

    // The function sets the form's. The anchor picks a kind ("flags", "permute", "identify"), fills the
    // form ("file:<path>", "preset:<name>", "group:<a | b>"), or shows a search ("run:<id>").
    void navigate(ViewContext&, const NavTarget& target) override {
        if (target.va) function_va_ = *target.va;
        const std::string& a = target.anchor;
        if (a == "flags" || a == "permute" || a == "identify") {
            kind_ = a == "flags" ? kFlags : a == "permute" ? kPermute : kIdentify;
            source_index_ = 0;
        } else if (a.starts_with("run:")) {
            selected_ = a.substr(4);
            tab_request_ = kResultTab;
        } else if (a.starts_with("file:")) {
            file_ = a.substr(5);
            source_index_ = static_cast<int>(sources_of(kind_).size()) - 1;  // "a file" comes last
        } else if (a.starts_with("preset:")) {
            preset_ = a.substr(7);
        } else if (a.starts_with("group:")) {
            groups_ += a.substr(6) + "\n";
        }
    }

    void draw(ViewContext& ctx) override {
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "Search runs the mechanical searches: compiler flags that make sources compile to the target's bytes, "
                             "permutations of a function's source, and which toolchain built the target.")) {
            return;
        }
        register_actions(ctx);
        update(ctx, access);
        if (start_requested_) {
            start_requested_ = false;
            start(ctx, access);
        }
        if (apply_requested_) {
            apply_requested_ = false;
            apply(ctx, access);
        }
        draw_form(ctx, access);
        draw_runs(ctx);
        draw_selected(ctx, access);
    }

private:
    // ---- actions ----------------------------------------------------------------------------------------

    void register_actions(ViewContext& ctx) {
        if (actions_registered_) return;
        actions_registered_ = true;
        ctx.actions.add({.id = "search.start", .label = "Start the search", .category = "Search",
                         .enabled = [this] { return !job_.pending(); }, .run = [this] { start_requested_ = true; }});
        ctx.actions.add({.id = "search.cancel", .label = "Stop the running search", .category = "Search",
                         .enabled = [this] { return job_.pending(); }, .run = [this] { cancel(); }});
        ctx.actions.add({.id = "search.apply", .label = "Keep the selected search's result in the project", .category = "Search",
                         .enabled = [this] { return applicable_; }, .run = [this] { apply_requested_ = true; }});
    }

    void cancel() {
        if (!job_.pending()) return;
        // The search stops after the candidates being compiled and keeps its run (status cancelled).
        job_.cancel();
        job_.reset();
        ++refresh_;
    }

    // ---- jobs -------------------------------------------------------------------------------------------

    void update(ViewContext& ctx, const ProjectAccess& access) {
        data_.poll();
        try {
            if (auto r = job_.take()) {
                if (!r->error.empty()) {
                    ctx.notify(Severity::warning, "The search did not run: " + r->error);
                } else {
                    ctx.notify(r->complete ? Severity::info : Severity::warning, "Search: " + r->text + ".",
                               NavEntry{std::string(id()), {.anchor = "run:" + r->run_id}});
                    if (!r->run_id.empty()) selected_ = r->run_id;
                    tab_request_ = kResultTab;
                }
                job_.reset();
                ++refresh_;
            }
        } catch (const std::exception& e) {
            ctx.notify(Severity::error, std::format("The search failed: {}", e.what()));
            job_.reset();
        }
        try {
            if (auto a = apply_job_.take()) {
                ctx.notify(a->ok ? Severity::info : Severity::warning, a->text);
                apply_job_.reset();
            }
        } catch (const std::exception& e) {
            ctx.notify(Severity::error, std::format("Keeping the source failed: {}", e.what()));
            apply_job_.reset();
        }
        // The newest search is shown until one is picked.
        if (!selected_ && data_.value() && !data_.value()->runs.empty()) selected_ = data_.value()->runs.front().id;
        // While a search runs (this one, one stopped that is finishing its compiles, or one `decomp search`
        // runs), its files change.
        const bool running = job_.pending() || (data_.value() && std::ranges::any_of(data_.value()->runs, [](const search::RunRecord& r) {
                                                    return r.status == search::RunStatus::running;
                                                }));
        const u64 tick = running ? static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch() / std::chrono::seconds(1)) : 0;
        const project::Project& project = *access.project;  // copied into the job
        data_.update(
            ctx.jobs, SearchKey{project_inputs(access), refresh_, selected_.value_or(std::string()), tick},
            [&] {
                return [project, selected = selected_.value_or(std::string())]() {
                    SearchData out;
                    const auto dir = search::search_dir(project);
                    out.runs = search::list_runs(dir);
                    if (selected.empty()) return out;
                    const auto run_dir = dir / fs::from_utf8(selected);
                    auto record = search::load_run(run_dir);
                    if (!record) return out;
                    SelectedRun s;
                    s.record = std::move(*record);
                    s.log = search::load_log(run_dir);
                    if (s.record.kind == search::SearchKind::permute) {
                        std::error_code ec;
                        for (const auto& entry : std::filesystem::directory_iterator(run_dir, ec)) {
                            const std::string name = fs::to_utf8(entry.path().filename());
                            if (name.starts_with("start-")) {
                                s.file = name.substr(6);
                                s.start_source = fs::read_text(entry.path()).value_or(std::string());
                            } else if (name.starts_with("best-")) {
                                s.best_source = fs::read_text(entry.path()).value_or(std::string());
                            }
                        }
                    }
                    out.selected = std::move(s);
                    return out;
                };
            },
            running ? std::chrono::milliseconds(job_.pending() ? 1000 : 2000) : refresh_interval(ctx, std::chrono::milliseconds(2000)));
    }

    // The request the form describes, or why it cannot be made.
    Result<search::SearchRequest> request_of(const ProjectAccess& access) const {
        search::SearchRequest r;
        r.kind = kind_ == kPermute ? search::SearchKind::permute : kind_ == kIdentify ? search::SearchKind::identify : search::SearchKind::flags;
        const Source source = sources_of(kind_)[static_cast<usize>(std::clamp(source_index_, 0, static_cast<int>(sources_of(kind_).size()) - 1))].source;
        const bool needs_function = source == Source::verified || source == Source::whole || source == Source::attempt;
        if (needs_function) {
            if (!function_va_) return make_error(ErrorCode::invalid_argument, "pick a function (select one, or give its name or address)");
            r.probes.functions = {*function_va_};
        }
        r.probes.whole = source == Source::whole;
        r.probes.attempt = source == Source::attempt;
        r.probes.verified = source == Source::every_verified;
        if (source == Source::file) {
            if (trim(file_).empty()) return make_error(ErrorCode::invalid_argument, "give the file to compile");
            std::filesystem::path path = fs::from_utf8(std::string(trim(file_)));
            if (path.is_relative()) path = access.project->root() / path;
            r.probes.sources = {path};
            if (function_va_ && kind_ == kPermute) r.probes.functions = {*function_va_};
        }
        if (kind_ == kFlags || kind_ == kIdentify) {
            for (const auto& line : split_lines(groups_))
                if (!trim(line).empty()) r.groups.push_back(std::string(trim(line)));
            r.preset = preset_;
        }
        if (kind_ == kIdentify)
            for (const auto& t : toolchains_)
                if (chosen_toolchains_.contains(t)) r.toolchains.push_back(t);
        r.limit = static_cast<usize>(std::max(limit_, 0));
        r.seed = static_cast<u64>(std::max(seed_, 0));
        return r;
    }

    void start(ViewContext& ctx, const ProjectAccess& access) {
        if (job_.pending()) return;
        auto request = request_of(access);
        if (!request) {
            ctx.notify(Severity::warning, "Cannot start the search: " + request.error().message);
            return;
        }
        auto live = std::make_shared<LiveSearch>();
        live_ = live;
        job_ = ctx.jobs.submit([program = access.program, project = *access.project, request = std::move(*request), live](const CancelToken& token) {
            SearchOutcomeText out;
            // Identification compiles with its candidates; a project without a toolchain still gives the
            // work directories and include paths.
            std::string toolchain;
            if (request.kind == search::SearchKind::identify && project.config().toolchain.empty())
                if (auto registry = matching::ToolchainRegistry::load(); registry && !registry->toolchains().empty())
                    toolchain = registry->toolchains().front().name;
            auto setup = project::make_match_setup(&project, toolchain);
            if (!setup) {
                out.error = setup.error().message;
                return out;
            }
            setup->cancelled = [token] { return token.cancelled(); };
            search::SearchCallbacks callbacks;
            callbacks.started = [live](const search::Probes& p) { live->started(p.target); };
            callbacks.candidate = [live](const search::LogEntry& e) { live->add(e); };
            callbacks.cancelled = [token] { return token.cancelled(); };
            auto r = search::run_search(&project, *program, *setup, request, callbacks);
            if (!r) {
                out.error = r.error().message;
                return out;
            }
            out.run_id = r->run ? r->run->id : std::string();
            out.text = r->headline();
            out.complete = r->score.complete();
            return out;
        });
        ++refresh_;
    }

    // Keeps the selected search's result in the project.
    void apply(ViewContext& ctx, const ProjectAccess& access) {
        if (!applicable_ || !data_.value() || !data_.value()->selected) return;
        const SelectedRun& s = *data_.value()->selected;
        project::Project& project = *access.project;
        if (s.record.kind == search::SearchKind::flags) {
            const auto v = vm::read_flag_search(s.record.result);
            if (auto r = search::apply_configuration(project, v.flags); !r) ctx.notify(Severity::error, "Cannot save decomp.json: " + r.error().message);
            else ctx.notify(Severity::info, std::format("decomp.json flags: {}. Units' Verify checks every verified function with them.", join(v.flags, " ")));
        } else if (s.record.kind == search::SearchKind::identify) {
            const auto v = vm::read_identify(s.record.result);
            if (v.ranking.empty()) return;
            const auto& best = v.ranking.front();
            if (auto r = search::apply_configuration(project, split_flags(best.flags), best.toolchain); !r)
                ctx.notify(Severity::error, "Cannot save decomp.json: " + r.error().message);
            else ctx.notify(Severity::info, std::format("decomp.json: toolchain {}, flags {}.", best.toolchain, best.flags));
        } else if (s.record.functions.size() == 1 && !apply_job_.pending()) {
            const auto v = vm::read_permute(s.record.result);
            apply_job_ = ctx.jobs.submit([program = access.program, project = *access.project, va = s.record.functions.front(),
                                          source = s.best_source, v](const CancelToken& token) mutable {
                ApplyOutcome out;
                auto setup = project::make_match_setup(&project, "");
                if (!setup) {
                    out.text = "Cannot keep the source: " + setup.error().message;
                    return out;
                }
                setup->cancelled = [token] { return token.cancelled(); };
                const Symbol* fn = program->symbols().at(va);
                if (!fn) {
                    out.text = "The function is gone from the symbols.";
                    return out;
                }
                auto kept = search::apply_source(project, *program, *setup, *fn, source, v.score.match_percent, v.score.complete(), "found by the permuter");
                if (!kept) {
                    out.text = "Cannot keep the source: " + kept.error().message;
                    return out;
                }
                out.ok = kept->verified || kept->best_attempt;
                out.text = kept->message + ".";
                return out;
            });
        }
    }

    static std::vector<std::string> split_flags(std::string_view text) {
        std::vector<std::string> out;
        for (auto part : split(text, ' '))
            if (!part.empty()) out.emplace_back(part);
        return out;
    }

    // ---- the form ---------------------------------------------------------------------------------------

    void draw_form(ViewContext& ctx, const ProjectAccess& access) {
        const bool running = job_.pending();
        if (!ImGui::CollapsingHeader("New search", form_open_ ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
            draw_running(ctx);
            return;
        }
        ImGui::BeginDisabled(running);
        for (int k = 0; k < 3; ++k) {
            if (k) ImGui::SameLine();
            if (ImGui::RadioButton(kKindNames[k], kind_ == k) && kind_ != k) {
                kind_ = k;
                source_index_ = 0;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(kind_ == kFlags     ? "compiler flags that make the sources compile to the target's bytes"
                            : kind_ == kPermute ? "edits of a function's source toward the target's bytes"
                                                : "which toolchain compiles the sources closest to the target");
        // The function.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Function:");
        ImGui::SameLine();
        if (function_va_) {
            function_link(ctx, access.program->describe_address(*function_va_), *function_va_);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) function_va_.reset();
        } else {
            ImGui::TextDisabled("none");
        }
        if (ctx.selection.function_va && ctx.selection.function_va != function_va_) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Use the selection")) function_va_ = ctx.selection.function_va;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::InputTextWithHint("##function", "name or address", &function_text_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (auto va = access.program->resolve(trim(function_text_))) {
                function_va_ = *va;
                function_text_.clear();
            } else {
                ctx.notify(Severity::warning, std::format("No function '{}'.", function_text_));
            }
        }
        // Its sources.
        const auto sources = sources_of(kind_);
        source_index_ = std::clamp(source_index_, 0, static_cast<int>(sources.size()) - 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        if (ImGui::BeginCombo("Sources", sources[static_cast<usize>(source_index_)].label)) {
            for (usize i = 0; i < sources.size(); ++i)
                if (ImGui::Selectable(sources[i].label, static_cast<int>(i) == source_index_)) source_index_ = static_cast<int>(i);
            ImGui::EndCombo();
        }
        if (sources[static_cast<usize>(source_index_)].source == Source::file) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
            ImGui::InputTextWithHint("##file", "path (relative to the project)", &file_);
        }
        // The kind's settings.
        if (kind_ == kFlags || kind_ == kIdentify) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            const char* presets[] = {"", "basic", "common", "full", "none"};
            if (ImGui::BeginCombo("Preset", preset_.empty() ? (kind_ == kFlags ? "common" : "basic") : preset_.c_str())) {
                for (const char* p : presets)
                    if (*p && ImGui::Selectable(p, preset_ == p)) preset_ = p;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("The groups of alternatives searched: basic (optimization level, frame pointers, security checks), common "
                                  "(also inlining, floating point, instruction set), full (and more).");
            if (kind_ == kFlags) {
                ImGui::TextDisabled("More groups, one per line (\"opt: /Od | /O1 | /O2\", \"none | /Oy-\"):");
                multiline_text("##groups", &groups_, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 3.5f));
            }
        }
        if (kind_ == kIdentify) draw_toolchains(access);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::InputInt("Limit", &limit_, 0);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(kind_ == kIdentify ? "Configurations to compile per toolchain, at most (0: 64)."
                                                 : kind_ == kPermute ? "Candidates to compile, at most (0: 500)." : "Configurations to compile, at most (0: 1000).");
        if (kind_ != kIdentify) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            ImGui::InputInt("Seed", &seed_, 0);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(running);
        if (ImGui::Button("Start")) start_requested_ = true;
        ImGui::EndDisabled();
        draw_running(ctx);
    }

    void draw_running(ViewContext& ctx) {
        if (!job_.pending()) return;
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) cancel();
        busy_marker(ctx, true, live_ ? live_->text() : std::string("searching..."));
    }

    void draw_toolchains(const ProjectAccess& access) {
        if (!toolchains_loaded_) {
            toolchains_loaded_ = true;
            toolchains_.clear();
            if (auto registry = matching::ToolchainRegistry::load())
                for (const auto& t : registry->toolchains())
                    if (auto arch = search::toolchain_arch(t); !arch || *arch == access.program->arch()) {
                        toolchains_.push_back(t.name);
                        chosen_toolchains_.insert(t.name);
                    }
        }
        ImGui::TextUnformatted("Toolchains:");
        if (toolchains_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("none for this architecture (Toolchains adds them)");
        }
        for (const auto& t : toolchains_) {
            ImGui::SameLine();
            bool on = chosen_toolchains_.contains(t);
            if (ImGui::Checkbox(t.c_str(), &on)) {
                if (on) chosen_toolchains_.insert(t);
                else chosen_toolchains_.erase(t);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload")) toolchains_loaded_ = false;
    }

    // ---- the searches -----------------------------------------------------------------------------------

    void draw_runs(ViewContext& ctx) {
        ImGui::SeparatorText("Searches");
        busy_marker(ctx, data_.busy());
        if (!data_.value()) {
            ImGui::TextDisabled("Reading the searches...");
            return;
        }
        const auto& runs = data_.value()->runs;
        if (runs.empty()) {
            ImGui::TextDisabled("No search yet: start one above, or with decomp search flags|permute|identify.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        const float height = std::min(ImGui::GetFrameHeightWithSpacing() * (static_cast<float>(runs.size()) + 1.5f),
                                      std::max(ImGui::GetContentRegionAvail().y * 0.3f, ImGui::GetFrameHeight() * 5));
        if (!ImGui::BeginTable("##searches", 6, flags, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"Started", "Kind", "Status", "Candidates", "Best"}) ImGui::TableSetupColumn(h);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < runs.size(); ++i) {
            const auto row = vm::search_run_row(runs[i]);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto when = vm::parse_iso8601(row.started);
            const std::string started = when ? local_date_time(*when) : row.started;
            if (ImGui::Selectable(started.c_str(), selected_ == row.id, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_ = row.id;
                tab_request_ = kResultTab;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", row.id.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.kind.c_str());
            ImGui::TableNextColumn();
            if (row.status == "running") status_label("running", ctx.colors().warn);
            else if (row.status == "failed") status_label("failed", ctx.colors().error);
            else if (row.status == "cancelled") ImGui::TextDisabled("stopped");
            else ImGui::TextUnformatted(row.status.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", row.candidates);
            ImGui::TableNextColumn();
            if (row.complete) status_label(row.best, ctx.colors().ok);
            else ImGui::TextUnformatted(row.best.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.target.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void draw_selected(ViewContext& ctx, const ProjectAccess& access) {
        applicable_ = false;
        if (!data_.value() || !data_.value()->selected || !selected_ || data_.value()->selected->record.id != *selected_) return;
        const SelectedRun& s = *data_.value()->selected;
        const auto& r = s.record;
        ImGui::SeparatorText(std::format("{} search on {}", search::to_string(r.kind), r.target).c_str());
        ImGui::TextDisabled("%s, %zu candidate(s) in %.1f s%s", std::string(search::to_string(r.status)).c_str(), r.candidates,
                            static_cast<double>(r.duration_ms) / 1000.0, r.status == search::RunStatus::running ? " so far" : "");
        if (r.best) {
            ImGui::SameLine();
            if (r.best->complete()) status_label("best: " + r.best->text(), ctx.colors().ok);
            else ImGui::TextUnformatted(("best: " + r.best->text()).c_str());
        }
        if (!r.error.empty()) colored_text(ctx.colors().warn, r.error);
        if (!ImGui::BeginTabBar("##search_tabs")) return;
        const char* names[] = {"Result", "Candidates", "Settings"};
        for (int t = 0; t < 3; ++t) {
            const ImGuiTabItemFlags flags = tab_request_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (!ImGui::BeginTabItem(names[t], nullptr, flags)) continue;
            if (ImGui::BeginChild("##search_tab", ImVec2(0, 0))) {
                switch (t) {
                case kResultTab: draw_result(ctx, access, s); break;
                case kLogTab: draw_log(ctx, s); break;
                case kSettingsTab: draw_settings(ctx, s); break;
                default: break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        tab_request_ = -1;
        ImGui::EndTabBar();
    }

    void draw_result(ViewContext& ctx, const ProjectAccess& access, const SelectedRun& s) {
        const auto& r = s.record;
        if (r.status == search::RunStatus::running) {
            ImGui::TextDisabled("The search is running: its result comes when it ends (Candidates shows what it has tried).");
            return;
        }
        const bool done = r.status == search::RunStatus::done || r.status == search::RunStatus::cancelled;
        if (r.kind == search::SearchKind::flags) {
            const auto v = vm::read_flag_search(r.result);
            ImGui::TextUnformatted("Best flags:");
            ImGui::SameLine();
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            ImGui::TextUnformatted(v.flags.empty() ? "(none)" : join(v.flags, " ").c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s; from %s. %zu of %zu configurations compiled%s.", v.score.text().c_str(), v.start_score.text().c_str(),
                                v.candidates, v.space, v.exhaustive ? " (every one)" : "");
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
            if (!v.groups.empty() && ImGui::BeginTable("##flag_groups", 4, flags)) {
                for (const char* h : {"Group", "Chosen", "As good", "Started from"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const auto& g : v.groups) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(g.name.empty() ? join(g.alternatives, " | ").c_str() : g.name.c_str());
                    ImGui::TableNextColumn();
                    if (g.changed()) status_label(g.chosen_text(), ctx.colors().warn);
                    else ImGui::TextUnformatted(g.chosen_text().c_str());
                    ImGui::TableNextColumn();
                    if (g.decided()) ImGui::TextDisabled("decided");
                    else ImGui::TextUnformatted(g.also_text().c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", g.start < g.alternatives.size() ? g.alternatives[g.start].c_str() : "");
                }
                ImGui::EndTable();
            }
            applicable_ = done && v.score.better_than(v.start_score);
            apply_button("Use these flags", "Set them as the project's flags (decomp.json).");
        } else if (r.kind == search::SearchKind::identify) {
            const auto v = vm::read_identify(r.result);
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("##ranking", 4, flags)) {
                for (const char* h : {"#", "Toolchain", "Best", "With"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (usize i = 0; i < v.ranking.size(); ++i) {
                    const auto& t = v.ranking[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", i + 1);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(t.toolchain.c_str());
                    ImGui::TableNextColumn();
                    if (!t.error.empty()) status_label("does not compile", ctx.colors().error);
                    else if (t.score.complete()) status_label(t.score.text(), ctx.colors().ok);
                    else ImGui::TextUnformatted(t.score.text().c_str());
                    ImGui::TableNextColumn();
                    if (!t.error.empty()) ImGui::TextDisabled("%s", std::string(trim(t.error.substr(0, t.error.find('\n')))).c_str());
                    else ImGui::TextUnformatted(t.flags.c_str());
                }
                ImGui::EndTable();
            }
            if (v.decided) ImGui::TextUnformatted(std::format("{} comes out ahead.", v.ranking.front().toolchain).c_str());
            else ImGui::TextDisabled("No toolchain comes out ahead.");
            applicable_ = done && v.decided;
            apply_button(v.decided ? std::format("Use {}", v.ranking.front().toolchain).c_str() : "Use the first",
                         "Set it and its flags as the project's toolchain and flags (decomp.json).");
        } else {
            const auto v = vm::read_permute(r.result);
            if (!v.error.empty()) colored_text(ctx.colors().warn, v.error);
            if (v.improved()) {
                ImGui::TextUnformatted(std::format("{} edit{}: {} (from {})", v.steps.size(), v.steps.size() == 1 ? "" : "s", v.score.text(),
                                                   v.start_score.text())
                                           .c_str());
                for (const auto& step : v.steps) ImGui::BulletText("%s", step.c_str());
            } else {
                ImGui::TextDisabled("Nothing better than the start (%s).", v.start_score.text().c_str());
            }
            const bool one = r.functions.size() == 1;
            applicable_ = done && one && !s.best_source.empty() && (v.improved() || v.score.complete()) && !apply_job_.pending();
            apply_button(v.score.complete() ? "Keep as the verified source" : "Keep as the best attempt",
                         one ? "Byte-exact: into the unit's source (or the function's own file), as a match by hand is. Otherwise "
                               "the function's best attempt, when it matches at least as well."
                             : "Only a search on one function's source can be kept.");
            if (!s.start_source.empty() && !s.best_source.empty() && v.improved()) {
                ImGui::SeparatorText(("Start and best " + s.file).c_str());
                draw_text_diff(ctx, "##permute_diff", s.start_source, s.best_source, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 8));
            }
        }
        (void)access;
    }

    void apply_button(const char* label, const char* tooltip) {
        ImGui::BeginDisabled(!applicable_);
        if (ImGui::Button(label)) apply_requested_ = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    }

    void draw_log(ViewContext& ctx, const SelectedRun& s) {
        ImGui::Checkbox("Improvements only", &improvements_only_);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu candidate(s) logged", s.log.size());
        if (s.log.size() >= 2) {
            const auto series = vm::best_so_far(s.log);
            if (ImPlot::BeginPlot("##best_so_far", ImVec2(-1, ImGui::GetFontSize() * 8))) {
                ImPlot::SetupAxes("candidate", "best match %", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 105, ImPlotCond_Always);
                ImPlot::PlotStairs("best", series.x.data(), series.y.data(), static_cast<int>(series.size()));
                ImPlot::EndPlot();
            }
        }
        const auto rows = vm::search_log_rows(s.log, improvements_only_);
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##search_log", 4, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 6)))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"#", "After", "Score"}) ImGui::TableSetupColumn(h);
        ImGui::TableSetupColumn("Candidate", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = rows[static_cast<usize>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", row.index);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f s", row.seconds);
                ImGui::TableNextColumn();
                if (row.complete) status_label(row.score, ctx.colors().ok);
                else if (row.best) ImGui::TextUnformatted(row.score.c_str());
                else ImGui::TextDisabled("%s", row.score.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.label.c_str());
            }
        ImGui::EndTable();
    }

    void draw_settings(ViewContext& ctx, const SelectedRun& s) {
        ImGui::TextDisabled("%s", s.record.id.c_str());
        draw_text_block(ctx, "##search_settings", dump_pretty(s.record.settings), std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 6));
    }

    KeyedJob<SearchKey, SearchData> data_;
    u64 refresh_ = 0;
    JobHandle<SearchOutcomeText> job_;
    std::shared_ptr<LiveSearch> live_;
    JobHandle<ApplyOutcome> apply_job_;
    std::optional<std::string> selected_;
    int tab_request_ = -1;
    bool improvements_only_ = false;
    bool applicable_ = false;
    bool start_requested_ = false, apply_requested_ = false;
    bool actions_registered_ = false;
    bool form_open_ = true;
    // The form.
    int kind_ = kFlags;
    std::optional<u64> function_va_;
    std::string function_text_;
    int source_index_ = 0;
    std::string file_;
    std::string preset_;
    std::string groups_;
    int limit_ = 0, seed_ = 1;
    std::vector<std::string> toolchains_;
    std::set<std::string> chosen_toolchains_;
    bool toolchains_loaded_ = false;
};

} // namespace

std::unique_ptr<View> make_search_view() { return std::make_unique<SearchView>(); }

} // namespace decomp::gui
