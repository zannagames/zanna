//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/tests/test_list_tree.cpp
// Purpose: Test suite for this component.
// Key invariants: To be documented.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#include "tui/render/renderer.hpp"
#include "tui/render/screen.hpp"
#include "tui/style/theme.hpp"
#include "tui/term/term_io.hpp"
#include "tui/widgets/list_view.hpp"
#include "tui/widgets/tree_view.hpp"

#include "tests/TestHarness.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <vector>

using zanna::tui::render::Renderer;
using zanna::tui::render::ScreenBuffer;
using zanna::tui::style::Role;
using zanna::tui::style::Theme;
using zanna::tui::term::KeyEvent;
using zanna::tui::term::StringTermIO;
using zanna::tui::ui::Event;
using zanna::tui::widgets::ListView;
using zanna::tui::widgets::TreeNode;
using zanna::tui::widgets::TreeView;

TEST(TUI, ListTree) {
    Theme theme;
    ScreenBuffer sb;
    StringTermIO tio;
    Renderer r(tio, true);

    // ListView basic paint and selection
    ListView lv({"one", "two", "three"}, theme);
    lv.layout({0, 0, 8, 3});
    sb.resize(3, 8);
    sb.clear(theme.style(Role::Normal));
    lv.paint(sb);
    tio.clear();
    r.draw(sb);
    ASSERT_TRUE(tio.buffer().find('>') != std::string::npos);
    ASSERT_TRUE(tio.buffer().find("one") != std::string::npos);

    Event ev{};
    ev.key.code = KeyEvent::Code::Down;
    lv.onEvent(ev);
    ev.key.mods = KeyEvent::Shift;
    ev.key.code = KeyEvent::Code::Down;
    lv.onEvent(ev);
    auto sel = lv.selection();
    ASSERT_TRUE(sel.size() == 2 && sel[0] == 1 && sel[1] == 2);

    // Custom renderer outputs uppercase without prefix
    lv.setRenderer(
        [](ScreenBuffer &target, int row, const std::string &it, bool, const Theme &rowTheme) {
            for (int i = 0; i < static_cast<int>(it.size()); ++i) {
                auto &c = target.at(row, i);
                c.ch = static_cast<char32_t>(std::toupper(it[i]));
                c.style = rowTheme.style(Role::Normal);
            }
        });
    sb.clear(theme.style(Role::Normal));
    lv.paint(sb);
    tio.clear();
    r.draw(sb);
    ASSERT_TRUE(tio.buffer().find("ONE") != std::string::npos);

    // TreeView expand/collapse
    auto root = std::make_unique<TreeNode>("root");
    root->add(std::make_unique<TreeNode>("child1"));
    auto c2 = root->add(std::make_unique<TreeNode>("child2"));
    c2->add(std::make_unique<TreeNode>("grand"));
    std::vector<std::unique_ptr<TreeNode>> roots;
    roots.push_back(std::move(root));
    TreeView tv(std::move(roots), theme);
    tv.layout({0, 0, 12, 5});
    sb.resize(5, 12);
    sb.clear(theme.style(Role::Normal));
    tv.paint(sb);
    tio.clear();
    r.draw(sb);
    ASSERT_TRUE(tio.buffer().find('+') != std::string::npos);
    ASSERT_TRUE(tio.buffer().find("root") != std::string::npos);

    ev = {};
    ev.key.code = KeyEvent::Code::Enter;
    tv.onEvent(ev);
    sb.clear(theme.style(Role::Normal));
    tv.paint(sb);
    tio.clear();
    r.draw(sb);
    ASSERT_TRUE(tio.buffer().find('-') != std::string::npos);
    ASSERT_TRUE(tio.buffer().find("root") != std::string::npos);

    ev.key.code = KeyEvent::Code::Down;
    tv.onEvent(ev); // child1
    ev.key.code = KeyEvent::Code::Down;
    tv.onEvent(ev); // child2
    ev.key.code = KeyEvent::Code::Enter;
    tv.onEvent(ev); // expand child2
    sb.clear(theme.style(Role::Normal));
    tv.paint(sb);
    tio.clear();
    r.draw(sb);
    ASSERT_TRUE(tio.buffer().find("grand") != std::string::npos);

    ev.key.code = KeyEvent::Code::Left;
    tv.onEvent(ev); // collapse child2
    sb.clear(theme.style(Role::Normal));
    tv.paint(sb);
    tio.clear();
    r.draw(sb);
    ASSERT_EQ(tio.buffer().find("grand"), std::string::npos);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
