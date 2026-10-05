#pragma once

// What `decomp run` and `decomp agent` share while a RunController runs: the bus, the event log and the
// controller, and the forwarding of logged warnings into the run's events.

#include "core/log.hpp"
#include "events/bus.hpp"
#include "run/controller.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace decomp::cli {

// Shared with the --interactive reader, which blocks on stdin and may outlive the command; it sends
// nothing once `active` is false.
struct LiveRun {
    explicit LiveRun(std::string run_id) : bus(std::move(run_id)) {}
    events::EventBus bus;
    std::unique_ptr<events::JsonlEventLog> log;
    std::unique_ptr<run::RunController> controller;
    std::atomic<bool> active{true};
};

// While it lives, warnings and errors logged anywhere become `log` events of the run, so the run log
// and the views keep them.
class RunLogForwarder {
public:
    explicit RunLogForwarder(std::shared_ptr<LiveRun> live)
        : id_(log::add_sink([live = std::move(live)](const log::Entry& e) {
              if (e.level >= log::Level::warn && live->active)
                  live->bus.publish(events::LogLine{std::string(log::to_string(e.level)), e.message, e.session}, e.worker);
          })) {}
    ~RunLogForwarder() { log::remove_sink(id_); }
    RunLogForwarder(const RunLogForwarder&) = delete;
    RunLogForwarder& operator=(const RunLogForwarder&) = delete;

private:
    int id_;
};

} // namespace decomp::cli
