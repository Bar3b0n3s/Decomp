#include "relink/linker.hpp"

#include "core/fs.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"

#include <array>
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

std::string detect_linker_version(const Linker& linker) {
    ProcessSpec spec;
    spec.argv = linker.wrapper;
    spec.argv.push_back(linker.path);
    // link.exe prints its banner when run without arguments.
    if (linker.kind == LinkerKind::lld) spec.argv.push_back("--version");
    spec.timeout = std::chrono::seconds(15);
    for (const auto& [k, v] : linker.env) spec.env.emplace_back(k, v);
    for (const auto& [k, v] : linker.env_prepend) {
        auto current = get_env(k);
        spec.env.emplace_back(k, current && !current->empty() ? v + path_list_separator() + *current : v);
    }
    auto r = run_process(spec);
    if (!r || r->timed_out) return {};
    const std::string_view marker = linker.kind == LinkerKind::lld ? "LLD " : "Version ";
    const std::string text = r->out + "\n" + r->err;
    for (auto line : split(text, '\n')) {
        const std::string_view l = trim(line);
        if (l.find(marker) != std::string_view::npos) return std::string(l);
    }
    return {};
}

namespace {

// The first "<major>.<minor>.<build>" after `marker` in `text`.
std::optional<std::array<u16, 3>> version_after(std::string_view text, std::string_view marker) {
    for (usize at = text.find(marker); at != std::string_view::npos; at = text.find(marker, at + 1)) {
        std::array<u16, 3> v{};
        usize i = at + marker.size(), part = 0;
        for (; part < 3; ++part) {
            const usize start = i;
            u32 n = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9' && n <= 0xFFFF) n = n * 10 + static_cast<u32>(text[i++] - '0');
            if (i == start || n > 0xFFFF) break;
            v[part] = static_cast<u16>(n);
            if (part < 2) {
                if (i >= text.size() || text[i] != '.') break;
                ++i;
            }
        }
        if (part == 3) return v;
    }
    return std::nullopt;
}

} // namespace

std::string OriginalLinker::text() const {
    return kind == LinkerKind::lld ? std::format("lld-link of LLVM {}.{}.{}", major, minor, build)
                                   : std::format("link.exe {}.{:02}.{}", major, minor, build);
}

std::optional<OriginalLinker> original_linker(const pe::Image& image, const std::vector<std::string>& compilers) {
    if (const auto build = image.build_info()) {
        if (!build->linker) return std::nullopt;
        return OriginalLinker{LinkerKind::msvc, image.linker_major(), image.linker_minor(), build->linker->entry.build};
    }
    // No Rich header: link.exe did not make it. lld-link writes linker version 14.0.
    if (image.linker_major() != 14 || image.linker_minor() != 0) return std::nullopt;
    for (const auto& c : compilers)
        if (auto v = version_after(c, "clang version ")) return OriginalLinker{LinkerKind::lld, (*v)[0], (*v)[1], (*v)[2]};
    return std::nullopt;
}

LinkerFit linker_fit(const std::optional<OriginalLinker>& original, LinkerKind kind, std::string_view version) {
    LinkerFit fit;
    fit.original = original;
    fit.kind = kind;
    fit.version = std::string(version);
    const auto v = version_after(version, kind == LinkerKind::lld ? "LLD " : "Version ");
    const std::string mine = !v ? std::string(kind == LinkerKind::lld ? "lld-link" : "link.exe") + " (version unknown)"
                             : kind == LinkerKind::lld ? std::format("lld-link of LLVM {}.{}.{}", (*v)[0], (*v)[1], (*v)[2])
                                                       : std::format("link.exe {}.{:02}.{}", (*v)[0], (*v)[1], (*v)[2]);
    if (!original) {
        fit.text = std::format("linked by {}; which linker made the image is not known", mine);
        return fit;
    }
    if (original->kind != kind) {
        fit.same = false;
        fit.text = std::format("the image was made by {}, the relink by {}: another linker lays the image out differently", original->text(), mine);
        return fit;
    }
    if (!v) {
        fit.text = std::format("the image was made by {}; the relink's linker did not say its version", original->text());
        return fit;
    }
    fit.same = original->major == (*v)[0] && original->minor == (*v)[1] && original->build == (*v)[2];
    fit.text = *fit.same ? std::format("linked by {}, as the image was", mine)
                         : std::format("the image was made by {}, the relink by {}: another version can lay the image out differently",
                                       original->text(), mine);
    return fit;
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

std::vector<std::string> image_link_flags(const pe::Image& image, std::string_view entry, std::optional<bool> safe_seh_table) {
    std::vector<std::string> f;
    const auto& oh = image.optional_header();
    const bool x64 = image.machine() == pe::machine::amd64;
    const u16 dll = image.dll_characteristics();
    const u16 file = image.characteristics();
    f.push_back("/nologo");
    f.push_back(x64 ? "/machine:x64" : "/machine:x86");
    if (image.is_dll()) f.push_back("/dll");
    const char* subsystem = subsystem_name(image.subsystem());
    if (subsystem) f.push_back(std::format("/subsystem:{},{}", subsystem, version(oh.subsystem_major, oh.subsystem_minor)));
    // Both linkers take the OS version from the subsystem's (6.0 without one); link.exe documents no
    // /OSVERSION, so it is only passed for an image whose OS version is another.
    const bool os_follows = subsystem ? oh.os_major == oh.subsystem_major && oh.os_minor == oh.subsystem_minor : oh.os_major == 6 && oh.os_minor == 0;
    if (!os_follows) f.push_back("/osversion:" + version(oh.os_major, oh.os_minor));
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
    if (!x64 && !(dll & 0x0400) && !safe_seh_table.value_or(!image.safe_seh_handlers().empty())) f.push_back("/safeseh:no");
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
