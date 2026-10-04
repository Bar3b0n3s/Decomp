#include "project/progress.hpp"

namespace decomp::project {

Progress compute_progress(const SymbolDb& symbols, const Project& project) {
    Progress p;
    for (const auto* f : symbols.functions()) {
        const FunctionInfo info = project.function_info(f->va);
        auto& b = p.buckets[info.status];
        ++b.functions;
        b.bytes += f->size;
        ++p.functions;
        p.code_bytes += f->size;
        p.spend_usd += info.cost_usd;
        if (info.status == FunctionStatus::matched) {
            ++p.matched_functions;
            p.matched_bytes += f->size;
        }
    }
    return p;
}

Json to_json(const Progress& p) {
    Json buckets = Json::object();
    for (const auto& [status, b] : p.buckets) buckets[std::string(to_string(status))] = {{"functions", b.functions}, {"bytes", b.bytes}};
    return {{"functions", p.functions},
            {"matched_functions", p.matched_functions},
            {"code_bytes", p.code_bytes},
            {"matched_bytes", p.matched_bytes},
            {"percent_functions", p.percent_functions()},
            {"percent_bytes", p.percent_bytes()},
            {"buckets", buckets},
            {"spend_usd", p.spend_usd}};
}

} // namespace decomp::project
