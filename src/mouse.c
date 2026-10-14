// glue to the optional mouse/keyboard companion mod (see mouse.h, mouse/).

#include "modding.h"
#include "recomputils.h"

#include "mouse.h"

#define MOUSE_MOD_ID "bk_minecraft_mouse"

// exported by the companion mod. only call these after checking it's loaded.
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_poll(int want_capture));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_take_dx(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_take_dy(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_take_clicks(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_take_wheel(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_key(int scancode));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_buttons(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_window_size(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_mouse_cursor(void));

// minecraft at default sensitivity (0.5): (0.5*0.6+0.2)^3 * 8 * 0.15 = 0.15 deg per pixel
#define MOUSE_DEG_PER_PIXEL 0.15f

MouseFrame gMouse;

static int checked = FALSE;
static int mod_present = FALSE;
static int pending_dx, pending_dy;

// key edge detection for the handful of keys we use as buttons
#define TRACKED_KEYS 20
// 1-9 (hotbar), f5, left ctrl, left shift, f3, f4, q, e, esc, f1
static const int tracked_keys[TRACKED_KEYS] = { 30, 31, 32, 33, 34, 35, 36, 37, 38, KEY_F5, KEY_LCTRL, KEY_LSHIFT, KEY_F3, KEY_F4, KEY_Q, KEY_E, KEY_ESCAPE, KEY_F1, KEY_F, KEY_L };
static u8 key_now[TRACKED_KEYS];
static u8 key_prev[TRACKED_KEYS];

void mouse_frame(int want_capture) {
    int flags, clicks, i;

    if (!checked) {
        checked = TRUE;
        mod_present = recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
        recomp_printf("[mc] mouse companion mod: %s\n", mod_present ? "found" : "not installed");
    }
    if (!mod_present) {
        gMouse.available = FALSE;
        return;
    }

    flags = mc_mouse_poll(want_capture);
    gMouse.available = (flags & 1) != 0;
    gMouse.captured = (flags & 2) != 0;
    if (!gMouse.available) {
        return;
    }

    pending_dx = mc_mouse_take_dx();
    pending_dy = mc_mouse_take_dy();
    clicks = mc_mouse_take_clicks();
    gMouse.left_clicks = clicks & 0xFF;
    gMouse.right_clicks = (clicks >> 8) & 0xFF;
    gMouse.middle_clicks = (clicks >> 16) & 0xFF;
    gMouse.wheel = mc_mouse_take_wheel();
    {
        int b = mc_mouse_buttons();
        int ws = mc_mouse_window_size();
        int cur = mc_mouse_cursor();

        gMouse.left_held = (b & 1) != 0;
        gMouse.right_held = (b & 2) != 0;
        gMouse.window_w = (ws >> 16) & 0xFFFF;
        gMouse.window_h = ws & 0xFFFF;
        gMouse.cursor_x = (cur >> 16) & 0xFFFF;
        gMouse.cursor_y = cur & 0xFFFF;
    }

    for (i = 0; i < TRACKED_KEYS; i++) {
        key_prev[i] = key_now[i];
        key_now[i] = mc_mouse_key(tracked_keys[i]) != 0;
    }
}

int mouse_get_look_delta(f32 *dx_deg, f32 *dy_deg) {
    if (!gMouse.available || !gMouse.captured) {
        return FALSE;
    }
    *dx_deg = pending_dx * MOUSE_DEG_PER_PIXEL;
    *dy_deg = pending_dy * MOUSE_DEG_PER_PIXEL;
    pending_dx = pending_dy = 0;
    return TRUE;
}

int mouse_key_held(int scancode) {
    int i;

    if (!gMouse.available) {
        return FALSE;
    }
    for (i = 0; i < TRACKED_KEYS; i++) {
        if (tracked_keys[i] == scancode) {
            return key_now[i];
        }
    }
    return mc_mouse_key(scancode) != 0;
}

int mouse_key_released(int scancode) {
    int i;

    if (!gMouse.available) {
        return FALSE;
    }
    for (i = 0; i < TRACKED_KEYS; i++) {
        if (tracked_keys[i] == scancode) {
            return !key_now[i] && key_prev[i];
        }
    }
    return FALSE;
}

int mouse_key_pressed(int scancode) {
    int i;

    if (!gMouse.available) {
        return FALSE;
    }
    for (i = 0; i < TRACKED_KEYS; i++) {
        if (tracked_keys[i] == scancode) {
            return key_now[i] && !key_prev[i];
        }
    }
    return FALSE;
}
