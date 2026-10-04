#pragma once

// Token prices and cost accounting for Messages API usage.

#include "agent/messages.hpp"
#include "core/json.hpp"

#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace decomp::agent {

// USD per million tokens.
struct Pricing {
    double input = 0;
    double output = 0;
    double cache_write_5m = 0;
    double cache_read = 0;

    bool operator==(const Pricing&) const = default;
};

inline constexpr std::string_view kDefaultPricingModel = "claude-opus-5-5";

// Model id -> price (first-party API rates).
const std::map<std::string, Pricing, std::less<>>& default_price_table();

struct PriceLookup {
    Pricing pricing;
    std::string model;   // the table row used
    bool known = true;   // false: unknown model, priced as claude-opus-5-5
};

// Exact match first, then the longest table entry that prefixes the id (dated or `@` variants).
PriceLookup price_for(std::string_view model);

double cost_usd(const Usage& usage, const Pricing& pricing);

struct ResponseCost {
    double usd = 0;
    Usage usage;                // tokens billed across all attempts
    bool unknown_model = false; // some attempt was priced with the fallback row
};

// Cost of one response. When the raw usage carries an `iterations` array (server-side fallback attempts)
// each entry is costed with its own model's price (entries without a model use `default_model`);
// otherwise the top-level usage is priced for the response's model, or `default_model` when that model
// is not in the table.
ResponseCost response_cost(const Response& response, std::string_view default_model);

// Spend shared by the sessions of a run (thread-safe). Every turn's cost is added; the limit can change
// while the run goes on.
class SpendLedger {
public:
    explicit SpendLedger(double limit_usd = 0, double spent_usd = 0) : limit_(limit_usd), spent_(spent_usd) {}

    void add(double usd) {
        std::lock_guard lock(mutex_);
        spent_ += usd;
    }
    double spent() const {
        std::lock_guard lock(mutex_);
        return spent_;
    }
    double limit() const {  // 0 = unlimited
        std::lock_guard lock(mutex_);
        return limit_;
    }
    void set_limit(double usd) {
        std::lock_guard lock(mutex_);
        limit_ = usd > 0 ? usd : 0;
    }
    bool exhausted() const {
        std::lock_guard lock(mutex_);
        return limit_ > 0 && spent_ >= limit_;
    }

private:
    mutable std::mutex mutex_;
    double limit_ = 0;
    double spent_ = 0;
};

} // namespace decomp::agent
