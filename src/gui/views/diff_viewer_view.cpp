// Diff viewer (docs/ui.md#diff-viewer): the target and a candidate's assembly side by side for the
// selected function and attempt, with the score, hints and binding suggestions linked to their rows, the
// data diff and the attempt history; and manual mode (docs/ui.md#manual-mode): edit the source with
// debounced background recompiles, verify and save it, or hand it back to the agent.

#include "gui/views/diff_viewer_view.hpp"

#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/export.hpp"
#include "gui/views/diff_table.hpp"
#include "gui/views/manual_mode.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/attempts.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/diff_view.hpp"
#include "viewmodel/exports.hpp"
#include "viewmodel/recompile.hpp"
#include "viewmodel/timeline.hpp"

#include <TextEditor.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <set>

namespace decomp::gui {

namespace {

using Clock = std::chrono::system_clock;

struct AttemptsLoad {
    u64 va = 0;
    std::vector<vm::AttemptRecord> attempts;
    std::optional<std::string> best_source;
};

// A compile of the editor's text, tagged with the edit generation it answers.
struct EditCompile {
    u64 generation = 0;
    CompiledSource result;
};

struct BindingWrite {
    bool ok = false;
    std::string message;
};

constexpr const char* kViewId = "diff_viewer";

// Text as an ImGui label: "##" would start the item's ID.
std::string label_text(std::string text) {
    for (usize at = text.find("##"); at != std::string::npos; at = text.find("##", at + 2)) text.replace(at, 2, "# #");
    return text;
}

class DiffViewerView final : public View, public DiffViewerControl {
public:
    DiffViewerView() {
        editor_.SetLanguage(TextEditor::Language::Cpp());
        editor_.SetShowLineNumbersEnabled(true);
        editor_.SetShowWhitespacesEnabled(false);
        editor_.SetTabSize(4);
        editor_.SetInsertSpacesOnTabs(true);
        editor_.SetChangeCallback([this] { edited_ = true; }, 0);
    }

    std::string_view id() const override { return kViewId; }
    std::string_view title() const override { return "Diff viewer"; }
    ImGuiWindowFlags window_flags() const override { return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse; }

    void navigate(ViewContext& ctx, const NavTarget& target) override {
        register_actions(ctx);
        if (target.va && target.va != va_) select_function(ctx, *target.va);
        if (!target.session.empty()) session_ = target.session;
        if (target.anchor == "manual") {
            wants_edit_ = true;
        } else if (target.anchor.starts_with("attempt:")) {
            if (auto n = parse_u64(target.anchor.substr(8))) wanted_attempt_ = static_cast<usize>(*n);
        }
    }

    void draw(ViewContext& ctx) override {
        register_actions(ctx);
        load_settings(ctx);
        drawn_frame_ = ImGui::GetFrameCount();
        poll(ctx);
        if (!va_) {
            ImGui::TextDisabled("Select a function (Function browser, Run monitor, Agent session or Ctrl+P) to see its diff.");
            return;
        }
        draw_toolbar(ctx);
        draw_header(ctx);
        const float bottom_h = std::max(ImGui::GetFrameHeightWithSpacing() * 6, ImGui::GetContentRegionAvail().y * 0.3f);
        const float main_h = std::max(ImGui::GetFrameHeight() * 4, ImGui::GetContentRegionAvail().y - bottom_h);
        if (editing_) draw_manual(ctx, main_h);
        else draw_diff(ctx, ImVec2(0, main_h));
        draw_panels(ctx);
    }

    // ---- DiffViewerControl ----
    std::optional<u64> function() const override { return va_; }
    usize attempt_count() const override { return attempts_.size(); }
    std::string shown_summary() const override {
        const CompiledSource* s = shown();
        return s ? s->summary : std::string();
    }
    bool editing() const override { return editing_; }
    void edit(ViewContext& ctx) override {
        const CompiledSource* s = attempt_result_ ? &*attempt_result_ : nullptr;
        if (s && s->va == va_) begin_edit(s->source, true);
        else if (attempt_ && *attempt_ < attempts_.size()) begin_edit(attempts_[*attempt_].source, false);
        else begin_edit(starter_source(ctx), false);
    }
    void set_text(std::string text) override {
        editor_.SetText(text);
        schedule_.edited(Clock::now());
        arm_timer_ = true;
    }
    std::string text() const override { return editor_.GetText(); }
    bool stale() const override { return editing_ && schedule_.stale(); }
    bool busy() const override {
        return edit_compile_.busy() || attempt_compile_.busy() || (verify_.valid() && !verify_.finished()) ||
               (attempts_job_.valid() && !attempts_job_.finished());
    }
    void verify_and_save(ViewContext& ctx) override {
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        auto program = ws ? ws->program() : nullptr;
        if (!editing_ || !va_ || !project || !program || (verify_.valid() && !verify_.finished())) return;
        verify_ = ctx.jobs.submit([program, project = *project, va = *va_, text = editor_.GetText(), session = manual_session_](const CancelToken& t) {
            return gui::verify_and_save(program, project, va, text, session, t);
        });
        verifying_text_ = editor_.GetText();
    }
    void hand_back(ViewContext& ctx) override;

private:
    // ---- state changes ----
    void select_function(ViewContext& ctx, u64 va) {
        va_ = va;
        attempts_.clear();
        attempts_job_.cancel();
        attempts_job_.reset();
        attempts_loaded_ = false;
        best_.reset();
        attempt_.reset();
        wanted_attempt_.reset();
        attempt_result_.reset();
        attempt_compile_.cancel();
        attempt_pending_ = false;
        edit_result_.reset();
        edit_compile_.cancel();
        editing_ = false;
        table_.clear();
        seen_signal_ = ~u64{0};
        load_attempts(ctx);
    }

    void load_attempts(ViewContext& ctx) {
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        auto program = ws ? ws->program() : nullptr;
        if (!va_ || !project || !program) {
            attempts_loaded_ = true;
            return;
        }
        attempts_job_ = ctx.jobs.submit([project = *project, program, va = *va_] {
            AttemptsLoad out;
            out.va = va;
            const Symbol fn = function_symbol(*program, va);
            out.attempts = vm::parse_attempts(project.attempts(fn));
            out.best_source = project.best_source(fn);
            return out;
        });
    }

    void compile_attempt(ViewContext& ctx, usize index) {
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        auto program = ws ? ws->program() : nullptr;
        if (!va_ || index >= attempts_.size() || !project || !program) return;
        attempt_ = index;
        attempt_pending_ = true;
        attempt_compile_.submit(ctx.jobs, [program, project = *project, va = *va_, source = attempts_[index].source](const CancelToken& t) {
            return compile_source(program, project, va, source, t);
        });
    }

    void begin_edit(const std::string& source, bool shown_matches) {
        editing_ = true;
        editor_.SetText(source);
        manual_session_ = vm::manual_session_id(Clock::now());
        edit_result_.reset();
        edit_compile_.cancel();
        schedule_.reset();
        if (shown_matches && attempt_result_) {
            edit_result_ = attempt_result_;  // the attempt's diff is the text's diff
        } else {
            schedule_.edited(Clock::now() - schedule_.quiet());  // compile it now
        }
        edited_ = false;
        markers_for_.reset();
    }

    std::string starter_source(ViewContext& ctx) const {
        Workspace* ws = ctx.services.workspace;
        auto program = ws ? ws->program() : nullptr;
        if (!program || !va_) return {};
        const Symbol fn = function_symbol(*program, *va_);
        return std::format("// {}\n// Write a complete translation unit that defines {}.\n", fn.display.empty() ? fn.name : fn.display, fn.name);
    }

    const CompiledSource* shown() const {
        if (editing_ && edit_result_) return &*edit_result_;
        if (attempt_result_ && attempt_result_->va == va_) return &*attempt_result_;
        return nullptr;
    }

    vm::DiffViewOptions layout() const { return {.differing_only = differing_only_, .context = static_cast<usize>(context_), .fuzzy = fuzzy_}; }
    DiffDrawOptions draw_options() const { return {.mode = raw_ ? vm::TextMode::raw : vm::TextMode::normalized, .bytes = bytes_, .relocations = relocations_}; }

    // ---- per frame ----
    void poll(ViewContext& ctx) {
        // Follow the selection unless editing (the edits belong to their function); with nothing
        // selected, the function of the run's newest session.
        if (!editing_) {
            std::optional<u64> wanted = ctx.selection.function_va;
            if (!wanted && !ctx.selection.session.empty() && ctx.snapshot)
                if (const auto* s = ctx.snapshot->session(ctx.selection.session)) wanted = s->va;
            if (!wanted && ctx.snapshot) {
                if (ctx.snapshot->sessions.size() != newest_count_ || ctx.snapshot->run_id != newest_run_) {
                    newest_count_ = ctx.snapshot->sessions.size();
                    newest_run_ = ctx.snapshot->run_id;
                    const events::SessionState* newest = vm::newest_session(*ctx.snapshot);
                    newest_va_ = newest ? std::optional<u64>(newest->va) : std::nullopt;
                }
                wanted = newest_va_;
            }
            if (wanted && wanted != va_) select_function(ctx, *wanted);
        }
        // New attempts recorded by a running session of the function: reload the history. Only the
        // workers' sessions can add attempts, so a large run costs nothing here.
        if (ctx.snapshot && va_ && ctx.snapshot->last_seq != signal_seq_) {
            signal_seq_ = ctx.snapshot->last_seq;
            u64 signal = 0;
            for (const auto& [id, w] : ctx.snapshot->workers)
                if (const auto* s = w.session.empty() ? nullptr : ctx.snapshot->session(w.session); s && s->va == *va_)
                    signal += static_cast<u64>(s->compiles) + s->scores.size() + 1;
            if (seen_signal_ != ~u64{0} && signal != seen_signal_ && attempts_loaded_ && !attempts_job_.valid()) load_attempts(ctx);
            seen_signal_ = signal;
        }
        try {
            if (auto loaded = attempts_job_.take()) {
                attempts_job_.reset();
                if (loaded->va == va_) on_attempts(ctx, std::move(*loaded));
            }
            if (auto r = attempt_compile_.poll()) {
                attempt_pending_ = false;
                attempt_result_ = std::move(*r);
                if (!editing_) table_.set(attempt_result_->diff, layout());
            }
            if (auto r = edit_compile_.poll()) {
                if (schedule_.finished(r->generation)) {
                    edit_result_ = std::move(r->result);
                    markers_for_.reset();
                }
            }
            if (auto v = verify_.take()) {
                verify_.reset();
                on_verified(ctx, std::move(*v));
            }
            if (auto b = binding_.take()) {
                binding_.reset();
                ctx.notify(b->ok ? Severity::info : Severity::error, b->message);
                if (b->ok && ctx.services.workspace) {
                    ctx.services.workspace->reload_symbols();
                    if (attempt_ && *attempt_ < attempts_.size() && !editing_) compile_attempt(ctx, *attempt_);
                    if (editing_) schedule_.edited(Clock::now() - schedule_.quiet());
                }
            }
        } catch (const std::exception& e) {
            ctx.notify(Severity::error, std::format("Diff viewer: {}", e.what()));
        }
        // Take over: edit the best attempt once the history is known (edits under way are kept).
        if (wants_edit_ && attempts_loaded_) {
            wants_edit_ = false;
            if (!editing_ && best_) {
                const std::string& source = attempts_[*best_].source;
                begin_edit(source, attempt_result_ && attempt_result_->va == va_ && attempt_result_->source == source);
            } else if (!editing_) {
                edit(ctx);
            }
        }
        if (editing_) poll_editing(ctx);
        if (editing_) table_.set(edit_result_ ? edit_result_->diff : nullptr, layout());
        else table_.set(attempt_result_ && attempt_result_->va == va_ ? attempt_result_->diff : nullptr, layout());
        // A hand-back waiting for its session to start.
        if (pending_hand_back_ && ctx.snapshot) {
            const auto live = vm::live_sessions(*ctx.snapshot);
            if (auto it = live.find(pending_hand_back_->va); it != live.end() && !pending_hand_back_->old_sessions.contains(it->second->id)) {
                if (ctx.services.commands->inject(it->second->id, pending_hand_back_->text))
                    ctx.notify(Severity::info, "The edited source went to the new session as guidance.",
                               NavEntry{"agent_session", {.va = pending_hand_back_->va, .session = it->second->id}});
                pending_hand_back_.reset();
            } else if (!ctx.services.commands->live()) {
                ctx.notify(Severity::warning, "The run ended before the function's session started: the edited source was not handed back.");
                pending_hand_back_.reset();
            }
        }
    }

    void on_attempts(ViewContext& ctx, AttemptsLoad loaded) {
        const bool first = !attempts_loaded_;
        const bool was_latest = attempt_ && *attempt_ + 1 == attempts_.size();
        attempts_ = std::move(loaded.attempts);
        best_ = vm::best_attempt(attempts_, loaded.best_source);
        attempts_loaded_ = true;
        if (wanted_attempt_) {
            auto it = std::ranges::find(attempts_, *wanted_attempt_, &vm::AttemptRecord::index);
            if (it != attempts_.end()) compile_attempt(ctx, static_cast<usize>(it - attempts_.begin()));
            wanted_attempt_.reset();
        } else if (first && !attempts_.empty()) {
            compile_attempt(ctx, best_ ? *best_ : attempts_.size() - 1);
        } else if (was_latest && !attempts_.empty() && !editing_) {
            compile_attempt(ctx, attempts_.size() - 1);  // following the run's newest attempt
        }
        if (first && attempts_.empty() && loaded.best_source && !editing_) {
            // A best source without a history (an older project): show it all the same.
            attempt_result_.reset();
            Workspace* ws = ctx.services.workspace;
            attempt_pending_ = ws && ws->project() && ws->program();
            if (attempt_pending_)
                attempt_compile_.submit(ctx.jobs, [program = ws->program(), project = *ws->project(), va = *va_, source = *loaded.best_source](
                                                      const CancelToken& t) { return compile_source(program, project, va, source, t); });
        }
    }

    void poll_editing(ViewContext& ctx) {
        const auto now = Clock::now();
        if (edited_) {
            edited_ = false;
            schedule_.edited(now);
            arm_timer_ = true;
        }
        // Wake the UI when the edit becomes due: a short job that waits out the quiet period.
        if (arm_timer_) {
            arm_timer_ = false;
            if (auto at = schedule_.due_at()) {
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(*at - now) + std::chrono::milliseconds(5);
                timer_.submit(ctx.jobs, [] {}, std::max(wait, std::chrono::milliseconds(0)));
            }
        }
        (void)timer_.poll();
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        auto program = ws ? ws->program() : nullptr;
        if (schedule_.due(now) && project && program && va_) {
            const u64 generation = schedule_.start();
            edit_compile_.submit(ctx.jobs, [program, project = *project, va = *va_, text = editor_.GetText(), generation](const CancelToken& t) {
                return EditCompile{generation, compile_source(program, project, va, text, t)};
            });
        }
        // Error markers for the diagnostics of the text shown.
        if (edit_result_ && markers_for_ != schedule_.shown()) {
            markers_for_ = schedule_.shown();
            editor_.ClearMarkers();
            const ThemeColors& c = ctx.colors();
            for (const auto& d : edit_result_->diagnostics) {
                if (d.line <= 0 || (d.severity != "error" && d.severity != "fatal error" && d.severity != "warning")) continue;
                const bool error = d.severity != "warning";
                const ImVec4 color = error ? c.error : c.warn;
                const std::string tip = std::format("{} {}: {}", d.severity, d.code, d.message);
                editor_.AddMarker(static_cast<size_t>(d.line - 1), ImGui::GetColorU32(color), ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.25f)),
                                  tip, tip);
            }
        }
    }

    void on_verified(ViewContext& ctx, VerifyResult r) {
        if (!r.error.empty() && !r.saved) ctx.notify(Severity::error, r.message);
        else ctx.notify(r.saved ? Severity::info : Severity::warning, r.message, NavEntry{kViewId, {.va = r.compiled.va}});
        if (r.compiled.va == va_ && editing_ && verifying_text_ == editor_.GetText()) {
            // The verification compiled exactly the text shown: its diff is current.
            schedule_.reset();
            edit_result_ = std::move(r.compiled);
            markers_for_.reset();
        }
        load_attempts(ctx);
    }

    // ---- drawing ----
    void draw_toolbar(ViewContext& ctx) {
        Workspace* ws = ctx.services.workspace;
        auto program = ws ? ws->program() : nullptr;
        const Symbol fn = program ? function_symbol(*program, *va_) : Symbol{};
        const std::string name = !fn.display.empty() ? fn.display : !fn.name.empty() ? fn.name : hex(*va_, 8);
        if (ImGui::TextLink(std::format("{}##fn", name).c_str())) ctx.open("inspector", NavTarget{.va = va_});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s at %s\nOpen in the Inspector.", fn.name.c_str(), hex(*va_, 8).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hex(*va_, 8).c_str());
        if (editing_) {
            ImGui::SameLine();
            colored_text(ctx.colors().warn, "Manual mode");
        }

        // Attempt history.
        if (!attempts_.empty()) {
            int k = static_cast<int>(attempt_ ? *attempt_ + 1 : attempts_.size());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::SliderInt("##attempt", &k, 1, static_cast<int>(attempts_.size()), "attempt %d")) {
                k = std::clamp(k, 1, static_cast<int>(attempts_.size()));
                if (!attempt_ || static_cast<usize>(k - 1) != *attempt_) compile_attempt(ctx, static_cast<usize>(k - 1));
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Scrub through every attempt for this function.");
            ImGui::SameLine();
            ImGui::Text("of %zu", attempts_.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(!best_);
            if (ImGui::SmallButton("Best")) compile_attempt(ctx, *best_);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Latest")) compile_attempt(ctx, attempts_.size() - 1);
            if (attempt_ && *attempt_ < attempts_.size()) {
                const vm::AttemptRecord& a = attempts_[*attempt_];
                ImGui::SameLine();
                if (best_ == attempt_) colored_text(ctx.colors().ok, "[best]");
                else ImGui::TextDisabled("[%.1f%%]", a.match_percent);
                ImGui::SameLine();
                ImGui::TextDisabled("%s #%d%s, %s", a.session.c_str(), a.attempt, a.origin == "user" ? " (by hand)" : "", a.time.c_str());
                if (editing_) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Load into the editor")) begin_edit(a.source, attempt_result_ && attempt_result_->source == a.source);
                }
            }
        } else if (!attempts_loaded_) {
            ImGui::TextDisabled("Loading the attempt history...");
        } else {
            ImGui::TextDisabled(ws && ws->project() ? "No attempts recorded for this function yet." : "Open a project to see this function's attempts.");
        }

        // View toggles (remembered per project).
        bool changed = false;
        changed |= ImGui::Checkbox("Raw", &raw_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Numbers as encoded instead of the names they refer to.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Relocations", &relocations_);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Bytes (Ctrl+B)", &bytes_);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Fuzzy registers and stack", &fuzzy_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Show register-only and stack-only differences as equal.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Differing rows only", &differing_only_);
        if (differing_only_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
            if (ImGui::InputInt("context", &context_)) {
                context_ = std::clamp(context_, 0, 20);
                changed = true;
            }
        }
        if (changed) save_settings(ctx);

        // Actions.
        const CompiledSource* s = shown();
        ImGui::BeginDisabled(!table_.diff());
        if (ImGui::Button("Copy rows")) ImGui::SetClipboardText(table_.copy_text(draw_options()).c_str());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The selected rows (or every shown row) as decomp diff prints them.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!table_.diff());
        if (ImGui::Button("Next (F7)")) table_.step(true);
        ImGui::SameLine();
        if (ImGui::Button("Previous (Shift+F7)")) table_.step(false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (s && s->diff) {
            const auto diff = s->diff;
            const vm::DiffExportOptions options{.compact = differing_only_, .context = static_cast<usize>(context_), .bytes = bytes_};
            const std::array<ExportFormat, 2> formats = {
                ExportFormat{"text", "txt", [diff, options] { return vm::diff_text(*diff, options); }},
                ExportFormat{"JSON", "json", [diff, options] { return vm::diff_json(*diff, options); }},
            };
            export_button(ctx, "##export_diff", std::format("diff-{}", export_stem(ctx)), formats);
            ImGui::SameLine();
        }
        draw_manual_buttons(ctx);
    }

    std::string export_stem(ViewContext& ctx) const {
        Workspace* ws = ctx.services.workspace;
        auto program = ws ? ws->program() : nullptr;
        return program && va_ ? project::safe_function_name(function_symbol(*program, *va_)) : std::string("function");
    }

    void draw_manual_buttons(ViewContext& ctx) {
        Workspace* ws = ctx.services.workspace;
        const bool has_project = ws && ws->project() && ws->program();
        RunCommands& commands = *ctx.services.commands;
        const events::SessionState* live = nullptr;
        if (ctx.snapshot && commands.live()) {
            const auto sessions = vm::live_sessions(*ctx.snapshot);
            if (auto it = sessions.find(*va_); it != sessions.end()) live = it->second;
        }
        if (!editing_) {
            if (live) {
                ImGui::BeginDisabled(!has_project);
                if (ImGui::Button("Take over...")) ImGui::OpenPopup("##take_over");
                ImGui::EndDisabled();
                if (ImGui::BeginPopup("##take_over")) {
                    ImGui::TextDisabled("The agent's session %s is working on this function.", live->id.c_str());
                    if (ImGui::MenuItem("Pause the session and edit (you can hand back)")) take_over(ctx, *va_, live->id, TakeOver::pause);
                    if (ImGui::MenuItem("End the session and edit")) take_over(ctx, *va_, live->id, TakeOver::end);
                    ImGui::EndPopup();
                }
            } else {
                ImGui::BeginDisabled(!has_project);
                if (ImGui::Button("Edit by hand")) edit(ctx);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(has_project ? "Edit the source of the shown attempt with live recompiles (manual mode)."
                                                  : "Manual mode needs an open project.");
            }
            return;
        }
        const bool verifying = verify_.valid() && !verify_.finished();
        ImGui::BeginDisabled(verifying || !has_project);
        if (ImGui::Button("Verify and save (Ctrl+S)")) verify_and_save(ctx);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Compile, diff and require a byte-exact match (as submit_result does), then save the source to "
                              "src/functions/ and mark the function matched.");
        ImGui::SameLine();
        if (ImGui::Button("Hand back")) hand_back(ctx);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(live ? "Send the edited source to the agent's session as guidance (and resume it)."
                                   : "Start a session on this function with the edited source as guidance.");
        ImGui::SameLine();
        if (ImGui::Button("Stop editing")) {
            editing_ = false;
            edit_compile_.cancel();
            schedule_.reset();
        }
        if (verifying) {
            ImGui::SameLine();
            ImGui::TextDisabled("verifying...");
        }
    }

    void draw_header(ViewContext& ctx) {
        const CompiledSource* s = shown();
        const ThemeColors& c = ctx.colors();
        if (!s) {
            Workspace* ws = ctx.services.workspace;
            if (attempt_pending_ || edit_compile_.busy() || (editing_ && schedule_.stale())) ImGui::TextDisabled("Compiling...");
            else if (!attempts_loaded_) ImGui::TextDisabled("Loading the attempt history...");
            else if (!ws || !ws->project()) ImGui::TextDisabled("%s", session_summary(ctx).c_str());
            else if (attempts_.empty() && !editing_) ImGui::TextDisabled("No attempt to show: edit the function by hand to make one.");
            else ImGui::TextDisabled("No diff.");
            return;
        }
        if (s->diff) {
            const auto& d = *s->diff;
            colored_text(d.byte_exact ? c.ok : d.exact ? c.warn : c.text, std::format("match {:.1f}%", d.match_percent));
            ImGui::SameLine();
            status_label(d.exact ? "exact" : "not exact", d.exact ? c.ok : c.muted);
            ImGui::SameLine();
            status_label(d.byte_exact ? "byte-exact" : "bytes differ", d.byte_exact ? c.ok : c.muted);
            const DiffPalette p = ctx.diff_palette();
            struct Count {
                char glyph;
                const char* name;
                usize n;
                ImU32 color;
            };
            const Count counts[] = {{'=', "equal", d.equal, p.equal},    {'e', "encoding", d.encoding, p.encoding}, {'~', "operand", d.operand, p.operand},
                                    {'!', "opcode", d.opcode, p.opcode}, {'+', "insert", d.inserted, p.insert},     {'-', "delete", d.deleted, p.del}};
            for (const Count& k : counts) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, k.n ? k.color : ImGui::GetColorU32(c.muted));
                ImGui::Text("%c %zu %s", k.glyph, k.n, k.name);
                ImGui::PopStyleColor();
            }
        } else {
            colored_text(s->compiled ? c.warn : c.error, s->compiled ? std::format("No diff: {}", s->summary) : std::string("The compile failed."));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%s in %lld ms%s)", s->compiled ? "compiled" : "failed", s->duration_ms, s->cached ? ", cached" : "");
        if (editing_ && schedule_.stale()) {
            ImGui::SameLine();
            colored_text(c.warn, edit_compile_.busy() || schedule_.compiling() ? "stale: compiling the latest edit..." : "stale: recompiles when you stop typing");
        }
    }

    std::string session_summary(ViewContext& ctx) const {
        if (!ctx.snapshot) return "No attempt to show.";
        const events::SessionState* latest = nullptr;
        for (const auto& [id, s] : ctx.snapshot->sessions)
            if (s->va == va_ && (!latest || s->started > latest->started)) latest = s.get();
        if (!latest) return "No attempt to show.";
        return std::format("Session {}: {} attempt(s), last {:.1f}%, best {:.1f}%. Open a project to see the assembly.", latest->id,
                           latest->scores.size(), latest->last_match, latest->best_match);
    }

    void draw_diff(ViewContext& ctx, ImVec2 size) {
        Workspace* ws = ctx.services.workspace;
        auto program = ws ? ws->program() : nullptr;
        table_.draw(ctx, "##diff_table", size, draw_options(), program.get());
    }

    void draw_manual(ViewContext& ctx, float height) {
        if (!ImGui::BeginTable("##manual", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, height))) return;
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("Diff", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const CompiledSource* s = edit_result_ ? &*edit_result_ : nullptr;
        const float diag_h = s && !s->diagnostics.empty() ? std::min(ImGui::GetTextLineHeightWithSpacing() * 6, height * 0.35f) : 0.0f;
        if (palette_theme_ != ctx.settings.theme) {
            palette_theme_ = ctx.settings.theme;
            editor_.SetPalette(ctx.settings.theme == Theme::light ? TextEditor::GetLightPalette() : TextEditor::GetDarkPalette());
        }
        if (ImGui::BeginChild("##editor_host", ImVec2(0, height - diag_h - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            editor_.Render("##source");
            ImGui::PopFont();
            // The editor takes text input while focused: global shortcuts stay quiet (Ctrl+S still saves).
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) ctx.claim_keyboard();
        }
        ImGui::EndChild();
        if (diag_h > 0) draw_diagnostics(ctx, *s, diag_h);
        ImGui::TableNextColumn();
        draw_diff(ctx, ImVec2(0, height - ImGui::GetStyle().CellPadding.y * 2));
        ImGui::EndTable();
    }

    void draw_diagnostics(ViewContext& ctx, const CompiledSource& s, float height) {
        if (!ImGui::BeginChild("##diagnostics", ImVec2(0, height), ImGuiChildFlags_Borders)) {
            ImGui::EndChild();
            return;
        }
        const ThemeColors& c = ctx.colors();
        for (usize i = 0; i < s.diagnostics.size(); ++i) {
            const auto& d = s.diagnostics[i];
            ImGui::PushID(static_cast<int>(i));
            const bool error = d.severity == "error" || d.severity == "fatal error";
            ImGui::PushStyleColor(ImGuiCol_Text, error ? c.error : d.severity == "warning" ? c.warn : c.muted);
            const std::string text = std::format("line {}:{}: {}{}{}: {}", d.line, d.column, d.severity, d.code.empty() ? "" : " ", d.code, d.message);
            // Clickable: jumps to the line in the editor.
            if (ImGui::Selectable(label_text(text).c_str()) && d.line > 0) {
                editor_.SetCursor(TextEditor::DocPos(static_cast<size_t>(d.line - 1), static_cast<size_t>(std::max(0, d.column - 1))));
                editor_.ScrollToLine(static_cast<size_t>(d.line - 1), TextEditor::Scroll::alignMiddle);
                editor_.SetFocus();
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void draw_panels(ViewContext& ctx) {
        if (!ImGui::BeginTabBar("##diff_panels")) return;
        const CompiledSource* s = shown();
        Workspace* ws = ctx.services.workspace;
        auto program = ws ? ws->program() : nullptr;
        const usize hint_count = s && s->diff ? s->diff->hints.size() + s->diff->bindings.size() : 0;
        if (ImGui::BeginTabItem(std::format("Hints ({})###hints", hint_count).c_str())) {
            draw_hints(ctx, s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Data###data")) {
            draw_data(ctx, s, program.get());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Row details###details")) {
            if (ImGui::BeginChild("##details_scroll", ImVec2(0, 0))) table_.draw_details(ctx, draw_options(), program.get());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compiler output###output")) {
            if (!s) ImGui::TextDisabled("Nothing compiled yet.");
            else if (ImGui::BeginChild("##output_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                ImGui::TextUnformatted(s->output.empty() ? "(no output)" : s->output.c_str());
                ImGui::PopFont();
            }
            if (s) ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void draw_hints(ViewContext& ctx, const CompiledSource* s) {
        if (!s || !s->diff) {
            ImGui::TextDisabled("No diff.");
            return;
        }
        const auto& d = *s->diff;
        if (d.hints.empty() && d.bindings.empty()) {
            ImGui::TextDisabled(d.byte_exact ? "Byte-exact: nothing to fix." : "No hints for this diff.");
            return;
        }
        if (!ImGui::BeginChild("##hints_scroll", ImVec2(0, 0))) {
            ImGui::EndChild();
            return;
        }
        for (usize i = 0; i < d.hints.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto rows = vm::hint_rows(d, d.hints[i]);
            ImGui::BeginDisabled(rows.empty());
            if (ImGui::SmallButton(rows.empty() ? "-" : std::format("{} row{}", rows.size(), rows.size() == 1 ? "" : "s").c_str())) table_.select(rows);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Select the rows this hint is about.");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", d.hints[i].c_str());
            ImGui::PopID();
        }
        if (!d.bindings.empty()) ImGui::SeparatorText("Binding suggestions");
        Workspace* ws = ctx.services.workspace;
        const bool can_write = ws && ws->project() && !ws->run_live() && !(binding_.valid() && !binding_.finished());
        for (usize i = 0; i < d.bindings.size(); ++i) {
            const auto& b = d.bindings[i];
            ImGui::PushID(static_cast<int>(1000 + i));
            const auto rows = vm::binding_rows(d, b);
            if (ImGui::SmallButton(std::format("{} row{}", rows.size(), rows.size() == 1 ? "" : "s").c_str())) table_.select(rows);
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_write);
            if (ImGui::SmallButton("Accept")) accept_binding(ctx, d, b);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(can_write ? "Name the address after the candidate's symbol (symbols.txt, source=user)."
                                            : "Needs an open project and no live run.");
            ImGui::SameLine();
            ImGui::TextWrapped("%s has no symbol; the candidate uses %s (%s)", hex(b.target_va, 8).c_str(), b.candidate_symbol.c_str(),
                               display_name(b.candidate_symbol).c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void accept_binding(ViewContext& ctx, const matching::FunctionDiff& d, const matching::Binding& b) {
        Workspace* ws = ctx.services.workspace;
        if (!ws || !ws->project()) return;
        const SymbolKind kind = vm::binding_kind(d, b);
        binding_ = ctx.jobs.submit([project = *ws->project(), b, kind]() mutable {
            BindingWrite out;
            project::SymbolEdit edit;
            edit.va = b.target_va;
            edit.name = b.candidate_symbol;
            edit.kind = kind;
            auto r = project.set_symbol(edit, project::ChangeOrigin{SymbolSource::user, "", "binding from diff"});
            out.ok = r.has_value();
            out.message = r ? std::format("Named {} {} ({}).", hex(b.target_va, 8), b.candidate_symbol, to_string(kind))
                            : std::format("Cannot name {}: {}", hex(b.target_va, 8), r.error().message);
            return out;
        });
    }

    void draw_data(ViewContext& ctx, const CompiledSource* s, const Program* program) {
        if (!s || !s->diff) {
            ImGui::TextDisabled("No diff.");
            return;
        }
        const auto entries = vm::data_diff(*s->diff, program);
        if (entries.empty()) {
            ImGui::TextDisabled("The function references no strings, constants or jump tables.");
            return;
        }
        const ThemeColors& c = ctx.colors();
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("##data", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Candidate");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(std::format("{}", e.row + 1).c_str())) {
                const usize row = e.row;
                table_.select(std::span(&row, 1));
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(std::string(matching::to_string(e.kind)).c_str());
            ImGui::TableNextColumn();
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            ImGui::TextWrapped("%s", e.target.empty() ? "-" : e.target.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", e.candidate.empty() ? "-" : e.candidate.c_str());
            ImGui::PopFont();
            ImGui::TableNextColumn();
            status_label(e.equal ? "same" : "differs", e.equal ? c.ok : c.error);
            if (!e.entries.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("cases");
                ImGui::TableNextColumn();
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                std::string left, right;
                for (usize k = 0; k < e.entries.size(); ++k) {
                    left += std::format("{}: {}\n", k, e.entries[k].first);
                    right += std::format("{}: {}\n", k, e.entries[k].second);
                }
                ImGui::TextUnformatted(left.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(right.c_str());
                ImGui::PopFont();
                ImGui::TableNextColumn();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- settings and actions ----
    void load_settings(ViewContext& ctx) {
        const std::string key = project_key(ctx.project.root);
        if (settings_loaded_ && key == settings_key_) return;
        settings_loaded_ = true;
        settings_key_ = key;
        const Json& state = ctx.view_state(kViewId);
        raw_ = json_bool_or(state, "raw", false);
        relocations_ = json_bool_or(state, "relocations", false);
        bytes_ = json_bool_or(state, "bytes", false);
        fuzzy_ = json_bool_or(state, "fuzzy", false);
        differing_only_ = json_bool_or(state, "differing_only", false);
        context_ = static_cast<int>(std::clamp<long long>(json_int_or(state, "context", 2), 0, 20));
    }

    void save_settings(ViewContext& ctx) {
        Json& state = ctx.view_state(kViewId);
        state["raw"] = raw_;
        state["relocations"] = relocations_;
        state["bytes"] = bytes_;
        state["fuzzy"] = fuzzy_;
        state["differing_only"] = differing_only_;
        state["context"] = context_;
        ctx.mark_settings_dirty();
    }

    bool visible() const { return drawn_frame_ >= 0 && ImGui::GetFrameCount() - drawn_frame_ <= 1; }

    void register_actions(ViewContext& ctx) {
        if (actions_registered_) return;
        actions_registered_ = true;
        ViewContext* c = &ctx;
        const auto shown_diff = [this] { return visible() && table_.diff() != nullptr; };
        auto toggle = [this, c](bool& flag) {
            flag = !flag;
            save_settings(*c);
        };
        ctx.actions.add({.id = "diff.next_difference", .label = "Next differing row", .category = "Diff viewer", .shortcut = ImGuiKey_F7,
                         .enabled = shown_diff, .run = [this] { table_.step(true); }});
        ctx.actions.add({.id = "diff.previous_difference", .label = "Previous differing row", .category = "Diff viewer",
                         .shortcut = ImGuiMod_Shift | ImGuiKey_F7, .enabled = shown_diff, .run = [this] { table_.step(false); }});
        ctx.actions.add({.id = "diff.toggle_bytes", .label = "Toggle raw bytes", .category = "Diff viewer", .shortcut = ImGuiMod_Ctrl | ImGuiKey_B,
                         .enabled = [this] { return visible(); }, .run = [this, toggle] { toggle(bytes_); }});
        ctx.actions.add({.id = "diff.toggle_raw", .label = "Toggle normalized or raw text", .category = "Diff viewer",
                         .run = [this, toggle] { toggle(raw_); }});
        ctx.actions.add({.id = "diff.toggle_relocations", .label = "Toggle relocations", .category = "Diff viewer",
                         .run = [this, toggle] { toggle(relocations_); }});
        ctx.actions.add({.id = "diff.toggle_fuzzy", .label = "Toggle fuzzy registers and stack", .category = "Diff viewer",
                         .run = [this, toggle] { toggle(fuzzy_); }});
        ctx.actions.add({.id = "diff.toggle_differing", .label = "Toggle differing rows only", .category = "Diff viewer",
                         .run = [this, toggle] { toggle(differing_only_); }});
        ctx.actions.add({.id = "diff.copy_rows", .label = "Copy diff rows", .category = "Diff viewer", .enabled = [this] { return table_.diff() != nullptr; },
                         .run = [this] { ImGui::SetClipboardText(table_.copy_text(draw_options()).c_str()); }});
        ctx.actions.add({.id = "diff.export_text", .label = "Export diff as text", .category = "Diff viewer",
                         .enabled = [this, c] { return shown() && shown()->diff && c->project.open; },
                         .run = [this, c] { export_diff(*c, false); }});
        ctx.actions.add({.id = "diff.export_json", .label = "Export diff as JSON", .category = "Diff viewer",
                         .enabled = [this, c] { return shown() && shown()->diff && c->project.open; },
                         .run = [this, c] { export_diff(*c, true); }});
        ctx.actions.add({.id = "diff.edit", .label = "Edit the function by hand (manual mode)", .category = "Diff viewer",
                         .enabled = [this, c] { return va_ && !editing_ && c->services.workspace && c->services.workspace->project(); },
                         .run = [this, c] { edit(*c); }});
        ctx.actions.add({.id = "diff.verify_and_save", .label = "Verify and save (manual mode)", .category = "Diff viewer",
                         .shortcut = ImGuiMod_Ctrl | ImGuiKey_S, .enabled = [this] { return editing_ && !(verify_.valid() && !verify_.finished()); },
                         .run = [this, c] { verify_and_save(*c); }, .in_text_input = true});
        ctx.actions.add({.id = "diff.hand_back", .label = "Hand the edited source back to the agent", .category = "Diff viewer",
                         .enabled = [this] { return editing_; }, .run = [this, c] { hand_back(*c); }});
    }

    void export_diff(ViewContext& ctx, bool json) {
        const CompiledSource* s = shown();
        if (!s || !s->diff) return;
        const vm::DiffExportOptions options{.compact = differing_only_, .context = static_cast<usize>(context_), .bytes = bytes_};
        const std::string content = json ? vm::diff_json(*s->diff, options) : vm::diff_text(*s->diff, options);
        if (auto written = write_export(ctx.project.root, std::format("diff-{}", export_stem(ctx)), json ? "json" : "txt", content))
            ctx.notify(Severity::info, std::format("Exported to {}", fs::to_utf8(*written)));
        else
            ctx.notify(Severity::error, std::format("Cannot export: {}", written.error().message));
    }

    struct PendingHandBack {
        u64 va = 0;
        std::string text;
        std::set<std::string> old_sessions;  // the function's sessions before the hand-back
    };

    std::optional<u64> va_;
    std::string session_;  // the session navigation named (take over, hand back)
    std::optional<u64> newest_va_;  // the newest session's function (shown when nothing is selected)
    usize newest_count_ = ~usize{0};
    std::string newest_run_;
    bool wants_edit_ = false;
    std::optional<usize> wanted_attempt_;  // an attempts.jsonl index from navigation

    std::vector<vm::AttemptRecord> attempts_;
    JobHandle<AttemptsLoad> attempts_job_;
    bool attempts_loaded_ = false;
    std::optional<usize> best_, attempt_;
    u64 signal_seq_ = ~u64{0}, seen_signal_ = ~u64{0};

    LatestWins<CompiledSource> attempt_compile_;
    bool attempt_pending_ = false;  // a compile of the chosen attempt has not been shown yet
    std::optional<CompiledSource> attempt_result_;
    DiffTable table_;

    // Manual mode.
    bool editing_ = false;
    TextEditor editor_;
    bool edited_ = false;
    bool arm_timer_ = false;
    std::string manual_session_;
    vm::RecompileSchedule schedule_;
    LatestWins<EditCompile> edit_compile_;
    LatestWins<void> timer_;
    std::optional<CompiledSource> edit_result_;
    std::optional<u64> markers_for_;
    JobHandle<VerifyResult> verify_;
    std::string verifying_text_;
    std::optional<PendingHandBack> pending_hand_back_;
    JobHandle<BindingWrite> binding_;
    std::optional<Theme> palette_theme_;

    // Toggles.
    bool raw_ = false, relocations_ = false, bytes_ = false, fuzzy_ = false, differing_only_ = false;
    int context_ = 2;
    bool settings_loaded_ = false;
    std::string settings_key_;
    bool actions_registered_ = false;
    int drawn_frame_ = -1;
};

} // namespace

void DiffViewerView::hand_back(ViewContext& ctx) {
    if (!editing_ || !va_) return;
    const std::string source = editor_.GetText();
    const std::string text = hand_back_text(source, edit_result_ ? edit_result_->summary : std::string());
    RunCommands& commands = *ctx.services.commands;
    if (ctx.snapshot && commands.live()) {
        const auto live = vm::live_sessions(*ctx.snapshot);
        if (auto it = live.find(*va_); it != live.end()) {
            const events::SessionState& s = *it->second;
            if (commands.inject(s.id, text)) {
                // A session paused for the take-over goes on with the source.
                if (auto w = ctx.snapshot->workers.find(s.worker); w != ctx.snapshot->workers.end() && w->second.phase == "paused")
                    commands.resume_worker(s.worker);
                ctx.notify(Severity::info, "Handed back: the edited source is queued as guidance for the agent's session.",
                           NavEntry{"agent_session", {.va = va_, .session = s.id}});
                editing_ = false;
                ctx.open("agent_session", NavTarget{.va = va_, .session = s.id});
                return;
            }
        }
        // The run goes on without a session for this function: queue it again, and send the source once
        // its session starts.
        std::set<std::string> old_sessions;
        for (const auto& [id, s] : ctx.snapshot->sessions)
            if (s->va == *va_) old_sessions.insert(id);
        if (commands.requeue(*va_) || commands.enqueue({*va_}) > 0) {
            pending_hand_back_ = PendingHandBack{*va_, text, std::move(old_sessions)};
            ctx.notify(Severity::info, "Handed back: the function is queued again; the edited source follows as guidance when its session starts.");
            editing_ = false;
            return;
        }
    }
    Workspace* ws = ctx.services.workspace;
    if (!ws || !ws->project()) {
        ctx.notify(Severity::error, "Cannot hand back: no project is open.");
        return;
    }
    RunRequest request;
    request.functions = {*va_};
    request.workers = 1;
    request.guidance = {text};
    if (auto started = ws->start_run(request); !started) {
        ctx.notify(Severity::error, std::format("Cannot hand back: {}", started.error().message));
        return;
    }
    ctx.notify(Severity::info, "Handed back: a run on this function started with the edited source as guidance.",
               NavEntry{"run_monitor", {}});
    editing_ = false;
}

std::unique_ptr<View> make_diff_viewer_view() { return std::make_unique<DiffViewerView>(); }

} // namespace decomp::gui
