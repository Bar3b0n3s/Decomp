#pragma once

// The Dashboard's other numbers (docs/ui.md "Dashboard"): the target's identity, the spend summary for
// the shown run and for all runs, and the recent-activity feed. Progress, its history and the treemap
// are progress.hpp and treemap.hpp. Pure; the identity reads the loaded image only.

#include "analysis/program.hpp"
#include "events/run_state.hpp"
#include "formats/pe.hpp"
#include "project/project.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/cost.hpp"
#include "viewmodel/run_history.hpp"

#include <string>
#include <vector>

namespace decomp::vm {

// One Rich-header entry with what it says about the toolchain.
struct RichBuild {
    enum class Role : u8 { compiler, linker, other };
    u16 product_id = 0;
    u16 build = 0;
    u32 count = 0;
    std::string description;  // pe::describe_rich_product(), or "product 0x00ff" when unknown
    Role role = Role::other;
};
// Compilers first, then linkers, then the rest (each in header order).
std::vector<RichBuild> rich_builds(const std::vector<pe::RichEntry>& entries);

struct TargetIdentity {
    std::string path;     // the image file (UTF-8)
    u64 file_size = 0;
    std::string sha1;     // of the image file
    std::string expected_sha1;  // decomp.json; empty when none is recorded
    bool sha1_ok = true;        // matches, or nothing recorded
    std::string format;         // "PE32" or "PE32+"
    std::string arch;           // "x86", "x64"
    bool dll = false;
    u64 image_base = 0;
    u64 entry_point = 0;        // 0: none
    std::string entry_name;     // the symbol at the entry point, when there is one
    std::string linker_version; // from the optional header, "14.00"
    std::vector<RichBuild> rich;
    PdbStatus pdb = PdbStatus::absent;
    std::string pdb_path;       // the PDB loaded or found (empty: none)
    std::string pdb_detail;     // why a PDB was ignored
    bool has_codeview = false;  // the image names a PDB (RSDS or NB10)
    std::string codeview_path;  // the PDB path the image records
    std::string guid;           // "{...}", RSDS only
    u32 age = 0;
};
// `status` from Project::target_status() (the workspace keeps it); without it the SHA-1 is computed
// here and counts as unverified-but-ok.
TargetIdentity target_identity(const Program& program, const project::TargetStatus* status);
// "matching GUID and age", "mismatch: ignored", "unsupported format (PDB 2.0?): ignored", "absent".
std::string pdb_status_text(PdbStatus status);

// Spend of a run, or of every run.
struct SpendSummary {
    double usd = 0;
    events::TokenUsage usage;
    usize matched = 0;
    usize runs = 0;

    double cache_hit_rate() const;  // cache reads / all prompt tokens, 0..1
    double usd_per_match() const { return matched ? usd / static_cast<double>(matched) : 0.0; }
};
// The shown run's totals, from its snapshot (current every frame, like the top bar).
SpendSummary run_spend(const events::RunStateData& run);
// A group of runs as cost.hpp counts it (all runs: cost_report().total), so the Dashboard and the Cost
// view agree.
SpendSummary slice_spend(const CostSlice& slice);

// The feed of notable outcomes and errors.
enum class ActivityKind : u8 { matched, gave_up, refused, error };
std::string_view to_string(ActivityKind kind);  // "matched", "gave up", "refused", "error"

struct ActivityItem {
    TimePoint time{};
    ActivityKind kind = ActivityKind::matched;
    u64 va = 0;              // 0 when the error belongs to no function
    std::string function;    // readable name
    std::string session;     // when known
    std::string run;         // run id
    std::string detail;      // error message, refusal detail
};
// Newest first, at most `limit`: the shown run's finished sessions (matched, gave up, refused, error)
// and its error records (API errors, tool and compiler failures, logged errors), then the outcomes the
// other runs' summaries record (timed at the end of their run). O(sessions + errors + recorded
// functions) plus a sort.
std::vector<ActivityItem> recent_activity(const events::RunStateData* shown, const std::vector<RunRecord>& runs, usize limit = 100);

} // namespace decomp::vm
