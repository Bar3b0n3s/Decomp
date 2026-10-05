#pragma once

#include "analysis/program.hpp"
#include "core/file_lock.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace decomp::project {

enum class FunctionStatus : u8 { unstarted, in_progress, nonmatching, matched, refused, gave_up, skipped, library };
std::string_view to_string(FunctionStatus status);
std::optional<FunctionStatus> status_from_string(std::string_view s);

struct AgentSettings {
    std::string model = "claude-opus-5-5";
    std::string effort = "high";
    int max_turns = 40;
    double max_usd_per_function = 5.0;
    long long max_tokens_per_function = 0;  // 0 = unlimited
    int max_minutes_per_function = 30;
    bool fallbacks = true;
    double max_usd_per_run = 0;  // default run budget for `decomp run` and the GUI; 0 = unlimited
    int workers = 4;             // default parallel sessions of a run
    // Supervisor approval per agent action ("write_source": saving a verified match): auto, ask or deny.
    // Actions not listed are automatic.
    std::map<std::string, std::string> approvals;
};

struct Config {
    int version = 1;
    std::string target;        // path relative to the project root
    std::string target_sha1;
    std::string pdb;           // optional, relative
    std::string toolchain;     // name in the toolchain registry
    std::vector<std::string> flags;         // compiler flags for candidates
    std::vector<std::string> include_dirs;  // relative to the project root
    AgentSettings agent;

    Json to_json() const;
    static Result<Config> from_json(const Json& j);
};

struct FunctionInfo {
    FunctionStatus status = FunctionStatus::unstarted;
    double best_match = 0;  // percent
    int attempts = 0;
    double cost_usd = 0;
};

// Who changed something in the project, for the audit logs.
struct ChangeOrigin {
    SymbolSource source = SymbolSource::user;  // user or agent
    std::string session;                       // agent session (or empty)
    std::string reason;
};

// A change to one symbol. Unset fields keep their value; `remove` deletes the symbol.
struct SymbolEdit {
    u64 va = 0;
    std::optional<std::string> name = std::nullopt;
    std::optional<SymbolKind> kind = std::nullopt;
    std::optional<u32> size = std::nullopt;
    bool remove = false;
};

struct SymbolChange {
    u64 va = 0;
    std::optional<Symbol> before, after;
};

// What a recorded write is about: the function it was made for, its unit, every function it touches.
struct ChangeSubject {
    std::string function;
    u64 va = 0;
    std::string unit;
    std::vector<u64> functions;
};

// A file the project wrote, with what it replaced (kept in .decomp/blobs/ so it can be restored).
struct WriteReceipt {
    std::filesystem::path path;
    u64 size = 0;
    std::string sha1;
    std::optional<std::string> previous_sha1;
};

// The target binary as the project sees it.
struct TargetStatus {
    std::string expected_sha1, actual_sha1;
    bool sha1_ok = true;
    PdbStatus pdb = PdbStatus::absent;
    std::string pdb_detail;
};

// A decomp project directory: decomp.json, symbols.txt, include/, src/functions/, .decomp/.
class Project {
public:
    static constexpr const char* kConfigFile = "decomp.json";
    static constexpr const char* kSymbolsFile = "symbols.txt";

    // Searches `start` (or the current directory when empty) and its parents for decomp.json.
    static Result<Project> find(const std::string& start = {});
    static Result<Project> load(const std::filesystem::path& root);
    // Creates a project for `binary` in `root`, importing its symbols into symbols.txt: from the PDB,
    // the build's link map (`map`) and the functions the analysis finds.
    static Result<Project> init(const std::filesystem::path& root, const std::filesystem::path& binary,
                                const std::optional<std::filesystem::path>& pdb, const std::string& toolchain,
                                const std::optional<std::filesystem::path>& map = std::nullopt);
    Project();  // an empty project; use find/load/init

    const std::filesystem::path& root() const { return root_; }
    const Config& config() const { return config_; }
    Config& config() { return config_; }
    Result<void> save_config() const;

    std::filesystem::path target_path() const { return root_ / config_.target; }
    std::vector<std::filesystem::path> include_paths() const;

    // Loads the target and applies symbols.txt on top of the derived symbols. With `verify_target`, a
    // target whose SHA-1 differs from decomp.json is an error.
    Result<Program> open_program(bool verify_target = true) const;
    TargetStatus target_status(const Program& program) const;
    // Replaces symbols.txt with `symbols` (and the function states the project has).
    Result<void> save_symbols(const SymbolDb& symbols);

    // Per-function state, keyed by address. function_infos() is an immutable snapshot.
    FunctionInfo function_info(u64 va) const;
    std::shared_ptr<const std::map<u64, FunctionInfo>> function_infos() const;
    // Changes one function's state under the project lock (in-process mutex + .decomp/project.lock),
    // starting from the latest state on disk, and rewrites symbols.txt. Returns the new state.
    Result<FunctionInfo> modify_function(u64 va, const std::function<void(FunctionInfo&)>& change);
    Result<void> update_function(u64 va, const FunctionInfo& info);
    // The same for many functions at once (the GUI's bulk status changes): one lock, one reload and one
    // rewrite of symbols.txt for all of them.
    Result<void> modify_functions(std::span<const u64> vas, const std::function<void(u64, FunctionInfo&)>& change);
    // Renames, creates, resizes or removes a symbol; recorded in .decomp/symbols.log.jsonl. A renamed
    // function's history (.decomp/functions/<fn>/) and own source (src/functions/<fn>.cpp) move to its
    // new key.
    Result<SymbolChange> set_symbol(const SymbolEdit& edit, const ChangeOrigin& origin) const;
    // Sets the object file (the unit; obj= in symbols.txt) of each listed symbol that exists, an empty
    // name clearing it, starting from the latest symbols.txt. Returns how many changed. Not logged: units
    // are derived from the build's records (project/units.hpp).
    Result<usize> assign_objects(const std::map<u64, std::string>& objects);
    // The symbols as symbols.txt holds them.
    std::vector<Symbol> symbols() const;
    // Reloads symbols.txt when another process changed it. Returns true when it did.
    Result<bool> reload_if_changed();
    // Increases on every change to symbols or function state (in this process or picked up from disk).
    u64 version() const;
    // Increases when the symbols change (not on function state updates alone), and on every reload.
    u64 symbols_version() const;

    // Working data for one function (attempts, best source, notes).
    std::filesystem::path function_dir(const Symbol& fn) const;
    std::filesystem::path matched_source_path(const Symbol& fn) const;
    Result<void> record_attempt(const Symbol& fn, const Json& attempt) const;
    std::vector<Json> attempts(const Symbol& fn) const;
    std::optional<std::string> best_source(const Symbol& fn) const;
    Result<void> save_best_source(const Symbol& fn, const std::string& source) const;
    std::string notes(const Symbol& fn) const;
    Result<void> append_note(const Symbol& fn, const std::string& note) const;
    // Replaces the notes (the GUI's note editor); empty text leaves an empty notes.md.
    Result<void> save_notes(const Symbol& fn, const std::string& text) const;
    // Writes the verified source to src/functions/; the previous content is kept as a blob and the
    // write is recorded in .decomp/changes.jsonl.
    Result<WriteReceipt> write_matched_source(const Symbol& fn, const std::string& source, const ChangeOrigin& origin = {}) const;
    // Writes `content` to the project file at `relative` (under the project root), or removes the file
    // when `content` is nullopt. The replaced content is kept in .decomp/blobs/ and the change recorded
    // in .decomp/changes.jsonl, so revert_change() can undo it. With `expected`, the file must hold that
    // content (empty: the file is absent or empty) or the write fails with ErrorCode::conflict.
    Result<WriteReceipt> write_project_file(const std::filesystem::path& relative, const std::optional<std::string>& content,
                                            const ChangeOrigin& origin, const ChangeSubject& subject = {},
                                            const std::optional<std::string>& expected = std::nullopt) const;
    // Content kept for a replaced file (see WriteReceipt::previous_sha1).
    Result<std::string> read_blob(const std::string& sha1) const;
    // Undoes a write recorded in changes.jsonl (one of changes()): restores the content it replaced, or
    // removes the file when there was none. Refused when the file has changed since that write. The
    // revert is recorded in changes.jsonl; a function whose matched source is removed goes back to
    // nonmatching (its best source and attempts stay).
    Result<void> revert_change(const Json& change, const ChangeOrigin& origin);
    std::vector<Json> changes() const;  // .decomp/changes.jsonl, oldest first
    std::vector<Json> symbol_log() const;  // .decomp/symbols.log.jsonl, oldest first

    // One live run per project: held by the process that runs agent sessions. nullopt when another
    // process holds it.
    Result<std::optional<FileLock>> try_lock_active_run() const;

    std::filesystem::path runs_dir() const { return root_ / ".decomp" / "runs"; }
    std::filesystem::path build_dir() const { return root_ / ".decomp" / "build"; }
    std::filesystem::path cache_dir() const { return root_ / ".decomp" / "cache"; }
    std::filesystem::path blobs_dir() const { return root_ / ".decomp" / "blobs"; }

private:
    struct State;
    Result<void> load_symbols_file(State& state) const;
    Result<void> reload_locked(State& state) const;
    Result<void> write_symbols_locked(State& state) const;
    Result<FileLock> lock_project() const;

    std::filesystem::path root_;
    Config config_;
    std::shared_ptr<State> state_;  // shared by copies of this Project
};

// The program with the project's symbols applied, a new generation whenever they change
// (Project::symbols_version()): what a run gives each session it dispatches, so that later sessions see
// the symbols earlier ones named. Thread-safe; generations share the image and decoder.
class ProgramGenerations {
public:
    static Result<std::shared_ptr<ProgramGenerations>> open(const Project& project);
    std::shared_ptr<const Program> current();

private:
    explicit ProgramGenerations(const Project& project) : project_(project) {}
    Project project_;  // shares the caller's state
    std::unique_ptr<const Program> base_;  // the binary's own symbols
    std::mutex mutex_;
    u64 version_ = 0;
    std::shared_ptr<const Program> current_;
};

// "Player::Hit" at 0x401000 -> "Player__Hit_401000" (stable, filesystem-safe, unique per address).
std::string safe_function_name(const Symbol& fn);
// Whether write_project_file() and revert_change() may write a path: relative, inside the project once
// `..` is resolved, and not under .decomp/.
bool writable_project_path(const std::filesystem::path& relative);

// symbols.txt line codec (exposed for tests).
std::string format_symbol_line(const Symbol& s, const FunctionInfo* info);
Result<std::pair<Symbol, std::optional<FunctionInfo>>> parse_symbol_line(std::string_view line);

} // namespace decomp::project
