#include "analysis/demangle.hpp"

#include <llvm/Demangle/Demangle.h>

#include <cctype>
#include <cstdlib>

namespace decomp {
namespace {

std::optional<std::string> take(char* p) {
    if (!p) return std::nullopt;
    std::string out(p);
    std::free(p);
    return out;
}

std::optional<std::string> ms_demangle(std::string_view mangled, llvm::MSDemangleFlags flags) {
    int status = 0;
    size_t n_read = 0;
    char* out = llvm::microsoftDemangle(mangled, &n_read, &status, flags);
    if (status != 0) {
        std::free(out);
        return std::nullopt;
    }
    return take(out);
}

// Cuts a demangled signature at its parameter list: "Player::Hit(int) const" -> "Player::Hit".
std::string strip_parameters(std::string_view s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        else if (c == '(' && depth == 0) {
            if (i >= 8 && s.substr(i - 8, 8) == "operator") continue;  // operator()
            if (i >= 1 && s[i - 1] == '`') continue;                   // `anonymous namespace'
            return std::string(s.substr(0, i));
        }
    }
    return std::string(s);
}

} // namespace

std::optional<std::string> demangle(std::string_view mangled) {
    if (mangled.starts_with("__imp_")) {
        auto inner = demangle(mangled.substr(6));
        if (inner) return "__imp_" + *inner;
        return std::nullopt;
    }
    if (mangled.starts_with('?')) return ms_demangle(mangled, llvm::MSDF_None);
    if (mangled.starts_with("_Z") || mangled.starts_with("__Z")) {
        auto s = mangled.starts_with("__Z") ? mangled.substr(1) : mangled;
        return take(llvm::itaniumDemangle(s, true));
    }
    return std::nullopt;
}

std::string undecorate(std::string_view name) {
    if (name.starts_with("__imp_")) name.remove_prefix(6);
    if (name.starts_with('?')) return std::string(name);
    if (name.starts_with('@')) {  // fastcall: @name@N
        name.remove_prefix(1);
        auto at = name.rfind('@');
        if (at != std::string_view::npos) name = name.substr(0, at);
        return std::string(name);
    }
    if (name.starts_with('_') && !name.starts_with("__")) name.remove_prefix(1);
    auto at = name.rfind('@');
    if (at != std::string_view::npos && at + 1 < name.size()) {
        bool digits = true;
        for (size_t i = at + 1; i < name.size(); ++i) digits = digits && std::isdigit(static_cast<unsigned char>(name[i]));
        if (digits) name = name.substr(0, at);  // stdcall: _name@N
    }
    return std::string(name);
}

std::string qualified_name(std::string_view name) {
    std::string_view base = name.starts_with("__imp_") ? name.substr(6) : name;
    if (base.starts_with('?')) {
        auto flags = static_cast<llvm::MSDemangleFlags>(llvm::MSDF_NoAccessSpecifier | llvm::MSDF_NoCallingConvention |
                                                        llvm::MSDF_NoReturnType | llvm::MSDF_NoMemberType |
                                                        llvm::MSDF_NoVariableType | llvm::MSDF_NoTagSpecifier);
        if (auto d = ms_demangle(base, flags)) return strip_parameters(*d);
        return std::string(base);
    }
    if (base.starts_with("_Z")) {
        llvm::ItaniumPartialDemangler partial;
        std::string tmp(base);
        if (!partial.partialDemangle(tmp.c_str())) {
            size_t n = 0;
            if (char* fn = partial.getFunctionName(nullptr, &n)) {
                std::string out(fn);
                std::free(fn);
                return out;
            }
        }
        if (auto d = demangle(base)) return strip_parameters(*d);
    }
    return undecorate(base);
}

std::string display_name(std::string_view name) {
    if (auto d = demangle(name)) return *d;
    return undecorate(name);
}

bool names_equivalent(std::string_view a, std::string_view b) {
    if (a == b) return true;
    return qualified_name(a) == qualified_name(b);
}

bool is_string_literal_symbol(std::string_view name) { return name.starts_with("??_C@"); }

bool is_float_constant_symbol(std::string_view name) {
    return name.starts_with("__real@") || name.starts_with("__xmm@") || name.starts_with("__ymm@");
}

bool is_code_label_symbol(std::string_view name, Arch arch) {
    if (name.starts_with('$') || name.starts_with("__catch$")) return true;
    return arch == Arch::x86 && (name.starts_with("?catch$") || name.starts_with("?dtor$") || name.starts_with("?cleanup$"));
}

} // namespace decomp
