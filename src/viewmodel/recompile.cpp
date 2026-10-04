#include "viewmodel/recompile.hpp"

namespace decomp::vm {

void RecompileSchedule::edited(TimePoint now) {
    ++latest_;
    last_edit_ = now;
}

bool RecompileSchedule::due(TimePoint now) const {
    auto at = due_at();
    return at && now >= *at;
}

std::optional<TimePoint> RecompileSchedule::due_at() const {
    // Nothing new since the last compile started (or since what is shown).
    if (latest_ == shown_ || latest_ == started_) return std::nullopt;
    return last_edit_ + quiet_;
}

u64 RecompileSchedule::start() {
    started_ = latest_;
    compiling_ = latest_;
    return latest_;
}

bool RecompileSchedule::finished(u64 generation) {
    if (generation == compiling_) compiling_ = 0;
    if (generation != started_ || generation <= shown_) return false;
    shown_ = generation;
    return true;
}

void RecompileSchedule::reset() {
    ++latest_;
    shown_ = latest_;
    started_ = latest_;
    compiling_ = 0;
}

} // namespace decomp::vm
