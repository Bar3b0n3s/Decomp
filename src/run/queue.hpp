#pragma once

// The functions of a run and their dispatch order. Not thread-safe: the run controller guards it.

#include "analysis/difficulty.hpp"
#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <optional>
#include <span>
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

// A function's difficulty score for the queue: difficulty() of its code's features from `analysis` when
// that covers it, else size_difficulty() of its symbol (nothing is decoded).
double queue_difficulty(const Program& program, u64 va, const FunctionAnalysis* analysis);
// The items of a new run over `vas`, each with its name and queue_difficulty(). With `easy_first`, the
// easy functions go first (a stable sort, so equal scores keep the order of `vas`); otherwise they keep
// the order of `vas`.
std::vector<QueueItem> make_queue_items(const Program& program, std::span<const u64> vas, const FunctionAnalysis* analysis, bool easy_first);

// Outcomes after which a resumed run does not work on the function again (matched, gave up, refused,
// out of its budget or turns, no result, skipped). Stopped, aborted and failed sessions run again.
bool is_final_outcome(std::string_view outcome);

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
