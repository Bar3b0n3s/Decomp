#include "gui/assert.hpp"

#include "core/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace decomp::gui {
namespace {

std::atomic<AssertHandler> g_handler{nullptr};

} // namespace

AssertHandler set_assert_handler(AssertHandler handler) {
    AssertHandler previous = g_handler.exchange(handler);
    return previous ? previous : &default_assert_handler;
}

void default_assert_handler(const char* expr, const char* file, int line) {
    log::error("GUI assertion failed: {} ({}:{})", expr ? expr : "?", file ? file : "?", line);
    std::fflush(stderr);
    std::abort();
}

void assert_failed(const char* expr, const char* file, int line) {
    if (AssertHandler handler = g_handler.load()) handler(expr, file, line);
    else default_assert_handler(expr, file, line);
}

} // namespace decomp::gui
