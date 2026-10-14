#ifndef __MC_MOUSE_H__
#define __MC_MOUSE_H__

#include <ultra64.h>

// mouse + keyboard, through the optional companion mod "bk_minecraft_mouse"
// (mouse/ in this repo). banjo: recompiled doesn't give mods the mouse, so
// that mod ships a small native library that reads it from sdl.
// everything here quietly does nothing if the companion mod isn't installed.

// sdl scancodes we use
#define KEY_1 30
#define KEY_5 34
#define KEY_ESCAPE 41
#define KEY_F1 58
#define KEY_F3 60
#define KEY_F4 61
#define KEY_F5 62
#define KEY_Q 20
#define KEY_E 8
#define KEY_F 9 // keySwapOffhand
#define KEY_L 15 // keyAdvancements
#define KEY_LCTRL 224
#define KEY_LSHIFT 225

typedef struct {
    int available;   // companion mod loaded and found sdl
    int captured;    // cursor locked for mouse look
    int left_clicks; // presses since last frame
    int right_clicks;
    int middle_clicks;
    int wheel;       // scroll steps since last frame, + = up
    int left_held;   // buttons down right now
    int right_held;
    int window_w, window_h; // game window in pixels (0 if unknown)
    int cursor_x, cursor_y; // cursor in window pixels (for screens)
} MouseFrame;

extern MouseFrame gMouse;

// call once per game frame. `want_capture`: 1 = mouse look, 2 = a screen is
// open (free cursor, clicks still counted), 0 = leave the mouse alone.
void mouse_frame(int want_capture);
// look delta in degrees (minecraft default sensitivity). FALSE if no mouse.
int mouse_get_look_delta(f32 *dx_deg, f32 *dy_deg);
// keyboard key held (sdl scancode). FALSE if no companion mod.
int mouse_key_held(int scancode);
// key let go this frame
int mouse_key_released(int scancode);
// key newly pressed this frame
int mouse_key_pressed(int scancode);

#endif
