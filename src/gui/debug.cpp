#include "gui/debug.hpp"

#include <imgui_internal.h>

#include <cstdarg>
#include <format>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace decomp::gui::debug {

struct IdConflictDetector::State {
    int frame = -1;
    std::unordered_set<ImGuiID> seen;  // IDs added in `frame`
    std::unordered_map<ImGuiID, std::string> labels;
    std::vector<IdConflict> conflicts;
};

namespace {

std::mutex g_mutex;
std::map<ImGuiContext*, IdConflictDetector::State*> g_detectors;

IdConflictDetector::State* find_state(ImGuiContext* ctx) {
    std::lock_guard lock(g_mutex);
    auto it = g_detectors.find(ctx);
    return it == g_detectors.end() ? nullptr : it->second;
}

} // namespace

IdConflictDetector::IdConflictDetector(ImGuiContext* ctx)
    : ctx_(ctx ? ctx : ImGui::GetCurrentContext()), state_(std::make_unique<State>()) {
    IM_ASSERT(ctx_ != nullptr && "IdConflictDetector needs an ImGui context");
    std::lock_guard lock(g_mutex);
    IM_ASSERT(!g_detectors.contains(ctx_) && "one IdConflictDetector per context");
    g_detectors[ctx_] = state_.get();
    ctx_->TestEngineHookItems = true;
}

IdConflictDetector::~IdConflictDetector() {
    std::lock_guard lock(g_mutex);
    g_detectors.erase(ctx_);
    ctx_->TestEngineHookItems = false;
}

const std::vector<IdConflict>& IdConflictDetector::conflicts() const { return state_->conflicts; }

std::string IdConflictDetector::describe() const {
    std::string out;
    for (const auto& c : state_->conflicts) {
        auto it = state_->labels.find(c.id);
        const std::string& label = !c.label.empty() ? c.label : (it != state_->labels.end() ? it->second : c.label);
        out += std::format("frame {}: 0x{:08x} \"{}\"\n", c.frame, c.id, label);
    }
    return out;
}

void IdConflictDetector::clear() {
    state_->conflicts.clear();
    state_->seen.clear();
    state_->frame = -1;
}

} // namespace decomp::gui::debug

// ImGui's item hooks (declared in imgui_internal.h under IMGUI_ENABLE_TEST_ENGINE). ImGui calls the item
// hooks only while ImGuiContext::TestEngineHookItems is set, i.e. while a detector is attached.
using decomp::gui::debug::IdConflictDetector;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& /*bb*/, const ImGuiLastItemData* item_data) {
    // Registrations without item data are windows and ButtonBehavior() calls without ItemAdd(); ImGui's
    // own conflict check ignores those as well.
    if (id == 0 || item_data == nullptr || (item_data->ItemFlags & ImGuiItemFlags_AllowDuplicateId) != 0) return;
    IdConflictDetector::State* state = decomp::gui::debug::find_state(ctx);
    if (!state) return;
    if (state->frame != ctx->FrameCount) {
        state->frame = ctx->FrameCount;
        state->seen.clear();
    }
    if (!state->seen.insert(id).second) state->conflicts.push_back({id, ctx->FrameCount, {}});
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags /*flags*/) {
    if (id == 0 || label == nullptr) return;
    if (IdConflictDetector::State* state = decomp::gui::debug::find_state(ctx)) state->labels[id] = label;
}

void ImGuiTestEngineHook_Log(ImGuiContext* /*ctx*/, const char* /*fmt*/, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext* ctx, ImGuiID id) {
    IdConflictDetector::State* state = decomp::gui::debug::find_state(ctx);
    if (!state) return nullptr;
    auto it = state->labels.find(id);
    return it == state->labels.end() ? nullptr : it->second.c_str();
}
