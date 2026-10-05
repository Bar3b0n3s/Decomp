#pragma once

// Exception-handling tables (docs/architecture.md#function-discovery): what MSVC and compatible
// compilers record about the code that only the exception dispatcher runs.
//
// On x86 a function registers a handler at entry (in fs:[0]). With C++ exception handling (/GX, /EHs)
// the handler is a stub (`__ehhandler$f`) that loads the function's FuncInfo and jumps to
// __CxxFrameHandler; the FuncInfo lists the catch blocks of each try block and the code that destroys
// objects while unwinding. With structured exception handling the handler is _except_handler3 (VC6
// on) or _except_handler4 (Visual Studio 2005 on), given a scope table that lists each __try's filter
// and handler (its __except block, or its __finally block). That code belongs to the function, though
// none of its flow reaches it. x64 FuncInfo tables have the same layout with image-relative addresses.

#include "arch/x86/decoder.hpp"
#include "formats/image.hpp"

#include <optional>
#include <vector>

namespace decomp {

// FuncInfo magic numbers: VC6 and Visual C++ .NET 2002; 2003 (an exception specification list);
// 2005 and later (flags).
constexpr u32 kFuncInfoMagic1 = 0x19930520, kFuncInfoMagic2 = 0x19930521, kFuncInfoMagic3 = 0x19930522;

struct CxxFuncInfo {
    u64 va = 0;
    u32 magic = 0;
    i32 max_state = 0;
    u32 try_blocks = 0;
    std::vector<u64> unwind_actions;  // code that destroys objects while unwinding (unwind funclets)
    std::vector<u64> catch_blocks;    // the handlers of the try blocks
};

// The FuncInfo at `va`, when the data there is one: the magic number, counts in range, and every
// table and code address it holds inside the image.
std::optional<CxxFuncInfo> read_cxx_funcinfo(const BinaryImage& image, u64 va);

// The FuncInfo an x86 handler stub loads: `mov eax, offset FuncInfo` (after a cookie check in newer
// compilers' stubs), then a jump to __CxxFrameHandler.
std::optional<u64> cxx_stub_funcinfo(const BinaryImage& image, const x86::Decoder& decoder, u64 stub);

struct ScopeEntry {
    i32 enclosing = -1;  // the enclosing __try's entry; -1 (_except_handler3) or -2 (_except_handler4) for none
    u64 filter = 0;      // the filter expression's code; 0 for a __finally
    u64 handler = 0;     // the __except block, or the __finally block
};

struct ScopeTable {
    u64 va = 0;
    bool cookies = false;  // _except_handler4's layout: 16 bytes of security cookie offsets come first
    std::vector<ScopeEntry> entries;
};

// The x86 scope table at `va`, in either layout, when the data there is one. Its length is not
// recorded: entries are read while they look like entries (at most 64).
std::optional<ScopeTable> read_scope_table(const BinaryImage& image, u64 va);

} // namespace decomp
