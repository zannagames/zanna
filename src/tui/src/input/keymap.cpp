//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the keymap responsible for translating key chords into command
// invocations.  The keymap keeps a registry of available commands, supports
// global and widget-specific bindings, and routes events to the appropriate
// callback at runtime.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime keymap used by the TUI input system.
/// @details The keymap stores metadata about commands, exposes helpers for
///          registering and binding them, and dispatches incoming key events.
///          Bindings favour widget-specific mappings before falling back to the
///          global table so focused widgets can override shortcuts.

#include "tui/input/keymap.hpp"

namespace zanna::tui::input {
/// @brief Compare two key chords for matching code, modifiers, and codepoint.
/// @details Equality requires the same key code, modifier mask, and Unicode
///          codepoint.  Treating chords as value types simplifies their use as
///          keys in associative containers.
/// @param other Chord to compare with this value.
/// @return true when every chord component matches.
bool KeyChord::operator==(const KeyChord &other) const {
    return code == other.code && mods == other.mods && codepoint == other.codepoint;
}

/// @brief Compute a hash combining key code, modifiers, and Unicode codepoint.
/// @details Mixes the three components into a single size_t value suitable for
///          unordered maps.  Bit shifting keeps the fields from overlapping and
///          yields stable hashes across architectures.
/// @param kc Key chord to hash.
/// @return Combined hash compatible with chord equality.
std::size_t KeyChordHash::operator()(const KeyChord &kc) const {
    return static_cast<std::size_t>(kc.code) ^ (static_cast<std::size_t>(kc.mods) << 8U) ^
           (static_cast<std::size_t>(kc.codepoint) << 16U);
}

/// @brief Register a command or update an existing one.
/// @details If the identifier already exists the function refreshes the
///          metadata and callback.  Otherwise it appends a new command to the
///          registry and records its index for quick lookup during dispatch.
/// @param id Stable command identifier.
/// @param name Human-readable command label.
/// @param action Callback invoked by execution.
void Keymap::registerCommand(CommandId id, std::string name, std::function<void()> action) {
    auto it = index_.find(id);
    if (it != index_.end()) {
        auto &cmd = commands_[it->second];
        cmd.name = std::move(name);
        cmd.action = std::move(action);
        return;
    }

    const auto slot = commands_.size();
    commands_.push_back(Command{std::move(id), std::move(name), std::move(action)});
    index_[commands_[slot].id] = slot;
}

/// @brief Associate a key chord with a command across the entire application.
/// @details Inserts or overwrites the mapping in the global binding table so
///          the shortcut applies regardless of widget focus.
/// @param kc Key chord to bind.
/// @param id Registered command identifier to dispatch.
void Keymap::bindGlobal(const KeyChord &kc, const CommandId &id) {
    global_[kc] = id;
}

/// @brief Bind a key chord to a command for a specific widget.
/// @details Widget bindings override global shortcuts when the given widget is
///          focused, enabling contextual behaviour without affecting other UI
///          components.
/// @param w Non-owning widget key identifying the binding scope.
/// @param kc Key chord to bind.
/// @param id Registered command identifier to dispatch.
void Keymap::bindWidget(ui::Widget *w, const KeyChord &kc, const CommandId &id) {
    widget_[w][kc] = id;
}

/// @brief Execute the callback associated with a command identifier.
/// @details Looks up the command in the registry, invokes the stored callback
///          when present, and returns whether execution occurred.  Missing or
///          unbound commands report false so callers can fall back gracefully.
/// @param id Command identifier to look up.
/// @return true when a nonempty callback was found and invoked.
bool Keymap::execute(const CommandId &id) const {
    auto it = index_.find(id);
    if (it != index_.end()) {
        const auto &cmd = commands_[it->second];
        if (cmd.action) {
            cmd.action();
            return true;
        }
    }
    return false;
}

/// @brief Retrieve a pointer to the registered command metadata, if present.
/// @details Returns nullptr when the identifier is unknown.  The pointer remains
///          valid for the lifetime of the keymap because commands are stored in a
///          vector that only grows.
/// @param id Command identifier to find.
/// @return Borrowed command pointer, or nullptr when absent.
const Command *Keymap::find(const CommandId &id) const {
    auto it = index_.find(id);
    if (it != index_.end()) {
        return &commands_[it->second];
    }
    return nullptr;
}

/// @brief Dispatch a key event using widget-specific and global bindings.
/// @details Constructs a `KeyChord` from the event and first checks whether the
///          focused widget has an override.  If not, it falls back to the global
///          bindings.  Successful dispatch executes the associated command and
///          returns true; otherwise the event remains unhandled.
/// @param w Currently focused widget, or nullptr.
/// @param key Decoded key event to convert into a chord.
/// @return true when a matching command callback executed.
bool Keymap::handle(ui::Widget *w, const term::KeyEvent &key) const {
    KeyChord kc{key.code, key.mods, key.codepoint};
    if (w) {
        auto wit = widget_.find(w);
        if (wit != widget_.end()) {
            auto cit = wit->second.find(kc);
            if (cit != wit->second.end()) {
                return execute(cit->second);
            }
        }
    }
    auto git = global_.find(kc);
    if (git != global_.end()) {
        return execute(git->second);
    }
    return false;
}

/// @brief Access the registered command metadata in insertion order.
/// @details Exposes the internal vector so UI components can enumerate available
///          commands for display purposes.  The reference remains valid as long
///          as the keymap outlives the caller.
/// @return Const reference to the owned command vector.
const std::vector<Command> &Keymap::commands() const {
    return commands_;
}

} // namespace zanna::tui::input
