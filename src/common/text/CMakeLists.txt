# SPDX-License-Identifier: GPL-3.0-only
# Zanna shared text engine library
#
# This target packages the piece-table text implementation and its C ABI facade
# so GUI and TUI editors link one shared buffer engine.

# The runtime regex parser/matcher is deliberately packaged ahead of both GUI
# and runtime targets. This keeps one implementation in the final process while
# avoiding a GUI -> whole-runtime dependency cycle.
add_library(zanna_regex_engine STATIC
        ${CMAKE_SOURCE_DIR}/src/runtime/text/rt_regex_engine.c
        ${CMAKE_SOURCE_DIR}/src/runtime/text/rt_regex_parse.c
        ${CMAKE_SOURCE_DIR}/src/runtime/text/rt_regex_match.c
)

target_compile_features(zanna_regex_engine PUBLIC c_std_11)

target_include_directories(zanna_regex_engine
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/runtime/text>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna/runtime>
)

target_link_libraries(zanna_regex_engine PUBLIC zanna_common_opts)

add_library(zanna_text_core STATIC
        ${CMAKE_SOURCE_DIR}/src/tui/src/text/EditHistory.cpp
        ${CMAKE_SOURCE_DIR}/src/tui/src/text/LineIndex.cpp
        ${CMAKE_SOURCE_DIR}/src/tui/src/text/PieceTable.cpp
        ${CMAKE_SOURCE_DIR}/src/tui/src/text/text_buffer.cpp
        zanna_text_buffer.cpp
)

target_compile_features(zanna_text_core PUBLIC cxx_std_20)

target_include_directories(zanna_text_core
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/tui/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna/text>
)

target_link_libraries(zanna_text_core
        PUBLIC
        zanna_common_opts
)

install(FILES zanna_text_buffer.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/text
)
