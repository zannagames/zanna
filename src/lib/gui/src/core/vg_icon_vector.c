//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/core/vg_icon_vector.c
// Purpose: Deterministic scalable vector icon library — fixed-point path
//          tables rasterized with an anti-aliased integer scanline fill
//          (even-odd rule, 4x vertical supersampling, analytic horizontal
//          coverage) and cached as tint-independent coverage masks.
// Key invariants:
//   - No floating point anywhere: Q16 coordinates, integer coverage, fixed
//     8-segment quadratic flattening — bit-identical across platforms.
//   - Masks are per color role; tint applies only at blit time.
//   - The LRU cache bounds live masks; eviction never frees an in-use mask
//     because draw completes before returning.
// Ownership/Lifetime:
//   - Icon tables are static const; cache entries own their mask storage.
// Links: lib/gui/include/vg_icon_vector.h,
//        docs/adr/0137-premium-rendering-surface.md
//
//===----------------------------------------------------------------------===//

#include "../../include/vg_icon_vector.h"

#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements deterministic fixed-point vector-icon rasterization and caching.
/// @details Compact commands on a 96-unit design grid are flattened into Q16 polygon edges,
/// filled with four vertical sub-samples and analytic horizontal coverage, and cached as
/// tint-independent role masks in a bounded least-recently-used table.

//=============================================================================
// Path command encoding (design grid: 0..96 units, square)
//=============================================================================

enum {
    IV_MOVE = 0, ///< Begin subpath at (x, y); role selects the color role.
    IV_LINE = 1, ///< Line to (x, y).
    IV_QUAD = 2, ///< Quadratic to (x, y) with control (cx, cy).
    IV_END = 3,  ///< Terminate the command list.
};

enum {
    IV_ROLE_TINT = 0,  ///< Caller-tinted subpath.
    IV_ROLE_GREEN = 1, ///< Brand green (Zanna mark).
    IV_ROLE_STEEL = 2, ///< Brand steel (Zanna mark).
    IV_ROLE_TEAL = 3,  ///< Brand teal (Zanna mark).
};

#define IV_ROLE_COUNT 4

/// @brief One compact design-grid vector-path command.
typedef struct vg_iv_cmd {
    /// @brief Operation selected from the `IV_*` command constants.
    uint8_t op;

    /// @brief Color role selected when @ref op starts a subpath.
    uint8_t role;

    /// @brief Command endpoint in design-grid units.
    int16_t x, y;

    /// @brief Quadratic control point in design-grid units.
    int16_t cx, cy;
} vg_iv_cmd_t;

// Brand role palette (the mark is brand-fixed by design; ADR 0137).
static const uint32_t k_iv_role_rgb[IV_ROLE_COUNT] = {0, 0x8CC63F, 0xB9C2C6, 0x2BC8C4};

#define M(role, x, y) {IV_MOVE, (role), (x), (y), 0, 0}
#define L(x, y) {IV_LINE, 0, (x), (y), 0, 0}
#define Q(cx, cy, x, y) {IV_QUAD, 0, (x), (y), (cx), (cy)}
#define E() {IV_END, 0, 0, 0, 0, 0}

//=============================================================================
// Icon tables
//=============================================================================

// -- files -------------------------------------------------------------------

// Sheet with a folded corner (fold reads as a hole via even-odd).
#define IV_SHEET                                                                                   \
    M(0, 26, 8), L(60, 8), L(76, 24), L(76, 88), L(26, 88), M(0, 58, 6), L(78, 26), L(58, 26)

static const vg_iv_cmd_t k_iv_file[] = {IV_SHEET, E()};

static const vg_iv_cmd_t k_iv_file_zia[] = {
    IV_SHEET, M(0, 36, 40), L(64, 40), L(64, 50), L(48, 66), L(64, 66), L(64, 76), L(36, 76),
    L(36, 66), L(52, 50), L(36, 50), E()};

static const vg_iv_cmd_t k_iv_file_basic[] = {
    IV_SHEET, M(0, 36, 42), L(64, 42), L(64, 52), L(36, 52),
    M(0, 36, 62), L(64, 62), L(64, 72), L(36, 72), E()};

static const vg_iv_cmd_t k_iv_file_il[] = {
    IV_SHEET, M(0, 36, 38), L(64, 38), L(64, 46), L(36, 46), M(0, 36, 54), L(64, 54), L(64, 62),
    L(36, 62), M(0, 36, 70), L(64, 70), L(64, 78), L(36, 78), E()};

static const vg_iv_cmd_t k_iv_file_json[] = {
    IV_SHEET, M(0, 38, 52), L(48, 52), L(48, 62), L(38, 62),
    M(0, 54, 52), L(64, 52), L(64, 62), L(54, 62), E()};

static const vg_iv_cmd_t k_iv_file_md[] = {
    IV_SHEET, M(0, 34, 44), L(42, 44), L(50, 56), L(58, 44), L(66, 44), L(66, 76), L(58, 76),
    L(58, 58), L(50, 70), L(42, 58), L(42, 76), L(34, 76), E()};

static const vg_iv_cmd_t k_iv_file_image[] = {
    IV_SHEET, M(0, 34, 76), L(48, 52), L(58, 64), L(66, 56), L(66, 76),
    M(0, 40, 40), Q(48, 40, 48, 46), Q(48, 52, 40, 52), Q(33, 52, 33, 46), Q(33, 40, 40, 40), E()};

static const vg_iv_cmd_t k_iv_folder[] = {
    M(0, 8, 22), L(38, 22), L(48, 32), L(88, 32), L(88, 82), L(8, 82), E()};

static const vg_iv_cmd_t k_iv_folder_open[] = {
    M(0, 8, 22), L(36, 22), L(46, 32), L(80, 32), L(80, 42), L(20, 42), L(8, 78),
    M(0, 26, 48), L(94, 48), L(80, 82), L(12, 82), E()};

// -- git ---------------------------------------------------------------------

static const vg_iv_cmd_t k_iv_git_added[] = {
    M(0, 42, 16), L(54, 16), L(54, 42), L(80, 42), L(80, 54), L(54, 54), L(54, 80), L(42, 80),
    L(42, 54), L(16, 54), L(16, 42), L(42, 42), E()};

static const vg_iv_cmd_t k_iv_git_modified[] = {
    M(0, 48, 26), Q(70, 26, 70, 48), Q(70, 70, 48, 70), Q(26, 70, 26, 48), Q(26, 26, 48, 26), E()};

static const vg_iv_cmd_t k_iv_git_deleted[] = {
    M(0, 16, 42), L(80, 42), L(80, 54), L(16, 54), E()};

static const vg_iv_cmd_t k_iv_git_renamed[] = {
    M(0, 12, 42), L(58, 42), L(58, 26), L(86, 48), L(58, 70), L(58, 54), L(12, 54), E()};

static const vg_iv_cmd_t k_iv_git_untracked[] = {
    M(0, 48, 22), Q(74, 22, 74, 48), Q(74, 74, 48, 74), Q(22, 74, 22, 48), Q(22, 22, 48, 22),
    M(0, 48, 34), Q(62, 34, 62, 48), Q(62, 62, 48, 62), Q(34, 62, 34, 48), Q(34, 34, 48, 34), E()};

static const vg_iv_cmd_t k_iv_source_control[] = {
    M(0, 26, 14), Q(38, 14, 38, 26), Q(38, 35, 30, 38), L(30, 58), Q(30, 62, 34, 62), L(56, 62),
    Q(64, 62, 64, 54), L(64, 38), Q(56, 35, 56, 26), Q(56, 14, 68, 14), Q(80, 14, 80, 26),
    Q(80, 35, 72, 38), L(72, 54), Q(72, 70, 56, 70), L(34, 70), Q(30, 70, 30, 74),
    Q(38, 77, 38, 86), Q(38, 96, 26, 96), Q(14, 96, 14, 86), Q(14, 77, 22, 74), L(22, 38),
    Q(14, 35, 14, 26), Q(14, 14, 26, 14), E()};

// -- debug -------------------------------------------------------------------

static const vg_iv_cmd_t k_iv_run[] = {M(0, 30, 16), L(78, 48), L(30, 80), E()};

static const vg_iv_cmd_t k_iv_debug_pause[] = {
    M(0, 28, 18), L(42, 18), L(42, 78), L(28, 78), M(0, 54, 18), L(68, 18), L(68, 78), L(54, 78),
    E()};

static const vg_iv_cmd_t k_iv_debug_stop[] = {M(0, 24, 24), L(72, 24), L(72, 72), L(24, 72), E()};

static const vg_iv_cmd_t k_iv_debug_restart[] = {
    M(0, 48, 14), Q(82, 14, 82, 48), Q(82, 82, 48, 82), Q(14, 82, 14, 48), L(26, 48),
    Q(26, 70, 48, 70), Q(70, 70, 70, 48), Q(70, 26, 48, 26), L(48, 40), L(28, 22), L(48, 4), E()};

static const vg_iv_cmd_t k_iv_debug_continue[] = {
    M(0, 20, 18), L(32, 18), L(32, 78), L(20, 78), M(0, 42, 18), L(84, 48), L(42, 78), E()};

static const vg_iv_cmd_t k_iv_debug_step_over[] = {
    M(0, 14, 54), Q(14, 22, 48, 22), Q(74, 22, 80, 40), L(88, 32), L(90, 58), L(64, 54), L(72, 47),
    Q(66, 34, 48, 34), Q(22, 34, 26, 56), M(0, 48, 64), Q(58, 64, 58, 73),
    Q(58, 82, 48, 82), Q(38, 82, 38, 73), Q(38, 64, 48, 64), E()};

static const vg_iv_cmd_t k_iv_debug_step_into[] = {
    M(0, 42, 10), L(54, 10), L(54, 40), L(70, 40), L(48, 62), L(26, 40), L(42, 40),
    M(0, 48, 68), Q(58, 68, 58, 77), Q(58, 86, 48, 86), Q(38, 86, 38, 77), Q(38, 68, 48, 68), E()};

static const vg_iv_cmd_t k_iv_debug_step_out[] = {
    M(0, 48, 8), L(70, 30), L(54, 30), L(54, 60), L(42, 60), L(42, 30), L(26, 30),
    M(0, 48, 68), Q(58, 68, 58, 77), Q(58, 86, 48, 86), Q(38, 86, 38, 77), Q(38, 68, 48, 68), E()};

static const vg_iv_cmd_t k_iv_breakpoint[] = {
    M(0, 48, 20), Q(76, 20, 76, 48), Q(76, 76, 48, 76), Q(20, 76, 20, 48), Q(20, 20, 48, 20), E()};

static const vg_iv_cmd_t k_iv_debug[] = {
    M(0, 48, 30), Q(66, 30, 66, 52), Q(66, 78, 48, 78), Q(30, 78, 30, 52), Q(30, 30, 48, 30),
    M(0, 40, 18), Q(48, 26, 56, 18), L(60, 24), Q(48, 32, 36, 24),
    M(0, 8, 46), L(24, 40), L(26, 48), L(10, 53), M(0, 88, 46), L(72, 40), L(70, 48), L(86, 53),
    M(0, 10, 74), L(26, 62), L(29, 69), L(13, 80), M(0, 86, 74), L(70, 62), L(67, 69), L(83, 80),
    E()};

// -- chrome / actions --------------------------------------------------------

static const vg_iv_cmd_t k_iv_new_file[] = {
    M(0, 26, 8), L(56, 8), L(72, 24), L(72, 46), L(60, 46), L(60, 30), L(52, 30), L(52, 20),
    L(38, 20), L(38, 88), L(26, 88),
    M(0, 62, 58), L(72, 58), L(72, 70), L(84, 70), L(84, 80), L(72, 80), L(72, 92), L(62, 92),
    L(62, 80), L(50, 80), L(50, 70), L(62, 70), E()};

static const vg_iv_cmd_t k_iv_save[] = {
    M(0, 14, 14), L(70, 14), L(84, 28), L(84, 84), L(14, 84),
    M(0, 28, 22), L(60, 22), L(60, 42), L(28, 42),
    M(0, 26, 56), L(72, 56), L(72, 78), L(26, 78), E()};

static const vg_iv_cmd_t k_iv_save_all[] = {
    M(0, 26, 6), L(74, 6), L(88, 20), L(88, 70), L(80, 70), L(80, 24), L(70, 14), L(26, 14),
    M(0, 8, 22), L(62, 22), L(76, 36), L(76, 90), L(8, 90),
    M(0, 20, 30), L(54, 30), L(54, 46), L(20, 46),
    M(0, 20, 60), L(64, 60), L(64, 84), L(20, 84), E()};

static const vg_iv_cmd_t k_iv_build[] = {
    M(0, 48, 6), L(88, 28), L(88, 68), L(48, 90), L(8, 68), L(8, 28),
    M(0, 48, 20), L(74, 34), L(48, 48), L(22, 34),
    M(0, 18, 42), L(42, 55), L(42, 76), L(18, 62),
    M(0, 78, 42), L(54, 55), L(54, 76), L(78, 62), E()};

static const vg_iv_cmd_t k_iv_find[] = {
    M(0, 42, 10), Q(74, 10, 74, 42), Q(74, 74, 42, 74), Q(10, 74, 10, 42), Q(10, 10, 42, 10),
    M(0, 42, 22), Q(62, 22, 62, 42), Q(62, 62, 42, 62), Q(22, 62, 22, 42), Q(22, 22, 42, 22),
    M(0, 64, 72), L(72, 64), L(90, 82), L(82, 90), E()};

static const vg_iv_cmd_t k_iv_replace[] = {
    M(0, 14, 40), Q(14, 14, 44, 14), L(44, 4), L(66, 20), L(44, 36), L(44, 26),
    Q(26, 26, 26, 40),
    M(0, 82, 56), Q(82, 82, 52, 82), L(52, 92), L(30, 76), L(52, 60), L(52, 70),
    Q(70, 70, 70, 56), E()};

static const vg_iv_cmd_t k_iv_terminal[] = {
    M(0, 12, 26), L(22, 18), L(48, 44), L(22, 70), L(12, 62), L(32, 44),
    M(0, 46, 70), L(86, 70), L(86, 80), L(46, 80), E()};

static const vg_iv_cmd_t k_iv_settings_gear[] = {
    M(0, 42, 6),  L(54, 6),  L(58, 20), L(70, 26), L(84, 20), L(90, 32), L(80, 42), L(80, 54),
    L(90, 64), L(84, 76), L(70, 70), L(58, 76), L(54, 90), L(42, 90), L(38, 76), L(26, 70),
    L(12, 76), L(6, 64),  L(16, 54), L(16, 42), L(6, 32),  L(12, 20), L(26, 26), L(38, 20),
    M(0, 48, 34), Q(62, 34, 62, 48), Q(62, 62, 48, 62), Q(34, 62, 34, 48), Q(34, 34, 48, 34), E()};

static const vg_iv_cmd_t k_iv_close[] = {
    M(0, 24, 16), L(48, 40), L(72, 16), L(80, 24), L(56, 48), L(80, 72), L(72, 80), L(48, 56),
    L(24, 80), L(16, 72), L(40, 48), L(16, 24), E()};

static const vg_iv_cmd_t k_iv_chevron_right[] = {
    M(0, 34, 14), L(70, 48), L(34, 82), L(26, 74), L(54, 48), L(26, 22), E()};

static const vg_iv_cmd_t k_iv_chevron_down[] = {
    M(0, 14, 34), L(48, 70), L(82, 34), L(74, 26), L(48, 54), L(22, 26), E()};

static const vg_iv_cmd_t k_iv_pin[] = {
    M(0, 38, 8), L(58, 8), L(58, 40), L(70, 52), L(70, 60), L(52, 60), L(52, 86), L(44, 86),
    L(44, 60), L(26, 60), L(26, 52), L(38, 40), E()};

static const vg_iv_cmd_t k_iv_split[] = {
    M(0, 10, 16), L(86, 16), L(86, 80), L(10, 80),
    M(0, 18, 24), L(43, 24), L(43, 72), L(18, 72),
    M(0, 53, 24), L(78, 24), L(78, 72), L(53, 72), E()};

static const vg_iv_cmd_t k_iv_plus[] = {
    M(0, 42, 14), L(54, 14), L(54, 42), L(82, 42), L(82, 54), L(54, 54), L(54, 82), L(42, 82),
    L(42, 54), L(14, 54), L(14, 42), L(42, 42), E()};

static const vg_iv_cmd_t k_iv_minus[] = {M(0, 16, 42), L(80, 42), L(80, 54), L(16, 54), E()};

static const vg_iv_cmd_t k_iv_dot[] = {
    M(0, 48, 32), Q(64, 32, 64, 48), Q(64, 64, 48, 64), Q(32, 64, 32, 48), Q(32, 32, 48, 32), E()};

static const vg_iv_cmd_t k_iv_check[] = {
    M(0, 12, 50), L(22, 40), L(40, 58), L(74, 18), L(84, 27), L(40, 78), E()};

static const vg_iv_cmd_t k_iv_warning[] = {
    M(0, 48, 8), L(92, 84), L(4, 84),
    M(0, 44, 36), L(52, 36), L(51, 60), L(45, 60),
    M(0, 44, 68), L(52, 68), L(52, 76), L(44, 76), E()};

static const vg_iv_cmd_t k_iv_error[] = {
    M(0, 48, 8), Q(88, 8, 88, 48), Q(88, 88, 48, 88), Q(8, 88, 8, 48), Q(8, 8, 48, 8),
    M(0, 34, 28), L(48, 42), L(62, 28), L(68, 34), L(54, 48), L(68, 62), L(62, 68), L(48, 54),
    L(34, 68), L(28, 62), L(42, 48), L(28, 34), E()};

static const vg_iv_cmd_t k_iv_info[] = {
    M(0, 48, 8), Q(88, 8, 88, 48), Q(88, 88, 48, 88), Q(8, 88, 8, 48), Q(8, 8, 48, 8),
    M(0, 44, 24), L(52, 24), L(52, 34), L(44, 34),
    M(0, 44, 42), L(52, 42), L(52, 72), L(44, 72), E()};

static const vg_iv_cmd_t k_iv_lightbulb[] = {
    M(0, 48, 8), Q(76, 8, 76, 36), Q(76, 52, 62, 60), L(62, 70), L(34, 70), L(34, 60),
    Q(20, 52, 20, 36), Q(20, 8, 48, 8),
    M(0, 36, 76), L(60, 76), L(60, 84), L(36, 84), E()};

static const vg_iv_cmd_t k_iv_explorer[] = {
    M(0, 14, 8), L(52, 8), L(64, 20), L(64, 30), L(56, 30), L(56, 24), L(46, 24), L(46, 16),
    L(22, 16), L(22, 80), L(14, 80),
    M(0, 30, 26), L(70, 26), L(82, 38), L(82, 92), L(30, 92), E()};

static const vg_iv_cmd_t k_iv_extensions[] = {
    M(0, 12, 12), L(42, 12), L(42, 42), L(12, 42),
    M(0, 54, 12), L(84, 12), L(84, 42), L(54, 42),
    M(0, 12, 54), L(42, 54), L(42, 84), L(12, 84),
    M(0, 58, 58), L(72, 58), L(72, 50), L(80, 50), L(80, 58), L(92, 58), L(92, 68), L(80, 68),
    L(80, 88), L(58, 88), E()};

// -- brand -------------------------------------------------------------------

static const vg_iv_cmd_t k_iv_zanna_mark[] = {
    M(IV_ROLE_GREEN, 16, 10), L(92, 10), L(86, 27), L(10, 27),
    M(IV_ROLE_STEEL, 70, 27), L(86, 27), L(32, 73), L(16, 73),
    M(IV_ROLE_TEAL, 16, 73), L(92, 73), L(86, 90), L(10, 90), E()};

#undef M
#undef L
#undef Q
#undef E

/// @brief Stable registry entry mapping one public name to its path commands.
typedef struct vg_iv_entry {
    /// @brief Process-lifetime kebab-case icon name.
    const char *name;

    /// @brief Process-lifetime terminated path-command array.
    const vg_iv_cmd_t *cmds;
} vg_iv_entry_t;

static const vg_iv_entry_t k_iv_registry[] = {
    {"file", k_iv_file},
    {"file-zia", k_iv_file_zia},
    {"file-basic", k_iv_file_basic},
    {"file-il", k_iv_file_il},
    {"file-json", k_iv_file_json},
    {"file-md", k_iv_file_md},
    {"file-image", k_iv_file_image},
    {"folder", k_iv_folder},
    {"folder-open", k_iv_folder_open},
    {"git-added", k_iv_git_added},
    {"git-modified", k_iv_git_modified},
    {"git-deleted", k_iv_git_deleted},
    {"git-renamed", k_iv_git_renamed},
    {"git-untracked", k_iv_git_untracked},
    {"source-control", k_iv_source_control},
    {"run", k_iv_run},
    {"debug", k_iv_debug},
    {"debug-pause", k_iv_debug_pause},
    {"debug-stop", k_iv_debug_stop},
    {"debug-restart", k_iv_debug_restart},
    {"debug-continue", k_iv_debug_continue},
    {"debug-step-over", k_iv_debug_step_over},
    {"debug-step-into", k_iv_debug_step_into},
    {"debug-step-out", k_iv_debug_step_out},
    {"breakpoint", k_iv_breakpoint},
    {"new-file", k_iv_new_file},
    {"save", k_iv_save},
    {"save-all", k_iv_save_all},
    {"build", k_iv_build},
    {"find", k_iv_find},
    {"replace", k_iv_replace},
    {"terminal", k_iv_terminal},
    {"settings-gear", k_iv_settings_gear},
    {"close", k_iv_close},
    {"chevron-right", k_iv_chevron_right},
    {"chevron-down", k_iv_chevron_down},
    {"pin", k_iv_pin},
    {"split", k_iv_split},
    {"plus", k_iv_plus},
    {"minus", k_iv_minus},
    {"dot", k_iv_dot},
    {"check", k_iv_check},
    {"warning", k_iv_warning},
    {"error", k_iv_error},
    {"info", k_iv_info},
    {"lightbulb", k_iv_lightbulb},
    {"explorer", k_iv_explorer},
    {"extensions", k_iv_extensions},
    {"zanna-mark", k_iv_zanna_mark},
};

#define IV_ICON_COUNT ((int32_t)(sizeof(k_iv_registry) / sizeof(k_iv_registry[0])))

//=============================================================================
// Fixed-point rasterizer (Q16, even-odd, 4x vertical supersampling)
//=============================================================================

#define IV_GRID 96
#define IV_MAX_POINTS 256
#define IV_MAX_EDGES 256
#define IV_MAX_CROSSINGS 64
#define IV_SUBSAMPLES 4
#define IV_COVER_PER_SUB 64 // 4 * 64 = 256 -> clamped to 255.

typedef struct vg_iv_edge {
    int32_t y0, y1; ///< Q16, y0 < y1.
    int32_t x0;     ///< Q16 x at y0.
    int64_t slope;  ///< Q16 dx per Q16 dy, in Q16 ((dx << 16) / dy).
} vg_iv_edge_t;

/// @brief Append one polygon edge, splitting orientation so y0 < y1.
/// @param edges Fixed-capacity destination edge array.
/// @param edge_count Mutable number of initialized edges.
/// @param ax First endpoint x coordinate in Q16 pixels.
/// @param ay First endpoint y coordinate in Q16 pixels.
/// @param bx Second endpoint x coordinate in Q16 pixels.
/// @param by Second endpoint y coordinate in Q16 pixels.
static void iv_add_edge(vg_iv_edge_t *edges,
                        int32_t *edge_count,
                        int32_t ax,
                        int32_t ay,
                        int32_t bx,
                        int32_t by) {
    if (ay == by || *edge_count >= IV_MAX_EDGES)
        return;
    vg_iv_edge_t *edge = &edges[(*edge_count)++];
    if (ay < by) {
        edge->y0 = ay;
        edge->y1 = by;
        edge->x0 = ax;
        edge->slope = ((int64_t)(bx - ax) << 16) / (by - ay);
    } else {
        edge->y0 = by;
        edge->y1 = ay;
        edge->x0 = bx;
        edge->slope = ((int64_t)(ax - bx) << 16) / (ay - by);
    }
}

/// @brief Accumulate analytic horizontal coverage for one sub-scanline span.
/// @param row Per-pixel coverage accumulators for one output row.
/// @param size_px Row width and icon edge length in pixels.
/// @param xa Span start in Q16 pixel coordinates.
/// @param xb Exclusive span end in Q16 pixel coordinates.
static void iv_cover_span(uint16_t *row, int32_t size_px, int32_t xa, int32_t xb) {
    if (xa < 0)
        xa = 0;
    int32_t limit = size_px << 16;
    if (xb > limit)
        xb = limit;
    if (xa >= xb)
        return;
    int32_t pa = xa >> 16;
    int32_t pb = (xb - 1) >> 16;
    if (pa == pb) {
        row[pa] = (uint16_t)(row[pa] + (uint16_t)(((int64_t)(xb - xa) * IV_COVER_PER_SUB) >> 16));
        return;
    }
    row[pa] = (uint16_t)(row[pa] +
                         (uint16_t)(((int64_t)(((pa + 1) << 16) - xa) * IV_COVER_PER_SUB) >> 16));
    for (int32_t px = pa + 1; px < pb; ++px)
        row[px] = (uint16_t)(row[px] + IV_COVER_PER_SUB);
    row[pb] = (uint16_t)(row[pb] + (uint16_t)(((int64_t)(xb - (pb << 16)) * IV_COVER_PER_SUB) >> 16));
}

/// @brief Rasterize one role's subpaths into an owned coverage mask.
/// @details Quadratic curves use a fixed eight-segment subdivision. Even-odd crossing pairs
/// contribute four vertical samples per output row, each with analytic horizontal coverage.
/// @param cmds Terminated command list for one registered icon.
/// @param size_px Positive square output size.
/// @param role Color role whose subpaths should be included.
/// @return Newly allocated size_px*size_px mask, or NULL on allocation failure.
static uint8_t *iv_rasterize_role(const vg_iv_cmd_t *cmds, int32_t size_px, uint8_t role) {
    // Flatten the role's subpaths into edges (Q16 pixel coordinates).
    vg_iv_edge_t edges[IV_MAX_EDGES];
    int32_t edge_count = 0;
    int64_t scale = ((int64_t)size_px << 16) / IV_GRID; // Q16 units-per-grid-step.

    int32_t px = 0, py = 0;       // Current point, Q16.
    int32_t sx = 0, sy = 0;       // Subpath start, Q16.
    int32_t have_subpath = 0;
    uint8_t current_role = IV_ROLE_TINT;

    for (const vg_iv_cmd_t *cmd = cmds;; ++cmd) {
        if (cmd->op == IV_MOVE || cmd->op == IV_END) {
            if (have_subpath && current_role == role && (px != sx || py != sy))
                iv_add_edge(edges, &edge_count, px, py, sx, sy);
            if (cmd->op == IV_END)
                break;
            current_role = cmd->role;
            px = sx = (int32_t)(cmd->x * scale);
            py = sy = (int32_t)(cmd->y * scale);
            have_subpath = 1;
            continue;
        }
        int32_t ex = (int32_t)(cmd->x * scale);
        int32_t ey = (int32_t)(cmd->y * scale);
        if (cmd->op == IV_LINE) {
            if (current_role == role)
                iv_add_edge(edges, &edge_count, px, py, ex, ey);
            px = ex;
            py = ey;
        } else { // IV_QUAD: fixed 8-segment subdivision keeps output deterministic.
            int32_t cx = (int32_t)(cmd->cx * scale);
            int32_t cy = (int32_t)(cmd->cy * scale);
            int32_t prev_x = px, prev_y = py;
            for (int32_t step = 1; step <= 8; ++step) {
                int64_t t = ((int64_t)step << 16) / 8; // Q16.
                int64_t u = 65536 - t;
                int64_t qx = (u * u * px + 2 * u * t * cx + t * t * ex) >> 32;
                int64_t qy = (u * u * py + 2 * u * t * cy + t * t * ey) >> 32;
                if (current_role == role)
                    iv_add_edge(edges, &edge_count, prev_x, prev_y, (int32_t)qx, (int32_t)qy);
                prev_x = (int32_t)qx;
                prev_y = (int32_t)qy;
            }
            px = prev_x;
            py = prev_y;
        }
    }
    if (edge_count == 0)
        return NULL;

    uint8_t *mask = (uint8_t *)calloc((size_t)size_px * (size_t)size_px, 1);
    if (!mask)
        return NULL;
    uint16_t *row = (uint16_t *)calloc((size_t)size_px, sizeof(uint16_t));
    if (!row) {
        free(mask);
        return NULL;
    }

    for (int32_t pixel_y = 0; pixel_y < size_px; ++pixel_y) {
        memset(row, 0, (size_t)size_px * sizeof(uint16_t));
        for (int32_t sub = 0; sub < IV_SUBSAMPLES; ++sub) {
            // Sub-scanline center: (pixel_y + (2*sub+1)/8) in Q16.
            int32_t sample_y = (pixel_y << 16) + ((2 * sub + 1) << 13);
            int32_t crossings[IV_MAX_CROSSINGS];
            int32_t crossing_count = 0;
            for (int32_t i = 0; i < edge_count && crossing_count < IV_MAX_CROSSINGS; ++i) {
                const vg_iv_edge_t *edge = &edges[i];
                if (sample_y < edge->y0 || sample_y >= edge->y1)
                    continue;
                int64_t dy = sample_y - edge->y0;
                crossings[crossing_count++] = edge->x0 + (int32_t)((edge->slope * dy) >> 16);
            }
            // Insertion sort: crossing counts are tiny.
            for (int32_t i = 1; i < crossing_count; ++i) {
                int32_t key = crossings[i];
                int32_t j = i - 1;
                while (j >= 0 && crossings[j] > key) {
                    crossings[j + 1] = crossings[j];
                    --j;
                }
                crossings[j + 1] = key;
            }
            for (int32_t i = 0; i + 1 < crossing_count; i += 2)
                iv_cover_span(row, size_px, crossings[i], crossings[i + 1]);
        }
        uint8_t *mask_row = mask + (size_t)pixel_y * (size_t)size_px;
        for (int32_t pixel_x = 0; pixel_x < size_px; ++pixel_x) {
            uint16_t cover = row[pixel_x];
            mask_row[pixel_x] = (uint8_t)(cover > 255 ? 255 : cover);
        }
    }
    free(row);
    return mask;
}

/// @brief Return whether an icon uses a color role at all.
/// @param cmds Terminated command list to inspect.
/// @param role Color role to find on a move command.
/// @return 1 when at least one subpath uses @p role, otherwise 0.
static int32_t iv_uses_role(const vg_iv_cmd_t *cmds, uint8_t role) {
    for (const vg_iv_cmd_t *cmd = cmds; cmd->op != IV_END; ++cmd) {
        if (cmd->op == IV_MOVE && cmd->role == role)
            return 1;
    }
    return 0;
}

//=============================================================================
// Coverage-mask cache (LRU)
//=============================================================================

typedef struct vg_iv_cache_entry {
    int32_t icon_id; ///< VG_ICON_VECTOR_INVALID marks a free slot.
    int32_t size_px;
    uint64_t tick;
    uint8_t *masks[IV_ROLE_COUNT]; ///< NULL when the role is unused.
} vg_iv_cache_entry_t;

#define IV_CACHE_CAPACITY 96
static vg_iv_cache_entry_t g_iv_cache[IV_CACHE_CAPACITY];
static uint64_t g_iv_cache_tick;
static int32_t g_iv_cache_initialized;

/// @brief Lazily mark every static cache slot as unused.
static void iv_cache_init(void) {
    if (g_iv_cache_initialized)
        return;
    for (int32_t i = 0; i < IV_CACHE_CAPACITY; ++i)
        g_iv_cache[i].icon_id = VG_ICON_VECTOR_INVALID;
    g_iv_cache_initialized = 1;
}

/// @brief Release every role mask owned by one entry and mark the slot unused.
/// @param entry Cache entry to clear.
static void iv_cache_entry_free(vg_iv_cache_entry_t *entry) {
    for (int32_t role = 0; role < IV_ROLE_COUNT; ++role) {
        free(entry->masks[role]);
        entry->masks[role] = NULL;
    }
    entry->icon_id = VG_ICON_VECTOR_INVALID;
}

/// @brief Find or build the cache entry for (icon, size).
/// @details A miss evicts the least-recently-used occupied entry, rasterizes every role present
/// in the icon, and returns the slot even when an individual role allocation fails.
/// @param icon_id Valid registry index.
/// @param size_px Positive requested icon edge length.
/// @return Borrowed cache entry whose lifetime extends until a later eviction or cache clear.
static vg_iv_cache_entry_t *iv_cache_get(int32_t icon_id, int32_t size_px) {
    iv_cache_init();
    vg_iv_cache_entry_t *victim = &g_iv_cache[0];
    for (int32_t i = 0; i < IV_CACHE_CAPACITY; ++i) {
        vg_iv_cache_entry_t *entry = &g_iv_cache[i];
        if (entry->icon_id == icon_id && entry->size_px == size_px) {
            entry->tick = ++g_iv_cache_tick;
            return entry;
        }
        if (entry->icon_id == VG_ICON_VECTOR_INVALID) {
            if (victim->icon_id != VG_ICON_VECTOR_INVALID || entry->tick < victim->tick)
                victim = entry;
        } else if (victim->icon_id != VG_ICON_VECTOR_INVALID && entry->tick < victim->tick) {
            victim = entry;
        }
    }
    iv_cache_entry_free(victim);
    const vg_iv_cmd_t *cmds = k_iv_registry[icon_id].cmds;
    victim->icon_id = icon_id;
    victim->size_px = size_px;
    victim->tick = ++g_iv_cache_tick;
    for (uint8_t role = 0; role < IV_ROLE_COUNT; ++role) {
        if (iv_uses_role(cmds, role))
            victim->masks[role] = iv_rasterize_role(cmds, size_px, role);
    }
    return victim;
}

//=============================================================================
// Public API
//=============================================================================

/// @copydoc vg_icon_vector_find
int32_t vg_icon_vector_find(const char *name) {
    if (!name || !*name)
        return VG_ICON_VECTOR_INVALID;
    for (int32_t i = 0; i < IV_ICON_COUNT; ++i) {
        if (strcmp(k_iv_registry[i].name, name) == 0)
            return i;
    }
    return VG_ICON_VECTOR_INVALID;
}

/// @copydoc vg_icon_vector_name
const char *vg_icon_vector_name(int32_t icon_id) {
    if (icon_id < 0 || icon_id >= IV_ICON_COUNT)
        return NULL;
    return k_iv_registry[icon_id].name;
}

/// @copydoc vg_icon_vector_count
int32_t vg_icon_vector_count(void) {
    return IV_ICON_COUNT;
}

/// @copydoc vg_icon_vector_cache_clear
void vg_icon_vector_cache_clear(void) {
    iv_cache_init();
    for (int32_t i = 0; i < IV_CACHE_CAPACITY; ++i)
        iv_cache_entry_free(&g_iv_cache[i]);
}

/// @copydoc vg_icon_vector_draw
void vg_icon_vector_draw(vgfx_window_t win,
                         int32_t icon_id,
                         int32_t x,
                         int32_t y,
                         int32_t size_px,
                         uint32_t tint_rgb) {
    if (!win || icon_id < 0 || icon_id >= IV_ICON_COUNT || size_px <= 0 || size_px > 512)
        return;
    vg_iv_cache_entry_t *entry = iv_cache_get(icon_id, size_px);
    if (!entry)
        return;
    for (int32_t role = 0; role < IV_ROLE_COUNT; ++role) {
        const uint8_t *mask = entry->masks[role];
        if (!mask)
            continue;
        uint32_t rgb = role == IV_ROLE_TINT ? (tint_rgb & 0x00FFFFFF)
                                            : (k_iv_role_rgb[role] & 0x00FFFFFF);
        for (int32_t row_index = 0; row_index < size_px; ++row_index) {
            const uint8_t *mask_row = mask + (size_t)row_index * (size_t)size_px;
            for (int32_t col = 0; col < size_px; ++col) {
                uint8_t alpha = mask_row[col];
                if (alpha)
                    vgfx_pset_alpha(win, x + col, y + row_index, ((uint32_t)alpha << 24) | rgb);
            }
        }
    }
}
