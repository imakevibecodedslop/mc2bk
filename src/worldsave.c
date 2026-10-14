// the world remembers what you did to it: every map keeps the blocks you
// placed, the holes you dug or blew up and the trees you cut, like a
// minecraft world keeps its chunks. kept in memory per map while playing, and
// written to disk with bk's own save (gameFile_save), per save slot:
// ~/.config/BanjoRecompiled/mc_steve_<slot + 10>.bin
//
// format "MCW4": u16 name count, names (u8 len + chars), u16 map count, then
// per map: u16 map id, u32 block count, blocks (s16 x, y, z, u16 name index,
// u8 state), u32 carved count, cells (s16 x, y, z, s16 edge-of-level depth
// * 16 or -32768), u32 mined tree cell
// count, cells (s16 x, y, z). little endian. MCW2 / MCW3 still load.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "world.h"
#include "terrain.h"
#include "shapes.h"
#include "steve.h"
#include "options.h"

#define MOUSE_MOD_ID "bk_minecraft_mouse"
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_begin(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_put(int b));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_commit(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_open(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_get(int i));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_delete(int slot));

typedef struct { s16 x, y, z; u16 block; } SavedBlock;
typedef struct { s16 x, y, z; s16 shell; } SavedCell; // shell: underground depth * 16 for edge-of-level cells, SHELL_NONE else
#define SHELL_NONE (-32768)

typedef struct {
    int used;
    s32 map;
    SavedBlock *blocks;
    int block_count, block_cap;
    SavedCell *cells;
    int cell_count, cell_cap;
    SavedCell *trees;
    int tree_count, tree_cap;
} MapStore;

#define MAX_MAPS 64
static MapStore maps[MAX_MAPS];
static s32 live_map = -1; // the map the grid currently holds
static int restoring;

static int io_available(void) {
    return recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
}

static MapStore *store_for(s32 map, int create) {
    int i, free_slot = -1;
    for (i = 0; i < MAX_MAPS; i++) {
        if (maps[i].used && maps[i].map == map) return &maps[i];
        if (!maps[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    maps[free_slot].used = TRUE;
    maps[free_slot].map = map;
    maps[free_slot].block_count = 0;
    maps[free_slot].cell_count = 0;
    maps[free_slot].tree_count = 0;
    return &maps[free_slot];
}

static void *grow(void *p, int *cap, int need, int elem) {
    if (need <= *cap) return p;
    {
        int ncap = (*cap == 0) ? 64 : *cap * 2;
        u8 *np, *op = p;
        int i;
        while (ncap < need) ncap *= 2;
        np = recomp_alloc(ncap * elem);
        if (np == NULL) return p;
        for (i = 0; i < *cap * elem; i++) np[i] = op[i];
        if (p) recomp_free(p);
        *cap = ncap;
        return np;
    }
}

static void store_clear(MapStore *m) {
    m->block_count = 0;
    m->cell_count = 0;
    m->tree_count = 0;
}

static void store_add_block(MapStore *m, s32 x, s32 y, s32 z, u16 b) {
    m->blocks = grow(m->blocks, &m->block_cap, m->block_count + 1, sizeof(SavedBlock));
    if (m->block_count >= m->block_cap) return;
    m->blocks[m->block_count].x = (s16)x;
    m->blocks[m->block_count].y = (s16)y;
    m->blocks[m->block_count].z = (s16)z;
    m->blocks[m->block_count].block = b;
    m->block_count++;
}

static void cell_list_add(SavedCell **list, int *count, int *cap, s32 x, s32 y, s32 z, s32 shell) {
    *list = grow(*list, cap, *count + 1, sizeof(SavedCell));
    if (*count >= *cap) return;
    (*list)[*count].x = (s16)x;
    (*list)[*count].y = (s16)y;
    (*list)[*count].z = (s16)z;
    (*list)[*count].shell = (s16)shell;
    (*count)++;
}

static void store_add_cell(MapStore *m, s32 x, s32 y, s32 z, s32 shell) {
    cell_list_add(&m->cells, &m->cell_count, &m->cell_cap, x, y, z, shell);
}

static void store_add_tree(MapStore *m, s32 x, s32 y, s32 z) {
    cell_list_add(&m->trees, &m->tree_count, &m->tree_cap, x, y, z, SHELL_NONE);
}

// the live grid + carved cells -> the map's store
static void snapshot_live(void) {
    MapStore *m;
    int i, n;

    if (live_map < 0) return;
    m = store_for(live_map, TRUE);
    if (m == NULL) return;
    store_clear(m);
    for (i = 0; i < world_section_count(); i++) {
        Section *s = world_section_at(i);
        for (n = 0; n < SECTION_VOLUME; n++) {
            if (s->blocks[n] != BLOCK_AIR) {
                s32 x = n % SECTION_SIZE, z = (n / SECTION_SIZE) % SECTION_SIZE, y = n / (SECTION_SIZE * SECTION_SIZE);
                store_add_block(m, s->cx * SECTION_SIZE + x, s->cy * SECTION_SIZE + y, s->cz * SECTION_SIZE + z, s->blocks[n]);
            }
        }
    }
    // the holes (mined or blown up) and the cut trees stay too
    if (!option_keep_terrain()) return;
    for (i = 0; i < terrain_carved_count(); i++) {
        const s32 *c = terrain_carved_cell(i);
        f32 d;
        s32 q = SHELL_NONE;
        if (terrain_carved_shell(i, &d)) {
            q = (s32)(d * 16.0f);
            if (q > 32767) q = 32767;
            if (q < 0) q = 0;
        }
        store_add_cell(m, c[0], c[1], c[2], q);
    }
    n = steve_tree_mined_count(live_map);
    for (i = 0; i < n; i++) {
        const s32 *c = steve_tree_mined_cell(i);
        store_add_tree(m, c[0], c[1], c[2]);
    }
}

// before the grid of the old map is cleared (world.c calls this)
void worldsave_map_leaving(void) {
    if (!restoring) snapshot_live();
}

// the new map's grid is empty and its model is loaded: put everything back
void worldsave_map_entered(s32 map) {
    MapStore *m = store_for(map, FALSE);
    int i;

    live_map = map;
    steve_tree_restore_begin(map);
    if (m == NULL) return;
    restoring = TRUE;
    if (option_keep_terrain()) {
        for (i = 0; i < m->tree_count; i++) steve_tree_restore_cell(m->trees[i].x, m->trees[i].y, m->trees[i].z);
        // edge-of-level cells first: they count as underground before the
        // carving asks about them
        for (i = 0; i < m->cell_count; i++) {
            if (m->cells[i].shell != SHELL_NONE) {
                terrain_shell_restore(m->cells[i].x, m->cells[i].y, m->cells[i].z, m->cells[i].shell / 16.0f);
            }
        }
        terrain_carve_begin();
        for (i = 0; i < m->cell_count; i++) terrain_carve(m->cells[i].x, m->cells[i].y, m->cells[i].z);
        terrain_carve_end();
    }
    // (as saved: no shape updates while it's put back, a door's halves come one at a time)
    shapes_suppress(TRUE);
    for (i = 0; i < m->block_count; i++) world_set(m->blocks[i].x, m->blocks[i].y, m->blocks[i].z, m->blocks[i].block);
    shapes_suppress(FALSE);
    restoring = FALSE;
    recomp_printf("[mc] map %d restored: %d blocks, %d mined cells, %d tree cells\n", map, m->block_count, m->cell_count, m->tree_count);
}

static void put16(int v) { mc_save_put(v & 0xFF); mc_save_put((v >> 8) & 0xFF); }
static void put32(int v) { put16(v & 0xFFFF); put16((v >> 16) & 0xFFFF); }

RECOMP_HOOK("gameFile_save") void worldsave_on_game_save(s32 gamenum) {
    // block ids -> a name table (ids move between exports, names don't)
    static u16 name_of_id[MC_BLOCK_COUNT];
    static u16 ids[MC_BLOCK_COUNT];
    int name_count = 0, i, k, maps_used = 0;

    if (!io_available()) return;
    snapshot_live();
    for (i = 0; i < MC_BLOCK_COUNT; i++) name_of_id[i] = 0xFFFF;
    for (i = 0; i < MAX_MAPS; i++) {
        if (!maps[i].used) continue;
        maps_used++;
        for (k = 0; k < maps[i].block_count; k++) {
            u16 b = CELL_ID(maps[i].blocks[k].block);
            if (b < MC_BLOCK_COUNT && name_of_id[b] == 0xFFFF) {
                name_of_id[b] = (u16)name_count;
                ids[name_count++] = b;
            }
        }
    }
    mc_save_begin();
    mc_save_put('M'); mc_save_put('C'); mc_save_put('W'); mc_save_put('4');
    put16(name_count);
    for (i = 0; i < name_count; i++) {
        const char *n = mc_registry_names[ids[i]];
        int len = 0;
        while (n[len] && len < 64) len++;
        mc_save_put(len);
        for (k = 0; k < len; k++) mc_save_put(n[k]);
    }
    put16(maps_used);
    for (i = 0; i < MAX_MAPS; i++) {
        MapStore *m = &maps[i];
        if (!m->used) continue;
        put16(m->map);
        put32(m->block_count);
        for (k = 0; k < m->block_count; k++) {
            put16(m->blocks[k].x); put16(m->blocks[k].y); put16(m->blocks[k].z);
            put16(name_of_id[CELL_ID(m->blocks[k].block)]);
            mc_save_put(CELL_STATE(m->blocks[k].block)); // block state (facing / axis)
        }
        put32(m->cell_count);
        for (k = 0; k < m->cell_count; k++) {
            put16(m->cells[k].x); put16(m->cells[k].y); put16(m->cells[k].z);
            put16(m->cells[k].shell);
        }
        put32(m->tree_count);
        for (k = 0; k < m->tree_count; k++) {
            put16(m->trees[k].x); put16(m->trees[k].y); put16(m->trees[k].z);
        }
    }
    recomp_printf("[mc] world saved for game %d (%d bytes)\n", gamenum, mc_save_commit(10 + gamenum));
}

static int rd_pos;
static int get8(void) { return mc_load_get(rd_pos++); }
static int get16(void) { int a = get8(); int b = get8(); return a | (b << 8); }
static int get32(void) { int a = get16(); int b = get16(); return a | (b << 16); }

RECOMP_HOOK("gameFile_load") void worldsave_on_game_load(s32 gamenum) {
    static u16 table[1024];
    int i, k, len, names, nmaps, version;

    for (i = 0; i < MAX_MAPS; i++) {
        maps[i].used = FALSE;
        store_clear(&maps[i]);
    }
    live_map = -1;
    if (!io_available()) return;
    len = mc_load_open(10 + gamenum);
    rd_pos = 0;
    if (len < 8 || get8() != 'M' || get8() != 'C' || get8() != 'W') return;
    version = get8();
    if (version < '2' || version > '4') return;
    names = get16();
    for (i = 0; i < names && i < 1024; i++) {
        char name[65];
        int nl = get8(), j, id = 0;
        for (k = 0; k < nl && k < 64; k++) name[k] = (char)get8();
        name[k] = 0;
        for (j = 1; j < MC_BLOCK_COUNT && id == 0; j++) {
            const char *r = mc_registry_names[j];
            for (k = 0; r[k] && r[k] == name[k]; k++) { }
            if (r[k] == 0 && name[k] == 0) id = j;
        }
        table[i] = (u16)id;
    }
    nmaps = get16();
    for (i = 0; i < nmaps && rd_pos < len; i++) {
        MapStore *m = store_for(get16(), TRUE);
        int nb = get32(), nc;
        for (k = 0; k < nb; k++) {
            s16 x = (s16)get16(), y = (s16)get16(), z = (s16)get16();
            int ni = get16();
            int st = get8();
            if (m && ni < names && table[ni] != 0) store_add_block(m, x, y, z, MAKE_CELL(table[ni], st & 0x3F));
        }
        nc = get32();
        for (k = 0; k < nc; k++) {
            s16 x = (s16)get16(), y = (s16)get16(), z = (s16)get16();
            s32 sh = (version >= '4') ? (s16)get16() : SHELL_NONE;
            if (m) store_add_cell(m, x, y, z, sh);
        }
        if (version >= '3') {
            nc = get32();
            for (k = 0; k < nc; k++) {
                s16 x = (s16)get16(), y = (s16)get16(), z = (s16)get16();
                if (m) store_add_tree(m, x, y, z);
            }
        }
    }
    recomp_printf("[mc] world loaded for game %d (%d maps)\n", gamenum, nmaps);
}

// a new world (an empty file picked on the file select): nothing placed yet
void worldsave_new_world(void) {
    int i;
    for (i = 0; i < MAX_MAPS; i++) {
        maps[i].used = FALSE;
        store_clear(&maps[i]);
    }
    live_map = -1;
}

RECOMP_HOOK("gameFile_clear") void worldsave_on_game_clear(s32 gamenum) {
    int i;
    for (i = 0; i < MAX_MAPS; i++) {
        maps[i].used = FALSE;
        store_clear(&maps[i]);
    }
    if (io_available()) mc_save_delete(10 + gamenum);
}
