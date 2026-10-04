#pragma once

#include "core/process.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace decomp::test {

inline std::filesystem::path source_dir() { return std::filesystem::path(DECOMP_SOURCE_DIR); }
inline std::filesystem::path fixture(const std::filesystem::path& rel) { return source_dir() / "tests" / "fixtures" / rel; }

// Sets an environment variable for the lifetime of the object, then restores the previous value.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)), old_(get_env(name_)) { set(value); }
    ~ScopedEnv() {
        if (old_) set(*old_);
        else unset();
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    void unset() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    std::optional<std::string> old_;
};

} // namespace decomp::test
