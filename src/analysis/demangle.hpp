#pragma once

#include "formats/image.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace decomp {

// Full demangling of MSVC ("?...") and Itanium ("_Z...") names, e.g.
// "?add@@YAHHH@Z" -> "int __cdecl add(int, int)". nullopt for undecorated or unknown schemes.
std::optional<std::string> demangle(std::string_view mangled);

// Strips C-level decoration: "__imp_" prefix, x86 "_name", stdcall "_name@8", fastcall "@name@8".
std::string undecorate(std::string_view name);

// The qualified entity name without signature: "?Hit@Player@@QAEXH@Z" -> "Player::Hit",
// "_ExitProcess@4" -> "ExitProcess", "add" -> "add".
std::string qualified_name(std::string_view name);

// Readable name: demangled when possible, else undecorated.
std::string display_name(std::string_view name);

// Two names refer to the same entity: identical, or one side is an undecorated/PDB name equal to the
// other's qualified name (static functions and data have no decorated name in PDB records).
bool names_equivalent(std::string_view a, std::string_view b);

// MSVC string literal symbol ("??_C@...") / floating-point constant symbol ("__real@...", "__xmm@...").
bool is_string_literal_symbol(std::string_view name);
bool is_float_constant_symbol(std::string_view name);

// A name compilers give to a place inside a function, not to a function: MSVC's line labels ($LN12@f)
// and catch blocks (__catch$?f@@YAXXZ$0), clang's catch continuations ($ehgcr_3_10) and, on x86, its
// catch and cleanup funclets (?catch$3@?0??f@@YAXXZ@4HA, ?dtor$2@...), which are part of their function
// there. (x64 funclets have unwind data of their own and are functions.)
bool is_code_label_symbol(std::string_view name, Arch arch);

} // namespace decomp
