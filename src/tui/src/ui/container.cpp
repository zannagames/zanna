//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// tui/src/ui/container.cpp
//
// Defines the core container widgets used by the terminal UI framework.  The
// Container base class manages child lifetimes, painting order, and layout
// delegation, while the @ref VStack and @ref HStack specialisations compute
// simple equal-sized column and row arrangements.  These building blocks power
// higher-level layouts throughout the sample applications.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements owning widget containers and equal-share stack layouts.
/// @details Container children are visited in insertion order for layout and
///          painting.  The vertical and horizontal stack implementations divide
///          the assigned rectangle along one axis and give any integer-division
///          remainder to the final child.

#include "tui/ui/container.hpp"
#include "tui/render/screen.hpp"

namespace zanna::tui::ui {
/// @brief Transfer ownership of a child widget into the container.
///
/// @details Containers own their children outright so they can drive layout
///          and painting lifecycles.  The method appends the child to the end of
///          the internal list, preserving insertion order for both layout and
///          paint traversals.  Passing @c nullptr is undefined and avoided by
///          callers.
/// @param child Widget whose ownership is transferred to this container.
void Container::addChild(std::unique_ptr<Widget> child) {
    children_.push_back(std::move(child));
}

/// @brief Update the container's rectangle and propagate layout to children.
///
/// @details The base class records the assigned geometry by delegating to
///          @ref Widget::layout and then invokes @ref layoutChildren, a virtual
///          hook that derived containers override to partition the space.
///          Keeping the hook centralised ensures every container obeys the
///          parent-provided rectangle while leaving distribution logic to the
///          subclass.
/// @param r Rectangle assigned by the parent layout.
void Container::layout(const Rect &r) {
    Widget::layout(r);
    layoutChildren();
}

/// @brief Paint all child widgets in insertion order onto the screen buffer.
///
/// @details Containers do not render their own visuals; instead they iterate
///          through the managed children and let each widget draw into the
///          shared @ref render::ScreenBuffer.  Iterating by reference avoids
///          copies and ensures that children paint using the rectangles computed
///          during layout.
/// @param sb Screen buffer that receives each child's rendered cells.
void Container::paint(render::ScreenBuffer &sb) {
    for (auto &ch : children_) {
        ch->paint(sb);
    }
}

/// @brief Evenly distribute the container height across all child widgets.
///
/// @details When the container is empty the method exits early.  Otherwise it
///          divides the available height equally and assigns the remainder to
///          the last child so the sum matches the parent rectangle exactly.
///          Children receive vertically stacked rectangles with full width and
///          contiguous y-coordinates, ensuring there are no gaps or overlaps.
void VStack::layoutChildren() {
    int count = static_cast<int>(children_.size());
    if (count == 0) {
        return;
    }
    int base = rect_.h / count;
    int rem = rect_.h - base * count;
    int y = rect_.y;
    for (int i = 0; i < count; ++i) {
        int h = base + (i == count - 1 ? rem : 0);
        Rect cr{rect_.x, y, rect_.w, h};
        children_[i]->layout(cr);
        y += h;
    }
}

/// @brief Evenly distribute the container width across all child widgets.
///
/// @details The algorithm mirrors @ref VStack::layoutChildren but works along
///          the horizontal axis.  Width is divided evenly, any rounding error is
///          assigned to the final child, and each child receives the full height
///          of the parent rectangle.  The approach guarantees deterministic
///          layout regardless of integer division quirks.
void HStack::layoutChildren() {
    int count = static_cast<int>(children_.size());
    if (count == 0) {
        return;
    }
    int base = rect_.w / count;
    int rem = rect_.w - base * count;
    int x = rect_.x;
    for (int i = 0; i < count; ++i) {
        int w = base + (i == count - 1 ? rem : 0);
        Rect cr{x, rect_.y, w, rect_.h};
        children_[i]->layout(cr);
        x += w;
    }
}

} // namespace zanna::tui::ui
