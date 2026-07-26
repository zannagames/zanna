//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the INI-backed configuration model for Zanna TUI.
/// @details Provides value types for semantic theme styles, editor behavior,
///          and global key bindings, plus the loader that overlays a file onto
///          functional defaults.
//
// The Config struct aggregates all configurable aspects of the TUI:
//   - Theme: maps semantic roles to RGBA color styles
//   - Editor: tab width, soft wrap, and other editor behaviors
//   - Keymap: global key chord to command bindings
//
// The loadFromFile() function parses a configuration file and populates
// a Config struct. Unrecognized keys are silently ignored, and missing
// keys retain their default values.
//
// Key invariants:
//   - Default Config values provide a functional dark-theme setup.
//   - loadFromFile() returns false on file I/O errors (not parse errors).
//   - Config is a plain aggregate with no invariants to maintain.
//
// Ownership: Config owns all its data by value. loadFromFile() borrows
// the file path string.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "tui/input/keymap.hpp"
#include "tui/render/screen.hpp"

namespace zanna::tui::config {

/// @brief Color palette configuration mapping semantic roles to render styles.
/// @details Configurable via the [theme] section of the configuration file.
///          Each field corresponds to a semantic role used by widgets.
struct Theme {
    render::Style normal{};    ///< Default text and background style.
    render::Style accent{};    ///< Emphasized controls and active highlights.
    render::Style disabled{};  ///< Unavailable-control presentation.
    render::Style selection{}; ///< Selected text or list-item presentation.
};

/// @brief Editor behavior configuration settings.
/// @details Configurable via the [editor] section of the configuration file.
///          Controls text display properties like tab width and word wrapping.
struct Editor {
    unsigned tab_width{4};  ///< Display width assigned to tab characters.
    bool soft_wrap{false}; ///< Whether long logical lines wrap to the viewport.
};

/// @brief Associates a key chord with a command identifier for key binding configuration.
/// @details Loaded from the [keymap] section of the configuration file. Each binding
///          maps a keyboard shortcut to a registered command name.
struct Binding {
    input::KeyChord chord{};   ///< Key chord that triggers the binding.
    input::CommandId command{}; ///< Registered command identifier to invoke.
};

/// @brief Aggregated configuration for the TUI application.
/// @details Combines theme, key binding, and editor settings into a single struct
///          that can be loaded from a configuration file. Default values provide
///          a functional dark-theme setup with standard key bindings.
struct Config {
    Theme theme{};                       ///< Semantic application color palette.
    std::vector<Binding> keymap_global{}; ///< Application-wide key bindings.
    Editor editor{};                     ///< Text-editor behavior settings.
};

/// @brief Load TUI configuration from an INI-style file.
/// @details Parses the configuration file and populates the output Config struct.
///          Sections include [theme], [editor], and [keymap]. Unrecognized keys
///          are silently ignored; missing keys retain default values.
/// @param path Filesystem path to the configuration file.
/// @param out Config struct to populate with parsed values.
/// @return True if the file was successfully opened and parsed; false on I/O errors.
bool loadFromFile(const std::string &path, Config &out);

} // namespace zanna::tui::config
