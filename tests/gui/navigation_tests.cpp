#include "gui/navigation.hpp"
#include "gui/notifications.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace decomp;
using namespace decomp::gui;

TEST_CASE("navigation history: open, back, forward, branch") {
    Navigation nav;
    CHECK_FALSE(nav.can_back());
    CHECK_FALSE(nav.can_forward());
    CHECK(nav.current() == nullptr);
    CHECK_FALSE(nav.take_request());

    nav.open("dashboard");
    nav.open("inspector", {.va = 0x401000});
    nav.open("diff_viewer", {.va = 0x401000, .anchor = "attempt:3"});
    REQUIRE(nav.current() != nullptr);
    CHECK(nav.current()->view == "diff_viewer");
    auto request = nav.take_request();
    REQUIRE(request);
    CHECK(request->target.anchor == "attempt:3");
    CHECK_FALSE(nav.take_request());  // applied once

    CHECK(nav.back());
    CHECK(nav.current()->view == "inspector");
    CHECK(nav.take_request()->view == "inspector");
    CHECK(nav.back());
    CHECK_FALSE(nav.back());
    CHECK(nav.current()->view == "dashboard");
    CHECK(nav.forward());
    CHECK(nav.current()->view == "inspector");
    CHECK(nav.can_forward());

    // Opening something new from the middle drops the forward history.
    nav.open("logs", {.anchor = "seq:120"});
    CHECK_FALSE(nav.can_forward());
    CHECK(nav.entries().size() == 3);
    CHECK(nav.entries().back().view == "logs");

    // Re-opening the current entry does not duplicate it, but is requested again.
    (void)nav.take_request();
    nav.open("logs", {.anchor = "seq:120"});
    CHECK(nav.entries().size() == 3);
    CHECK(nav.take_request().has_value());
}

TEST_CASE("navigation history is bounded") {
    Navigation nav;
    for (int i = 0; i < 300; ++i) nav.open("inspector", {.va = static_cast<u64>(0x401000 + i)});
    CHECK(nav.entries().size() == Navigation::kMaxEntries);
    CHECK(nav.current()->target.va == static_cast<u64>(0x401000 + 299));
    int steps = 0;
    while (nav.back()) ++steps;
    CHECK(steps == static_cast<int>(Navigation::kMaxEntries) - 1);
}

TEST_CASE("notifications: info and warnings expire, errors stay") {
    Notifications n;
    CHECK(Notifications::lifetime(Severity::info) == doctest::Approx(Notifications::kInfoSeconds));
    CHECK(Notifications::lifetime(Severity::warning) == doctest::Approx(Notifications::kWarningSeconds));
    CHECK_FALSE(Notifications::lifetime(Severity::error));

    const u64 info = n.notify(Severity::info, "Function matched", NavEntry{"diff_viewer", {.va = 0x401060}});
    n.notify(Severity::warning, "Run budget at 80%");
    n.notify(Severity::error, "Authentication error (401)", NavEntry{"settings", {}});
    n.notify(Severity::info, "A fallback model served a turn", std::nullopt, false);  // history only

    CHECK(n.history().size() == 4);
    CHECK(n.unread() == 4);
    CHECK(n.visible_toasts(10.0).size() == 3);  // first shown at t=10
    CHECK(n.next_expiry(10.0) == doctest::Approx(Notifications::kInfoSeconds));
    CHECK(n.visible_toasts(14.0).size() == 3);
    CHECK(n.visible_toasts(15.5).size() == 2);  // the info toast expired
    auto toasts = n.visible_toasts(100.0);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0]->severity == Severity::error);
    CHECK_FALSE(n.next_expiry(100.0));

    n.dismiss(toasts[0]->id);
    CHECK(n.visible_toasts(100.0).empty());
    CHECK(n.history().size() == 4);  // the history keeps everything
    CHECK(n.history().front().id == info);
    REQUIRE(n.history().front().link);
    CHECK(n.history().front().link->view == "diff_viewer");
    n.mark_read();
    CHECK(n.unread() == 0);
}

TEST_CASE("notifications: too many toasts keep the errors") {
    Notifications n;
    n.notify(Severity::error, "e1");
    for (int i = 0; i < 10; ++i) n.notify(Severity::info, "i" + std::to_string(i));
    auto toasts = n.visible_toasts(0.0);
    CHECK(toasts.size() == Notifications::kMaxToasts);
    CHECK(toasts.front()->text == "e1");
    CHECK(toasts.back()->text == "i9");
}

TEST_CASE("notifications: posted from any thread, bounded history") {
    Notifications n;
    std::atomic<int> wakes = 0;
    n.set_wake([&] { ++wakes; });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&n, t] {
            for (int i = 0; i < 200; ++i) n.notify(Severity::info, std::to_string(t) + ":" + std::to_string(i));
        });
    for (auto& t : threads) t.join();
    CHECK(wakes == 800);
    CHECK(n.history().size() == Notifications::kHistoryLimit);
    CHECK(n.unread() == Notifications::kHistoryLimit);
    n.clear();
    CHECK(n.history().empty());
}
