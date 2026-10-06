#include "relink/linker.hpp"

#include "core/fs.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"

#include <format>

namespace decomp::relink {

std::string_view to_string(LinkerKind kind) { return kind == LinkerKind::lld ? "lld" : "msvc"; }

Linker linker_for(const matching::Toolchain& toolchain, const std::string& configured) {
    Linker l;
    l.wrapper = toolchain.wrapper;
    l.env = toolchain.env;
    l.env_prepend = toolchain.env_prepend;
    const auto compiler_dir = fs::from_utf8(toolchain.compiler).parent_path();
#ifdef _WIN32
    const char* lld_name = "lld-link.exe";
#else
    const char* lld_name = "lld-link";
#endif
    std::error_code ec;
    if (!configured.empty()) {
        l.path = configured;
    } else if (toolchain.kind == matching::ToolchainKind::clang_cl || toolchain.kind == matching::ToolchainKind::clang) {
        if (!compiler_dir.empty() && std::filesystem::exists(compiler_dir / lld_name, ec)) l.path = fs::to_utf8(compiler_dir / lld_name);
        else l.path = matching::find_llvm_tool("lld-link").value_or("lld-link");
    } else {
        l.path = compiler_dir.empty() ? "link.exe" : fs::to_utf8(compiler_dir / "link.exe");
    }
    const std::string file = to_lower(fs::to_utf8(fs::from_utf8(l.path).filename()));
    l.kind = file.find("lld") != std::string::npos ? LinkerKind::lld : LinkerKind::msvc;
    return l;
}

namespace {

std::string version(u16 major, u16 minor) { return std::format("{}.{:02}", major, minor); }

const char* subsystem_name(u16 subsystem) {
    switch (subsystem) {
    case 1: return "native";
    case 2: return "windows";
    case 3: return "console";
    case 9: return "windowsce";
    case 10: return "efi_application";
    case 11: return "efi_boot_service_driver";
    case 12: return "efi_runtime_driver";
    case 13: return "efi_rom";
    case 16: return "boot_application";
    default: return nullptr;
    }
}

} // namespace

std::vector<std::string> image_link_flags(const pe::Image& image, std::string_view entry) {
    std::vector<std::string> f;
    const auto& oh = image.optional_header();
    const bool x64 = image.machine() == pe::machine::amd64;
    const u16 dll = image.dll_characteristics();
    const u16 file = image.characteristics();
    f.push_back("/nologo");
    f.push_back(x64 ? "/machine:x64" : "/machine:x86");
    if (image.is_dll()) f.push_back("/dll");
    if (const char* name = subsystem_name(image.subsystem()))
        f.push_back(std::format("/subsystem:{},{}", name, version(oh.subsystem_major, oh.subsystem_minor)));
    f.push_back("/osversion:" + version(oh.os_major, oh.os_minor));
    f.push_back("/version:" + version(oh.image_major, oh.image_minor));
    f.push_back(std::format("/base:{:#x}", image.image_base()));
    // Both linkers default to 4 KB sections in 512-byte file blocks (and lld-link warns about /align).
    if (oh.section_alignment != 4096) f.push_back(std::format("/align:{}", oh.section_alignment));
    if (oh.file_alignment != 512) f.push_back(std::format("/filealign:{}", oh.file_alignment));
    f.push_back(std::format("/stack:{},{}", oh.stack_reserve, oh.stack_commit));
    f.push_back(std::format("/heap:{},{}", oh.heap_reserve, oh.heap_commit));
    if (file & 0x0001) f.push_back("/fixed");
    else f.push_back((dll & 0x0040) ? "/dynamicbase" : "/dynamicbase:no");
    f.push_back((dll & 0x0100) ? "/nxcompat" : "/nxcompat:no");
    if (x64) f.push_back((dll & 0x0020) ? "/highentropyva" : "/highentropyva:no");
    f.push_back((file & 0x0020) ? "/largeaddressaware" : "/largeaddressaware:no");
    if (!image.is_dll()) f.push_back((dll & 0x8000) ? "/tsaware" : "/tsaware:no");
    // x86 /SAFESEH: without a handler table the linker marks the image NO_SEH unless told /SAFESEH:NO.
    if (!x64 && !(dll & 0x0400) && image.safe_seh_handlers().empty()) f.push_back("/safeseh:no");
    if (dll & 0x0200) f.push_back("/allowisolation:no");
    if (dll & 0x0800) f.push_back("/allowbind:no");
    if (dll & 0x1000) f.push_back("/appcontainer");
    if (dll & 0x4000) f.push_back("/guard:cf");
    if (dll & 0x0080) f.push_back("/integritycheck");
    if (const auto& cv = image.codeview()) {
        f.push_back("/debug");
        if (!cv->pdb_path.empty()) f.push_back("/pdbaltpath:" + cv->pdb_path);
    }
    for (const auto& e : image.debug_entries())
        if (e.type == pe::debug_type::repro) {
            f.push_back("/Brepro");
            break;
        }
    if (oh.checksum) f.push_back("/release");
    f.push_back("/incremental:no");
    f.push_back("/nodefaultlib");
    f.push_back("/manifest:no");
    if (!entry.empty()) f.push_back("/entry:" + std::string(entry));
    else if (image.is_dll()) f.push_back("/noentry");
    return f;
}

Result<LinkResult> run_linker(const Linker& linker, const LinkRequest& request) {
    LinkResult out;
    TRY(fs::create_directories(request.work_dir));
    std::vector<std::string> args = request.flags;
    args.push_back("/out:" + fs::to_utf8(request.output));
    if (!request.pdb.empty()) args.push_back("/pdb:" + fs::to_utf8(request.pdb));
    args.insert(args.end(), request.inputs.begin(), request.inputs.end());
    const auto rsp = request.work_dir / "link.rsp";
    TRY(fs::write_text(rsp, build_windows_command_line(args) + "\n"));

    ProcessSpec spec;
    spec.argv = linker.wrapper;
    spec.argv.push_back(linker.path);
    spec.argv.push_back("@" + fs::to_utf8(rsp));
    spec.cwd = request.work_dir;
    spec.timeout = request.timeout;
    spec.cancelled = request.cancelled;
    for (const auto& [k, v] : linker.env) spec.env.emplace_back(k, v);
    for (const auto& [k, v] : linker.env_prepend) {
        auto current = get_env(k);
        spec.env.emplace_back(k, current && !current->empty() ? v + path_list_separator() + *current : v);
    }
    // LINK and _LINK_ add options link.exe reads from the environment; only the relink's may apply.
    for (const char* var : {"LINK", "_LINK_"})
        if (std::ranges::none_of(linker.env, [&](const auto& kv) { return kv.first == var; })) spec.env.emplace_back(var, std::nullopt);
    out.command = linker.wrapper;
    out.command.push_back(linker.path);
    out.command.insert(out.command.end(), args.begin(), args.end());
    TRY_ASSIGN(auto r, run_process(spec));
    out.exit_code = r.exit_code;
    out.timed_out = r.timed_out;
    out.cancelled = r.cancelled;
    out.duration = r.duration;
    out.output = r.out + r.err;
    std::error_code ec;
    out.ok = r.ok() && std::filesystem::exists(request.output, ec);
    return out;
}

} // namespace decomp::relink
