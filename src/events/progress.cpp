#include "events/progress.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <format>

namespace decomp::events {

namespace {

std::string duration_text(std::chrono::system_clock::duration d) {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    if (s < 0) s = 0;
    return std::format("{:02}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
}

std::string clip(std::string s, usize width) {
    if (s.size() > width) {
        s.resize(width > 1 ? width - 1 : width);
        s += "~";
    }
    return s;
}

} // namespace

std::string ProgressRenderer::render_block(const RunStateData& st, std::chrono::system_clock::time_point now, usize activity_lines) {
    std::string out;
    auto end = st.status == "running" || st.status.empty() ? now : st.ended;
    out += std::format("run {}  {}  model {} ({})  elapsed {}  spend ${:.2f}  cache {:.0f}%  matched {}/{}\n", st.run_id,
                       st.status.empty() ? "starting" : st.status, st.model, st.effort, duration_text(end - st.started), st.cost_usd,
                       100 * st.cache_hit_rate(), st.matched, st.planned_count());
    if (st.queue_total > 0) out += std::format("  queue {} function(s)\n", st.queue_total);
    // At most kMaxWorkerLines worker lines, so the block stays small with many workers.
    constexpr usize kMaxWorkerLines = 12;
    usize shown = 0, idle = 0;
    for (const auto& [id, w] : st.workers) {
        const SessionState* s = w.session.empty() ? nullptr : st.session(w.session);
        if (!s) {
            ++idle;
            continue;
        }
        if (shown++ >= kMaxWorkerLines) continue;
        out += std::format("  worker {}  {:<28} turn {:>2}  {:<18} best {:5.1f}%  ${:.2f}  {}\n", id,
                           clip(s->display.empty() ? s->function : s->display, 28), s->turn, clip(w.phase, 18), s->best_match,
                           s->cost_usd, duration_text(now - s->started));
    }
    if (shown > kMaxWorkerLines) out += std::format("  ... {} more busy worker(s)\n", shown - kMaxWorkerLines);
    if (idle) out += std::format("  {} idle worker(s)\n", idle);
    usize start = st.activity.size() > activity_lines ? st.activity.size() - activity_lines : 0;
    for (usize i = start; i < st.activity.size(); ++i) out += "  " + clip(st.activity[i], 150) + "\n";
    return out;
}

void ProgressRenderer::attach(EventBus& bus) {
    subscription_ = bus.subscribe([this](const Event& e) { on_event(e); });
}

void ProgressRenderer::detach(EventBus& bus) { bus.unsubscribe(subscription_); }

void ProgressRenderer::on_event(const Event& e) {
    std::lock_guard lock(mutex_);
    state_.apply(e);
    if (!tty_) {
        // Print the activity lines this event added.
        const auto& st = state_.data();
        u64 fresh = std::min<u64>(st.activity_total - printed_activity_, st.activity.size());
        for (usize i = st.activity.size() - static_cast<usize>(fresh); i < st.activity.size(); ++i)
            std::fprintf(out_, "%s\n", st.activity[i].c_str());
        printed_activity_ = st.activity_total;
        std::fflush(out_);
        return;
    }
    bool important = !std::holds_alternative<StreamDelta>(e.payload);
    redraw(important);
}

void ProgressRenderer::redraw(bool force) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now - last_draw_ < std::chrono::milliseconds(100)) return;
    last_draw_ = now;
    std::string block = render_block(state_.data(), std::chrono::system_clock::now());
    std::string out;
    if (drawn_lines_ > 0) out += std::format("\x1b[{}F\x1b[J", drawn_lines_);
    out += block;
    drawn_lines_ = static_cast<int>(std::count(block.begin(), block.end(), '\n'));
    std::fwrite(out.data(), 1, out.size(), out_);
    std::fflush(out_);
}

void ProgressRenderer::finish() {
    std::lock_guard lock(mutex_);
    if (tty_) redraw(true);
}

} // namespace decomp::events
