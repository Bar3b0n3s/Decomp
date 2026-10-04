// Agent session (docs/ui.md#agent-session): one session of the shown run, turn by turn from its
// transcript (read incrementally while the session runs) and the turn being streamed; its scores, the
// function's source history, notes and outcome; and the supervisor's controls: guidance, pausing or
// ending the session, taking over, comparing attempts and exporting the transcript.

#include "gui/views/agent_session_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/export.hpp"
#include "gui/views/code_view.hpp"
#include "gui/views/diff_table.hpp"
#include "gui/views/manual_mode.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/attempts.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/diff_view.hpp"
#include "viewmodel/series.hpp"
#include "viewmodel/timeline.hpp"
#include "viewmodel/transcript.hpp"

#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace decomp::gui {

namespace {

using Kind = vm::TimelineItem::Kind;
using Key = std::pair<i32, i32>;

constexpr const char* kViewId = "agent_session";
constexpr double kPollSeconds = 0.25;   // how often a live session's transcript is read
constexpr usize kResultLines = 12;      // tool results longer than this start collapsed

struct LoadedTranscript {
    std::string session;
    vm::TranscriptReader reader;
    std::string error;
};

struct FunctionHistory {
    u64 va = 0;
    std::vector<vm::AttemptRecord> attempts;
    std::optional<std::string> best_source;
    std::string notes;
};

std::string thousands(long long v) {
    std::string digits = std::to_string(v < 0 ? -v : v), out;
    for (usize i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return v < 0 ? "-" + out : out;
}

std::string seconds(long long ms) { return std::format("{:.1f} s", static_cast<double>(ms) / 1000.0); }

usize count_lines(std::string_view text) { return text.empty() ? 0 : static_cast<usize>(std::ranges::count(text, '\n')) + (text.back() == '\n' ? 0 : 1); }

void wrapped(std::string_view text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

void wrapped_colored(const ImVec4& color, std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

// The first `n` lines of `text`.
std::string_view head_lines(std::string_view text, usize n) {
    usize pos = 0;
    for (usize i = 0; i < n; ++i) {
        pos = text.find('\n', pos);
        if (pos == std::string_view::npos) return text;
        ++pos;
    }
    return text.substr(0, pos);
}

class AgentSessionView final : public View, public AgentSessionControl {
public:
    std::string_view id() const override { return kViewId; }
    std::string_view title() const override { return "Agent session"; }
    ImGuiWindowFlags window_flags() const override { return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse; }

    void navigate(ViewContext& ctx, const NavTarget& target) override {
        register_actions(ctx);
        if (!target.session.empty()) {
            show_session(ctx, target.session);
            pinned_selection_ = ctx.selection;
            pinned_ = true;
        } else if (target.va) {
            pinned_ = false;
        }
    }

    void draw(ViewContext& ctx) override {
        register_actions(ctx);
        drawn_frame_ = ImGui::GetFrameCount();
        resolve_session(ctx);
        poll(ctx);
        const events::SessionState* s = state(ctx);
        if (session_.empty()) {
            draw_session_list(ctx);
            return;
        }
        draw_header(ctx, s);
        if (ImGui::BeginTabBar("##session_tabs")) {
            if (ImGui::BeginTabItem("Timeline")) {
                draw_timeline_tab(ctx, s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Attempts")) {
                draw_attempts_tab(ctx, s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Notes")) {
                draw_notes_tab(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    // ---- AgentSessionControl ----
    std::string session() const override { return session_; }
    bool transcript_loaded() const override { return reader_.has_value(); }
    usize timeline_items() const override { return items_.size(); }
    std::vector<u64> pending_guidance() const override {
        std::vector<u64> ids;
        for (const auto& p : pending_)
            if (p.session == session_) ids.push_back(p.id);
        return ids;
    }
    u64 send_guidance(ViewContext& ctx, std::string text) override {
        if (session_.empty() || trim(text).empty()) return 0;
        const u64 id = ctx.services.commands->inject(session_, text);
        if (id == 0) {
            ctx.notify(Severity::error, "The guidance could not be queued: the session is not running in a live run.");
            return 0;
        }
        pending_.push_back(PendingGuidance{id, session_, std::move(text)});
        return id;
    }
    bool retract_guidance(ViewContext& ctx, u64 id) override {
        auto it = std::ranges::find(pending_, id, &PendingGuidance::id);
        if (it == pending_.end()) return false;
        if (!ctx.services.commands->retract(it->session, id)) {
            ctx.notify(Severity::warning, "Too late to retract: the guidance was already sent with the next request.");
            return false;
        }
        pending_.erase(it);
        ctx.notify(Severity::info, "Guidance retracted.");
        return true;
    }

private:
    struct PendingGuidance {
        u64 id = 0;
        std::string session, text;
    };

    // ---- which session ----
    const events::SessionState* state(ViewContext& ctx) const { return ctx.snapshot && !session_.empty() ? ctx.snapshot->session(session_) : nullptr; }

    // Follows the shared selection: its session, or the selected function's latest session in the run.
    void resolve_session(ViewContext& ctx) {
        if (!ctx.snapshot) return;
        const auto& sel = ctx.selection;
        if (pinned_ && sel.function_va == pinned_selection_.function_va && sel.session == pinned_selection_.session) return;
        pinned_ = false;
        const usize sessions = ctx.snapshot->sessions.size();
        if (sel.function_va == resolved_va_ && sel.session == resolved_session_ && sessions == resolved_count_ &&
            ctx.snapshot->run_id == resolved_run_)
            return;
        resolved_va_ = sel.function_va;
        resolved_session_ = sel.session;
        resolved_count_ = sessions;
        resolved_run_ = ctx.snapshot->run_id;
        std::string wanted;
        const events::SessionState* named = sel.session.empty() ? nullptr : ctx.snapshot->session(sel.session);
        if (named && (!sel.function_va || named->va == *sel.function_va)) {
            wanted = named->id;
        } else if (sel.function_va) {
            const auto latest = vm::latest_sessions(*ctx.snapshot);
            if (auto it = latest.find(*sel.function_va); it != latest.end()) wanted = it->second->id;
        }
        if (!wanted.empty() && wanted != session_) show_session(ctx, wanted);
        if (wanted.empty() && sel.function_va && (!va_ || *va_ != *sel.function_va)) {
            show_session(ctx, {});
            va_ = sel.function_va;  // a function without a session in this run
        }
    }

    void show_session(ViewContext& ctx, const std::string& id) {
        if (id == session_ && !id.empty()) return;
        session_ = id;
        va_.reset();
        reader_.reset();
        load_.cancel();
        load_.reset();
        transcript_error_.clear();
        path_.clear();
        final_read_ = false;
        items_.clear();
        items_version_ = ~u64{0};
        heights_ = {};
        heights_width_ = 0;
        sources_.clear();
        code_.clear();
        line_diffs_.clear();
        results_.clear();
        open_.clear();
        show_diff_.clear();
        history_.reset();
        history_job_.cancel();
        history_job_.reset();
        history_signal_ = ~u64{0};
        compare_a_.reset();
        compare_b_.reset();
        compare_result_a_.reset();
        compare_result_b_.reset();
        if (id.empty() || !ctx.snapshot) return;
        const events::SessionState* s = ctx.snapshot->session(id);
        if (!s) return;
        va_ = s->va;
        scroll_to_end_ = !s->finished;
        Workspace* ws = ctx.services.workspace;
        if (!ws || s->transcript.empty() || ws->run_dir().empty()) {
            transcript_error_ = s->transcript.empty() ? "This session recorded no transcript." : "The run's directory is not known.";
            return;
        }
        path_ = ws->run_dir() / fs::from_utf8(s->transcript);
        start_load(ctx);
    }

    // The first read may be megabytes: off the UI thread. Later reads take only what was appended.
    void start_load(ViewContext& ctx) {
        load_ = ctx.jobs.submit([path = path_, id = session_](const CancelToken&) {
            LoadedTranscript out;
            out.session = id;
            if (auto r = out.reader.feed_file(path); !r) out.error = r.error().message;
            return out;
        });
    }

    // ---- per frame ----
    void poll(ViewContext& ctx) {
        const events::SessionState* s = state(ctx);
        try {
            if (auto loaded = load_.take()) {
                load_.reset();
                last_poll_ = ImGui::GetTime();
                if (loaded->session == session_) {
                    if (loaded->error.empty()) reader_ = std::move(loaded->reader);
                    transcript_error_ = loaded->error;
                }
            }
            if (auto h = history_job_.take()) {
                history_job_.reset();
                if (h->va == va_) history_ = std::move(*h);
            }
            if (auto r = compile_a_.poll()) compare_result_a_ = std::move(*r);
            if (auto r = compile_b_.poll()) compare_result_b_ = std::move(*r);
        } catch (const std::exception& e) {
            ctx.notify(Severity::error, std::format("Agent session: {}", e.what()));
        }
        // A transcript that could not be read is tried again now and then while its session runs.
        if (!reader_ && !load_.valid() && !path_.empty() && s && !s->finished && ImGui::GetTime() - last_poll_ >= 1.0) start_load(ctx);
        // A running session's transcript grows: read what was appended, a few times a second, and once
        // more after it ended.
        if (reader_ && !path_.empty() && s) {
            const bool due = ImGui::GetTime() - last_poll_ >= kPollSeconds;
            if ((!s->finished && due) || (s->finished && !final_read_)) {
                last_poll_ = ImGui::GetTime();
                if (s->finished) final_read_ = true;
                if (auto r = reader_->feed_file(path_); !r) transcript_error_ = r.error().message;
                else transcript_error_.clear();
            }
        }
        // Guidance shows as pending until the transcript has it.
        if (reader_) std::erase_if(pending_, [&](const PendingGuidance& p) { return p.session == session_ && vm::guidance_sent(reader_->doc(), p.id); });
        // The function's attempts and notes, again whenever the session compiled something new.
        if (va_) {
            const u64 signal = s ? static_cast<u64>(s->compiles) + s->scores.size() + (s->finished ? 1 : 0) : 0;
            if ((signal != history_signal_ || reload_history_) && !history_job_.valid()) {
                history_signal_ = signal;
                reload_history_ = false;
                load_history(ctx);
            }
        }
        rebuild_timeline(ctx, s);
    }

    void load_history(ViewContext& ctx) {
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        auto program = ws ? ws->program() : nullptr;
        if (!project || !program || !va_) return;
        history_job_ = ctx.jobs.submit([project = *project, program, va = *va_] {
            FunctionHistory h;
            h.va = va;
            const Symbol fn = function_symbol(*program, va);
            h.attempts = vm::parse_attempts(project.attempts(fn));
            h.best_source = project.best_source(fn);
            h.notes = project.notes(fn);
            return h;
        });
    }

    bool live_buffer(const events::SessionState* s) const {
        if (!s || !reader_) return s && !s->finished && (!s->live_text.empty() || !s->live_thinking.empty());
        return vm::show_live(reader_->doc(), s->turn, s->finished, !s->live_text.empty() || !s->live_thinking.empty());
    }

    void rebuild_timeline(ViewContext& ctx, const events::SessionState* s) {
        static const vm::TranscriptDoc empty;
        const vm::TranscriptDoc& doc = reader_ ? reader_->doc() : empty;
        const bool live = live_buffer(s);
        const u64 version = reader_ ? reader_->version() : 0;
        if (version == items_version_ && live == items_live_) return;
        auto items = vm::build_timeline(doc, live);
        // Heights measured for the unchanged start of the timeline stay; the rest is estimated.
        usize same = 0;
        while (same < items.size() && same < items_.size() && items[same].kind == items_[same].kind && items[same].turn == items_[same].turn &&
               items[same].index == items_[same].index)
            ++same;
        const usize grew = items.size() > items_.size() ? items.size() - items_.size() : 0;
        items_ = std::move(items);
        items_version_ = version;
        items_live_ = live;
        sources_ = vm::transcript_sources(doc);
        if (code_.size() > sources_.size()) code_.clear();
        heights_.resize(same, 0.0f);
        heights_.resize(items_.size(), 0.0f);
        for (usize i = same; i < items_.size(); ++i) heights_.set(i, estimate(ctx, items_[i], doc, s));
        if (grew > 0 && follow_) scroll_to_end_ = scroll_to_end_ || at_bottom_;
    }

    // ---- estimates (before an item has been drawn once) ----
    float estimate(ViewContext& ctx, const vm::TimelineItem& item, const vm::TranscriptDoc& doc, const events::SessionState* s) const {
        const float line = ImGui::GetTextLineHeightWithSpacing();
        const float width = std::max(heights_width_, 200.0f);
        const float per_line = std::max(20.0f, width / std::max(1.0f, ImGui::GetFontSize() * 0.5f));
        auto text_lines = [&](std::string_view text) {
            float lines = 0;
            usize start = 0;
            while (start <= text.size()) {
                const usize nl = std::min(text.find('\n', start), text.size());
                lines += std::max(1.0f, std::ceil(static_cast<float>(nl - start) / per_line));
                start = nl + 1;
            }
            return lines;
        };
        const vm::TurnRecord* t = item.turn >= 0 && static_cast<usize>(item.turn) < doc.turns.size() ? &doc.turns[static_cast<usize>(item.turn)] : nullptr;
        switch (item.kind) {
        case Kind::turn: return line * 2.5f;
        case Kind::user:
            if (t && static_cast<usize>(item.index) < t->before.size()) {
                const auto& u = t->before[static_cast<usize>(item.index)];
                if (u.kind == vm::UserItem::Kind::tool_result) return line * (2 + static_cast<float>(std::min(count_lines(u.text), kResultLines)));
                return line * (1 + text_lines(u.text));
            }
            return line;
        case Kind::text:
            if (t && static_cast<usize>(item.index) < t->blocks.size()) return line * text_lines(t->blocks[static_cast<usize>(item.index)].text);
            return line;
        case Kind::thinking:
            if (t && open_.contains({item.turn, item.index}) && static_cast<usize>(item.index) < t->blocks.size())
                return line * (1 + text_lines(t->blocks[static_cast<usize>(item.index)].text));
            return line;
        case Kind::tool_call:
            if (t && static_cast<usize>(item.index) < t->blocks.size()) {
                const auto& b = t->blocks[static_cast<usize>(item.index)];
                if (item.source >= 0) {
                    const std::string_view src = vm::block_source(doc, sources_[static_cast<usize>(item.source)]);
                    ImGui::PushFont(ctx.fonts.mono, 0.0f);
                    const float code_line = ImGui::GetTextLineHeight();
                    ImGui::PopFont();
                    return line * 2 + code_line * static_cast<float>(count_lines(src));
                }
                return line * (1 + static_cast<float>(count_lines(dump_pretty(b.input))));
            }
            return line;
        case Kind::tool_result:
            if (t && static_cast<usize>(item.index) < t->tools.size()) {
                const auto& x = t->tools[static_cast<usize>(item.index)];
                const usize lines = std::min(count_lines(x.result), kResultLines);
                return line * (3 + static_cast<float>(lines));
            }
            return line * 2;
        case Kind::live:
            return s ? line * (2 + text_lines(s->live_thinking) + text_lines(s->live_text)) : line;
        case Kind::outcome: return line * 3;
        default: return line * 1.5f;
        }
    }

    // ---- drawing: header ----
    void draw_session_list(ViewContext& ctx) {
        if (!ctx.snapshot) {
            ImGui::TextDisabled("No run. Start one with Run > Start run (F5), or open a past run (Run > Runs...).");
            return;
        }
        if (va_) ImGui::TextDisabled("The selected function has no session in this run.");
        ImGui::TextDisabled("Pick a session (or a function in the Function browser, the Run monitor or the Logs):");
        if (list_seq_ != ctx.snapshot->last_seq || list_run_ != ctx.snapshot->run_id) {
            list_seq_ = ctx.snapshot->last_seq;
            list_run_ = ctx.snapshot->run_id;
            list_.clear();
            for (const auto& [id, s] : ctx.snapshot->sessions) list_.push_back(s.get());
            std::ranges::sort(list_, [](const events::SessionState* a, const events::SessionState* b) { return a->started > b->started; });
        }
        if (list_.empty()) {
            ImGui::TextDisabled("No session has started yet.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##sessions", 4, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Best");
        ImGui::TableSetupColumn("Turns");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(list_.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const events::SessionState& s = *list_[static_cast<usize>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable((s.display.empty() ? s.function : s.display).c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                    ctx.open(kViewId, NavTarget{.va = s.va, .session = s.id});
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.finished ? s.outcome.c_str() : s.phase.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", s.best_match);
                ImGui::TableNextColumn();
                ImGui::Text("%d", s.turn);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    void draw_header(ViewContext& ctx, const events::SessionState* s) {
        const ThemeColors& c = ctx.colors();
        RunCommands& commands = *ctx.services.commands;
        const vm::TranscriptDoc* doc = reader_ ? &reader_->doc() : nullptr;
        const vm::TranscriptHeader* h = doc && doc->header ? &*doc->header : nullptr;
        const std::string name = s ? (s->display.empty() ? s->function : s->display) : h ? (h->display.empty() ? h->function : h->display) : session_;
        if (ImGui::TextLink(std::format("{}##function", name).c_str()) && va_) ctx.open("inspector", NavTarget{.va = va_});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && va_)
            ImGui::SetTooltip("%s at %s\nOpen in the Inspector.", s ? s->function.c_str() : "", hex(*va_, 8).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", session_.c_str());
        if (s && ctx.snapshot) {
            // The function's other sessions in this run (after a requeue or a resume).
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
            if (ImGui::BeginCombo("##other_sessions", "Sessions", ImGuiComboFlags_HeightLarge)) {
                for (const auto& [id, other] : ctx.snapshot->sessions) {
                    if (other->va != s->va) continue;
                    if (ImGui::Selectable(std::format("{} ({})##{}", id, other->finished ? other->outcome : other->phase, id).c_str(), id == session_))
                        ctx.open(kViewId, NavTarget{.va = other->va, .session = id});
                }
                ImGui::EndCombo();
            }
        }

        // Status, models and budget use.
        if (s) {
            const ImVec4 color = !s->finished ? c.info : s->outcome == "matched" ? c.ok : s->outcome == "refused" || s->outcome == "error" ? c.error : c.warn;
            status_label(s->finished ? s->outcome : std::format("running: {}", s->phase.empty() ? "starting" : s->phase), color);
            ImGui::SameLine();
        }
        const std::string model = h && !h->model.empty() ? h->model : ctx.snapshot ? ctx.snapshot->model : std::string();
        const std::string effort = h && !h->effort.empty() ? h->effort : ctx.snapshot ? ctx.snapshot->effort : std::string();
        ImGui::Text("model %s, effort %s", model.c_str(), effort.c_str());
        // A fallback model served at least one turn: say which.
        std::set<std::string> served;
        if (doc)
            for (const auto& t : doc->turns)
                if (t.has_response && !t.model.empty() && (t.had_fallback || t.model != model)) served.insert(t.model);
        if (s && !s->model.empty() && s->model != model) served.insert(s->model);
        if (!served.empty() || (s && s->fallback_turns > 0)) {
            ImGui::SameLine();
            const std::string list = served.empty() ? std::string("a fallback model") : join(std::vector<std::string>(served.begin(), served.end()), ", ");
            colored_text(c.warn, std::format("[served by {}{}]", list, s && s->fallback_turns ? std::format(", {} fallback turn(s)", s->fallback_turns) : ""));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("A server-side fallback answered a declined request on another model; the timeline marks the switch points.");
        }
        if (s) draw_budget(ctx, *s);

        // Controls.
        const bool live = s && !s->finished && commands.live();
        ImGui::BeginDisabled(!live);
        const bool paused = live && ctx.snapshot && ctx.snapshot->workers.contains(s->worker) && ctx.snapshot->workers.at(s->worker).phase == "paused";
        if (ImGui::Button(paused ? "Resume worker" : "Pause worker")) {
            if (paused) commands.resume_worker(s->worker);
            else commands.pause_worker(s->worker);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Pause this session's worker after its current turn (or let it go on).");
        ImGui::SameLine();
        if (ImGui::Button("End session")) commands.skip(*va_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("End the session after its current turn (outcome: skipped); its attempts and best source stay.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        Workspace* ws = ctx.services.workspace;
        const bool has_project = ws && ws->project();
        ImGui::BeginDisabled(!va_ || !has_project);
        if (ImGui::Button("Take over...")) ImGui::OpenPopup("##take_over");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Edit this function by hand in the Diff viewer, starting from its best attempt.");
        if (ImGui::BeginPopup("##take_over")) {
            if (ImGui::MenuItem("Pause the session and edit (you can hand back)", nullptr, false, live)) take_over(ctx, *va_, session_, TakeOver::pause);
            if (ImGui::MenuItem(live ? "End the session and edit" : "Edit by hand", nullptr, false, true)) take_over(ctx, *va_, session_, TakeOver::end);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!va_);
        if (ImGui::Button("Diff viewer")) ctx.open("diff_viewer", NavTarget{.va = va_, .session = session_});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (reader_) {
            const std::filesystem::path path = path_;
            const vm::TranscriptReader* reader = &*reader_;
            const std::array<ExportFormat, 2> formats = {
                ExportFormat{"Markdown", "md", [reader] { return vm::to_markdown(reader->doc()); }},
                ExportFormat{"JSONL (as recorded)", "jsonl", [path] { return fs::read_text(path).value_or(std::string()); }},
            };
            export_button(ctx, "##export_transcript", std::format("transcript-{}", session_), formats);
        }

        // The outcome and its reason.
        if (s && s->finished) {
            std::string line = std::format("Outcome: {}, best {:.1f}%, {} turn(s), {}", s->outcome, s->best_match, s->turn, vm::format_usd(s->cost_usd));
            if (!s->detail.empty()) line += " - " + s->detail;
            if (!s->refusal_category.empty()) line += std::format(" (refusal category: {})", s->refusal_category);
            wrapped_colored(s->matched ? c.ok : s->outcome == "refused" || s->outcome == "error" ? c.error : c.muted, line);
        }
    }

    void draw_budget(ViewContext& ctx, const events::SessionState& s) {
        const events::RunStateData& run = *ctx.snapshot;
        const Json limits = run.config.is_object() ? run.config.value("limits", Json::object()) : Json::object();
        const int max_turns = run.budget.function.turns > 0 ? run.budget.function.turns : static_cast<int>(json_int_or(limits, "max_turns", 0));
        const double max_usd = run.budget.function.usd > 0 ? run.budget.function.usd : json_number_or(limits, "max_usd", 0);
        const long long max_tokens = run.budget.function.tokens > 0 ? run.budget.function.tokens : json_int_or(limits, "max_tokens", 0);
        const long long max_seconds = run.budget.function.minutes > 0 ? run.budget.function.minutes * 60LL : json_int_or(limits, "max_seconds", 0);
        const auto end = s.finished ? s.ended : std::chrono::system_clock::now();
        const double elapsed = s.started == vm::TimePoint{} ? 0.0 : vm::seconds_between(s.started, end);
        std::string text = max_turns > 0 ? std::format("turn {}/{}", s.turn, max_turns) : std::format("turn {}", s.turn);
        text += max_usd > 0 ? std::format(" | {} of {}", vm::format_usd(s.cost_usd), vm::format_usd(max_usd)) : std::format(" | {}", vm::format_usd(s.cost_usd));
        text += max_tokens > 0 ? std::format(" | {} of {} tokens", thousands(s.usage.total()), thousands(max_tokens))
                               : std::format(" | {} tokens", thousands(s.usage.total()));
        text += max_seconds > 0 ? std::format(" | {} of {}", vm::format_duration(elapsed), vm::format_duration(static_cast<double>(max_seconds)))
                                : std::format(" | {}", vm::format_duration(elapsed));
        if (s.retries) text += std::format(" | {} retries", s.retries);
        text += std::format(" | worker {}", s.worker);
        const bool high = (max_usd > 0 && s.cost_usd >= 0.8 * max_usd) || (max_turns > 0 && s.turn >= max_turns * 4 / 5);
        if (high) colored_text(ctx.colors().warn, text);
        else ImGui::TextUnformatted(text.c_str());
    }

    // ---- drawing: timeline ----
    void draw_timeline_tab(ViewContext& ctx, const events::SessionState* s) {
        RunCommands& commands = *ctx.services.commands;
        const bool live = s && !s->finished && commands.live();
        ImGui::Checkbox("Follow", &follow_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Keep the newest output in view while the session runs.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Expand thinking")) {
            if (reader_)
                for (const auto& item : items_)
                    if (item.kind == Kind::thinking) open_.insert({item.turn, item.index});
            heights_width_ = 0;  // re-estimate
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Collapse")) {
            open_.clear();
            heights_width_ = 0;
        }
        ImGui::SameLine();
        if (!reader_ && load_.valid()) ImGui::TextDisabled("Loading the transcript...");
        else if (!transcript_error_.empty()) colored_text(ctx.colors().warn, std::format("Transcript: {}", transcript_error_));
        else if (reader_) {
            const auto& doc = reader_->doc();
            ImGui::TextDisabled("%zu turn(s), %zu record(s)%s", doc.turns.size(), doc.records,
                                doc.malformed ? std::format(", {} unreadable line(s) skipped", doc.malformed).c_str() : "");
        }
        const float composer_h = live ? ImGui::GetTextLineHeightWithSpacing() * 3 + ImGui::GetFrameHeightWithSpacing() * 2 : ImGui::GetFrameHeightWithSpacing();
        const float pending_h = static_cast<float>(pending_guidance().size()) * ImGui::GetFrameHeightWithSpacing();
        const float timeline_h = std::max(ImGui::GetFrameHeight() * 4, ImGui::GetContentRegionAvail().y - composer_h - pending_h);
        draw_timeline(ctx, s, timeline_h);
        draw_composer(ctx, live);
    }

    void draw_timeline(ViewContext& ctx, const events::SessionState* s, float height) {
        if (!ImGui::BeginChild("##timeline", ImVec2(0, height), ImGuiChildFlags_Borders)) {
            ImGui::EndChild();
            return;
        }
        static const vm::TranscriptDoc empty;
        const vm::TranscriptDoc& doc = reader_ ? reader_->doc() : empty;
        if (items_.empty()) {
            ImGui::TextDisabled(s && !s->finished ? "Waiting for the first request..." : "Nothing recorded for this session.");
            ImGui::EndChild();
            return;
        }
        const float width = ImGui::GetContentRegionAvail().x;
        if (std::abs(width - heights_width_) > 1.0f) {
            // Wrapping changed: every height is an estimate again.
            heights_width_ = width;
            for (usize i = 0; i < items_.size(); ++i) heights_.set(i, estimate(ctx, items_[i], doc, s));
        }
        at_bottom_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        const float base = ImGui::GetCursorPosY();
        const float scroll = ImGui::GetScrollY();
        const float bottom = scroll + ImGui::GetWindowHeight();
        // Only the items in view are drawn; their measured heights replace the estimates.
        for (usize i = heights_.find(std::max(0.0f, scroll - base)); i < items_.size(); ++i) {
            const float top = heights_.offset(i);
            if (base + top > bottom) break;
            ImGui::SetCursorPosY(base + top);
            ImGui::PushID(static_cast<int>(i));
            draw_item(ctx, items_[i], doc, s);
            ImGui::PopID();
            heights_.set(i, std::max(1.0f, ImGui::GetCursorPosY() - (base + top)));
        }
        ImGui::SetCursorPosY(base + heights_.total());
        ImGui::Dummy(ImVec2(0, 0));
        if (scroll_to_end_ && follow_) ImGui::SetScrollHereY(1.0f);
        scroll_to_end_ = false;
        ImGui::EndChild();
    }

    void draw_item(ViewContext& ctx, const vm::TimelineItem& item, const vm::TranscriptDoc& doc, const events::SessionState* s) {
        const ThemeColors& c = ctx.colors();
        const vm::TurnRecord* t = item.turn >= 0 && static_cast<usize>(item.turn) < doc.turns.size() ? &doc.turns[static_cast<usize>(item.turn)] : nullptr;
        const Key key{item.turn, item.index};
        switch (item.kind) {
        case Kind::brief: {
            const bool open = open_.contains({-1, -1});
            ImGui::SetNextItemOpen(open);
            const bool now = ImGui::TreeNodeEx("##brief", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "Brief (%zu lines)",
                                               count_lines(doc.brief));
            toggle({-1, -1}, open, now);
            if (now) {
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                ImGui::TextUnformatted(doc.brief.data(), doc.brief.data() + doc.brief.size());
                ImGui::PopFont();
            }
            break;
        }
        case Kind::turn: draw_turn_header(ctx, *t, doc); break;
        case Kind::user: draw_user_item(ctx, t->before[static_cast<usize>(item.index)]); break;
        case Kind::thinking: {
            const auto& b = t->blocks[static_cast<usize>(item.index)];
            if (b.text.empty()) {
                ImGui::TextDisabled("thinking (no summary)");
                break;
            }
            const bool open = open_.contains(key);
            ImGui::SetNextItemOpen(open);
            const bool now = ImGui::TreeNodeEx("##thinking", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                               "Thinking summary (%zu characters)", b.text.size());
            toggle(key, open, now);
            if (now) wrapped_colored(c.muted, b.text);
            break;
        }
        case Kind::redacted: ImGui::TextDisabled("thinking (redacted)"); break;
        case Kind::text: wrapped(t->blocks[static_cast<usize>(item.index)].text); break;
        case Kind::tool_call: draw_tool_call(ctx, item, *t, doc); break;
        case Kind::tool_result: draw_tool_result(ctx, item, t->tools[static_cast<usize>(item.index)]); break;
        case Kind::fallback: {
            const auto& b = t->blocks[static_cast<usize>(item.index)];
            colored_text(c.warn, std::format(">> fallback: {} -> {} (a declined request continued on another model here)", b.from_model.empty() ? "?" : b.from_model,
                                             b.to_model.empty() ? "?" : b.to_model));
            break;
        }
        case Kind::other: ImGui::TextDisabled("[%s block]", t->blocks[static_cast<usize>(item.index)].text.c_str()); break;
        case Kind::retry: {
            const auto& r = t->retries[static_cast<usize>(item.index)];
            wrapped_colored(c.warn, std::format("Retry {} after {}{}: {}", r.attempt, seconds(r.delay_ms), r.status ? std::format(" (HTTP {})", r.status) : "", r.error));
            break;
        }
        case Kind::no_response:
            if (s && !s->finished && t->turn == s->turn) ImGui::TextDisabled("Waiting for the response...");
            else ImGui::TextDisabled("No response recorded (the request failed or was cut off).");
            break;
        case Kind::live: draw_live(ctx, s); break;
        case Kind::pending: {
            const auto& p = doc.pending[static_cast<usize>(item.index)];
            if (p.kind == vm::UserItem::Kind::guidance) wrapped_colored(c.accent, "Guidance on its way (sent with the next request): " + p.text);
            else draw_user_item(ctx, p);
            break;
        }
        case Kind::auto_submit: {
            const auto& a = *doc.auto_submit;
            wrapped_colored(a.accepted ? c.ok : c.warn, std::format("The byte-exact attempt the session did not submit was {}: {}",
                                                                    a.accepted ? "verified and saved" : "not accepted", a.result));
            break;
        }
        case Kind::outcome: {
            const auto& o = *doc.outcome;
            ImGui::SeparatorText("Outcome");
            const ImVec4 color = o.outcome == "matched" ? c.ok : o.outcome == "refused" || o.outcome == "error" ? c.error : c.warn;
            colored_text(color, std::format("{}: best {:.1f}%, {} turn(s), {}", o.outcome, o.best_match, o.turns, vm::format_usd(o.cost_usd)));
            if (!o.detail.empty()) wrapped(o.detail);
            if (s && !s->refusal_category.empty()) wrapped_colored(c.error, std::format("Refusal category: {}", s->refusal_category));
            break;
        }
        }
    }

    void toggle(const Key& key, bool was, bool now) {
        if (was == now) return;
        if (now) open_.insert(key);
        else open_.erase(key);
    }

    void draw_turn_header(ViewContext& ctx, const vm::TurnRecord& t, const vm::TranscriptDoc& doc) {
        const ThemeColors& c = ctx.colors();
        ImGui::SeparatorText(std::format("Turn {}", t.turn).c_str());
        if (t.has_request) {
            ImGui::TextDisabled("%s", local_clock(t.request_time).c_str());
            ImGui::SameLine();
        }
        if (!t.has_response) {
            ImGui::TextDisabled("request sent");
            return;
        }
        const std::string configured = doc.header ? doc.header->model : std::string();
        if (!t.model.empty() && (t.had_fallback || (!configured.empty() && t.model != configured))) {
            colored_text(c.warn, std::format("[served by {}{}]", t.model, t.had_fallback ? ", fallback" : ""));
            ImGui::SameLine();
        }
        const auto& u = t.usage;
        ImGui::TextDisabled("stop: %s | in %s, out %s, cache write %s, cache read %s | %s | first token %s, total %s%s",
                            t.stop_reason.empty() ? "?" : t.stop_reason.c_str(), thousands(u.input).c_str(), thousands(u.output).c_str(),
                            thousands(u.cache_write).c_str(), thousands(u.cache_read).c_str(), vm::format_usd(t.cost_usd).c_str(), seconds(t.ttft_ms).c_str(),
                            seconds(t.latency_ms).c_str(), t.retries.empty() ? "" : std::format(" | {} retries", t.retries.size()).c_str());
    }

    void draw_user_item(ViewContext& ctx, const vm::UserItem& u) {
        const ThemeColors& c = ctx.colors();
        switch (u.kind) {
        case vm::UserItem::Kind::guidance:
            colored_text(c.accent, std::format("Supervisor guidance{}{}:", u.time ? " at " + local_clock(*u.time) : "",
                                               u.guidance_id ? std::format(" (#{})", u.guidance_id) : ""));
            wrapped(u.text);
            break;
        case vm::UserItem::Kind::status: wrapped_colored(c.muted, u.text); break;
        case vm::UserItem::Kind::nudge: wrapped_colored(c.warn, "Reminder: " + u.text); break;
        case vm::UserItem::Kind::text: wrapped(u.text); break;
        case vm::UserItem::Kind::tool_result:
            ImGui::TextDisabled("Tool result for %s%s:", u.tool_use_id.c_str(), u.is_error ? " (error)" : "");
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            ImGui::TextUnformatted(std::string(head_lines(u.text, kResultLines)).c_str());
            ImGui::PopFont();
            break;
        case vm::UserItem::Kind::paused: colored_text(c.warn, std::format("Paused{}", u.time ? " at " + local_clock(*u.time) : "")); break;
        case vm::UserItem::Kind::resumed: colored_text(c.ok, std::format("Resumed{}", u.time ? " at " + local_clock(*u.time) : "")); break;
        }
    }

    void draw_tool_call(ViewContext& ctx, const vm::TimelineItem& item, const vm::TurnRecord& t, const vm::TranscriptDoc& doc) {
        const ThemeColors& c = ctx.colors();
        const auto& b = t.blocks[static_cast<usize>(item.index)];
        colored_text(c.accent, std::format("Tool call: {}", b.tool_name));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", b.tool_id.c_str());
        if (item.source < 0 || static_cast<usize>(item.source) >= sources_.size()) {
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            const std::string json = b.input.is_string() ? b.input.get<std::string>() : dump_pretty(b.input.is_null() ? Json::object() : b.input);
            wrapped(json);
            ImGui::PopFont();
            return;
        }
        const usize k = static_cast<usize>(item.source);
        const std::string_view source = vm::block_source(doc, sources_[k]);
        ImGui::SameLine();
        ImGui::TextDisabled("candidate %zu", k + 1);
        ImGui::SameLine();
        bool diff = show_diff_.contains(item.source);
        ImGui::BeginDisabled(k == 0);
        if (ImGui::Checkbox("Diff against the previous attempt", &diff)) {
            if (diff) show_diff_.insert(item.source);
            else show_diff_.erase(item.source);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(std::string(source).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Diff viewer")) open_attempt(ctx, source);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open this attempt in the Diff viewer.");
        // The call's other fields (submit_result's outcome and reason).
        if (b.input.is_object() && b.input.size() > 1) {
            Json rest = b.input;
            rest.erase("source");
            ImGui::TextDisabled("%s", dump_compact(rest).c_str());
        }
        if (diff && k > 0) {
            auto it = line_diffs_.find(item.source);
            if (it == line_diffs_.end())
                it = line_diffs_.emplace(item.source, vm::diff_lines(vm::block_source(doc, sources_[k - 1]), source)).first;
            draw_line_diff(ctx, it->second, 3);
            return;
        }
        if (code_.size() <= k) code_.resize(k + 1);
        if (code_[k].empty() && !source.empty()) code_[k] = vm::highlight_cpp(source);
        draw_code(ctx, source, code_[k], true);
    }

    void draw_tool_result(ViewContext& ctx, const vm::TimelineItem& item, const vm::ToolExchange& x) {
        const ThemeColors& c = ctx.colors();
        const Key key{item.turn, item.index};
        auto it = results_.find(key);
        if (it == results_.end()) it = results_.emplace(key, vm::summarize_tool_result(x.result)).first;
        const vm::ToolResultView& v = it->second;
        ImGui::TextDisabled("Result of %s (%s)%s", x.name.c_str(), seconds(x.elapsed_ms).c_str(), x.is_error ? ", error" : "");
        if (v.compile) {
            if (!v.compiled) {
                colored_text(c.error, v.headline);
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                for (const auto& e : v.errors) wrapped_colored(c.error, e);
                ImGui::PopFont();
            } else {
                const bool exact = v.headline.find("byte-exact") != std::string::npos;
                colored_text(exact ? c.ok : c.text, std::format("compile ok{}: {}", v.cached ? " (cached)" : "", v.headline));
            }
            if (!v.attempt.empty()) ImGui::TextDisabled("%s", v.attempt.c_str());
        } else if (x.is_error) {
            colored_text(c.error, v.headline);
        }
        const usize lines = count_lines(x.result);
        const bool long_result = lines > kResultLines || v.compile;
        const bool open = open_.contains(key);
        if (long_result) {
            ImGui::SetNextItemOpen(open);
            const std::string label = v.compile ? std::format("Full result ({} lines)", lines) : std::format("All {} lines", lines);
            const bool now = ImGui::TreeNodeEx("##full", ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", label.c_str());
            toggle(key, open, now);
            if (!now) {
                if (!v.compile) {
                    ImGui::PushFont(ctx.fonts.mono, 0.0f);
                    ImGui::TextUnformatted(std::string(head_lines(x.result, kResultLines)).c_str());
                    ImGui::PopFont();
                }
                return;
            }
        }
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextUnformatted(x.result.data(), x.result.data() + x.result.size());
        ImGui::PopFont();
    }

    void draw_live(ViewContext& ctx, const events::SessionState* s) {
        if (!s) return;
        const ThemeColors& c = ctx.colors();
        ImGui::SeparatorText(std::format("Turn {} (streaming)", s->turn).c_str());
        if (!s->live_thinking.empty()) {
            ImGui::TextDisabled("Thinking:");
            wrapped_colored(c.muted, s->live_thinking);
        }
        if (!s->live_text.empty()) wrapped(s->live_text + " _");
    }

    // Opens the attempts.jsonl entry of a source this session sent (or just the function).
    void open_attempt(ViewContext& ctx, std::string_view source) {
        if (history_)
            for (const auto& a : history_->attempts)
                if (a.session == session_ && a.source == source) {
                    ctx.open("diff_viewer", NavTarget{.va = va_, .session = session_, .anchor = std::format("attempt:{}", a.index)});
                    return;
                }
        ctx.open("diff_viewer", NavTarget{.va = va_, .session = session_});
    }

    void draw_composer(ViewContext& ctx, bool live) {
        // Pending guidance can be retracted until it is sent.
        for (const auto& p : std::vector<PendingGuidance>(pending_)) {
            if (p.session != session_) continue;
            ImGui::PushID(static_cast<int>(p.id));
            if (ImGui::SmallButton("Retract")) retract_guidance(ctx, p.id);
            ImGui::SameLine();
            colored_text(ctx.colors().accent, "pending: " + std::string(head_lines(p.text, 1)));
            ImGui::PopID();
        }
        if (!live) {
            ImGui::TextDisabled("Guidance can be sent to a session while it runs in a live run.");
            return;
        }
        ImGui::PushID("composer");
        // A multi-line field submits its ID twice (the field, then its scrolling child): intended, not a conflict.
        ImGui::PushItemFlag(ImGuiItemFlags_AllowDuplicateId, true);
        const bool send = ImGui::InputTextMultiline("##guidance", &composer_, ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 3),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemFlag();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Appended after the next tool results, prefixed [Supervisor guidance]. Ctrl+Enter sends.");
        ImGui::BeginDisabled(trim(composer_).empty());
        const bool clicked = ImGui::Button("Send guidance (Ctrl+Enter)");
        ImGui::EndDisabled();
        if ((send || clicked) && !trim(composer_).empty()) {
            if (send_guidance(ctx, composer_)) composer_.clear();
        }
        ImGui::PopID();
    }

    // ---- drawing: attempts ----
    void draw_attempts_tab(ViewContext& ctx, const events::SessionState* s) {
        if (!ImGui::BeginChild("##attempts_scroll", ImVec2(0, 0))) {
            ImGui::EndChild();
            return;
        }
        ImGui::SeparatorText("Score per attempt (this session)");
        if (s && !s->scores.empty()) {
            const vm::Series scores = vm::score_per_attempt(*s);
            const vm::Series best = vm::best_score_per_attempt(*s);
            if (ImPlot::BeginPlot("##scores", ImVec2(-1, ImGui::GetFontSize() * 11))) {
                ImPlot::SetupAxes("attempt", "match %", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 105, ImPlotCond_Always);
                const int n = static_cast<int>(scores.size());
                ImPlot::PlotBars("score", scores.x.data(), scores.y.data(), n, 0.6, {ImPlotProp_FillAlpha, 0.6f});
                ImPlot::PlotLine("best so far", best.x.data(), best.y.data(), n, {ImPlotProp_Marker, ImPlotMarker_Circle, ImPlotProp_LineWeight, 2.0f});
                ImPlot::EndPlot();
            }
        } else {
            ImGui::TextDisabled("No diff yet in this session.");
        }
        ImGui::SeparatorText("Source history (every attempt for this function)");
        if (!history_) {
            ImGui::TextDisabled(history_job_.valid() ? "Loading..." : "Open the run's project to see the function's attempts.");
            ImGui::EndChild();
            return;
        }
        const auto& attempts = history_->attempts;
        const auto best = vm::best_attempt(attempts, history_->best_source);
        if (attempts.empty()) ImGui::TextDisabled("No attempts recorded.");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_Resizable;
        const float table_h = std::min(ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(attempts.size() + 2), ImGui::GetFontSize() * 16);
        if (!attempts.empty() && ImGui::BeginTable("##history", 7, flags, ImVec2(0, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Session");
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Match");
            ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Compare");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(attempts.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const usize k = static_cast<usize>(i);
                    const auto& a = attempts[k];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", k + 1);
                    ImGui::TableNextColumn();
                    const bool here = a.session == session_;
                    if (here) colored_text(ctx.colors().accent, std::format("#{} this session", a.attempt));
                    else ImGui::TextDisabled("#%d %s%s", a.attempt, a.session.c_str(), a.origin == "user" ? " (by hand)" : "");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(a.time.c_str());
                    ImGui::TableNextColumn();
                    if (!a.compiled) colored_text(ctx.colors().error, "failed");
                    else if (a.byte_exact) colored_text(ctx.colors().ok, "100% exact");
                    else ImGui::Text("%.1f%%", a.match_percent);
                    if (best == k) {
                        ImGui::SameLine();
                        colored_text(ctx.colors().ok, "[best]");
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(a.summary.c_str());
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("A", compare_a_ == k)) choose_compare(ctx, compare_a_, compile_a_, compare_result_a_, k);
                    ImGui::SameLine();
                    if (ImGui::RadioButton("B", compare_b_ == k)) choose_compare(ctx, compare_b_, compile_b_, compare_result_b_, k);
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Diff viewer"))
                        ctx.open("diff_viewer", NavTarget{.va = va_, .session = a.session, .anchor = std::format("attempt:{}", a.index)});
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        draw_compare(ctx);
        ImGui::EndChild();
    }

    void choose_compare(ViewContext& ctx, std::optional<usize>& slot, LatestWins<CompiledSource>& job, std::optional<CompiledSource>& result, usize k) {
        slot = k;
        result.reset();
        Workspace* ws = ctx.services.workspace;
        if (!history_ || !ws || !ws->project() || !ws->program() || !va_) return;
        job.submit(ctx.jobs, [program = ws->program(), project = *ws->project(), va = *va_, source = history_->attempts[k].source](const CancelToken& t) {
            return compile_source(program, project, va, source, t);
        });
    }

    void draw_compare(ViewContext& ctx) {
        ImGui::SeparatorText("Compare two attempts");
        if (!history_ || !compare_a_ || !compare_b_ || *compare_a_ >= history_->attempts.size() || *compare_b_ >= history_->attempts.size()) {
            ImGui::TextDisabled("Choose attempts A and B in the history.");
            return;
        }
        const auto& a = history_->attempts[*compare_a_];
        const auto& b = history_->attempts[*compare_b_];
        ImGui::Text("A: attempt %zu (%.1f%%)  |  B: attempt %zu (%.1f%%)", *compare_a_ + 1, a.match_percent, *compare_b_ + 1, b.match_percent);
        if (compare_sources_ != std::pair{*compare_a_, *compare_b_}) {
            compare_sources_ = {*compare_a_, *compare_b_};
            compare_diff_ = vm::diff_lines(a.source, b.source);
        }
        if (ImGui::BeginTable("##compare", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Source (A to B)");
            ImGui::TableSetupColumn("Assembly (target | A | B)");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float h = ImGui::GetFontSize() * 22;
            if (ImGui::BeginChild("##source_diff", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) draw_line_diff(ctx, compare_diff_, 3);
            ImGui::EndChild();
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("##asm_diff", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) draw_paired(ctx);
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }

    // The two attempts' assembly diffs side by side, paired by target instruction.
    void draw_paired(ViewContext& ctx) {
        if (!compare_result_a_ || !compare_result_b_) {
            ImGui::TextDisabled(compile_a_.busy() || compile_b_.busy() ? "Compiling both attempts..." : "No diff yet.");
            return;
        }
        if (!compare_result_a_->diff || !compare_result_b_->diff) {
            ImGui::TextDisabled("A: %s", compare_result_a_->summary.c_str());
            ImGui::TextDisabled("B: %s", compare_result_b_->summary.c_str());
            return;
        }
        const auto& da = *compare_result_a_->diff;
        const auto& db = *compare_result_b_->diff;
        const std::pair<const void*, const void*> pair_key{compare_result_a_->diff.get(), compare_result_b_->diff.get()};
        if (paired_for_ != pair_key) {
            paired_for_ = pair_key;
            paired_ = vm::pair_attempt_rows(da, db);
        }
        ImGui::TextDisabled("A: %s", matching::summary_line(da).c_str());
        ImGui::TextDisabled("B: %s", matching::summary_line(db).c_str());
        const DiffPalette p = ctx.diff_palette();
        auto color = [&](matching::RowKind k) {
            switch (k) {
            case matching::RowKind::equal: return p.equal;
            case matching::RowKind::encoding: return p.encoding;
            case matching::RowKind::operand: return p.operand;
            case matching::RowKind::opcode: return p.opcode;
            case matching::RowKind::insert: return p.insert;
            case matching::RowKind::del: return p.del;
            }
            return p.equal;
        };
        if (!ImGui::BeginTable("##paired", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) return;
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("A");
        ImGui::TableSetupColumn("B");
        ImGui::TableHeadersRow();
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(paired_.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const vm::PairedRow& pr = paired_[static_cast<usize>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const matching::Row* ra = pr.a ? &da.rows[*pr.a] : nullptr;
                const matching::Row* rb = pr.b ? &db.rows[*pr.b] : nullptr;
                const matching::Row* rt = ra && ra->target ? ra : rb && rb->target ? rb : nullptr;
                if (rt) ImGui::TextUnformatted(vm::side_cell(rt == ra ? da : db, *rt, vm::DiffSide::target).text().c_str());
                for (auto [row, diff] : {std::pair{ra, &da}, std::pair{rb, &db}}) {
                    ImGui::TableNextColumn();
                    if (!row) continue;
                    const vm::SideCell cell = vm::side_cell(*diff, *row, vm::DiffSide::candidate);
                    const std::string text = std::format("{} {}", vm::row_glyph(*row), cell.present ? cell.text() : std::string("-"));
                    ImGui::PushStyleColor(ImGuiCol_Text, row->kind == matching::RowKind::equal ? ImGui::GetColorU32(ctx.colors().muted) : color(row->kind));
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopStyleColor();
                }
            }
        ImGui::PopFont();
        ImGui::EndTable();
    }

    void draw_notes_tab(ViewContext& ctx) {
        if (ImGui::SmallButton("Refresh")) reload_history_ = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Notes the agent recorded for this function (record_note), kept across sessions.");
        if (!history_) {
            ImGui::TextDisabled("Open the run's project to see the notes.");
            return;
        }
        if (history_->notes.empty()) {
            ImGui::TextDisabled("No notes.");
            return;
        }
        if (ImGui::BeginChild("##notes", ImVec2(0, 0), ImGuiChildFlags_Borders)) wrapped(history_->notes);
        ImGui::EndChild();
        (void)ctx;
    }

    // ---- actions ----
    bool visible() const { return drawn_frame_ >= 0 && ImGui::GetFrameCount() - drawn_frame_ <= 1; }

    void register_actions(ViewContext& ctx) {
        if (actions_registered_) return;
        actions_registered_ = true;
        ViewContext* c = &ctx;
        ctx.actions.add({.id = "session.export_markdown", .label = "Export transcript as Markdown", .category = "Agent session",
                         .enabled = [this, c] { return reader_.has_value() && c->project.open; }, .run = [this, c] { export_transcript(*c, true); }});
        ctx.actions.add({.id = "session.export_jsonl", .label = "Export transcript as recorded (JSONL)", .category = "Agent session",
                         .enabled = [this, c] { return reader_.has_value() && c->project.open; }, .run = [this, c] { export_transcript(*c, false); }});
        ctx.actions.add({.id = "session.take_over", .label = "Take over the session's function (manual mode)", .category = "Agent session",
                         .enabled = [this, c] { return va_.has_value() && c->services.workspace && c->services.workspace->project(); },
                         .run = [this, c] { take_over(*c, *va_, session_, TakeOver::pause); }});
        ctx.actions.add({.id = "session.end", .label = "End the shown session", .category = "Agent session",
                         .enabled = [this, c] {
                             const auto* s = state(*c);
                             return s && !s->finished && c->services.commands->live();
                         },
                         .run = [this, c] { c->services.commands->skip(*va_); }});
    }

    void export_transcript(ViewContext& ctx, bool markdown) {
        if (!reader_) return;
        std::string content = markdown ? vm::to_markdown(reader_->doc()) : fs::read_text(path_).value_or(std::string());
        if (auto written = write_export(ctx.project.root, std::format("transcript-{}", session_), markdown ? "md" : "jsonl", content))
            ctx.notify(Severity::info, std::format("Exported to {}", fs::to_utf8(*written)));
        else
            ctx.notify(Severity::error, std::format("Cannot export: {}", written.error().message));
    }

    // Session.
    std::string session_;
    std::optional<u64> va_;
    bool pinned_ = false;  // navigated to session_; kept until the selection changes
    Selection pinned_selection_;
    std::optional<u64> resolved_va_;
    std::string resolved_session_, resolved_run_;
    usize resolved_count_ = 0;
    std::vector<const events::SessionState*> list_;  // the session picker, newest first
    u64 list_seq_ = ~u64{0};
    std::string list_run_;

    // Transcript.
    std::optional<vm::TranscriptReader> reader_;
    JobHandle<LoadedTranscript> load_;
    std::filesystem::path path_;
    std::string transcript_error_;
    double last_poll_ = 0;
    bool final_read_ = false;

    // Timeline.
    std::vector<vm::TimelineItem> items_;
    u64 items_version_ = ~u64{0};
    bool items_live_ = false;
    vm::ItemHeights heights_;
    float heights_width_ = 0;
    std::vector<vm::SourceRef> sources_;
    std::vector<std::vector<vm::CodeLine>> code_;  // highlighted sources, by source index
    std::map<i32, vm::LineDiff> line_diffs_;       // diffs against the previous source, by source index
    std::map<Key, vm::ToolResultView> results_;
    std::set<Key> open_;    // expanded thinking summaries and results; {-1, -1}: the brief
    std::set<i32> show_diff_;
    bool follow_ = true;
    bool at_bottom_ = true;
    bool scroll_to_end_ = false;

    // History and comparison.
    JobHandle<FunctionHistory> history_job_;
    std::optional<FunctionHistory> history_;
    u64 history_signal_ = ~u64{0};
    bool reload_history_ = false;
    std::optional<usize> compare_a_, compare_b_;
    LatestWins<CompiledSource> compile_a_, compile_b_;
    std::optional<CompiledSource> compare_result_a_, compare_result_b_;
    std::pair<usize, usize> compare_sources_{~usize{0}, ~usize{0}};
    vm::LineDiff compare_diff_;
    std::pair<const void*, const void*> paired_for_{nullptr, nullptr};
    std::vector<vm::PairedRow> paired_;

    // Guidance.
    std::string composer_;
    std::vector<PendingGuidance> pending_;

    bool actions_registered_ = false;
    int drawn_frame_ = -1;
};

} // namespace

std::unique_ptr<View> make_agent_session_view() { return std::make_unique<AgentSessionView>(); }

} // namespace decomp::gui
