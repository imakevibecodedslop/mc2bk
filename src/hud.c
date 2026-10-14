// minecraft's hud, ported from Hud.java (26.3): hotbar, selection, item
// slots, hearts (hurt blinking, low health shake), food (shakes without
// saturation), air bubbles, experience bar, crosshair. drawn in minecraft's
// gui space at minecraft's gui scale (see gui.c), with the real sprites.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "gui.h"
#include "screen.h"
#include "items.h"
#include "hotbars.h"
#include "xp.h"
#include "f3.h"
#include "advancements.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#ifdef MC_HAVE_TEXTURES

#define SPRITE(name, x, y) \
    gui_blit(gfx, mc_hud_##name, MC_HUD_##name##_W, MC_HUD_##name##_H, 0, 0, MC_HUD_##name##_W, MC_HUD_##name##_H, \
        x, y, MC_HUD_##name##_W, MC_HUD_##name##_H)

// heart blinking (Hud: healthBlinkTime / lastHealth)
static s32 last_hp = -1;
static u32 blink_until;
static s32 blink_old_hp;

// Hud.getCurrentAirSupplyBubble: ceil(ticks * 10 / 300)
static s32 bubble_count(s32 ticks) {
    s32 n = ticks * 10;
    return (n <= 0) ? 0 : (n + 299) / 300;
}

static void hud_hotbar(Gfx **gfx, s32 w, s32 h) {
    s32 center = w / 2, i;

    SPRITE(hotbar, center - 91, h - 22);
    SPRITE(hotbar_selection, center - 91 - 1 + gSurv.selected * 20, h - 22 - 1);
    for (i = 0; i < 9; i++) {
        gui_item(gfx, &gSurv.inv[i], center - 90 + i * 20 + 2, h - 16 - 3, TRUE);
    }
}

static void hud_player_health(Gfx **gfx, s32 w, s32 h) {
    s32 x_left = w / 2 - 91, x_right = w / 2 + 91;
    s32 y_base = h - 39, y_air;
    u32 now = gSteve.tick_count;
    s32 hp, i;
    int blink;

    // Hud.extractArmor (one row of hearts: 10 above them)
    {
        int armor = steve_armor_value();
        if (armor > 0) {
            s32 y_armor = y_base - 10;
            for (i = 0; i < 10; i++) {
                s32 xo = x_left + i * 8;
                if (i * 2 + 1 < armor) SPRITE(armor_full, xo, y_armor);
                if (i * 2 + 1 == armor) SPRITE(armor_half, xo, y_armor);
                if (i * 2 + 1 > armor) SPRITE(armor_empty, xo, y_armor);
            }
        }
    }

    // ExperienceBar.extractBackground: progress * 183 of the 182 wide bar
    // (gameMode.hasExperience: survival and adventure only)
    if (gSurv.gamemode == GM_SURVIVAL || gSurv.gamemode == GM_ADVENTURE) {
        s32 top = h - 24 - 5, progress = (s32)(gXp.progress * 183.0f);
        SPRITE(xp_bg, x_left, top);
        if (progress > 182) progress = 182;
        if (progress > 0) {
            gui_blit(gfx, mc_hud_xp_progress, MC_HUD_xp_progress_W, MC_HUD_xp_progress_H, 0, 0, progress, 5,
                x_left, top, progress, 5);
        }
        // ContextualBar.extractExperienceLevel: black outline, colour 0x80FF20
        if (gXp.level > 0) {
            char str[12];
            s32 n = gXp.level, len = 0, x, y, k;
            char tmp[12];
            do { tmp[len++] = (char)('0' + n % 10); n /= 10; } while (n > 0 && len < 11);
            for (k = 0; k < len; k++) str[k] = tmp[len - 1 - k];
            str[len] = 0;
            x = (w - gui_text_width(str)) / 2;
            y = h - 24 - 9 - 2;
            gui_text(gfx, str, x + 1, y, 0x000000, FALSE);
            gui_text(gfx, str, x - 1, y, 0x000000, FALSE);
            gui_text(gfx, str, x, y + 1, 0x000000, FALSE);
            gui_text(gfx, str, x, y - 1, 0x000000, FALSE);
            gui_text(gfx, str, x, y, 0x80FF20, FALSE);
        }
    }

    hp = (s32)(gSteve.health + 0.999f);
    if (last_hp >= 0 && hp < last_hp && gSteve.hurt_time > 0) {
        blink_until = now + 20;
        blink_old_hp = last_hp;
    }
    if (last_hp < 0 || hp > last_hp) {
        blink_old_hp = hp;
    }
    last_hp = hp;
    blink = blink_until > now && ((blink_until - now) / 3) % 2 == 1;

    for (i = 9; i >= 0; i--) {
        s32 hx = x_left + i * 8, hy = y_base;

        if (hp <= 4 && ((now * 7 + i * 13) % 2) == 0) {
            hy += 1; // low health: hearts jump by a random pixel
        }
        if (blink) {
            SPRITE(heart_container_blinking, hx, hy);
            if (i * 2 + 1 < blink_old_hp) {
                SPRITE(heart_full_blinking, hx, hy);
            } else if (i * 2 + 1 == blink_old_hp) {
                SPRITE(heart_half_blinking, hx, hy);
            }
        } else {
            SPRITE(heart_container, hx, hy);
        }
        if (i * 2 + 1 < hp) {
            SPRITE(heart_full, hx, hy);
        } else if (i * 2 + 1 == hp) {
            SPRITE(heart_half, hx, hy);
        }
    }

    // food (extractFood)
    for (i = 0; i < 10; i++) {
        s32 fx = x_right - i * 8 - 9, fy = y_base;
        if (gSteve.saturation <= 0.0f && (now % (gSteve.food_level * 3 + 1)) == 0) {
            fy += (s32)((now * 31 + i * 17) % 3) - 1;
        }
        SPRITE(food_empty, fx, fy);
        if (i * 2 + 1 < gSteve.food_level) {
            SPRITE(food_full, fx, fy);
        } else if (i * 2 + 1 == gSteve.food_level) {
            SPRITE(food_half, fx, fy);
        }
    }

    // air bubbles (extractAirBubbles), one row above the food
    y_air = y_base - 10;
    {
        s32 air = gSteve.air_supply;
        int under = gSteve.eye_in_water;

        if (air < 0) air = 0;
        if (air > 300) air = 300;
        if (under || air < 300) {
            s32 full = bubble_count(air - 2);
            s32 popping = bubble_count(air);
            s32 empty = 10 - bubble_count(air + ((air != 0 && under) ? 1 : 0));
            int is_popping = full != popping;
            int b;

            for (b = 1; b <= 10; b++) {
                s32 bx = x_right - (b - 1) * 8 - 9;
                if (b <= full) {
                    SPRITE(air, bx, y_air);
                } else if (is_popping && b == popping && under) {
                    SPRITE(air_bursting, bx, y_air);
                } else if (b > 10 - empty) {
                    s32 wobble = (empty == 10 && (now % 2) == 0) ? (s32)((now * 13 + b) % 2) : 0;
                    SPRITE(air_empty, bx, y_air + wobble);
                }
            }
        }
    }
}

// Hud.setOverlayMessage / extractOverlayMessage: 60 ticks, fading over the
// last 20, centered at guiHeight - 68
static const char *overlay_text;
static u32 overlay_until;

void hud_set_overlay_message(const char *text) {
    overlay_text = text;
    overlay_until = gSteve.tick_count + 60;
}

static void hud_overlay_message(Gfx **gfx, s32 w, s32 h) {
    s32 t, alpha;

    if (overlay_text == NULL || overlay_until <= gSteve.tick_count) return;
    t = (s32)(overlay_until - gSteve.tick_count);
    alpha = t * 255 / 20;
    if (alpha > 255) alpha = 255;
    if (alpha > 0) {
        s32 width = gui_text_width(overlay_text);
        gui_text_alpha(gfx, overlay_text, w / 2 - width / 2, h - 68 - 4, 0xFFFFFF, TRUE, (u8)alpha);
    }
}

RECOMP_HOOK("printbuffer_draw") void hud_draw(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    s32 w, h;

    if (!gSteve.enabled || !D_8037BFB8) {
        return;
    }
    if (gHideGui && !screen_is_open()) {
        return; // f1
    }
    gui_update_metrics();
    w = gGui.width;
    h = gGui.height;

    gui_begin(gfx, mtx);

    // crosshair: first person only, and not while a screen is open
    if (gSteve.driving && gSteve.first_person && f3_crosshair_3d()) {
        f3_draw_crosshair(gfx, w, h);
    } else if (gSteve.driving && gSteve.first_person && !screen_is_open() && !GM_SPECTATING()) {
        SPRITE(crosshair, (w - 15) / 2, (h - 15) / 2);
        // attack indicator (AttackIndicatorStatus.CROSSHAIR): the attack
        // cooldown filling up under the crosshair, getAttackStrengthScale
        {
            f32 delay = item_attack_delay(surv_held_item());
            f32 scale = (f32)gSteve.attack_strength_ticker / delay;
            s32 x = w / 2 - 8, y = h / 2 - 7 + 16;

            if (scale < 1.0f) {
                s32 progress = (s32)(scale * 17.0f);
                SPRITE(attack_bg, x, y);
                if (progress > 0) {
                    gui_blit(gfx, mc_hud_attack_progress, MC_HUD_attack_progress_W, MC_HUD_attack_progress_H, 0, 0,
                        progress, 4, x, y, progress, 4);
                }
            }
        }
    }
    // spectators get neither hotbar nor hearts (Gui: isSpectator)
    if (GM_SPECTATING()) {
        hud_overlay_message(gfx, w, h);
        f3_draw(gfx, w, h);
        screen_draw(gfx, w, h);
        advancements_draw_toasts(gfx, w); // ToastManager, over screens
        gui_end(gfx);
        return;
    }
    hud_hotbar(gfx, w, h);
    if (!gSurv.creative) {
        hud_player_health(gfx, w, h);
    }

    // the held item's name above the hotbar (Hud.extractSelectedItemName):
    // 40 ticks when the selection changes, fading out over the last 10
    {
        static u16 last_item;
        static s32 timer;
        static u32 last_tick;
        u16 cur = surv_held_item();
        u32 now = gSteve.tick_count;

        if (cur == 0) {
            timer = 0;
        } else if (cur != last_item) {
            timer = 40;
        } else if (timer > 0) {
            timer -= (s32)(now - last_tick);
        }
        last_item = cur;
        last_tick = now;
        if (timer > 0 && cur != 0) {
            const char *name = item_name(cur);
            s32 alpha = timer * 256 / 10;
            s32 y = h - 59 + (gSurv.creative ? 14 : 0);

            if (alpha > 255) alpha = 255;
            gui_text_alpha(gfx, name, (w - gui_text_width(name)) / 2, y, 0xFFFFFF, TRUE, (u8)alpha);
        }
    }

    hud_overlay_message(gfx, w, h);
    // DebugScreenOverlay: over the hud, under screens
    f3_draw(gfx, w, h);

    // an open inventory / crafting screen goes on top of the hud
    screen_draw(gfx, w, h);
    advancements_draw_toasts(gfx, w); // ToastManager, over screens

    gui_end(gfx);
}

#endif
