#include "matching/match.hpp"

#include "core/fs.hpp"

namespace decomp::matching {

Result<CandidateResult> compile_and_diff(const Program& program, const MatchSetup& setup, u64 va, const std::string& source,
                                         const std::string& candidate_symbol) {
    CandidateResult out;
    Compiler compiler(setup.toolchain, setup.work_dir, setup.cache_dir);
    CompileRequest req;
    req.source = source;
    req.flags = setup.flags;
    req.include_dirs = setup.include_dirs;
    TRY_ASSIGN(out.compile, compiler.compile(req));
    if (!out.compile.ok) {
        out.diff_error = out.compile.timed_out ? "the compiler timed out" : "compilation failed";
        return out;
    }
    auto obj = coff::Object::parse(out.compile.object_data);
    if (!obj) {
        out.diff_error = "cannot read the compiled object: " + obj.error().message;
        return out;
    }
    auto diff = diff_function(program, va, *obj, candidate_symbol);
    if (!diff) {
        out.diff_error = diff.error().message;
        return out;
    }
    out.diff = std::move(*diff);
    return out;
}

} // namespace decomp::matching
