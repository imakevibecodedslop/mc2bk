// "bk_minecraft_mouse": passes the native library's mouse/keyboard functions
// on to the main mod. native library functions can only be imported by the
// mod that ships them, so this mod re-exports them. keeping it separate means
// the main mod still loads if the native library is missing.

#include "modding.h"

RECOMP_IMPORT(".", int mcm_poll(int want_capture));
RECOMP_IMPORT(".", int mcm_take_dx(void));
RECOMP_IMPORT(".", int mcm_take_dy(void));
RECOMP_IMPORT(".", int mcm_take_clicks(void));
RECOMP_IMPORT(".", int mcm_take_wheel(void));
RECOMP_IMPORT(".", int mcm_key(int scancode));
RECOMP_IMPORT(".", int mcm_buttons(void));
RECOMP_IMPORT(".", int mcm_window_size(void));
RECOMP_IMPORT(".", int mcm_cursor(void));
RECOMP_IMPORT(".", int mcm_save_begin(void));
RECOMP_IMPORT(".", int mcm_save_put(int b));
RECOMP_IMPORT(".", int mcm_save_commit(int slot));
RECOMP_IMPORT(".", int mcm_load_open(int slot));
RECOMP_IMPORT(".", int mcm_load_get(int i));
RECOMP_IMPORT(".", int mcm_save_delete(int slot));
RECOMP_IMPORT(".", int mcm_snd_play(int file, int vol, int pitch, int pan));
RECOMP_IMPORT(".", int mcm_snd_stop_all(void));
RECOMP_IMPORT(".", int mcm_skin_poll(void));
RECOMP_IMPORT(".", int mcm_skin_px(int i));

RECOMP_EXPORT int mc_mouse_window_size(void) {
    return mcm_window_size();
}

RECOMP_EXPORT int mc_mouse_cursor(void) {
    return mcm_cursor();
}

RECOMP_EXPORT int mc_mouse_buttons(void) {
    return mcm_buttons();
}

RECOMP_EXPORT int mc_mouse_poll(int want_capture) {
    return mcm_poll(want_capture);
}

RECOMP_EXPORT int mc_mouse_take_dx(void) {
    return mcm_take_dx();
}

RECOMP_EXPORT int mc_mouse_take_dy(void) {
    return mcm_take_dy();
}

RECOMP_EXPORT int mc_mouse_take_clicks(void) {
    return mcm_take_clicks();
}

RECOMP_EXPORT int mc_mouse_take_wheel(void) {
    return mcm_take_wheel();
}

RECOMP_EXPORT int mc_mouse_key(int scancode) {
    return mcm_key(scancode);
}

// save files for the steve mod (see the native library)
RECOMP_EXPORT int mc_save_begin(void) { return mcm_save_begin(); }
RECOMP_EXPORT int mc_save_put(int b) { return mcm_save_put(b); }
RECOMP_EXPORT int mc_save_commit(int slot) { return mcm_save_commit(slot); }
RECOMP_EXPORT int mc_load_open(int slot) { return mcm_load_open(slot); }
RECOMP_EXPORT int mc_load_get(int i) { return mcm_load_get(i); }
RECOMP_EXPORT int mc_save_delete(int slot) { return mcm_save_delete(slot); }

// minecraft sounds for the steve mod (see the native library)
RECOMP_EXPORT int mc_snd_play(int file, int vol, int pitch, int pan) {
    return mcm_snd_play(file, vol, pitch, pan);
}

RECOMP_EXPORT int mc_snd_stop_all(void) {
    return mcm_snd_stop_all();
}

// the custom skin (mc_skin.png in the config folder, see the native library)
RECOMP_EXPORT int mc_skin_poll(void) {
    return mcm_skin_poll();
}

RECOMP_EXPORT int mc_skin_px(int i) {
    return mcm_skin_px(i);
}
