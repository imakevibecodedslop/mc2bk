// steve's inventory is saved with bk's own game file: when bk saves, loads or
// erases a save slot, the same happens to steve's file for that slot. the
// file io lives in the mouse mod's native library (mods can't touch files),
// so without the mouse mod the inventory just isn't saved.
//
// format: "MCS3", version byte, selected slot, game mode, then INV_SIZE x
// (name length, minecraft registry name, count, damage lo, damage hi).
// names, not ids: the ids come from the exporter and can move when it
// changes. older files (MCS1/2, numeric ids) are ignored. then optional
// tagged blocks: "XP" (level, total, progress), "EQ" (armor + offhand stacks),
// "RB" (recipe book), "AD" (done advancement criteria by name).

#include "xp.h"
#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "chest.h"
#include "survival.h"
#include "steve.h"
#include "recipebook.h"
#include "advancements.h"

#define MOUSE_MOD_ID "bk_minecraft_mouse"
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_begin(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_put(int b));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_commit(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_open(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_get(int i));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_delete(int slot));

#define SAVE_VERSION 3

static int io_available(void) {
    return recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
}

static void clear_inventory(void) {
    int i;
    for (i = 0; i < INV_TOTAL; i++) {
        gSurv.inv[i].id = 0;
        gSurv.inv[i].count = 0;
        gSurv.inv[i].damage = 0;
    }
    gSurv.selected = 0;
}

// a stack: name length, registry name, count, damage lo, damage hi
static void put_stack(const ItemStack *st) {
    const char *name = (st->count > 0 && st->id < MC_ITEM_END) ? mc_registry_names[st->id] : "";
    int len = 0, n;
    while (name[len] != 0 && len < 64) len++;
    mc_save_put(len);
    for (n = 0; n < len; n++) mc_save_put(name[n]);
    mc_save_put(len ? st->count : 0);
    mc_save_put(st->damage & 0xFF);
    mc_save_put(st->damage >> 8);
}

static void put_name(const char *s) {
    int len = 0, n;
    while (s[len] != 0 && len < 64) len++;
    mc_save_put(len);
    for (n = 0; n < len; n++) mc_save_put(s[n]);
}

static int get_name(char *out, int *o) {
    int len = mc_load_get((*o)++), q;
    if (len < 0) return FALSE;
    for (q = 0; q < len; q++) {
        int ch = mc_load_get((*o)++);
        if (q < 64) out[q] = (char)ch;
    }
    out[len < 64 ? len : 64] = 0;
    return TRUE;
}

RECOMP_HOOK("gameFile_save") void steve_on_game_save(s32 gamenum) {
    int i, n, k;

    if (!io_available()) return;
    mc_save_begin();
    mc_save_put('M');
    mc_save_put('C');
    mc_save_put('S');
    mc_save_put('3');
    mc_save_put(SAVE_VERSION);
    mc_save_put(gSurv.selected);
    mc_save_put(gSurv.gamemode);
    for (i = 0; i < INV_SIZE; i++) put_stack(&gSurv.inv[i]);
    // then (optional) "XP": XpLevel, XpTotal (u32 le), XpP (u16 / 65535)
    mc_save_put('X');
    mc_save_put('P');
    for (n = 0; n < 4; n++) mc_save_put((gXp.level >> (n * 8)) & 0xFF);
    for (n = 0; n < 4; n++) mc_save_put((gXp.total >> (n * 8)) & 0xFF);
    {
        u32 p = (u32)(gXp.progress * 65535.0f);
        mc_save_put(p & 0xFF);
        mc_save_put((p >> 8) & 0xFF);
    }
    // then (optional) "EQ": the armor (36-39) and the offhand (40)
    mc_save_put('E');
    mc_save_put('Q');
    for (i = INV_SIZE; i < INV_TOTAL; i++) put_stack(&gSurv.inv[i]);
    // then (optional) "RB": the recipe book (settings, known recipes by name)
    mc_save_put('R');
    mc_save_put('B');
    mc_save_put(recipebook_settings());
    k = recipebook_save_count();
    mc_save_put(k & 0xFF);
    mc_save_put((k >> 8) & 0xFF);
    for (i = 0; i < k; i++) {
        const char *rn = recipebook_save_name(i);
        int len = 0;
        while (rn[len] != 0 && len < 64) len++;
        mc_save_put(len);
        for (n = 0; n < len; n++) mc_save_put(rn[n]);
    }
    // then (optional) "AD": PlayerAdvancements, the done criteria
    // ("advancement", "criterion" names)
    mc_save_put('A');
    mc_save_put('D');
    k = advancements_save_count();
    mc_save_put(k & 0xFF);
    mc_save_put((k >> 8) & 0xFF);
    for (i = 0; i < k; i++) {
        const char *an, *cn;
        advancements_save_entry(i, &an, &cn);
        put_name(an);
        put_name(cn);
    }
    n = mc_save_commit(gamenum);
    recomp_printf("[mc] inventory saved for game %d (%d bytes)\n", gamenum, n);
}

// registry name -> id (0 if this export doesn't have it)
static ItemId id_for_name(const char *name) {
    int i, k;

    if (name[0] == 0) return 0;
    for (i = 1; i < MC_ITEM_END; i++) {
        const char *r = mc_registry_names[i];
        for (k = 0; r[k] != 0 && r[k] == name[k]; k++) { }
        if (r[k] == 0 && name[k] == 0) return (ItemId)i;
    }
    return 0;
}

static int get_stack(ItemStack *st, int *o) {
    char name[65];
    int nl = mc_load_get(*o), k;

    if (nl < 0) return FALSE;
    for (k = 0; k < nl && k < 64; k++) name[k] = (char)mc_load_get(*o + 1 + k);
    name[k] = 0;
    *o += 1 + nl;
    st->id = id_for_name(name);
    st->count = (u8)mc_load_get(*o);
    st->damage = (u16)(mc_load_get(*o + 1) | (mc_load_get(*o + 2) << 8));
    *o += 3;
    if (st->count == 0 || st->id == 0) {
        st->id = 0;
        st->count = 0;
        st->damage = 0;
    }
    return TRUE;
}

RECOMP_HOOK("gameFile_load") void steve_on_game_load(s32 gamenum) {
    int len, i, o = 7;

    clear_inventory(); // a slot without a file starts empty, like a new world
    xp_reset();
    recipebook_reset();
    advancements_reset();
    surv_starter_kit_pending = FALSE; // (set again below if it has no file)
    if (!io_available()) return;
    len = mc_load_open(gamenum);
    if (len < 7 || mc_load_get(0) != 'M' || mc_load_get(1) != 'C' || mc_load_get(2) != 'S'
        || mc_load_get(3) != '3') {
        recomp_printf("[mc] no inventory file for game %d\n", gamenum);
        surv_starter_kit_pending = TRUE; // a new world (see surv_starter_kit)
        return;
    }
    gSurv.selected = mc_load_get(5) % 9;
    gSurv.gamemode = -1;
    surv_set_gamemode(mc_load_get(6) & 3);
    gSurv.prev_gamemode = -1;
    for (i = 0; i < INV_SIZE; i++) {
        if (!get_stack(&gSurv.inv[i], &o)) break;
    }
    if (mc_load_get(o) == 'X' && mc_load_get(o + 1) == 'P') {
        s32 lv = 0, tot = 0;
        for (i = 0; i < 4; i++) lv |= (mc_load_get(o + 2 + i) & 0xFF) << (i * 8);
        for (i = 0; i < 4; i++) tot |= (mc_load_get(o + 6 + i) & 0xFF) << (i * 8);
        gXp.level = lv;
        gXp.total = tot;
        gXp.progress = (f32)(mc_load_get(o + 10) | (mc_load_get(o + 11) << 8)) / 65535.0f;
        o += 12;
        if (mc_load_get(o) == 'E' && mc_load_get(o + 1) == 'Q') {
            o += 2;
            for (i = INV_SIZE; i < INV_TOTAL; i++) {
                if (!get_stack(&gSurv.inv[i], &o)) break;
            }
            if (mc_load_get(o) == 'R' && mc_load_get(o + 1) == 'B') {
                int cnt, j;
                recipebook_set_settings(mc_load_get(o + 2));
                cnt = mc_load_get(o + 3) | (mc_load_get(o + 4) << 8);
                o += 5;
                for (j = 0; j < cnt; j++) {
                    char rn[65];
                    int len = mc_load_get(o++), q;
                    if (len < 0) break;
                    for (q = 0; q < len; q++) {
                        int ch = mc_load_get(o++);
                        if (q < 64) rn[q] = (char)ch;
                    }
                    rn[len < 64 ? len : 64] = 0;
                    recipebook_load_name(rn);
                }
                if (mc_load_get(o) == 'A' && mc_load_get(o + 1) == 'D') {
                    cnt = mc_load_get(o + 2) | (mc_load_get(o + 3) << 8);
                    o += 4;
                    for (j = 0; j < cnt; j++) {
                        char an[65], cn[65];
                        if (!get_name(an, &o) || !get_name(cn, &o)) break;
                        advancements_load_entry(an, cn);
                    }
                }
            }
        }
    }
    recomp_printf("[mc] inventory loaded for game %d\n", gamenum);
}

RECOMP_HOOK("gameFile_clear") void steve_on_game_clear(s32 gamenum) {
    clear_inventory();
    surv_starter_kit_pending = TRUE;
    xp_reset();
    recipebook_reset();
    advancements_reset();
    if (!io_available()) return;
    mc_save_delete(gamenum);
}

// the file select loads a file (gameFile_load) only to show a file that has
// something in it. an empty one loads nothing, so a new game would start with
// the last shown file's inventory and blocks: start them fresh here instead.
extern s32 gameFile_isNotEmpty(s32 gamenum);
void worldsave_new_world(void);

RECOMP_HOOK("setGameInformationZoombox") void steve_on_file_shown(s32 gamenum) {
    if (gameFile_isNotEmpty(gamenum)) return; // gameFile_load does it
    clear_inventory();
    xp_reset();
    recipebook_reset();
    advancements_reset();
    worldsave_new_world();
    chests_reset();
    gSurv.gamemode = -1;
    surv_set_gamemode(GM_SURVIVAL);
    gSurv.prev_gamemode = -1;
    surv_starter_kit_pending = TRUE;
    recomp_printf("[mc] empty game %d: a new world\n", gamenum);
}
