#pragma once

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace decomp {

enum class ErrorCode {
    io,
    parse,
    not_found,
    invalid_argument,
    unsupported,
    process,
    timeout,
    network,
    api,
    cancelled,
    internal,
};

constexpr std::string_view to_string(ErrorCode code) {
    switch (code) {
    case ErrorCode::io: return "io";
    case ErrorCode::parse: return "parse";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::process: return "process";
    case ErrorCode::timeout: return "timeout";
    case ErrorCode::network: return "network";
    case ErrorCode::api: return "api";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::internal: return "internal";
    }
    return "unknown";
}

struct Error {
    ErrorCode code = ErrorCode::internal;
    std::string message;

    // Prefixes the message with context, e.g. "loading GAME.EXE: <message>".
    Error&& with_context(std::string_view context) && {
        message = std::string(context) + ": " + message;
        return std::move(*this);
    }

    std::string describe() const { return std::format("{} error: {}", to_string(code), message); }
};

template <class T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> make_error(ErrorCode code, std::string message) {
    return std::unexpected<Error>(Error{code, std::move(message)});
}

template <class... Args>
std::unexpected<Error> make_error(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected<Error>(Error{code, std::format(fmt, std::forward<Args>(args)...)});
}

} // namespace decomp

#define DECOMP_CONCAT_IMPL(a, b) a##b
#define DECOMP_CONCAT(a, b) DECOMP_CONCAT_IMPL(a, b)

// Propagates the error of a Result-returning expression; discards the value.
#define TRY(expr)                                                                                     \
    do {                                                                                              \
        auto&& try_result_ = (expr);                                                                  \
        if (!try_result_) return std::unexpected(std::move(try_result_.error()));                     \
    } while (0)

// Declares `decl` from the value of a Result-returning expression, or propagates its error.
//   TRY_ASSIGN(auto image, pe::Image::load(path));
#define TRY_ASSIGN(decl, expr)                                                                        \
    auto DECOMP_CONCAT(try_tmp_, __LINE__) = (expr);                                                  \
    if (!DECOMP_CONCAT(try_tmp_, __LINE__))                                                           \
        return std::unexpected(std::move(DECOMP_CONCAT(try_tmp_, __LINE__).error()));                 \
    decl = std::move(*DECOMP_CONCAT(try_tmp_, __LINE__))
