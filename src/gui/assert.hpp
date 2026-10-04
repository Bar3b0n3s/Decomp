#pragma once

// IM_ASSERT (gui/imgui_config.h) is on in every build and lands here.

namespace decomp::gui {

// Called on the thread whose assertion failed. A handler may throw (the headless tests turn assertions
// into exceptions) or return, in which case execution continues after the assertion.
using AssertHandler = void (*)(const char* expr, const char* file, int line);

// Installs `handler` (nullptr restores the default) and returns the previous one.
AssertHandler set_assert_handler(AssertHandler handler);

// Logs the failed expression and its location, then aborts.
[[noreturn]] void default_assert_handler(const char* expr, const char* file, int line);

// The IM_ASSERT target: calls the installed handler.
void assert_failed(const char* expr, const char* file, int line);

} // namespace decomp::gui
