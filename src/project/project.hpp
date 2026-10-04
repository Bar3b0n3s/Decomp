#pragma once

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <map>
#include <optional>
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

// A decomp project directory: decomp.json, symbols.txt, include/, src/functions/, .decomp/.
class Project {
public:
    static constexpr const char* kConfigFile = "decomp.json";
    static constexpr const char* kSymbolsFile = "symbols.txt";

    // Searches `start` (or the current directory when empty) and its parents for decomp.json.
    static Result<Project> find(const std::string& start = {});
    static Result<Project> load(const std::filesystem::path& root);
    // Creates a project for `binary` in `root`, importing its symbols into symbols.txt.
    static Result<Project> init(const std::filesystem::path& root, const std::filesystem::path& binary,
                                const std::optional<std::filesystem::path>& pdb, const std::string& toolchain);

    const std::filesystem::path& root() const { return root_; }
    const Config& config() const { return config_; }
    Config& config() { return config_; }
    Result<void> save_config() const;

    std::filesystem::path target_path() const { return root_ / config_.target; }
    std::vector<std::filesystem::path> include_paths() const;

    // Loads the target and applies symbols.txt on top of the derived symbols.
    Result<Program> open_program() const;
    Result<void> save_symbols(const SymbolDb& symbols) const;

    // Per-function state, keyed by address.
    FunctionInfo function_info(u64 va) const;
    const std::map<u64, FunctionInfo>& function_infos() const { return functions_; }
    Result<void> update_function(u64 va, const FunctionInfo& info);

    // Working data for one function (attempts, best source, notes).
    std::filesystem::path function_dir(const Symbol& fn) const;
    std::filesystem::path matched_source_path(const Symbol& fn) const;
    Result<void> record_attempt(const Symbol& fn, const Json& attempt) const;
    std::vector<Json> attempts(const Symbol& fn) const;
    std::optional<std::string> best_source(const Symbol& fn) const;
    Result<void> save_best_source(const Symbol& fn, const std::string& source) const;
    std::string notes(const Symbol& fn) const;
    Result<void> append_note(const Symbol& fn, const std::string& note) const;
    Result<void> write_matched_source(const Symbol& fn, const std::string& source) const;

    std::filesystem::path runs_dir() const { return root_ / ".decomp" / "runs"; }
    std::filesystem::path build_dir() const { return root_ / ".decomp" / "build"; }
    std::filesystem::path cache_dir() const { return root_ / ".decomp" / "cache"; }

private:
    Result<void> load_symbols_file();
    std::filesystem::path root_;
    Config config_;
    std::vector<Symbol> symbol_overrides_;  // parsed symbols.txt
    std::map<u64, FunctionInfo> functions_;
};

// "Player::Hit" at 0x401000 -> "Player__Hit_401000" (stable, filesystem-safe, unique per address).
std::string safe_function_name(const Symbol& fn);

// symbols.txt line codec (exposed for tests).
std::string format_symbol_line(const Symbol& s, const FunctionInfo* info);
Result<std::pair<Symbol, std::optional<FunctionInfo>>> parse_symbol_line(std::string_view line);

} // namespace decomp::project
