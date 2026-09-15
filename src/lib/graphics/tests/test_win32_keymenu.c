//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_win32_keymenu.c
// Purpose: Prove that a lone Alt or F10 press no longer parks a ZannaGFX window
//          in the Win32 keyboard menu loop, which stopped the program's frame
//          loop, while Alt+Space still opens the window menu.
// Key invariants:
//   - Windows runs a keyboard menu loop only for the active foreground window.
//     Each probe activates its window first, and a plain DefWindowProc control
//     window proves the session runs the loop; otherwise the test reports a skip.
//   - A timer that fires inside the modal loop records the loop and ends it, so
//     a regression fails instead of hanging.
// Ownership/Lifetime: The test owns its control window and its ZannaGFX window
//                     and destroys both before exit.
// Links: src/lib/graphics/src/vgfx_platform_win32.c
//
//===----------------------------------------------------------------------===//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_harness.h"
#include "vgfx.h"

/// @brief Set by the probe timer when it observes a running menu loop.
static int g_menu_loop_seen = 0;

/// @brief Timer callback that records and ends a keyboard menu loop.
/// @details A modal menu loop dispatches timer messages, so this runs while
///          SendMessageW is still inside the loop.
/// @param hwnd Window owning the timer.
/// @param msg Timer message identifier (unused).
/// @param id Timer identifier.
/// @param now Tick count at dispatch (unused).
static void CALLBACK end_menu_loop(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)msg;
    (void)now;
    GUITHREADINFO info = {.cbSize = (DWORD)sizeof(GUITHREADINFO)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && (info.flags & GUI_INMENUMODE)) {
        g_menu_loop_seen = 1;
        KillTimer(hwnd, id);
        EndMenu();
    }
}

/// @brief Show @p hwnd, request activation, and drain pending messages.
/// @param hwnd Window to activate.
/// @return 1 when @p hwnd is the foreground window afterwards.
static int activate(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return GetForegroundWindow() == hwnd;
}

/// @brief Deliver a keyboard window-menu request and report whether a loop ran.
/// @param hwnd Active window receiving the request.
/// @param key Character that accompanied Alt (0 for a lone Alt or F10).
/// @return 1 when a menu loop ran inside the request.
static int keymenu_runs_menu_loop(HWND hwnd, LPARAM key) {
    g_menu_loop_seen = 0;
    UINT_PTR timer = SetTimer(hwnd, 0x4B4D, 20, end_menu_loop);
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_KEYMENU, key);
    KillTimer(hwnd, timer);
    return g_menu_loop_seen;
}

/// @brief Create the plain control window that uses DefWindowProcW directly.
/// @return Control window handle, or NULL on failure.
static HWND create_control_window(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ZannaKeyMenuControl";
    RegisterClassW(&wc);
    return CreateWindowExW(0,
                           wc.lpszClassName,
                           L"Zanna keyboard menu control",
                           WS_OVERLAPPEDWINDOW,
                           80,
                           80,
                           240,
                           160,
                           NULL,
                           NULL,
                           wc.hInstance,
                           NULL);
}

static void test_lone_alt_does_not_enter_menu_loop(void) {
    TEST_BEGIN("Win32: a lone Alt or F10 press leaves the frame loop running");

    HWND control = create_control_window();
    ASSERT_NOT_NULL(control);
    const int control_active = activate(control);
    const int control_loop = control_active && keymenu_runs_menu_loop(control, 0);
    DestroyWindow(control);
    if (!control_loop) {
        printf("  skipped: %s\n",
               control_active ? "Windows ran no keyboard menu loop for an active plain window"
                              : "this session could not activate a test window");
        TEST_END();
        return;
    }

    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 240;
    params.height = 160;
    params.title = "Zanna keyboard menu test";
    params.fps = -1;
    vgfx_window_t window = vgfx_create_window(&params);
    ASSERT_NOT_NULL(window);
    vgfx_native_handles_t handles;
    ASSERT_EQ(vgfx_get_native_handles(window, &handles), 1);
    ASSERT_EQ(handles.backend, VGFX_NATIVE_BACKEND_WIN32);
    HWND hwnd = (HWND)handles.surface;
    ASSERT_NOT_NULL(hwnd);

    if (!activate(hwnd)) {
        printf("  skipped: the ZannaGFX window could not be activated\n");
        vgfx_destroy_window(window);
        TEST_END();
        return;
    }
    const int lone_alt_loop = keymenu_runs_menu_loop(hwnd, 0);
    const int alt_letter_loop = keymenu_runs_menu_loop(hwnd, 'a');
    const int still_active = activate(hwnd);
    const int alt_space_loop = still_active && keymenu_runs_menu_loop(hwnd, ' ');
    const int pumps = vgfx_pump_events(window);
    vgfx_destroy_window(window);

    ASSERT_EQ(lone_alt_loop, 0);
    ASSERT_EQ(alt_letter_loop, 0);
    ASSERT_EQ(pumps, 1);
    if (still_active)
        ASSERT_EQ(alt_space_loop, 1);
    else
        printf("  note: activation was lost before the Alt+Space check\n");

    TEST_END();
}

int main(void) {
    printf("ZannaGFX Win32 keyboard menu tests\n");
    test_lone_alt_does_not_enter_menu_loop();
    TEST_SUMMARY();
    return TEST_RETURN_CODE();
}
