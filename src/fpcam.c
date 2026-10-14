// first person camera and crosshair.
//
// bk's own camera keeps running every frame (it follows the hidden banjo).
// right before the 3d scene is drawn we swap the viewport to steve's eyes,
// and swap bk's camera back right after, so bk's camera logic never notices.

#include "options.h"
#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"

// minecraft's near plane is 0.05 blocks. bk's is much further out, which
// would cut off blocks right in front of your face.
#define FP_NEAR_PLANE 8.0f

FpView gFpView;

static int fp_active = FALSE;   // inside the 3d draw with our camera
static f32 saved_pos[3];
static f32 saved_rot[3];
static f32 saved_fov;
// the scene draw's gfx/mtx pointers, so the return hook can draw the hand last
static Gfx **scene_gfx;
static Mtx **scene_mtx;

// steve's camera runs in every camera type (minecraft's camera always
// follows the player's look). bk's own camera is only used while steve is off.
// (only while steve drives: cutscenes, swimming etc. use bk's camera)
static int fp_wanted(void) {
    return gSteve.enabled && gSteve.driving && gSteveDrivingFrames >= 10 && D_8037BFB8;
}

RECOMP_HOOK("func_80334540") void fpcam_before_scene(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    f32 eye[3], rot[3];

    fp_active = FALSE;
    if (!fp_wanted()) {
        return;
    }

    viewport_getPosition_vec3f(saved_pos);
    viewport_getRotation_vec3f(saved_rot);
    saved_fov = sViewportFOVy;
    scene_gfx = gfx;
    scene_mtx = mtx;

    eye[0] = gSteve.render_pos[0];
    eye[1] = gSteve.render_pos[1] + steve_lerp(gSteve.o_eye_height, gSteve.eye_height, steve_partial_tick());
    eye[2] = gSteve.render_pos[2];
    // steve faces (sin yaw, cos yaw). bk's camera at yaw y faces (-sin y, -cos y),
    // so the camera yaw is steve's + 180. positive pitch looks up for both.
    rot[0] = gSteve.pitch;
    rot[1] = gSteve.yaw + 180.0f;
    rot[2] = 0.0f;

    gFpView.eye[0] = eye[0];
    gFpView.eye[1] = eye[1];
    gFpView.eye[2] = eye[2];

    // GameRenderer.bobView: minecraft multiplies a small translate + roll + pitch
    // into the projection. bk builds the projection from the viewport, so do the
    // same thing by nudging the camera. bk's projection is rotZ(roll)·rotX(-pitch)
    // ·rotY(-yaw), so a world rotation of a around z is roll a, around x is pitch -a.
    if (gSteve.driving) {
        f32 pt = steve_partial_tick();
        // ClientAvatarState.getBackwardsInterpolatedWalkDistance
        f32 wd = -(gSteve.walk_dist + (gSteve.walk_dist - gSteve.o_walk_dist) * pt);
        f32 bob = steve_lerp(gSteve.o_bob, gSteve.bob, pt);
        f32 sw = ml_sin_deg(wd * 180.0f);
        f32 cw = ml_cos_deg(wd * 180.0f);
        f32 cw2 = ml_cos_deg(wd * 180.0f - 11.4591559f); // cos(x - 0.2 rad)
        f32 tx = sw * bob * 0.5f * MC_BLOCK;
        f32 ty = -(cw < 0.0f ? -cw : cw) * bob * MC_BLOCK;
        f32 right[3], up[3], sy = ml_sin_deg(gSteve.yaw), cy = ml_cos_deg(gSteve.yaw);
        f32 sp = ml_sin_deg(gSteve.pitch), cp = ml_cos_deg(gSteve.pitch);
        int i;

        // steve's view basis (see steve_draw_hand)
        right[0] = -cy; right[1] = 0.0f; right[2] = sy;
        up[0] = -sy * sp; up[1] = cp; up[2] = -cy * sp;
        // moving the world by t in view space = moving the camera by -t
        for (i = 0; i < 3; i++) {
            eye[i] -= right[i] * tx + up[i] * ty;
        }
        rot[2] = sw * bob * 3.0f;
        rot[0] -= (cw2 < 0.0f ? -cw2 : cw2) * bob * 5.0f;
    }
    // Camera.setup for the detached types: front view turns the camera around
    // (yRot + 180, -xRot), then move(-getMaxZoom(4 blocks)) along its forward
    if (gSteve.camera_type != CAMERA_FIRST_PERSON) {
        f32 look[3], back[3], dist;
        int i;

        steve_look_dir(look);
        if (gSteve.camera_type == CAMERA_THIRD_PERSON_FRONT) {
            rot[0] = -rot[0];
            rot[1] += 180.0f;
            for (i = 0; i < 3; i++) back[i] = look[i];
        } else {
            for (i = 0; i < 3; i++) back[i] = -look[i];
        }
        dist = steve_camera_max_zoom(eye, back, 4.0f * MC_BLOCK);
        for (i = 0; i < 3; i++) {
            eye[i] += back[i] * dist;
        }
    }
    // GameRenderer.bobHurt: for the 10 hurt ticks the view tilts by up to 14
    // degrees (sin(t^4 * pi)), around the direction the hit came from:
    // rotY(-dir) * rotZ(tilt) * rotY(dir), i.e. roll * cos(dir) + pitch * sin(dir)
    if (gSteve.hurt_time > 0) {
        f32 h = ((f32)gSteve.hurt_time - steve_partial_tick()) / 10.0f;

        if (h > 0.0f) {
            f32 tilt;

            h = h * h * h * h;
            tilt = -ml_sin_deg(h * 180.0f) * 14.0f;
            rot[2] += tilt * ml_cos_deg(gSteve.hurt_dir);
            rot[0] += tilt * ml_sin_deg(gSteve.hurt_dir);
        }
    }
    gFpView.cam[0] = eye[0];
    gFpView.cam[1] = eye[1];
    gFpView.cam[2] = eye[2];

    viewport_setPosition_vec3f(eye);
    viewport_setRotation_vec3f(rot);
    // bk's camera is 40 degrees, minecraft's is 70 (x1.15 sprinting, eased).
    // viewport_update rebuilds the culling frustum, so set this first.
    sViewportFOVy = option_fov() * steve_lerp(gSteve.o_fov_mod, gSteve.fov_mod, steve_partial_tick());
    viewport_update();
    fp_active = TRUE;
}

RECOMP_HOOK_RETURN("func_80334540") void fpcam_after_scene(void) {
    if (!fp_active) {
        return;
    }
    // the hand goes on top of everything, like minecraft (it clears depth first)
    if (gSteve.driving && gSteve.first_person && !gHideGui) {
        steve_draw_hand(scene_gfx, scene_mtx);
    }
    // ScreenEffectRenderer: burning (after the hand)
    if (gSteve.driving && gSteve.first_person) {
        steve_draw_fire_overlay(scene_gfx, scene_mtx);
    }
    sViewportFOVy = saved_fov;
    viewport_setPosition_vec3f(saved_pos);
    viewport_setRotation_vec3f(saved_rot);
    viewport_update();
    fp_active = FALSE;
}

// the scene draw sets bk's near/far right before building the projection.
// pull the near plane in while we're in first person.
RECOMP_HOOK_RETURN("viewport_setNearAndFar") void fpcam_after_near_far(void) {
    if (fp_active && sViewportNear > FP_NEAR_PLANE) {
        sViewportNear = FP_NEAR_PLANE;
    }
}

// crosshair, drawn with the 2d text pass (after the 3d scene)
RECOMP_HOOK("printbuffer_draw") void fpcam_draw_crosshair(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    s32 cx, cy;

    if (!fp_wanted() || !gSteve.first_person) {
        return; // minecraft only shows the crosshair in first person
    }
#if __has_include("../generated/mc_textures.h")
    return; // hud.c draws minecraft's own crosshair sprite
#endif
    cx = gFramebufferWidth / 2;
    cy = gFramebufferHeight / 2;
    drawRectangle2D(gfx, cx - 2, cy, 5, 1, 0xFF, 0xFF, 0xFF);
    drawRectangle2D(gfx, cx, cy - 2, 1, 5, 0xFF, 0xFF, 0xFF);
    // leave the pipeline in a normal mode for the text that follows
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
}
