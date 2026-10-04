#include "agent/cost.hpp"

namespace decomp::agent {

const std::map<std::string, Pricing, std::less<>>& default_price_table() {
    static const std::map<std::string, Pricing, std::less<>> table{
        {"claude-opus-5-5", {4.0, 20.0, 5.0, 0.20}},
        {"claude-opus-5", {5.0, 25.0, 6.25, 0.50}},
        {"claude-opus-4-8", {5.0, 25.0, 6.25, 0.50}},
        {"claude-sonnet-5-5", {2.0, 10.0, 2.5, 0.20}},
        {"claude-fable-5-1", {10.0, 50.0, 12.5, 0.25}},
        {"claude-haiku-4-5", {1.0, 5.0, 1.25, 0.10}},
    };
    return table;
}

PriceLookup price_for(std::string_view model) {
    const auto& table = default_price_table();
    if (auto it = table.find(model); it != table.end()) return PriceLookup{it->second, it->first, true};

    const std::pair<const std::string, Pricing>* best = nullptr;
    for (const auto& entry : table) {
        const std::string& id = entry.first;
        if (model.size() > id.size() && model.starts_with(id) && (model[id.size()] == '-' || model[id.size()] == '@')) {
            if (!best || id.size() > best->first.size()) best = &entry;
        }
    }
    if (best) return PriceLookup{best->second, best->first, true};

    const auto& fallback = table.at(std::string(kDefaultPricingModel));
    return PriceLookup{fallback, std::string(kDefaultPricingModel), false};
}

double cost_usd(const Usage& usage, const Pricing& pricing) {
    const double per_token = 1.0 / 1'000'000.0;
    return per_token * (static_cast<double>(usage.input_tokens) * pricing.input +
                        static_cast<double>(usage.output_tokens) * pricing.output +
                        static_cast<double>(usage.cache_creation_input_tokens) * pricing.cache_write_5m +
                        static_cast<double>(usage.cache_read_input_tokens) * pricing.cache_read);
}

ResponseCost response_cost(const Response& response, std::string_view default_model) {
    ResponseCost result;
    auto lookup = [&](std::string_view model) {
        PriceLookup price = price_for(model.empty() ? default_model : model);
        if (!price.known && model != default_model) {
            PriceLookup configured = price_for(default_model);
            if (configured.known) price = configured;
        }
        result.unknown_model = result.unknown_model || !price.known;
        return price.pricing;
    };

    const Json* iterations = nullptr;
    if (response.usage_raw.is_object()) {
        auto it = response.usage_raw.find("iterations");
        if (it != response.usage_raw.end() && it->is_array() && !it->empty()) iterations = &*it;
    }
    if (iterations) {
        for (const Json& entry : *iterations) {
            const Usage usage = Usage::from_json(entry);
            result.usage.add(usage);
            result.usd += cost_usd(usage, lookup(json_string_or(entry, "model", "")));
        }
        return result;
    }
    result.usage = response.usage;
    result.usd = cost_usd(response.usage, lookup(response.model));
    return result;
}

} // namespace decomp::agent
