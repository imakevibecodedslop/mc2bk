// debug readout + drawing glue.
//
// - hooks banjo's per-frame player update (func_80295C14): counts frames per
//   second for the f3 screen (f3.c) and logs once a second to the console
//   (recomp_printf). nothing is printed on screen in banjo mode.
// - d-pad down toggles hiding banjo's model by hand (m0 test).
// - steve mode (d-pad up, see steve.c) always hides banjo and draws steve.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "PR/os_time.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "mouse.h"
#include "survival.h"
#include "terrain.h"

int gShowDebug = FALSE;
int gHideGui = FALSE;
u32 gFps; // game frames in the last second (the f3 screen's fps)
u32 gMcFrame; // drawn frames (per-frame vertex pools)

// osGetTime() ticks per second (OS_CPU_COUNTER for ntsc).
#define MC_OS_TICKS_PER_SEC 46875000u

static int mc_started = FALSE;
static int mc_hide_banjo = FALSE;

// per-second stats
static OSTime mc_last_second_time;
static u32 mc_updates_this_second;
static f32 mc_game_time_this_second;
static u32 mc_updates_per_sec;
static u32 mc_delta_ms;

// runs after banjo's whole per-frame update (input, state, physics, collision...)
RECOMP_HOOK_RETURN("func_80295C14") void mc_on_player_update(void) {
    OSTime now = osGetTime();
    f32 dt = time_getDelta();

    if (!mc_started) {
        mc_started = TRUE;
        mc_last_second_time = now;
        recomp_printf("[mc] hooks are firing (player update)\n");
    }

    // d-pad down: drop the held item (minecraft's q) while steve plays,
    // otherwise the old hide-banjo debug toggle
    if (bakey_pressed(BUTTON_D_DOWN)) {
        if (gSteve.enabled) {
            surv_drop_selected(FALSE);
        } else {
            mc_hide_banjo = !mc_hide_banjo;
            recomp_printf("[mc] hide banjo: %d\n", mc_hide_banjo);
        }
    }

    mc_updates_this_second++;
    mc_game_time_this_second += dt;
    mc_delta_ms = (u32)(dt * 1000.0f + 0.5f);

    // fits in u32 for ~90 seconds, so no 64 bit division needed
    if ((u32)(now - mc_last_second_time) >= MC_OS_TICKS_PER_SEC) {
        f32 pos[3];

        mc_updates_per_sec = mc_updates_this_second;
        gFps = mc_updates_per_sec;
        player_getPosition(pos);
        recomp_printf("[mc] map %d  pos %d %d %d  updates/s %d  game time/s %d ms  dt %d ms  steve %d/%d ticks %d\n",
            map_get(), (s32)pos[0], (s32)pos[1], (s32)pos[2],
            mc_updates_this_second, (s32)(mc_game_time_this_second * 1000.0f), mc_delta_ms,
            gSteve.enabled, gSteve.driving, gSteve.tick_count);

        mc_updates_this_second = 0;
        mc_game_time_this_second = 0.0f;
        mc_last_second_time = now;
    }

}

// hide banjo by clearing the model's visible flag just for the draw call, then
// restoring it, so the game's own visibility logic is left alone.
// a return hook doesn't get args, so the pre hook does the work.
static int mc_saved_visible;
static int mc_did_hide;

// everything the mod draws in the world goes into its own display list (one
// per frame, three in a ring so the gpu can still read the last ones). bk's
// per-frame list only gets a call to it: lots of hole walls, water, chests or
// tnt can't overflow bk's gfx buffer then
#define MC_WORLD_GFX 32768
static Gfx mc_world_gfx[3][MC_WORLD_GFX];
static int mc_world_gfx_frame;

RECOMP_HOOK("player_draw") void mc_before_player_draw(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    Gfx *start, *g, **sub;

    mc_did_hide = FALSE;
    mc_world_gfx_frame = (mc_world_gfx_frame + 1) % 3;
    gMcFrame++;
    start = g = mc_world_gfx[mc_world_gfx_frame];
    sub = &g;
    world_draw(sub, mtx);
    terrain_draw(sub, mtx);
    if (gSteve.enabled) {
        surv_draw_world(sub, mtx);
    }
    // in first person you're inside steve's head, so don't draw him
    if (gSteve.enabled && D_8037BFB8 && (!gSteve.first_person || !gSteve.driving) && !steve_transformed()) {
        steve_draw(sub, mtx);
    }
    gSPEndDisplayList(g++);
    if (g - start > MC_WORLD_GFX) {
        recomp_printf("[mc] world gfx list overflow: %d commands\n", (s32)(g - start));
    }
    gSPDisplayList((*gfx)++, start);
    if (mc_hide_banjo || (gSteve.enabled && !steve_transformed())) {
        mc_saved_visible = baModel_isVisible();
        baModel_setVisible(FALSE);
        mc_did_hide = TRUE;
    }
}

RECOMP_HOOK_RETURN("player_draw") void mc_after_player_draw(void) {
    if (mc_did_hide) {
        baModel_setVisible(mc_saved_visible);
        mc_did_hide = FALSE;
    }
}

// banjo's touch spheres (what collects notes, gets hit by enemies, presses
// switches) sit on bones of his model, and those bone positions only update
// when the model is drawn (baModel_80292284 reads them). we never draw banjo
// in steve mode, so the spheres stayed frozen where banjo was last drawn.
// while the marker collision runs, say banjo is invisible: the game then uses
// its fallback, player position + 75 / + 33 units, which follows steve.
static int mc_marker_saved_visible;
static int mc_marker_hid;

RECOMP_HOOK("baMarker_update") void mc_before_marker_update(void) {
    mc_marker_hid = FALSE;
    if (gSteve.enabled) {
        mc_marker_saved_visible = baModel_isVisible();
        baModel_setVisible(FALSE);
        mc_marker_hid = TRUE;
    }
}

RECOMP_HOOK_RETURN("baMarker_update") void mc_after_marker_update(void) {
    if (mc_marker_hid) {
        baModel_setVisible(mc_marker_saved_visible);
        mc_marker_hid = FALSE;
    }
}
