#pragma once

// Dear ImGui build configuration for Decomp (IMGUI_USER_CONFIG, set in premake/deps.lua). Every
// translation unit that includes imgui.h is compiled with it: ours, ImGui's, ImPlot's and the text
// editor's, so inline functions and struct layouts agree everywhere.

// Assertions stay on in Release builds. They report programming errors (unbalanced Begin/End, ID stack
// misuse, bad table setup) that would otherwise corrupt the UI silently, and CI builds Release. A failed
// assertion calls the handler installed with decomp::gui::set_assert_handler() (gui/assert.hpp): by
// default it logs and aborts; the headless tests install one that throws.
namespace decomp::gui {
void assert_failed(const char* expr, const char* file, int line);
} // namespace decomp::gui

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ::decomp::gui::assert_failed(#_EXPR, __FILE__, __LINE__))

// ImVec2/ImVec4 arithmetic everywhere (ImPlot and the text editor define this too; defining it here
// keeps the include order irrelevant).
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

// Item hooks (ImGuiTestEngineHook_*, defined in gui/debug.cpp). ImGui calls them only while
// ImGuiContext::TestEngineHookItems is set, which the headless tests do to detect conflicting IDs among
// all items, not just the hovered one.
#define IMGUI_ENABLE_TEST_ENGINE

// IMGUI_DISABLE_OBSOLETE_FUNCTIONS stays undefined: ImPlot v1.0 still calls the obsolete
// ImDrawList::AddRect()/AddPolyline() overloads. Our own code should not use obsolete APIs either way.
