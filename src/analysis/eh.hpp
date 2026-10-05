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
#include <span>
#include <string>
#include <vector>

namespace decomp {

// FuncInfo magic numbers: VC6 and Visual C++ .NET 2002; 2003 (an exception specification list);
// 2005 and later (flags).
constexpr u32 kFuncInfoMagic1 = 0x19930520, kFuncInfoMagic2 = 0x19930521, kFuncInfoMagic3 = 0x19930522;

// A catch block: the handler of a try block for one type.
struct CatchHandler {
    u32 try_block = 0;
    u32 adjectives = 0;  // 1 const, 2 volatile, 8 reference, 0x40 catch (...)
    u64 type = 0;        // the caught type's TypeDescriptor; 0 for catch (...)
    u64 code = 0;
};

struct CxxFuncInfo {
    u64 va = 0;
    u32 magic = 0;
    i32 max_state = 0;
    u32 try_blocks = 0;
    std::vector<u64> unwind_actions;  // code that destroys objects while unwinding (unwind funclets)
    std::vector<u64> catch_blocks;    // the handlers of the try blocks
    std::vector<CatchHandler> handlers;  // in try-block order
};

// The FuncInfo at `va`, when the data there is one: the magic number, counts in range, and every
// table and code address it holds inside the image.
std::optional<CxxFuncInfo> read_cxx_funcinfo(const BinaryImage& image, u64 va);

// The FuncInfo an x86 handler stub loads: `mov eax, offset FuncInfo` (after a cookie check in newer
// compilers' stubs), then a jump to __CxxFrameHandler.
std::optional<u64> cxx_stub_funcinfo(const BinaryImage& image, const x86::Decoder& decoder, u64 stub);

// The catch clause a handler stands for: "catch (const Failure&)", "catch (int)", "catch (...)".
std::string catch_clause(const BinaryImage& image, const CatchHandler& handler);

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

// The tables an x86 function's code registers: the FuncInfo of the handler stub it stores, the scope
// table it pushes. Nothing on x64 (the unwind data names a function's handler there).
struct FunctionEh {
    std::optional<CxxFuncInfo> cxx;
    u64 stub = 0;  // the handler stub that loads the FuncInfo
    std::optional<ScopeTable> seh;
    bool empty() const { return !cxx && !seh; }
};
FunctionEh function_eh(const BinaryImage& image, const x86::Decoder& decoder, std::span<const x86::Instruction> code);

} // namespace decomp
