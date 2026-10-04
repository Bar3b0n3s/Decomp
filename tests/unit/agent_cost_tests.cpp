#include "agent/cost.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::agent;

TEST_CASE("price table and lookups") {
    CHECK(price_for("claude-opus-5-5").pricing == Pricing{4, 20, 5, 0.20});
    CHECK(price_for("claude-opus-5").pricing == Pricing{5, 25, 6.25, 0.50});
    CHECK(price_for("claude-opus-4-8").pricing == Pricing{5, 25, 6.25, 0.50});
    CHECK(price_for("claude-sonnet-5-5").pricing == Pricing{2, 10, 2.5, 0.20});
    CHECK(price_for("claude-fable-5-1").pricing == Pricing{10, 50, 12.5, 0.25});
    CHECK(price_for("claude-haiku-4-5").pricing == Pricing{1, 5, 1.25, 0.10});
    CHECK(price_for("claude-opus-5-5").known);

    // Dated / platform variants resolve to the longest matching id, not a shorter prefix.
    CHECK(price_for("claude-opus-5-5-20270101").model == "claude-opus-5-5");
    CHECK(price_for("claude-opus-5@20260101").model == "claude-opus-5");

    auto unknown = price_for("gpt-something");
    CHECK_FALSE(unknown.known);
    CHECK(unknown.model == "claude-opus-5-5");
    CHECK(unknown.pricing == Pricing{4, 20, 5, 0.20});
}

TEST_CASE("cost_usd prices every token class") {
    const Pricing opus = price_for("claude-opus-5-5").pricing;
    CHECK(cost_usd(Usage{1'000'000, 0, 0, 0}, opus) == doctest::Approx(4.0));
    CHECK(cost_usd(Usage{0, 1'000'000, 0, 0}, opus) == doctest::Approx(20.0));
    CHECK(cost_usd(Usage{0, 0, 1'000'000, 0}, opus) == doctest::Approx(5.0));
    CHECK(cost_usd(Usage{0, 0, 0, 1'000'000}, opus) == doctest::Approx(0.20));
    CHECK(cost_usd(Usage{2000, 500, 10000, 50000}, opus) ==
          doctest::Approx((2000 * 4.0 + 500 * 20.0 + 10000 * 5.0 + 50000 * 0.20) / 1e6));
}

TEST_CASE("response_cost uses the response model and per-iteration models") {
    Response r;
    r.model = "claude-opus-5-5";
    r.usage = Usage{1000, 200, 0, 3000};
    r.usage_raw = r.usage.to_json();
    auto c = response_cost(r, "claude-opus-5-5");
    CHECK(c.usd == doctest::Approx((1000 * 4.0 + 200 * 20.0 + 3000 * 0.20) / 1e6));
    CHECK(c.usage == r.usage);
    CHECK_FALSE(c.unknown_model);

    // A sticky fallback served by another model is priced at that model's rates.
    r.model = "claude-opus-4-8";
    CHECK(response_cost(r, "claude-opus-5-5").usd == doctest::Approx((1000 * 5.0 + 200 * 25.0 + 3000 * 0.50) / 1e6));

    // Unknown response model -> the configured model's price.
    r.model = "claude-experimental";
    CHECK(response_cost(r, "claude-sonnet-5-5").usd == doctest::Approx((1000 * 2.0 + 200 * 10.0 + 3000 * 0.20) / 1e6));
    CHECK_FALSE(response_cost(r, "claude-sonnet-5-5").unknown_model);
    CHECK(response_cost(r, "also-unknown").unknown_model);

    // Server-side fallback attempts: each iteration is priced with its own model.
    r.model = "claude-opus-4-8";
    r.usage = Usage{1500, 300, 0, 0};
    r.usage_raw = Json::parse(R"({
        "input_tokens": 1500, "output_tokens": 300,
        "iterations": [
            {"type": "message", "model": "claude-opus-5-5", "input_tokens": 1000, "output_tokens": 40,
             "cache_creation_input_tokens": 0, "cache_read_input_tokens": 2000},
            {"type": "fallback_message", "model": "claude-opus-4-8", "input_tokens": 1500, "output_tokens": 300},
            {"type": "message", "input_tokens": 10, "output_tokens": 1}
        ]})");
    auto f = response_cost(r, "claude-opus-5-5");
    const double expected = (1000 * 4.0 + 40 * 20.0 + 2000 * 0.20) / 1e6 + (1500 * 5.0 + 300 * 25.0) / 1e6 +
                            (10 * 4.0 + 1 * 20.0) / 1e6;
    CHECK(f.usd == doctest::Approx(expected));
    CHECK(f.usage == Usage{2510, 341, 0, 2000});
}
