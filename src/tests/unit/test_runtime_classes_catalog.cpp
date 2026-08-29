//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_runtime_classes_catalog.cpp
// Purpose: Smoke test for runtime class catalog exposing Zanna.String.
// Key invariants:
//   - Public runtime classes expose authored documentation and canonical method
//     signatures through the generated registry catalog.
// Ownership/Lifetime: Catalog descriptors are process-lifetime immutable data.
// Links: docs/il/il-guide.md#reference, src/il/runtime/defs/classes/
//
//===----------------------------------------------------------------------===//

#include "il/runtime/classes/RuntimeClasses.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace {

const il::runtime::RuntimeClass *findClass(std::string_view qname) {
    for (const auto &cls : il::runtime::runtimeClassCatalog()) {
        if (std::string_view(cls.qname) == qname)
            return &cls;
    }
    return nullptr;
}

bool hasMethod(const il::runtime::RuntimeClass &cls,
               std::string_view name,
               std::string_view signature) {
    for (const auto &method : cls.methods) {
        if (std::string_view(method.name) == name &&
            std::string_view(method.signature) == signature)
            return true;
    }
    return false;
}

bool hasProperty(const il::runtime::RuntimeClass &cls,
                 std::string_view name,
                 std::string_view type) {
    for (const auto &prop : cls.properties) {
        if (std::string_view(prop.name) == name && std::string_view(prop.type) == type)
            return true;
    }
    return false;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

/// @brief Assert that a static typed-constant class exposes exactly the requested i64 properties.
/// @details Exact property counts catch both missing constants and accidental unrelated members;
///          per-name checks also validate the public scalar type used by both language frontends.
/// @param qname Fully qualified runtime class name.
/// @param names Complete ordered-independent set of expected read-only property names.
void assertI64ConstantClass(std::string_view qname, std::initializer_list<std::string_view> names) {
    const il::runtime::RuntimeClass *cls = findClass(qname);
    assert(cls != nullptr && "typed GUI constant class missing from runtime catalog");
    assert(cls->properties.size() == names.size());
    assert(cls->methods.empty());
    for (std::string_view name : names)
        assert(hasProperty(*cls, name, "i64"));
}

} // namespace

int main() {
    const auto &cat = il::runtime::runtimeClassCatalog();
    assert(cat.size() >= 1);

    for (const auto &cls : cat) {
        assert(cls.summary != nullptr && *cls.summary != '\0' &&
               "runtime classes must have authored summaries");
        assert(cls.details != nullptr && *cls.details != '\0' &&
               "runtime classes must have authored details");
        for (const auto &method : cls.methods) {
            const std::string_view name(method.name);
            assert(!startsWith(name, "get_") &&
                   "accessors must be runtime properties, not methods");
            assert(!startsWith(name, "set_") &&
                   "accessors must be runtime properties, not methods");
        }
    }

    // Find Zanna.String in the catalog (order-independent)
    const il::runtime::RuntimeClass *stringCls = findClass("Zanna.String");

    assert(stringCls != nullptr && "Zanna.String not found in catalog");
    assert(std::string_view(stringCls->summary) ==
           "Provides immutable runtime string values and common text operations.");
    assert(std::string_view(stringCls->details).find("`Zanna.String`") != std::string_view::npos);
    assert(stringCls->properties.size() >= 2);

    // Find Length and IsEmpty properties (order-independent)
    bool hasLength = false, hasIsEmpty = false;
    for (const auto &prop : stringCls->properties) {
        if (std::string_view(prop.name) == std::string_view("Length"))
            hasLength = true;
        if (std::string_view(prop.name) == std::string_view("IsEmpty"))
            hasIsEmpty = true;
    }
    assert(hasLength && "Zanna.String should have Length property");
    assert(hasIsEmpty && "Zanna.String should have IsEmpty property");

    const il::runtime::RuntimeClass *fileCls = findClass("Zanna.IO.File");
    assert(fileCls != nullptr && "Zanna.IO.File not found in catalog");
    assert(hasMethod(*fileCls, "SameFile", "i1(str,str)"));

    const il::runtime::RuntimeClass *pathCls = findClass("Zanna.IO.Path");
    assert(pathCls != nullptr && "Zanna.IO.Path not found in catalog");
    assert(hasMethod(*pathCls, "IsLink", "i1(str)"));
    assert(hasMethod(*pathCls, "SameEntry", "i1(str,str)"));

    const il::runtime::RuntimeClass *weakRefCls = findClass("Zanna.Memory.WeakRef");
    assert(weakRefCls != nullptr && "Zanna.Memory.WeakRef not found in catalog");
    assert(hasMethod(*weakRefCls, "New", "obj<Zanna.Memory.WeakRef>(obj)"));
    assert(hasMethod(*weakRefCls, "Get", "obj(obj)"));
    assert(hasMethod(*weakRefCls, "Free", "void(obj)"));
    assert(hasMethod(*weakRefCls, "Reset", "void(obj,obj)"));

    // Zanna.Core.ValueType was removed from the public catalog; value-type
    // registration lives only under Zanna.Runtime.Unsafe.
    assert(findClass("Zanna.Core.ValueType") == nullptr &&
           "Zanna.Core.ValueType must not be published");

    const il::runtime::RuntimeClass *systemClipboardCls = findClass("Zanna.System.Clipboard");
    assert(systemClipboardCls != nullptr && "Zanna.System.Clipboard not found in catalog");
    assert(hasMethod(*systemClipboardCls, "Get", "str()"));
    assert(hasMethod(*systemClipboardCls, "Set", "void(str)"));
    assert(hasMethod(*systemClipboardCls, "HasText", "i1()"));

    const il::runtime::RuntimeClass *processHandleCls =
        findClass("Zanna.System.Process.ProcessHandle");
    assert(processHandleCls != nullptr && "Zanna.System.Process.ProcessHandle not found");
    assert(hasMethod(*processHandleCls, "ReadOutputResult", "obj<Zanna.Collections.Map>()"));
    assert(hasMethod(
        *processHandleCls, "ReadOutputResultBounded", "obj<Zanna.Collections.Map>(i64,i64)"));

    const il::runtime::RuntimeClass *workspaceEditCls = findClass("Zanna.Workspace.Edit");
    assert(workspaceEditCls != nullptr && "Zanna.Workspace.Edit not found in catalog");
    assert(hasMethod(*workspaceEditCls, "ValidateInRoots", "obj<Zanna.Collections.Map>(obj,obj)"));
    assert(hasMethod(*workspaceEditCls, "ApplyInRoots", "obj<Zanna.Collections.Map>(obj,obj)"));

    const il::runtime::RuntimeClass *cameraCls = findClass("Zanna.Graphics.Camera");
    assert(cameraCls != nullptr && "Zanna.Graphics.Camera not found in catalog");
    assert(hasMethod(*cameraCls, "SmoothFollow", "void(i64,i64,i64)"));
    assert(hasMethod(*cameraCls, "SetDeadzone", "void(i64,i64)"));

    const il::runtime::RuntimeClass *bitmapFontCls = findClass("Zanna.Graphics.BitmapFont");
    assert(bitmapFontCls != nullptr && "Zanna.Graphics.BitmapFont not found in catalog");
    assert(hasMethod(*bitmapFontCls, "LoadBdf", "obj<Zanna.Graphics.BitmapFont>(str)"));
    assert(hasMethod(*bitmapFontCls, "LoadPsf", "obj<Zanna.Graphics.BitmapFont>(str)"));

    const il::runtime::RuntimeClass *spriteFontCls = findClass("Zanna.Graphics.BitmapFont");
    assert(spriteFontCls != nullptr && "Zanna.Graphics.BitmapFont not found in catalog");
    assert(hasMethod(*spriteFontCls, "LoadBdf", "obj<Zanna.Graphics.BitmapFont>(str)"));
    assert(hasMethod(*spriteFontCls, "LoadPsf", "obj<Zanna.Graphics.BitmapFont>(str)"));

    const il::runtime::RuntimeClass *guiAppCls = findClass("Zanna.GUI.App");
    assert(guiAppCls != nullptr && "Zanna.GUI.App not found in catalog");
    assert(hasMethod(*guiAppCls, "TryNew", "obj<Zanna.Result>(str,i64,i64)"));
    assert(hasMethod(*guiAppCls, "SetMinimumSize", "void(i64,i64)"));
    assert(hasMethod(*guiAppCls, "WasFileDropped", "i1()"));
    assert(hasMethod(*guiAppCls, "GetDroppedFileCount", "i64()"));
    assert(hasMethod(*guiAppCls, "GetDroppedFile", "str(i64)"));

    const il::runtime::RuntimeClass *guiSystemCls = findClass("Zanna.GUI.System");
    assert(guiSystemCls != nullptr && "Zanna.GUI.System not found in catalog");
    assert(hasMethod(*guiSystemCls, "IsAvailable", "i1()"));
    assert(hasMethod(*guiSystemCls, "GetUnavailableReason", "str()"));

    const il::runtime::RuntimeClass *guiWidgetCls = findClass("Zanna.GUI.Widget");
    assert(guiWidgetCls != nullptr && "Zanna.GUI.Widget not found in catalog");
    assert(hasMethod(*guiWidgetCls, "SetTooltip", "void(str)"));
    assert(hasMethod(*guiWidgetCls, "SetDraggable", "void(i1)"));
    assert(hasMethod(*guiWidgetCls, "SetDragData", "void(str,str)"));
    assert(hasMethod(*guiWidgetCls, "WasDropped", "i1()"));
    assert(hasMethod(*guiWidgetCls, "GetDropData", "str()"));

    const il::runtime::RuntimeClass *guiCodeEditorCls = findClass("Zanna.GUI.CodeEditor");
    assert(guiCodeEditorCls != nullptr && "Zanna.GUI.CodeEditor not found in catalog");
    assert(hasProperty(*guiCodeEditorCls, "Revision", "i64"));
    assert(hasMethod(*guiCodeEditorCls, "GetGutterClickSlot", "i64()"));
    assert(hasMethod(*guiCodeEditorCls, "GetShowLineNumbers", "i64()"));
    assert(hasMethod(*guiCodeEditorCls, "GetPerfStats", "obj<Zanna.Collections.Map>()"));

    const il::runtime::RuntimeClass *guiTestHarnessCls = findClass("Zanna.GUI.TestHarness");
    assert(guiTestHarnessCls != nullptr && "Zanna.GUI.TestHarness not found in catalog");
    assert(hasMethod(*guiTestHarnessCls, "AssertNonBlank", "i1(obj)"));
    assert(hasMethod(*guiTestHarnessCls, "BindApp", "i1(obj)"));
    assert(hasMethod(*guiTestHarnessCls, "UnbindApp", "void()"));
    assert(hasMethod(*guiTestHarnessCls, "DispatchPending", "i64()"));
    assert(hasMethod(*guiTestHarnessCls, "RenderFrame", "i1(f64)"));
    assert(hasMethod(
        *guiTestHarnessCls, "CapturePixels", "obj<Zanna.Graphics.Pixels>(i64,i64,i64,i64)"));
    assert(hasMethod(*guiTestHarnessCls, "CaptureHash", "str(i64,i64,i64,i64)"));
    assert(hasMethod(
        *guiTestHarnessCls, "CompareRegion", "obj<Zanna.Collections.Map>(obj,i64,i64,i64)"));
    assert(
        hasMethod(*guiTestHarnessCls, "GetAccessibilitySnapshot", "obj<Zanna.Collections.Map>()"));

    assertI64ConstantClass("Zanna.GUI.Align", {"Start", "Center", "End", "Stretch"});
    assertI64ConstantClass(
        "Zanna.GUI.Justify",
        {"Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly"});
    assertI64ConstantClass("Zanna.GUI.FlexDirection",
                           {"Row", "Column", "RowReverse", "ColumnReverse"});
    assertI64ConstantClass("Zanna.GUI.FlexWrap", {"NoWrap", "Wrap", "WrapReverse"});
    assertI64ConstantClass("Zanna.GUI.Dock", {"Left", "Top", "Right", "Bottom", "Fill"});
    assertI64ConstantClass("Zanna.GUI.ThemeMode", {"Dark", "Light", "System", "Custom"});
    assertI64ConstantClass(
        "Zanna.GUI.AccessibleRole",
        {"None",        "Application", "Window",    "Group",    "Label",    "Button",   "CheckBox",
         "RadioButton", "TextBox",     "SearchBox", "ComboBox", "List",     "ListItem", "Tree",
         "TreeItem",    "TabList",     "Tab",       "Table",    "Row",      "Cell",     "Slider",
         "ProgressBar", "Dialog",      "Alert",     "Menu",     "MenuItem", "ToolBar",  "StatusBar",
         "Image",       "Video",       "Link"});
    assertI64ConstantClass("Zanna.GUI.LiveRegionMode", {"Off", "Polite", "Assertive"});
    assertI64ConstantClass(
        "Zanna.GUI.DialogButtonRole",
        {"Normal", "Default", "Cancel", "Destructive", "Accept", "Reject", "Help"});
    assertI64ConstantClass("Zanna.GUI.DialogStatus",
                           {"Idle", "Open", "Accepted", "Cancelled", "Failed"});
    assertI64ConstantClass("Zanna.GUI.ImageFilter", {"Nearest", "Bilinear"});
    assertI64ConstantClass("Zanna.GUI.SortDirection", {"None", "Ascending", "Descending"});

    const il::runtime::RuntimeClass *guiToastCls = findClass("Zanna.GUI.Toast");
    assert(guiToastCls != nullptr && "Zanna.GUI.Toast not found in catalog");
    assert(hasMethod(*guiToastCls, "New", "obj(str,i64,i64)"));

    const il::runtime::RuntimeClass *guiFontCls = findClass("Zanna.GUI.Font");
    assert(guiFontCls != nullptr && "Zanna.GUI.Font not found in catalog");
    assert(hasMethod(*guiFontCls, "Load", "obj(str)"));

    const il::runtime::RuntimeClass *guiTreeViewCls = findClass("Zanna.GUI.TreeView");
    assert(guiTreeViewCls != nullptr && "Zanna.GUI.TreeView not found in catalog");
    assert(hasMethod(*guiTreeViewCls, "GetNodeAt", "obj(i64,i64)"));
    assert(hasMethod(*guiTreeViewCls, "SetVirtualModel", "i1(obj)"));
    assert(hasMethod(*guiTreeViewCls, "ClearVirtualModel", "void()"));

    const il::runtime::RuntimeClass *guiListBoxCls = findClass("Zanna.GUI.ListBox");
    assert(guiListBoxCls != nullptr && "Zanna.GUI.ListBox not found in catalog");
    assert(hasMethod(*guiListBoxCls, "GetSelectedData", "seq<str>()"));
    assert(hasMethod(*guiListBoxCls, "SetVirtualModel", "i1(obj)"));
    assert(hasMethod(*guiListBoxCls, "GetVisibleFirst", "i64()"));
    assert(hasMethod(*guiListBoxCls, "GetVisibleCount", "i64()"));

    const il::runtime::RuntimeClass *guiVirtualListCls = findClass("Zanna.GUI.VirtualList");
    assert(guiVirtualListCls != nullptr && "Zanna.GUI.VirtualList not found in catalog");
    assert(hasMethod(*guiVirtualListCls, "SetRowText", "void(i64,str)"));
    assert(hasMethod(*guiVirtualListCls, "Bind", "i1(obj)"));

    const il::runtime::RuntimeClass *guiVirtualTreeCls = findClass("Zanna.GUI.VirtualTree");
    assert(guiVirtualTreeCls != nullptr && "Zanna.GUI.VirtualTree not found in catalog");
    assert(
        hasMethod(*guiVirtualTreeCls, "VisibleRowsRange", "obj<Zanna.Collections.Seq>(i64,i64)"));
    assert(hasMethod(*guiVirtualTreeCls, "MoveNode", "i1(str,str)"));
    assert(hasMethod(*guiVirtualTreeCls, "Bind", "i1(obj)"));

    const il::runtime::RuntimeClass *guiColorSwatchCls = findClass("Zanna.GUI.ColorSwatch");
    assert(guiColorSwatchCls != nullptr && "Zanna.GUI.ColorSwatch not found in catalog");
    assert(hasMethod(*guiColorSwatchCls, "SetColor", "void(i64)"));
    assert(hasMethod(*guiColorSwatchCls, "WasChanged", "i1()"));

    const il::runtime::RuntimeClass *guiColorPaletteCls = findClass("Zanna.GUI.ColorPalette");
    assert(guiColorPaletteCls != nullptr && "Zanna.GUI.ColorPalette not found in catalog");
    assert(hasMethod(*guiColorPaletteCls, "RemoveColor", "i1(i64)"));
    assert(hasMethod(*guiColorPaletteCls, "GetColorAt", "i64(i64)"));

    const il::runtime::RuntimeClass *guiColorPickerCls = findClass("Zanna.GUI.ColorPicker");
    assert(guiColorPickerCls != nullptr && "Zanna.GUI.ColorPicker not found in catalog");
    assert(hasMethod(*guiColorPickerCls, "SetAlphaEnabled", "void(i1)"));
    assert(hasMethod(*guiColorPickerCls, "GetAlpha", "i64()"));

    const il::runtime::RuntimeClass *guiGridCls = findClass("Zanna.GUI.Grid");
    assert(guiGridCls != nullptr && "Zanna.GUI.Grid not found in catalog");
    assert(hasMethod(*guiGridCls, "SetVirtualCell", "void(i64,i64,str)"));
    assert(hasMethod(*guiGridCls, "SelectCell", "i1(i64,i64)"));
    assert(hasMethod(*guiGridCls, "CommitEdit", "i1(str)"));
    assert(hasMethod(*guiGridCls, "WasColumnResized", "i1()"));

    const il::runtime::RuntimeClass *guiMessageBoxCls = findClass("Zanna.GUI.MessageBox");
    assert(guiMessageBoxCls != nullptr && "Zanna.GUI.MessageBox not found in catalog");
    assert(hasMethod(*guiMessageBoxCls, "New", "obj(str,str,i64)"));
    assert(hasMethod(*guiMessageBoxCls, "PromptOption", "obj<Zanna.Option>(str,str)"));
    assert(hasMethod(*guiMessageBoxCls, "AddButtonWithRole", "void(str,i64,i64)"));
    assert(hasMethod(*guiMessageBoxCls, "SetCancelButton", "i1(i64)"));
    assert(hasMethod(*guiMessageBoxCls, "ShowAsync", "i1()"));
    assert(hasMethod(*guiMessageBoxCls, "GetStatus", "i64()"));

    const il::runtime::RuntimeClass *guiFileDialogCls = findClass("Zanna.GUI.FileDialog");
    assert(guiFileDialogCls != nullptr && "Zanna.GUI.FileDialog not found in catalog");
    assert(hasMethod(*guiFileDialogCls, "New", "obj(i64)"));
    assert(hasMethod(*guiFileDialogCls, "OpenOption", "obj<Zanna.Option>(str,str,str)"));
    assert(
        hasMethod(*guiFileDialogCls, "OpenMultipleSeq", "obj<Zanna.Collections.Seq>(str,str,str)"));
    assert(hasMethod(*guiFileDialogCls, "SetShowHidden", "void(i1)"));
    assert(hasMethod(*guiFileDialogCls, "ShowAsync", "i1()"));
    assert(hasMethod(*guiFileDialogCls, "GetPaths", "obj<Zanna.Collections.Seq>()"));

    const il::runtime::RuntimeClass *guiToolbarCls = findClass("Zanna.GUI.Toolbar");
    assert(guiToolbarCls != nullptr && "Zanna.GUI.Toolbar not found in catalog");
    assert(hasMethod(*guiToolbarCls, "NewVertical", "obj(obj)"));

    constexpr std::array<std::string_view, 43> graphics2DClasses = {
        "Zanna.Graphics.RenderTarget2D",
        "Zanna.Graphics.RenderTarget2D",
        "Zanna.Graphics.Texture2D",
        "Zanna.Graphics.GpuTexture2D",
        "Zanna.Graphics.Renderer2D",
        "Zanna.Graphics.Material2D",
        "Zanna.Graphics.Shader2D",
        "Zanna.Graphics.PostProcess2D",
        "Zanna.Graphics.Viewport2D",
        "Zanna.Graphics.Viewport2D",
        "Zanna.Graphics.TileSet2D",
        "Zanna.Graphics.TileLayer2D",
        "Zanna.Graphics.ObjectLayer2D",
        "Zanna.Graphics.AutoTile2D",
        "Zanna.Graphics.Path2D",
        "Zanna.Graphics.ShapeRenderer2D",
        "Zanna.Graphics.TextRenderer2D",
        "Zanna.Graphics.BitmapFont",
        "Zanna.Graphics.SdfFont",
        "Zanna.Graphics.NineSlice2D",
        "Zanna.Game.ParticleEmitter",
        "Zanna.Game.ParticleEmitter",
        "Zanna.Graphics.DebugDraw2D",
        "Zanna.Graphics.Transform2D",
        "Zanna.Graphics.Sampler2D",
        "Zanna.Graphics.BlendState2D",
        "Zanna.Graphics.SpriteRenderer2D",
        "Zanna.Graphics2D.TilemapRenderer2D",
        "Zanna.Graphics.TileChunkCache2D",
        "Zanna.Graphics.AnimationClip2D",
        "Zanna.Graphics.AnimatedSprite2D",
        "Zanna.Graphics.TextLayout2D",
        "Zanna.Graphics.BitmapFont",
        "Zanna.Graphics.RenderPass2D",
        "Zanna.Graphics.RenderGraph2D",
        "Zanna.Graphics.CollisionMask2D",
        "Zanna.Graphics.Hitbox2D",
        "Zanna.Graphics.Palette2D",
        "Zanna.Graphics.Gradient2D",
        "Zanna.Graphics.CameraRig2D",
        "Zanna.Graphics.TexturePackerAtlas",
        "Zanna.Graphics.AsepriteImporter",
        "Zanna.Graphics.TiledMapLoader",
    };

    for (std::string_view qname : graphics2DClasses) {
        bool found = false;
        for (const auto &cls : cat) {
            if (std::string_view(cls.qname) == qname) {
                found = true;
                break;
            }
        }
        assert(found && "Graphics2D runtime class missing from catalog");
    }

    return 0;
}
