#include "gui/navigation.hpp"

#include <utility>

namespace decomp::gui {

void Navigation::open(std::string view, NavTarget target) {
    NavEntry entry{std::move(view), std::move(target)};
    if (entries_.empty() || entries_[index_] != entry) {
        if (!entries_.empty()) entries_.resize(index_ + 1);  // a new branch drops the forward history
        entries_.push_back(entry);
        if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin());
        index_ = entries_.size() - 1;
    }
    request_ = std::move(entry);
}

bool Navigation::back() {
    if (!can_back()) return false;
    --index_;
    request_ = entries_[index_];
    return true;
}

bool Navigation::forward() {
    if (!can_forward()) return false;
    ++index_;
    request_ = entries_[index_];
    return true;
}

std::optional<NavEntry> Navigation::take_request() { return std::exchange(request_, std::nullopt); }

} // namespace decomp::gui
