#pragma once

#include <filesystem>

namespace decomp::test {

inline std::filesystem::path source_dir() { return std::filesystem::path(DECOMP_SOURCE_DIR); }
inline std::filesystem::path fixture(const std::filesystem::path& rel) { return source_dir() / "tests" / "fixtures" / rel; }

} // namespace decomp::test
