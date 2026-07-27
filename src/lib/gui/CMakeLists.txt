# Zanna GUI Library CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

include(GNUInstallDirs)

# GUI library sources
set(VG_GUI_FONT_SOURCES
        src/font/vg_ttf.c
        src/font/vg_raster.c
        src/font/vg_gsub.c
        src/font/vg_cache.c
        src/font/vg_font.c
        src/font/vg_canvas_integration.c
)

set(VG_GUI_CORE_SOURCES
        src/core/vg_widget.c
        src/core/vg_layout.c
        src/core/vg_event.c
        src/core/vg_grapheme.c
        src/core/vg_theme.c
        src/core/vg_draw.c
        src/core/vg_icon_vector.c
)

set(VG_GUI_WIDGET_SOURCES
        src/widgets/vg_label.c
        src/widgets/vg_button.c
        src/widgets/vg_textinput.c
        src/widgets/vg_checkbox.c
        src/widgets/vg_scrollview.c
        src/widgets/vg_listbox.c
        src/widgets/vg_dropdown.c
        src/widgets/vg_slider.c
        src/widgets/vg_progressbar.c
        src/widgets/vg_radiobutton.c
        src/widgets/vg_image.c
        src/widgets/vg_spinner.c
        src/widgets/vg_colorswatch.c
        src/widgets/vg_colorpalette.c
        src/widgets/vg_colorpicker.c
)

set(VG_GUI_IDE_WIDGET_SOURCES
        src/widgets/vg_treeview.c
        src/widgets/vg_tabbar.c
        src/widgets/vg_splitpane.c
        src/widgets/vg_menubar.c
        src/widgets/vg_codeeditor.c
        src/widgets/vg_statusbar.c
        src/widgets/vg_toolbar.c
        src/widgets/vg_dialog.c
        src/widgets/vg_filedialog.c
        src/widgets/vg_contextmenu.c
        src/widgets/vg_findreplacebar.c
        src/widgets/vg_tooltip.c
        src/widgets/vg_commandpalette.c
        src/widgets/vg_outputpane.c
        src/widgets/vg_breadcrumb.c
        src/widgets/vg_minimap.c
        src/widgets/vg_notification.c
        src/widgets/vg_floatingpanel.c
        src/widgets/vg_groupbox.c
        src/widgets/vg_datagrid.c
        src/widgets/vg_popuplist.c
)

if (WIN32)
    list(APPEND VG_GUI_IDE_WIDGET_SOURCES src/widgets/vg_filedialog_platform_win32.c)
else ()
    list(APPEND VG_GUI_IDE_WIDGET_SOURCES src/widgets/vg_filedialog_platform_posix.c)
endif ()

# Platform-specific native dialogs
if (APPLE)
    set(VG_GUI_NATIVE_DIALOG_SOURCES
            src/dialogs/vg_filedialog_native.m
    )
elseif (WIN32)
    set(VG_GUI_NATIVE_DIALOG_SOURCES
            src/dialogs/vg_filedialog_native_win32.c
    )
endif ()

# Phase 1 + Phase 2 + Phase 3 + Phase 5: Font engine + Widget core + Widgets + IDE Widgets
set(VG_GUI_SOURCES ${VG_GUI_FONT_SOURCES} ${VG_GUI_CORE_SOURCES} ${VG_GUI_WIDGET_SOURCES} ${VG_GUI_IDE_WIDGET_SOURCES} ${VG_GUI_NATIVE_DIALOG_SOURCES})

# Create GUI library
add_library(zannagui STATIC ${VG_GUI_SOURCES})

# Include directories
target_include_directories(zannagui
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/font
        ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
)

# Link against zannagfx
target_link_libraries(zannagui
        PUBLIC
        zannagfx
        zanna_text_core
)

# Native dialog + shell services on Windows
if (WIN32)
    target_link_libraries(zannagui PRIVATE ole32 shell32)
endif ()

# Link math library on Unix
if (UNIX AND NOT APPLE)
    target_link_libraries(zannagui PRIVATE m)
    # GNU feature test macros for Linux (strdup, FNM_CASEFOLD, etc.)
    target_compile_definitions(zannagui PRIVATE _GNU_SOURCE)
endif ()

# Link UniformTypeIdentifiers framework on macOS (for file dialogs)
if (APPLE)
    target_link_libraries(zannagui PRIVATE
            "-framework UniformTypeIdentifiers"
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework ImageIO"
    )
endif ()

# C11 standard
set_target_properties(zannagui PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
)

# Compiler warnings
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zannagui PRIVATE
            -Wall
            -Wextra
            -Wno-unused-parameter
    )
elseif (MSVC)
    target_compile_options(zannagui PRIVATE /utf-8 /FS)
endif ()

# Tests
if (ZANNA_BUILD_TESTING)
    add_executable(test_vg_font tests/test_font.c)
    target_link_libraries(test_vg_font zannagui)
    target_include_directories(test_vg_font PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_font COMMAND test_vg_font)
    set_tests_properties(test_vg_font PROPERTIES LABELS "gui")

    add_executable(test_vg_smooth_scroll tests/test_vg_smooth_scroll.c)
    target_link_libraries(test_vg_smooth_scroll zannagui)
    target_include_directories(test_vg_smooth_scroll PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_smooth_scroll COMMAND test_vg_smooth_scroll)
    set_tests_properties(test_vg_smooth_scroll PROPERTIES LABELS "gui" TIMEOUT 15)

    # GSUB ligature shaping against the shipped JetBrains Mono face.
    add_executable(test_vg_gsub tests/test_vg_gsub.c)
    target_link_libraries(test_vg_gsub zannagui)
    target_include_directories(test_vg_gsub PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(test_vg_gsub PRIVATE
            ZANNA_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/src/runtime/graphics/text/fonts/JetBrainsMono-Regular.ttf")
    add_test(NAME test_vg_gsub COMMAND test_vg_gsub)
    set_tests_properties(test_vg_gsub PROPERTIES LABELS "gui" TIMEOUT 30)

    add_executable(test_vg_widgets_new tests/test_vg_widgets_new.c)
    target_link_libraries(test_vg_widgets_new zannagui)
    target_include_directories(test_vg_widgets_new PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_widgets_new COMMAND test_vg_widgets_new)
    set_tests_properties(test_vg_widgets_new PROPERTIES LABELS "gui" TIMEOUT 15)

    add_executable(test_vg_tier1_fixes tests/test_vg_tier1_fixes.c)
    target_link_libraries(test_vg_tier1_fixes zannagui)
    target_include_directories(test_vg_tier1_fixes PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_tier1_fixes COMMAND test_vg_tier1_fixes)
    set_tests_properties(test_vg_tier1_fixes PROPERTIES LABELS "gui" TIMEOUT 15)

    add_executable(test_vg_tier2_fixes tests/test_vg_tier2_fixes.c)
    target_link_libraries(test_vg_tier2_fixes zannagui)
    target_include_directories(test_vg_tier2_fixes PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(test_vg_tier2_fixes PRIVATE
            ZANNA_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/src/runtime/graphics/text/fonts/JetBrainsMono-Regular.ttf")
    add_test(NAME test_vg_tier2_fixes COMMAND test_vg_tier2_fixes)
    set_tests_properties(test_vg_tier2_fixes PROPERTIES LABELS "gui" TIMEOUT 15)

    add_executable(test_vg_tier3_fixes tests/test_vg_tier3_fixes.c)
    target_link_libraries(test_vg_tier3_fixes zannagui)
    target_include_directories(test_vg_tier3_fixes PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/font
    )
    add_test(NAME test_vg_tier3_fixes COMMAND test_vg_tier3_fixes)
    set_tests_properties(test_vg_tier3_fixes PROPERTIES LABELS "gui" TIMEOUT 15)

    # Regression suite for GUI audit fixes.
    add_executable(test_vg_audit_fixes tests/test_vg_audit_fixes.c)
    target_link_libraries(test_vg_audit_fixes zannagui)
    target_include_directories(test_vg_audit_fixes PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(test_vg_audit_fixes PRIVATE
            ZANNA_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/src/runtime/graphics/text/fonts/JetBrainsMono-Regular.ttf")
    add_test(NAME test_vg_audit_fixes COMMAND test_vg_audit_fixes)
    set_tests_properties(test_vg_audit_fixes PROPERTIES
            LABELS "gui"
            TIMEOUT 180
            RUN_SERIAL TRUE
            ENVIRONMENT "ZANNA_GUI_TEST_VERBOSE=1")

    add_executable(test_vg_grapheme tests/test_vg_grapheme.c)
    target_link_libraries(test_vg_grapheme zannagui)
    target_include_directories(test_vg_grapheme PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(test_vg_grapheme PRIVATE
            VG_GRAPHEME_TEST_DATA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/GraphemeBreakTest-17.0.0.txt")
    add_test(NAME test_vg_grapheme COMMAND test_vg_grapheme)
    set_tests_properties(test_vg_grapheme PROPERTIES LABELS "gui;unicode" TIMEOUT 30)

    add_executable(test_vg_textinput tests/test_vg_textinput.c)
    target_link_libraries(test_vg_textinput zannagui)
    target_include_directories(test_vg_textinput PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_textinput COMMAND test_vg_textinput)
    set_tests_properties(test_vg_textinput PROPERTIES LABELS "gui;unicode" TIMEOUT 30)

    add_executable(test_vg_outputpane_metrics tests/test_vg_outputpane_metrics.c)
    target_link_libraries(test_vg_outputpane_metrics zannagui)
    target_include_directories(test_vg_outputpane_metrics PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_outputpane_metrics COMMAND test_vg_outputpane_metrics)
    set_tests_properties(test_vg_outputpane_metrics PROPERTIES LABELS "gui" TIMEOUT 60 RUN_SERIAL TRUE)

    add_executable(test_vg_outputpane_term tests/test_vg_outputpane_term.c)
    target_link_libraries(test_vg_outputpane_term zannagui)
    target_include_directories(test_vg_outputpane_term PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_outputpane_term COMMAND test_vg_outputpane_term)
    set_tests_properties(test_vg_outputpane_term PROPERTIES LABELS "gui" TIMEOUT 60 RUN_SERIAL TRUE)

    add_executable(test_vg_datagrid tests/test_vg_datagrid.c)
    target_link_libraries(test_vg_datagrid zannagui)
    target_include_directories(test_vg_datagrid PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_datagrid COMMAND test_vg_datagrid)
    set_tests_properties(test_vg_datagrid PROPERTIES LABELS "gui" TIMEOUT 60 RUN_SERIAL TRUE)

    add_executable(test_vg_popuplist tests/test_vg_popuplist.c)
    target_link_libraries(test_vg_popuplist zannagui)
    target_include_directories(test_vg_popuplist PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_popuplist COMMAND test_vg_popuplist)
    set_tests_properties(test_vg_popuplist PROPERTIES LABELS "gui" TIMEOUT 60 RUN_SERIAL TRUE)

    add_executable(test_vg_font_bounds tests/test_vg_font_bounds.c)
    target_link_libraries(test_vg_font_bounds zannagui)
    target_include_directories(test_vg_font_bounds PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/font
    )
    add_test(NAME test_vg_font_bounds COMMAND test_vg_font_bounds)
    set_tests_properties(test_vg_font_bounds PROPERTIES LABELS "gui" TIMEOUT 15)

    add_executable(test_vg_codeeditor_perf tests/test_vg_codeeditor_perf.c)
    target_link_libraries(test_vg_codeeditor_perf zannagui)
    target_include_directories(test_vg_codeeditor_perf PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_codeeditor_perf COMMAND test_vg_codeeditor_perf)
    set_tests_properties(test_vg_codeeditor_perf PROPERTIES LABELS "gui;zannastudio;perf" TIMEOUT 15)

    add_executable(test_vg_codeeditor_behavior tests/test_vg_codeeditor_behavior.c)
    target_link_libraries(test_vg_codeeditor_behavior zannagui)
    target_include_directories(test_vg_codeeditor_behavior PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_codeeditor_behavior COMMAND test_vg_codeeditor_behavior)
    set_tests_properties(test_vg_codeeditor_behavior PROPERTIES LABELS "gui;zannastudio" TIMEOUT 15)

    add_executable(test_vg_codeeditor_journal tests/test_vg_codeeditor_journal.c)
    target_link_libraries(test_vg_codeeditor_journal zannagui)
    target_include_directories(test_vg_codeeditor_journal PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_codeeditor_journal COMMAND test_vg_codeeditor_journal)
    set_tests_properties(test_vg_codeeditor_journal PROPERTIES LABELS "gui;zannastudio" TIMEOUT 15)

    add_executable(test_vg_treeview_dnd tests/test_vg_treeview_dnd.c)
    target_link_libraries(test_vg_treeview_dnd zannagui)
    target_include_directories(test_vg_treeview_dnd PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_treeview_dnd COMMAND test_vg_treeview_dnd)
    set_tests_properties(test_vg_treeview_dnd PROPERTIES LABELS "gui;zannastudio" TIMEOUT 15)

    add_executable(test_vg_virtual_models tests/test_vg_virtual_models.c)
    target_link_libraries(test_vg_virtual_models zannagui)
    target_include_directories(test_vg_virtual_models PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME test_vg_virtual_models COMMAND test_vg_virtual_models)
    set_tests_properties(test_vg_virtual_models PROPERTIES LABELS "gui;zannastudio;perf" TIMEOUT 15)

    # Anti-aliased drawing core. Built self-contained against the mock backend so
    # the pixel-exact and determinism assertions run headless on every platform.
    add_executable(test_vg_draw
            tests/test_vg_draw.c
            src/core/vg_draw.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src/vgfx.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src/vgfx_draw.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src/vgfx_platform_mock.c
    )
    target_include_directories(test_vg_draw PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src
    )
    if (UNIX AND NOT APPLE)
        target_compile_definitions(test_vg_draw PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    endif ()
    add_test(NAME test_vg_draw COMMAND test_vg_draw)
    set_tests_properties(test_vg_draw PROPERTIES LABELS "gui" TIMEOUT 15)

    # WCAG contrast gate over every built-in theme palette. Palette-agnostic:
    # protects future retunes without pinning any color value.
    add_executable(test_vg_theme_contrast
            tests/test_vg_theme_contrast.c
            src/core/vg_theme.c
    )
    target_include_directories(test_vg_theme_contrast PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
    )
    if (UNIX AND NOT APPLE)
        target_compile_definitions(test_vg_theme_contrast PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
        target_link_libraries(test_vg_theme_contrast PRIVATE m)
    endif ()
    add_test(NAME test_vg_theme_contrast COMMAND test_vg_theme_contrast)
    set_tests_properties(test_vg_theme_contrast PROPERTIES LABELS "gui" TIMEOUT 15)

    # Vector icon library: registry integrity, AA coverage, determinism, and
    # brand-mark roles against the mock backend framebuffer.
    add_executable(test_vg_icon_vector
            tests/test_vg_icon_vector.c
            src/core/vg_icon_vector.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src/vgfx.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src/vgfx_draw.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src/vgfx_platform_mock.c
    )
    target_include_directories(test_vg_icon_vector PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/src
    )
    if (UNIX AND NOT APPLE)
        target_compile_definitions(test_vg_icon_vector PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    endif ()
    add_test(NAME test_vg_icon_vector COMMAND test_vg_icon_vector)
    set_tests_properties(test_vg_icon_vector PROPERTIES LABELS "gui" TIMEOUT 30)

    if (UNIX AND NOT APPLE)
        foreach (vg_gui_test_target
                test_vg_font
                test_vg_widgets_new
                test_vg_tier1_fixes
                test_vg_tier2_fixes
                test_vg_tier3_fixes
                test_vg_audit_fixes
                test_vg_grapheme
                test_vg_textinput
                test_vg_font_bounds
                test_vg_codeeditor_perf)
            target_compile_definitions(${vg_gui_test_target} PRIVATE _GNU_SOURCE)
        endforeach ()
    endif ()
endif ()

# Demo applications
add_executable(gui_demo examples/gui_demo.c)
target_link_libraries(gui_demo zannagui zannagfx)
target_include_directories(gui_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
)

add_executable(widget_showcase examples/widget_showcase.c)
target_link_libraries(widget_showcase zannagui zannagfx)
target_include_directories(widget_showcase PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
)

add_executable(color_widgets_demo examples/color_widgets_demo.c)
target_link_libraries(color_widgets_demo zannagui zannagfx)
target_include_directories(color_widgets_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
)

# Zanna Studio Runtime Test (Phase 1 & 2 features)
add_executable(zannastudio_menu_test ../../zannastudio/src/tests/menu_test.c)
target_link_libraries(zannastudio_menu_test PRIVATE zanna_runtime zannagui zannagfx)
target_include_directories(zannastudio_menu_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../graphics/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime
)

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)
