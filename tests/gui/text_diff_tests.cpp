// The shared text and line-diff blocks.

#include "gui/views/text_diff.hpp"
#include "harness.hpp"

#include <doctest/doctest.h>

#include <format>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;

TEST_CASE("text diffs render changed, added and removed lines, all or only the changes") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    std::string before, after;
    for (int i = 1; i <= 200; ++i) {
        before += std::format("line {}\n", i);
        if (i == 20) after += "line twenty\n";       // changed
        else if (i != 120) after += std::format("line {}\n", i);  // 120 removed
    }
    after += "a new last line\n";  // added
    auto draw = [&](bool changes_only, std::string_view a, std::string_view b) {
        gui.frames(3, [&] {
            app.frame();
            ImGui::Begin("text diff test");
            ImGui::PushID("##diff_under_test");
            ImGui::GetStateStorage()->SetBool(ImGui::GetID("##changes_only"), changes_only);
            ImGui::PopID();
            draw_text_diff(app.context(), "##diff_under_test", a, b, 300);
            draw_text_block(app.context(), "##block", a, 100);
            ImGui::End();
        });
    };
    CHECK_NOTHROW(draw(false, before, after));
    CHECK_NOTHROW(draw(true, before, after));
    CHECK_NOTHROW(draw(true, before, before));  // identical: nothing to show but the note
    CHECK_NOTHROW(draw(false, "", after));      // a new file
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}
