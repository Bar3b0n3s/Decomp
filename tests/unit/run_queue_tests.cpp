#include "analysis/difficulty.hpp"
#include "run/queue.hpp"
#include "run/selection.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::run;

namespace {

WorkQueue make_queue(int n) {
    std::vector<QueueItem> items;
    for (int i = 0; i < n; ++i) items.push_back(QueueItem{.va = 0x1000u + static_cast<u64>(i), .name = "f" + std::to_string(i)});
    return WorkQueue(std::move(items));
}

std::vector<u64> pending_vas(const WorkQueue& q) {
    std::vector<u64> out;
    for (const auto* item : q.pending()) out.push_back(item->va);
    return out;
}

} // namespace

TEST_CASE("work queue: dispatch order, pins, moves, skip, requeue, remove") {
    WorkQueue q = make_queue(5);  // 0x1000..0x1004
    CHECK(pending_vas(q) == std::vector<u64>{0x1000, 0x1001, 0x1002, 0x1003, 0x1004});

    CHECK(q.pin(0x1003, true));
    CHECK(pending_vas(q) == std::vector<u64>{0x1003, 0x1000, 0x1001, 0x1002, 0x1004});
    // Moving to the front of the pinned group pins; moving behind the pinned group unpins.
    CHECK(q.move(0x1004, 0));
    CHECK(q.find(0x1004)->pinned);
    CHECK(pending_vas(q) == std::vector<u64>{0x1004, 0x1003, 0x1000, 0x1001, 0x1002});
    CHECK(q.move(0x1003, 4));
    CHECK_FALSE(q.find(0x1003)->pinned);
    CHECK(pending_vas(q) == std::vector<u64>{0x1004, 0x1000, 0x1001, 0x1002, 0x1003});
    CHECK(q.move(0x1002, 1));  // at the pinned boundary: keeps its (unpinned) state
    CHECK(pending_vas(q) == std::vector<u64>{0x1004, 0x1002, 0x1000, 0x1001, 0x1003});
    CHECK_FALSE(q.move(0x9999, 0));

    auto first = q.next(2);
    REQUIRE(first);
    CHECK(first->va == 0x1004);
    CHECK(first->sessions == 1);
    CHECK(q.find(0x1004)->worker == 2);
    CHECK(q.running_count() == 1);
    CHECK_FALSE(q.remove(0x1004));  // running
    CHECK_FALSE(q.requeue(0x1004));
    q.finish(0x1004, "matched");
    CHECK(q.find(0x1004)->state == ItemState::done);
    CHECK(q.find(0x1004)->outcome == "matched");

    CHECK(q.skip(0x1002));
    CHECK(q.find(0x1002)->state == ItemState::skipped);
    CHECK_FALSE(q.skip(0x1002));
    CHECK(q.remove(0x1000));
    CHECK_FALSE(q.find(0x1000));
    CHECK(pending_vas(q) == std::vector<u64>{0x1001, 0x1003});

    CHECK(q.requeue(0x1004));  // done -> pending, at the end
    CHECK(q.requeue(0x1002));  // skipped -> pending
    CHECK(pending_vas(q) == std::vector<u64>{0x1004, 0x1001, 0x1003, 0x1002});  // 0x1004 is still pinned
    auto again = q.next(0);
    REQUIRE(again);
    CHECK(again->va == 0x1004);
    CHECK(again->sessions == 2);

    // A skip requested while the session ran keeps the item set aside.
    q.finish(0x1004, "skipped");
    CHECK(q.find(0x1004)->state == ItemState::skipped);

    CHECK(q.enqueue({QueueItem{.va = 0x1001}, QueueItem{.va = 0x2000, .name = "new"}}) == 1);  // 0x1001 is already queued
    CHECK(q.pending_count() == 4);
    CHECK(q.count(ItemState::skipped) == 1);
}

TEST_CASE("queue items round-trip through JSON") {
    QueueItem item{.va = 0x401060, .name = "?add@@YAHHH@Z", .display = "add", .difficulty = 4.2, .pinned = true,
                   .state = ItemState::done, .sessions = 2, .outcome = "matched"};
    auto back = QueueItem::from_json(item.to_json());
    REQUIRE(back);
    CHECK(back->va == item.va);
    CHECK(back->display == "add");
    CHECK(back->pinned);
    CHECK(back->state == ItemState::done);
    CHECK(back->sessions == 2);
    CHECK(back->outcome == "matched");
    CHECK(back->difficulty == doctest::Approx(4.2));
    CHECK_FALSE(QueueItem::from_json(Json{{"name", "no address"}}));
}

TEST_CASE("queue items: easy functions first, scored by their code") {
    const Program p = Program::open(test::fixture("x86/basic.exe")).value();
    const auto vas = select_functions(p, nullptr, {}).value();
    REQUIRE(vas.size() == 13);
    const FunctionAnalysis analysis = analyze_functions(p, vas);
    const auto items = make_queue_items(p, vas, &analysis, true);
    REQUIRE(items.size() == vas.size());
    CHECK(std::ranges::is_sorted(items, {}, &QueueItem::difficulty));
    auto item = [&](const char* name) { return *std::ranges::find(items, *p.resolve(name), &QueueItem::va); };
    auto position = [&](const char* name) { return std::ranges::find(items, *p.resolve(name), &QueueItem::va) - items.begin(); };
    // A 15-byte leaf goes before a switch with calls, and before the entry point that calls everything.
    CHECK(position("add") < position("dispatch"));
    CHECK(position("add") < position("entry"));
    CHECK(item("add").display == "int __cdecl add(int, int)");
    CHECK(item("add").state == ItemState::pending);
    // The score is the code's: the entry point's calls weigh more than its size alone.
    const QueueItem entry = item("entry");
    CHECK(entry.difficulty == doctest::Approx(difficulty(*analysis.find(entry.va))));
    CHECK(entry.difficulty > size_difficulty(*p.symbols().at(entry.va)) + 2);
    // Without an analysis (a GUI run started before it is done), the size decides.
    for (const auto& i : make_queue_items(p, vas, nullptr, true))
        CHECK(i.difficulty == doctest::Approx(size_difficulty(*p.symbols().at(i.va))));
    // Functions chosen in an order keep it.
    const auto kept = make_queue_items(p, vas, &analysis, false);
    CHECK(std::ranges::equal(kept, vas, {}, &QueueItem::va));
}
