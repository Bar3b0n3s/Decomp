#pragma once

// The functions of a run and their dispatch order. Not thread-safe: the run controller guards it.

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::run {

enum class ItemState {
    pending,  // waiting for a worker
    running,
    done,     // a session finished it (whatever the outcome)
    skipped,  // set aside by the supervisor
};

std::string_view to_string(ItemState state);
std::optional<ItemState> parse_item_state(std::string_view text);

struct QueueItem {
    u64 va = 0;
    std::string name = {};     // symbol name
    std::string display = {};  // readable name
    double difficulty = 0;
    bool pinned = false;
    ItemState state = ItemState::pending;
    int sessions = 0;          // sessions started for it in this run
    std::string outcome = {};  // the last session's outcome
    int worker = -1;           // while running

    Json to_json() const;
    static Result<QueueItem> from_json(const Json& j);
};

// Rough effort estimate from the function's size (log2 of its bytes): easy functions go first.
double estimate_difficulty(const Symbol& fn);

// A run's items. Pending items are dispatched pinned first, then in list order.
class WorkQueue {
public:
    WorkQueue() = default;
    explicit WorkQueue(std::vector<QueueItem> items) : items_(std::move(items)) {}

    // Appends items not already in the queue; returns how many were added.
    usize enqueue(std::vector<QueueItem> items);
    // The next pending item (marked running on `worker`), or nothing.
    std::optional<QueueItem> next(int worker);
    // A running item's session ended: done with `outcome`.
    void finish(u64 va, const std::string& outcome);
    // A pending item is set aside (a running one is set aside when its session ends).
    bool skip(u64 va);
    // A finished or skipped item goes back to pending (at the end of the list).
    bool requeue(u64 va);
    bool remove(u64 va);   // pending items only
    bool pin(u64 va, bool pinned);
    // Moves a pending item to `index` among the pending items in dispatch order.
    bool move(u64 va, usize index);

    const QueueItem* find(u64 va) const;
    QueueItem* find(u64 va);
    const std::vector<QueueItem>& items() const { return items_; }
    // Pending items in dispatch order.
    std::vector<const QueueItem*> pending() const;
    usize pending_count() const;
    usize running_count() const;
    usize count(ItemState state) const;

private:
    std::vector<QueueItem> items_;
};

} // namespace decomp::run
