//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_messagebox.c
// Purpose: Localizable message-box and prompt runtime bindings with compatibility helpers,
//   semantic button roles, and reusable synchronous/asynchronous controllers.
//
// Key invariants:
//   - Compatibility helpers retain their blocking return contracts.
//   - ShowAsync never polls recursively and records one completion edge per terminal outcome.
//   - Stable button IDs are unique; Enter/Escape semantics derive from explicit roles, not labels.
//   - Lower close callbacks never destroy their dialog while vg_dialog_close is on the stack.
//
// Ownership/Lifetime:
//   - rt_messagebox_data_t is GC-managed and owns the lower dialog and copied button labels.
//   - Prompt Option results retain their runtime string independently of temporary widgets.
//
// Links: src/runtime/graphics/gui/rt_gui.h,
//        src/lib/gui/src/widgets/vg_dialog.c,
//        docs/adr/0109-gui-dialog-media-scheduling-and-automation.md
//
//===----------------------------------------------------------------------===//

#include "rt_gui_internal.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_trap.h"

#ifdef ZANNA_ENABLE_GRAPHICS

//=============================================================================
// Phase 5: MessageBox Dialog
//=============================================================================

#define RT_MESSAGEBOX_DATA_MAGIC UINT64_C(0x52544D5347424F58)

/// @brief Return the active GUI app for message box hosting (falls back to `s_current_app`).
/// @details The returned pointer is borrowed and may be NULL when neither an active polling app nor
///          a widget-construction context exists.
/// @return Borrowed host app, preferring the active app, or NULL.
static rt_gui_app_t *rt_messagebox_app(void) {
    rt_gui_app_t *app = rt_gui_get_active_app();
    return app ? app : s_current_app;
}

/// @brief Duplicate a message-box label with malloc ownership.
/// @details Custom button labels are released with `free` by the message-box
///          wrapper, so this helper avoids relying on platform-specific
///          `strdup` declarations for fallback labels.
/// @param text Source label to copy; NULL returns NULL.
/// @return Newly allocated copy, or NULL on invalid input, overflow, or OOM.
static char *rt_messagebox_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len > SIZE_MAX - 1u)
        return NULL;
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

/// @brief Return non-zero if the button label should be treated as a "cancel / close / no" action.
/// @details Performs exact ASCII case-insensitive comparisons for the three legacy English labels;
///          role-aware button APIs intentionally bypass this localization-sensitive inference.
/// @param label Borrowed NUL-terminated button label; may be NULL.
/// @return One for Cancel, Close, or No ignoring ASCII case, otherwise zero.
static int rt_messagebox_label_is_cancel(const char *label) {
    if (!label)
        return 0;
    return rt_gui_ascii_casecmp(label, "cancel") == 0 ||
           rt_gui_ascii_casecmp(label, "close") == 0 || rt_gui_ascii_casecmp(label, "no") == 0;
}

/// @brief Configure a dialog for modal presentation: enforce minimum width, apply font,
///        set modal root, center-show, and push onto the app's dialog stack.
/// @details Activates the host app, lazily applies its effective default font, constrains width to
///          the supported modal range, and verifies that the dialog became the top modal entry.
///          Invalid host/window/root/dialog state is rejected without entering an event loop.
/// @param app Borrowed live app that will own modal routing.
/// @param dlg Borrowed live lower dialog to present.
/// @return One when @p dlg is installed as the top modal dialog, otherwise zero.
static int rt_messagebox_prepare_modal(rt_gui_app_t *app, vg_dialog_t *dlg) {
    if (!app || !app->window || !app->root || !dlg)
        return 0;
    rt_gui_activate_app(app);
    rt_gui_ensure_default_font();
    rt_gui_apply_default_font((vg_widget_t *)dlg);
    if (dlg->min_width < 360)
        vg_dialog_set_size_constraints(dlg, 360, dlg->min_height, 720, dlg->max_height);
    vg_dialog_set_modal(dlg, true, app->root);
    vg_dialog_show_centered(dlg, app->root);
    rt_gui_push_dialog(app, dlg);
    return rt_gui_top_dialog(app) == dlg;
}

/// @brief Run the event loop until the dialog closes or the app signals shutdown.
/// @details Repeatedly polls and renders the supplied app without transferring ownership of either
///          argument. The dialog is removed from the app's modal stack on every exit path.
/// @param app Borrowed app whose normal poll/render loop should be driven.
/// @param dlg Borrowed dialog whose open flag terminates the nested loop.
/// @return Lower dialog result, or @c VG_DIALOG_RESULT_NONE for a NULL dialog.
static vg_dialog_result_t rt_messagebox_run_modal(rt_gui_app_t *app, vg_dialog_t *dlg) {
    while (dlg && dlg->is_open && app && !app->should_close) {
        rt_gui_app_poll(app);
        rt_gui_app_render(app);
    }
    if (app)
        rt_gui_remove_dialog(app, dlg);
    return dlg ? vg_dialog_get_result(dlg) : VG_DIALOG_RESULT_NONE;
}

/// @brief One-shot informational message box (single OK button, info icon). Blocks until user
/// dismisses. Returns 0 (no meaningful selection — the OK is the only choice).
/// @param title Runtime dialog title converted to GUI-safe UTF-8 for the call.
/// @param message Runtime body text converted to GUI-safe UTF-8 for the call.
/// @return Always zero, including allocation, presentation, and shutdown failure paths.
int64_t rt_messagebox_info(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_messagebox_app();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cmsg = rt_string_to_gui_cstr(message);
    vg_dialog_t *dlg = vg_dialog_message(rt_gui_cstr_or_empty(ctitle),
                                         rt_gui_cstr_or_empty(cmsg),
                                         VG_DIALOG_ICON_INFO,
                                         VG_DIALOG_BUTTONS_OK);
    if (ctitle)
        free(ctitle);
    if (cmsg)
        free(cmsg);
    if (!dlg)
        return 0;
    if (!rt_messagebox_prepare_modal(app, dlg)) {
        vg_widget_destroy(&dlg->base);
        return 0;
    }
    rt_messagebox_run_modal(app, dlg);
    vg_widget_destroy(&dlg->base);
    return 0;
}

/// @brief One-shot warning message box (single OK button, warning/exclamation icon). Always
/// returns 0; for accept/decline use `_confirm` or `_question`.
/// @param title Runtime dialog title converted to GUI-safe UTF-8 for the call.
/// @param message Runtime body text converted to GUI-safe UTF-8 for the call.
/// @return Always zero, including allocation, presentation, and shutdown failure paths.
int64_t rt_messagebox_warning(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_messagebox_app();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cmsg = rt_string_to_gui_cstr(message);
    vg_dialog_t *dlg = vg_dialog_message(rt_gui_cstr_or_empty(ctitle),
                                         rt_gui_cstr_or_empty(cmsg),
                                         VG_DIALOG_ICON_WARNING,
                                         VG_DIALOG_BUTTONS_OK);
    if (ctitle)
        free(ctitle);
    if (cmsg)
        free(cmsg);
    if (!dlg)
        return 0;
    if (!rt_messagebox_prepare_modal(app, dlg)) {
        vg_widget_destroy(&dlg->base);
        return 0;
    }
    rt_messagebox_run_modal(app, dlg);
    vg_widget_destroy(&dlg->base);
    return 0;
}

/// @brief One-shot error message box (single OK, red/error icon). Returns 0 always.
/// @param title Runtime dialog title converted to GUI-safe UTF-8 for the call.
/// @param message Runtime body text converted to GUI-safe UTF-8 for the call.
/// @return Always zero, including allocation, presentation, and shutdown failure paths.
int64_t rt_messagebox_error(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_messagebox_app();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cmsg = rt_string_to_gui_cstr(message);
    vg_dialog_t *dlg = vg_dialog_message(rt_gui_cstr_or_empty(ctitle),
                                         rt_gui_cstr_or_empty(cmsg),
                                         VG_DIALOG_ICON_ERROR,
                                         VG_DIALOG_BUTTONS_OK);
    if (ctitle)
        free(ctitle);
    if (cmsg)
        free(cmsg);
    if (!dlg)
        return 0;
    if (!rt_messagebox_prepare_modal(app, dlg)) {
        vg_widget_destroy(&dlg->base);
        return 0;
    }
    rt_messagebox_run_modal(app, dlg);
    vg_widget_destroy(&dlg->base);
    return 0;
}

/// @brief Yes/No question box (question icon). Returns 1 if user chose Yes, 0 for No or any
/// other dismissal (Esc, window close).
/// @param title Runtime dialog title converted to GUI-safe UTF-8 for the call.
/// @param message Runtime body text converted to GUI-safe UTF-8 for the call.
/// @return One only for an explicit Yes result; zero for No, dismissal, shutdown, or failure.
int64_t rt_messagebox_question(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_messagebox_app();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cmsg = rt_string_to_gui_cstr(message);
    vg_dialog_t *dlg = vg_dialog_message(rt_gui_cstr_or_empty(ctitle),
                                         rt_gui_cstr_or_empty(cmsg),
                                         VG_DIALOG_ICON_QUESTION,
                                         VG_DIALOG_BUTTONS_YES_NO);
    if (ctitle)
        free(ctitle);
    if (cmsg)
        free(cmsg);
    if (!dlg)
        return 0;
    if (!rt_messagebox_prepare_modal(app, dlg)) {
        vg_widget_destroy(&dlg->base);
        return 0;
    }
    vg_dialog_result_t result = rt_messagebox_run_modal(app, dlg);
    vg_widget_destroy(&dlg->base);
    return (result == VG_DIALOG_RESULT_YES) ? 1 : 0;
}

/// @brief OK/Cancel confirmation box (question icon). Returns 1 for OK, 0 for Cancel.
/// @param title Runtime dialog title converted to GUI-safe UTF-8 for the call.
/// @param message Runtime body text converted to GUI-safe UTF-8 for the call.
/// @return One only for an explicit OK result; zero for Cancel, dismissal, shutdown, or failure.
int64_t rt_messagebox_confirm(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_messagebox_app();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cmsg = rt_string_to_gui_cstr(message);
    vg_dialog_t *dlg = vg_dialog_message(rt_gui_cstr_or_empty(ctitle),
                                         rt_gui_cstr_or_empty(cmsg),
                                         VG_DIALOG_ICON_QUESTION,
                                         VG_DIALOG_BUTTONS_OK_CANCEL);
    if (ctitle)
        free(ctitle);
    if (cmsg)
        free(cmsg);
    if (!dlg)
        return 0;
    if (!rt_messagebox_prepare_modal(app, dlg)) {
        vg_widget_destroy(&dlg->base);
        return 0;
    }
    vg_dialog_result_t result = rt_messagebox_run_modal(app, dlg);
    vg_widget_destroy(&dlg->base);
    return (result == VG_DIALOG_RESULT_OK) ? 1 : 0;
}

// Prompt commit callback data
typedef struct {
    uint64_t magic;
    vg_dialog_t *dialog;
} rt_prompt_commit_data_t;

/// @brief Text-input `on_commit` callback — closes the prompt dialog as OK when Enter is pressed.
/// @param w Borrowed committing TextInput widget; unused because @p user_data owns routing.
/// @param text Borrowed committed text; collection occurs after modal exit, so this callback ignores it.
/// @param user_data Borrowed stack-scoped @ref rt_prompt_commit_data_t valid for the modal loop.
static void prompt_on_commit(vg_widget_t *w, const char *text, void *user_data) {
    (void)w;
    (void)text;
    rt_prompt_commit_data_t *d = (rt_prompt_commit_data_t *)user_data;
    if (d && d->dialog)
        vg_dialog_close(d->dialog, VG_DIALOG_RESULT_OK);
}

/// @brief Execute the shared synchronous prompt and report acceptance separately from its text.
/// @details The separate @p accepted result preserves an accepted empty string for the Option API,
///          while the compatibility wrapper can continue returning an empty sentinel for both
///          cancel and empty acceptance. The helper owns and destroys all temporary widgets.
/// @param title Runtime dialog title borrowed for the call.
/// @param message Runtime prompt label borrowed for the call.
/// @param accepted Required output set to one only for an explicit OK/Enter result.
/// @return Owned runtime string containing the accepted text, including the canonical empty string.
static rt_string rt_messagebox_prompt_impl(rt_string title, rt_string message, int *accepted) {
    RT_ASSERT_MAIN_THREAD();
    if (accepted)
        *accepted = 0;
    rt_gui_app_t *app = rt_messagebox_app();
    if (!app)
        return rt_str_empty();
    rt_gui_activate_app(app);
    rt_gui_ensure_default_font();

    char *ctitle = rt_string_to_gui_cstr(title);
    char *cmsg = rt_string_to_gui_cstr(message);

    vg_dialog_t *dlg = vg_dialog_create(rt_gui_cstr_or_empty(ctitle));
    if (ctitle)
        free(ctitle);
    if (!dlg) {
        if (cmsg)
            free(cmsg);
        return rt_str_empty();
    }

    // Show the prompt message above the text input
    if (cmsg) {
        vg_dialog_set_message(dlg, rt_gui_cstr_or_empty(cmsg));
        free(cmsg);
    }

    // Apply app font to dialog
    rt_gui_apply_default_font((vg_widget_t *)dlg);

    // Create the text input and attach it as dialog content.
    vg_textinput_t *input = vg_textinput_create(NULL);
    if (!input) {
        vg_widget_destroy((vg_widget_t *)dlg);
        return rt_str_empty();
    }

    rt_gui_apply_default_font((vg_widget_t *)input);
    input->base.constraints.min_width = 240.0f;
    input->base.constraints.preferred_width = 360.0f;
    vg_textinput_set_placeholder(input, "Enter a value");

    // When Enter is pressed inside the input, dismiss as OK
    rt_prompt_commit_data_t commit_data = {.dialog = dlg};
    vg_textinput_set_on_commit(input, prompt_on_commit, &commit_data);

    // Place the input as the dialog's content widget
    vg_dialog_set_content(dlg, (vg_widget_t *)input);
    vg_dialog_set_buttons(dlg, VG_DIALOG_BUTTONS_OK_CANCEL);
    vg_dialog_set_size_constraints(dlg, 420, 190, 760, 420);

    // Show and focus the input so the user can type immediately.
    if (!rt_messagebox_prepare_modal(app, dlg)) {
        vg_widget_destroy((vg_widget_t *)dlg);
        return rt_str_empty();
    }
    vg_widget_set_focus((vg_widget_t *)input);

    vg_dialog_result_t result_code = rt_messagebox_run_modal(app, dlg);

    // Collect result before destroying
    rt_string result = rt_str_empty();
    if (result_code == VG_DIALOG_RESULT_OK) {
        const char *text = vg_textinput_get_text(input);
        if (accepted)
            *accepted = 1;
        if (text)
            result = rt_string_from_bytes(text, strlen(text));
    }

    vg_widget_destroy((vg_widget_t *)dlg);
    return result;
}

/// @brief Show a single-line prompt using the legacy empty-string sentinel contract.
/// @details Pressing Enter accepts. Cancellation and accepted empty input both return an empty
///          string for compatibility; use @ref rt_messagebox_prompt_option to distinguish them.
/// @param title Dialog title.
/// @param message Prompt label shown above the input.
/// @return Entered text, or an empty string for cancel/empty acceptance/failure.
rt_string rt_messagebox_prompt(rt_string title, rt_string message) {
    int accepted = 0;
    return rt_messagebox_prompt_impl(title, message, &accepted);
}

/// @brief Show a single-line prompt with an explicit optional result.
/// @details `Some("")` represents accepted empty input and `None` represents cancellation or
///          inability to present. The Option retains the text before the local reference is
///          released.
/// @param title Dialog title.
/// @param message Prompt label shown above the input.
/// @return Managed `Zanna.Option[str]` preserving empty acceptance.
void *rt_messagebox_prompt_option(rt_string title, rt_string message) {
    int accepted = 0;
    rt_string value = rt_messagebox_prompt_impl(title, message, &accepted);
    if (!accepted) {
        rt_string_unref(value);
        return rt_option_none();
    }
    void *option = rt_option_some_str(value);
    rt_string_unref(value);
    return option;
}

// Custom MessageBox structure for tracking state
typedef struct {
    uint64_t magic;
    vg_dialog_t *dialog;
    int64_t result;
    int64_t status;
    const char *error;
    uint64_t completed_edges;
    int64_t default_button;
    int has_default_button;
    int64_t cancel_button;
    int has_cancel_button;
    rt_gui_app_t *owner_app;
    // Custom button tracking for rt_messagebox_add_button
    vg_dialog_button_def_t *custom_buttons;
    int64_t *custom_button_ids;
    int64_t *custom_button_roles;
    size_t custom_button_count;
    size_t custom_button_cap;
} rt_messagebox_data_t;

static rt_messagebox_data_t **s_messagebox_wrappers = NULL;
static size_t s_messagebox_wrapper_count = 0;
static size_t s_messagebox_wrapper_cap = 0;

/// @brief Record a wrapper in the global message-box registry (idempotent).
/// @details The registry is the source of truth for handle validation: a checked
///          cast only trusts an opaque `void*` once it is found here (then verifies
///          the magic tag), guarding against forged/freed handles. Capacity doubles from 8.
/// @param data GC-managed wrapper to register; ownership is not transferred.
/// @return 1 on success or if already present; 0 on overflow or realloc failure.
static int rt_messagebox_register_wrapper(rt_messagebox_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_messagebox_wrapper_count; i++) {
        if (s_messagebox_wrappers[i] == data)
            return 1;
    }
    if (s_messagebox_wrapper_count >= s_messagebox_wrapper_cap) {
        size_t new_cap = s_messagebox_wrapper_cap ? s_messagebox_wrapper_cap * 2 : 8;
        if (new_cap < s_messagebox_wrapper_cap ||
            new_cap > SIZE_MAX / sizeof(rt_messagebox_data_t *))
            return 0;
        void *p = realloc(s_messagebox_wrappers, new_cap * sizeof(rt_messagebox_data_t *));
        if (!p)
            return 0;
        s_messagebox_wrappers = (rt_messagebox_data_t **)p;
        s_messagebox_wrapper_cap = new_cap;
    }
    s_messagebox_wrappers[s_messagebox_wrapper_count++] = data;
    return 1;
}

/// @brief Remove a wrapper from the message-box registry, compacting the array. No-op if absent.
/// @param data Wrapper identity to remove; may be NULL.
static void rt_messagebox_unregister_wrapper(rt_messagebox_data_t *data) {
    if (!data)
        return;
    for (size_t i = 0; i < s_messagebox_wrapper_count; i++) {
        if (s_messagebox_wrappers[i] != data)
            continue;
        memmove(&s_messagebox_wrappers[i],
                &s_messagebox_wrappers[i + 1],
                (s_messagebox_wrapper_count - i - 1) * sizeof(*s_messagebox_wrappers));
        s_messagebox_wrapper_count--;
        return;
    }
}

/// @brief True if @p data is a currently-registered wrapper; backs handle validation.
/// @param data Candidate wrapper address; never dereferenced by this function.
/// @return One when pointer identity exists in the registry, otherwise zero.
static int rt_messagebox_wrapper_is_registered(const rt_messagebox_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_messagebox_wrapper_count; i++) {
        if (s_messagebox_wrappers[i] == data)
            return 1;
    }
    return 0;
}

/// @brief Record one completed MessageBox operation without wrapping its unread-edge count.
/// @param data Live registered wrapper whose terminal status/result are already populated.
static void rt_messagebox_record_completion(rt_messagebox_data_t *data) {
    if (data && data->completed_edges < UINT64_MAX)
        data->completed_edges++;
}

/// @brief Return whether an integer is a supported semantic dialog-button role.
/// @param role Candidate public role value.
/// @return Non-zero for `RT_GUI_DIALOG_BUTTON_NORMAL` through `HELP`.
static int rt_messagebox_role_is_valid(int64_t role) {
    return role >= RT_GUI_DIALOG_BUTTON_NORMAL && role <= RT_GUI_DIALOG_BUTTON_HELP;
}

/// @brief Locate a custom button by its stable application ID.
/// @param data Live MessageBox wrapper.
/// @param id Stable ID to search for.
/// @return Zero-based index, or `SIZE_MAX` when absent.
static size_t rt_messagebox_find_button(const rt_messagebox_data_t *data, int64_t id) {
    if (!data)
        return SIZE_MAX;
    for (size_t index = 0; index < data->custom_button_count; index++) {
        if (data->custom_button_ids && data->custom_button_ids[index] == id)
            return index;
    }
    return SIZE_MAX;
}

/// @brief Recompute lower Enter/Escape flags from semantic roles and explicit bindings.
/// @details Explicit SetDefaultButton/SetCancelButton choices take precedence. Without an explicit
///          choice, Default and Accept roles bind Enter, while Cancel and Reject roles bind Escape.
///          At most one button receives each binding, selected in insertion order.
/// @param data Live MessageBox wrapper with parallel button/id/role arrays.
static void rt_messagebox_apply_button_bindings(rt_messagebox_data_t *data) {
    if (!data)
        return;
    int assigned_default = 0;
    int assigned_cancel = 0;
    for (size_t index = 0; index < data->custom_button_count; index++) {
        int64_t id = data->custom_button_ids[index];
        int64_t role = data->custom_button_roles[index];
        int default_candidate =
            role == RT_GUI_DIALOG_BUTTON_DEFAULT || role == RT_GUI_DIALOG_BUTTON_ACCEPT;
        int cancel_candidate =
            role == RT_GUI_DIALOG_BUTTON_CANCEL || role == RT_GUI_DIALOG_BUTTON_REJECT;
        data->custom_buttons[index].is_default = data->has_default_button
                                                     ? id == data->default_button
                                                     : (!assigned_default && default_candidate);
        data->custom_buttons[index].is_cancel = data->has_cancel_button
                                                    ? id == data->cancel_button
                                                    : (!assigned_cancel && cancel_candidate);
        assigned_default = assigned_default || data->custom_buttons[index].is_default;
        assigned_cancel = assigned_cancel || data->custom_buttons[index].is_cancel;
    }
}

/// @brief Capture one lower dialog result into the asynchronous MessageBox state machine.
/// @details The callback runs inside `vg_dialog_close`, so it never destroys the lower object. A
///          custom result maps through the stable ID/role arrays; Cancel/Reject roles classify the
///          terminal state as Cancelled independent of translated labels.
/// @param dialog Borrowed lower dialog that has just closed.
/// @param result Lower result code assigned to the activated button or close action.
/// @param user_data Borrowed registered @ref rt_messagebox_data_t wrapper.
static void rt_messagebox_on_result(vg_dialog_t *dialog,
                                    vg_dialog_result_t result,
                                    void *user_data) {
    rt_messagebox_data_t *data = (rt_messagebox_data_t *)user_data;
    if (!rt_messagebox_wrapper_is_registered(data) || data->dialog != dialog ||
        data->status != RT_GUI_DIALOG_STATUS_OPEN) {
        return;
    }

    data->result = -1;
    data->error = "";
    int64_t index = (int64_t)result - (int64_t)VG_DIALOG_RESULT_CUSTOM_1;
    if (index >= 0 && (size_t)index < data->custom_button_count) {
        size_t selected = (size_t)index;
        int64_t role = data->custom_button_roles[selected];
        data->result = data->custom_button_ids[selected];
        data->status = role == RT_GUI_DIALOG_BUTTON_CANCEL || role == RT_GUI_DIALOG_BUTTON_REJECT
                           ? RT_GUI_DIALOG_STATUS_CANCELLED
                           : RT_GUI_DIALOG_STATUS_ACCEPTED;
    } else if (result == VG_DIALOG_RESULT_CANCEL || result == VG_DIALOG_RESULT_NONE) {
        data->result = result == VG_DIALOG_RESULT_CANCEL ? 2 : -1;
        data->status = RT_GUI_DIALOG_STATUS_CANCELLED;
    } else {
        if (result == VG_DIALOG_RESULT_OK || result == VG_DIALOG_RESULT_YES)
            data->result = 0;
        else if (result == VG_DIALOG_RESULT_NO)
            data->result = 1;
        data->status = RT_GUI_DIALOG_STATUS_ACCEPTED;
    }
    rt_messagebox_record_completion(data);
    if (rt_gui_is_app_handle(data->owner_app))
        rt_gui_remove_dialog(data->owner_app, dialog);
}

/// @brief Detach every stateful MessageBox wrapper that references a retiring lower dialog.
/// @details Open operations transition once to Failed with the stable no-app diagnostic and record
///          a completion edge. All matching wrappers then clear dialog/app pointers and reset the
///          selected result before lower storage can be released.
/// @param dialog Borrowed dialog being destroyed; NULL is a no-op.
void rt_messagebox_invalidate_dialog(vg_dialog_t *dialog) {
    if (!dialog)
        return;
    for (size_t i = 0; i < s_messagebox_wrapper_count; i++) {
        rt_messagebox_data_t *data = s_messagebox_wrappers[i];
        if (data && data->dialog == dialog) {
            if (data->status == RT_GUI_DIALOG_STATUS_OPEN) {
                data->status = RT_GUI_DIALOG_STATUS_FAILED;
                data->error = "No active GUI application is available";
                rt_messagebox_record_completion(data);
            }
            data->dialog = NULL;
            data->owner_app = NULL;
            data->result = -1;
        }
    }
}

/// @brief Authenticate a MessageBox handle via its magic tag (NULL if not).
/// @details Registry membership is checked before reading the wrapper, preventing dereference of
///          arbitrary, finalized, or forged runtime addresses.
/// @param box Candidate opaque runtime handle.
/// @return Borrowed live registered wrapper, or NULL when validation fails.
static rt_messagebox_data_t *rt_messagebox_checked(void *box) {
    rt_messagebox_data_t *data = (rt_messagebox_data_t *)box;
    return rt_messagebox_wrapper_is_registered(data) && data->magic == RT_MESSAGEBOX_DATA_MAGIC
               ? data
               : NULL;
}

/// @brief Release all resources: free custom button labels, destroy the VG dialog,
///        and remove it from the app's dialog stack.
/// @details Clears all parallel custom-button arrays, detaches/destroys any lower dialog, resets
///          result and magic state, and removes registry authentication. The GC-managed wrapper
///          allocation itself remains owned by the runtime.
/// @param data Registered or partially initialized wrapper to dispose; NULL is a no-op.
static void rt_messagebox_dispose(rt_messagebox_data_t *data) {
    if (!data)
        return;
    rt_gui_app_t *app =
        rt_gui_is_app_handle(data->owner_app) ? data->owner_app : rt_messagebox_app();
    for (size_t i = 0; i < data->custom_button_count; i++)
        free(data->custom_buttons[i].label);
    free(data->custom_buttons);
    free(data->custom_button_ids);
    free(data->custom_button_roles);
    data->custom_buttons = NULL;
    data->custom_button_ids = NULL;
    data->custom_button_roles = NULL;
    data->custom_button_count = 0;
    data->custom_button_cap = 0;
    if (data->dialog) {
        rt_gui_remove_dialog(app, data->dialog);
        vg_widget_destroy((vg_widget_t *)data->dialog);
        data->dialog = NULL;
    }
    data->result = -1;
    data->magic = 0;
    rt_messagebox_unregister_wrapper(data);
}

/// @brief GC finalizer — delegates to `rt_messagebox_dispose` to free custom button labels
///        and destroy the underlying VG dialog handle before the GC reclaims the object.
/// @param box GC-managed MessageBox wrapper being finalized.
static void rt_messagebox_finalize(void *box) {
    rt_messagebox_dispose((rt_messagebox_data_t *)box);
}

/// @brief Build a stateful MessageBox (icon by `type`: INFO/WARNING/ERROR/QUESTION). Buttons
/// start empty — call `_add_button` to register them, then `_show` to display modally. Returns
/// the GC-managed handle, or NULL on failure.
/// @details Converts/copies title and message into a new lower dialog, defaults unknown @p type
///          values to the information icon, allocates a GC-managed authenticated wrapper, installs
///          result/finalizer callbacks, and records the current host app without presenting.
///          Every partially allocated resource is released on failure.
/// @param title Runtime dialog title.
/// @param message Runtime dialog body.
/// @param type Public MessageBox icon kind.
/// @return New GC-managed stateful MessageBox handle, or NULL on allocation/registration failure.
void *rt_messagebox_new(rt_string title, rt_string message, int64_t type) {
    RT_ASSERT_MAIN_THREAD();
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_dialog_t *dlg = vg_dialog_create(rt_gui_cstr_or_empty(ctitle));
    if (ctitle)
        free(ctitle);
    if (!dlg)
        return NULL;

    char *cmsg = rt_string_to_gui_cstr(message);
    vg_dialog_set_message(dlg, rt_gui_cstr_or_empty(cmsg));
    if (cmsg)
        free(cmsg);

    vg_dialog_icon_t icon = VG_DIALOG_ICON_INFO;
    switch (type) {
        case RT_MESSAGEBOX_INFO:
            icon = VG_DIALOG_ICON_INFO;
            break;
        case RT_MESSAGEBOX_WARNING:
            icon = VG_DIALOG_ICON_WARNING;
            break;
        case RT_MESSAGEBOX_ERROR:
            icon = VG_DIALOG_ICON_ERROR;
            break;
        case RT_MESSAGEBOX_QUESTION:
            icon = VG_DIALOG_ICON_QUESTION;
            break;
    }
    vg_dialog_set_icon(dlg, icon);
    vg_dialog_set_buttons(dlg, VG_DIALOG_BUTTONS_NONE);

    rt_messagebox_data_t *data =
        (rt_messagebox_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_messagebox_data_t));
    if (!data) {
        vg_widget_destroy((vg_widget_t *)dlg);
        return NULL;
    }
    data->dialog = dlg;
    data->magic = RT_MESSAGEBOX_DATA_MAGIC;
    data->result = -1;
    data->status = RT_GUI_DIALOG_STATUS_IDLE;
    data->error = "";
    data->completed_edges = 0;
    data->default_button = 0;
    data->has_default_button = 0;
    data->cancel_button = 0;
    data->has_cancel_button = 0;
    data->owner_app = rt_messagebox_app();
    data->custom_buttons = NULL;
    data->custom_button_ids = NULL;
    data->custom_button_roles = NULL;
    data->custom_button_count = 0;
    data->custom_button_cap = 0;
    if (!rt_messagebox_register_wrapper(data)) {
        rt_messagebox_dispose(data);
        return NULL;
    }
    vg_dialog_set_on_result(dlg, rt_messagebox_on_result, data);
    rt_obj_set_finalizer(data, rt_messagebox_finalize);

    return data;
}

/// @brief Convenience: stateful MessageBox with INFO icon.
/// @param title Runtime dialog title.
/// @param message Runtime dialog body.
/// @return New GC-managed idle MessageBox, or NULL on failure.
void *rt_messagebox_new_info(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    return rt_messagebox_new(title, message, RT_MESSAGEBOX_INFO);
}

/// @brief Convenience: stateful MessageBox with WARNING icon.
/// @param title Runtime dialog title.
/// @param message Runtime dialog body.
/// @return New GC-managed idle MessageBox, or NULL on failure.
void *rt_messagebox_new_warning(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    return rt_messagebox_new(title, message, RT_MESSAGEBOX_WARNING);
}

/// @brief Convenience: stateful MessageBox with ERROR icon.
/// @param title Runtime dialog title.
/// @param message Runtime dialog body.
/// @return New GC-managed idle MessageBox, or NULL on failure.
void *rt_messagebox_new_error(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    return rt_messagebox_new(title, message, RT_MESSAGEBOX_ERROR);
}

/// @brief Convenience: stateful MessageBox with QUESTION icon.
/// @param title Runtime dialog title.
/// @param message Runtime dialog body.
/// @return New GC-managed idle MessageBox, or NULL on failure.
void *rt_messagebox_new_question(rt_string title, rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    return rt_messagebox_new(title, message, RT_MESSAGEBOX_QUESTION);
}

/// @brief Append one custom button using allocation-first parallel-array growth.
/// @details Stable IDs are unique. Duplicate rejection occurs before allocation or mutation and
///          traps with the public exact error. Compatibility callers may request label-based cancel
///          inference; role-aware callers never depend on translated text.
/// @param box Live MessageBox wrapper.
/// @param text Runtime UTF-8 button label.
/// @param id Stable application result ID.
/// @param role Valid semantic role, or Normal when the caller requested compatibility inference.
/// @param infer_cancel_from_label Non-zero only for legacy AddButton behavior.
static void rt_messagebox_add_button_impl(
    void *box, rt_string text, int64_t id, int64_t role, int infer_cancel_from_label) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || !data->dialog)
        return;
    if (rt_messagebox_find_button(data, id) != SIZE_MAX) {
        char error[96];
        snprintf(error, sizeof(error), "Message box button ID must be unique: %lld", (long long)id);
        rt_trap(error);
        return;
    }
    if (!rt_messagebox_role_is_valid(role))
        role = RT_GUI_DIALOG_BUTTON_NORMAL;

    // Grow the custom buttons array if needed
    if (data->custom_button_count >= data->custom_button_cap) {
        if (data->custom_button_cap > SIZE_MAX / 2) {
            return;
        }
        size_t new_cap = data->custom_button_cap ? data->custom_button_cap * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(vg_dialog_button_def_t) ||
            new_cap > SIZE_MAX / sizeof(int64_t)) {
            return;
        }
        vg_dialog_button_def_t *new_buttons =
            (vg_dialog_button_def_t *)calloc(new_cap, sizeof(vg_dialog_button_def_t));
        int64_t *new_ids = (int64_t *)calloc(new_cap, sizeof(int64_t));
        int64_t *new_roles = (int64_t *)calloc(new_cap, sizeof(int64_t));
        if (!new_buttons || !new_ids || !new_roles) {
            free(new_buttons);
            free(new_ids);
            free(new_roles);
            return;
        }
        if (data->custom_button_count > 0) {
            memcpy(new_buttons,
                   data->custom_buttons,
                   data->custom_button_count * sizeof(vg_dialog_button_def_t));
            memcpy(new_ids, data->custom_button_ids, data->custom_button_count * sizeof(int64_t));
            memcpy(
                new_roles, data->custom_button_roles, data->custom_button_count * sizeof(int64_t));
        }
        free(data->custom_buttons);
        free(data->custom_button_ids);
        free(data->custom_button_roles);
        data->custom_buttons = new_buttons;
        data->custom_button_ids = new_ids;
        data->custom_button_roles = new_roles;
        data->custom_button_cap = new_cap;
    }

    char *clabel = rt_string_to_gui_cstr(text);
    if (!clabel)
        clabel = rt_messagebox_strdup("OK");
    if (!clabel)
        return;
    size_t index = data->custom_button_count++;
    vg_dialog_button_def_t *btn = &data->custom_buttons[index];
    btn->label = clabel;
    btn->result = (vg_dialog_result_t)((int64_t)VG_DIALOG_RESULT_CUSTOM_1 + (int64_t)index);
    data->custom_button_ids[index] = id;
    data->custom_button_roles[index] =
        infer_cancel_from_label && rt_messagebox_label_is_cancel(btn->label)
            ? RT_GUI_DIALOG_BUTTON_CANCEL
            : role;
    rt_messagebox_apply_button_bindings(data);
}

/// @brief Append a legacy custom button while retaining historical cancel-label inference.
/// @param box Live MessageBox wrapper.
/// @param text Visible button label.
/// @param id Unique stable result ID.
void rt_messagebox_add_button(void *box, rt_string text, int64_t id) {
    rt_messagebox_add_button_impl(
        box, text, id, RT_GUI_DIALOG_BUTTON_NORMAL, /*infer_cancel_from_label=*/1);
}

/// @brief Append a custom button whose behavior is independent of its localized label.
/// @param box Live MessageBox wrapper.
/// @param text Visible localized label.
/// @param id Unique stable result ID.
/// @param role Semantic button role controlling keyboard binding and completion classification.
void rt_messagebox_add_button_with_role(void *box, rt_string text, int64_t id, int64_t role) {
    rt_messagebox_add_button_impl(box, text, id, role, /*infer_cancel_from_label=*/0);
}

/// @brief Update one existing button's semantic role and recompute keyboard bindings.
/// @param box Live MessageBox wrapper.
/// @param id Stable ID of the button to update.
/// @param role Supported semantic role.
/// @return 1 when updated, otherwise 0 for invalid role/handle or missing ID.
int64_t rt_messagebox_set_button_role(void *box, int64_t id, int64_t role) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || !data->dialog || !rt_messagebox_role_is_valid(role))
        return 0;
    size_t index = rt_messagebox_find_button(data, id);
    if (index == SIZE_MAX)
        return 0;
    data->custom_button_roles[index] = role;
    rt_messagebox_apply_button_bindings(data);
    return 1;
}

/// @brief Mark an existing custom button as the explicit Enter-key default.
/// @details Requires the stable ID to exist, records the explicit binding, and recomputes every
///          lower button flag so only the matching item receives Enter.
/// @param box Live registered MessageBox wrapper.
/// @param id Stable ID of an existing custom button.
/// @return One when the binding was installed, otherwise zero for invalid state or missing ID.
int64_t rt_messagebox_set_default_button(void *box, int64_t id) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || !data->dialog || rt_messagebox_find_button(data, id) == SIZE_MAX)
        return 0;
    data->default_button = id;
    data->has_default_button = 1;
    rt_messagebox_apply_button_bindings(data);
    return 1;
}

/// @brief Bind Escape to an existing custom button by stable ID.
/// @param box Live MessageBox wrapper.
/// @param id Existing unique button ID.
/// @return 1 when bound, otherwise 0.
int64_t rt_messagebox_set_cancel_button(void *box, int64_t id) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || !data->dialog || rt_messagebox_find_button(data, id) == SIZE_MAX)
        return 0;
    data->cancel_button = id;
    data->has_cancel_button = 1;
    rt_messagebox_apply_button_bindings(data);
    return 1;
}

/// @brief Present a MessageBox without entering a nested poll/render loop.
/// @details Custom button definitions are copied into the lower dialog immediately. Completion is
///          driven by ordinary app frames and captured by @ref rt_messagebox_on_result. Reopening
///          the same live object traps without changing its current operation.
/// @param box Live stateful MessageBox wrapper.
/// @return 1 when presentation started, otherwise 0.
int64_t rt_messagebox_show_async(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || !data->dialog)
        return 0;
    if (data->status == RT_GUI_DIALOG_STATUS_OPEN || vg_dialog_is_open(data->dialog)) {
        rt_trap("GUI dialog is already open");
        return 0;
    }
    rt_gui_app_t *app =
        rt_gui_is_app_handle(data->owner_app) ? data->owner_app : rt_messagebox_app();
    data->result = -1;
    data->error = "";
    if (!app || !app->window || !app->root) {
        data->status = RT_GUI_DIALOG_STATUS_FAILED;
        data->error = "No active GUI application is available";
        rt_messagebox_record_completion(data);
        return 0;
    }
    data->owner_app = app;

    if (data->custom_button_count > 0) {
        rt_messagebox_apply_button_bindings(data);
        for (size_t i = 0; i < data->custom_button_count; i++) {
            data->custom_buttons[i].result =
                (vg_dialog_result_t)((int64_t)VG_DIALOG_RESULT_CUSTOM_1 + (int64_t)i);
        }
        vg_dialog_set_custom_buttons(data->dialog, data->custom_buttons, data->custom_button_count);
    }

    data->status = RT_GUI_DIALOG_STATUS_OPEN;
    if (!rt_messagebox_prepare_modal(app, data->dialog)) {
        vg_dialog_hide(data->dialog);
        data->status = RT_GUI_DIALOG_STATUS_FAILED;
        data->error = "GUI dialog could not be presented";
        rt_messagebox_record_completion(data);
        return 0;
    }
    return 1;
}

/// @brief Display the dialog with the legacy blocking convenience contract.
/// @details Presentation is initialized through ShowAsync, then this compatibility wrapper alone
///          drives the nested loop. New event handlers should call ShowAsync to avoid reentrancy.
/// @param box Live MessageBox wrapper.
/// @return Selected stable button ID, preset compatibility code, or -1 on cancel/failure.
int64_t rt_messagebox_show(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || !rt_messagebox_show_async(box))
        return -1;
    rt_messagebox_run_modal(data->owner_app, data->dialog);
    if (data->status == RT_GUI_DIALOG_STATUS_OPEN)
        vg_dialog_close(data->dialog, VG_DIALOG_RESULT_CANCEL);
    return data->result;
}

/// @brief Query whether a stateful MessageBox is currently open.
/// @param box MessageBox wrapper; invalid/destroyed handles are treated as closed.
/// @return 1 only while both runtime status and lower visibility are Open.
int64_t rt_messagebox_is_open(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    return data && data->dialog && data->status == RT_GUI_DIALOG_STATUS_OPEN &&
                   vg_dialog_is_open(data->dialog)
               ? 1
               : 0;
}

/// @brief Consume one unread MessageBox completion edge.
/// @param box Registered MessageBox wrapper.
/// @return 1 for an unread accepted/cancelled/failed completion, otherwise 0.
int64_t rt_messagebox_was_completed(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data || data->completed_edges == 0)
        return 0;
    data->completed_edges--;
    return 1;
}

/// @brief Return the current MessageBox state-machine status without consuming it.
/// @param box Registered MessageBox wrapper.
/// @return One of `RT_GUI_DIALOG_STATUS_*`, or Failed for an invalid handle.
int64_t rt_messagebox_get_status(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    return data ? data->status : RT_GUI_DIALOG_STATUS_FAILED;
}

/// @brief Return the stable result ID from the most recent completed MessageBox.
/// @param box Registered MessageBox wrapper.
/// @return Custom ID/preset compatibility code, or -1 when no action was selected.
int64_t rt_messagebox_get_result(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    return data ? data->result : -1;
}

/// @brief Copy the most recent MessageBox failure diagnostic into a runtime string.
/// @param box Registered MessageBox wrapper.
/// @return Owned runtime string, empty when no error is recorded.
rt_string rt_messagebox_get_error(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    const char *error = data && data->error ? data->error : "";
    return rt_string_from_bytes(error, strlen(error));
}

/// @brief Manually free dialog resources (custom buttons, backend handle). The GC finalizer
/// also calls this — explicit destruction is optional but useful for early cleanup.
/// @param box Candidate registered MessageBox wrapper; invalid or already disposed handles are ignored.
void rt_messagebox_destroy(void *box) {
    RT_ASSERT_MAIN_THREAD();
    rt_messagebox_data_t *data = rt_messagebox_checked(box);
    if (!data)
        return;
    rt_messagebox_dispose(data);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: graphics disabled — no dialog shown; returns 0 immediately.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always zero.
int64_t rt_messagebox_info(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return 0;
}

/// @brief Stub: graphics disabled — no dialog shown; returns 0 immediately.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always zero.
int64_t rt_messagebox_warning(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return 0;
}

/// @brief Stub: graphics disabled — no dialog shown; returns 0 immediately.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always zero.
int64_t rt_messagebox_error(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return 0;
}

/// @brief Stub: graphics disabled — no Yes/No dialog; returns 0 (treated as "No").
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always zero, the compatibility No/dismissed result.
int64_t rt_messagebox_question(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return 0;
}

/// @brief Stub: graphics disabled — no OK/Cancel dialog; returns 0 (treated as "Cancel").
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always zero, the compatibility Cancel/dismissed result.
int64_t rt_messagebox_confirm(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return 0;
}

/// @brief Stub: graphics disabled — no text-input prompt; returns empty string immediately.
/// @param title Ignored runtime title.
/// @param message Ignored runtime prompt label.
/// @return Canonical empty runtime string.
rt_string rt_messagebox_prompt(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return rt_str_empty();
}

/// @brief Stub: graphics-disabled prompts always return `None`.
/// @param title Ignored runtime title.
/// @param message Ignored runtime prompt label.
/// @return Managed None option indicating no accepted prompt.
void *rt_messagebox_prompt_option(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return rt_option_none();
}

/// @brief Stub: graphics disabled — returns NULL; no stateful dialog object is created.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @param type Ignored public icon kind.
/// @return Always NULL.
void *rt_messagebox_new(rt_string title, rt_string message, int64_t type) {
    (void)title;
    (void)message;
    (void)type;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no info dialog object is created.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always NULL.
void *rt_messagebox_new_info(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no warning dialog object is created.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always NULL.
void *rt_messagebox_new_warning(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no error dialog object is created.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always NULL.
void *rt_messagebox_new_error(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no question dialog object is created.
/// @param title Ignored runtime title.
/// @param message Ignored runtime body.
/// @return Always NULL.
void *rt_messagebox_new_question(rt_string title, rt_string message) {
    (void)title;
    (void)message;
    return NULL;
}

/// @brief Stub: graphics disabled — no-op; button registration is silently ignored.
/// @param box Ignored candidate MessageBox handle.
/// @param text Ignored runtime button label.
/// @param id Ignored stable result ID.
void rt_messagebox_add_button(void *box, rt_string text, int64_t id) {
    (void)box;
    (void)text;
    (void)id;
}

/// @brief Stub: semantic button registration is ignored without graphics.
/// @param box Ignored candidate MessageBox handle.
/// @param text Ignored runtime button label.
/// @param id Ignored stable result ID.
/// @param role Ignored semantic role.
void rt_messagebox_add_button_with_role(void *box, rt_string text, int64_t id, int64_t role) {
    (void)box;
    (void)text;
    (void)id;
    (void)role;
}

/// @brief Stub: no button role can be updated without a dialog object.
/// @param box Ignored candidate MessageBox handle.
/// @param id Ignored stable result ID.
/// @param role Ignored semantic role.
/// @return Always zero.
int64_t rt_messagebox_set_button_role(void *box, int64_t id, int64_t role) {
    (void)box;
    (void)id;
    (void)role;
    return 0;
}

/// @brief Stub: no cancel binding exists without a dialog object.
/// @param box Ignored candidate MessageBox handle.
/// @param id Ignored stable result ID.
/// @return Always zero.
int64_t rt_messagebox_set_cancel_button(void *box, int64_t id) {
    (void)box;
    (void)id;
    return 0;
}

/// @brief Stub: graphics disabled — no-op; default button selection is silently ignored.
/// @param box Ignored candidate MessageBox handle.
/// @param id Ignored stable result ID.
/// @return Always zero.
int64_t rt_messagebox_set_default_button(void *box, int64_t id) {
    (void)box;
    (void)id;
    return 0;
}

/// @brief Stub: asynchronous presentation cannot start without graphics.
/// @param box Ignored candidate MessageBox handle.
/// @return Always zero.
int64_t rt_messagebox_show_async(void *box) {
    (void)box;
    return 0;
}

/// @brief Stub: graphics disabled — returns -1 (no dialog to show, no button chosen).
/// @param box Ignored candidate MessageBox handle.
/// @return Always -1.
int64_t rt_messagebox_show(void *box) {
    (void)box;
    return -1;
}

/// @brief Stub: no MessageBox is open without graphics.
/// @param box Ignored candidate MessageBox handle.
/// @return Always zero.
int64_t rt_messagebox_is_open(void *box) {
    (void)box;
    return 0;
}

/// @brief Stub: no stateful MessageBox completion exists without graphics.
/// @param box Ignored candidate MessageBox handle.
/// @return Always zero.
int64_t rt_messagebox_was_completed(void *box) {
    (void)box;
    return 0;
}

/// @brief Stub: absent MessageBox handles report Failed.
/// @param box Ignored candidate MessageBox handle.
/// @return @c RT_GUI_DIALOG_STATUS_FAILED.
int64_t rt_messagebox_get_status(void *box) {
    (void)box;
    return RT_GUI_DIALOG_STATUS_FAILED;
}

/// @brief Stub: no custom result is available without graphics.
/// @param box Ignored candidate MessageBox handle.
/// @return Always -1.
int64_t rt_messagebox_get_result(void *box) {
    (void)box;
    return -1;
}

/// @brief Stub: return the stable graphics-disabled capability diagnostic.
/// @param box Ignored candidate MessageBox handle.
/// @return Runtime string describing unavailable GUI support.
rt_string rt_messagebox_get_error(void *box) {
    (void)box;
    return rt_const_cstr("GUI support is not available in this build");
}

/// @brief Stub: graphics disabled — no-op; nothing to destroy when graphics are absent.
/// @param box Ignored candidate MessageBox handle.
void rt_messagebox_destroy(void *box) {
    (void)box;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
