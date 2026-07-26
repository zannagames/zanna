//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Provides the implementation of the TreeView widget used throughout the Zanna
// terminal UI.  The widget renders a hierarchical set of nodes, supports
// keyboard-driven expansion and collapse, and exposes helpers for retrieving the
// currently selected node.  Rendering is deliberately lightweight so large
// trees remain responsive: visible nodes are cached in a linear array that is
// rebuilt only when expansion state changes.
//
// Invariants:
//   * The `visible_` cache mirrors the depth-first traversal of expanded nodes
//     and is refreshed whenever expansion toggles to keep painting deterministic.
//   * The cursor always references a valid element inside `visible_` when the
//     tree is non-empty, clamping automatically during rebuilds.
// Ownership:
//   * TreeView owns the root node collection but borrows the theme reference so
//     callers can maintain a shared style palette.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the TreeView widget for hierarchical navigation.
/// @details Functions in this unit manage visible node caches, handle keyboard
///          events for traversal, and render the textual representation into a
///          screen buffer.  The widget focuses on behaviour; presentation is
///          derived entirely from the injected theme object.

#include "tui/widgets/tree_view.hpp"
#include "tui/render/screen.hpp"

#include <algorithm>

namespace zanna::tui::widgets {

/// @brief Construct a tree node with the provided label string.
/// @details Nodes start collapsed with no children and no parent reference.
///          The label is stored by value to guarantee stable data for rendering.
/// @param lbl Display label moved into the node.
TreeNode::TreeNode(std::string lbl) : label(std::move(lbl)) {}

/// @brief Append a child node and wire its parent pointer.
/// @details Ownership of the child transfers to the current node.  The helper
///          patches the child's `parent` field before storing it so upward
///          navigation and rebuilds can track ancestry.
/// @param child Node to adopt as the final child.
/// @return Raw pointer to the stored child to aid fluent tree construction.
TreeNode *TreeNode::add(std::unique_ptr<TreeNode> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

/// @brief Build a tree view using the supplied root nodes and theme.
/// @details The constructor takes ownership of @p roots, stores the borrowed
///          theme reference, and immediately calls @ref rebuild() to populate
///          the visible cache.  This ensures the widget paints valid content on
///          the very first frame.
/// @param roots Root-node collection whose ownership transfers to the view.
/// @param theme Borrowed theme that must outlive the tree view.
TreeView::TreeView(std::vector<std::unique_ptr<TreeNode>> roots, const style::Theme &theme)
    : roots_(std::move(roots)), theme_(theme) {
    rebuild();
}

/// @brief Tree views require focus to drive expansion and navigation via keyboard.
/// @return Always true so focus-based navigation can interact with the widget.
bool TreeView::wantsFocus() const {
    return true;
}

/// @brief Render the visible portion of the tree into a screen buffer.
/// @details Iterates over cached visible nodes until either the buffer height or
///          the visible list is exhausted.  For each node the routine paints a
///          cursor marker, indentation derived from @ref depth(), an expansion
///          glyph (`+` or `-`), and the node label truncated to the available
///          width.  Styling uses the accent role for markers and the normal role
///          for text, keeping selection obvious while retaining theme control.
/// @param sb Screen buffer receiving the rendered characters.
void TreeView::paint(render::ScreenBuffer &sb) {
    const auto &sel = theme_.style(style::Role::Accent);
    const auto &nor = theme_.style(style::Role::Normal);
    for (std::size_t i = 0; i < visible_.size() && static_cast<int>(i) < rect_.h; ++i) {
        auto *node = visible_[i];
        int y = rect_.y + static_cast<int>(i);
        int x = rect_.x;
        auto &csel = sb.at(y, x);
        csel.ch = (cursor_ == static_cast<int>(i)) ? U'>' : U' ';
        csel.style = sel;
        x += 1;
        int d = depth(node);
        for (int k = 0; k < d * 2 && k + x < rect_.x + rect_.w; ++k) {
            auto &cell = sb.at(y, x + k);
            cell.ch = U' ';
            cell.style = nor;
        }
        x += d * 2;
        if (x < rect_.x + rect_.w) {
            auto &mark = sb.at(y, x);
            mark.ch = node->children.empty() ? U' ' : (node->expanded ? U'-' : U'+');
            mark.style = sel;
            ++x;
        }
        int maxw = rect_.x + rect_.w - x;
        for (int j = 0; j < maxw; ++j) {
            auto &cell = sb.at(y, x + j);
            if (j < static_cast<int>(node->label.size())) {
                cell.ch = static_cast<char32_t>(node->label[j]);
            } else {
                cell.ch = U' ';
            }
            cell.style = nor;
        }
    }
}

/// @brief Handle keyboard navigation and expansion shortcuts.
/// @details Supports the following bindings:
///          * Up/Down move the cursor within the visible node list.
///          * Right expands the current node when it contains collapsed children.
///          * Left collapses the current node or moves to the parent when
///            already collapsed.
///          * Enter toggles expansion when the node has children.
///          Rebuilds are triggered whenever expansion state changes so painting
///          reflects the latest structure.
/// @param ev UI event containing the key input.
/// @return True when the event was consumed and state potentially changed.
bool TreeView::onEvent(const ui::Event &ev) {
    if (visible_.empty()) {
        return false;
    }
    const auto &k = ev.key;
    if (k.code == term::KeyEvent::Code::Up) {
        if (cursor_ > 0) {
            --cursor_;
        }
        return true;
    }
    if (k.code == term::KeyEvent::Code::Down) {
        if (cursor_ + 1 < static_cast<int>(visible_.size())) {
            ++cursor_;
        }
        return true;
    }
    auto *node = visible_[cursor_];
    if (k.code == term::KeyEvent::Code::Right) {
        if (!node->children.empty() && !node->expanded) {
            node->expanded = true;
            rebuild();
        }
        return true;
    }
    if (k.code == term::KeyEvent::Code::Left) {
        if (node->expanded) {
            node->expanded = false;
            rebuild();
        } else if (node->parent) {
            auto *p = node->parent;
            auto it = std::find(visible_.begin(), visible_.end(), p);
            if (it != visible_.end()) {
                cursor_ = static_cast<int>(it - visible_.begin());
            }
        }
        return true;
    }
    if (k.code == term::KeyEvent::Code::Enter) {
        if (!node->children.empty()) {
            node->expanded = !node->expanded;
            rebuild();
        }
        return true;
    }
    return false;
}

/// @brief Access the node currently selected by the cursor.
/// @details Returns `nullptr` when the tree has no visible nodes.  The pointer
///          remains valid as long as the tree structure is unchanged.
/// @return Pointer to the active node or nullptr when no selection exists.
TreeNode *TreeView::current() const {
    if (visible_.empty()) {
        return nullptr;
    }
    return visible_[cursor_];
}

/// @brief Recompute the linear list of visible nodes after structural changes.
/// @details Performs a manual depth-first traversal using a stack to avoid
///          recursion and push children in reverse order so the resulting array
///          preserves natural left-to-right iteration.  The cursor is clamped to
///          the new bounds to maintain a valid selection.  This method is called
///          eagerly from the constructor and whenever expansion state toggles.
void TreeView::rebuild() {
    visible_.clear();
    std::vector<TreeNode *> stack;
    for (auto &r : roots_) {
        stack.push_back(r.get());
        while (!stack.empty()) {
            TreeNode *n = stack.back();
            stack.pop_back();
            visible_.push_back(n);
            if (n->expanded) {
                for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
                    stack.push_back(it->get());
                }
            }
        }
    }
    if (cursor_ >= static_cast<int>(visible_.size())) {
        cursor_ = static_cast<int>(visible_.size()) - 1;
    }
    if (cursor_ < 0) {
        cursor_ = 0;
    }
}

/// @brief Compute the depth of a node relative to the root set.
/// @details Walks parent pointers until reaching a root, counting the number of
///          hops on the way.  Used exclusively for indentation when painting.
/// @param n Node whose depth should be calculated.
/// @return Zero for root nodes, otherwise the ancestor count.
int TreeView::depth(TreeNode *n) {
    int d = 0;
    while (n->parent) {
        n = n->parent;
        ++d;
    }
    return d;
}

} // namespace zanna::tui::widgets
