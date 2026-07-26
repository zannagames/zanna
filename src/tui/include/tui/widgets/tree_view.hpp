//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares an expandable hierarchical tree widget and owning tree nodes.
/// @details TreeView flattens expanded subtrees into a visible navigation list,
///          paints depth-based indentation, and handles keyboard cursor and
///          expansion changes.
//
// Each TreeNode contains a label, child nodes, and expansion state.
// The TreeView renders visible nodes (expanded subtrees) with indentation
// proportional to depth and supports keyboard navigation (Up/Down arrows,
// Enter to toggle expansion).
//
// Key invariants:
//   - Root nodes are the top-level entries in the tree.
//   - Only expanded nodes' children are visible and navigable.
//   - The cursor index is always within the visible node list bounds.
//   - Parent pointers are maintained by the add() method.
//
// Ownership: TreeView owns root TreeNodes via unique_ptr. TreeNode owns
// its children via unique_ptr and stores a non-owning parent pointer.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tui/style/theme.hpp"
#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {

/// @brief Node in a hierarchical tree structure with label, children, and expansion state.
/// @details Each node contains a display label, a vector of child nodes owned via
///          unique_ptr, a non-owning parent pointer for traversal, and a flag indicating
///          whether the node's children are visible (expanded).
struct TreeNode {
    std::string label{}; ///< Owned display label.
    std::vector<std::unique_ptr<TreeNode>> children{}; ///< Owned child nodes.
    TreeNode *parent{nullptr}; ///< Non-owning parent pointer, null for roots.
    bool expanded{false}; ///< Whether direct children participate in the visible tree.

    /// @brief Construct a detached collapsed node.
    /// @param lbl Display label moved into the node.
    explicit TreeNode(std::string lbl);

    /// @brief Adopt a child and establish its parent pointer.
    /// @param child Node to append to owned children.
    /// @return Raw pointer to the adopted child.
    TreeNode *add(std::unique_ptr<TreeNode> child);
};

/// @brief Hierarchical tree display widget with keyboard navigation and expand/collapse.
/// @details Renders a tree of labeled nodes with indentation proportional to depth.
///          Supports Up/Down arrow navigation, Enter to toggle node expansion, and
///          Left/Right arrows to collapse/expand. Only expanded subtrees are visible.
class TreeView : public ui::Widget {
  public:
    /// @brief Construct with root nodes and theme.
    /// @param roots Owned top-level tree nodes.
    /// @param theme Borrowed render palette that must outlive the widget.
    TreeView(std::vector<std::unique_ptr<TreeNode>> roots, const style::Theme &theme);

    /// @brief Paint visible nodes.
    /// @param sb Screen buffer receiving indented tree rows.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle navigation and expansion keys.
    /// @param ev Input event to interpret.
    /// @return True if event consumed.
    bool onEvent(const ui::Event &ev) override;

    /// @brief Tree view wants focus for keyboard handling.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

    /// @brief Current node under cursor.
    /// @return Non-owning pointer to the active visible node, or nullptr.
    [[nodiscard]] TreeNode *current() const;

  private:
    std::vector<std::unique_ptr<TreeNode>> roots_{}; ///< Owned top-level nodes.
    const style::Theme &theme_;                      ///< Borrowed render palette.
    std::vector<TreeNode *> visible_{}; ///< Non-owning depth-first visible-node cache.
    int cursor_{0};                    ///< Index into @c visible_.

    /// @brief Rebuild the visible-node cache from expansion state and clamp the cursor.
    void rebuild();

    /// @brief Count parent links from a node to its root.
    /// @param n Node whose indentation depth is requested.
    /// @return Zero-based tree depth.
    static int depth(TreeNode *n);
};

} // namespace zanna::tui::widgets
