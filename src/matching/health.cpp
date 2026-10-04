#include "matching/health.hpp"

#include "core/fs.hpp"
#include "formats/coff.hpp"

#include <format>

namespace decomp::matching {

Result<HealthReport> check_toolchain(const Toolchain& toolchain) {
    TRY_ASSIGN(auto tmp, fs::TempDir::create("decomp-probe"));
    Compiler compiler(toolchain, tmp.path());
    CompileRequest request;
    request.source = "int decomp_probe(int x) { return x * 3 + 1; }\n";
    TRY_ASSIGN(auto r, compiler.compile(request));
    HealthReport report;
    report.ok = r.ok;
    report.command = r.command;
    report.output = r.output;
    report.duration = r.duration;
    if (r.ok) {
        auto obj = coff::Object::parse(r.object_data);
        if (obj) {
            report.arch = obj->arch();
            report.functions = obj->function_symbols().size();
            report.object = std::format("COFF {} object, {} functions", to_string(obj->arch()), report.functions);
        } else {
            report.ok = false;
            report.object = "not a COFF object (" + obj.error().message + ")";
        }
    }
    return report;
}

Json to_json(const HealthReport& r) {
    return {{"ok", r.ok}, {"command", r.command}, {"output", r.output}, {"duration_ms", r.duration.count()}, {"object", r.object},
            {"arch", r.arch ? Json(std::string(to_string(*r.arch))) : Json(nullptr)}, {"functions", r.functions}};
}

} // namespace decomp::matching
