#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/lint_zannastudio_notification_policy.cmake
# Purpose: Keep routine Studio feedback out of transient notification pop-ups.
# Key invariants:
#   - Routine success is durable status text, never a transient interruption.
#   - Informational toasts are limited to reviewed recovery/activation events.
#   - Background-capable Build/Search controllers cannot call Toast directly.
#   - Warnings and errors remain available through contextual policy.
# Ownership/Lifetime: Build/test-time source audit with no runtime state.
# Links: src/zannastudio/src/ui/notification_policy.zia,
#        src/zannastudio/src/app/studio_application_base.zia
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
    ZANNA_STUDIO_NOTIFICATION_SOURCES
    LIST_DIRECTORIES false
    "${ZANNA_STUDIO_SOURCE_ROOT}/*.zia")

set(ZANNA_STUDIO_NOTIFICATION_VIOLATIONS "")
set(ZANNA_STUDIO_CONTEXTUAL_NOTIFICATION_VIOLATIONS "")
foreach(ZANNA_STUDIO_SOURCE IN LISTS ZANNA_STUDIO_NOTIFICATION_SOURCES)
    file(
        STRINGS
        "${ZANNA_STUDIO_SOURCE}"
        ZANNA_STUDIO_NOTIFICATION_LINES
        REGEX "GUI\\.Toast\\.(Info|Success)\\(")
    file(
        RELATIVE_PATH
        ZANNA_STUDIO_SOURCE_RELATIVE
        "${ZANNA_STUDIO_SOURCE_ROOT}"
        "${ZANNA_STUDIO_SOURCE}")

    foreach(ZANNA_STUDIO_NOTIFICATION_LINE IN LISTS ZANNA_STUDIO_NOTIFICATION_LINES)
        string(STRIP
               "${ZANNA_STUDIO_NOTIFICATION_LINE}"
               ZANNA_STUDIO_NOTIFICATION_LINE)
        set(ZANNA_STUDIO_NOTIFICATION_ALLOWED false)
        string(
            FIND
            "${ZANNA_STUDIO_NOTIFICATION_LINE}"
            "Recovered \" + recoveredOpened"
            ZANNA_STUDIO_RECOVERY_TOAST)
        string(
            FIND
            "${ZANNA_STUDIO_NOTIFICATION_LINE}"
            "visible but macOS did not make it active"
            ZANNA_STUDIO_ACTIVATION_TOAST)
        if(ZANNA_STUDIO_SOURCE_RELATIVE STREQUAL
               "app/studio_application_base.zia"
           AND NOT ZANNA_STUDIO_RECOVERY_TOAST EQUAL -1)
            set(ZANNA_STUDIO_NOTIFICATION_ALLOWED true)
        elseif(ZANNA_STUDIO_SOURCE_RELATIVE STREQUAL "main.zia"
               AND NOT ZANNA_STUDIO_ACTIVATION_TOAST EQUAL -1)
            set(ZANNA_STUDIO_NOTIFICATION_ALLOWED true)
        endif()

        if(NOT ZANNA_STUDIO_NOTIFICATION_ALLOWED)
            list(
                APPEND
                ZANNA_STUDIO_NOTIFICATION_VIOLATIONS
                "${ZANNA_STUDIO_SOURCE_RELATIVE}: ${ZANNA_STUDIO_NOTIFICATION_LINE}")
        endif()
    endforeach()

    if(ZANNA_STUDIO_SOURCE_RELATIVE STREQUAL "commands/build_commands.zia"
       OR ZANNA_STUDIO_SOURCE_RELATIVE STREQUAL "commands/search_commands.zia")
        file(
            STRINGS
            "${ZANNA_STUDIO_SOURCE}"
            ZANNA_STUDIO_DIRECT_CONTEXTUAL_NOTIFICATION_LINES
            REGEX "GUI\\.Toast\\.(Warning|Error)\\(")
        foreach(
            ZANNA_STUDIO_DIRECT_CONTEXTUAL_NOTIFICATION_LINE
            IN LISTS ZANNA_STUDIO_DIRECT_CONTEXTUAL_NOTIFICATION_LINES)
            string(
                STRIP
                "${ZANNA_STUDIO_DIRECT_CONTEXTUAL_NOTIFICATION_LINE}"
                ZANNA_STUDIO_DIRECT_CONTEXTUAL_NOTIFICATION_LINE)
            list(
                APPEND
                ZANNA_STUDIO_CONTEXTUAL_NOTIFICATION_VIOLATIONS
                "${ZANNA_STUDIO_SOURCE_RELATIVE}: ${ZANNA_STUDIO_DIRECT_CONTEXTUAL_NOTIFICATION_LINE}")
        endforeach()
    endif()
endforeach()

if(ZANNA_STUDIO_NOTIFICATION_VIOLATIONS)
    list(
        JOIN
        ZANNA_STUDIO_NOTIFICATION_VIOLATIONS
        "\n  "
        ZANNA_STUDIO_NOTIFICATION_VIOLATION_TEXT)
    message(
        FATAL_ERROR
        "Routine Studio feedback must use AppShell.SetStatusLeft. "
        "Success toasts are forbidden; only reviewed recovery/activation Info "
        "toasts are allowed:\n"
        "  ${ZANNA_STUDIO_NOTIFICATION_VIOLATION_TEXT}")
endif()

if(ZANNA_STUDIO_CONTEXTUAL_NOTIFICATION_VIOLATIONS)
    list(
        JOIN
        ZANNA_STUDIO_CONTEXTUAL_NOTIFICATION_VIOLATIONS
        "\n  "
        ZANNA_STUDIO_CONTEXTUAL_NOTIFICATION_VIOLATION_TEXT)
    message(
        FATAL_ERROR
        "Background-capable Build/Search controllers must keep completion on "
        "durable surfaces and route immediate failures through "
        "ui/notification_policy.zia; direct warning/error toasts are "
        "forbidden:\n"
        "  ${ZANNA_STUDIO_CONTEXTUAL_NOTIFICATION_VIOLATION_TEXT}")
endif()

message(STATUS "Zanna Studio notification policy: clean")
