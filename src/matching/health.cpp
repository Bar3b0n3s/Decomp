#include "matching/health.hpp"

#include "core/fs.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"
#include "formats/coff.hpp"

#include <format>

namespace decomp::matching {

std::string detect_version(const Toolchain& toolchain) {
    ProcessSpec spec;
    spec.argv = toolchain.wrapper;
    spec.argv.push_back(toolchain.compiler);
    if (toolchain.kind != ToolchainKind::msvc) spec.argv.push_back("--version");
    spec.timeout = std::chrono::seconds(15);
    for (const auto& [k, v] : toolchain.env) spec.env.emplace_back(k, v);
    for (const auto& [k, v] : toolchain.env_prepend) {
        auto current = get_env(k);
        spec.env.emplace_back(k, current && !current->empty() ? v + path_list_separator() + *current : v);
    }
    auto r = run_process(spec);
    if (!r || r->timed_out) return {};
    // cl.exe writes its banner to stderr (and usage to stdout); the line with "Version" is the one.
    const std::string text = r->err + "\n" + r->out;
    std::string first;
    for (auto line : split(text, '\n')) {
        const std::string_view l = trim(line);
        if (l.empty()) continue;
        if (first.empty()) first = std::string(l);
        if (toolchain.kind == ToolchainKind::msvc && l.find("Version") != std::string_view::npos) return std::string(l);
    }
    return toolchain.kind == ToolchainKind::msvc ? std::string() : first;
}

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
    report.version = detect_version(toolchain);
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
            {"arch", r.arch ? Json(std::string(to_string(*r.arch))) : Json(nullptr)}, {"functions", r.functions}, {"version", r.version}};
}

} // namespace decomp::matching
