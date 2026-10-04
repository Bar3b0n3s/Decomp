// Exports land in the project's .decomp/exports/ under unique, file-name-safe names.

#include "core/fs.hpp"
#include "gui/export.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::gui;

TEST_CASE("exports are written under .decomp/exports with safe, unique names") {
    auto dir = fs::TempDir::create("decomp-export").value();
    const auto first = write_export(dir.path(), "progress report: x/y", "md", "# Progress\n");
    REQUIRE(first);
    CHECK(first->parent_path() == dir.path() / ".decomp" / "exports");
    const std::string name = fs::to_utf8(first->filename());
    CHECK(name.starts_with("progress_report_x_y-"));
    CHECK(name.ends_with(".md"));
    CHECK(fs::read_text(*first).value() == "# Progress\n");
    // A second export in the same second gets its own file.
    const auto second = write_export(dir.path(), "progress report: x/y", "md", "again");
    REQUIRE(second);
    CHECK(*second != *first);
    CHECK(fs::read_text(*first).value() == "# Progress\n");
    CHECK(fs::to_utf8(export_path(dir.path(), "", "csv").filename()).starts_with("export-"));
}
