//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_headless_egl.cpp
// Purpose: Prove the Linux OpenGL adapter can create an EGL pbuffer without a display server.
// Key invariants: The test never creates an X11/Wayland window or reads display environment state.
// Ownership/Lifetime: The test releases every successfully created EGL binding before exit.
// Links: docs/adr/0191-gpu-offscreen-editor-rendering.md
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "vgfx3d_egl_wayland.h"
}

#include <cstdio>

int main() {
    if (!vgfx3d_egl_available()) {
        std::puts("Headless EGL test skipped: no runtime EGL implementation");
        return 0;
    }
    vgfx3d_egl_wayland_t *binding = vgfx3d_egl_headless_create(64, 48);
    if (!binding) {
        std::fputs("EGL is installed but a headless OpenGL pbuffer could not be created\n", stderr);
        return 1;
    }
    if (!vgfx3d_egl_wayland_make_current(binding)) {
        std::fputs("headless EGL context could not be made current a second time\n", stderr);
        vgfx3d_egl_wayland_destroy(binding);
        return 1;
    }
    vgfx3d_egl_wayland_destroy(binding);
    std::puts("Headless EGL OpenGL pbuffer created successfully");
    return 0;
}
