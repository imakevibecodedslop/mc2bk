// creative saved hotbars, ported from HotbarManager / Hotbar and
// CreativeModeInventoryScreen.handleHotbarLoadOrSave (26.3). one file for all
// saves (like minecraft's hotbar.nbt), through the mouse mod's file io.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "items.h"
#include "hotbars.h"

#define MOUSE_MOD_ID "bk_minecraft_mouse"
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_begin(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_put(int b));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_commit(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_open(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_get(int i));

#define HOTBAR_FILE 100

static ItemStack hotbars[9][9];
static int loaded;

static int io_available(void) {
    return recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
}

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

// HotbarManager.load
static void load_file(void) {
    int len, o = 4, h, i;

    loaded = TRUE;
    if (!io_available()) return;
    len = mc_load_open(HOTBAR_FILE);
    if (len < 4 || mc_load_get(0) != 'M' || mc_load_get(1) != 'C' || mc_load_get(2) != 'H' || mc_load_get(3) != '1') {
        return;
    }
    for (h = 0; h < 9; h++) {
        for (i = 0; i < 9; i++) {
            char name[65];
            int nl = mc_load_get(o), k;
            ItemStack *st = &hotbars[h][i];

            if (nl < 0) return;
            for (k = 0; k < nl && k < 64; k++) name[k] = (char)mc_load_get(o + 1 + k);
            name[k] = 0;
            o += 1 + nl;
            st->id = id_for_name(name);
            st->count = (u8)mc_load_get(o);
            st->damage = (u16)(mc_load_get(o + 1) | (mc_load_get(o + 2) << 8));
            o += 3;
            if (st->id == 0 || st->count == 0) {
                st->id = 0;
                st->count = 0;
                st->damage = 0;
            }
        }
    }
}

// HotbarManager.save
static void save_file(void) {
    int h, i, n;

    if (!io_available()) return;
    mc_save_begin();
    mc_save_put('M');
    mc_save_put('C');
    mc_save_put('H');
    mc_save_put('1');
    for (h = 0; h < 9; h++) {
        for (i = 0; i < 9; i++) {
            const ItemStack *st = &hotbars[h][i];
            const char *name = (st->count > 0 && st->id < MC_ITEM_END) ? mc_registry_names[st->id] : "";
            int len = 0;
            while (name[len] != 0 && len < 64) len++;
            mc_save_put(len);
            for (n = 0; n < len; n++) mc_save_put(name[n]);
            mc_save_put(len ? st->count : 0);
            mc_save_put(st->damage & 0xFF);
            mc_save_put(st->damage >> 8);
        }
    }
    mc_save_commit(HOTBAR_FILE);
}

// Hotbar.isEmpty
int hotbars_is_empty(int h) {
    int i;
    if (!loaded) load_file();
    for (i = 0; i < 9; i++) {
        if (hotbars[h][i].count > 0) return FALSE;
    }
    return TRUE;
}

const ItemStack *hotbars_get(int h, int i) {
    if (!loaded) load_file();
    return &hotbars[h][i];
}

void hotbars_save(int h) {
    static char msg[96];
    const char *f = MC_STR_HOTBAR_SAVED;
    int i, k = 0;

    if (!loaded) load_file();
    for (i = 0; i < 9; i++) {
        hotbars[h][i] = gSurv.inv[i];
        if (hotbars[h][i].count == 0) hotbars[h][i].id = 0;
    }
    save_file();
    // "Item hotbar saved (restore with %1$s + %2$s)": keyLoadHotbarActivator (X), the hotbar key
    while (*f && k < 90) {
        if (f[0] == '%' && f[1] == '1') { msg[k++] = 'X'; f += 4; }
        else if (f[0] == '%' && f[1] == '2') { msg[k++] = (char)('1' + h); f += 4; }
        else msg[k++] = *f++;
    }
    msg[k] = 0;
    hud_set_overlay_message(msg);
}

void hotbars_load(int h) {
    int i;
    if (!loaded) load_file();
    for (i = 0; i < 9; i++) gSurv.inv[i] = hotbars[h][i];
}
