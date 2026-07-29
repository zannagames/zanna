#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/lint_zannastudio_modal_input_policy.cmake
# Purpose: Keep routine Studio input/review on non-modal workbench surfaces.
# Key invariants:
#   - Studio command text entry never blocks the native event loop.
#   - Search, settings, navigation, and refactors retain normal keyboard flow.
#   - Rename review uses the bounded workspace-edit preview, not a message box.
#   - Active search/build conflicts reveal durable panels instead of prompting.
#   - Destructive confirmation dialogs remain a separate reviewed interaction.
# Ownership/Lifetime: Build/test-time source audit with no runtime state.
# Links: src/zannastudio/src/ui/command_input.zia,
#        src/zannastudio/src/commands/search_commands.zia,
#        src/zannastudio/src/commands/file_commands.zia,
#        src/zannastudio/src/commands/file_save_support.zia
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.20)

get_filename_component(
    ZANNA_REPOSITORY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.."
    ABSOLUTE)
set(
    ZANNA_STUDIO_SOURCE_ROOT
    "${ZANNA_REPOSITORY_ROOT}/src/zannastudio/src")

file(
    GLOB_RECURSE
    ZANNA_STUDIO_INPUT_SOURCES
    LIST_DIRECTORIES false
    "${ZANNA_STUDIO_SOURCE_ROOT}/*.zia")

set(ZANNA_STUDIO_INPUT_VIOLATIONS "")
foreach(ZANNA_STUDIO_SOURCE IN LISTS ZANNA_STUDIO_INPUT_SOURCES)
    file(
        STRINGS
        "${ZANNA_STUDIO_SOURCE}"
        ZANNA_STUDIO_INPUT_LINES
        REGEX "MessageBox\\.(Prompt|Info|Confirm)[ \t]*\\(")
    file(
        RELATIVE_PATH
        ZANNA_STUDIO_SOURCE_RELATIVE
        "${ZANNA_STUDIO_SOURCE_ROOT}"
        "${ZANNA_STUDIO_SOURCE}")
    if(ZANNA_STUDIO_INPUT_LINES)
        foreach(ZANNA_STUDIO_INPUT_LINE IN LISTS ZANNA_STUDIO_INPUT_LINES)
            string(STRIP
                   "${ZANNA_STUDIO_INPUT_LINE}"
                   ZANNA_STUDIO_INPUT_LINE)
            set(ZANNA_STUDIO_INPUT_ALLOWED false)
            if(ZANNA_STUDIO_SOURCE_RELATIVE STREQUAL
                   "commands/file_save_support.zia"
               AND ZANNA_STUDIO_INPUT_LINE MATCHES "MessageBox\\.Confirm")
                set(ZANNA_STUDIO_INPUT_ALLOWED true)
            endif()
            if(NOT ZANNA_STUDIO_INPUT_ALLOWED)
                list(
                    APPEND
                    ZANNA_STUDIO_INPUT_VIOLATIONS
                    "${ZANNA_STUDIO_SOURCE_RELATIVE}: ${ZANNA_STUDIO_INPUT_LINE}")
            endif()
        endforeach()
    endif()
endforeach()

if(ZANNA_STUDIO_INPUT_VIOLATIONS)
    list(
        JOIN
        ZANNA_STUDIO_INPUT_VIOLATIONS
        "\n  "
        ZANNA_STUDIO_INPUT_VIOLATION_TEXT)
    message(
        FATAL_ERROR
        "Routine Studio input/review must use a docked panel, palette, settings "
        "surface, CommandInputOverlay, or WorkspaceEditPreview. Blocking "
        "MessageBox.Prompt/Info calls are forbidden, and direct Confirm calls "
        "are restricted to reviewed external-file safeguards:\n"
        "  ${ZANNA_STUDIO_INPUT_VIOLATION_TEXT}")
endif()

message(STATUS "Zanna Studio modal-input policy: clean")
