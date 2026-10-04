#include "run/queue.hpp"

#include <algorithm>
#include <cmath>

namespace decomp::run {

std::string_view to_string(ItemState state) {
    switch (state) {
    case ItemState::pending: return "pending";
    case ItemState::running: return "running";
    case ItemState::done: return "done";
    case ItemState::skipped: return "skipped";
    }
    return "pending";
}

std::optional<ItemState> parse_item_state(std::string_view text) {
    if (text == "pending") return ItemState::pending;
    if (text == "running") return ItemState::running;
    if (text == "done") return ItemState::done;
    if (text == "skipped") return ItemState::skipped;
    return std::nullopt;
}

Json QueueItem::to_json() const {
    Json j = {{"va", va}, {"name", name}, {"display", display}, {"difficulty", difficulty}, {"state", to_string(state)}, {"sessions", sessions}};
    if (pinned) j["pinned"] = true;
    if (!outcome.empty()) j["outcome"] = outcome;
    return j;
}

Result<QueueItem> QueueItem::from_json(const Json& j) {
    if (!j.is_object() || !j.contains("va") || !j["va"].is_number_unsigned())
        return make_error(ErrorCode::parse, "queue item without an address: {}", dump_compact(j));
    QueueItem item;
    item.va = j["va"].get<u64>();
    item.name = json_string_or(j, "name", "");
    item.display = json_string_or(j, "display", item.name);
    item.difficulty = json_number_or(j, "difficulty", 0);
    item.pinned = json_bool_or(j, "pinned", false);
    item.state = parse_item_state(json_string_or(j, "state", "pending")).value_or(ItemState::pending);
    item.sessions = static_cast<int>(json_int_or(j, "sessions", 0));
    item.outcome = json_string_or(j, "outcome", "");
    return item;
}

double estimate_difficulty(const Symbol& fn) { return std::log2(1.0 + static_cast<double>(fn.size)); }

usize WorkQueue::enqueue(std::vector<QueueItem> items) {
    usize added = 0;
    for (auto& item : items) {
        if (find(item.va)) continue;
        item.state = ItemState::pending;
        item.worker = -1;
        items_.push_back(std::move(item));
        ++added;
    }
    return added;
}

std::vector<const QueueItem*> WorkQueue::pending() const {
    std::vector<const QueueItem*> out;
    for (const auto& item : items_)
        if (item.state == ItemState::pending && item.pinned) out.push_back(&item);
    for (const auto& item : items_)
        if (item.state == ItemState::pending && !item.pinned) out.push_back(&item);
    return out;
}

std::optional<QueueItem> WorkQueue::next(int worker) {
    auto order = pending();
    if (order.empty()) return std::nullopt;
    QueueItem* item = find(order.front()->va);
    item->state = ItemState::running;
    item->worker = worker;
    ++item->sessions;
    return *item;
}

void WorkQueue::finish(u64 va, const std::string& outcome) {
    QueueItem* item = find(va);
    if (!item) return;
    item->outcome = outcome;
    item->worker = -1;
    // A skip requested while the session ran keeps the item set aside.
    item->state = outcome == "skipped" ? ItemState::skipped : ItemState::done;
}

bool WorkQueue::skip(u64 va) {
    QueueItem* item = find(va);
    if (!item || item->state != ItemState::pending) return false;
    item->state = ItemState::skipped;
    item->outcome = "skipped";
    return true;
}

bool WorkQueue::requeue(u64 va) {
    auto it = std::ranges::find(items_, va, &QueueItem::va);
    if (it == items_.end() || (it->state != ItemState::done && it->state != ItemState::skipped)) return false;
    QueueItem item = std::move(*it);
    items_.erase(it);
    item.state = ItemState::pending;
    items_.push_back(std::move(item));
    return true;
}

bool WorkQueue::remove(u64 va) {
    auto it = std::ranges::find(items_, va, &QueueItem::va);
    if (it == items_.end() || it->state != ItemState::pending) return false;
    items_.erase(it);
    return true;
}

bool WorkQueue::pin(u64 va, bool pinned) {
    QueueItem* item = find(va);
    if (!item) return false;
    item->pinned = pinned;
    return true;
}

bool WorkQueue::move(u64 va, usize index) {
    auto order = pending();
    auto from = std::ranges::find(order, va, &QueueItem::va);
    if (from == order.end()) return false;
    const QueueItem* moving = *from;
    order.erase(from);
    index = std::min(index, order.size());
    // Pinned items stay ahead of unpinned ones: moving across that boundary changes the pin (at the
    // boundary itself either state gives the same position).
    const usize pinned_count = static_cast<usize>(std::ranges::count_if(order, &QueueItem::pinned));
    bool pinned = moving->pinned;
    if (index < pinned_count) pinned = true;
    else if (index > pinned_count) pinned = false;
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(index), moving);

    // Rebuild the list: non-pending items keep their places, pending items follow the new order.
    std::vector<u64> pending_order;
    for (const QueueItem* p : order) pending_order.push_back(p->va);
    std::vector<QueueItem> rebuilt;
    rebuilt.reserve(items_.size());
    std::vector<QueueItem> pending_items;
    for (auto& item : items_) (item.state == ItemState::pending ? pending_items : rebuilt).push_back(std::move(item));
    for (u64 p : pending_order) {
        auto it = std::ranges::find(pending_items, p, &QueueItem::va);
        rebuilt.push_back(std::move(*it));
    }
    items_ = std::move(rebuilt);
    find(va)->pinned = pinned;
    return true;
}

const QueueItem* WorkQueue::find(u64 va) const {
    auto it = std::ranges::find(items_, va, &QueueItem::va);
    return it == items_.end() ? nullptr : &*it;
}

QueueItem* WorkQueue::find(u64 va) {
    auto it = std::ranges::find(items_, va, &QueueItem::va);
    return it == items_.end() ? nullptr : &*it;
}

usize WorkQueue::pending_count() const { return count(ItemState::pending); }
usize WorkQueue::running_count() const { return count(ItemState::running); }

usize WorkQueue::count(ItemState state) const {
    return static_cast<usize>(std::ranges::count(items_, state, &QueueItem::state));
}

} // namespace decomp::run
