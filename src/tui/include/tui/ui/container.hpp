//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares owning widget containers and equal-size stack layouts.
/// @details Container centralizes child ownership, paint delegation, and
///          two-phase layout; VStack and HStack divide available space along
///          the vertical or horizontal axis.
//
// VStack arranges children vertically (top to bottom), dividing the
// available height equally among children. HStack does the same
// horizontally (left to right) with available width.
//
// Key invariants:
//   - Children are owned exclusively via unique_ptr.
//   - Paint iterates children in insertion order (front-to-back).
//   - layoutChildren() is called after the container's own rect is set.
//
// Ownership: Container owns all children via unique_ptr. The container
// must outlive any external references to its children.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/ui/widget.hpp"

#include <memory>
#include <vector>

namespace zanna::tui::ui {
/// @brief Base container holding child widgets with ownership and layout delegation.
/// @details Provides the common infrastructure for composite widgets: child ownership
///          via unique_ptr, delegation of paint() to children in insertion order, and
///          a two-phase layout protocol where layout() stores the container's rect and
///          then calls the pure virtual layoutChildren() for concrete arrangement.
class Container : public Widget {
  public:
    /// @brief Transfer ownership of a child widget into the container.
    /// @details Appends the widget to the children list. The container assumes
    ///          exclusive ownership. Layout is not automatically recalculated;
    ///          the next layout() call will include the new child.
    /// @param child Widget to add. Must not be null.
    void addChild(std::unique_ptr<Widget> child);

    /// @brief Set the container's rectangle and trigger child layout.
    /// @details Stores the given rectangle, then delegates to layoutChildren()
    ///          which concrete subclasses implement to arrange children.
    /// @param r The rectangle assigned to this container by its parent.
    void layout(const Rect &r) override;

    /// @brief Paint all children into the screen buffer in insertion order.
    /// @param sb Screen buffer to paint into.
    void paint(render::ScreenBuffer &sb) override;

  protected:
    /// @brief Arrange child widgets within the container's rectangle.
    /// @details Called by layout() after the container's own rect_ is set.
    ///          Subclasses must implement this to assign rectangles to each child.
    virtual void layoutChildren() = 0;
    std::vector<std::unique_ptr<Widget>> children_{}; ///< Owned children in paint order.
};

/// @brief Vertical stack container that arranges children top-to-bottom.
/// @details Divides the container's height equally among all children.
///          Each child receives the full container width.
class VStack : public Container {
  protected:
    /// @brief Assign equal-height top-to-bottom rectangles to all children.
    void layoutChildren() override;
};

/// @brief Horizontal stack container that arranges children left-to-right.
/// @details Divides the container's width equally among all children.
///          Each child receives the full container height.
class HStack : public Container {
  protected:
    /// @brief Assign equal-width left-to-right rectangles to all children.
    void layoutChildren() override;
};

} // namespace zanna::tui::ui
