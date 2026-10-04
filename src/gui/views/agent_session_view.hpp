#pragma once

#include "gui/view.hpp"

#include <memory>
#include <string>
#include <vector>

namespace decomp::gui {

std::unique_ptr<View> make_agent_session_view();

// What the Agent session view offers besides its window (tests drive it through this; the view returned
// by make_agent_session_view() implements it).
class AgentSessionControl {
public:
    virtual ~AgentSessionControl() = default;

    virtual std::string session() const = 0;  // the session shown ("" for none)
    virtual bool transcript_loaded() const = 0;
    virtual usize timeline_items() const = 0;
    // Guidance queued from this view that the transcript does not have yet (by id).
    virtual std::vector<u64> pending_guidance() const = 0;
    // Queues guidance for the session shown; its id, 0 when it could not be queued.
    virtual u64 send_guidance(ViewContext& ctx, std::string text) = 0;
    virtual bool retract_guidance(ViewContext& ctx, u64 id) = 0;
};

} // namespace decomp::gui
