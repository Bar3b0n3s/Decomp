#include "matching/suggest.hpp"

#include <format>
#include <map>
#include <set>

namespace decomp::matching {

namespace {

std::string release_of(const pe::BuildTool& t) {
    return t.version.visual_studio.empty() ? std::string("an unknown release") : t.version.visual_studio;
}

// The release a tool belongs to, at the granularity toolchains are named by ("vs2019", not 16.11).
std::string family_of(const pe::BuildTool& t) {
    if (!t.product) return {};
    if (!t.version.suggested_name.empty()) return t.version.suggested_name;
    return std::string(pe::to_string(t.product->release));
}

} // namespace

std::optional<ToolchainSuggestion> suggest_toolchain(const pe::BuildInfo& build) {
    const pe::BuildTool* main = build.main_compiler();
    if (!main || !main->product) return std::nullopt;
    ToolchainSuggestion s;
    s.name = main->version.suggested_name;
    s.visual_studio = main->version.visual_studio;
    s.compiler = main->description();
    s.product_id = main->entry.product_id;
    s.build = main->entry.build;

    const std::string_view variant = main->product->variant;
    if (variant.find("Standard edition") != std::string_view::npos || variant.find("Introductory edition") != std::string_view::npos)
        s.notes.push_back(std::format("The {} compiler has no optimizer: the code is unoptimized whatever flags were given (match with /Od).",
                                      variant.find("Standard") != std::string_view::npos ? "Standard edition" : "Introductory edition"));
    if (build.has_variant("LTCG"))
        s.notes.push_back("Objects were compiled for link-time code generation (/GL): the linker generated their code together, so "
                          "inlining and layout cross object boundaries and a function compiled alone may not match.");
    if (build.has_variant("PGO optimized"))
        s.notes.push_back("Code was optimized with a profile (PGO): inlining and layout follow a training run that cannot be reproduced.");
    if (build.has_variant("CIL")) s.notes.push_back("Some objects were compiled to CIL (/clr).");

    const u32 c = build.objects_in("C"), cpp = build.objects_in("C++");
    if (c && cpp) s.notes.push_back(std::format("{} C and {} C++ objects.", c, cpp));
    // Other builds of the release (usually the runtime and SDK libraries), and other releases.
    std::set<u16> other_builds;
    std::map<std::string, u32> other_releases;
    const std::string family = family_of(*main);
    for (const auto& t : build.compilers) {
        if (!t.product || t.product->language == "Basic") continue;
        if (family_of(t) == family) {
            if (t.entry.build != main->entry.build) other_builds.insert(t.entry.build);
        } else {
            other_releases[release_of(t)] += t.entry.count;
        }
    }
    if (!other_builds.empty()) {
        std::string list;
        for (u16 b : other_builds) list += (list.empty() ? "" : ", ") + std::to_string(b);
        s.notes.push_back(std::format("Other builds of the same compiler made some objects ({}): probably the runtime and SDK libraries.", list));
    }
    for (const auto& [release, objects] : other_releases)
        s.notes.push_back(std::format("{} objects came from {}: libraries built with another compiler.", objects, release));
    for (const auto& a : build.assemblers)
        s.notes.push_back(std::format("{} objects were assembled with {}: hand-written assembly.", a.entry.count, a.description()));
    if (build.unmarked) s.notes.push_back(std::format("{} objects carry no tool id (made by older or other tools).", build.unmarked));
    if (!build.checksum_ok)
        s.notes.push_back("The Rich header's checksum does not match: it was edited after linking, so its entries may not be accurate.");
    return s;
}

std::string_view to_string(ToolchainFit fit) {
    switch (fit) {
    case ToolchainFit::unknown: return "unknown";
    case ToolchainFit::same_build: return "same_build";
    case ToolchainFit::same_release: return "same_release";
    case ToolchainFit::other_release: return "other_release";
    }
    return "unknown";
}

FitReport toolchain_fit(const pe::BuildInfo& build, std::optional<u32> comp_id) {
    FitReport r;
    const pe::BuildTool* main = build.main_compiler();
    if (!comp_id) {
        r.text = "the toolchain's objects carry no compiler id (@comp.id), so its version cannot be compared";
        return r;
    }
    pe::BuildTool tool;
    tool.entry = {static_cast<u16>(*comp_id >> 16), static_cast<u16>(*comp_id & 0xFFFF), 1};
    tool.product = pe::rich_product(tool.entry.product_id);
    if (tool.product) tool.version = pe::tool_version(*tool.product, tool.entry.build);
    r.toolchain_compiler = tool.description();
    if (!tool.version.visual_studio.empty()) r.toolchain_compiler += std::format(" ({})", tool.version.visual_studio);
    if (!main || !main->product || !tool.product || tool.product->tool != pe::RichTool::compiler) {
        r.text = main ? std::format("the toolchain's compiler id {:#010x} is not a known compiler", *comp_id)
                      : "the target's Rich header names no compiler";
        return r;
    }
    if (family_of(tool) != family_of(*main)) {
        r.fit = ToolchainFit::other_release;
        r.text = std::format("another release: the target's compiler is {} ({}), the toolchain's {}", main->description(), release_of(*main),
                             r.toolchain_compiler);
    } else if (tool.entry.build != main->entry.build || tool.product->major != main->product->major || tool.product->minor != main->product->minor) {
        r.fit = ToolchainFit::same_release;
        r.text = std::format("the same release, another build: the target's compiler is build {}, the toolchain's build {}", main->entry.build,
                             tool.entry.build);
    } else {
        r.fit = ToolchainFit::same_build;
        r.text = std::format("the same compiler as the target's code: {}", main->description());
    }
    return r;
}

Json to_json(const pe::BuildTool& t) {
    return {{"product_id", t.entry.product_id},
            {"product", t.product ? std::string(t.product->name) : std::string()},
            {"build", t.entry.build},
            {"count", t.entry.count},
            {"tool", std::string(pe::to_string(t.product ? t.product->tool : pe::RichTool::unknown))},
            {"language", t.product ? std::string(t.product->language) : std::string()},
            {"variant", t.product ? std::string(t.product->variant) : std::string()},
            {"version", t.product ? t.version.text() : std::string()},
            {"visual_studio", t.version.visual_studio},
            {"description", t.description()}};
}

Json to_json(const pe::BuildInfo& b) {
    auto list = [](const std::vector<pe::BuildTool>& tools) {
        Json out = Json::array();
        for (const auto& t : tools) out.push_back(to_json(t));
        return out;
    };
    Json j = {{"checksum_ok", b.checksum_ok}, {"compilers", list(b.compilers)}, {"linker", b.linker ? to_json(*b.linker) : Json(nullptr)},
              {"assemblers", list(b.assemblers)}, {"others", list(b.others)}, {"imports", b.imports}, {"unmarked", b.unmarked}};
    if (const pe::BuildTool* m = b.main_compiler()) j["main_compiler"] = to_json(*m);
    return j;
}

Json to_json(const ToolchainSuggestion& s) {
    return {{"name", s.name}, {"visual_studio", s.visual_studio}, {"compiler", s.compiler}, {"product_id", s.product_id}, {"build", s.build},
            {"notes", s.notes}};
}

Json to_json(const FitReport& f) {
    return {{"fit", std::string(to_string(f.fit))}, {"toolchain_compiler", f.toolchain_compiler}, {"text", f.text}};
}

} // namespace decomp::matching
